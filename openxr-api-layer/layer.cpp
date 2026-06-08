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
// by (pid, thread_id, display_time) and target_cpu is computed per frame.
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

// GPU side of the sandwich: a D3D11 single-timestamp-per-frame profiler.
// The D3D11 implementation is isolated behind gpu::IGpuTimer so layer.cpp
// stays free of D3D query plumbing.
#include "utils/gpu_timer.h"

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
        // Also doubles as the "did this session record anything" signal
        // that MergeIntoOutput's zero-frame guard checks before touching
        // the merged CSV on disk.
        //
        // g_skipNext drops the FIRST frame after a toggle ON: that frame's
        // pre.bracket subsumes post's std::thread() spawn (~50-500 us under
        // Windows) which would otherwise contaminate target_us on frame 0
        // of every session.
        std::atomic<uint64_t> g_frameCounter{0};
        std::atomic<uint32_t> g_skipNext{0};
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
        //   - The caller debounces successful STARTs to >= 500 ms so a
        //     remapper / OBS / ShadowPlay firing Ctrl+F9 repeatedly cannot
        //     flap the recording on. A STOP is NEVER debounced: dropping it
        //     would leave the writers running after the user pressed Ctrl+F9
        //     to stop. A rapid START->STOP pair therefore nets to OFF as
        //     intended; its merge covers only the handful of frames in that
        //     <500 ms window (cheap -- not the 10-50 ms stall of a full
        //     session).
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
        // The frame thread does the cheapest possible thing: a single
        // SPSC ring-buffer push (two atomic stores + one branch, ~15-20 ns
        // on modern x86). No mutex, no condition_variable signal, no
        // heap allocation. The writer thread polls the ring at ~10 ms
        // intervals and drains it.
        //
        // WHY LOCK-FREE: post.bracket subtracts everything inside post's
        // xrEndFrame from pre.bracket. Whatever still sits OUTSIDE post's
        // bracket (the Append call + return epilogue) ends up attributed
        // to the target layer. A mutex lock costs ~100 ns under
        // contention; a ring push costs ~20 ns. With the wide-bracket
        // change, this delta IS the residual target_us bias for sub-µs
        // target layers.

        struct FrameRow {
            // The frame's predicted display time (XrFrameEndInfo::displayTime,
            // an XrTime). Both halves of the sandwich observe the SAME value
            // for a given host frame, so it is the intrinsic key the merge
            // joins on -- unlike a per-DLL counter, it cannot desync when the
            // layer ordering or a one-sided start throws the two sides out of
            // step. Stored as uint64_t (XrTime is non-negative in practice) so
            // the merge keys and the GPU ring need no signed/unsigned churn.
            uint64_t display_time;
            uint32_t thread_id;
            int64_t qpc_entry;
            int64_t qpc_exit;
        };

        // SPSC ring buffer with a single producer (frame thread) and a
        // single consumer (writer thread). N must be a power of two so
        // the index wrap is a cheap bitmask instead of a modulo divide.
        //
        // SINGLE-PRODUCER CONTRACT: Push() does a plain `load + store`
        // pair on m_head, NOT a CAS. Concurrent producers would race on
        // the same slot and tear the FrameRow write. The contract is
        // satisfied today because the OpenXR spec requires per-session
        // xrEndFrame serialization, and the layer holds one OpenXrLayer
        // (one g_csv) per XrInstance. A future multi-session host that
        // calls xrEndFrame concurrently from two sessions of the same
        // XrInstance would break this -- the fix would be either to
        // intercept xrCreateSession and refuse the second one, or to
        // upgrade Push to a CAS-based MPSC variant. Today's overlay /
        // compositor uses a SEPARATE XrInstance so the contract holds.
        //
        // Size: 4096 * sizeof(Slot) = 256 KiB per FrameCsvSink. With one
        // row per host xrEndFrame at 90 Hz, the ring buffers ~45 s of
        // records -- more than enough to absorb any disk hiccup. If it
        // ever does overflow, Push returns false and the caller bumps a
        // dropped counter; the row is dropped, the frame thread does
        // NOT block.
        template <typename T, size_t N>
        class SpscRingBuffer {
            static_assert((N & (N - 1)) == 0, "N must be a power of two");
            static constexpr size_t kMask = N - 1;

            // Each slot lives on its own cache line. alignas on the
            // array base alone (the original layout) only aligned the
            // first slot -- adjacent FrameRow entries (32 bytes each)
            // would still share a 64-byte line, recreating the
            // producer/consumer false-sharing that we tried to avoid
            // with the head/tail padding below.
            struct alignas(64) Slot {
                T value;
            };

          public:
            // Producer: returns false if full. ~15-20 ns on x86 (relaxed
            // load, acquire load, conditional, store, release store).
            bool Push(const T& item) {
                const size_t head = m_head.load(std::memory_order_relaxed);
                const size_t next = (head + 1) & kMask;
                if (next == m_tail.load(std::memory_order_acquire)) {
                    return false;
                }
                m_buffer[head].value = item;
                m_head.store(next, std::memory_order_release);
                return true;
            }

            // Consumer: returns false if empty.
            bool Pop(T& out) {
                const size_t tail = m_tail.load(std::memory_order_relaxed);
                if (tail == m_head.load(std::memory_order_acquire)) {
                    return false;
                }
                out = m_buffer[tail].value;
                m_tail.store((tail + 1) & kMask, std::memory_order_release);
                return true;
            }

            bool ConsumerEmpty() const {
                return m_tail.load(std::memory_order_relaxed) ==
                       m_head.load(std::memory_order_acquire);
            }

          private:
            // 64-byte padding so head and tail live on different cache
            // lines -- avoids the classic SPSC false-sharing where the
            // producer's head store invalidates the consumer's tail
            // cache line on every push.
            alignas(64) std::atomic<size_t> m_head{0};
            alignas(64) std::atomic<size_t> m_tail{0};
            alignas(64) Slot m_buffer[N]{};
        };

        // Generic background CSV writer. Two instantiations exist: the CPU
        // sink (FrameRow) and the GPU sink (gpu::GpuRow); Traits supplies the
        // file-name prefix, the header block, and the per-row formatter so the
        // threading / lock-free-ring / lifecycle machinery is written once.
        //
        // RowT must be trivially copyable (it travels through the SPSC ring by
        // value). Traits must provide:
        //   static constexpr const char* kFilePrefix;          // "frames"/"gpu"
        //   static void WriteHeader(std::ostream&);            // # lines + cols
        //   static void WriteRow(std::ostream&, const RowT&);  // one data row
        template <typename RowT, typename Traits>
        class CsvSink {
          public:
            // The g_csv / g_gpuCsv instances live at namespace scope and are
            // destroyed during DLL unload. If the host terminated via
            // TerminateProcess / Alt+F4 / anti-cheat kill / a crash mid-
            // recording, xrDestroyInstance was never called, OpenXrLayer's
            // destructor never ran, and the writer thread is still joinable.
            // Without this destructor, ~std::thread on a joinable thread
            // invokes std::terminate during teardown -- the layer would crash
            // the host's exit path. Stop() is idempotent so a clean shutdown
            // path (where ApplyToggle(false) already stopped the writer)
            // passes through here harmlessly.
            ~CsvSink() { Stop(); }

            // Throws std::system_error if std::thread construction fails
            // (e.g., thread resource exhaustion), or std::runtime_error if
            // we are wedged from a previous failed Stop() join. Callers
            // (ApplyToggle) wrap the call so the exception never reaches
            // xrEndFrame.
            void Start() {
                if (m_wedged.load(std::memory_order_acquire)) {
                    // A previous Stop() had to detach because join() threw.
                    // The orphaned writer thread may still hold the CSV
                    // file open; spawning a new one would race on the
                    // shared ring + CSV file. Refuse -- recovery requires
                    // a process restart.
                    throw std::runtime_error(
                        "FrameCsvSink wedged after a prior detach; "
                        "restart the host process to recover");
                }
                // Reset BEFORE the CAS so an Start() that hits the
                // running=true early-return still observes a fresh
                // counter on the next Stop()->footer cycle. Symmetry
                // with Stop()'s reset at the end ensures the counter
                // is always cleared between sessions, never leaking
                // drops across the boundary.
                m_droppedQueueFull.store(0, std::memory_order_relaxed);
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
            // the orphaned one on the ring + CSV file.
            //
            // Latency: prior to this commit Stop() relied on a sleep_for
            // tick to expire (~15 ms on Windows without timeBeginPeriod).
            // Now Stop signals the writer through a cv whose ONLY purpose
            // is the stop wakeup -- Append() does NOT touch the cv, so
            // the frame-thread fast path stays at ~20 ns. Stop's
            // wake-then-join completes in under a millisecond in the
            // common case.
            void Stop() noexcept {
                bool expected = true;
                if (!m_running.compare_exchange_strong(expected, false)) {
                    return;
                }
                // The lock is held briefly only to ensure the wait_for
                // predicate observes the m_running=false store above
                // (the lock pairs with the unique_lock in Run()). The
                // notify itself is mutex-free.
                {
                    std::lock_guard<std::mutex> lock(m_wake_mutex);
                }
                m_wake_cv.notify_all();
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

            // Frame-thread hot path. The whole xrEndFrame budget for the
            // post side's residual bias hangs on this being cheap and
            // never blocking. No mutex, no cv signal -- the writer
            // polls (waking instantly only on Stop). Drop counter is
            // bumped on overflow (~45 s of continuous disk-stall at
            // 90 Hz before the ring fills).
            //
            // SINGLE PRODUCER ONLY (see SpscRingBuffer's contract).
            void Append(const RowT& row) {
                if (!m_ring.Push(row)) {
                    m_droppedQueueFull.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }

            // Acquire matches the producer's relaxed fetch_add: the
            // synchronization that actually publishes the final counter
            // value to the consumer happens via the m_running release
            // store in Stop() and the m_thread.join() that pairs with
            // the writer's exit. Stating acquire here makes the
            // happens-before chain explicit for non-x86 readers.
            uint64_t DroppedQueueFull() const {
                return m_droppedQueueFull.load(std::memory_order_acquire);
            }

          private:
            void Run() {
                // entry.cpp resolves localAppData to the SHARED base folder
                // (suffix _pre / _post stripped), so we add the side back
                // into the CSV filename to keep pre and post outputs
                // distinct. PID also goes in the name so concurrent OpenXR
                // processes don't trample each other.
                const std::filesystem::path path =
                    localAppData / fmt::format("{}-{}-{}.csv", Traits::kFilePrefix,
                                               GetCurrentProcessId(), kSideStr);

                // LAZY OPEN: defer truncation + header write until the
                // FIRST batch with actual rows arrives. A parasitic
                // toggle ON-then-OFF (Discord screenshot binding fires
                // while the user is doing something else, then they hit
                // Ctrl+F9 to cancel before any xrEndFrame Appends) used
                // to wipe the previous session's CSV at writer-start
                // time -- the file was opened with std::ios::trunc and
                // the header written immediately, even when no rows
                // were ever pushed. Combined with MergeIntoOutput's
                // zero-frame guard, the result on disk is: a session
                // that recorded zero rows leaves every previously
                // existing file (per-side CSVs AND frames-merged) bit-
                // for-bit untouched.
                std::ofstream stream;
                bool open_failed = false;
                const auto ensureOpen = [&]() -> bool {
                    if (stream.is_open()) {
                        return true;
                    }
                    if (open_failed) {
                        return false;
                    }
                    // Binary mode keeps line endings LF-only on every platform.
                    // Traits::WriteHeader / Traits::WriteRow emit '\n' literally;
                    // without binary mode MSVC translates to "\r\n" and the per-
                    // side CSVs end up CRLF on Windows / LF elsewhere.
                    // MergeIntoOutput's ofstream uses the same flag for the same
                    // reason -- keep them in sync.
                    stream.open(path,
                                std::ios::out | std::ios::trunc | std::ios::binary);
                    if (!stream) {
                        ErrorLog(fmt::format("Failed to open {}\n",
                                             path.string()));
                        open_failed = true;
                        return false;
                    }
                    // Pin the classic locale so a host that installed a
                    // thousands-grouping global locale cannot inject separators
                    // into the integer CSV columns (qpc_freq header,
                    // display_time, thread_id, qpc/gpu values) and corrupt the
                    // file. Floats are written via fmt (locale-independent).
                    stream.imbue(std::locale::classic());
                    Traits::WriteHeader(stream);
                    return true;
                };

                const auto writeOne = [&](const RowT& r) {
                    if (ensureOpen()) {
                        Traits::WriteRow(stream, r);
                    }
                };

                // Poll loop. The 10 ms timeout caps the worst-case
                // "row pushed but not yet flushed" window; on Windows
                // without an active timeBeginPeriod, wait_for's
                // underlying scheduler granularity is ~15 ms, so the
                // steady-state poll cadence is "10 ms requested,
                // ~15 ms observed". Stop() flips m_running AND notifies
                // m_wake_cv, so the wait_for predicate fires
                // immediately on stop -- the previous sleep_for-only
                // design (no notify) paid the full ~15 ms tick on
                // every toggle-OFF, which stacked with the merge stall
                // directly on the frame thread.
                RowT row{};
                while (true) {
                    bool drained_any = false;
                    while (m_ring.Pop(row)) {
                        writeOne(row);
                        drained_any = true;
                    }
                    if (drained_any && stream.is_open()) {
                        stream.flush();
                    }

                    if (!m_running.load(std::memory_order_acquire)) {
                        // Final drain after running=false. Producer may
                        // have pushed between our last Pop and the flip.
                        while (m_ring.Pop(row)) {
                            writeOne(row);
                        }
                        if (stream.is_open()) {
                            stream.flush();
                        }
                        // Footer: each Traits decides what end-of-session
                        // diagnostics to emit. CPU sink writes the queue-
                        // overflow count; GPU sink ALSO surfaces its timer's
                        // ring-overwrite count (the GPU readback ring is
                        // independent of this CSV ring). Quiet when there's
                        // nothing to say so a clean session stays clean.
                        const auto drops = DroppedQueueFull();
                        if (Traits::HasFooter(drops) && ensureOpen()) {
                            Traits::WriteFooter(stream, drops);
                            stream.flush();
                        }
                        return;
                    }

                    std::unique_lock<std::mutex> lock(m_wake_mutex);
                    m_wake_cv.wait_for(
                        lock, std::chrono::milliseconds(10),
                        [this] {
                            return !m_running.load(
                                std::memory_order_acquire);
                        });
                }
            }

            std::atomic<bool> m_running{false};
            // Sticky terminal state set when Stop()->join() throws and we
            // have to detach the orphaned writer. Subsequent Start() calls
            // throw to prevent a second writer from racing the orphan on
            // the ring + CSV file.
            std::atomic<bool> m_wedged{false};
            std::atomic<uint64_t> m_droppedQueueFull{0};
            std::thread m_thread;
            // Stop-only wakeup. Append() never touches these (the frame
            // thread stays lock-free); only Stop() ever calls notify.
            // The writer waits on m_wake_cv with a 10 ms timeout, so
            // new rows pushed between Stop and the previous Pop drain
            // still hit disk on the final drain phase.
            std::mutex m_wake_mutex;
            std::condition_variable m_wake_cv;
            // 4096 rows (~128 KiB) absorbs ~45 s of continuous 90-Hz
            // recording even if the writer thread never gets a tick of
            // CPU. Overflow is unreachable on a healthy machine; the
            // counter is here to flag pathological disk stalls.
            SpscRingBuffer<RowT, 4096> m_ring;
        };

        // ---- CSV-schema traits + sink instances --------------------------
        //
        // Each Traits struct is the only place a row type's on-disk format
        // lives: the file-name prefix, the comment + column header, and the
        // per-row formatter. CsvSink<RowT, Traits> supplies everything else.

        struct FrameCsvTraits {
            static constexpr const char* kFilePrefix = "frames";
            static void WriteHeader(std::ostream& s) {
                s << "# qpc_freq=" << QpcFreqHz() << "\n"
                  << "# side=" << kSideStr << "\n"
                  << "# layer=" << LayerName << "\n"
                  << "# fn=xrEndFrame\n"
                  << "display_time,thread_id,qpc_entry,qpc_exit\n";
            }
            static void WriteRow(std::ostream& s, const FrameRow& r) {
                s << r.display_time << ',' << r.thread_id << ','
                  << r.qpc_entry << ',' << r.qpc_exit << '\n';
            }
            // The CPU sink's only end-of-session diagnostic is the SPSC ring
            // overflow count -- emitted only when non-zero so a clean session
            // produces a clean CSV.
            static bool HasFooter(uint64_t droppedQueueFull) {
                return droppedQueueFull > 0;
            }
            static void WriteFooter(std::ostream& s, uint64_t droppedQueueFull) {
                s << "# session_end dropped_queue_full=" << droppedQueueFull << "\n";
            }
        };

        // Per-DLL singleton GPU timer, recreated on each xrCreateSession and
        // released on each xrDestroySession. Null on non-D3D11/D3D12 hosts
        // (Vulkan / OpenGL) or if backend creation failed. Declared HERE
        // (before GpuCsvTraits) so the GPU sink's footer code can read the
        // snapshot helpers below inline.
        //
        // THREADING: three thread classes touch this pointer.
        //   * The frame thread (xrEndFrame, xrCreateSession, xrDestroySession,
        //     ApplyToggle on the frame thread). All accesses on this thread
        //     are serialised by the OpenXR spec's per-session "one thread at
        //     a time" contract.
        //   * The GPU CSV writer thread (CsvSink<gpu::GpuRow>::Run). Reads
        //     g_gpuTimer via SnapshotBackendName() / SnapshotDroppedFrames()
        //     to fill the "# gpu_clock=" header line and the
        //     "# session_end gpu_ring_overflow=" footer line.
        //   * Test / future threads: do not access.
        //
        // The race the mutex below closes: xrDestroySession can call
        // g_gpuTimer.reset() while the writer thread is mid-WriteHeader or
        // mid-WriteFooter on the GPU sink. Without serialisation, the writer
        // would dispatch BackendName() / DroppedFrames() through a freed
        // vtable on a host that destroys its session without first toggling
        // Ctrl+F9 off (Unity XR Toolkit lifecycle reset, headset reconfigure,
        // etc.). All writer-thread reads go through the snapshot helpers
        // below; all frame-thread writes (the two reset() calls + the
        // assignment) take the lock too. Frame-thread READS
        // (RecordTimestamp, PollResolved, Reset) do NOT take the lock --
        // they're already serialised against the create/destroy sites by
        // virtue of running on the same thread.
        //
        // SAME SINGLE-SESSION ASSUMPTION AS THE SPSC RING ABOVE: an app that
        // holds two XrSession handles open concurrently on the same DLL would
        // see them share this timer. Every shipping host we know of --
        // including OpenComposite's probe-then-real pattern -- destroys the
        // previous session before creating the next, which IS the case
        // xrCreateSession's reset() below handles. A truly concurrent-
        // session host would have to either refuse the second session or
        // upgrade this to a per-session map.
        std::mutex g_gpuTimerMutex;
        std::unique_ptr<gpu::IGpuTimer> g_gpuTimer;

        // Writer-thread accessors. Const char* return is safe because every
        // backend's BackendName() returns a static string literal; uint64_t
        // is copied out under the lock. Callers MUST NOT cache the returned
        // pointer beyond the immediate header-write line.
        const char* SnapshotBackendName() {
            std::lock_guard<std::mutex> lock(g_gpuTimerMutex);
            return g_gpuTimer ? g_gpuTimer->BackendName() : "unknown";
        }
        uint64_t SnapshotGpuDroppedFrames() {
            std::lock_guard<std::mutex> lock(g_gpuTimerMutex);
            return g_gpuTimer ? g_gpuTimer->DroppedFrames() : 0;
        }

        struct GpuCsvTraits {
            static constexpr const char* kFilePrefix = "gpu";
            static void WriteHeader(std::ostream& s) {
                // No qpc_freq line: the GPU clock frequency is per-row (it
                // comes from the active backend's clock source -- D3D11
                // disjoint query, D3D12 GetTimestampFrequency() at init),
                // so it lives in the gpu_freq column rather than a single
                // file-level header.
                //
                // The "gpu_clock" line carries the backend name pulled from
                // the writer-thread-safe snapshot helper above. By the time
                // the writer reaches WriteHeader the sink has at least one
                // row to flush, so g_gpuTimer was non-null when that row
                // was queued; the lock prevents a concurrent
                // xrDestroySession from racing in between.
                s << "# gpu_clock=" << SnapshotBackendName() << "\n"
                  << "# side=" << kSideStr << "\n"
                  << "# layer=" << LayerName << "\n"
                  << "# fn=xrEndFrame\n"
                  << "display_time,gpu_ticks,gpu_freq,valid\n";
            }
            static void WriteRow(std::ostream& s, const gpu::GpuRow& r) {
                s << r.display_time << ',' << r.gpu_ticks << ','
                  << r.gpu_freq << ',' << r.valid << '\n';
            }
            // GPU sink surfaces TWO independent drop sources at end of
            // session: the SPSC CSV-ring overflow (same as the CPU sink, ~45
            // s of disk stall at 90 Hz to fill), AND the GPU TIMER's own
            // ring-overwrite count (g_gpuTimer->DroppedFrames(), bumped when
            // GPU readback is >kGpuRingSize frames behind, i.e. a >~44 ms
            // GPU stall). Read here on the writer thread through the
            // snapshot helper so a concurrent xrDestroySession cannot
            // free the timer between our null-check and the method call.
            static bool HasFooter(uint64_t droppedQueueFull) {
                return droppedQueueFull > 0 || SnapshotGpuDroppedFrames() > 0;
            }
            static void WriteFooter(std::ostream& s, uint64_t droppedQueueFull) {
                if (droppedQueueFull > 0) {
                    s << "# session_end dropped_queue_full=" << droppedQueueFull << "\n";
                }
                const uint64_t overwritten = SnapshotGpuDroppedFrames();
                if (overwritten > 0) {
                    s << "# session_end gpu_ring_overflow=" << overwritten << "\n";
                }
            }
        };

        using FrameCsvSink = CsvSink<FrameRow, FrameCsvTraits>;
        using GpuCsvSink = CsvSink<gpu::GpuRow, GpuCsvTraits>;

        FrameCsvSink g_csv;
        GpuCsvSink g_gpuCsv;

        // One-shot, best-effort drain of any GPU timestamps the timer has
        // ALREADY resolved, into the GPU CSV sink. Called at stop / teardown
        // right before g_gpuCsv.Stop(): the per-frame DrainGpu() runs at
        // different points on pre (after qpc_exit) vs post (inside the
        // bracket), so at stop each side can still be holding a few resolved
        // samples the writer never picked up -- without this they are dropped
        // and the last frames' target_gpu_us go blank. Off the hot path, so a
        // local buffer is fine (unlike the per-frame DrainGpu's reused scratch).
        // Frames the GPU has NOT finished yet are not recovered (that would
        // need a blocking fence wait); the resolved tail is. No-op on a null
        // timer (Vulkan / OpenGL host).
        void DrainResolvedGpuOnce() {
            if (!g_gpuTimer) {
                return;
            }
            std::vector<gpu::GpuRow> resolved;
            g_gpuTimer->PollResolved(resolved);
            for (const auto& row : resolved) {
                g_gpuCsv.Append(row);
            }
        }

        // Walks an XrBaseInStructure-style `next` chain for the first
        // XrGraphicsBindingD3D11KHR. Returns nullptr if the app uses a
        // different graphics API -- the caller then tries the D3D12 binding
        // (see FindD3D12Binding below) before falling back to CPU-only. We
        // rely on the common-initial-sequence layout (XrStructureType type;
        // const void* next) shared by every OpenXR `next` struct.
        const XrGraphicsBindingD3D11KHR* FindD3D11Binding(const void* next) {
            for (auto* base = reinterpret_cast<const XrBaseInStructure*>(next);
                 base != nullptr; base = base->next) {
                if (base->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                    return reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(base);
                }
            }
            return nullptr;
        }

        // Same walk for XrGraphicsBindingD3D12KHR. The struct carries an
        // ID3D12Device* AND an ID3D12CommandQueue* (D3D12 has no immediate
        // context -- the app submits commands to a queue it owns), so the
        // GPU timer needs BOTH handles: device to create the query heap +
        // command lists + fence, queue to ExecuteCommandLists onto the
        // same stream the app draws on.
        const XrGraphicsBindingD3D12KHR* FindD3D12Binding(const void* next) {
            for (auto* base = reinterpret_cast<const XrBaseInStructure*>(next);
                 base != nullptr; base = base->next) {
                if (base->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                    return reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(base);
                }
            }
            return nullptr;
        }

        // ---- Auto-merge (post side only, on Ctrl+F9 stop or destructor) ---
        //
        // Thin wrapper around utils/merge.cpp's pure functions. This is the
        // OS-facing glue: resolves the three paths under localAppData,
        // applies the data-preservation policy (zero-frame guard + a
        // write-to-temp-then-atomic-rename so a failed merge never destroys
        // the previous session's output), and opens the temp ofstream in
        // binary mode so the LF '\n' characters emitted by WriteMergedCsv
        // survive MSVC's text-mode translation.
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
            const fs::path gpuPreCsv =
                localAppData / fmt::format("gpu-{}-pre.csv", pid);
            const fs::path gpuPostCsv =
                localAppData / fmt::format("gpu-{}-post.csv", pid);
            const fs::path outCsv =
                localAppData / fmt::format("frames-merged-{}.csv", pid);

            // ZERO-FRAME GUARD: a parasitic toggle (accidental Ctrl+F9
            // from another app fires the rising edge, user cancels with
            // a second Ctrl+F9 before any xrEndFrame Append) MUST NOT
            // remove the previous session's frames-merged-<pid>.csv
            // from disk. With the FrameCsvSink lazy-open change, the
            // per-side CSVs are also untouched in this case -- we mirror
            // that by skipping the merge entirely here. g_frameCounter
            // is reset to 0 on every ApplyToggle(true) and incremented
            // only on real recorded frames, so g_frameCounter == 0 at
            // toggle-OFF time means "this session never wrote a row".
            if (g_frameCounter.load(std::memory_order_acquire) == 0) {
                Log("Skipping merge: this session recorded zero frames "
                    "(parasitic toggle, or xrDestroyInstance fired before "
                    "any xrEndFrame). Previous frames-merged-<pid>.csv "
                    "(if any) preserved on disk.\n");
                return;
            }

            // DATA-LOSS POLICY: the previous session's frames-merged-<pid>.csv
            // is preserved through EVERY failure path below. The new merge is
            // written to a sibling temp file and atomically renamed over the
            // destination only once a complete, valid file exists (bottom of
            // this function). The earlier code deleted the destination eagerly
            // on the error paths AND opened it in truncate mode before the
            // write, so a session that recorded frames but then failed
            // transiently (a per-side CSV that never flushed, ENOSPC, a lost
            // ACL) destroyed a still-valid merged CSV and left nothing behind.
            // A stale-but-valid file from an earlier session beats no file --
            // and matches what the zero-frame guard above already does; the
            // logs below tell the user the merge was skipped.

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
                return;
            }
            if (!post.header_valid) {
                ErrorLog(fmt::format(
                    "Post CSV has unexpected column header at {}: did you "
                    "point a merged-CSV here by mistake?\n",
                    postCsv.string()));
                return;
            }
            if (pre.rows.empty() || post.rows.empty()) {
                Log(fmt::format(
                    "Skipping auto-merge: pre={} rows, post={} rows\n",
                    pre.rows.size(), post.rows.size()));
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

            auto merged = merge::ComputeMerge(
                pre.rows, pre.qpc_freq, post.rows, post.qpc_freq);
            if (merged.empty()) {
                Log("Skipping auto-merge: no matched (display_time, thread_id) "
                    "pairs between pre and post (does a layer between pre and "
                    "post rewrite frameEndInfo->displayTime?)\n");
                return;
            }

            // GPU join. Missing gpu-<pid>-{pre,post}.csv is the normal case
            // on non-D3D11 hosts -- ReadGpuCsv returns empty + header_valid,
            // JoinGpu then leaves every target_gpu_us blank and the merge
            // proceeds CPU-only. A present-but-malformed GPU CSV is logged
            // and skipped (we do NOT abort the CPU merge over it). JoinGpu
            // must run before ComputeStats so the GPU aggregates populate.
            const auto gpuPre = merge::ReadGpuCsv(gpuPreCsv);
            const auto gpuPost = merge::ReadGpuCsv(gpuPostCsv);
            if (!gpuPre.header_valid) {
                ErrorLog(fmt::format(
                    "GPU pre CSV has unexpected column header at {}; "
                    "skipping GPU join (CPU merge proceeds)\n",
                    gpuPreCsv.string()));
            }
            if (!gpuPost.header_valid) {
                ErrorLog(fmt::format(
                    "GPU post CSV has unexpected column header at {}; "
                    "skipping GPU join (CPU merge proceeds)\n",
                    gpuPostCsv.string()));
            }
            merge::JoinGpu(merged, gpuPre.rows, gpuPost.rows);

            const auto stats = merge::ComputeStats(merged);
            if (stats.negative_target_count > 0) {
                Log(fmt::format(
                    "note: {} of {} frames have negative target_us "
                    "(QPC jitter / noise floor)\n",
                    stats.negative_target_count, merged.size()));
            }

            // Write to a sibling temp file, then atomically rename it over
            // outCsv -- so the previous merged CSV is replaced only once a
            // complete, valid new one exists, and survives any failure here.
            // Binary mode keeps line endings LF-only on every platform:
            // utils/merge.cpp writes '\n' literally; without binary mode MSVC
            // would translate to "\r\n" and break byte-equivalence with
            // analyze.py's output.
            fs::path tmpCsv = outCsv;
            tmpCsv += ".tmp";
            {
                std::ofstream out(
                    tmpCsv, std::ios::out | std::ios::trunc | std::ios::binary);
                if (!out) {
                    ErrorLog(fmt::format(
                        "Failed to open merged temp output {} (previous "
                        "merged CSV left intact)\n",
                        tmpCsv.string()));
                    return;
                }
                merge::WriteMergedCsv(out, merged, stats);
                out.flush();
                // The ofstream sets failbit if any inserter hit a hard error
                // (ENOSPC, sandboxed write denial, broken pipe). Detect it
                // before the rename so a half-written temp can never replace a
                // good destination.
                if (!out.good()) {
                    ErrorLog(fmt::format(
                        "Merged output write failed (disk full / lost write "
                        "access during the merge): {} (previous merged CSV "
                        "left intact)\n",
                        tmpCsv.string()));
                    out.close();
                    std::error_code rmEc;
                    fs::remove(tmpCsv, rmEc);  // drop the partial temp
                    return;
                }
            }  // close the stream before rename (Windows won't move an open file)

            // Atomic replace on the same directory/volume. On failure the
            // previous merged CSV is untouched and the new data remains in the
            // temp file, so nothing is lost either way.
            std::error_code ec;
            fs::rename(tmpCsv, outCsv, ec);
            if (ec) {
                ErrorLog(fmt::format(
                    "Failed to replace merged output {} (the new merge is at "
                    "{}): {}\n",
                    outCsv.string(), tmpCsv.string(), ec.message()));
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
        //   bail out before touching shared memory. This guarantees post
        //   never sees a "broadcast was on but pre actually failed" state.
        //
        //   Data-loss protection lives elsewhere now (was previously the
        //   g_writerEverStarted gate): MergeIntoOutput's zero-frame guard
        //   refuses to write or remove the merged CSV when this session
        //   recorded nothing, and FrameCsvSink::Run() defers opening the
        //   per-side CSV until the first non-empty batch arrives, so a
        //   Start() that throws (or a session that ends with no Append)
        //   leaves the previous session's files on disk untouched.
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
                    // Start both sinks. If the GPU sink throws after the CPU
                    // sink started, undo the CPU start (Stop() is noexcept +
                    // idempotent) so we never leave one writer running while
                    // we bail. Symmetric for any future sink added here.
                    g_csv.Start();
                    g_gpuCsv.Start();
                } catch (const std::exception& e) {
                    ErrorLog(fmt::format(
                        "Writer start failed: {}\n", e.what()));
                    g_csv.Stop();
                    g_gpuCsv.Stop();
                    // Bail before touching local g_monitoring or shared
                    // state, so pre / post stay consistent at "off".
                    return;
                } catch (...) {
                    ErrorLog("Writer start failed: unknown exception\n");
                    g_csv.Stop();
                    g_gpuCsv.Stop();
                    return;
                }
                // Drop any GPU slots still in flight from a PREVIOUS session:
                // they were tagged with that session's display_time values and
                // would otherwise resolve into the just-truncated GPU CSV under
                // the new session's frames. Also resets the per-session
                // drop counter so the new session's footer reports only its
                // own overflow count. Safe on the frame thread (no other
                // thread touches the timer's ring / counter; D3D11 path is
                // pure CPU bookkeeping, D3D12 does a bounded fence drain).
                // Null on Vulkan / OpenGL hosts where no timer was created.
                if (g_gpuTimer) {
                    g_gpuTimer->Reset();
                }
            } else {
                g_csv.Stop();     // noexcept; catches its own join exceptions
                // Recover any GPU timestamps the timer resolved but the
                // per-frame drain never picked up, before closing the sink.
                DrainResolvedGpuOnce();
                g_gpuCsv.Stop();  // noexcept; flush the GPU CSV before merge
                if constexpr (kIsPostSide) {
                    // Post is downstream, so by the time we reach this
                    // line pre's ApplyToggle has already returned -- which
                    // means pre's writers are drained and pre's CSVs are
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
        // g_csv.Stop() / g_gpuCsv.Stop() are themselves declared noexcept.
        ~OpenXrLayer() override {
            if (!g_monitoring.load(std::memory_order_acquire)) {
                // Either: user pressed Ctrl+F9 to stop, and the merge
                // already happened on that toggle; or: user never started
                // a session, and there is nothing to merge.
                return;
            }
            g_csv.Stop();     // noexcept
            DrainResolvedGpuOnce();  // recover the resolved GPU tail before closing
            g_gpuCsv.Stop();  // noexcept; flush GPU CSV before the merge reads it
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
            Log(fmt::format(
                "GPU timings (D3D11 / D3D12 hosts) will be written to: {}\n",
                (localAppData / fmt::format("gpu-{}-{}.csv",
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

        // xrCreateSession stands up the per-session D3D11 GPU timer from the
        // app's ID3D11Device (XrGraphicsBindingD3D11KHR in createInfo->next).
        // We never mutate createInfo and never fail the host's session over a
        // GPU-timer problem (CLAUDE.md rule 9: degrade gracefully) -- a
        // non-D3D11 host or a query-creation failure just means GPU rows are
        // not written and target_gpu_us comes out blank in the merge.
        // https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrCreateSession
        XrResult xrCreateSession(XrInstance instance,
                                 const XrSessionCreateInfo* createInfo,
                                 XrSession* session) override {
            const XrResult result =
                OpenXrApi::xrCreateSession(instance, createInfo, session);
            if (XR_FAILED(result) || !createInfo) {
                return result;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateSession",
                              TLArg(kSideStr, "Side"));

            // Walk the chain BEFORE taking the mutex so the search itself
            // (cheap pointer chasing) doesn't block the GPU CSV writer.
            // Holding the lock across both bindings being searched, then
            // the previous timer's destructor (which may wait up to a few
            // hundred ms on a fence), then the new MakeD3D* init (also
            // possibly slow) is acceptable: the only concurrent reader is
            // the GPU CSV writer footer/header path, and that path runs at
            // most twice per Ctrl+F9 cycle.
            const auto* d3d11 = FindD3D11Binding(createInfo->next);
            const auto* d3d12 = FindD3D12Binding(createInfo->next);
            if (d3d11 && d3d12) {
                // Non-conformant but observed in middleware (D3D11On12-style
                // wrappers, OpenComposite translators). The OpenXR runtime
                // picks one; we have no way to know which, so we prefer
                // D3D11 and warn loudly so a user seeing weird target_gpu_us
                // numbers can match it to this log line.
                Log("WARNING: xrCreateSession.next provides BOTH "
                    "XrGraphicsBindingD3D11KHR and XrGraphicsBindingD3D12KHR. "
                    "This is non-conformant; the OpenXR runtime will pick "
                    "one for actual rendering, but we cannot tell which. "
                    "Preferring D3D11 for the GPU timer -- if target_gpu_us "
                    "looks like uncorrelated noise, the runtime probably "
                    "chose D3D12 and our timestamps are landing on an idle "
                    "context.\n");
            }

            // Drop any timer from a prior session of this instance before
            // building the new one (multi-session hosts: OpenComposite does
            // a probe session then the real one). Lock guards against a
            // concurrent GPU CSV writer dereferencing the dying timer.
            {
                std::lock_guard<std::mutex> lock(g_gpuTimerMutex);
                g_gpuTimer.reset();
            }

            std::unique_ptr<gpu::IGpuTimer> built;
            const char* backend_log = nullptr;
            const char* fail_log = nullptr;
            if (d3d11) {
                built = gpu::MakeD3D11GpuTimer(d3d11->device);
                backend_log = "D3D11";
                fail_log = "D3D11 binding present but GPU query creation "
                           "failed; target_gpu_us will be blank for this "
                           "session\n";
            } else if (d3d12) {
                built = gpu::MakeD3D12GpuTimer(d3d12->device, d3d12->queue);
                backend_log = "D3D12";
                fail_log = "D3D12 binding present but GPU query heap / fence "
                           "/ command-list creation failed (or the queue is "
                           "not a DIRECT queue); target_gpu_us will be blank "
                           "for this session\n";
            }
            if (built) {
                std::lock_guard<std::mutex> lock(g_gpuTimerMutex);
                g_gpuTimer = std::move(built);
                Log(fmt::format(
                    "GPU timer active ({}) on side='{}'\n",
                    backend_log, kSideStr));
            } else if (fail_log) {
                Log(fail_log);
            } else {
                Log("No D3D11 or D3D12 binding in xrCreateSession (Vulkan / "
                    "OpenGL host); GPU monitoring off, CPU monitoring active\n");
            }
            return result;
        }

        // xrDestroySession releases the GPU timer's query objects before the
        // session's device can go away. It deliberately does NOT stop the CSV
        // sinks or touch g_monitoring: the OpenXR spec allows multiple
        // create/destroy-session cycles per instance, and the recording +
        // merge lifecycle is owned by the Ctrl+F9 toggle and xrDestroyInstance
        // (the ~OpenXrLayer fallback), not by session teardown. Any GPU slots
        // still in flight are dropped (the last 1-3 frames of a session may
        // lack GPU rows -- the merge tolerates the gap).
        // https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrDestroySession
        XrResult xrDestroySession(XrSession session) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrDestroySession",
                              TLArg(kSideStr, "Side"));
            // Lock against the GPU CSV writer thread reading g_gpuTimer for
            // the "# gpu_clock=" header or the "# session_end gpu_ring_
            // overflow=" footer line. Without the lock, an app that destroys
            // the session without first toggling Ctrl+F9 off (Unity XR
            // Toolkit lifecycle reset, headset reconfigure, scene restart)
            // races the writer thread directly: the writer could dispatch
            // a virtual call through a freed vtable.
            {
                std::lock_guard<std::mutex> lock(g_gpuTimerMutex);
                g_gpuTimer.reset();
            }
            return OpenXrApi::xrDestroySession(session);
        }

        // xrEndFrame is split into two distinct code paths via constexpr
        // because the bracket placement differs between pre and post:
        //
        // PRE (narrow bracket): the toggle decision + skipNext/monitoring
        // checks happen OUTSIDE the bracket. Pre's bracket measures only
        // the call to next, i.e. target.full_body + post.full_body. Pre's
        // own overhead does not appear in the bracket and therefore not
        // in target_us.
        //
        // POST (wide bracket): qpc_entry is taken at the VERY FIRST line
        // of the function, qpc_exit just before the Append call. So
        // post.bracket includes nearly everything post does -- including
        // its observe / skipNext / monitoring overhead, the runtime call,
        // the g_frameCounter increment, and the TraceLoggingWrite. When pre
        // subtracts post.bracket, all of that cancels out, leaving
        // target_us = target_layer's actual work + the lock-free
        // ring.Push call (the only thing post does AFTER qpc_exit). With
        // SPSC ring.Push at ~20 ns, residual target_us bias is ~20-30 ns
        // -- about 10x better than the narrow-bracket version that
        // inflated target_us by ~300-500 ns of post-side bookkeeping.
        //
        // Toggle-decision overhead in the no-monitoring case stays low:
        // three GetAsyncKeyState calls (Ctrl, F9, RAlt for the AltGr
        // mask) + one atomic exchange for pre; an acquire-load on
        // shared->generation + a local comparison for post. Sub-
        // microsecond on modern x86.
        XrResult xrEndFrame(XrSession session,
                            const XrFrameEndInfo* frameEndInfo) override {
            if constexpr (kIsPreSide) {
                // -------- PRE side --------
                if (ConsumeHotkeyEdge()) {
                    // The 500 ms debounce applies ONLY to turning monitoring
                    // ON. It swallows remapper / OBS / ShadowPlay bindings that
                    // fire Ctrl+F9 repeatedly so the recording cannot flap on.
                    // A STOP is ALWAYS honored: because each edge toggles, the
                    // swallowed second edge of a rapid pair is always the STOP,
                    // so the old direction-agnostic debounce dropped exactly
                    // the press the user cared about -- leaving the writers
                    // running and the CSVs growing after they pressed Ctrl+F9
                    // to stop. Now a deliberate fast START->STOP ends OFF, and a
                    // spurious double-fire nets to OFF (its few-frame merge is
                    // cheap) instead of leaving monitoring stuck ON.
                    const bool target =
                        !g_monitoring.load(std::memory_order_acquire);
                    const int64_t now = Qpc();
                    bool apply = true;
                    if (target) {  // START -- debounce rapid / spurious edges
                        constexpr int64_t kDebounceMs = 500;
                        const int64_t debounceTicks =
                            (QpcFreqHz() * kDebounceMs) / 1000;
                        const int64_t last =
                            g_lastToggleQpc.load(std::memory_order_acquire);
                        apply = (last == 0 || (now - last) >= debounceTicks);
                    }
                    if (apply) {
                        g_lastToggleQpc.store(now, std::memory_order_release);
                        ApplyToggle(target);
                    }
                }

                if (g_skipNext.load(std::memory_order_acquire) > 0) {
                    g_skipNext.fetch_sub(1, std::memory_order_acq_rel);
                    return OpenXrApi::xrEndFrame(session, frameEndInfo);
                }
                if (!g_monitoring.load(std::memory_order_acquire)) {
                    return OpenXrApi::xrEndFrame(session, frameEndInfo);
                }
                // We key the frame on frameEndInfo->displayTime below; a null
                // frameEndInfo is an app validation error the runtime will
                // reject. Pass it through unrecorded rather than dereferencing
                // null. Both halves forward the same (possibly null) pointer
                // down the chain, so neither records and the two sides stay
                // aligned.
                if (!frameEndInfo) {
                    return OpenXrApi::xrEndFrame(session, frameEndInfo);
                }

                // display_time + GPU marker BEFORE the narrow bracket. The GPU
                // RecordTimestamp's cost is backend-dependent (a few D3D11
                // immediate-context Begin/End calls, ~100 ns; or a D3D12
                // command-list Reset + EndQuery + ResolveQueryData + Close
                // + ExecuteCommandLists + Signal, ~5-15 us as a syscall
                // sequence) -- either way that work is pre's OWN overhead
                // and MUST stay OUTSIDE the bracket. Inside it would inflate
                // pre.bracket and bias target_us by the GPU-instrumentation
                // cost. T_pre marks the command stream as the last GPU event
                // before the target's draws. displayTime is read here so the
                // GPU row and the CPU row below share the same key (the merge
                // joins GPU pre<->post and GPU<->CPU on display_time).
                // g_frameCounter still advances, but only to feed
                // MergeIntoOutput's zero-frame guard ("did this session record
                // anything?"); it is no longer the CSV key, so the two sides'
                // counters drifting apart can no longer corrupt the join.
                const uint64_t display_time =
                    static_cast<uint64_t>(frameEndInfo->displayTime);
                g_frameCounter.fetch_add(1, std::memory_order_relaxed);
                const uint32_t thread_id = GetCurrentThreadId();
                if (g_gpuTimer) {
                    g_gpuTimer->RecordTimestamp(display_time);
                }

                // Narrow bracket -- pre's own work is OUTSIDE.
                const int64_t qpc_entry = Qpc();
                const XrResult result =
                    OpenXrApi::xrEndFrame(session, frameEndInfo);
                const int64_t qpc_exit = Qpc();

                TraceLoggingWrite(g_traceProvider,
                                  "xrEndFrame",
                                  TLArg(kSideStr, "Side"),
                                  TLArg(display_time, "DisplayTime"),
                                  TLArg(qpc_entry, "QpcEntry"),
                                  TLArg(qpc_exit, "QpcExit"),
                                  TLArg(qpc_exit - qpc_entry, "QpcDelta"));
                g_csv.Append({display_time, thread_id, qpc_entry, qpc_exit});

                // Drain resolved GPU timestamps AFTER qpc_exit (outside the
                // narrow bracket) so the GetData polls never inflate target_us.
                DrainGpu();
                return result;
            } else {
                // -------- POST side (wide bracket) --------
                //
                // qpc_entry FIRST so post's observe / skipNext /
                // monitoring overhead is INSIDE post.bracket and
                // therefore canceled when pre subtracts.
                const int64_t qpc_entry = Qpc();

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

                if (g_skipNext.load(std::memory_order_acquire) > 0) {
                    g_skipNext.fetch_sub(1, std::memory_order_acq_rel);
                    return OpenXrApi::xrEndFrame(session, frameEndInfo);
                }
                if (!g_monitoring.load(std::memory_order_acquire)) {
                    return OpenXrApi::xrEndFrame(session, frameEndInfo);
                }
                // Same null guard as pre: we key on frameEndInfo->displayTime
                // below, and both halves forward the same (possibly null)
                // pointer down the chain, so neither records the frame.
                if (!frameEndInfo) {
                    return OpenXrApi::xrEndFrame(session, frameEndInfo);
                }

                // display_time + GPU marker INSIDE the wide bracket (before the
                // runtime call). Both cancel in the pre-minus-post subtraction,
                // so they add no bias to target_us. T_post marks the command
                // stream right after the target's draws and before the
                // runtime/compositor's GPU work, so (T_post - T_pre) brackets
                // exactly the target's GPU work. displayTime is the intrinsic
                // per-frame key both sides agree on; g_frameCounter advances
                // only to feed MergeIntoOutput's zero-frame guard.
                const uint64_t display_time =
                    static_cast<uint64_t>(frameEndInfo->displayTime);
                g_frameCounter.fetch_add(1, std::memory_order_relaxed);
                const uint32_t thread_id = GetCurrentThreadId();
                if (g_gpuTimer) {
                    g_gpuTimer->RecordTimestamp(display_time);
                    // Drain INSIDE the bracket too: the GetData polls then
                    // cancel in the subtraction rather than becoming residual
                    // target_us bias (they would otherwise sit after qpc_exit).
                    DrainGpu();
                }

                const XrResult result =
                    OpenXrApi::xrEndFrame(session, frameEndInfo);

                // ETW emitted INSIDE post's bracket with qpc_entry only.
                // qpc_exit is not known yet (we take it as late as
                // possible, just before Append, so post's TraceLogging
                // cost is itself absorbed by post.bracket). Real exit
                // timestamps land in the CSV; consumers wanting WPA-side
                // delta correlation should join the ETW stream to the
                // CSV by (Side, DisplayTime).
                TraceLoggingWrite(g_traceProvider,
                                  "xrEndFrame",
                                  TLArg(kSideStr, "Side"),
                                  TLArg(display_time, "DisplayTime"),
                                  TLArg(qpc_entry, "QpcEntry"));

                // qpc_exit AS LATE AS POSSIBLE so post.bracket subsumes
                // the bulk of post's work. The only things outside this
                // are the ring.Push below (~20 ns) and the function
                // return (~5 ns).
                const int64_t qpc_exit = Qpc();
                g_csv.Append({display_time, thread_id, qpc_entry, qpc_exit});
                return result;
            }
        }

      private:
        // Drain every GPU timestamp the timer has resolved since the last poll
        // and append each to the GPU CSV sink. m_gpuResolved is reused across
        // frames (cleared, capacity retained) so the render-thread hot path
        // never allocates after warm-up. Self-guards on a null timer so PRE
        // can call it unconditionally.
        void DrainGpu() {
            if (!g_gpuTimer) {
                return;
            }
            m_gpuResolved.clear();
            g_gpuTimer->PollResolved(m_gpuResolved);
            for (const auto& row : m_gpuResolved) {
                g_gpuCsv.Append(row);
            }
        }

        // Reusable scratch buffer for DrainGpu (render thread only).
        std::vector<gpu::GpuRow> m_gpuResolved;
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
