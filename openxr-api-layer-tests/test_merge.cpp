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
//   * Deterministic sort order in ComputeMerge: (thread_id, frame_idx).
//   * Exact byte format produced by WriteMergedCsv -- this is the contract
//     between the in-DLL merge and analyze.py.
// =============================================================================

#include <doctest/doctest.h>

#include "utils/merge.h"

#include <chrono>
#include <filesystem>
#include <fstream>
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
    //   frame_idx,thread_id,qpc_entry,qpc_exit
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
            << "frame_idx,thread_id,qpc_entry,qpc_exit\n";
        for (const auto& r : rows) {
            out << r.frame_idx << ',' << r.thread_id << ','
                << r.qpc_entry << ',' << r.qpc_exit << '\n';
        }
        return path;
    }

    // Convenience: build a RawFrameRow from positional args.
    RawFrameRow Row(uint64_t fi, uint32_t tid, int64_t qe, int64_t qx) {
        return RawFrameRow{fi, tid, qe, qx};
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
    CHECK(parsed.rows[0].frame_idx == 0);
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
                << "frame_idx,thread_id,qpc_entry,qpc_exit\n"
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
            << "frame_idx,thread_id,qpc_entry,qpc_exit\n"
            << "0,1,10,20\n"
            << "\n"
            << "1,1,30,40\n";
    }
    const auto parsed = ReadFrameCsv(path, /*defaultFreq=*/0);
    CHECK(parsed.qpc_freq == 1000);
    REQUIRE(parsed.rows.size() == 2);
    CHECK(parsed.rows[0].frame_idx == 0);
    CHECK(parsed.rows[1].frame_idx == 1);
}

