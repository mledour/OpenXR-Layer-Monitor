// MIT License
//
// Copyright (c) 2026 Michael Ledour
//
// Based on https://github.com/mbucchia/OpenXR-Layer-Template.
// Copyright (c) 2022-2023 Matthieu Bucchianeri
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
// XR_APILAYER_MLEDOUR_layer_monitor — sandwich CPU/GPU profiler for OpenXR layers.
//
// This single source file produces TWO DLLs from the same code: one named
// "..._pre" (loaded before the target layer) and one named "..._post" (loaded
// after). The OpenXR loader chains them as:
//
//     app -> pre -> target_layer -> post -> runtime
//
// Each side records QPC timestamps around its own call to the next layer.
//   - pre.bracket   = entry..exit  =  target_work + post_overhead + runtime
//   - post.bracket  = entry..exit  =  runtime
//   - target_cpu    = pre.bracket - post.bracket
//
// Both DLLs live in separate address spaces (different .dll files -> different
// singletons), so they cannot share in-process state directly. They each emit:
//   1. An ETW event per frame via TraceLoggingWrite (~50 ns, no I/O, no locks).
//   2. A row to %LOCALAPPDATA%\<base>\frames-<pid>-<side>.csv from a background
//      writer thread, so the frame thread only ever does a brief lock + push.
//      <base> is the shared folder (suffix _pre/_post stripped in entry.cpp).
//
// Offline (or via a small consumer) the two CSVs / two ETW streams are merged
// by (pid, thread_id, frame_idx) and target_cpu is computed per frame.
//
// IMPORTANT: never call Log() from xrEndFrame -- see framework/log.h's banner
// about the DBWinMutex trap that drops frames on some compositors. The
// per-frame instrumentation is ETW + a queue with disk I/O off the frame
// thread.
// =============================================================================

#include "pch.h"

#include "layer.h"
#include <log.h>
#include <util.h>

// pch.h pulls <unordered_map> but not <map>, which the auto-merge below uses
// for its ordered-by-pair lookups (post index, next-entry per thread). Adding
// the header here keeps the dependency local to the consumer.
#include <map>

namespace openxr_api_layer {

    using namespace log;

    // Pass-through w.r.t. extensions. The monitor neither advertises nor
    // blocks anything; it just measures.
    const std::vector<std::pair<std::string, uint32_t>> advertisedExtensions = {};
    const std::vector<std::string> blockedExtensions = {};
    const std::vector<std::string> implicitExtensions = {};

    namespace {

        // ---- Side detection ------------------------------------------------
        //
        // LAYER_NAME is defined at compile time to $(SolutionName), which is
        // either "XR_APILAYER_MLEDOUR_layer_monitor_pre" or "..._post"
        // depending on which .sln is being built. Both DLLs share this source
        // file; the suffix is the only thing that differs between the two
        // binaries.
        constexpr bool EndsWith(std::string_view s, std::string_view suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
        constexpr bool kIsPostSide = EndsWith(LAYER_NAME, "_post");
        constexpr const char* kSideStr = kIsPostSide ? "post" : "pre";

        // ---- QPC helpers ---------------------------------------------------

        int64_t QpcFreqHz() {
            static const int64_t freq = []() {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                return f.QuadPart;
            }();
            return freq;
        }

        inline int64_t Qpc() {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            return now.QuadPart;
        }

        // ---- Frame counter -------------------------------------------------
        //
        // Per-DLL, atomic. Pre and post each maintain their own counter; they
        // start at 0 simultaneously (first xrEndFrame after xrCreateInstance)
        // and increment in lockstep because the application is required by the
        // OpenXR spec to serialize xrEndFrame submission per session. Merging
        // the two CSVs by frame_idx therefore matches like-for-like, unless
        // one side fails to load -- in which case the analyzer detects the
        // mismatch via the per-side row count.
        std::atomic<uint64_t> g_frameCounter{0};

        // ---- Background CSV writer -----------------------------------------
        //
        // The frame thread does the cheapest possible thing: take a brief
        // mutex, push a tiny POD, signal the cv if the queue was empty. The
        // writer thread does the slow stuff (formatting, fwrite, periodic
        // flush) entirely off the critical path.
        //
        // Synchronous I/O in xrEndFrame would inflate pre.bracket by however
        // long the fwrite took -- and because pre.bracket subsumes post's own
        // fwrite, post's I/O cost would be attributed to the target layer.
        // The whole point of this layer is that target_cpu measurements are
        // trustworthy, so the background thread is non-optional.

        struct FrameRow {
            uint64_t frame_idx;
            uint32_t thread_id;
            int64_t qpc_entry;
            int64_t qpc_exit;
        };

        class FrameCsvSink {
          public:
            void Start() {
                bool expected = false;
                if (!m_running.compare_exchange_strong(expected, true)) {
                    return;
                }
                m_thread = std::thread([this] { Run(); });
            }

            void Stop() {
                bool expected = true;
                if (!m_running.compare_exchange_strong(expected, false)) {
                    return;
                }
                m_cv.notify_all();
                if (m_thread.joinable()) {
                    m_thread.join();
                }
            }

            void Append(const FrameRow& row) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_queue.push_back(row);
                }
                m_cv.notify_one();
            }

