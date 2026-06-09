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

#include "synthetic_load.h"

// Reuse the unit-tested ring-state machine the GPU timer uses: Pending/Idle
// bookkeeping + overwrite-on-full is identical here, so there's no reason to
// re-derive (and re-bug) it.
#include "gpu_timer.h"

#include <array>
#include <cstring>

namespace openxr_api_layer::load {

    namespace {

        // Same depth as the GPU timer: ~kGpuRingSize frames in flight covers
        // the 1-3 frame GPU->CPU readback latency with margin. Two timestamp
        // queries per slot (T0 before the copy loop, T1 after).
        constexpr size_t kRing = 4;
        constexpr size_t kQueriesPerSlot = 2;

        class SyntheticGpuLoadD3D12 final : public SyntheticGpuLoad {
          public:
            ~SyntheticGpuLoadD3D12() override {
                // Drain in-flight submissions before releasing referenced
                // resources -- same strict-lifetime rule as the D3D12 timer.
                if (m_active && m_fence && m_queue && m_fenceCounter > 0) {
                    const UINT64 v = ++m_fenceCounter;
                    if (SUCCEEDED(m_queue->Signal(m_fence.Get(), v))) {
                        waitForFenceBounded(v, /*timeoutMs=*/200);
                    }
                }
                shutdownInternal();
            }

            bool init(ID3D12Device* device, ID3D12CommandQueue* queue,
                      uint64_t iterations, uint64_t bufferBytes) {
                if (!device || !queue || bufferBytes == 0) {
                    return false;
                }
                const D3D12_COMMAND_QUEUE_DESC queueDesc = queue->GetDesc();
                if (queueDesc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                    return false;  // OpenXR D3D12 binding mandates a DIRECT queue
                }
                const UINT nodeMask = queueDesc.NodeMask;

                m_device = device;
                m_queue = queue;
                m_iterations = iterations;
                m_bufferBytes = bufferBytes;
                if (FAILED(m_queue->GetTimestampFrequency(&m_frequency)) ||
                    m_frequency == 0) {
                    m_queue.Reset();
                    m_device.Reset();
                    return false;
                }

                // Timestamp query heap: two slots per ring entry.
                D3D12_QUERY_HEAP_DESC qhDesc{};
                qhDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                qhDesc.Count = static_cast<UINT>(kRing * kQueriesPerSlot);
                qhDesc.NodeMask = nodeMask;
                if (FAILED(m_device->CreateQueryHeap(
                        &qhDesc, IID_PPV_ARGS(m_queryHeap.GetAddressOf())))) {
                    shutdownInternal();
                    return false;
                }

                // Readback buffer for the resolved timestamps.
                if (!createBuffer(D3D12_HEAP_TYPE_READBACK, nodeMask,
                                  sizeof(UINT64) * kRing * kQueriesPerSlot,
                                  D3D12_RESOURCE_STATE_COPY_DEST,
                                  m_readback)) {
                    shutdownInternal();
                    return false;
                }

                // The two DEFAULT-heap buffers the copy loop ping-pongs. Their
                // contents are irrelevant (we time the copy, not the data);
                // we zero-fill src once below only to keep the D3D12 debug
                // layer from flagging an uninitialised-resource read.
                if (!createBuffer(D3D12_HEAP_TYPE_DEFAULT, nodeMask, bufferBytes,
                                  D3D12_RESOURCE_STATE_COMMON, m_src) ||
                    !createBuffer(D3D12_HEAP_TYPE_DEFAULT, nodeMask, bufferBytes,
                                  D3D12_RESOURCE_STATE_COMMON, m_dst)) {
                    shutdownInternal();
                    return false;
                }

                // Per-slot (allocator, command list), created closed so the
                // first per-frame Reset() succeeds.
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
                    if (FAILED(slot.cmdList->Close())) {
                        shutdownInternal();
                        return false;
                    }
                    slot.fenceValue = 0;
                }

                if (FAILED(m_waitEvent.create())) {
                    shutdownInternal();
                    return false;
                }
                if (FAILED(m_device->CreateFence(
                        0, D3D12_FENCE_FLAG_NONE,
                        IID_PPV_ARGS(m_fence.GetAddressOf())))) {
                    shutdownInternal();
                    return false;
                }

                if (!initSourceBuffer(nodeMask)) {
                    shutdownInternal();
                    return false;
                }

                m_ring.Reset();
                m_active = true;
                return true;
            }

