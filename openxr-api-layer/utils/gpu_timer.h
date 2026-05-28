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
// THE GPU SANDWICH (D3D11)
// ------------------------
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
// Both DLLs hold the SAME ID3D11Device (the app's device, handed identically
// to every layer through XrGraphicsBindingD3D11KHR), and timestamps on the
// same device read the same monotonic GPU counter, so the two values are
// directly subtractable. Each side wraps its single timestamp in its own tiny
// D3D11_QUERY_TIMESTAMP_DISJOINT bracket to recover the clock frequency and a
// "the clock was unstable at this instant" flag.
//
// ASYNCHRONY
// ----------
// A timestamp issued at frame N's xrEndFrame reflects the GPU clock at the
// moment that command actually executes -- typically 1-3 frames later. So the
// render thread can only READ frame N's timestamp once the GPU has caught up.
// Each backend keeps a small ring (kGpuRingSize) of in-flight slots;
// PollResolved() drains every slot whose GetData() has gone ready and emits
// one GpuRow per resolved frame. The CSV is therefore a few frames behind the
// CPU CSV at any instant, which the merge tolerates (it joins on frame_idx and
// leaves target_gpu_us blank for any frame missing a GPU row on either side).
//
// THREADING
// ---------
// Every method runs on the app's render thread (the thread that calls
// xrEndFrame). The D3D11 immediate context is single-threaded, so RecordTimes
// tamp / PollResolved / Reset must never be called from the CSV writer thread
// or any helper thread.
//
// LIMITATIONS (documented in the README too)
//   * D3D11 only for now. D3D12 / Vulkan hosts get no GPU rows (the CPU
//     sandwich still works); the merge leaves target_gpu_us blank.
//   * Captures only GPU work the target submits on the app's immediate context
//     between the two timestamps, in command-stream order. A target that
//     renders on a deferred context, a private device, or a separate queue is
//     invisible to this measurement.
//   * The per-side disjoint bracket only covers the instant of each timestamp,
//     not the whole span between them, so a GPU clock change DURING the
//     target's work between T_pre and T_post may go unflagged. Clock changes
//     are rare; re-run if a session looks suspicious. A frequency mismatch
//     between the two sides IS caught and blanks the frame.
// =============================================================================

#include <cstdint>
#include <memory>
#include <vector>

// ID3D11Device is forward-declared rather than #included so this header stays
// cheap to include from the merge/test translation units that never touch D3D.
// The implementation (gpu_timer.cpp) pulls the real d3d11.h via pch.h.
struct ID3D11Device;

namespace openxr_api_layer::gpu {

    // One row of gpu-<pid>-<side>.csv, produced by PollResolved and written by
    // the GPU CSV sink. valid == 0 means the timestamp is unusable for this
    // frame (the disjoint query reported Disjoint, or frequency came back 0);
    // the merge must skip the GPU delta for any frame whose pre OR post row is
    // invalid. gpu_ticks is the raw GPU counter value; gpu_freq is the clock
    // rate from the disjoint query (ticks per second).
    struct GpuRow {
        uint64_t frame_idx;
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
        // point, tagging it with frame_idx so PollResolved can pair it back to
        // the CPU row later. Overwrites the oldest ring slot if the ring is
        // full (a multi-frame GPU stall) -- the dropped frame simply gets no
        // GPU row, which the merge tolerates.
        virtual void RecordTimestamp(uint64_t frame_idx) = 0;

        // Drain every slot whose GPU result is ready and append one GpuRow per
        // resolved frame to `out` (oldest first). Non-blocking: a slot whose
        // GetData is not yet ready stays in flight for a later poll. `out` is
        // appended to, never cleared -- the caller owns its lifetime and can
        // reuse it across frames to avoid per-frame allocation.
        virtual void PollResolved(std::vector<GpuRow>& out) = 0;

        // Drop all in-flight slots without resolving them. Called when a fresh
        // monitoring session starts (Ctrl+F9) so timestamps recorded under the
        // PREVIOUS session's frame_idx numbering cannot resolve into the new
        // (truncated, frame_idx-reset-to-0) CSV.
        virtual void Reset() = 0;
    };

    // Build a D3D11 GPU timer on `device` (the app's ID3D11Device pulled from
    // XrGraphicsBindingD3D11KHR in xrCreateSession). Returns nullptr if device
    // is null or if query creation fails -- the caller then degrades to a
    // CPU-only session (gpu_timer stays null, no GPU rows are written, and the
    // merge leaves target_gpu_us blank). Never throws.
    std::unique_ptr<IGpuTimer> MakeD3D11GpuTimer(ID3D11Device* device);

} // namespace openxr_api_layer::gpu
