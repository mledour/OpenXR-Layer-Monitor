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
// test_merge.cpp -- exhaustive tests for utils/merge.{h,cpp}.
//
// The merge logic was extracted out of layer.cpp specifically so it could be
// tested without spinning up an OpenXR layer chain, a mock runtime, or even a
// background writer thread. Each TEST_CASE here exercises one of the four
// pure functions (ReadFrameCsv / ComputeMerge / ComputeStats / WriteMergedCsv)
// on synthetic input the test builds in-memory.
//
// Edge cases covered:
//   * Missing files, missing headers, malformed qpc_freq, garbage rows.
//   * Empty merge (no matching keys) and partial mismatch.
//   * Multi-thread frame_interval computation (per thread, not global).
//   * Last frame per thread = no successor = no frame_interval / no pct.
//   * Negative target_us (QPC noise floor) flagged in MergeStats.
//   * Deterministic sort order in ComputeMerge: (thread_id, display_time).
//   * Exact byte format produced by WriteMergedCsv -- this is the contract
//     between the in-DLL merge and analyze.py.
// =============================================================================

#include <doctest/doctest.h>

#include "utils/merge.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <locale>
#include <random>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace openxr_api_layer::merge;

// ----------------------------------------------------------------------------
// Test-only helpers
// ----------------------------------------------------------------------------

namespace {

    // Per-process directory for the test fixtures. The name mixes a
    // monotonic high-resolution timestamp captured once at startup and
    // four random_device draws -- so the two CI matrix shards
    // (Release / Debug) running on the same windows-2022 image never
    // collide, even if std::random_device happens to share seed across
    // processes. No <windows.h> needed.
    fs::path TestTempDir() {
        static const fs::path dir = [] {
            const auto now =
                std::chrono::high_resolution_clock::now()
                    .time_since_epoch()
                    .count();
            std::random_device rd;
            const std::string unique =
                std::to_string(static_cast<long long>(now)) + "_" +
                std::to_string(rd()) + "_" + std::to_string(rd()) + "_" +
                std::to_string(rd()) + "_" + std::to_string(rd());
            auto p = fs::temp_directory_path() /
                     ("openxr_layer_monitor_tests_" + unique);
            std::error_code ec;
            fs::create_directories(p, ec);
            return p;
        }();
        return dir;
    }

    // Write a raw per-side CSV with the layout the writer thread emits:
    //   # qpc_freq=...
    //   # side=...
    //   # layer=...
    //   # fn=xrEndFrame
    //   display_time,thread_id,qpc_entry,qpc_exit
    //   <rows>
    // Header lines are optional (callers pass nullopt to omit qpc_freq).
    fs::path WriteRawCsv(const std::string& name,
                         std::optional<int64_t> qpcFreq,
                         const std::vector<RawFrameRow>& rows) {
        const fs::path path = TestTempDir() / name;
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        REQUIRE(out);
        if (qpcFreq.has_value()) {
            out << "# qpc_freq=" << *qpcFreq << '\n';
        }
        out << "# side=test\n# layer=test\n# fn=xrEndFrame\n"
            << "display_time,thread_id,qpc_entry,qpc_exit\n";
        for (const auto& r : rows) {
            out << r.display_time << ',' << r.thread_id << ','
                << r.qpc_entry << ',' << r.qpc_exit << '\n';
        }
        return path;
    }

    // Convenience: build a RawFrameRow from positional args.
    RawFrameRow Row(uint64_t fi, uint32_t tid, int64_t qe, int64_t qx) {
        return RawFrameRow{fi, tid, qe, qx};
    }

    // Convenience: build a RawGpuRow from positional args. valid is uint32
    // (0 / 1) to match the on-disk encoding the layer writes.
    RawGpuRow GpuRow(uint64_t fi, uint64_t ticks, uint64_t freq, uint32_t valid) {
        return RawGpuRow{fi, ticks, freq, valid};
    }

    // Write a per-side GPU CSV with the layout GpuCsvSink emits:
    //   # gpu_clock=d3d11
    //   # side=...
    //   # layer=...
    //   # fn=xrEndFrame
    //   display_time,gpu_ticks,gpu_freq,valid
    //   <rows>
    fs::path WriteGpuCsv(const std::string& name,
                         const std::vector<RawGpuRow>& rows) {
        const fs::path path = TestTempDir() / name;
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        REQUIRE(out);
        out << "# gpu_clock=d3d11\n# side=test\n# layer=test\n# fn=xrEndFrame\n"
            << "display_time,gpu_ticks,gpu_freq,valid\n";
        for (const auto& r : rows) {
            out << r.display_time << ',' << r.gpu_ticks << ','
                << r.gpu_freq << ',' << r.valid << '\n';
        }
        return path;
    }

} // namespace

// ============================================================================
// ReadFrameCsv
// ============================================================================

TEST_CASE("ReadFrameCsv: missing file returns empty rows and default freq") {
    const auto path = TestTempDir() / "definitely_not_there.csv";
    std::error_code ec;
    fs::remove(path, ec);

    const auto parsed = ReadFrameCsv(path, /*defaultFreq=*/12345);
    CHECK(parsed.rows.empty());
    CHECK(parsed.qpc_freq == 12345);
    // Missing file is NOT a header violation -- the caller distinguishes
    // "absent" from "wrong shape" via header_valid + rows.empty() combo.
    CHECK(parsed.header_valid);
}

