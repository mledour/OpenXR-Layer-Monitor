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

// =============================================================================
// test_gpu_ring.cpp -- direct tests for utils/gpu_timer.h's
// detail::GpuSlotRing<N> state machine.
//
// GpuTimerD3D11 needs a real ID3D11Device to drive Begin / End / GetData, so
// it can only be validated on a real GPU. The TRICKY part of its logic (the
// overwrite-on-full path that bumps the drop counter, the in-order drain
// guarantee, the wrap-around indices) lives in GpuSlotRing -- a pure CPU
// state machine that holds no D3D objects. These tests exercise THAT
// directly, without spinning up a layer chain, a D3D11 device, or even a
// background thread.
//
// What is NOT covered here and would need a real GPU OR a mock of
// ID3D11DeviceContext (out of scope for this commit):
//   * RecordTimestamp's Begin(disjoint) / End(timestamp) / End(disjoint) calls.
//   * PollResolved's GetData success / partial-failure paths.
//   * init() rolling back ComPtrs on CreateQuery failure.
// The class is small enough that those paths are validated by code review
// plus a live recording on a D3D11 host.
// =============================================================================

#include <doctest/doctest.h>

#include "utils/gpu_timer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

using openxr_api_layer::gpu::detail::GpuSlotRing;
using openxr_api_layer::gpu::detail::SlotState;

namespace {

    // Drain every Pending slot via Peek + Consume and collect the frame index
    // values in resolution order. Mirrors what GpuTimerD3D11::PollResolved
    // does once GetData reports ready -- minus the D3D calls.
    template <std::size_t N>
    std::vector<uint64_t> DrainAll(GpuSlotRing<N>& ring) {
        std::vector<uint64_t> out;
        std::size_t slot;
        uint64_t fi;
        while (ring.PeekOldest(slot, fi)) {
            out.push_back(fi);
            ring.ConsumeOldest();
        }
        return out;
    }

} // namespace

TEST_CASE("GpuSlotRing: a fresh ring is empty") {
    GpuSlotRing<4> ring;
    std::size_t slot = 999;
    uint64_t fi = 999;
    CHECK_FALSE(ring.PeekOldest(slot, fi));
    CHECK(ring.WriteIndex() == 0);
    CHECK(ring.ReadIndex() == 0);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(ring.SlotStateAt(i) == SlotState::Idle);
    }
}

TEST_CASE("GpuSlotRing: Reserve into an empty ring never reports overwrite") {
    GpuSlotRing<4> ring;
    const auto r = ring.Reserve(42);
    CHECK(r.slot_index == 0);
    CHECK_FALSE(r.overwrote_pending);
    CHECK(ring.WriteIndex() == 1);
    CHECK(ring.ReadIndex() == 0);
    CHECK(ring.SlotStateAt(0) == SlotState::Pending);
    CHECK(ring.FrameIndexAt(0) == 42);
}

TEST_CASE("GpuSlotRing: Reserve up to capacity never reports overwrite") {
    GpuSlotRing<4> ring;
    for (uint64_t i = 0; i < 4; ++i) {
        const auto r = ring.Reserve(100 + i);
        CHECK(r.slot_index == static_cast<std::size_t>(i));
        CHECK_FALSE(r.overwrote_pending);
    }
    // After exactly N reserves, write wrapped to 0 (matches read), every
    // slot is Pending.
    CHECK(ring.WriteIndex() == 0);
    CHECK(ring.ReadIndex() == 0);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(ring.SlotStateAt(i) == SlotState::Pending);
    }
}

TEST_CASE("GpuSlotRing: drain order matches insertion order") {
    GpuSlotRing<4> ring;
    ring.Reserve(10);
    ring.Reserve(11);
    ring.Reserve(12);
    const auto drained = DrainAll(ring);
    CHECK(drained == std::vector<uint64_t>{10, 11, 12});
    // Drain leaves the ring empty.
    std::size_t slot;
    uint64_t fi;
    CHECK_FALSE(ring.PeekOldest(slot, fi));
}

TEST_CASE("GpuSlotRing: PeekOldest does not consume the slot") {
    GpuSlotRing<4> ring;
    ring.Reserve(7);
    std::size_t slot1, slot2;
    uint64_t fi1, fi2;
    REQUIRE(ring.PeekOldest(slot1, fi1));
    REQUIRE(ring.PeekOldest(slot2, fi2));
    CHECK(slot1 == slot2);
    CHECK(fi1 == fi2);
    CHECK(fi1 == 7);
}

TEST_CASE("GpuSlotRing: interleaved Reserve / Consume in steady state never overwrites") {
    // Mimics the healthy case: GPU resolves frame N a few frames after it
    // was recorded, ring never fills. Drive 20 frames at "1 in, 1 out per
    // tick" cadence and verify no overwrite ever happens.
    GpuSlotRing<4> ring;
    ring.Reserve(0);
    ring.Reserve(1);
    ring.Reserve(2);  // 3 in flight (steady-state GPU lag at 3 frames)
    for (uint64_t i = 3; i < 20; ++i) {
        const auto r = ring.Reserve(i);
        CHECK_FALSE(r.overwrote_pending);
        std::size_t slot;
        uint64_t fi;
        REQUIRE(ring.PeekOldest(slot, fi));
        CHECK(fi == i - 3);  // oldest is always 3 frames behind
        ring.ConsumeOldest();
    }
}

