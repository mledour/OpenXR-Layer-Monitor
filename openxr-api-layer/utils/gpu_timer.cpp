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

#include <array>

namespace openxr_api_layer::gpu {

    namespace {

        // 4 slots -> ~44 ms in flight at 90 Hz, comfortably above any realistic
        // GPU->CPU readback latency (typically 1-3 frames). Kept small so a
        // session end holds back at most a few unresolved frames. Same constant
        // XrTelemetry uses for its duration timer.
        constexpr size_t kGpuRingSize = 4;

        enum class SlotState { Idle, Pending };

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
        class GpuTimerD3D11 final : public IGpuTimer {
          public:
            ~GpuTimerD3D11() override {
                for (auto& slot : m_ring) {
                    slot.disjoint.Reset();
                    slot.timestamp.Reset();
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
                for (auto& slot : m_ring) {
                    if (FAILED(m_device->CreateQuery(&disjointDesc,
                                                     slot.disjoint.GetAddressOf())) ||
                        FAILED(m_device->CreateQuery(&tsDesc,
                                                     slot.timestamp.GetAddressOf()))) {
                        for (auto& s : m_ring) {
                            s.disjoint.Reset();
                            s.timestamp.Reset();
                        }
                        m_context.Reset();
                        m_device.Reset();
                        return false;
                    }
                    slot.state = SlotState::Idle;
                    slot.frame_index = 0;
                }
                m_writeIdx = 0;
                m_readIdx = 0;
                m_active = true;
                return true;
            }

            void RecordTimestamp(uint64_t frame_idx) override {
                if (!m_active) {
                    return;
                }
                Slot& slot = m_ring[m_writeIdx];
                // If the slot we're about to reuse is still Pending, the ring
                // is full: the GPU is more than kGpuRingSize frames behind on
                // readback (a severe stall). We overwrite the oldest unresolved
                // slot and pull the consumer's read cursor forward so it never
                // reads freshly-overwritten data as the dropped frame. The
                // dropped frame simply gets no GPU row; the merge tolerates it.
                const bool overwritingPending = (slot.state == SlotState::Pending);

                m_context->Begin(slot.disjoint.Get());
                m_context->End(slot.timestamp.Get());
                m_context->End(slot.disjoint.Get());
                slot.frame_index = frame_idx;
                slot.state = SlotState::Pending;

                m_writeIdx = (m_writeIdx + 1) % kGpuRingSize;
                if (overwritingPending) {
                    m_readIdx = m_writeIdx;
                }
            }

            void PollResolved(std::vector<GpuRow>& out) override {
                if (!m_active) {
                    return;
                }
                // Drain oldest-first. GPU timestamps complete in command-stream
                // (FIFO) order, so if the slot at m_readIdx isn't ready yet,
                // nothing behind it is either -- we stop and retry next poll,
                // preserving frame order in the CSV.
                while (true) {
                    Slot& slot = m_ring[m_readIdx];
                    if (slot.state != SlotState::Pending) {
                        break;
                    }
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
                    const HRESULT hr =
                        m_context->GetData(slot.disjoint.Get(), &disjointData,
                                           sizeof(disjointData),
                                           D3D11_ASYNC_GETDATA_DONOTFLUSH);
                    if (hr != S_OK) {
                        break;  // not ready yet
                    }
                    UINT64 ticks = 0;
                    const HRESULT hrTs =
                        m_context->GetData(slot.timestamp.Get(), &ticks,
                                           sizeof(ticks),
                                           D3D11_ASYNC_GETDATA_DONOTFLUSH);
                    if (hrTs != S_OK) {
                        // The disjoint query went ready but the paired
                        // timestamp did not -- not expected from a conformant
                        // driver. Drop the slot rather than stall the ring.
                        slot.state = SlotState::Idle;
                        m_readIdx = (m_readIdx + 1) % kGpuRingSize;
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
                    out.push_back(GpuRow{slot.frame_index, ticks,
                                         disjointData.Frequency, valid});
                    slot.state = SlotState::Idle;
                    m_readIdx = (m_readIdx + 1) % kGpuRingSize;
                }
            }

            void Reset() override {
                if (!m_active) {
                    return;
                }
                // Abandon every in-flight slot. Re-issuing Begin on a query
                // that was End'd but never read is well-defined in D3D11 (the
                // stale result is discarded), so no GPU calls are needed here
                // -- this is pure CPU bookkeeping, safe to call from the frame
                // thread inside ApplyToggle.
                for (auto& slot : m_ring) {
                    slot.state = SlotState::Idle;
                    slot.frame_index = 0;
                }
                m_writeIdx = 0;
                m_readIdx = 0;
            }

          private:
            struct Slot {
                ComPtr<ID3D11Query> disjoint;
                ComPtr<ID3D11Query> timestamp;
                uint64_t frame_index = 0;
                SlotState state = SlotState::Idle;
            };

            ComPtr<ID3D11Device> m_device;
            ComPtr<ID3D11DeviceContext> m_context;
            std::array<Slot, kGpuRingSize> m_ring;
            size_t m_writeIdx = 0;
            size_t m_readIdx = 0;
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