TEST_CASE("ReadFrameCsv: header_valid is false when the column header does not "
          "match the spec (e.g. merged CSV fed in by mistake)") {
    // Simulate a frames-merged-<pid>.csv fed in where the per-side CSV
    // belongs. Its first non-comment line lists seven columns, not four.
    const fs::path path = TestTempDir() / "rfc_merged_by_mistake.csv";
    {
        std::ofstream out(path);
        out << "# frame_count=42\n"
            << "# target_ms_mean=0.0123\n"
            << "frame_idx,thread_id,frame_interval_us,pre_us,post_us,"
               "target_us,target_pct_of_frame\n"
            << "0,1,11100.000,1.234,0.123,1.111,0.0100\n";
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
            << "frame_idx,thread_id,qpc_entry,qpc_exit\r\n"
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
            << "frame_idx,thread_id,qpc_entry,qpc_exit\n"
            << "0,1,10,20\n"
            << "garbage_with,too_few_fields\n"
            << "2,1,50,60\n";
    }
    const auto parsed = ReadFrameCsv(path, 0);
    REQUIRE(parsed.rows.size() == 2);
    CHECK(parsed.rows[0].frame_idx == 0);
    CHECK(parsed.rows[1].frame_idx == 2);
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
    CHECK(m[0].frame_idx == 0);
    CHECK(m[0].thread_id == 1);
    CHECK(m[0].pre_us == doctest::Approx(1000.0));
    CHECK(m[0].post_us == doctest::Approx(800.0));
    CHECK(m[0].target_us == doctest::Approx(200.0));
    CHECK_FALSE(m[0].frame_interval_us.has_value());
    CHECK_FALSE(m[0].target_pct_of_frame.has_value());
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
    CHECK(m[0].frame_idx == 0);
    CHECK(m[0].pre_us == doctest::Approx(100.0));
    CHECK(m[0].post_us == doctest::Approx(50.0));
    CHECK(m[0].target_us == doctest::Approx(50.0));
    REQUIRE(m[0].frame_interval_us.has_value());
    CHECK(*m[0].frame_interval_us == doctest::Approx(111'000.0));
    REQUIRE(m[0].target_pct_of_frame.has_value());
    CHECK(*m[0].target_pct_of_frame ==
          doctest::Approx(50.0 / 111'000.0 * 100.0));

    // Frame 1: no successor.
    CHECK(m[1].frame_idx == 1);
    CHECK(m[1].target_us == doctest::Approx(40.0));
    CHECK_FALSE(m[1].frame_interval_us.has_value());
    CHECK_FALSE(m[1].target_pct_of_frame.has_value());
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
    // frame_idx 0 and 2 match. frame_idx 1 is pre-only, 99 is post-only.
    REQUIRE(m.size() == 2);
    CHECK(m[0].frame_idx == 0);
    CHECK(m[1].frame_idx == 2);
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
    CHECK(m[0].frame_idx == 0);
    CHECK(m[0].frame_interval_us.has_value());  // has successor in thread 1

    CHECK(m[1].thread_id == 1);
    CHECK(m[1].frame_idx == 1);
    CHECK_FALSE(m[1].frame_interval_us.has_value());  // last in thread 1

    CHECK(m[2].thread_id == 2);
    CHECK(m[2].frame_idx == 0);
    CHECK_FALSE(m[2].frame_interval_us.has_value());  // only one in thread 2
}

TEST_CASE("ComputeMerge: output sorted by (thread_id, frame_idx) regardless of input order") {
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
    CHECK(m[0].frame_idx == 0);
    CHECK(m[1].thread_id == 1);
    CHECK(m[1].frame_idx == 3);
    CHECK(m[2].thread_id == 2);
    CHECK(m[2].frame_idx == 0);
    CHECK(m[3].thread_id == 2);
    CHECK(m[3].frame_idx == 5);
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
    // Two pre rows share the same (frame_idx, thread_id) -- defensive
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
    CHECK_FALSE(m[0].target_pct_of_frame.has_value());
    // Second row (frame 1) is last per thread -> also blank, as before.
    CHECK_FALSE(m[1].frame_interval_us.has_value());
}

// ============================================================================
// ComputeStats
// ============================================================================

TEST_CASE("ComputeStats: empty merged gives all zeros") {
    const auto stats = ComputeStats({});
    CHECK(stats.frame_count == 0);
    CHECK(stats.target_ms_mean == 0.0);
    CHECK(stats.target_ms_min == 0.0);
    CHECK(stats.target_ms_max == 0.0);
    CHECK(stats.target_pct_mean == 0.0);
    CHECK(stats.negative_target_count == 0);
}

TEST_CASE("ComputeStats: ms aggregates over every row, pct only over rows with pct") {
    // Build merged rows directly (skip ComputeMerge). Two threads, three
    // frames total. Two have target_pct_of_frame, one (last per thread)
    // doesn't.
    std::vector<MergedRow> merged(3);
    merged[0].frame_idx = 0;
    merged[0].thread_id = 1;
    merged[0].pre_us = 100;
    merged[0].post_us = 60;
    merged[0].target_us = 40;
    merged[0].frame_interval_us = 11000.0;
    merged[0].target_pct_of_frame = 40.0 / 11000.0 * 100.0;

    merged[1].frame_idx = 1;
    merged[1].thread_id = 1;
    merged[1].pre_us = 80;
    merged[1].post_us = 70;
    merged[1].target_us = 10;
    // No successor on thread 1.

    merged[2].frame_idx = 0;
    merged[2].thread_id = 2;
    merged[2].pre_us = 200;
    merged[2].post_us = 50;
    merged[2].target_us = 150;
    // No successor (only frame on thread 2).

    const auto stats = ComputeStats(merged);
    CHECK(stats.frame_count == 3);
    // target_ms_*: 0.040, 0.010, 0.150 ms.
    CHECK(stats.target_ms_mean == doctest::Approx((0.040 + 0.010 + 0.150) / 3.0));
    CHECK(stats.target_ms_min == doctest::Approx(0.010));
    CHECK(stats.target_ms_max == doctest::Approx(0.150));
    // target_pct_*: only frame 0/thread 1 contributes.
    CHECK(stats.target_pct_mean ==
          doctest::Approx(40.0 / 11000.0 * 100.0));
    CHECK(stats.target_pct_min ==
          doctest::Approx(40.0 / 11000.0 * 100.0));
    CHECK(stats.target_pct_max ==
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
    CHECK(stats.target_ms_min == doctest::Approx(-0.050));
    CHECK(stats.target_ms_max == doctest::Approx(0.100));
}

// ============================================================================
// WriteMergedCsv
// ============================================================================

TEST_CASE("WriteMergedCsv: header has the seven # lines + column line") {
    std::vector<MergedRow> merged(1);
    merged[0].frame_idx = 0;
    merged[0].thread_id = 12345;
    merged[0].pre_us = 100.0;
    merged[0].post_us = 50.0;
    merged[0].target_us = 50.0;
    merged[0].frame_interval_us = 11100.0;
    merged[0].target_pct_of_frame = 50.0 / 11100.0 * 100.0;

    const auto stats = ComputeStats(merged);
    std::ostringstream out;
    WriteMergedCsv(out, merged, stats);

    const std::string s = out.str();
    // Seven # lines + 1 column header + 1 data row + trailing \n.
    INFO("Full output:\n" << s);
    CHECK(s.find("# frame_count=1\n") != std::string::npos);
    CHECK(s.find("# target_ms_mean=0.0500\n") != std::string::npos);
    CHECK(s.find("# target_ms_min=0.0500\n") != std::string::npos);
    CHECK(s.find("# target_ms_max=0.0500\n") != std::string::npos);
    // pct lines carry the '%' suffix (per the user request).
    CHECK(s.find("# target_pct_mean=0.4505%\n") != std::string::npos);
    CHECK(s.find("# target_pct_min=0.4505%\n") != std::string::npos);
    CHECK(s.find("# target_pct_max=0.4505%\n") != std::string::npos);
    CHECK(s.find("frame_idx,thread_id,frame_interval_us,pre_us,post_us,"
                 "target_us,target_pct_of_frame\n") != std::string::npos);
}

TEST_CASE("WriteMergedCsv: data row format matches the .3f/.4f contract") {
    std::vector<MergedRow> merged(2);
    merged[0].frame_idx = 0;
    merged[0].thread_id = 42;
    merged[0].pre_us = 1.234567;
    merged[0].post_us = 0.123456;
    merged[0].target_us = 1.111111;
    merged[0].frame_interval_us = 11111.555;
    merged[0].target_pct_of_frame = 0.01000;

    // Last row per thread: optionals blank.
    merged[1].frame_idx = 1;
    merged[1].thread_id = 42;
    merged[1].pre_us = 2.000;
    merged[1].post_us = 1.000;
    merged[1].target_us = 1.000;
    // frame_interval_us / target_pct_of_frame stay nullopt.

    const auto stats = ComputeStats(merged);
    std::ostringstream out;
    WriteMergedCsv(out, merged, stats);

    const std::string s = out.str();
    // .3f on us values, .4f on pct.
    CHECK(s.find("0,42,11111.555,1.235,0.123,1.111,0.0100") !=
          std::string::npos);
    // Last row: empty frame_interval_us and empty target_pct_of_frame.
    CHECK(s.find("1,42,,2.000,1.000,1.000,") != std::string::npos);
}

TEST_CASE("WriteMergedCsv: output uses LF line endings on disk via a binary ofstream") {
    // Round-trip through a real std::ofstream opened in binary mode, then
    // re-read as raw bytes. This exercises the layer.cpp:std::ios::binary
    // contract -- an in-memory std::ostringstream check (used previously)
    // could not catch a regression where someone drops the binary flag,
    // because ostringstream is mode-agnostic.
    std::vector<MergedRow> merged(1);
    merged[0].frame_idx = 0;
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
    CHECK(stats.target_ms_mean == doctest::Approx(0.0550));
    CHECK(stats.target_ms_min == doctest::Approx(0.0400));
    CHECK(stats.target_ms_max == doctest::Approx(0.0800));

    std::ostringstream out;
    WriteMergedCsv(out, merged, stats);
    const std::string s = out.str();
    // Sanity-check the same hand-verified header the README documents.
    CHECK(s.find("# frame_count=4\n") != std::string::npos);
    CHECK(s.find("# target_ms_mean=0.0550\n") != std::string::npos);
}
