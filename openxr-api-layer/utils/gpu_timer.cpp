// MIT License
//
// Copyright (c) 2026 Michael Ledour
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "gpu_timer.h"

#include <atomic>
#include <array>

namespace openxr_api_layer::gpu {

    namespace {

        // 4 slots -> ~44 ms in flight at 90 Hz, comfortably above any realistic
        // GPU->CPU readback latency (typically 1-3 frames). Kept small so a
        // session end holds back at most a few unresolved frames. Same constant
        // XrTelemetry uses for its duration timer.
        constexpr size_t kGpuRingSize = 4;

        // D3D11 backend: one D3D11_QUERY_TIMESTAMP wrapped in its own tiny
        // D3D11_QUERY_TIMESTAMP_DISJOINT bracket, per ring slot. RecordTimestamp
        // issues Begin(disjoint) / End(timestamp) / End(disjoint) as a unit at
        // the current command-stream point; PollResolved reads them back once
        // the GPU has caught up.
        //
        // Why a single timestamp and not a Begin/End pair like XrTelemetry's
        // duration timer: in the sandwich, the two ENDPOINTS of the measured
        // GPU span live in two different DLLs (pre records the start, post the
        // end). Each side can only place ONE marker; the subtraction happens
        // offline in the merge. So a slot here holds one timestamp, not two.
        //
        // Ring bookkeeping (Pending/Idle indexing, overwrite-on-full) lives in
        // detail::GpuSlotRing<N> (gpu_timer.h) so the tricky path is unit-
        // testable without a D3D11 device. This class is just the D3D plumbing
        // around it.
        class GpuTimerD3D11 final : public IGpuTimer {
          public:
            ~GpuTimerD3D11() override {
                for (auto& pair : m_querySlots) {
                    pair.disjoint.Reset();
                    pair.timestamp.Reset();
                }
                m_context.Reset();
                m_device.Reset();
                m_active = false;
            }

            // Pulls the immediate context off the app's device and pre-creates
            // every query object. Returns false (caller degrades to CPU-only)
            // if the device is null or any query allocation fails. The
            // immediate context is the same single-threaded context the app
            // submits its draws on, so our timestamp queries land in command-
            // stream order against the target's work without any explicit
            // Flush.
            bool init(ID3D11Device* device) {
                if (!device) {
                    return false;
                }
                m_device = device;
                m_device->GetImmediateContext(m_context.ReleaseAndGetAddressOf());
                if (!m_context) {
                    m_device.Reset();
                    return false;
                }

                const D3D11_QUERY_DESC disjointDesc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                const D3D11_QUERY_DESC tsDesc{D3D11_QUERY_TIMESTAMP, 0};
                for (auto& pair : m_querySlots) {
                    if (FAILED(m_device->CreateQuery(&disjointDesc,
                                                     pair.disjoint.GetAddressOf())) ||
                        FAILED(m_device->CreateQuery(&tsDesc,
                                                     pair.timestamp.GetAddressOf()))) {
                        for (auto& p : m_querySlots) {
                            p.disjoint.Reset();
                            p.timestamp.Reset();
                        }
                        m_context.Reset();
                        m_device.Reset();
                        return false;
                    }
                }
                m_ring.Reset();
                m_droppedFrames.store(0, std::memory_order_relaxed);
                m_active = true;
                return true;
            }

            void RecordTimestamp(uint64_t frame_idx) override {
                if (!m_active) {
                    return;
                }
                // Reserve a ring slot. On a full ring (GPU >kGpuRingSize
                // frames behind, a >~44 ms stall at 90 Hz) the oldest
                // unresolved entry is silently overwritten -- we bump the
                // drop counter so the GPU CSV footer can surface it.
                const auto reserved = m_ring.Reserve(frame_idx);
                if (reserved.overwrote_pending) {
                    m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
                }
                auto& pair = m_querySlots[reserved.slot_index];
                m_context->Begin(pair.disjoint.Get());
                m_context->End(pair.timestamp.Get());
                m_context->End(pair.disjoint.Get());
            }