TEST_CASE("GpuSlotRing: Reserve on a full ring overwrites the oldest pending slot") {
    // Fill the ring and then push one more. The (N+1)th Reserve MUST report
    // overwrote_pending=true and bump the read cursor past the dropped entry
    // so a subsequent PollResolved never sees freshly-overwritten data as
    // the "oldest" frame.
    GpuSlotRing<4> ring;
    ring.Reserve(100);
    ring.Reserve(101);
    ring.Reserve(102);
    ring.Reserve(103);
    CHECK(ring.WriteIndex() == 0);
    CHECK(ring.ReadIndex() == 0);

    const auto r = ring.Reserve(104);
    CHECK(r.slot_index == 0);
    CHECK(r.overwrote_pending);
    // After overwriting slot 0, write advanced to 1. Read MUST also be 1
    // (track the new oldest).
    CHECK(ring.WriteIndex() == 1);
    CHECK(ring.ReadIndex() == 1);
    // The slot we reused now holds frame 104 (the new arrival).
    CHECK(ring.FrameIndexAt(0) == 104);
    CHECK(ring.SlotStateAt(0) == SlotState::Pending);
}

TEST_CASE("GpuSlotRing: drain after a single overwrite skips the dropped frame") {
    // The drop semantic: frame 100 was overwritten, so a downstream
    // PollResolved must see frames 101, 102, 103, 104 (in that order) --
    // never frame 100, and never the overwritten slot's stale data.
    GpuSlotRing<4> ring;
    for (uint64_t i = 100; i <= 103; ++i) ring.Reserve(i);  // full
    ring.Reserve(104);                                       // overwrite 100
    const auto drained = DrainAll(ring);
    CHECK(drained == std::vector<uint64_t>{101, 102, 103, 104});
}

TEST_CASE("GpuSlotRing: multiple consecutive overwrites all bump read past dropped slots") {
    // Two stalls in a row: fill 4, push 2 more. Frames 100 and 101 should
    // both be dropped; drain sees 102, 103, 104, 105.
    GpuSlotRing<4> ring;
    for (uint64_t i = 100; i <= 103; ++i) ring.Reserve(i);
    const auto r1 = ring.Reserve(104);
    const auto r2 = ring.Reserve(105);
    CHECK(r1.overwrote_pending);
    CHECK(r2.overwrote_pending);
    const auto drained = DrainAll(ring);
    CHECK(drained == std::vector<uint64_t>{102, 103, 104, 105});
}

TEST_CASE("GpuSlotRing: ConsumeOldest on the only Pending slot empties the ring") {
    GpuSlotRing<4> ring;
    ring.Reserve(7);
    std::size_t slot;
    uint64_t fi;
    REQUIRE(ring.PeekOldest(slot, fi));
    ring.ConsumeOldest();
    CHECK_FALSE(ring.PeekOldest(slot, fi));
    CHECK(ring.SlotStateAt(0) == SlotState::Idle);
}

TEST_CASE("GpuSlotRing: Reset clears every slot and both cursors") {
    GpuSlotRing<4> ring;
    // Drive it into a non-trivial state: 2 pending, write=2, read=0.
    ring.Reserve(50);
    ring.Reserve(51);
    REQUIRE(ring.WriteIndex() == 2);
    REQUIRE(ring.SlotStateAt(0) == SlotState::Pending);

    ring.Reset();

    CHECK(ring.WriteIndex() == 0);
    CHECK(ring.ReadIndex() == 0);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(ring.SlotStateAt(i) == SlotState::Idle);
        CHECK(ring.FrameIndexAt(i) == 0);
    }
    std::size_t slot;
    uint64_t fi;
    CHECK_FALSE(ring.PeekOldest(slot, fi));
}

TEST_CASE("GpuSlotRing: Reset after an overwrite clears the drop bookkeeping in the ring") {
    // The drop COUNTER lives on GpuTimerD3D11 (atomic uint64_t bumped from
    // RecordTimestamp's caller), NOT on the ring -- Reset() does not touch
    // it (that's the right behaviour: the counter accumulates over the
    // session, surfaced once in the footer). This test pins the ring's
    // Reset to ONLY ring state.
    GpuSlotRing<4> ring;
    for (uint64_t i = 0; i < 5; ++i) ring.Reserve(i);  // one overwrite
    ring.Reset();
    CHECK(ring.WriteIndex() == 0);
    CHECK(ring.ReadIndex() == 0);
    // Subsequent Reserve into the fresh ring behaves as if from boot.
    const auto r = ring.Reserve(999);
    CHECK(r.slot_index == 0);
    CHECK_FALSE(r.overwrote_pending);
}

TEST_CASE("GpuSlotRing: a single-slot ring overwrites on every Reserve after the first") {
    // Degenerate but useful corner -- pins the indexing math.
    GpuSlotRing<1> ring;
    CHECK(ring.Capacity() == 1);

    const auto r0 = ring.Reserve(0);
    CHECK_FALSE(r0.overwrote_pending);
    CHECK(ring.WriteIndex() == 0);  // wrapped immediately

    const auto r1 = ring.Reserve(1);
    CHECK(r1.overwrote_pending);
    CHECK(ring.FrameIndexAt(0) == 1);

    const auto r2 = ring.Reserve(2);
    CHECK(r2.overwrote_pending);
    CHECK(ring.FrameIndexAt(0) == 2);
}
