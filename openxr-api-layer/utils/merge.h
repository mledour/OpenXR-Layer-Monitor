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

// Pure functions extracted from layer.cpp's MergeIntoOutput so tests can drive
// each step independently. None of these touch globals (localAppData, kSideStr,
// the writer thread...) -- they take paths and streams as arguments and return
// plain data. The thin wrapper in layer.cpp glues them to %LOCALAPPDATA% and
// the post-side destructor.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <vector>

namespace openxr_api_layer::merge {

    // One row of a per-side frames-<pid>-{pre,post}.csv, parsed from disk.
    struct RawFrameRow {
        uint64_t display_time;
        uint32_t thread_id;
        int64_t qpc_entry;
        int64_t qpc_exit;
    };

    // One row of a per-side gpu-<pid>-{pre,post}.csv, parsed from disk.
    // valid == 0 means the timestamp is unusable for this frame (the D3D11
    // disjoint query reported Disjoint, or frequency came back 0); JoinGpu
    // skips any frame whose pre OR post row is invalid. Not thread-tagged:
    // D3D11 GPU submission is single-threaded, so display_time alone is the key.
    struct RawGpuRow {
        uint64_t display_time;
        uint64_t gpu_ticks;
        uint64_t gpu_freq;
        uint32_t valid;
    };

    // Result of parsing a per-side CSV. qpc_freq comes from the
    // "# qpc_freq=..." header line; if the header is missing or malformed,
    // ReadFrameCsv falls back to whatever defaultFreq the caller passed.
    //
    // header_valid = false flags the "user pointed us at a merged CSV by
    // mistake" case (or any other file whose column-header line is not
    // exactly `display_time,thread_id,qpc_entry,qpc_exit`). Callers should
    // refuse to merge such input rather than silently producing zero
    // matched rows. The Python analyzer enforces the same contract via
    // a SystemExit with the same error wording.
    struct ParsedFrameCsv {
        std::vector<RawFrameRow> rows;
        int64_t qpc_freq;
        bool header_valid;
    };

    // Result of parsing a per-side gpu-<pid>-{pre,post}.csv. A MISSING file
    // (the common case on D3D12 / Vulkan / OpenGL hosts, where no GPU timer
    // is created) yields rows = {} and header_valid = true, so the merge
    // degrades to CPU-only with every target_gpu_us blank. header_valid is
    // flipped to false only when the file exists but its column-header line
    // is not exactly kExpectedGpuColumnHeader; callers warn and proceed
    // CPU-only rather than aborting the whole merge.
    struct ParsedGpuCsv {
        std::vector<RawGpuRow> rows;
        bool header_valid;
    };

    // Expected raw per-side CSV column header, exactly. Any deviation
    // (including the 8-column merged-CSV header) is rejected.
    inline constexpr const char* kExpectedColumnHeader =
        "display_time,thread_id,qpc_entry,qpc_exit";

    // Expected per-side GPU CSV column header, exactly.
    inline constexpr const char* kExpectedGpuColumnHeader =
        "display_time,gpu_ticks,gpu_freq,valid";

    // One row of the merged CSV. Optionals are blanked out when undefined:
    //   frame_interval_us / target_cpu_pct_of_frame -> last frame per thread
    //       (no successor) or a non-positive interval.
    //   target_gpu_us -> no GPU row on one/both sides for this frame, an
    //       invalid GPU sample, or a GPU clock mismatch (see JoinGpu).
    //   target_gpu_pct_of_frame -> target_gpu_us is blank, OR this frame has
    //       no frame_interval (last per thread). GPU duration as % of frame.
    struct MergedRow {
        uint64_t display_time;
        uint32_t thread_id;
        std::optional<double> frame_interval_us;
        double pre_us;
        double post_us;
        double target_us;
        std::optional<double> target_cpu_pct_of_frame;
        std::optional<double> target_gpu_us;
        std::optional<double> target_gpu_pct_of_frame;
    };

    // Session-summary numbers written as # comment lines at the top of the
    // merged CSV. target_cpu_pct_* aggregates ONLY frames that have a
    // frame_interval (i.e. every frame except the last per thread);
    // target_cpu_ms_* aggregates every matched frame. negative_target_count is
    // surfaced so callers can warn the user when QPC jitter (or a broken
    // chain) pushes some target_us below zero.
    struct MergeStats {
        size_t frame_count;
        double target_cpu_ms_mean;
        double target_cpu_ms_min;
        double target_cpu_ms_max;
        double target_cpu_pct_mean;
        double target_cpu_pct_min;
        double target_cpu_pct_max;
        size_t negative_target_count;
        // GPU aggregates over the frames that have a target_gpu_us (i.e. a
        // valid GPU sample on BOTH sides). gpu_frame_count == 0 means no GPU
        // data was captured this session (non-D3D11 host, or GPU timer
        // creation failed) -- the ms_*/pct_* fields are then 0.0 and the
        // merged CSV's target_gpu_us column is blank on every row.
        // target_gpu_pct_* aggregates the subset that ALSO has a
        // frame_interval (same exclusion as the CPU pct: last frame per
        // thread has none).
        size_t gpu_frame_count;
        double target_gpu_ms_mean;
        double target_gpu_ms_min;
        double target_gpu_ms_max;
        double target_gpu_pct_mean;
        double target_gpu_pct_min;
        double target_gpu_pct_max;
    };

