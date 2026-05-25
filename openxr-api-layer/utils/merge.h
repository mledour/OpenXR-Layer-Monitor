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
        uint64_t frame_idx;
        uint32_t thread_id;
        int64_t qpc_entry;
        int64_t qpc_exit;
    };

    // Result of parsing a per-side CSV. qpc_freq comes from the
    // "# qpc_freq=..." header line; if the header is missing or malformed,
    // ReadFrameCsv falls back to whatever defaultFreq the caller passed.
    //
    // header_valid = false flags the "user pointed us at a merged CSV by
    // mistake" case (or any other file whose column-header line is not
    // exactly `frame_idx,thread_id,qpc_entry,qpc_exit`). Callers should
    // refuse to merge such input rather than silently producing zero
    // matched rows. The Python analyzer enforces the same contract via
    // a SystemExit with the same error wording.
    struct ParsedFrameCsv {
        std::vector<RawFrameRow> rows;
        int64_t qpc_freq;
        bool header_valid;
    };

    // Expected raw per-side CSV column header, exactly. Any deviation
    // (including the 7-column merged-CSV header) is rejected.
    inline constexpr const char* kExpectedColumnHeader =
        "frame_idx,thread_id,qpc_entry,qpc_exit";

    // One row of the merged CSV. Optionals are blanked out for the last
    // frame per thread (no successor -> no interval -> no pct).
    struct MergedRow {
        uint64_t frame_idx;
        uint32_t thread_id;
        std::optional<double> frame_interval_us;
        double pre_us;
        double post_us;
        double target_us;
        std::optional<double> target_pct_of_frame;
    };

    // Session-summary numbers written as # comment lines at the top of the
    // merged CSV. target_pct_* aggregates ONLY frames that have a
    // frame_interval (i.e. every frame except the last per thread);
    // target_ms_* aggregates every matched frame. negative_target_count is
    // surfaced so callers can warn the user when QPC jitter (or a broken
    // chain) pushes some target_us below zero.
    struct MergeStats {
        size_t frame_count;
        double target_ms_mean;
        double target_ms_min;
        double target_ms_max;
        double target_pct_mean;
        double target_pct_min;
        double target_pct_max;
        size_t negative_target_count;
    };

    // Returns rows = {} and qpc_freq = defaultFreq if the file is missing,
    // unreadable, or has a malformed header. The column header row
    // (`frame_idx,thread_id,qpc_entry,qpc_exit`) is recognised and skipped;
    // mis-named columns or extra trailing data is silently ignored at the
    // row level (truncated parse).
    ParsedFrameCsv ReadFrameCsv(const std::filesystem::path& path,
                                int64_t defaultFreq);

    // Joins preRows + postRows by (frame_idx, thread_id), computes
    // target_us = pre_bracket - post_bracket, derives frame_interval_us
    // from pre.qpc_entry deltas (per thread), and sets target_pct_of_frame
    // accordingly. Result is sorted by (thread_id, frame_idx) for
    // deterministic CSV output. Rows missing from either side are dropped.
    //
    // Each (frame_idx, thread_id) post entry is CONSUMED on match (like
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
    // frame_interval_us / target_pct_of_frame left blank, matching the
    // last-frame-per-thread case.
    std::vector<MergedRow> ComputeMerge(
        const std::vector<RawFrameRow>& preRows, int64_t preFreq,
        const std::vector<RawFrameRow>& postRows, int64_t postFreq);

    // Aggregate mean/min/max + negative-count over `merged`. Empty `merged`
    // yields all zeros and frame_count=0.
    MergeStats ComputeStats(const std::vector<MergedRow>& merged);

    // Writes the merged CSV with seven `#`-comment header lines (stats),
    // then the column header, then one row per MergedRow. Output is LF-
    // only regardless of platform; the caller is responsible for opening
    // `out` in a mode that does not translate line endings (binary mode
    // on Windows).
    void WriteMergedCsv(std::ostream& out,
                        const std::vector<MergedRow>& merged,
                        const MergeStats& stats);

} // namespace openxr_api_layer::merge
