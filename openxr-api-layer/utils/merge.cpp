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

#include "merge.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace openxr_api_layer::merge {

    namespace {

        // Trim trailing CR/space/tab so a CRLF-terminated header line still
        // compares equal to kExpectedColumnHeader. We deliberately do NOT
        // touch leading whitespace -- the spec'd column header has none.
        std::string_view RTrim(std::string_view s) {
            while (!s.empty() &&
                   (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) {
                s.remove_suffix(1);
            }
            return s;
        }

        // Strict integer parse: succeeds only if the ENTIRE string is one
        // base-10 integer (after RTrim). std::stoll would silently accept
        // "1e7" -> 1 and "10000000 Hz" -> 10000000, both of which Python's
        // int() rejects. Aligning the two paths.
        std::optional<int64_t> StrictStoll(std::string_view s) {
            s = RTrim(s);
            // Reject leading whitespace too; Python's int() does the same.
            if (s.empty() || s.front() == ' ' || s.front() == '\t') {
                return std::nullopt;
            }
            int64_t out = 0;
            const auto* begin = s.data();
            const auto* end = s.data() + s.size();
            const auto [ptr, ec] = std::from_chars(begin, end, out);
            if (ec != std::errc{} || ptr != end) {
                return std::nullopt;
            }
            return out;
        }

    } // namespace

    ParsedFrameCsv ReadFrameCsv(const std::filesystem::path& path,
                                int64_t defaultFreq) {
        ParsedFrameCsv result;
        result.qpc_freq = defaultFreq;
        result.header_valid = true;  // optimistic; flipped below if violated
        std::ifstream s(path);
        if (!s) {
            return result;
        }
        std::string line;
        bool header_consumed = false;
        while (std::getline(s, line)) {
            if (line.empty()) {
                continue;
            }
            if (line[0] == '#') {
                static const std::string kFreqKey = "qpc_freq=";
                if (const auto p = line.find(kFreqKey); p != std::string::npos) {
                    if (const auto parsed = StrictStoll(
                            std::string_view(line).substr(p + kFreqKey.size()))) {
                        result.qpc_freq = *parsed;
                    }
                    // else: keep defaultFreq -- garbage qpc_freq value (e.g.
                    // "1e7", "10 MHz", or trailing junk) does not silently
                    // become a wrong base-10 truncation.
                }
                continue;
            }
            if (!header_consumed) {
                // The first non-comment, non-empty line MUST be the spec'd
                // column header. Anything else (most likely: a merged CSV
                // fed in by mistake, whose first non-comment line lists
                // seven different columns) is rejected so the caller can
                // emit a clear error instead of "skipping merge: 0 rows".
                if (RTrim(line) != kExpectedColumnHeader) {
                    result.header_valid = false;
                    return result;
                }
                header_consumed = true;
                continue;
            }
            RawFrameRow r{};
            char c;
            std::stringstream ss(line);
            if (ss >> r.frame_idx >> c >> r.thread_id >> c >> r.qpc_entry >> c >>
                r.qpc_exit) {
                result.rows.push_back(r);
            }
        }
        return result;
    }

    // preFreq is authoritative for both sides; postFreq is intentionally
    // ignored (matches analyze.py's pre.qpc_freq) -- kept in the signature
    // so callers continue to ferry per-side metadata through, and so a
    // future cross-machine bridging feature has a place to land.
    std::vector<MergedRow> ComputeMerge(
        const std::vector<RawFrameRow>& preRows, int64_t preFreq,
        const std::vector<RawFrameRow>& postRows, int64_t /*postFreq*/) {
        using Key = std::pair<uint64_t, uint32_t>;
        const auto mk = [](uint64_t fi, uint32_t tid) -> Key { return {fi, tid}; };

        // Index post by (frame_idx, thread_id). We will erase entries as
        // they are matched so a duplicate pre key cannot re-match the same
        // post entry twice -- matches analyze.py's post_index.pop().
        std::map<Key, RawFrameRow> postIndex;
        for (const auto& r : postRows) {
            postIndex[mk(r.frame_idx, r.thread_id)] = r;
        }

        // Compute next pre.qpc_entry per thread for frame_interval_us.
        std::map<uint32_t, std::vector<std::pair<uint64_t, int64_t>>> preByThread;
        for (const auto& r : preRows) {
            preByThread[r.thread_id].emplace_back(r.frame_idx, r.qpc_entry);
        }
        for (auto& [tid, items] : preByThread) {
            std::sort(items.begin(), items.end());
        }
        std::map<Key, int64_t> nextEntry;
        for (const auto& [tid, items] : preByThread) {
            for (size_t i = 0; i + 1 < items.size(); ++i) {
                nextEntry[mk(items[i].first, tid)] = items[i + 1].second;
            }
        }

        const int64_t freq = preFreq;  // authoritative for both sides
        std::vector<MergedRow> merged;
        merged.reserve(preRows.size());
        for (const auto& pre : preRows) {
            const auto it = postIndex.find(mk(pre.frame_idx, pre.thread_id));
            if (it == postIndex.end()) {
                continue;
            }
            const RawFrameRow post = it->second;
            postIndex.erase(it);  // consume on match -- prevents double-counting

            MergedRow m{};
            m.frame_idx = pre.frame_idx;
            m.thread_id = pre.thread_id;
            m.pre_us =
                static_cast<double>(pre.qpc_exit - pre.qpc_entry) / freq * 1e6;
            m.post_us =
                static_cast<double>(post.qpc_exit - post.qpc_entry) / freq * 1e6;
            m.target_us = m.pre_us - m.post_us;
            if (const auto ne = nextEntry.find(mk(pre.frame_idx, pre.thread_id));
                ne != nextEntry.end()) {
                const double fiv =
                    static_cast<double>(ne->second - pre.qpc_entry) / freq * 1e6;
                // Only populate the interval / pct fields when fiv is a
                // sane positive duration. A non-invariant TSC across a
                // core migration can produce qpc_entry[i+1] < qpc_entry[i];
                // emitting that as a negative "interval" downstream would
                // mislead any tool that divides by it.
                if (fiv > 0.0) {
                    m.frame_interval_us = fiv;
                    m.target_pct_of_frame = m.target_us / fiv * 100.0;
                }
            }
            merged.push_back(m);
        }
        // stable_sort: defends against duplicate (thread, frame_idx) keys
        // sneaking through if a future refactor weakens the consume-on-match
        // contract above.
        std::stable_sort(merged.begin(), merged.end(),
                         [](const MergedRow& a, const MergedRow& b) {
                             if (a.thread_id != b.thread_id) {
                                 return a.thread_id < b.thread_id;
                             }
                             return a.frame_idx < b.frame_idx;
                         });
        return merged;
    }

    namespace {

        // Kahan-Neumaier compensated summation. Naive `s += v` loses ~ulp
        // per term and on long sessions with mixed-sign values near zero
        // (QPC noise floor + a target layer that genuinely runs free) the
        // accumulated error can flip the .4f-rounded mean we print in the
        // CSV header. Python's statistics.fmean uses a similar compensated
        // strategy, so matching here keeps the byte-equivalence contract
        // on long real-world sessions, not just on short fixtures.
        double KahanNeumaierSum(const std::vector<double>& v) {
            double sum = 0.0;
            double c = 0.0;  // running compensation for lost low-order bits
            for (double x : v) {
                const double t = sum + x;
                if (std::abs(sum) >= std::abs(x)) {
                    c += (sum - t) + x;
                } else {
                    c += (x - t) + sum;
                }
                sum = t;
            }
            return sum + c;
        }

    } // namespace

    MergeStats ComputeStats(const std::vector<MergedRow>& merged) {
        MergeStats stats{};
        stats.frame_count = merged.size();
        if (merged.empty()) {
            return stats;
        }
        std::vector<double> ms_values;
        std::vector<double> pct_values;
        ms_values.reserve(merged.size());
        pct_values.reserve(merged.size());
        for (const auto& m : merged) {
            const double ms = m.target_us / 1000.0;
            ms_values.push_back(ms);
            if (ms < 0.0) {
                ++stats.negative_target_count;
            }
            if (m.target_pct_of_frame.has_value()) {
                pct_values.push_back(*m.target_pct_of_frame);
            }
        }
        // ms_values always non-empty because merged is non-empty.
        stats.target_ms_mean = KahanNeumaierSum(ms_values) / ms_values.size();
        stats.target_ms_min = *std::min_element(ms_values.begin(), ms_values.end());
        stats.target_ms_max = *std::max_element(ms_values.begin(), ms_values.end());
        if (!pct_values.empty()) {
            stats.target_pct_mean =
                KahanNeumaierSum(pct_values) / pct_values.size();
            stats.target_pct_min =
                *std::min_element(pct_values.begin(), pct_values.end());
            stats.target_pct_max =
                *std::max_element(pct_values.begin(), pct_values.end());
        }
        return stats;
    }

    void WriteMergedCsv(std::ostream& out,
                        const std::vector<MergedRow>& merged,
                        const MergeStats& stats) {
        out << "# frame_count=" << stats.frame_count << '\n'
            << "# target_ms_mean=" << fmt::format("{:.4f}", stats.target_ms_mean) << '\n'
            << "# target_ms_min="  << fmt::format("{:.4f}", stats.target_ms_min)  << '\n'
            << "# target_ms_max="  << fmt::format("{:.4f}", stats.target_ms_max)  << '\n'
            << "# target_pct_mean=" << fmt::format("{:.4f}", stats.target_pct_mean) << "%\n"
            << "# target_pct_min="  << fmt::format("{:.4f}", stats.target_pct_min)  << "%\n"
            << "# target_pct_max="  << fmt::format("{:.4f}", stats.target_pct_max)  << "%\n"
            << "frame_idx,thread_id,frame_interval_us,pre_us,post_us,target_us,"
               "target_pct_of_frame\n";
        for (const auto& m : merged) {
            out << m.frame_idx << ',' << m.thread_id << ',';
            if (m.frame_interval_us.has_value()) {
                out << fmt::format("{:.3f}", *m.frame_interval_us);
            }
            out << ',' << fmt::format("{:.3f}", m.pre_us) << ','
                << fmt::format("{:.3f}", m.post_us) << ','
                << fmt::format("{:.3f}", m.target_us) << ',';
            if (m.target_pct_of_frame.has_value()) {
                out << fmt::format("{:.4f}", *m.target_pct_of_frame);
            }
            out << '\n';
        }
    }

} // namespace openxr_api_layer::merge