    // Returns rows = {} and qpc_freq = defaultFreq if the file is missing,
    // unreadable, or has a malformed header. The column header row
    // (`display_time,thread_id,qpc_entry,qpc_exit`) is recognised and skipped;
    // mis-named columns or extra trailing data is silently ignored at the
    // row level (truncated parse).
    ParsedFrameCsv ReadFrameCsv(const std::filesystem::path& path,
                                int64_t defaultFreq);

    // Parses a per-side GPU CSV. Returns rows = {} and header_valid = true
    // when the file is missing (the expected case on non-D3D11 hosts), and
    // header_valid = false when the file exists but the column header is not
    // exactly kExpectedGpuColumnHeader. Rows that don't parse cleanly are
    // skipped silently (truncated parse), matching ReadFrameCsv.
    ParsedGpuCsv ReadGpuCsv(const std::filesystem::path& path);

    // Joins preRows + postRows by (display_time, thread_id), computes
    // target_us = pre_bracket - post_bracket, derives frame_interval_us
    // from pre.qpc_entry deltas (per thread), and sets target_cpu_pct_of_frame
    // accordingly. Result is sorted by (thread_id, display_time) for
    // deterministic CSV output. Rows missing from either side are dropped.
    //
    // Each (display_time, thread_id) post entry is CONSUMED on match (like
    // analyze.py's post_index.pop), so duplicate pre keys do not double-
    // count against the same post entry -- output row count equals number
    // of matched pairs, never more.
    //
    // preFreq is authoritative for BOTH sides (matches the Python analyzer
    // which uses pre.qpc_freq after warning on mismatch). postFreq is
    // currently ignored; kept in the signature for future cross-machine
    // bridging support, and so callers know there is one freq per side
    // they need to ferry through to here. The mismatch is the caller's
    // responsibility to warn about.
    //
    // Rows where the next-per-thread interval would be <= 0 (TSC went
    // backwards across a core migration on non-invariant hardware) get
    // frame_interval_us / target_cpu_pct_of_frame left blank, matching the
    // last-frame-per-thread case.
    std::vector<MergedRow> ComputeMerge(
        const std::vector<RawFrameRow>& preRows, int64_t preFreq,
        const std::vector<RawFrameRow>& postRows, int64_t postFreq);

    // Fills target_gpu_us on each MergedRow by joining preGpu + postGpu on
    // display_time (GPU rows carry no thread_id). For a given frame:
    //
    //   target_gpu_us = (post.gpu_ticks - pre.gpu_ticks) / pre.gpu_freq * 1e6
    //
    // left BLANK (target_gpu_us stays nullopt) unless ALL of:
    //   * a GPU row exists on BOTH sides for that display_time,
    //   * both rows are valid (valid != 0),
    //   * pre.gpu_freq == post.gpu_freq and is non-zero (a frequency change
    //     between the two timestamps means the GPU clock was disjoint across
    //     the measured span -- the delta would be meaningless),
    //   * post.gpu_ticks >= pre.gpu_ticks (a backwards counter is a driver
    //     bug; treating the unsigned wrap as a huge delta would poison stats).
    //
    // Must run BEFORE ComputeStats so the GPU aggregates see the populated
    // target_gpu_us values. Mutates `merged` in place. Duplicate display_time in
    // either GPU input resolves last-wins (matches analyze.py's dict join).
    void JoinGpu(std::vector<MergedRow>& merged,
                 const std::vector<RawGpuRow>& preGpu,
                 const std::vector<RawGpuRow>& postGpu);

    // Aggregate mean/min/max + negative-count over `merged`, including the
    // GPU aggregates over rows that have a target_gpu_us. Empty `merged`
    // yields all zeros and frame_count = gpu_frame_count = 0. Call AFTER
    // JoinGpu so the GPU stats reflect the joined values.
    MergeStats ComputeStats(const std::vector<MergedRow>& merged);

    // Writes the merged CSV with eleven `#`-comment header lines (seven CPU
    // stats + four GPU stats), then the column header, then one row per
    // MergedRow. The trailing column is target_gpu_us (blank when the row
    // has no GPU sample). Output is LF-only regardless of platform; the
    // caller is responsible for opening `out` in a mode that does not
    // translate line endings (binary mode on Windows).
    void WriteMergedCsv(std::ostream& out,
                        const std::vector<MergedRow>& merged,
                        const MergeStats& stats);

} // namespace openxr_api_layer::merge