            void RecordAndTime(uint64_t display_time) override {
                if (!m_active) {
                    return;
                }
                // Skip (don't overwrite) a slot still executing on the GPU --
                // D3D12 forbids resetting an allocator whose commands are in
                // flight. A skipped frame simply emits no K row; the join
                // tolerates the gap.
                const std::size_t target_slot = m_ring.WriteIndex();
                auto& slot = m_slots[target_slot];
                if (slot.fenceValue > 0 &&
                    m_fence->GetCompletedValue() < slot.fenceValue) {
                    return;
                }

                const auto reserved = m_ring.Reserve(display_time);
                if (FAILED(slot.allocator->Reset()) ||
                    FAILED(slot.cmdList->Reset(slot.allocator.Get(), nullptr))) {
                    slot.fenceValue = 0;
                    return;
                }

                const UINT q0 =
                    static_cast<UINT>(reserved.slot_index * kQueriesPerSlot);
                const UINT q1 = q0 + 1;

                // T0 -> [copy loop] -> T1, all inside one command list: an
                // inline duration with no inter-submission gap (the ground
                // truth K).
                slot.cmdList->EndQuery(m_queryHeap.Get(),
                                       D3D12_QUERY_TYPE_TIMESTAMP, q0);
                for (uint64_t i = 0; i < m_iterations; ++i) {
                    slot.cmdList->CopyBufferRegion(m_dst.Get(), 0, m_src.Get(), 0,
                                                   m_bufferBytes);
                }
                slot.cmdList->EndQuery(m_queryHeap.Get(),
                                       D3D12_QUERY_TYPE_TIMESTAMP, q1);
                slot.cmdList->ResolveQueryData(
                    m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0,
                    /*NumQueries=*/static_cast<UINT>(kQueriesPerSlot),
                    m_readback.Get(),
                    /*AlignedDestinationBufferOffset=*/sizeof(UINT64) * q0);
                if (FAILED(slot.cmdList->Close())) {
                    slot.fenceValue = 0;
                    return;
                }

                ID3D12CommandList* lists[] = {slot.cmdList.Get()};
                m_queue->ExecuteCommandLists(1, lists);
                const UINT64 nextFenceValue = ++m_fenceCounter;
                // Tag the slot UNCONDITIONALLY and gate reuse on the fence --
                // same use-after-free guard the D3D12 timer documents: the
                // work is in flight whether or not the Signal below lands.
                (void)m_queue->Signal(m_fence.Get(), nextFenceValue);
                slot.fenceValue = nextFenceValue;
            }

            void PollResolved(std::vector<LoadRow>& out) override {
                if (!m_active) {
                    return;
                }
                while (true) {
                    std::size_t slot_index = 0;
                    uint64_t display_time = 0;
                    if (!m_ring.PeekOldest(slot_index, display_time)) {
                        break;
                    }
                    auto& slot = m_slots[slot_index];
                    if (slot.fenceValue == 0) {
                        m_ring.ConsumeOldest();  // record-time failure sentinel
                        continue;
                    }
                    if (m_fence->GetCompletedValue() < slot.fenceValue) {
                        break;  // not finished yet -- FIFO, so nothing behind is either
                    }

                    const UINT q0 =
                        static_cast<UINT>(slot_index * kQueriesPerSlot);
                    const SIZE_T offset = sizeof(UINT64) * q0;
                    const D3D12_RANGE readRange{
                        offset, offset + sizeof(UINT64) * kQueriesPerSlot};
                    void* mapped = nullptr;
                    uint64_t known = 0;
                    bool ok = false;
                    if (SUCCEEDED(m_readback->Map(0, &readRange, &mapped)) &&
                        mapped) {
                        const auto* ts = reinterpret_cast<const UINT64*>(
                            reinterpret_cast<const std::byte*>(mapped) + offset);
                        const UINT64 t0 = ts[0];
                        const UINT64 t1 = ts[1];
                        if (t1 >= t0) {
                            known = t1 - t0;
                            ok = true;
                        }
                        const D3D12_RANGE noWrite{0, 0};
                        m_readback->Unmap(0, &noWrite);
                    }
                    out.push_back(LoadRow{display_time, known, m_frequency,
                                          ok ? 1u : 0u});
                    m_ring.ConsumeOldest();
                }
            }

            uint64_t Iterations() const override { return m_iterations; }

          private:
            struct Slot {
                ComPtr<ID3D12CommandAllocator> allocator;
                ComPtr<ID3D12GraphicsCommandList> cmdList;
                UINT64 fenceValue = 0;
            };