            void PollResolved(std::vector<GpuRow>& out) override {
                if (!m_active) {
                    return;
                }
                // Drain oldest-first. GPU timestamps complete in command-stream
                // (FIFO) order, so if the slot at the read cursor isn't ready
                // yet, nothing behind it is either -- we stop and retry next
                // poll, preserving frame order in the CSV.
                while (true) {
                    std::size_t slot_index = 0;
                    uint64_t frame_idx = 0;
                    if (!m_ring.PeekOldest(slot_index, frame_idx)) {
                        break;
                    }
                    auto& pair = m_querySlots[slot_index];
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
                    const HRESULT hr =
                        m_context->GetData(pair.disjoint.Get(), &disjointData,
                                           sizeof(disjointData),
                                           D3D11_ASYNC_GETDATA_DONOTFLUSH);
                    if (hr != S_OK) {
                        break;  // not ready yet
                    }
                    UINT64 ticks = 0;
                    const HRESULT hrTs =
                        m_context->GetData(pair.timestamp.Get(), &ticks,
                                           sizeof(ticks),
                                           D3D11_ASYNC_GETDATA_DONOTFLUSH);
                    if (hrTs != S_OK) {
                        // The disjoint query went ready but the paired
                        // timestamp did not -- not expected from a conformant
                        // driver. Drop the slot rather than stall the ring.
                        m_ring.ConsumeOldest();
                        continue;
                    }
                    // The Disjoint flag and Frequency both live on the disjoint
                    // query. valid == 0 tells the merge to skip this frame's
                    // GPU delta (the raw ticks/freq are still written so the
                    // CSV is self-describing for debugging).
                    const uint32_t valid =
                        (!disjointData.Disjoint && disjointData.Frequency != 0)
                            ? 1u
                            : 0u;
                    out.push_back(GpuRow{frame_idx, ticks,
                                         disjointData.Frequency, valid});
                    m_ring.ConsumeOldest();
                }
            }

            void Reset() override {
                if (!m_active) {
                    return;
                }
                // Abandon every in-flight slot. Re-issuing Begin on a query
                // that was End'd but never read back via GetData is well-
                // defined in D3D11: per the ID3D11DeviceContext::Begin docs,
                // "calling Begin on a query that has been previously issued
                // [...] will start a new query". The stale, never-read result
                // is simply discarded by the runtime, so we do NOT need any
                // GPU-side cleanup here -- this is pure CPU bookkeeping, safe
                // to call from the frame thread inside ApplyToggle.
                // https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-begin
                m_ring.Reset();
            }

            uint64_t DroppedFrames() const override {
                // Acquire matches the producer's relaxed fetch_add: the
                // happens-before edge that publishes the final counter value
                // to the GPU CSV writer thread is the writer's join inside
                // CsvSink::Stop(), but stating acquire here makes the
                // contract explicit for non-x86 readers.
                return m_droppedFrames.load(std::memory_order_acquire);
            }

          private:
            // A ring slot's D3D11 objects. The Pending/Idle state and frame
            // index sit in m_ring (detail::GpuSlotRing); this struct only
            // carries the query handles we hand to the immediate context.
            struct QuerySlot {
                ComPtr<ID3D11Query> disjoint;
                ComPtr<ID3D11Query> timestamp;
            };

            ComPtr<ID3D11Device> m_device;
            ComPtr<ID3D11DeviceContext> m_context;
            std::array<QuerySlot, kGpuRingSize> m_querySlots;
            detail::GpuSlotRing<kGpuRingSize> m_ring;
            // Bumped each time RecordTimestamp overwrites a Pending slot
            // (ring full -> GPU stall longer than kGpuRingSize frames).
            // Read from the writer thread inside the GPU CSV footer; written
            // from the render thread. Relaxed RMW since there's nothing else
            // to publish alongside it -- the acquire on DroppedFrames() and
            // the writer-thread join in Stop() give the cross-thread fence.
            std::atomic<uint64_t> m_droppedFrames{0};
            bool m_active = false;
        };

    } // namespace

    std::unique_ptr<IGpuTimer> MakeD3D11GpuTimer(ID3D11Device* device) {
        auto timer = std::make_unique<GpuTimerD3D11>();
        if (!timer->init(device)) {
            return nullptr;
        }
        return timer;
    }

} // namespace openxr_api_layer::gpu
