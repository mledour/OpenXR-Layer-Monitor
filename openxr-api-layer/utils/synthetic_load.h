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
// SYNTHETIC GPU LOAD -- D3D12 additivity-validation harness ONLY.  NOT SHIPPED.
//
// Purpose
// -------
// Decide whether the D3D12 GPU sandwich's ~220 us noise floor (see the empty
// pre/post run: target_gpu_us ~= 270 us with no target between the layers) is
// an ADDITIVE constant we can calibrate-and-subtract, or an artifact that a
// real target's GPU work ABSORBS.
//
// How it plugs in
// ---------------
// The monitor's PRE side calls RecordAndTime() AFTER it records its T_pre
// timestamp and BEFORE it forwards xrEndFrame downstream. So on the app's
// D3D12 command queue the work lands exactly between the sandwich's two
// markers, reproducing the canonical 3-submission topology of a real target:
//
//     [app draws] -> [T_pre submit] -> [gap_a][copy loop = K][gap_b] -> [T_post submit]
//
// Each frame this class brackets ITS OWN copy loop with a timestamp pair
// (T0, T1) on a single command list -- an INLINE duration measurement with no
// inter-submission gap, i.e. the artifact-free ground truth K = (T1-T0)/freq.
//
// The monitor's merge already computes target_gpu_us = (T_post - T_pre)/freq
// = S.  Joining gpuload-<pid>.csv (this class's K) against
// frames-merged-<pid>.csv (S) on display_time gives the per-frame overhead:
//
//     O = S - K
//
// Sweep the iteration count (MLEDOUR_GPULOAD_ITERS) to learn O's behaviour:
//   * O ~constant across the sweep            -> ADDITIVE  (subtract O)
//   * O -> 0 as K grows                       -> ABSORBED  (do not subtract)
//   * O at K~=0 ~= the empty baseline (~270us) -> empty calibration is OK
//   * O ~= 2x the empty baseline               -> per-boundary cost
//   * anything else                            -> document only
//
// Workload
// --------
// A loop of CopyBufferRegion on two DEFAULT-heap buffers. No PSO / shader /
// root signature / descriptor heap: buffer copies promote their resource
// state implicitly, so the loop is pure, bandwidth-bound, stable, and linear
// in the iteration count -- a clean knob.  Absolute per-iteration cost is
// IRRELEVANT because every frame self-times K; the sweep only needs DIFFERENT
// K values.
//
// Threading / lifetime
// --------------------
// Identical contract to gpu::IGpuTimer: every method runs on the app's render
// thread (the xrEndFrame thread).  Non-blocking on the per-frame path
// (RecordAndTime / PollResolved); the only blocking fence waits are at init
// (one-time src buffer fill) and in the destructor drain.  Gated behind
// MLEDOUR_GPULOAD_ITERS and instantiated only on the PRE side -- a normal
// monitoring session never builds one.
// =============================================================================

#include <cstdint>
#include <memory>
#include <vector>

// Forward-declared (see gpu_timer.h for the rationale): keeps this header
// cheap to include from the merge/test TUs that never touch D3D.
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace openxr_api_layer::load {

    // One row of gpuload-<pid>.csv. known_ticks is the inline-timed duration
    // (T1 - T0) of the copy loop in GPU ticks; gpu_freq converts it to seconds.
    // valid == 0 means the readback Map failed or the counter ran backwards --
    // the analysis skips such frames.
    struct LoadRow {
        uint64_t display_time;
        uint64_t known_ticks;
        uint64_t gpu_freq;
        uint32_t valid;
    };

    // Self-timed synthetic GPU workload submitted on the app's command queue.
    // See the file banner for the experiment it serves.
    class SyntheticGpuLoad {
      public:
        virtual ~SyntheticGpuLoad() = default;

        // Record + submit one copy-loop command list, self-timed by an
        // EndQuery(TIMESTAMP) pair, tagged with display_time. Non-blocking.
        // Skips the frame (no row emitted) if the target ring slot is still
        // executing on the GPU -- same overflow discipline as the D3D12 timer.
        virtual void RecordAndTime(uint64_t display_time) = 0;

        // Drain every self-timing whose GPU result is ready and append one
        // LoadRow per resolved frame (oldest first). Appends, never clears.
        virtual void PollResolved(std::vector<LoadRow>& out) = 0;

        // Copies per frame -- the sweep knob, echoed into the CSV header so
        // the analysis can label each run without a side channel.
        virtual uint64_t Iterations() const = 0;
    };

    // Build the load on the app's (device, queue) -- the same DIRECT queue the
    // monitor's D3D12 timer uses, pulled from XrGraphicsBindingD3D12KHR.
    // iterations = CopyBufferRegion calls per frame (0 is a valid control run:
    // K~=0, S measures the bare overhead). bufferBytes = size of each copy.
    // Returns nullptr on any creation failure (the caller just skips the load
    // for that session). Never throws.
    std::unique_ptr<SyntheticGpuLoad> MakeSyntheticGpuLoad(
        ID3D12Device* device, ID3D12CommandQueue* queue,
        uint64_t iterations, uint64_t bufferBytes);

}  // namespace openxr_api_layer::load