            bool createBuffer(D3D12_HEAP_TYPE heapType, UINT nodeMask,
                              UINT64 bytes, D3D12_RESOURCE_STATES initialState,
                              ComPtr<ID3D12Resource>& out) {
                D3D12_HEAP_PROPERTIES heapProps{};
                heapProps.Type = heapType;
                heapProps.CreationNodeMask = nodeMask;
                heapProps.VisibleNodeMask = nodeMask;
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                desc.Width = bytes;
                desc.Height = 1;
                desc.DepthOrArraySize = 1;
                desc.MipLevels = 1;
                desc.Format = DXGI_FORMAT_UNKNOWN;
                desc.SampleDesc.Count = 1;
                desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                return SUCCEEDED(m_device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState,
                    nullptr, IID_PPV_ARGS(out.GetAddressOf())));
            }

            // One-time zero-fill of m_src via a transient UPLOAD buffer, using
            // slot[0]'s allocator/list and a blocking fence wait. Keeps the
            // debug layer quiet about reading an uninitialised resource in the
            // copy loop. Leaves slot[0] closed + idle and m_fenceCounter at 1.
            bool initSourceBuffer(UINT nodeMask) {
                ComPtr<ID3D12Resource> upload;
                if (!createBuffer(D3D12_HEAP_TYPE_UPLOAD, nodeMask, m_bufferBytes,
                                  D3D12_RESOURCE_STATE_GENERIC_READ, upload)) {
                    return false;
                }
                void* mapped = nullptr;
                const D3D12_RANGE noRead{0, 0};
                if (FAILED(upload->Map(0, &noRead, &mapped)) || !mapped) {
                    return false;
                }
                std::memset(mapped, 0, static_cast<size_t>(m_bufferBytes));
                upload->Unmap(0, nullptr);

                auto& slot = m_slots[0];
                if (FAILED(slot.allocator->Reset()) ||
                    FAILED(slot.cmdList->Reset(slot.allocator.Get(), nullptr))) {
                    return false;
                }
                slot.cmdList->CopyBufferRegion(m_src.Get(), 0, upload.Get(), 0,
                                               m_bufferBytes);
                if (FAILED(slot.cmdList->Close())) {
                    return false;
                }
                ID3D12CommandList* lists[] = {slot.cmdList.Get()};
                m_queue->ExecuteCommandLists(1, lists);
                const UINT64 v = ++m_fenceCounter;  // -> 1
                if (FAILED(m_queue->Signal(m_fence.Get(), v))) {
                    return false;
                }
                // Blocking is fine: init runs inside xrCreateSession, never on
                // the per-frame path. upload stays alive until the copy lands.
                return waitForFenceBounded(v, /*timeoutMs=*/1000);
            }

            void shutdownInternal() {
                for (auto& slot : m_slots) {
                    slot.allocator.Reset();
                    slot.cmdList.Reset();
                    slot.fenceValue = 0;
                }
                m_src.Reset();
                m_dst.Reset();
                m_readback.Reset();
                m_queryHeap.Reset();
                m_fence.Reset();
                m_queue.Reset();
                m_device.Reset();
                m_waitEvent.reset();
                m_active = false;
            }

            bool waitForFenceBounded(UINT64 value, DWORD timeoutMs) {
                if (m_fence->GetCompletedValue() >= value) {
                    return true;
                }
                if (!m_waitEvent.is_valid()) {
                    return false;
                }
                ::ResetEvent(m_waitEvent.get());
                if (FAILED(m_fence->SetEventOnCompletion(value,
                                                         m_waitEvent.get()))) {
                    return false;
                }
                return WaitForSingleObject(m_waitEvent.get(), timeoutMs) ==
                       WAIT_OBJECT_0;
            }

            ComPtr<ID3D12Device> m_device;
            ComPtr<ID3D12CommandQueue> m_queue;
            ComPtr<ID3D12QueryHeap> m_queryHeap;
            ComPtr<ID3D12Resource> m_readback;
            ComPtr<ID3D12Resource> m_src;
            ComPtr<ID3D12Resource> m_dst;
            ComPtr<ID3D12Fence> m_fence;
            wil::unique_event_nothrow m_waitEvent;
            UINT64 m_fenceCounter = 0;
            UINT64 m_frequency = 0;
            uint64_t m_iterations = 0;
            uint64_t m_bufferBytes = 0;
            std::array<Slot, kRing> m_slots;
            gpu::detail::GpuSlotRing<kRing> m_ring;
            bool m_active = false;
        };

    }  // namespace

    std::unique_ptr<SyntheticGpuLoad> MakeSyntheticGpuLoad(
        ID3D12Device* device, ID3D12CommandQueue* queue, uint64_t iterations,
        uint64_t bufferBytes) {
        auto load = std::make_unique<SyntheticGpuLoadD3D12>();
        if (!load->init(device, queue, iterations, bufferBytes)) {
            return nullptr;
        }
        return load;
    }

}  // namespace openxr_api_layer::load