          private:
            void Run() {
                // entry.cpp resolves localAppData to the SHARED base folder
                // (suffix _pre / _post stripped), so we add the side back into
                // the CSV filename to keep pre and post outputs distinct.
                // PID also goes in the name so concurrent OpenXR processes
                // don't trample each other.
                const std::filesystem::path path =
                    localAppData / fmt::format("frames-{}-{}.csv",
                                               GetCurrentProcessId(), kSideStr);

                std::ofstream stream(path, std::ios::out | std::ios::trunc);
                if (!stream) {
                    ErrorLog(fmt::format("Failed to open {}\n", path.string()));
                    return;
                }
                stream << "# qpc_freq=" << QpcFreqHz() << "\n"
                       << "# side=" << kSideStr << "\n"
                       << "# layer=" << LayerName << "\n"
                       << "# fn=xrEndFrame\n"
                       << "frame_idx,thread_id,qpc_entry,qpc_exit\n";

                std::vector<FrameRow> batch;
                while (true) {
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_cv.wait(lock, [this] {
                            return !m_queue.empty() || !m_running.load();
                        });
                        batch.swap(m_queue);
                    }

                    for (const auto& r : batch) {
                        stream << r.frame_idx << ',' << r.thread_id << ','
                               << r.qpc_entry << ',' << r.qpc_exit << '\n';
                    }
                    batch.clear();
                    stream.flush();

                    if (!m_running.load(std::memory_order_acquire)) {
                        // Drain the trailing rows that may have arrived
                        // between our last swap and the running=false flip.
                        std::lock_guard<std::mutex> lock(m_mutex);
                        for (const auto& r : m_queue) {
                            stream << r.frame_idx << ',' << r.thread_id << ','
                                   << r.qpc_entry << ',' << r.qpc_exit << '\n';
                        }
                        m_queue.clear();
                        stream.flush();
                        return;
                    }
                }
            }

