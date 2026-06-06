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

#pragma once

// =============================================================================
// GPU side of the sandwich profiler.
//
// THE GPU SANDWICH (D3D11 + D3D12)
// --------------------------------
// The CPU sandwich measures the target layer's xrEndFrame wall-clock cost by
// bracketing the call with QPC on both the pre and post sides and subtracting.
// The GPU sandwich does the same thing for GPU work, but a GPU timestamp is a
// single point in the command stream, not a duration -- so each side records
// ONE timestamp and the offline merge subtracts:
//
//     app -> pre -> target -> post -> runtime
//
//   pre  records T_pre  into the command stream JUST BEFORE forwarding to the
//        target (so it is the last GPU marker before the target's draws).
//   post records T_post into the command stream at the VERY START of its
//        xrEndFrame (so it is the first GPU marker after the target's draws,
//        before the runtime/compositor's own GPU work).
//
//   target_gpu_ns = (T_post - T_pre) / gpu_freq   (computed by the merge)
//
// Both DLLs see the same app-provided device (D3D11) or (device, queue) pair
// (D3D12), handed identically through XrGraphicsBindingD3D{11,12}KHR.
// Timestamps on the same device/queue read the same monotonic GPU counter,
// so the two values are directly subtractable.
//
//   D3D11: each side wraps its single timestamp in its own tiny D3D11_QUERY_
//          TIMESTAMP_DISJOINT bracket to recover the clock frequency and a
//          "the clock was unstable at this instant" flag.
//   D3D12: each side issues EndQuery(TIMESTAMP) + ResolveQueryData on its
//          own short command list, submitted to the app's command queue.
//          D3D12 has no per-query disjoint flag; the queue's GetTimestamp
//          Frequency() is trusted for the queue's lifetime.
//
// ASYNCHRONY
// ----------
// A timestamp issued at frame N's xrEndFrame reflects the GPU clock at the
// moment that command actually executes -- typically 1-3 frames later. So the
// render thread can only READ frame N's timestamp once the GPU has caught up.
// Each backend keeps a small ring (kGpuRingSize) of in-flight slots;
// PollResolved() drains every slot whose GPU result is ready and emits one
// GpuRow per resolved frame. The CSV is therefore a few frames behind the
// CPU CSV at any instant, which the merge tolerates (it joins on display_time and
// leaves target_gpu_us blank for any frame missing a GPU row on either side).
//
// THREADING
// ---------
// IGpuTimer methods (RecordTimestamp, PollResolved, Reset) MUST run on the
// app's render thread -- the same thread that calls xrEndFrame. This holds
// for both backends:
//   * D3D11's immediate context is single-threaded by API contract.
//   * D3D12 has no immediate context, but our ring + per-slot fence values
//     are plain (non-atomic) state -- they tolerate single-threaded use only.
// The DroppedFrames() / BackendName() accessors are also single-thread
// safe (atomic load + static literal return respectively). Callers running
// on OTHER threads -- e.g. the GPU CSV writer thread reading the backend
// name for the file header -- MUST serialise their access to the owning
// std::unique_ptr<IGpuTimer> separately (the layer uses g_gpuTimerMutex
// for this).
//
// LIMITATIONS (documented in the README too)
//   * D3D11 and D3D12 are wired up; Vulkan / OpenGL hosts get no GPU rows
//     (the CPU sandwich still works); the merge leaves target_gpu_us blank.
//   * Captures only GPU work the target submits on the app's immediate
//     context (D3D11) or render queue (D3D12) between the two timestamps,
//     in command-stream order. A target that renders on a deferred context,
//     a private device, or a separate queue is invisible to this
//     measurement.
//   * D3D11: the per-side disjoint bracket only covers the instant of each
//     timestamp, not the whole span between them, so a GPU clock change
//     DURING the target's work between T_pre and T_post may go unflagged.
//     A frequency mismatch between the two sides IS caught and blanks the
//     frame; clock changes are rare; re-run if a session looks suspicious.
//   * D3D12: no per-query disjoint flag exists; the queue's frequency is
//     trusted for its lifetime. A device removal (TDR) between Signal and
//     Map can leave a row with valid=1 holding stale readback data -- the
//     merge's post_ticks >= pre_ticks + frequency-match guards catch the
//     common cases, but a coincidental match-after-corruption would not
//     be detected. Re-run if a session crosses a TDR / driver restart.
// =============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// ID3D11Device / ID3D12Device / ID3D12CommandQueue are forward-declared rather
// than #included so this header stays cheap to include from the merge/test
// translation units that never touch D3D. The implementation (gpu_timer.cpp)
// pulls the real d3d11.h / d3d12.h via pch.h.
struct ID3D11Device;
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace openxr_api_layer::gpu {

    namespace detail {

        // Slot states for the GPU query ring. Lives outside GpuSlotRing so a
        // test (or a future backend) can name the values without instantiating
        // the template.
        enum class SlotState { Idle, Pending };

        // Fixed-capacity, single-thread state machine for the GPU query ring.
        // Tracks Pending vs Idle per slot and exposes overwrite-on-full
        // semantics + oldest-first drain order. Holds NO D3D objects -- the
        // concrete backend (GpuTimerD3D11) keeps its query handles in a
        // parallel std::array<...QueryPair, N> and uses the indices returned
        // here to drive its actual Begin/End/GetData calls. Extracted out of
        // GpuTimerD3D11 specifically so the tricky overwrite-on-full path
        // (gpu_timer.cpp Reserve()) can be unit-tested without a D3D11 device.
        //
        // Thread model: every method runs on the app's render thread, the
        // same thread that owns the D3D11 immediate context. No internal
        // synchronisation -- callers MUST NOT touch the ring from any other
        // thread.
        template <std::size_t N>
        class GpuSlotRing {
            static_assert(N > 0, "GpuSlotRing capacity must be positive");

          public:
            struct ReserveResult {
                // Index of the slot we reserved (caller writes its query data
                // into m_querySlots[slot_index] in the backend).
                std::size_t slot_index;
                // True iff the slot we reused was still Pending -- the GPU
                // hadn't caught up before we wrapped around. Caller increments
                // its dropped-frames counter on true.
                bool overwrote_pending;
            };

            // Reserve the next write slot for `display_time`, mark Pending,
            // advance the write cursor. On a full ring, overwrites the oldest
            // pending slot and bumps the read cursor past the dropped entry
            // so PollResolved never sees a freshly-overwritten slot as the
            // "next" frame.
            ReserveResult Reserve(uint64_t display_time) {
                const bool overwriting =
                    (m_ring[m_writeIdx].state == SlotState::Pending);
                const std::size_t reserved = m_writeIdx;
                m_ring[m_writeIdx].frame_index = display_time;
                m_ring[m_writeIdx].state = SlotState::Pending;
                m_writeIdx = (m_writeIdx + 1) % N;
                if (overwriting) {
                    // In a full ring, after the advance, the new write index
                    // points at the next-oldest pending slot -- which IS the
                    // new oldest. The read cursor tracks that.
                    m_readIdx = m_writeIdx;
                }
                return {reserved, overwriting};
            }

            // Look at the oldest Pending slot without consuming it. Returns
            // false if no slot is Pending (the empty case).
            bool PeekOldest(std::size_t& slot_index,
                            uint64_t& display_time) const {
                if (m_ring[m_readIdx].state != SlotState::Pending) {
                    return false;
                }
                slot_index = m_readIdx;
                display_time = m_ring[m_readIdx].frame_index;
                return true;
            }

            // Mark the oldest Pending slot Idle and advance the read cursor.
            // Precondition: PeekOldest just returned true (i.e. there IS an
            // oldest Pending slot). Used both for the success path and to
            // drop a slot whose GetData came back malformed.
            void ConsumeOldest() {
                m_ring[m_readIdx].state = SlotState::Idle;
                m_readIdx = (m_readIdx + 1) % N;
            }

            // Drop every Pending slot and reset both cursors. Called on a
            // fresh monitoring session (Ctrl+F9) so stale slots tagged with
            // the previous session's display_time numbering cannot resolve into
            // the just-truncated CSV.
            void Reset() {
                for (auto& s : m_ring) {
                    s.state = SlotState::Idle;
                    s.frame_index = 0;
                }
                m_writeIdx = 0;
                m_readIdx = 0;
            }

            // Test-only inspectors (used by test_gpu_ring.cpp; production
            // callers go through Reserve / PeekOldest / ConsumeOldest / Reset
            // exclusively). Both are const and inlinable, so leaving them in
            // the public API has zero cost in release.
            std::size_t WriteIndex() const { return m_writeIdx; }
            std::size_t ReadIndex() const { return m_readIdx; }
            SlotState SlotStateAt(std::size_t i) const {
                return m_ring[i].state;
            }
            uint64_t FrameIndexAt(std::size_t i) const {
                return m_ring[i].frame_index;
            }
            static constexpr std::size_t Capacity() { return N; }

          private:
            struct Slot {
                uint64_t frame_index = 0;
                SlotState state = SlotState::Idle;
            };
            std::array<Slot, N> m_ring{};
            std::size_t m_writeIdx = 0;
            std::size_t m_readIdx = 0;
        };

    } // namespace detail

    // One row of gpu-<pid>-<side>.csv, produced by PollResolved and written by
    // the GPU CSV sink. valid == 0 means the timestamp is unusable for this
    // frame (the disjoint query reported Disjoint, or frequency came back 0);
    // the merge must skip the GPU delta for any frame whose pre OR post row is
    // invalid. gpu_ticks is the raw GPU counter value; gpu_freq is the clock
    // rate from the disjoint query (ticks per second).
    struct GpuRow {
        uint64_t display_time;
        uint64_t gpu_ticks;
        uint64_t gpu_freq;
        uint32_t valid;
    };

    // Single-timestamp-per-frame GPU profiler. One concrete backend today
    // (D3D11); the interface is kept minimal so a D3D12 backend can drop in
    // behind it without changing any layer.cpp call site.
    //
    // All methods MUST run on the app's render thread (see THREADING above).
    class IGpuTimer {
      public:
        virtual ~IGpuTimer() = default;

        // Insert one GPU timestamp into the command stream at the current
        // point, tagging it with display_time so PollResolved can pair it back to
        // the CPU row later. Overwrites the oldest ring slot if the ring is
        // full (a multi-frame GPU stall) -- the dropped frame simply gets no
        // GPU row, which the merge tolerates.
        virtual void RecordTimestamp(uint64_t display_time) = 0;

        // Drain every slot whose GPU result is ready and append one GpuRow per
        // resolved frame to `out` (oldest first). Non-blocking: a slot whose
        // GetData is not yet ready stays in flight for a later poll. `out` is
        // appended to, never cleared -- the caller owns its lifetime and can
        // reuse it across frames to avoid per-frame allocation.
        virtual void PollResolved(std::vector<GpuRow>& out) = 0;

        // Drop all in-flight slots without resolving them. Called when a fresh
        // monitoring session starts (Ctrl+F9) so timestamps recorded under the
        // PREVIOUS session's display_time numbering cannot resolve into the new
        // (truncated, display_time-reset-to-0) CSV.
        virtual void Reset() = 0;

        // Count of frames the backend dropped IN THE CURRENT SESSION because
        // the GPU was more than the ring's capacity behind on readback (a
        // >~44 ms stall at 90 Hz, kGpuRingSize=4). Surfaced in the GPU CSV
        // footer so the user knows when a session's GPU timeline has gaps it
        // cannot attribute. Cleared by Reset() (which ApplyToggle calls on
        // every Ctrl+F9-on) so each session reports only ITS OWN drops --
        // earlier wording said "monotonic across sessions" but that lied to
        // the user: a clean second session would inherit the first session's
        // overflow count in its footer.
        //
        // D3D11 overwrites the in-flight query and counts the dropped frame;
        // D3D12 instead SKIPS the new frame (D3D12 forbids resetting a
        // command allocator while its commands execute) AND ALSO counts a
        // drop when an overwrite-on-full is detected at Reserve() time (the
        // ring's bookkeeping reused a slot whose previous PollResolved had
        // not yet drained it -- silent CSV loss without this count).
        // User-visible behaviour is identical: one frame's GPU sample is
        // missing, the merge leaves target_gpu_us blank.
        virtual uint64_t DroppedFrames() const = 0;

        // Lowercase identifier of the active backend ("d3d11", "d3d12").
        // Written into the GPU CSV's "# gpu_clock=" header line so the
        // user can tell at a glance which API produced the timestamps.
        // Returned as a static string -- safe to read from the writer
        // thread without ownership transfer.
        virtual const char* BackendName() const = 0;
    };

    // Build a D3D11 GPU timer on `device` (the app's ID3D11Device pulled from
    // XrGraphicsBindingD3D11KHR in xrCreateSession). Returns nullptr if device
    // is null or if query creation fails -- the caller then degrades to a
    // CPU-only session (gpu_timer stays null, no GPU rows are written, and the
    // merge leaves target_gpu_us blank). Never throws.
    std::unique_ptr<IGpuTimer> MakeD3D11GpuTimer(ID3D11Device* device);

    // Build a D3D12 GPU timer on the app's (device, command_queue) pair
    // pulled from XrGraphicsBindingD3D12KHR in xrCreateSession. The queue
    // MUST be of type D3D12_COMMAND_LIST_TYPE_DIRECT (the OpenXR spec
    // requires this for the app's render queue) -- a compute / copy queue
    // is refused at init. Returns nullptr on any creation failure (query
    // heap, readback buffer, allocators, command lists, fence); the caller
    // degrades to CPU-only the same way as for D3D11.
    //
    // Submits one tiny command list per frame to `command_queue` (an
    // EndQuery + ResolveQueryData), so the GPU work added to the app's
    // queue is essentially free (no draws, no transitions).
    std::unique_ptr<IGpuTimer> MakeD3D12GpuTimer(ID3D12Device* device,
                                                 ID3D12CommandQueue* command_queue);

} // namespace openxr_api_layer::gpu