TEST_CASE("ReadFrameCsv: parses qpc_freq header") {
    const auto path = WriteRawCsv("rfc_freq.csv", /*freq=*/1234567,
                                  {Row(0, 100, 1'000, 1'500)});
    const auto parsed = ReadFrameCsv(path, /*defaultFreq=*/99);
    CHECK(parsed.qpc_freq == 1234567);
    REQUIRE(parsed.rows.size() == 1);
    CHECK(parsed.rows[0].display_time == 0);
    CHECK(parsed.rows[0].thread_id == 100);
    CHECK(parsed.rows[0].qpc_entry == 1'000);
    CHECK(parsed.rows[0].qpc_exit == 1'500);
}

TEST_CASE("ReadFrameCsv: missing qpc_freq header falls back to defaultFreq") {
    const auto path = WriteRawCsv("rfc_nofreq.csv", std::nullopt,
                                  {Row(0, 1, 100, 200)});
    const auto parsed = ReadFrameCsv(path, /*defaultFreq=*/7777);
    CHECK(parsed.qpc_freq == 7777);
    CHECK(parsed.rows.size() == 1);
}

TEST_CASE("ReadFrameCsv: malformed qpc_freq value keeps defaultFreq") {
    // All-letters, scientific notation (`1e7` would silently become 1 with
    // std::stoll), and trailing garbage (`10000000 Hz`). Each must fall
    // back to defaultFreq rather than be truncated to a wrong integer.
    auto check_falls_back = [](const std::string& freqLiteral,
                                const char* tag) {
        INFO("freq literal: " << freqLiteral << " (" << tag << ")");
        const fs::path path =
            TestTempDir() / ("rfc_badfreq_" + std::string(tag) + ".csv");
        {
            std::ofstream out(path);
            out << "# qpc_freq=" << freqLiteral << "\n"
                << "display_time,thread_id,qpc_entry,qpc_exit\n"
                << "0,1,10,20\n";
        }
        const auto parsed = ReadFrameCsv(path, /*defaultFreq=*/42);
        CHECK(parsed.qpc_freq == 42);
        CHECK(parsed.rows.size() == 1);
        CHECK(parsed.header_valid);
    };
    check_falls_back("not_a_number", "letters");
    check_falls_back("1e7", "scientific");
    check_falls_back("10000000 Hz", "trailing");
    check_falls_back("", "empty");
}

TEST_CASE("ReadFrameCsv: empty lines and stray comments are ignored") {
    const fs::path path = TestTempDir() / "rfc_blanks.csv";
    {
        std::ofstream out(path);
        out << "\n"
            << "# qpc_freq=1000\n"
            << "\n"
            << "# random comment\n"
            << "display_time,thread_id,qpc_entry,qpc_exit\n"
            << "0,1,10,20\n"
            << "\n"
            << "1,1,30,40\n";
    }
    const auto parsed = ReadFrameCsv(path, /*defaultFreq=*/0);
    CHECK(parsed.qpc_freq == 1000);
    REQUIRE(parsed.rows.size() == 2);
    CHECK(parsed.rows[0].display_time == 0);
    CHECK(parsed.rows[1].display_time == 1);
}

TEST_CASE("ReadFrameCsv: header_valid is false when the column header does not "
          "match the spec (e.g. merged CSV fed in by mistake)") {
    // Simulate a frames-merged-<pid>.csv fed in where the per-side CSV
    // belongs. Its first non-comment line lists eight columns (since GPU
    // monitoring was added), not the four ReadFrameCsv expects.
    const fs::path path = TestTempDir() / "rfc_merged_by_mistake.csv";
    {
        std::ofstream out(path);
        out << "# frame_count=42\n"
            << "# target_cpu_ms_mean=0.0123\n"
            << "display_time,thread_id,frame_interval_us,pre_us,post_us,"
               "target_us,target_cpu_pct_of_frame,target_gpu_us\n"
            << "0,1,11100.000,1.234,0.123,1.111,0.0100,5.000\n";
    }
    const auto parsed = ReadFrameCsv(path, /*defaultFreq=*/10'000'000);
    CHECK_FALSE(parsed.header_valid);
    CHECK(parsed.rows.empty());
}

TEST_CASE("ReadFrameCsv: trailing CR on the column header still validates") {
    // CRLF on the column-header line must not fail validation -- the user
    // may produce CSVs on Windows where every line is \r\n.
    const fs::path path = TestTempDir() / "rfc_crlf_header.csv";
    {
        std::ofstream out(path, std::ios::out | std::ios::binary);
        out << "# qpc_freq=10000000\r\n"
            << "display_time,thread_id,qpc_entry,qpc_exit\r\n"
            << "0,1,10,20\r\n";
    }
    const auto parsed = ReadFrameCsv(path, 0);
    CHECK(parsed.header_valid);
    CHECK(parsed.qpc_freq == 10'000'000);
    CHECK(parsed.rows.size() == 1);
}

TEST_CASE("ReadFrameCsv: a row with too few fields is skipped not crashed") {
    const fs::path path = TestTempDir() / "rfc_short.csv";
    {
        std::ofstream out(path);
        out << "# qpc_freq=1000\n"
            << "display_time,thread_id,qpc_entry,qpc_exit\n"
            << "0,1,10,20\n"
            << "garbage_with,too_few_fields\n"
            << "2,1,50,60\n";
    }
    const auto parsed = ReadFrameCsv(path, 0);
    REQUIRE(parsed.rows.size() == 2);
    CHECK(parsed.rows[0].display_time == 0);
    CHECK(parsed.rows[1].display_time == 2);
}

// ============================================================================
// ComputeMerge
// ============================================================================

TEST_CASE("ComputeMerge: empty input gives empty output") {
    CHECK(ComputeMerge({}, 1000, {}, 1000).empty());
    CHECK(ComputeMerge({Row(0, 1, 0, 1)}, 1000, {}, 1000).empty());
    CHECK(ComputeMerge({}, 1000, {Row(0, 1, 0, 1)}, 1000).empty());
}

TEST_CASE("ComputeMerge: single matched pair, no successor leaves interval blank") {
    // qpc_freq=1MHz -> 1 tick = 1 us. Delta of 1000 ticks = 1000 us = 1 ms.
    const std::vector<RawFrameRow> pre = {Row(0, 1, 1'000, 2'000)};
    const std::vector<RawFrameRow> post = {Row(0, 1, 1'100, 1'900)};
    const auto m = ComputeMerge(pre, 1'000'000, post, 1'000'000);
    REQUIRE(m.size() == 1);
    CHECK(m[0].display_time == 0);
    CHECK(m[0].thread_id == 1);
    CHECK(m[0].pre_us == doctest::Approx(1000.0));
    CHECK(m[0].post_us == doctest::Approx(800.0));
    CHECK(m[0].target_us == doctest::Approx(200.0));
    CHECK_FALSE(m[0].frame_interval_us.has_value());
    CHECK_FALSE(m[0].target_cpu_pct_of_frame.has_value());
}

TEST_CASE("ComputeMerge: two frames same thread, first row has interval second has none") {
    // qpc_freq=10M -> 1 tick = 0.1 us. Use round numbers so the test
    // exactly matches without floating-point fuzz.
    const std::vector<RawFrameRow> pre = {
        Row(0, 42, 10'000'000, 10'001'000),  // 100 us bracket
        Row(1, 42, 11'110'000, 11'111'000),  // 100 us bracket
    };
    const std::vector<RawFrameRow> post = {
        Row(0, 42, 10'000'400, 10'000'900),  // 50 us bracket
        Row(1, 42, 11'110'200, 11'110'800),  // 60 us bracket
    };
    const auto m = ComputeMerge(pre, 10'000'000, post, 10'000'000);
    REQUIRE(m.size() == 2);

    // Frame 0: has successor (frame 1).
    CHECK(m[0].display_time == 0);
    CHECK(m[0].pre_us == doctest::Approx(100.0));
    CHECK(m[0].post_us == doctest::Approx(50.0));
    CHECK(m[0].target_us == doctest::Approx(50.0));
    REQUIRE(m[0].frame_interval_us.has_value());
    CHECK(*m[0].frame_interval_us == doctest::Approx(111'000.0));
    REQUIRE(m[0].target_cpu_pct_of_frame.has_value());
    CHECK(*m[0].target_cpu_pct_of_frame ==
          doctest::Approx(50.0 / 111'000.0 * 100.0));

    // Frame 1: no successor.
    CHECK(m[1].display_time == 1);
    CHECK(m[1].target_us == doctest::Approx(40.0));
    CHECK_FALSE(m[1].frame_interval_us.has_value());
    CHECK_FALSE(m[1].target_cpu_pct_of_frame.has_value());
}

TEST_CASE("ComputeMerge: unmatched pre / post rows are dropped silently") {
    const std::vector<RawFrameRow> pre = {
        Row(0, 1, 100, 200),
        Row(1, 1, 300, 400),
        Row(2, 1, 500, 600),  // no post counterpart
    };
    const std::vector<RawFrameRow> post = {
        Row(0, 1, 110, 190),
        Row(2, 1, 510, 590),  // no pre at this idx? actually has, see above
        Row(99, 1, 700, 800), // no pre counterpart
    };
    const auto m = ComputeMerge(pre, 1000, post, 1000);
    // display_time 0 and 2 match. display_time 1 is pre-only, 99 is post-only.
    REQUIRE(m.size() == 2);
    CHECK(m[0].display_time == 0);
    CHECK(m[1].display_time == 2);
}

TEST_CASE("ComputeMerge: frame_interval computed per thread, not across") {
    // Thread 1 has frames 0, 1. Thread 2 has frame 0. Thread 2's single
    // frame must NOT take thread 1's frame 1 as its successor.
    const std::vector<RawFrameRow> pre = {
        Row(0, 1, 100, 200),
        Row(1, 1, 300, 400),
        Row(0, 2, 1'000'000, 1'000'100),
    };
    const std::vector<RawFrameRow> post = {
        Row(0, 1, 110, 190),
        Row(1, 1, 310, 390),
        Row(0, 2, 1'000'010, 1'000'090),
    };
    const auto m = ComputeMerge(pre, 1'000'000, post, 1'000'000);
    REQUIRE(m.size() == 3);

    // Sort order: thread 1 frames 0,1, then thread 2 frame 0.
    CHECK(m[0].thread_id == 1);
    CHECK(m[0].display_time == 0);
    CHECK(m[0].frame_interval_us.has_value());  // has successor in thread 1

    CHECK(m[1].thread_id == 1);
    CHECK(m[1].display_time == 1);
    CHECK_FALSE(m[1].frame_interval_us.has_value());  // last in thread 1

    CHECK(m[2].thread_id == 2);
    CHECK(m[2].display_time == 0);
    CHECK_FALSE(m[2].frame_interval_us.has_value());  // only one in thread 2
}

TEST_CASE("ComputeMerge: output sorted by (thread_id, display_time) regardless of input order") {
    // Feed pre rows out of order; ComputeMerge must still emit thread-then-
    // frame ordering.
    const std::vector<RawFrameRow> pre = {
        Row(5, 2, 500, 510),
        Row(0, 1, 100, 110),
        Row(0, 2, 50, 60),
        Row(3, 1, 300, 310),
    };
    const std::vector<RawFrameRow> post = {
        Row(5, 2, 502, 508),
        Row(0, 1, 102, 108),
        Row(0, 2, 52, 58),
        Row(3, 1, 302, 308),
    };
    const auto m = ComputeMerge(pre, 1000, post, 1000);
    REQUIRE(m.size() == 4);
    CHECK(m[0].thread_id == 1);
    CHECK(m[0].display_time == 0);
    CHECK(m[1].thread_id == 1);
    CHECK(m[1].display_time == 3);
    CHECK(m[2].thread_id == 2);
    CHECK(m[2].display_time == 0);
    CHECK(m[3].thread_id == 2);
    CHECK(m[3].display_time == 5);
}

TEST_CASE("ComputeMerge: preFreq is authoritative for both sides (matches analyze.py)") {
    // Contract per merge.h: postFreq is currently ignored, preFreq is
    // applied uniformly. analyze.py:147 does the same after warning about
    // the mismatch. So post's ticks here are scaled by preFreq=10M, not
    // by postFreq=1M; that is the WHOLE point of the new contract.
    const std::vector<RawFrameRow> pre = {Row(0, 1, 0, 1'000'000)};
    const std::vector<RawFrameRow> post = {Row(0, 1, 0, 500'000)};
    const auto m = ComputeMerge(pre, 10'000'000, post, 1'000'000);
    REQUIRE(m.size() == 1);
    // pre: 1_000_000 ticks / 10_000_000 Hz * 1e6 = 100_000 us.
    CHECK(m[0].pre_us == doctest::Approx(100'000.0));
    // post: 500_000 ticks / 10_000_000 Hz (preFreq!) * 1e6 = 50_000 us.
    // If we used postFreq=1_000_000, this would be 500_000 us instead.
    CHECK(m[0].post_us == doctest::Approx(50'000.0));
    CHECK(m[0].target_us == doctest::Approx(50'000.0));
}

TEST_CASE("ComputeMerge: duplicate pre rows do not double-count against a single post entry") {
    // Two pre rows share the same (display_time, thread_id) -- defensive
    // against a future writer-thread retry or any other source of dupes.
    // The first match consumes the post entry; the second sees no match
    // and is dropped. Output count = matched pairs, never inflated.
    const std::vector<RawFrameRow> pre = {
        Row(0, 1, 100, 200),
        Row(0, 1, 100, 200),  // duplicate
    };
    const std::vector<RawFrameRow> post = {
        Row(0, 1, 110, 190),  // single post entry
    };
    const auto m = ComputeMerge(pre, 1'000'000, post, 1'000'000);
    CHECK(m.size() == 1);
}

TEST_CASE("ComputeMerge: non-positive frame_interval leaves the interval + pct blank") {
    // qpc_entry[i+1] < qpc_entry[i] on the same thread happens on
    // non-invariant TSC hardware after a core migration. The "interval"
    // is undefined; emitting a negative number downstream would confuse
    // any tool that divides by it.
    const std::vector<RawFrameRow> pre = {
        Row(0, 1, 1'000, 1'500),     // pre.qpc_entry = 1000
        Row(1, 1, 900, 1'400),       // next pre.qpc_entry = 900 (< 1000)
    };
    const std::vector<RawFrameRow> post = {
        Row(0, 1, 1'100, 1'400),
        Row(1, 1, 950, 1'350),
    };
    const auto m = ComputeMerge(pre, 1'000'000, post, 1'000'000);
    REQUIRE(m.size() == 2);
    // First row (frame 0) has a NEGATIVE interval (900 - 1000 = -100).
    // Both interval and pct must be blank, NOT a negative number.
    CHECK_FALSE(m[0].frame_interval_us.has_value());
    CHECK_FALSE(m[0].target_cpu_pct_of_frame.has_value());
    // Second row (frame 1) is last per thread -> also blank, as before.
    CHECK_FALSE(m[1].frame_interval_us.has_value());
}

// ============================================================================
// ComputeStats
// ============================================================================

TEST_CASE("ComputeStats: empty merged gives all zeros") {
    const auto stats = ComputeStats({});
    CHECK(stats.frame_count == 0);
    CHECK(stats.target_cpu_ms_mean == 0.0);
    CHECK(stats.target_cpu_ms_min == 0.0);
    CHECK(stats.target_cpu_ms_max == 0.0);
    CHECK(stats.target_cpu_pct_mean == 0.0);
    CHECK(stats.negative_target_count == 0);
}

TEST_CASE("ComputeStats: ms aggregates over every row, pct only over rows with pct") {
    // Build merged rows directly (skip ComputeMerge). Two threads, three
    // frames total. Two have target_cpu_pct_of_frame, one (last per thread)
    // doesn't.
    std::vector<MergedRow> merged(3);
    merged[0].display_time = 0;
    merged[0].thread_id = 1;
    merged[0].pre_us = 100;
    merged[0].post_us = 60;
    merged[0].target_us = 40;
    merged[0].frame_interval_us = 11000.0;
    merged[0].target_cpu_pct_of_frame = 40.0 / 11000.0 * 100.0;

    merged[1].display_time = 1;
    merged[1].thread_id = 1;
    merged[1].pre_us = 80;
    merged[1].post_us = 70;
    merged[1].target_us = 10;
    // No successor on thread 1.

    merged[2].display_time = 0;
    merged[2].thread_id = 2;
    merged[2].pre_us = 200;
    merged[2].post_us = 50;
    merged[2].target_us = 150;
    // No successor (only frame on thread 2).

    const auto stats = ComputeStats(merged);
    CHECK(stats.frame_count == 3);
    // target_cpu_ms_*: 0.040, 0.010, 0.150 ms.
    CHECK(stats.target_cpu_ms_mean == doctest::Approx((0.040 + 0.010 + 0.150) / 3.0));
    CHECK(stats.target_cpu_ms_min == doctest::Approx(0.010));
    CHECK(stats.target_cpu_ms_max == doctest::Approx(0.150));
    // target_cpu_pct_*: only frame 0/thread 1 contributes.
    CHECK(stats.target_cpu_pct_mean ==
          doctest::Approx(40.0 / 11000.0 * 100.0));
    CHECK(stats.target_cpu_pct_min ==
          doctest::Approx(40.0 / 11000.0 * 100.0));
    CHECK(stats.target_cpu_pct_max ==
          doctest::Approx(40.0 / 11000.0 * 100.0));
    CHECK(stats.negative_target_count == 0);
}

TEST_CASE("ComputeStats: negative target_us counted") {
    std::vector<MergedRow> merged(3);
    merged[0].target_us = 100.0;
    merged[1].target_us = -50.0;
    merged[2].target_us = -10.0;
    const auto stats = ComputeStats(merged);
    CHECK(stats.negative_target_count == 2);
    CHECK(stats.target_cpu_ms_min == doctest::Approx(-0.050));
    CHECK(stats.target_cpu_ms_max == doctest::Approx(0.100));
}

// ============================================================================
// WriteMergedCsv
// ============================================================================

TEST_CASE("WriteMergedCsv: header has the fourteen # lines + column line") {
    std::vector<MergedRow> merged(1);
    merged[0].display_time = 0;
    merged[0].thread_id = 12345;
    merged[0].pre_us = 100.0;
    merged[0].post_us = 50.0;
    merged[0].target_us = 50.0;
    merged[0].frame_interval_us = 11100.0;
    merged[0].target_cpu_pct_of_frame = 50.0 / 11100.0 * 100.0;
    // No target_gpu_us on this row -- exercises the CPU-only-session path
    // where the seven GPU header lines must still appear (with zeros) so the
    // merged-CSV schema stays uniform across D3D11 / non-D3D11 sessions.

    const auto stats = ComputeStats(merged);
    std::ostringstream out;
    WriteMergedCsv(out, merged, stats);

    const std::string s = out.str();
    // 7 CPU stat lines + 7 GPU stat lines + 1 column header + 1 data row.
    INFO("Full output:\n" << s);
    CHECK(s.find("# frame_count=1\n") != std::string::npos);
    CHECK(s.find("# target_cpu_ms_mean=0.0500\n") != std::string::npos);
    CHECK(s.find("# target_cpu_ms_min=0.0500\n") != std::string::npos);
    CHECK(s.find("# target_cpu_ms_max=0.0500\n") != std::string::npos);
    // pct lines carry the '%' suffix (per the user request).
    CHECK(s.find("# target_cpu_pct_mean=0.4505%\n") != std::string::npos);
    CHECK(s.find("# target_cpu_pct_min=0.4505%\n") != std::string::npos);
    CHECK(s.find("# target_cpu_pct_max=0.4505%\n") != std::string::npos);
    // GPU block: no rows have target_gpu_us, so the count is 0 and the
    // ms_*/pct_* aggregates round to 0.0000. The ms lines carry no '%'
    // suffix; the pct lines do.
    CHECK(s.find("# target_gpu_frame_count=0\n") != std::string::npos);
    CHECK(s.find("# target_gpu_ms_mean=0.0000\n") != std::string::npos);
    CHECK(s.find("# target_gpu_ms_min=0.0000\n") != std::string::npos);
    CHECK(s.find("# target_gpu_ms_max=0.0000\n") != std::string::npos);
    CHECK(s.find("# target_gpu_pct_mean=0.0000%\n") != std::string::npos);
    CHECK(s.find("# target_gpu_pct_min=0.0000%\n") != std::string::npos);
    CHECK(s.find("# target_gpu_pct_max=0.0000%\n") != std::string::npos);
    // Column header gained the target_gpu_us + target_gpu_pct_of_frame columns.
    CHECK(s.find("display_time,thread_id,frame_interval_us,pre_us,post_us,"
                 "target_us,target_cpu_pct_of_frame,target_gpu_us,"
                 "target_gpu_pct_of_frame\n") !=
          std::string::npos);
}

TEST_CASE("WriteMergedCsv: data row format matches the .3f/.4f contract") {
    std::vector<MergedRow> merged(2);
    merged[0].display_time = 0;
    merged[0].thread_id = 42;
    merged[0].pre_us = 1.234567;
    merged[0].post_us = 0.123456;
    merged[0].target_us = 1.111111;
    merged[0].frame_interval_us = 11111.555;
    merged[0].target_cpu_pct_of_frame = 0.01000;
    merged[0].target_gpu_us = 12.345678;  // .3f -> "12.346"
    merged[0].target_gpu_pct_of_frame = 0.05000;  // .4f -> "0.0500"

    // Last row per thread: optionals blank, including both GPU columns.
    merged[1].display_time = 1;
    merged[1].thread_id = 42;
    merged[1].pre_us = 2.000;
    merged[1].post_us = 1.000;
    merged[1].target_us = 1.000;
    // frame_interval_us / target_cpu_pct_of_frame / target_gpu_us /
    // target_gpu_pct_of_frame stay nullopt.

    const auto stats = ComputeStats(merged);
    std::ostringstream out;
    WriteMergedCsv(out, merged, stats);

    const std::string s = out.str();
    // Row with all nine cells populated: .3f on the GPU duration, .4f on the
    // GPU pct, ending on a newline (no trailing comma at end-of-line).
    CHECK(s.find("0,42,11111.555,1.235,0.123,1.111,0.0100,12.346,0.0500\n") !=
          std::string::npos);
    // Last row: empty frame_interval_us, target_cpu_pct_of_frame,
    // target_gpu_us AND target_gpu_pct_of_frame -- four blank cells, three
    // trailing commas before the newline.
    CHECK(s.find("1,42,,2.000,1.000,1.000,,,\n") != std::string::npos);
}

TEST_CASE("WriteMergedCsv: output uses LF line endings on disk via a binary ofstream") {
    // Round-trip through a real std::ofstream opened in binary mode, then
    // re-read as raw bytes. This exercises the layer.cpp:std::ios::binary
    // contract -- an in-memory std::ostringstream check (used previously)
    // could not catch a regression where someone drops the binary flag,
    // because ostringstream is mode-agnostic.
    std::vector<MergedRow> merged(1);
    merged[0].display_time = 0;
    merged[0].thread_id = 1;
    merged[0].pre_us = 1.0;
    merged[0].post_us = 0.0;
    merged[0].target_us = 1.0;

    const auto stats = ComputeStats(merged);
    const fs::path path = TestTempDir() / "wmc_lf_disk.csv";
    {
        std::ofstream out(path,
                          std::ios::out | std::ios::trunc | std::ios::binary);
        REQUIRE(out);
        WriteMergedCsv(out, merged, stats);
    }
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>{});
    CHECK(bytes.find('\r') == std::string::npos);
    // Sanity: the file should contain at least one '\n' (otherwise the
    // CHECK above would trivially pass on an empty file).
    CHECK(bytes.find('\n') != std::string::npos);
}

// ============================================================================
// ReadGpuCsv
// ============================================================================

TEST_CASE("ReadGpuCsv: missing file returns empty rows and header_valid=true") {
    // Missing GPU CSV is the normal case on non-D3D11 hosts. The caller
    // proceeds CPU-only with every target_gpu_us blank -- header_valid must
    // stay true so the caller does not mistake it for a parse error.
    const auto path = TestTempDir() / "gpu_definitely_not_there.csv";
    std::error_code ec;
    fs::remove(path, ec);
    const auto parsed = ReadGpuCsv(path);
    CHECK(parsed.rows.empty());
    CHECK(parsed.header_valid);
}

TEST_CASE("ReadGpuCsv: parses a well-formed GPU CSV") {
    const auto path = WriteGpuCsv("gpu_basic.csv", {
        GpuRow(0, 10'000, 1'000'000'000, 1),
        GpuRow(1, 20'000, 1'000'000'000, 0),
    });
    const auto parsed = ReadGpuCsv(path);
    CHECK(parsed.header_valid);
    REQUIRE(parsed.rows.size() == 2);
    CHECK(parsed.rows[0].display_time == 0);
    CHECK(parsed.rows[0].gpu_ticks == 10'000);
    CHECK(parsed.rows[0].gpu_freq == 1'000'000'000);
    CHECK(parsed.rows[0].valid == 1);
    CHECK(parsed.rows[1].valid == 0);
}

TEST_CASE("ReadGpuCsv: header_valid is false on a malformed column header") {
    // A garbled column header must signal "skip GPU join" via header_valid
    // = false rather than aborting -- the CPU merge has to keep running.
    const fs::path path = TestTempDir() / "gpu_bad_header.csv";
    {
        std::ofstream out(path);
        out << "# gpu_clock=d3d11\n# side=pre\n"
            << "this_is_not_a_valid_header\n"
            << "0,10000,1000000000,1\n";
    }
    const auto parsed = ReadGpuCsv(path);
    CHECK_FALSE(parsed.header_valid);
    CHECK(parsed.rows.empty());
}

TEST_CASE("ReadGpuCsv: CRLF on the column header still validates") {
    const fs::path path = TestTempDir() / "gpu_crlf.csv";
    {
        std::ofstream out(path, std::ios::out | std::ios::binary);
        out << "# gpu_clock=d3d11\r\n"
            << "display_time,gpu_ticks,gpu_freq,valid\r\n"
            << "0,123,456,1\r\n";
    }
    const auto parsed = ReadGpuCsv(path);
    CHECK(parsed.header_valid);
    REQUIRE(parsed.rows.size() == 1);
    CHECK(parsed.rows[0].gpu_ticks == 123);
}

// ============================================================================
// JoinGpu
// ============================================================================

TEST_CASE("JoinGpu: empty inputs leave every target_gpu_us blank") {
    std::vector<MergedRow> merged(2);
    merged[0].display_time = 0; merged[0].thread_id = 1;
    merged[1].display_time = 1; merged[1].thread_id = 1;
    JoinGpu(merged, {}, {});
    CHECK_FALSE(merged[0].target_gpu_us.has_value());
    CHECK_FALSE(merged[1].target_gpu_us.has_value());
}

TEST_CASE("JoinGpu: matched valid pair fills target_gpu_us") {
    // 1 GHz GPU clock -> 1 tick = 1 ns -> 1000 ticks = 1 us.
    std::vector<MergedRow> merged(1);
    merged[0].display_time = 5;
    merged[0].thread_id = 42;
    JoinGpu(merged,
            {GpuRow(5, 10'000, 1'000'000'000, 1)},
            {GpuRow(5, 15'000, 1'000'000'000, 1)});
    REQUIRE(merged[0].target_gpu_us.has_value());
    CHECK(*merged[0].target_gpu_us == doctest::Approx(5.0));
}

TEST_CASE("JoinGpu: a valid=0 row on either side blanks the frame") {
    // Pre invalid.
    {
        std::vector<MergedRow> merged(1);
        merged[0].display_time = 0; merged[0].thread_id = 1;
        JoinGpu(merged,
                {GpuRow(0, 10'000, 1'000'000'000, 0)},
                {GpuRow(0, 15'000, 1'000'000'000, 1)});
        CHECK_FALSE(merged[0].target_gpu_us.has_value());
    }
    // Post invalid.
    {
        std::vector<MergedRow> merged(1);
        merged[0].display_time = 0; merged[0].thread_id = 1;
        JoinGpu(merged,
                {GpuRow(0, 10'000, 1'000'000'000, 1)},
                {GpuRow(0, 15'000, 1'000'000'000, 0)});
        CHECK_FALSE(merged[0].target_gpu_us.has_value());
    }
}

TEST_CASE("JoinGpu: gpu_freq mismatch blanks the frame") {
    // Different reported clock rates mean the GPU clock was disjoint across
    // the measured span -- the delta would be meaningless.
    std::vector<MergedRow> merged(1);
    merged[0].display_time = 0; merged[0].thread_id = 1;
    JoinGpu(merged,
            {GpuRow(0, 10'000, 1'000'000'000, 1)},
            {GpuRow(0, 15'000, 2'000'000'000, 1)});
    CHECK_FALSE(merged[0].target_gpu_us.has_value());
}

TEST_CASE("JoinGpu: gpu_freq == 0 blanks the frame") {
    std::vector<MergedRow> merged(1);
    merged[0].display_time = 0; merged[0].thread_id = 1;
    JoinGpu(merged,
            {GpuRow(0, 10'000, 0, 1)},
            {GpuRow(0, 15'000, 0, 1)});
    CHECK_FALSE(merged[0].target_gpu_us.has_value());
}

TEST_CASE("JoinGpu: post_ticks < pre_ticks (driver bug) blanks the frame") {
    // Treating the unsigned wrap of (post - pre) as the delta would
    // produce a huge bogus value. The join must refuse it.
    std::vector<MergedRow> merged(1);
    merged[0].display_time = 0; merged[0].thread_id = 1;
    JoinGpu(merged,
            {GpuRow(0, 20'000, 1'000'000'000, 1)},
            {GpuRow(0, 10'000, 1'000'000'000, 1)});
    CHECK_FALSE(merged[0].target_gpu_us.has_value());
}

TEST_CASE("JoinGpu: a frame missing on one GPU side stays blank") {
    std::vector<MergedRow> merged(2);
    merged[0].display_time = 0; merged[0].thread_id = 1;
    merged[1].display_time = 1; merged[1].thread_id = 1;
    JoinGpu(merged,
            {GpuRow(0, 10'000, 1'000'000'000, 1)},    // only frame 0 pre
            {GpuRow(1, 15'000, 1'000'000'000, 1)});   // only frame 1 post
    CHECK_FALSE(merged[0].target_gpu_us.has_value());
    CHECK_FALSE(merged[1].target_gpu_us.has_value());
}

TEST_CASE("JoinGpu: ignores GPU rows that don't match any CPU merged row") {
    // GPU rows for frames the CPU side never saw must NOT crash or invent
    // rows -- they are simply ignored.
    std::vector<MergedRow> merged(1);
    merged[0].display_time = 0; merged[0].thread_id = 1;
    JoinGpu(merged,
            {GpuRow(0, 10'000, 1'000'000'000, 1),
             GpuRow(999, 99'000, 1'000'000'000, 1)},
            {GpuRow(0, 15'000, 1'000'000'000, 1),
             GpuRow(999, 99'500, 1'000'000'000, 1)});
    REQUIRE(merged[0].target_gpu_us.has_value());
    CHECK(*merged[0].target_gpu_us == doctest::Approx(5.0));
    CHECK(merged.size() == 1);  // no new rows invented
}

TEST_CASE("JoinGpu: duplicate display_time in input resolves last-wins") {
    // Matches std::map[key] = row on the C++ side and analyze.py's dict
    // comprehension. The contract is "later rows overwrite", not "first
    // wins".
    std::vector<MergedRow> merged(1);
    merged[0].display_time = 0; merged[0].thread_id = 1;
    JoinGpu(merged,
            {GpuRow(0, 10'000, 1'000'000'000, 1),
             GpuRow(0, 12'000, 1'000'000'000, 1)},   // last-wins: 12000
            {GpuRow(0, 15'000, 1'000'000'000, 1)});
    REQUIRE(merged[0].target_gpu_us.has_value());
    // delta = 15000 - 12000 = 3000 ticks / 1 GHz = 3 us.
    CHECK(*merged[0].target_gpu_us == doctest::Approx(3.0));
}

// ============================================================================
// ComputeStats GPU aggregates
// ============================================================================

TEST_CASE("ComputeStats: gpu aggregates over rows with target_gpu_us only") {
    std::vector<MergedRow> merged(3);
    // Three frames; first two have GPU samples, third doesn't.
    for (size_t i = 0; i < merged.size(); ++i) {
        merged[i].display_time = static_cast<uint64_t>(i);
        merged[i].thread_id = 1;
        merged[i].pre_us = 100.0;
        merged[i].post_us = 50.0;
        merged[i].target_us = 50.0;
    }
    merged[0].target_gpu_us = 5'000.0;   // 5.0 ms
    merged[1].target_gpu_us = 8'000.0;   // 8.0 ms
    // merged[2].target_gpu_us stays nullopt -- excluded from GPU stats.
    // GPU pct is its own subset (a frame also needs an interval); give the
    // two GPU frames a pct and leave the third without.
    merged[0].target_gpu_pct_of_frame = 10.0;
    merged[1].target_gpu_pct_of_frame = 20.0;

    const auto stats = ComputeStats(merged);
    CHECK(stats.gpu_frame_count == 2);
    CHECK(stats.target_gpu_ms_mean == doctest::Approx((5.0 + 8.0) / 2.0));
    CHECK(stats.target_gpu_ms_min == doctest::Approx(5.0));
    CHECK(stats.target_gpu_ms_max == doctest::Approx(8.0));
    CHECK(stats.target_gpu_pct_mean == doctest::Approx((10.0 + 20.0) / 2.0));
    CHECK(stats.target_gpu_pct_min == doctest::Approx(10.0));
    CHECK(stats.target_gpu_pct_max == doctest::Approx(20.0));
}

TEST_CASE("ComputeStats: gpu aggregates are zero when no row has target_gpu_us") {
    std::vector<MergedRow> merged(2);
    merged[0].target_us = 50.0;
    merged[1].target_us = 40.0;
    const auto stats = ComputeStats(merged);
    CHECK(stats.gpu_frame_count == 0);
    CHECK(stats.target_gpu_ms_mean == 0.0);
    CHECK(stats.target_gpu_ms_min == 0.0);
    CHECK(stats.target_gpu_ms_max == 0.0);
    CHECK(stats.target_gpu_pct_mean == 0.0);
    CHECK(stats.target_gpu_pct_min == 0.0);
    CHECK(stats.target_gpu_pct_max == 0.0);
}

// ============================================================================
// End-to-end (Read -> Merge -> Stats -> Write)
// ============================================================================

TEST_CASE("end-to-end: round-trip raw CSVs through merge to merged CSV") {
    // Mirror the smoke test that scripts/analyze.py validates against.
    // qpc_freq = 10 MHz means 1 tick = 0.1 us.
    const std::vector<RawFrameRow> preRows = {
        Row(0, 12345, 1'000'000, 1'001'100),
        Row(1, 12345, 1'111'000, 1'112'000),
        Row(2, 12345, 1'222'000, 1'223'500),
        Row(3, 12345, 1'333'000, 1'334'200),
    };
    const std::vector<RawFrameRow> postRows = {
        Row(0, 12345, 1'000'400, 1'001'000),
        Row(1, 12345, 1'111'200, 1'111'800),
        Row(2, 12345, 1'222'300, 1'223'000),
        Row(3, 12345, 1'333'300, 1'334'000),
    };
    const auto preCsv = WriteRawCsv("e2e_pre.csv", 10'000'000, preRows);
    const auto postCsv = WriteRawCsv("e2e_post.csv", 10'000'000, postRows);

    const auto pre = ReadFrameCsv(preCsv, 0);
    const auto post = ReadFrameCsv(postCsv, 0);
    REQUIRE(pre.qpc_freq == 10'000'000);
    REQUIRE(post.qpc_freq == 10'000'000);

    const auto merged = ComputeMerge(pre.rows, pre.qpc_freq,
                                     post.rows, post.qpc_freq);
    REQUIRE(merged.size() == 4);
    const auto stats = ComputeStats(merged);
    CHECK(stats.frame_count == 4);
    // Same values as the python smoke test: target_us 50, 40, 80, 50.
    CHECK(stats.target_cpu_ms_mean == doctest::Approx(0.0550));
    CHECK(stats.target_cpu_ms_min == doctest::Approx(0.0400));
    CHECK(stats.target_cpu_ms_max == doctest::Approx(0.0800));

    std::ostringstream out;
    WriteMergedCsv(out, merged, stats);
    const std::string s = out.str();
    // Sanity-check the same hand-verified header the README documents.
    CHECK(s.find("# frame_count=4\n") != std::string::npos);
    CHECK(s.find("# target_cpu_ms_mean=0.0550\n") != std::string::npos);
    // No GPU CSVs were read for this case, so target_gpu_us stays blank on
    // every row and the seven GPU header lines all read zero.
    CHECK(s.find("# target_gpu_frame_count=0\n") != std::string::npos);
    CHECK(s.find("# target_gpu_ms_mean=0.0000\n") != std::string::npos);
}

TEST_CASE("end-to-end: with GPU CSVs the join fills target_gpu_us") {
    // Same CPU fixture as the smoke test above, plus a synthetic GPU pair
    // at 1 GHz (1 tick = 1 ns). Verifies the full pipeline:
    //   Read{Frame,Gpu}Csv -> ComputeMerge -> JoinGpu -> ComputeStats ->
    //   WriteMergedCsv. The merged output must carry a populated target_gpu_us
    //   column on every frame with a valid sample on both sides, and the GPU
    //   stat lines must reflect the actual count + min/max.
    const std::vector<RawFrameRow> preRows = {
        Row(0, 12345, 1'000'000, 1'001'100),
        Row(1, 12345, 1'111'000, 1'112'000),
    };
    const std::vector<RawFrameRow> postRows = {
        Row(0, 12345, 1'000'400, 1'001'000),
        Row(1, 12345, 1'111'200, 1'111'800),
    };
    const auto preCsv = WriteRawCsv("e2e_gpu_pre.csv", 10'000'000, preRows);
    const auto postCsv = WriteRawCsv("e2e_gpu_post.csv", 10'000'000, postRows);
    // GPU: frame 0 delta 5000 ticks = 5 us; frame 1 delta 8000 ticks = 8 us.
    const auto gpuPreCsv = WriteGpuCsv("e2e_gpu_pre_gpu.csv", {
        GpuRow(0, 10'000, 1'000'000'000, 1),
        GpuRow(1, 20'000, 1'000'000'000, 1),
    });
    const auto gpuPostCsv = WriteGpuCsv("e2e_gpu_post_gpu.csv", {
        GpuRow(0, 15'000, 1'000'000'000, 1),
        GpuRow(1, 28'000, 1'000'000'000, 1),
    });

    const auto pre = ReadFrameCsv(preCsv, 0);
    const auto post = ReadFrameCsv(postCsv, 0);
    const auto gpuPre = ReadGpuCsv(gpuPreCsv);
    const auto gpuPost = ReadGpuCsv(gpuPostCsv);
    CHECK(gpuPre.header_valid);
    CHECK(gpuPost.header_valid);

    auto merged = ComputeMerge(pre.rows, pre.qpc_freq,
                               post.rows, post.qpc_freq);
    REQUIRE(merged.size() == 2);
    JoinGpu(merged, gpuPre.rows, gpuPost.rows);
    REQUIRE(merged[0].target_gpu_us.has_value());
    REQUIRE(merged[1].target_gpu_us.has_value());
    CHECK(*merged[0].target_gpu_us == doctest::Approx(5.0));
    CHECK(*merged[1].target_gpu_us == doctest::Approx(8.0));
    // GPU pct: frame 0 has a successor (interval = 111'000 ticks / 10 MHz =
    // 11'100 us), so JoinGpu fills its pct; frame 1 is the last per thread
    // (no interval) so its pct stays blank even though target_gpu_us is set.
    REQUIRE(merged[0].target_gpu_pct_of_frame.has_value());
    CHECK(*merged[0].target_gpu_pct_of_frame ==
          doctest::Approx(5.0 / 11100.0 * 100.0));
    CHECK_FALSE(merged[1].target_gpu_pct_of_frame.has_value());

    const auto stats = ComputeStats(merged);
    CHECK(stats.gpu_frame_count == 2);
    CHECK(stats.target_gpu_ms_mean == doctest::Approx((0.005 + 0.008) / 2.0));
    CHECK(stats.target_gpu_ms_min == doctest::Approx(0.005));
    CHECK(stats.target_gpu_ms_max == doctest::Approx(0.008));
    // Only frame 0 has BOTH a GPU sample and an interval -> single-value pct.
    CHECK(stats.target_gpu_pct_mean == doctest::Approx(5.0 / 11100.0 * 100.0));
    CHECK(stats.target_gpu_pct_min == doctest::Approx(5.0 / 11100.0 * 100.0));
    CHECK(stats.target_gpu_pct_max == doctest::Approx(5.0 / 11100.0 * 100.0));

    std::ostringstream out;
    WriteMergedCsv(out, merged, stats);
    const std::string s = out.str();
    CHECK(s.find("# target_gpu_frame_count=2\n") != std::string::npos);
    CHECK(s.find("# target_gpu_ms_mean=0.0065\n") != std::string::npos);
    CHECK(s.find("# target_gpu_ms_min=0.0050\n") != std::string::npos);
    CHECK(s.find("# target_gpu_ms_max=0.0080\n") != std::string::npos);
    // Each data row carries the .3f GPU delta then the .4f GPU pct. Frame 0
    // has an interval (-> pct = 5/11100*100 = 0.0450); frame 1 is last
    // (-> blank pct, leaving a trailing comma before the newline).
    CHECK(s.find(",5.000,0.0450\n") != std::string::npos);
    CHECK(s.find(",8.000,\n") != std::string::npos);
}

// ============================================================================
// Locale immunity (#6): integer columns must not pick up a grouping locale
// ============================================================================

TEST_CASE("WriteMergedCsv integer columns survive a grouping global locale") {
    // Some middleware / Qt launchers call std::locale::global(std::locale(""))
    // and land on a thousands-grouping locale. Our integer columns must NOT
    // then sprout separators like "1,234,567" -- that injects spurious CSV
    // fields and corrupts the merge. WriteMergedCsv pins std::locale::classic()
    // on its stream to stay immune. (Floats go through fmt, already locale-
    // independent, so only the integer columns are at risk.)
    struct Grouping : std::numpunct<char> {
        char do_thousands_sep() const override { return ','; }
        std::string do_grouping() const override { return "\3"; }
    };
    // RAII restore so a CHECK/REQUIRE failure can't leak the grouping locale
    // into every later TEST_CASE in this binary.
    struct LocaleGuard {
        std::locale prev;
        explicit LocaleGuard(const std::locale& loc)
            : prev(std::locale::global(loc)) {}
        ~LocaleGuard() { std::locale::global(prev); }
    } guard(std::locale(std::locale::classic(), new Grouping));

    // Confirm the guard actually installed a grouping locale (else the test
    // would pass vacuously).
    {
        std::ostringstream probe;
        probe << 1234567;
        REQUIRE(probe.str() == "1,234,567");
    }

    // Large display_time + thread_id so grouping would be visible if it leaked.
    const auto merged = ComputeMerge(
        {Row(1234567, 89012, 10'000'000, 10'001'100)}, 10'000'000,
        {Row(1234567, 89012, 10'000'400, 10'001'000)}, 10'000'000);
    REQUIRE(merged.size() == 1);

    std::ostringstream out;  // inherits the grouping locale at construction
    WriteMergedCsv(out, merged, ComputeStats(merged));
    const std::string s = out.str();

    // The data row must carry raw integers -- exact prefix, no separators.
    CHECK(s.find("1234567,89012,") != std::string::npos);
    CHECK(s.find("1,234,567") == std::string::npos);
    CHECK(s.find("89,012") == std::string::npos);
}
