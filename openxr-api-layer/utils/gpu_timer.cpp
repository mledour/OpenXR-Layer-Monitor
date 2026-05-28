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

            const char* BackendName() const override { return "d3d11"; }

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

        // ---- D3D12 backend ------------------------------------------------
        //
        // D3D12 has no immediate context. We can't piggy-back on the app's
        // command lists, so we own a tiny pair (ID3D12CommandAllocator,
        // ID3D12GraphicsCommandList) per ring slot and submit each one
        // ourselves on the APP's command queue via ExecuteCommandLists. The
        // command list does exactly two things: EndQuery(TIMESTAMP) at the
        // requested ring slot, then ResolveQueryData of that single slot
        // into a per-slot offset in a READBACK heap. After ExecuteCommand
        // Lists, we Signal a fence on the queue; the fence value is stored
        // with the slot so a later PollResolved knows when the readback
        // buffer is safe to Map.
        //
        // Differences from XrTelemetry's D3D12GpuTimer (which measures
        // durations and so issues TWO command lists per frame, one for
        // begin and one for end+resolve):
        //
        //   * We measure a single POINT in the command stream per side, so
        //     each slot has ONE command list, not two.
        //   * On a stalled GPU (the in-flight ring overflowing), we cannot
        //     simply overwrite the slot like D3D11 does -- D3D12 ERRORS
        //     loudly if we Reset() a command allocator whose commands are
        //     still executing. So RecordTimestamp pre-checks the slot's
        //     fence value and SKIPS the frame if the GPU isn't caught up,
        //     bumping the drop counter. The user-visible behaviour is the
        //     same as D3D11's overwrite-and-count: one frame's GPU sample
        //     is missing, the merge leaves target_gpu_us blank.
        //
        // No equivalent of the D3D11 disjoint flag exists on D3D12. We trust
        // the queue's GetTimestampFrequency() value for the lifetime of the
        // queue (same convention OpenXR Toolkit, fpsVR and XrTelemetry use)
        // and embed it in every emitted GpuRow's gpu_freq column.
        class GpuTimerD3D12 final : public IGpuTimer {
          public:
            ~GpuTimerD3D12() override {
                // Drain in-flight GPU work before releasing the resources
                // it references. D3D12 spec is strict that objects bound
                // to submitted command lists must outlive their execution.
                if (m_active && m_fence && m_queue && m_fenceCounter > 0) {
                    const UINT64 v = ++m_fenceCounter;
                    if (SUCCEEDED(m_queue->Signal(m_fence.Get(), v))) {
                        waitForFenceBounded(v, /*timeoutMs=*/1000);
                    }
                }
                shutdownInternal();
            }

            bool init(ID3D12Device* device, ID3D12CommandQueue* queue) {
                if (!device || !queue) {
                    return false;
                }
                // The OpenXR D3D12 binding requires a DIRECT queue. Refuse
                // anything else -- ExecuteCommandLists on a compute/copy
                // queue with a direct command list would fail at runtime.
                const D3D12_COMMAND_QUEUE_DESC queueDesc = queue->GetDesc();
                if (queueDesc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                    return false;
                }
                // Propagate the queue's NodeMask to every D3D12 object we
                // create. Hardcoding node 0 breaks silently on multi-adapter
                // rigs where the app's queue lives on node 1+.
                const UINT nodeMask = queueDesc.NodeMask;

                m_device = device;
                m_queue = queue;
                m_frequency = 0;
                if (FAILED(m_queue->GetTimestampFrequency(&m_frequency)) ||
                    m_frequency == 0) {
                    m_queue.Reset();
                    m_device.Reset();
                    return false;
                }

                // Query heap: one timestamp slot per ring entry.
                D3D12_QUERY_HEAP_DESC qhDesc{};
                qhDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                qhDesc.Count = static_cast<UINT>(kGpuRingSize);
                qhDesc.NodeMask = nodeMask;
                if (FAILED(m_device->CreateQueryHeap(
                        &qhDesc,
                        IID_PPV_ARGS(m_queryHeap.GetAddressOf())))) {
                    m_queue.Reset();
                    m_device.Reset();
                    return false;
                }

                // Readback buffer: UINT64 x kGpuRingSize, READBACK heap, the
                // implicit initial state for which is COPY_DEST (the right
                // state for ResolveQueryData's destination). Map can be
                // called on a READBACK resource regardless of state.
                D3D12_HEAP_PROPERTIES heapProps{};
                heapProps.Type = D3D12_HEAP_TYPE_READBACK;
                heapProps.CreationNodeMask = nodeMask;
                heapProps.VisibleNodeMask = nodeMask;
                D3D12_RESOURCE_DESC bufDesc{};
                bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bufDesc.Width = sizeof(UINT64) * kGpuRingSize;
                bufDesc.Height = 1;
                bufDesc.DepthOrArraySize = 1;
                bufDesc.MipLevels = 1;
                bufDesc.Format = DXGI_FORMAT_UNKNOWN;
                bufDesc.SampleDesc.Count = 1;
                bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (FAILED(m_device->CreateCommittedResource(
                        &heapProps, D3D12_HEAP_FLAG_NONE,
                        &bufDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                        nullptr,
                        IID_PPV_ARGS(m_readback.GetAddressOf())))) {
                    shutdownInternal();
                    return false;
                }

                // Per-slot (allocator, command list). Created closed so the
                // first Reset() in RecordTimestamp succeeds (newly-built
                // command lists are open by default).
                for (auto& slot : m_slots) {
                    if (FAILED(m_device->CreateCommandAllocator(
                            D3D12_COMMAND_LIST_TYPE_DIRECT,
                            IID_PPV_ARGS(slot.allocator.GetAddressOf()))) ||
                        FAILED(m_device->CreateCommandList(
                            nodeMask, D3D12_COMMAND_LIST_TYPE_DIRECT,
                            slot.allocator.Get(), nullptr,
                            IID_PPV_ARGS(slot.cmdList.GetAddressOf())))) {
                        shutdownInternal();
                        return false;
                    }
                    slot.cmdList->Close();
                    slot.fenceValue = 0;
                }

                if (FAILED(m_device->CreateFence(
                        0, D3D12_FENCE_FLAG_NONE,
                        IID_PPV_ARGS(m_fence.GetAddressOf())))) {
                    shutdownInternal();
                    return false;
                }

                m_ring.Reset();
                m_droppedFrames.store(0, std::memory_order_relaxed);
                m_fenceCounter = 0;
                m_active = true;
                return true;
            }

            void RecordTimestamp(uint64_t frame_idx) override {
                if (!m_active) {
                    return;
                }
                // Pre-check: is the slot we're about to reuse still
                // executing on the GPU? D3D12 forbids Reset() on a command
                // allocator whose commands haven't completed (debug layer
                // raises, runtime may corrupt). Unlike D3D11 where re-Begin
                // is well-defined, here we SKIP the frame if the GPU
                // hasn't caught up.
                const std::size_t target_slot = m_ring.WriteIndex();
                auto& slot = m_slots[target_slot];
                if (slot.fenceValue > 0 &&
                    m_fence->GetCompletedValue() < slot.fenceValue) {
                    m_droppedFrames.fetch_add(
                        1, std::memory_order_relaxed);
                    return;
                }

                // Now safe to advance the ring + reset the allocator.
                const auto reserved = m_ring.Reserve(frame_idx);
                if (FAILED(slot.allocator->Reset()) ||
                    FAILED(slot.cmdList->Reset(slot.allocator.Get(),
                                               nullptr))) {
                    // Recording failed at the API level. Mark the slot
                    // with the sentinel fenceValue=0 so PollResolved skips
                    // it (Map would otherwise read whatever garbage was
                    // in the readback buffer for this index).
                    m_droppedFrames.fetch_add(
                        1, std::memory_order_relaxed);
                    slot.fenceValue = 0;
                    return;
                }

                const UINT queryIdx = static_cast<UINT>(reserved.slot_index);
                slot.cmdList->EndQuery(m_queryHeap.Get(),
                                       D3D12_QUERY_TYPE_TIMESTAMP,
                                       queryIdx);
                slot.cmdList->ResolveQueryData(
                    m_queryHeap.Get(),
                    D3D12_QUERY_TYPE_TIMESTAMP,
                    queryIdx, /*NumQueries=*/1,
                    m_readback.Get(),
                    /*AlignedDestinationBufferOffset=*/sizeof(UINT64) * queryIdx);
                if (FAILED(slot.cmdList->Close())) {
                    m_droppedFrames.fetch_add(
                        1, std::memory_order_relaxed);
                    slot.fenceValue = 0;
                    return;
                }

                ID3D12CommandList* lists[] = { slot.cmdList.Get() };
                m_queue->ExecuteCommandLists(1, lists);
                slot.fenceValue = ++m_fenceCounter;
                m_queue->Signal(m_fence.Get(), slot.fenceValue);
            }

            void PollResolved(std::vector<GpuRow>& out) override {
                if (!m_active) {
                    return;
                }
                // Drain oldest-first; D3D12 timestamps complete in command-
                // stream order so the FIFO drain is preserved.
                while (true) {
                    std::size_t slot_index = 0;
                    uint64_t frame_idx = 0;
                    if (!m_ring.PeekOldest(slot_index, frame_idx)) {
                        break;
                    }
                    auto& slot = m_slots[slot_index];
                    if (slot.fenceValue == 0) {
                        // Sentinel from a record-time failure; skip without
                        // emitting a row.
                        m_ring.ConsumeOldest();
                        continue;
                    }
                    if (m_fence->GetCompletedValue() < slot.fenceValue) {
                        break;  // GPU hasn't finished this slot yet
                    }

                    const SIZE_T offset = sizeof(UINT64) * slot_index;
                    const D3D12_RANGE readRange{offset,
                                                offset + sizeof(UINT64)};
                    void* mapped = nullptr;
                    UINT64 ticks = 0;
                    bool mapOk = false;
                    if (SUCCEEDED(m_readback->Map(0, &readRange, &mapped)) &&
                        mapped) {
                        const auto* ts = reinterpret_cast<const UINT64*>(
                            reinterpret_cast<const std::byte*>(mapped) +
                            offset);
                        ticks = *ts;
                        // CPU writes nothing back into the readback buffer.
                        const D3D12_RANGE noWrite{0, 0};
                        m_readback->Unmap(0, &noWrite);
                        mapOk = true;
                    }
                    // D3D12 has no per-query disjoint flag: valid means
                    // "we successfully read the slot's value". The merge's
                    // own guards (post_ticks >= pre_ticks, freq match)
                    // still catch driver bugs / clock anomalies.
                    const uint32_t valid = mapOk ? 1u : 0u;
                    out.push_back(GpuRow{frame_idx, ticks,
                                         m_frequency, valid});
                    m_ring.ConsumeOldest();
                }
            }

            void Reset() override {
                if (!m_active) {
                    return;
                }
                // CRUCIAL: drain every in-flight submission BEFORE dropping
                // ring bookkeeping. If we don't, a subsequent RecordTimestamp
                // could see slot.fenceValue=0 (cleared below), think the
                // allocator is free, and Reset() it while the GPU is still
                // executing -- D3D12 debug layer raises and the recording
                // is poisoned. Bounded wait so a hung GPU doesn't lock the
                // frame thread indefinitely.
                if (m_fence && m_queue && m_fenceCounter > 0) {
                    const UINT64 v = ++m_fenceCounter;
                    if (SUCCEEDED(m_queue->Signal(m_fence.Get(), v))) {
                        waitForFenceBounded(v, /*timeoutMs=*/200);
                    }
                }
                for (auto& slot : m_slots) {
                    slot.fenceValue = 0;
                }
                m_ring.Reset();
            }

            uint64_t DroppedFrames() const override {
                return m_droppedFrames.load(std::memory_order_acquire);
            }

            const char* BackendName() const override { return "d3d12"; }

          private:
            void shutdownInternal() {
                for (auto& slot : m_slots) {
                    slot.allocator.Reset();
                    slot.cmdList.Reset();
                    slot.fenceValue = 0;
                }
                m_readback.Reset();
                m_queryHeap.Reset();
                m_fence.Reset();
                m_queue.Reset();
                m_device.Reset();
                m_active = false;
            }

            // Block until the fence reaches `value`, or timeoutMs elapses.
            // Returns false on timeout. Used both for shutdown drain and
            // for the Reset() barrier; never on the per-frame hot path.
            bool waitForFenceBounded(UINT64 value, DWORD timeoutMs) {
                if (m_fence->GetCompletedValue() >= value) {
                    return true;
                }
                wil::unique_event_nothrow ev;
                if (FAILED(ev.create())) {
                    return false;
                }
                if (FAILED(m_fence->SetEventOnCompletion(value, ev.get()))) {
                    return false;
                }
                return WaitForSingleObject(ev.get(), timeoutMs) ==
                       WAIT_OBJECT_0;
            }

            struct Slot {
                ComPtr<ID3D12CommandAllocator> allocator;
                ComPtr<ID3D12GraphicsCommandList> cmdList;
                // fenceValue == 0 doubles as a sentinel for "never
                // submitted" / "record failed". Real submissions get a
                // monotonically increasing fence value from m_fenceCounter.
                UINT64 fenceValue = 0;
            };

            ComPtr<ID3D12Device> m_device;
            ComPtr<ID3D12CommandQueue> m_queue;
            ComPtr<ID3D12QueryHeap> m_queryHeap;
            ComPtr<ID3D12Resource> m_readback;
            ComPtr<ID3D12Fence> m_fence;
            UINT64 m_fenceCounter = 0;
            UINT64 m_frequency = 0;
            std::array<Slot, kGpuRingSize> m_slots;
            detail::GpuSlotRing<kGpuRingSize> m_ring;
            // Same semantics as D3D11's m_droppedFrames: counts frames whose
            // GPU sample we could not record because the GPU was too far
            // behind on readback. D3D12 SKIPS the frame (vs D3D11's
            // overwrite) but the user-visible cost is the same.
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

    std::unique_ptr<IGpuTimer> MakeD3D12GpuTimer(ID3D12Device* device,
                                                 ID3D12CommandQueue* queue) {
        auto timer = std::make_unique<GpuTimerD3D12>();
        if (!timer->init(device, queue)) {
            return nullptr;
        }
        return timer;
    }

} // namespace openxr_api_layer::gpu