            std::atomic<bool> m_running{false};
            std::thread m_thread;
            std::mutex m_mutex;
            std::condition_variable m_cv;
            std::vector<FrameRow> m_queue;
        };

        FrameCsvSink g_csv;

        // ---- Auto-merge (post side only, on clean shutdown) ---------------
        //
        // After the post-side writer joins (end of g_csv.Stop()), both
        // frames-<pid>-pre.csv and frames-<pid>-post.csv are fully flushed
        // on disk because the OpenXR loader walks the destroy chain pre ->
        // target -> post and the framework runs each layer's ResetInstance()
        // BEFORE chaining downward (see framework/dispatch_generator.py's
        // xrDestroyInstance template). So by the time post's OpenXrLayer
        // destructor returns from Stop(), pre's destructor has already
        // returned and pre's CSV is closed.
        //
        // We replicate analyze.py's logic here so the user gets a ready-to-
        // open frames-merged-<pid>.csv next to the raw CSVs without having
        // to run any post-processing. The script remains useful for stats
        // and for sessions that didn't shut down cleanly (crash, kill -9):
        // in that case no merge runs and the user falls back to analyze.py.
        struct RawFrameRow {
            uint64_t frame_idx;
            uint32_t thread_id;
            int64_t qpc_entry;
            int64_t qpc_exit;
        };

        // Returns rows + qpc_freq parsed from the file's "# qpc_freq=" header.
        // qpc_freq defaults to the process's own QPC frequency if the header
        // is missing or unparseable.
        std::pair<std::vector<RawFrameRow>, int64_t> ReadFrameCsv(
            const std::filesystem::path& path) {
            std::vector<RawFrameRow> rows;
            int64_t freq = QpcFreqHz();
            std::ifstream s(path);
            if (!s) {
                return {rows, freq};
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
                        try {
                            freq = std::stoll(line.substr(p + kFreqKey.size()));
                        } catch (...) {
                            // keep default
                        }
                    }
                    continue;
                }
                if (!header_consumed) {
                    header_consumed = true;
                    continue;
                }
                RawFrameRow r{};
                char c;
                std::stringstream ss(line);
                if (ss >> r.frame_idx >> c >> r.thread_id >> c >> r.qpc_entry >> c >>
                    r.qpc_exit) {
                    rows.push_back(r);
                }
            }
            return {rows, freq};
        }

        void MergeIntoOutput() {
            namespace fs = std::filesystem;
            const auto pid = GetCurrentProcessId();
            const fs::path preCsv = localAppData / fmt::format("frames-{}-pre.csv", pid);
            const fs::path postCsv =
                localAppData / fmt::format("frames-{}-post.csv", pid);
            const fs::path outCsv =
                localAppData / fmt::format("frames-merged-{}.csv", pid);

            auto [preRows, preFreq] = ReadFrameCsv(preCsv);
            auto [postRows, postFreq] = ReadFrameCsv(postCsv);
            if (preRows.empty() || postRows.empty()) {
                Log(fmt::format("Skipping auto-merge: pre={} rows, post={} rows\n",
                                preRows.size(), postRows.size()));
                return;
            }

            using Key = std::pair<uint64_t, uint32_t>;
            const auto mk = [](uint64_t fi, uint32_t tid) -> Key { return {fi, tid}; };

            std::map<Key, RawFrameRow> postIndex;
            for (const auto& r : postRows) {
                postIndex[mk(r.frame_idx, r.thread_id)] = r;
            }

            // Frame interval comes from pre's qpc_entry deltas, per thread.
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

            struct MergedRow {
                uint64_t frame_idx;
                uint32_t thread_id;
                std::optional<double> frame_interval_us;
                double pre_us;
                double post_us;
                double target_us;
                std::optional<double> target_pct_of_frame;
            };
            std::vector<MergedRow> merged;
            merged.reserve(preRows.size());
            for (const auto& pre : preRows) {
                const auto it = postIndex.find(mk(pre.frame_idx, pre.thread_id));
                if (it == postIndex.end()) {
                    continue;
                }
                const auto& post = it->second;
                MergedRow m{};
                m.frame_idx = pre.frame_idx;
                m.thread_id = pre.thread_id;
                m.pre_us =
                    static_cast<double>(pre.qpc_exit - pre.qpc_entry) / preFreq * 1e6;
                m.post_us =
                    static_cast<double>(post.qpc_exit - post.qpc_entry) / postFreq * 1e6;
                m.target_us = m.pre_us - m.post_us;
                if (const auto ne = nextEntry.find(mk(pre.frame_idx, pre.thread_id));
                    ne != nextEntry.end()) {
                    const double fiv =
                        static_cast<double>(ne->second - pre.qpc_entry) / preFreq * 1e6;
                    m.frame_interval_us = fiv;
                    if (fiv > 0.0) {
                        m.target_pct_of_frame = m.target_us / fiv * 100.0;
                    }
                }
                merged.push_back(m);
            }
            // Deterministic output: thread, then frame_idx -- matches analyze.py.
            std::sort(merged.begin(), merged.end(),
                      [](const MergedRow& a, const MergedRow& b) {
                          if (a.thread_id != b.thread_id) {
                              return a.thread_id < b.thread_id;
                          }
                          return a.frame_idx < b.frame_idx;
                      });

            // Session-wide summary written as comment headers at the top of
            // the merged CSV. Three numbers the user usually wants without
            // having to open a spreadsheet: total frames matched, mean CPU
            // ms attributed to the target, mean % of frame budget eaten.
            // Frames with no successor (last frame per thread) contribute
            // to frame_count and target_ms_mean but NOT to target_pct_mean
            // (their target_pct_of_frame is undefined).
            double target_ms_sum = 0.0;
            double target_pct_sum = 0.0;
            size_t pct_count = 0;
            for (const auto& m : merged) {
                target_ms_sum += m.target_us / 1000.0;
                if (m.target_pct_of_frame) {
                    target_pct_sum += *m.target_pct_of_frame;
                    ++pct_count;
                }
            }
            const double target_ms_mean =
                merged.empty() ? 0.0 : target_ms_sum / merged.size();
            const double target_pct_mean =
                pct_count == 0 ? 0.0 : target_pct_sum / pct_count;

            std::ofstream out(outCsv, std::ios::out | std::ios::trunc);
            if (!out) {
                ErrorLog(fmt::format("Failed to open merged output {}\n",
                                     outCsv.string()));
                return;
            }
            out << "# frame_count=" << merged.size() << '\n'
                << "# target_ms_mean=" << fmt::format("{:.4f}", target_ms_mean) << '\n'
                << "# target_pct_mean=" << fmt::format("{:.4f}", target_pct_mean) << '\n';
            out << "frame_idx,thread_id,frame_interval_us,pre_us,post_us,target_us,"
                   "target_pct_of_frame\n";
            for (const auto& m : merged) {
                out << m.frame_idx << ',' << m.thread_id << ',';
                if (m.frame_interval_us) {
                    out << fmt::format("{:.3f}", *m.frame_interval_us);
                }
                out << ',' << fmt::format("{:.3f}", m.pre_us) << ','
                    << fmt::format("{:.3f}", m.post_us) << ','
                    << fmt::format("{:.3f}", m.target_us) << ',';
                if (m.target_pct_of_frame) {
                    out << fmt::format("{:.4f}", *m.target_pct_of_frame);
                }
                out << '\n';
            }
            Log(fmt::format("Wrote merged CSV: {} ({} frames)\n", outCsv.string(),
                            merged.size()));
        }

    } // namespace

    class OpenXrLayer : public OpenXrApi {
      public:
        OpenXrLayer() = default;

        // ResetInstance() (called from xrDestroyInstance) destroys this
        // OpenXrLayer while the process is still alive. We tear down the
        // writer here so the trailing rows make it to disk before the
        // DLL unloads, then -- on the post side only -- merge the pre and
        // post CSVs into frames-merged-<pid>.csv so the user doesn't have
        // to run analyze.py for the common case.
        ~OpenXrLayer() override {
            g_csv.Stop();
            if constexpr (kIsPostSide) {
                MergeIntoOutput();
            }
        }

        XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo) override {
            if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateInstance",
                              TLArg(kSideStr, "Side"));

            const std::string appName = createInfo->applicationInfo.applicationName;
            Log(fmt::format(
                "{} {} starting on side='{}' for application '{}'\n",
                LayerName, VersionString, kSideStr, appName));

            const XrResult result = OpenXrApi::xrCreateInstance(createInfo);
            if (XR_FAILED(result)) {
                return result;
            }

            XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
            if (XR_SUCCEEDED(OpenXrApi::xrGetInstanceProperties(GetXrInstance(), &props))) {
                Log(fmt::format("Runtime: {} {}.{}.{}\n",
                                props.runtimeName,
                                XR_VERSION_MAJOR(props.runtimeVersion),
                                XR_VERSION_MINOR(props.runtimeVersion),
                                XR_VERSION_PATCH(props.runtimeVersion)));
            }

            Log(fmt::format("QPC frequency: {} Hz\n", QpcFreqHz()));
            Log(fmt::format(
                "Writing frames to: {}\n",
                (localAppData / fmt::format("frames-{}-{}.csv",
                                            GetCurrentProcessId(), kSideStr))
                    .string()));

            g_csv.Start();
            return result;
        }

        XrResult xrEndFrame(XrSession session,
                            const XrFrameEndInfo* frameEndInfo) override {
            // The sandwich bracket. Keep this region free of work --
            // no Log(), no string formatting, no allocations -- so the only
            // cost the bracket sees is the OpenXrApi::xrEndFrame dispatch
            // itself plus whatever sits downstream. Anything that runs here
            // would be attributed to the target layer in the analyzer.
            const int64_t qpc_entry = Qpc();
            const XrResult result = OpenXrApi::xrEndFrame(session, frameEndInfo);
            const int64_t qpc_exit = Qpc();

            const uint64_t frame_idx =
                g_frameCounter.fetch_add(1, std::memory_order_relaxed);
            const uint32_t thread_id = GetCurrentThreadId();

            TraceLoggingWrite(g_traceProvider,
                              "xrEndFrame",
                              TLArg(kSideStr, "Side"),
                              TLArg(frame_idx, "FrameIdx"),
                              TLArg(qpc_entry, "QpcEntry"),
                              TLArg(qpc_exit, "QpcExit"),
                              TLArg(qpc_exit - qpc_entry, "QpcDelta"));

            g_csv.Append({frame_idx, thread_id, qpc_entry, qpc_exit});
            return result;
        }
    };

    // See framework/dispatch.gen.cpp -- g_instance is reset on xrDestroyInstance
    // so OpenXrLayer's destructor runs while the process is still alive.
    // DO NOT make this static-init -- the probe-then-real init pattern used
    // by some loaders leaves a stale writer thread from the probe instance
    // when the real instance starts; the reset-on-destroy contract avoids it.
    OpenXrApi* GetInstance() {
        if (!g_instance) {
            g_instance = std::make_unique<OpenXrLayer>();
        }
        return g_instance.get();
    }

} // namespace openxr_api_layer
