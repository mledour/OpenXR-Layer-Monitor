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

// Pure merge logic lives in utils/merge.* so it can be unit-tested without
// dragging in the OpenXR layer chain or the writer thread.
#include "utils/merge.h"

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
        // binaries. The static_asserts below catch a future rename of the
        // .sln (or a typo) at compile time, so the missing post-side merge
        // can never disappear silently from a debug build that someone runs.
        constexpr bool EndsWith(std::string_view s, std::string_view suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
        constexpr bool kIsPostSide = EndsWith(LAYER_NAME, "_post");
        constexpr bool kIsPreSide = EndsWith(LAYER_NAME, "_pre");
        static_assert(kIsPreSide || kIsPostSide,
                      "LAYER_NAME must end with _pre or _post -- check the "
                      ".sln name. CLAUDE.md notes that a layer rename is just "
                      "a .sln rename, but for THIS layer the suffix drives the "
                      "side-specific behaviour (toggle owner, merge trigger).");
        static_assert(kIsPreSide != kIsPostSide,
                      "LAYER_NAME ends with both suffixes somehow");
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

        // ---- Per-session state --------------------------------------------
        //
        // g_frameCounter resets to 0 on each Ctrl+F9 toggle ON so every
        // recording session starts at frame 0 (matches analyze.py's view).
        //
        // g_skipNext drops the FIRST frame after a toggle ON: that frame's
        // pre.bracket subsumes post's std::thread() spawn (~50-500 us under
        // Windows) which would otherwise contaminate target_us on frame 0
        // of every session.
        //
        // g_writerEverStarted gates the "remove stale frames-merged" path
        // in MergeIntoOutput: if this process never tried to record, we
        // leave any pre-existing merged CSV alone -- it might belong to an
        // earlier process Windows recycled our PID from.
        std::atomic<uint64_t> g_frameCounter{0};
        std::atomic<uint32_t> g_skipNext{0};
        std::atomic<bool> g_writerEverStarted{false};
        std::atomic<bool> g_monitoring{false};

        // ---- Cross-DLL toggle sync ----------------------------------------
        //
        // Naive per-DLL polling does NOT work for the sandwich: pre and
        // post run inside the same host xrEndFrame call, but the gap
        // between their polls is the time the TARGET LAYER spends in its
        // xrEndFrame -- exactly what we are here to measure (often
        // milliseconds for compositors or reprojection layers). A brief
        // Ctrl+F9 tap that is released between pre's poll and post's poll
        // would leave the two halves in different monitoring states for
        // the rest of the session, silently corrupting the merge.
        //
        // Fix: pre is the authoritative poller; the toggle decision is
        // broadcast to post through a tiny named shared-memory segment
        // (process-local, PID-keyed). Post reads the segment on every
        // xrEndFrame and follows. Generation counter makes "changed since
        // last seen" trivially detectable.
        //
        // Lock-freeness of std::atomic on x64 is guaranteed for these
        // types, so no further mutex is needed for the cross-DLL access.
        struct alignas(64) SharedToggleState {
            std::atomic<uint64_t> generation;
            std::atomic<bool> monitoring;
        };
        static_assert(std::atomic<uint64_t>::is_always_lock_free,
                      "uint64_t atomic must be lock-free for shared memory");
        static_assert(std::atomic<bool>::is_always_lock_free,
                      "bool atomic must be lock-free for shared memory");

        SharedToggleState* GetSharedToggleState() {
            static std::atomic<SharedToggleState*> cached{nullptr};
            static std::atomic<bool> failed{false};
            if (auto* p = cached.load(std::memory_order_acquire)) {
                return p;
            }
            if (failed.load(std::memory_order_acquire)) {
                return nullptr;
            }
            static std::mutex initMutex;
            std::lock_guard<std::mutex> lock(initMutex);
            if (auto* p = cached.load(std::memory_order_acquire)) {
                return p;
            }
            if (failed.load(std::memory_order_acquire)) {
                return nullptr;
            }

            // Local\ prefix = session-scoped namespace (per Windows session,
            // not cross-session). PID suffix ensures concurrent OpenXR
            // processes on the same machine do not collide.
            const std::string name = fmt::format(
                "Local\\XR_APILAYER_MLEDOUR_layer_monitor_shared_{}",
                GetCurrentProcessId());

            HANDLE handle = CreateFileMappingA(
                INVALID_HANDLE_VALUE,  // page-file backed (anonymous)
                nullptr,
                PAGE_READWRITE,
                0,
                sizeof(SharedToggleState),
                name.c_str());
            if (!handle) {
                ErrorLog(fmt::format("CreateFileMapping failed: {}\n",
                                     GetLastError()));
                failed.store(true, std::memory_order_release);
                return nullptr;
            }
            void* view = MapViewOfFile(handle,
                                       FILE_MAP_READ | FILE_MAP_WRITE,
                                       0, 0, sizeof(SharedToggleState));
            if (!view) {
                ErrorLog(fmt::format("MapViewOfFile failed: {}\n",
                                     GetLastError()));
                CloseHandle(handle);
                failed.store(true, std::memory_order_release);
                return nullptr;
            }
            // CreateFileMapping with INVALID_HANDLE_VALUE zero-initialises
            // the backing page on first creation, so {generation=0,
            // monitoring=false} is the default we want.
            auto* state = static_cast<SharedToggleState*>(view);
            cached.store(state, std::memory_order_release);
            // Intentional leak of `handle` here: it stays open for the DLL's
            // lifetime so the kernel object survives. The OS reclaims both
            // the handle and the mapping at process exit.
            return state;
        }

        // Per-DLL view of the last shared generation we acted on. Post uses
        // this to detect "pre toggled, follow now" without acting on the
        // same edge twice. Pre also keeps it consistent so a future
        // refactor that makes either side observe-only works as-is.
        std::atomic<uint64_t> g_lastSeenGeneration{0};

        std::atomic<bool> g_lastComboDown{false};

        // Debounce: minimum QPC ticks between two ApplyToggle invocations.
        // Stored in QPC units rather than ms so the comparison is one
        // subtraction, no division per poll. 0 = no toggle has happened
        // yet. Updated only when a toggle actually fires.
        std::atomic<int64_t> g_lastToggleQpc{0};

        // SampleHotkeyDown returns the CURRENT physical state of the
        // Ctrl+F9 combo, masking AltGr's synthetic LCtrl.
        //
        // On AZERTY / QWERTZ / Nordic / many other layouts, AltGr is
        // generated by Windows as `LCtrl down` + `RAlt down`. The LCtrl
        // is synthetic (purely for backward compat with apps that read
        // Ctrl modifiers) and the user has not actually pressed Ctrl.
        // Without the mask, every AltGr+F9 (a real shortcut: F9 = run /
        // step debugger in VS Code, Visual Studio, IntelliJ, most DAWs)
        // would fire ConsumeHotkeyEdge. Mask: if VK_RMENU is held, treat
        // any Ctrl signal as AltGr-synthetic and ignore it.
        //
        // Edge case: a user who genuinely holds Ctrl + RAlt + F9 cannot
        // trigger the toggle. Acceptable -- this combo has no standard
        // meaning anywhere we care about.
        bool SampleHotkeyDown() {
            const bool ralt = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
            const bool ctrl =
                (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && !ralt;
            const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            return ctrl && f9;
        }

        // ConsumeHotkeyEdge reflects the fact that the exchange mutates
        // g_lastComboDown, so calling it twice in a row collapses the
        // second call to "no edge". The semantic is "ask once per frame,
        // get the rising edge if any".
        //
        // PREVIOUSLY this was also gated by GetForegroundWindow() ==
        // GetCurrentProcessId(), which broke every VR runtime that puts
        // its own compositor / device-direct window in the foreground
        // while the HMD is mounted (Pimax OpenXR, SteamVR direct mode,
        // headless samples like hello_xr). The check is gone; tradeoffs:
        //
        //   - Ctrl+F9 is system-wide. Recovery from an accidental
        //     toggle is one keypress. DISABLE_XR_APILAYER_..._pre=1 /
        //     ..._post=1 stay as the permanent-conflict escape hatch.
        //   - AltGr's synthetic LCtrl is masked in SampleHotkeyDown so
        //     AZERTY users can press AltGr+F9 (the debugger run key)
        //     without toggling the recording.
        //   - The caller debounces successful toggles to >= 500 ms so a
        //     remapper / OBS / ShadowPlay that fires Ctrl+F9 twice in
        //     quick succession cannot churn the recording (the merge
        //     stall is 10-50 ms; a stutter on every double-tap is
        //     unacceptable).
        //
        // The atomic exchange is explicitly memory_order_acq_rel so the
        // ordering convention used elsewhere in this file (release stores,
        // acquire loads) is unbroken and a future maintainer cannot
        // accidentally "normalize" it to relaxed.
        bool ConsumeHotkeyEdge() {
            const bool down = SampleHotkeyDown();
            const bool prev =
                g_lastComboDown.exchange(down, std::memory_order_acq_rel);
            return down && !prev;
        }

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
            // Throws std::system_error if std::thread construction fails
            // (e.g., thread resource exhaustion), or std::runtime_error if
            // we are wedged from a previous failed Stop() join. Callers
            // (ApplyToggle) wrap the call so the exception never reaches
            // xrEndFrame.
            //
            // On thread-ctor failure the m_running flag is rolled back so a
            // subsequent Start() can retry; without the rollback we would
            // have a m_running=true zombie state with no live thread, and
            // every Append() would silently accumulate forever.
            void Start() {
                if (m_wedged.load(std::memory_order_acquire)) {
                    // A previous Stop() had to detach because join() threw.
                    // The orphaned writer thread may still hold the CSV
                    // file open; spawning a new one would race on the
                    // shared queue / mutex / CSV file. Refuse instead --
                    // recovery requires a process restart, which is no
                    // worse than the original join failure.
                    throw std::runtime_error(
                        "FrameCsvSink wedged after a prior detach; "
                        "restart the host process to recover");
                }
                bool expected = false;
                if (!m_running.compare_exchange_strong(expected, true)) {
                    return;
                }
                try {
                    m_thread = std::thread([this] { Run(); });
                } catch (...) {
                    m_running.store(false, std::memory_order_release);
                    throw;
                }
            }

            // join() can throw std::system_error in pathological cases
            // (deadlock detection, invalid_argument). We swallow it,
            // detach as a last resort, and flip m_wedged so any future
            // Start() refuses to spawn a second writer that would race
            // the orphaned one on the queue + CSV file (finding N3).
            void Stop() noexcept {
                bool expected = true;
                if (!m_running.compare_exchange_strong(expected, false)) {
                    return;
                }
                m_cv.notify_all();
                if (m_thread.joinable()) {
                    try {
                        m_thread.join();
                    } catch (const std::exception& e) {
                        ErrorLog(fmt::format(
                            "Writer thread join failed: {}\n", e.what()));
                        m_wedged.store(true, std::memory_order_release);
                        try { m_thread.detach(); } catch (...) {}
                    } catch (...) {
                        ErrorLog("Writer thread join failed: unknown\n");
                        m_wedged.store(true, std::memory_order_release);
                        try { m_thread.detach(); } catch (...) {}
                    }
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
            // Sticky terminal state set when Stop()->join() throws and we
            // have to detach the orphaned writer. Subsequent Start() calls
            // throw to prevent a second writer from racing the orphan on
            // the queue + CSV file. Recovery is process restart only.
            std::atomic<bool> m_wedged{false};
            std::thread m_thread;
            std::mutex m_mutex;
            std::condition_variable m_cv;
            std::vector<FrameRow> m_queue;
        };

        FrameCsvSink g_csv;

        // ---- Auto-merge (post side only, on Ctrl+F9 stop or destructor) ---
        //
        // Thin wrapper around utils/merge.cpp's pure functions. This is the
        // OS-facing glue: resolves the three paths under localAppData,
        // applies the removeStale policy (gated on g_writerEverStarted so
        // we never wipe a recycled-PID file from an earlier process), and
        // opens the ofstream in binary mode so the LF '\n' characters
        // emitted by WriteMergedCsv survive MSVC's text-mode translation.
        //
        // All actual parsing, joining, stats, and formatting lives in
        // utils/merge.cpp where it can be unit-tested independently.
        //
        // By the time we reach this function in the toggle-OFF path, pre's
        // writer has already been joined (pre.ApplyToggle ran upstream in
        // the same xrEndFrame call). In the destructor-fallback path, pre
        // destructs strictly before post, so pre's CSV is closed here.
        void MergeIntoOutput() {
            namespace fs = std::filesystem;
            const auto pid = GetCurrentProcessId();
            const fs::path preCsv =
                localAppData / fmt::format("frames-{}-pre.csv", pid);
            const fs::path postCsv =
                localAppData / fmt::format("frames-{}-post.csv", pid);
            const fs::path outCsv =
                localAppData / fmt::format("frames-merged-{}.csv", pid);

            // Gated on g_writerEverStarted so a process that never recorded
            // does not delete a perfectly valid merged CSV inherited from a
            // previous process at the same PID.
            const auto removeStale = [&] {
                if (!g_writerEverStarted.load(std::memory_order_acquire)) {
                    return;
                }
                std::error_code ec;
                std::filesystem::remove(outCsv, ec);
            };

            // Default qpc_freq matches analyze.py (10 MHz) rather than the
            // local machine's QpcFreqHz(). In production both halves write
            // their header so this default never applies, but cross-
            // machine debugging (CSVs copied off the recording box) used
            // to give a platform-dependent factor difference between the
            // C++ merge and analyze.py. The fixed 10 MHz makes that
            // scenario reproducible.
            constexpr int64_t kDefaultQpcFreq = 10'000'000;
            const auto pre = merge::ReadFrameCsv(preCsv, kDefaultQpcFreq);
            const auto post = merge::ReadFrameCsv(postCsv, kDefaultQpcFreq);

            if (!pre.header_valid) {
                ErrorLog(fmt::format(
                    "Pre CSV has unexpected column header at {}: did you "
                    "point a merged-CSV here by mistake?\n",
                    preCsv.string()));
                removeStale();
                return;
            }
            if (!post.header_valid) {
                ErrorLog(fmt::format(
                    "Post CSV has unexpected column header at {}: did you "
                    "point a merged-CSV here by mistake?\n",
                    postCsv.string()));
                removeStale();
                return;
            }
            if (pre.rows.empty() || post.rows.empty()) {
                Log(fmt::format(
                    "Skipping auto-merge: pre={} rows, post={} rows\n",
                    pre.rows.size(), post.rows.size()));
                removeStale();
                return;
            }

            // Mirror analyze.py: warn on freq mismatch, use pre.qpc_freq
            // for both sides. ComputeMerge does the right thing with the
            // single-freq invariant; this log surfaces the surprise so
            // the user is not silently misled.
            if (pre.qpc_freq != post.qpc_freq) {
                Log(fmt::format(
                    "warning: qpc_freq mismatch pre={} post={} -- using pre\n",
                    pre.qpc_freq, post.qpc_freq));
            }

            const auto merged = merge::ComputeMerge(
                pre.rows, pre.qpc_freq, post.rows, post.qpc_freq);
            if (merged.empty()) {
                Log("Skipping auto-merge: no matched (frame_idx, thread_id) "
                    "pairs between pre and post\n");
                removeStale();
                return;
            }

            const auto stats = merge::ComputeStats(merged);
            if (stats.negative_target_count > 0) {
                Log(fmt::format(
                    "note: {} of {} frames have negative target_us "
                    "(QPC jitter / noise floor)\n",
                    stats.negative_target_count, merged.size()));
            }

            // Binary mode keeps line endings LF-only on every platform.
            // utils/merge.cpp writes '\n' literally; without binary mode
            // MSVC would translate to "\r\n" and break byte-equivalence
            // with analyze.py's output.
            std::ofstream out(outCsv,
                              std::ios::out | std::ios::trunc | std::ios::binary);
            if (!out) {
                ErrorLog(fmt::format("Failed to open merged output {}\n",
                                     outCsv.string()));
                removeStale();
                return;
            }
            merge::WriteMergedCsv(out, merged, stats);
            out.flush();
            // After write + flush, the ofstream sets failbit if any inserter
            // hit a hard error (ENOSPC, sandboxed write denial, broken pipe,
            // etc.). Surfacing the failure here turns a silently-truncated
            // merged CSV into an obvious "Wrote ... failed" line in the log.
            if (!out.good()) {
                ErrorLog(fmt::format(
                    "Merged output write failed (disk full / lost write "
                    "access during the merge): {}\n",
                    outCsv.string()));
                removeStale();
                return;
            }
            Log(fmt::format("Wrote merged CSV: {} ({} frames)\n",
                            outCsv.string(), merged.size()));
        }

        // ---- Toggle action ------------------------------------------------
        //
        // ApplyToggle(target) drives the side-effects of a state change AND
        // (on the pre side only) broadcasts the new state to shared memory
        // so post follows in the same xrEndFrame call.
        //
        // - Local state is committed FIRST. If g_csv.Start() throws (e.g.
        //   std::system_error from std::thread resource exhaustion, or a
        //   std::runtime_error from the FrameCsvSink wedged state), we
        //   bail out before touching shared memory. This guarantees:
        //     * post never sees a "broadcast was on but pre actually
        //       failed" state (finding N1),
        //     * g_writerEverStarted only flips on a real Start success,
        //       so a recycled-PID frames-merged-<pid>.csv from an earlier
        //       process is not nuked by a Start that never produced data
        //       (finding N2 / N4).
        // - Post calls ApplyToggle from its observe path; the constexpr
        //   guard on the broadcast prevents it from re-broadcasting and
        //   creating a generation feedback loop with pre.
        //
        // No-op if the local state already matches `target` -- defends
        // against post seeing a stale generation read or pre being asked
        // to toggle when already in the requested state.
        //
        // ASYMMETRY (intentional): on toggle ON, the writer is started
        // BEFORE we return, then g_skipNext=1 ensures THIS xrEndFrame
        // pass-throughs (frame 0 of the session would otherwise eat the
        // std::thread() spawn cost inside pre's bracket and inflate
        // target_us). On toggle OFF, the writer is stopped BEFORE we
        // return, so THIS xrEndFrame's record path is never entered --
        // there is no "frame of the stop" in the CSV. The count in the
        // header is the number of frames between start and stop press,
        // exclusive of the stop frame itself.
        void ApplyToggle(bool target) {
            const bool wasOn = g_monitoring.load(std::memory_order_acquire);
            if (wasOn == target) {
                return;
            }

            if (target) {
                g_frameCounter.store(0, std::memory_order_relaxed);
                g_skipNext.store(1, std::memory_order_release);
                try {
                    g_csv.Start();
                } catch (const std::exception& e) {
                    ErrorLog(fmt::format(
                        "Writer start failed: {}\n", e.what()));
                    // Bail before touching local g_monitoring or shared
                    // state, so pre / post stay consistent at "off".
                    return;
                } catch (...) {
                    ErrorLog("Writer start failed: unknown exception\n");
                    return;
                }
                g_writerEverStarted.store(true, std::memory_order_release);
            } else {
                g_csv.Stop();  // noexcept; catches its own join exceptions
                if constexpr (kIsPostSide) {
                    // Post is downstream, so by the time we reach this
                    // line pre's ApplyToggle has already returned -- which
                    // means pre's writer is drained and pre's CSV is
                    // closed. Safe to merge synchronously.
                    try {
                        MergeIntoOutput();
                    } catch (const std::exception& e) {
                        ErrorLog(fmt::format("Merge failed: {}\n", e.what()));
                    } catch (...) {
                        ErrorLog("Merge failed: unknown exception\n");
                    }
                }
            }

            // Commit local state only after the side-effect succeeded.
            g_monitoring.store(target, std::memory_order_release);

            // Broadcast on pre side only. Post is the observer; making it
            // broadcast would create a feedback loop where each side ping-
            // pongs the generation counter.
            if constexpr (kIsPreSide) {
                if (auto* shared = GetSharedToggleState()) {
                    shared->monitoring.store(target,
                                             std::memory_order_release);
                    const uint64_t newGen =
                        shared->generation.fetch_add(
                            1, std::memory_order_acq_rel) + 1;
                    g_lastSeenGeneration.store(
                        newGen, std::memory_order_release);
                }
            }

            // Logging from xrEndFrame is normally forbidden (see the file
            // banner). This is the documented exception: Log() costs only
            // matter when called per-frame; here we call it twice per
            // user-initiated toggle, which happens a few times per session
            // at most. We also emit an ETW event so consumers that watch
            // the layer in WPA see the toggle without parsing the .log.
            if (target) {
                Log(fmt::format("Monitoring STARTED on side='{}' "
                                "(Ctrl+F9 pressed)\n", kSideStr));
                TraceLoggingWrite(g_traceProvider,
                                  "MonitoringToggle",
                                  TLArg(kSideStr, "Side"),
                                  TLArg("started", "Action"));
            } else {
                Log(fmt::format("Monitoring STOPPED on side='{}' "
                                "(Ctrl+F9 pressed)\n", kSideStr));
                TraceLoggingWrite(g_traceProvider,
                                  "MonitoringToggle",
                                  TLArg(kSideStr, "Side"),
                                  TLArg("stopped", "Action"));
            }
        }

    } // namespace

    class OpenXrLayer : public OpenXrApi {
      public:
        OpenXrLayer() = default;

        // ResetInstance() (called from xrDestroyInstance) destroys this
        // OpenXrLayer while the process is still alive. If the user is
        // still in a monitoring session (didn't press Ctrl+F9 to stop),
        // wrap things up automatically -- otherwise the toggle-OFF path
        // already did the merge and there is nothing left to do here.
        //
        // Destructors are implicitly noexcept since C++11; an exception
        // escaping here calls std::terminate and takes the game down at
        // xrDestroyInstance. Everything that can throw is wrapped, and
        // g_csv.Stop() is itself declared noexcept.
        ~OpenXrLayer() override {
            if (!g_monitoring.load(std::memory_order_acquire)) {
                // Either: user pressed Ctrl+F9 to stop, and the merge
                // already happened on that toggle; or: user never started
                // a session, and there is nothing to merge.
                return;
            }
            g_csv.Stop();  // noexcept
            if constexpr (kIsPostSide) {
                try {
                    MergeIntoOutput();
                } catch (const std::exception& e) {
                    ErrorLog(fmt::format("Merge failed: {}\n", e.what()));
                } catch (...) {
                    ErrorLog("Merge failed: unknown exception\n");
                }
            }
            g_monitoring.store(false, std::memory_order_release);
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
                "Once started, frames will be written to: {}\n",
                (localAppData / fmt::format("frames-{}-{}.csv",
                                            GetCurrentProcessId(), kSideStr))
                    .string()));
            Log("Press Ctrl+F9 (system-wide hotkey) to start / stop monitoring\n");

            // Seed g_lastComboDown with the CURRENT physical key state so
            // the first xrEndFrame poll does not synthesize a rising edge
            // on a combo the user has been holding for unrelated reasons
            // (push-to-talk, stream-deck script, AutoHotkey macro,
            // accessibility binding, ...). Forcing the state to `false`
            // here -- the previous behaviour -- would have produced a
            // spurious ApplyToggle(true) on every such case, silently
            // starting a recording that would then overwrite the
            // previous session's per-side CSVs at writer-thread Start
            // time.
            //
            // Also resets the debounce timestamp so the first real edge
            // after a fresh xrCreateInstance is honored regardless of
            // when the previous instance toggled.
            g_lastComboDown.store(SampleHotkeyDown(),
                                  std::memory_order_release);
            g_lastToggleQpc.store(0, std::memory_order_release);

            // Eagerly map the shared toggle state so both DLLs hit it
            // before the first xrEndFrame, surfacing any mapping error
            // in the init log rather than under load.
            (void)GetSharedToggleState();

            // Writer is NOT started here. The first Ctrl+F9 (pre side) or
            // the first observed generation bump (post side) calls
            // ApplyToggle(true), which spins up the writer and truncates
            // the per-side CSV.
            return result;
        }

        XrResult xrEndFrame(XrSession session,
                            const XrFrameEndInfo* frameEndInfo) override {
            // Toggle decision happens OUTSIDE the QPC bracket. Total
            // overhead in the no-monitoring case: three GetAsyncKeyState
            // calls (Ctrl, F9, RAlt for the AltGr mask) + one atomic
            // exchange for pre; acquire-load on shared->generation +
            // local comparison for post. Sub-microsecond on modern x86.
            if constexpr (kIsPreSide) {
                // Pre is the authoritative poller. ApplyToggle handles the
                // broadcast to shared memory internally, ONLY on success,
                // so a Start() failure leaves shared state consistent with
                // pre's actual local state.
                if (ConsumeHotkeyEdge()) {
                    // Debounce: ignore the edge if the previous toggle was
                    // under kDebounceMs ago. Catches OBS / ShadowPlay
                    // instant-replay bindings that fire Ctrl+F9 twice in
                    // rapid succession (a churned START/STOP would stall
                    // the frame thread on the merge -- documented in the
                    // README's Limitations section).
                    constexpr int64_t kDebounceMs = 500;
                    const int64_t now = Qpc();
                    const int64_t freq = QpcFreqHz();
                    const int64_t debounceTicks = (freq * kDebounceMs) / 1000;
                    const int64_t last =
                        g_lastToggleQpc.load(std::memory_order_acquire);
                    if (last == 0 || (now - last) >= debounceTicks) {
                        g_lastToggleQpc.store(now, std::memory_order_release);
                        ApplyToggle(
                            !g_monitoring.load(std::memory_order_acquire));
                    }
                }
            } else {
                // Post follows what pre wrote to shared memory. The
                // pre -> target -> post chain order means pre's write is
                // visible here by virtue of release/acquire ordering on
                // the generation counter. ApplyToggle on post is gated by
                // a constexpr so it does NOT re-broadcast.
                if (auto* shared = GetSharedToggleState()) {
                    const uint64_t gen = shared->generation.load(
                        std::memory_order_acquire);
                    if (gen != g_lastSeenGeneration.load(
                            std::memory_order_acquire)) {
                        g_lastSeenGeneration.store(
                            gen, std::memory_order_release);
                        const bool target = shared->monitoring.load(
                            std::memory_order_acquire);
                        ApplyToggle(target);
                    }
                }
            }

            // Skip the first frame after a toggle ON so the std::thread()
            // spawn cost in ApplyToggle does not land inside any bracket.
            // Both halves decrement in lockstep (each ApplyToggle sets
            // skipNext=1) so the merge stays matched.
            if (g_skipNext.load(std::memory_order_acquire) > 0) {
                g_skipNext.fetch_sub(1, std::memory_order_acq_rel);
                return OpenXrApi::xrEndFrame(session, frameEndInfo);
            }

            // Pass-through when monitoring is off. The host never knows
            // the layer is even there until the user presses Ctrl+F9.
            if (!g_monitoring.load(std::memory_order_acquire)) {
                return OpenXrApi::xrEndFrame(session, frameEndInfo);
            }

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
