# OpenXR-Layer-Monitor

Measure the per-frame CPU **and GPU** cost an OpenXR API layer adds to your
application, without modifying the target layer. You install two tiny layers
around the one you want to profile, run your app, and get a CSV that tells
you how many microseconds the target layer burned on the frame thread (and
on the GPU command stream) per frame.

```
app -> ..._pre -> <target layer> -> ..._post -> runtime
```

Each side records a `QueryPerformanceCounter` timestamp around its own
`xrEndFrame`. The pre side uses a NARROW bracket (just the call to the
next layer); the post side uses a WIDE bracket that opens at the very
first line of its xrEndFrame and closes just before the lock-free CSV
append. When pre subtracts post's bracket, the runtime + compositor +
**post's own per-frame bookkeeping** all cancel out, leaving only the
target layer's actual work:

```
target_cpu = (pre exit - pre entry) - (post exit - post entry)
```

Residual measurement bias on the post side is about **~25 ns** (the
SPSC ring `Push` + function return that live after `qpc_exit_post`).
For target layers in the µs-to-ms range this is well below the QPC
noise floor; even a sub-µs target layer is mostly faithfully measured.

For **GPU cost** the same idea applies, but the endpoints are single GPU
timestamps in the command stream rather than a pair on each side. Pre
inserts a GPU timestamp just before forwarding to the target; post
inserts one at the very start of its `xrEndFrame` (i.e. right after the
target finished submitting). The merge subtracts:

```
target_gpu_us = (post_gpu_ticks - pre_gpu_ticks) / gpu_freq * 1e6
```

Both **D3D11** and **D3D12** are wired up:

- **D3D11**: timestamps go through `ID3D11DeviceContext::End` on the app's
  immediate context, each wrapped in a tiny `D3D11_QUERY_TIMESTAMP_DISJOINT`
  bracket for the frequency + clock-stability flag. Read back asynchronously
  a few frames later via `GetData(..., D3D11_ASYNC_GETDATA_DONOTFLUSH)`.
- **D3D12**: each side owns a tiny pair (`ID3D12CommandAllocator`,
  `ID3D12GraphicsCommandList`) per ring slot. The command list does just
  `EndQuery(TIMESTAMP)` + `ResolveQueryData` into a per-slot offset in a
  `READBACK` heap. It is submitted on the app's own command queue
  (`ID3D12CommandQueue` pulled from `XrGraphicsBindingD3D12KHR`), gated by
  an `ID3D12Fence`; the readback buffer is `Map`'d once the fence reaches
  the slot's value. No D3D11On12 wrapper, no extra translation layer.

Vulkan / OpenGL hosts get a blank GPU column; the CPU sandwich keeps
working.

### D3D12 `target_gpu_us`: a measured overhead floor (read before trusting it)

The CPU sandwich cancels cleanly on both APIs. The **GPU sandwich does not,
on D3D12** — and this is architectural, not a bug we can patch away.

- **D3D11** puts both timestamps inline in the app's *single immediate-context
  stream*, so `T_pre`, the target's draws, and `T_post` are adjacent in one
  ordered command stream → `T_post − T_pre` ≈ the target's GPU work, floor ≈ 0.
- **D3D12** has no shared stream: pre and post each `ExecuteCommandLists` their
  timestamp *separately*, and the GPU **idles between the two submissions**
  when the target's work is light. That idle inflates `target_gpu_us`.

A swept synthetic load (a layer doing a known amount `K` of GPU work between
pre and post) measured the overhead `O = target_gpu_us − K`:

| target GPU work `K` | sandwich `target_gpu_us` | overhead `O` |
|---|---|---|
| 0 µs   | 289 µs  | **289 µs** |
| 60 µs  | 347 µs  | **287 µs** |
| 281 µs | 572 µs  | **291 µs** |
| 1066 µs| 1353 µs | **287 µs** (bimodal) |
| 3433 µs| 3507 µs | **74 µs** |

So `O` is a near-constant ~290 µs while the target's GPU work is small, then
**gets absorbed** as the work grows (the GPU stops idling). Because it is *not
additive*, it **cannot be removed by subtracting a constant baseline** — doing
so would over-correct heavy targets.

**What this means in practice:**

- ✅ **Heavy targets (> ~1 ms per-frame GPU):** `target_gpu_us` is reliable
  (the ~74–290 µs bias is small in relative terms).
- ⚠️ **Light targets (< ~300 µs per-frame GPU):** the ~290 µs floor dominates;
  treat `target_gpu_us` as an upper bound, not an absolute figure.
- ✅ **CPU (`target_us`) is unaffected** on either API.

**To profile a layer you own**, prefer timing its GPU work **inline in the
layer's own command stream** (one `EndQuery`…work…`EndQuery` pair in the same
command list / immediate context) — the clean, floor-free method that
OpenXR-Toolkit and XrTelemetry use for their own work. The zero-touch GPU
sandwich above is the fallback for profiling a *third-party* target you can't
modify, with the caveat in mind.

## What you can do with it

- Compare two versions of the same layer after an optimization
  ("did my change actually reduce CPU/GPU?").
- Spot frame spikes (p95 / p99 / max) instead of just averages, on
  either timeline.
- Sanity-check that a third-party layer isn't burning more CPU or GPU
  than it claims.
- Establish a CPU+GPU baseline before writing your own layer.

The tool produces CPU + GPU CSVs per process and ETW events. A small
Python script merges the four halves into a single row per frame with
both `target_us` and `target_gpu_us` columns.

## Status

| Capability                       | Status |
| -------------------------------- | ------ |
| CPU sandwich on `xrEndFrame`     | yes |
| GPU sandwich on `xrEndFrame` (D3D11) | yes |
| GPU sandwich on `xrEndFrame` (D3D12) | yes |
| GPU sandwich (Vulkan / OpenGL)   | not yet |
| Other per-frame functions        | not yet (planned: `xrWaitFrame`, `xrBeginFrame`, `xrLocateViews`) |
| ETW emission                     | yes (`TraceLoggingWrite`, capture with `scripts\Tracing.wprp`) |
| CSV per process                  | yes (CPU + GPU, separate files merged on Ctrl+F9 stop) |
| Python analyzer                  | yes (joins GPU automatically when sibling `gpu-*.csv` files exist) |

Built on [`mbucchia/OpenXR-Layer-Template`](https://github.com/mbucchia/OpenXR-Layer-Template).

## Installation

Prerequisites: Windows 10/11 x64, Visual Studio 2019+ with the Desktop C++
workload, Python 3, PowerShell, and the OpenXR submodules:

```powershell
git submodule update --init --recursive
```

### 1. Build both DLLs

```powershell
pwsh scripts\Build-All.ps1                          # Release | x64
pwsh scripts\Build-All.ps1 -Configuration Debug
```

Output lands in `bin\x64\Release\`:

```
XR_APILAYER_MLEDOUR_layer_monitor_pre.dll
XR_APILAYER_MLEDOUR_layer_monitor_pre.json
XR_APILAYER_MLEDOUR_layer_monitor_post.dll
XR_APILAYER_MLEDOUR_layer_monitor_post.json
Install-Layer.ps1
Uninstall-Layer.ps1
```

Prefer Visual Studio? Open `XR_APILAYER_MLEDOUR_layer_monitor_pre.sln`,
then `..._post.sln`, build each as `Release | x64`. Both reference the
same `openxr-api-layer.vcxproj` and share the same OutDir, so building
either order produces the four files above side-by-side.

### 2. Register both layers in HKLM

From an **elevated** PowerShell in the output folder:

```powershell
cd bin\x64\Release
.\Install-Layer.ps1
```

The script writes two values under
`HKLM\Software\Khronos\OpenXR\1\ApiLayers\Implicit` -- one per DLL.

### 3. Place the target layer between pre and post

This is the manual step the sandwich math depends on. The OpenXR loader
walks `ApiLayers\Implicit` in insertion order. For the bracket to isolate
the target layer, the order in that registry key must be:

```
XR_APILAYER_MLEDOUR_layer_monitor_pre.json
<target_layer>.json
XR_APILAYER_MLEDOUR_layer_monitor_post.json
```

Check the current order:

```powershell
reg query "HKLM\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
```

If the target is not sandwiched, delete then recreate the values in the
right order (the loader respects insertion order). A friendlier GUI for
this is [OpenXR Explorer](https://github.com/maluoi/openxr-explorer).

### Uninstall

```powershell
.\Uninstall-Layer.ps1
```

or remove the two HKLM values manually.

## Usage

### A first measurement

1. Launch your OpenXR app normally. The two layers attach automatically
   but stay **idle** until you press the hotkey. While idle, each
   xrEndFrame pays for ~three `GetAsyncKeyState` polls (Ctrl, F9, RAlt
   for the AltGr mask) plus one atomic exchange on the pre side, or
   an acquire-load on shared memory plus a local comparison on the
   post side. Sub-microsecond on modern x86, comfortably below the
   QPC noise floor.
2. Press **Ctrl+F9** (system-wide hotkey) to start monitoring. The pre
   DLL detects the keypress and broadcasts the new state to the post
   DLL through a small shared-memory segment; both halves enter
   recording on the same xrEndFrame call. You will see a
   "Monitoring STARTED" line in each side's `.log`.

   The hotkey is **system-wide**, not gated by the host being the
   foreground window. This is intentional: under many OpenXR runtimes
   (Pimax OpenXR, SteamVR direct mode, headless samples like
   hello_xr) the host process never owns the foreground window while
   the HMD is mounted -- a foreground-only check made the layer
   impossible to drive on those hosts.

   Side effects to know:

   - A Ctrl+F9 binding in another app (Discord screenshot, screen
     recorder, IDE debugger) will also toggle the recording. Press
     Ctrl+F9 again to undo. **AltGr+F9** is explicitly NOT a toggle
     (AltGr's synthetic LCtrl is masked) so AZERTY / QWERTZ users can
     keep using their debugger.
   - Successive toggles are debounced to ~500 ms apart so a rapid-fire
     binding (OBS / ShadowPlay instant replay) cannot start+stop the
     recording in two consecutive frames.
   - **Each OpenXR process polls independently.** If you have a
     second OpenXR process running concurrently (a debug tool, a
     parallel test app, OpenXR Tools for WMR) it will receive the
     same Ctrl+F9 and start its own recording. Stop it manually or
     set `DISABLE_XR_APILAYER_MLEDOUR_layer_monitor_pre=1` /
     `..._post=1` in that process's environment to keep it out.
   - `DISABLE_*` env vars remain the permanent-conflict escape hatch
     if a host app's own Ctrl+F9 binding cannot be rebound.
3. Let the app run for **at least 30 seconds** of representative
   gameplay -- you want a steady-state sample, not the loading screen.
4. Press **Ctrl+F9 again** to stop. The post DLL immediately drains both
   writers and produces `frames-merged-<pid>.csv` while the app is still
   running. You can press Ctrl+F9 a third time to start a fresh session
   (the previous merge is overwritten).
5. If you forget step 4 and just quit the app: the auto-merge falls back
   to `xrDestroyInstance`, so you still get the merged CSV. If the
   process is killed hard (Task Manager / crash) you only get the
   per-side CSVs and need `analyze.py` to merge them manually.
6. Open the merged CSV. It lives in:

   ```
   %LOCALAPPDATA%\XR_APILAYER_MLEDOUR_layer_monitor\frames-merged-<pid>.csv
   ```

   The top **fourteen** lines are a session-wide summary (also greppable
   from the shell):

   ```
   # frame_count=1842
   # target_cpu_ms_mean=0.0123
   # target_cpu_ms_min=0.0001
   # target_cpu_ms_max=0.3417
   # target_cpu_pct_mean=0.1110%
   # target_cpu_pct_min=0.0030%
   # target_cpu_pct_max=3.0780%
   # gpu_frame_count=1839
   # target_gpu_ms_mean=0.0840
   # target_gpu_ms_min=0.0210
   # target_gpu_ms_max=2.1500
   # target_gpu_pct_mean=0.7560%
   # target_gpu_pct_min=0.1890%
   # target_gpu_pct_max=19.3500%
   ```

   `frame_count` is the number of matched frames between pre and post.
   `target_cpu_ms_*` is the CPU cost the target layer added per frame, in
   milliseconds. `target_cpu_pct_*` is that cost expressed as a percentage
   of the host's frame interval. The min / max bound the variability:
   useful for spotting bursty or sparse layers where the mean dilutes
   the actual per-call cost. The `*_pct_*` lines aggregate only over frames
   that have a successor (every frame except the last per thread).

   The seven GPU lines aggregate the GPU sandwich: `gpu_frame_count` plus
   three `target_gpu_ms_*` and three `target_gpu_pct_*`. `gpu_frame_count`
   is independent of `frame_count`: it only counts frames whose timestamp
   resolved successfully on BOTH sides (no disjoint clock, matching
   frequency, monotonic delta); the `pct_*` subset also needs an interval.
   On a non-D3D11 host the lines are still present with zero counts and
   zeros -- the schema stays uniform so a single parser handles every
   session. (See the D3D12 floor caveat above before trusting GPU numbers
   for a light target.)

   **`target_cpu_ms_min` can be negative.** `target_us = pre_us - post_us`
   and QPC jitter occasionally pushes the post-side bracket slightly
   above the pre-side one, especially for layers whose own cost is
   below the QPC noise floor (~hundreds of nanoseconds). The data is
   real -- not clamped -- and the post DLL writes a count of
   negative-`target_us` frames to its `.log` file when this happens.
   For a layer with single-digit-µs mean cost, a small negative min
   is normal noise; a large negative min hints at counter desync or a
   broken layer chain.

   Below the summary, one row per frame with these columns:

   | column                  | meaning |
   | ----------------------- | ------- |
   | `display_time`          | frame's predicted display time (`XrTime`); the key matched between pre and post |
   | `thread_id`             | thread that called `xrEndFrame` |
   | `frame_interval_us`     | wall-clock between this and the next frame's pre-entry (blank on the last row) |
   | `pre_us`                | pre-side bracket = target + post + runtime |
   | `post_us`               | post-side bracket = runtime |
   | `target_us`             | `pre_us − post_us` -- the target layer's CPU cost |
   | `target_cpu_pct_of_frame` | `target_us / frame_interval_us * 100` (blank on the last row) |
   | `target_gpu_us`         | `(post_gpu_ticks − pre_gpu_ticks) / gpu_freq * 1e6` (blank when no valid GPU sample on both sides, non-D3D11 host, frequency mismatch, or backwards counter) |
   | `target_gpu_pct_of_frame` | `target_gpu_us / frame_interval_us * 100` (blank when `target_gpu_us` is blank, or on the last row) |

   Drop it into a spreadsheet / pandas / your plotting tool of choice.
   The `target_cpu_pct_of_frame` column answers "what slice of the host's
   frame budget did this layer's CPU work eat" directly; useful for triaging
   spikes (filter `target_cpu_pct_of_frame > 1.0` to find frames where the
   layer ate more than 1 % of the budget). `target_gpu_pct_of_frame` does the
   same for its GPU work.

7. **For stats at the command line**, run `scripts\analyze.py` against
   the per-side CSVs (the script and the in-DLL merge produce the same
   `frames-merged-<pid>.csv`; the script additionally prints mean /
   median / p95 / p99 / max to stdout):

   ```powershell
   $dir = "$env:LOCALAPPDATA\XR_APILAYER_MLEDOUR_layer_monitor"
   python scripts\analyze.py "$dir\frames-<pid>-pre.csv" "$dir\frames-<pid>-post.csv"
   ```

   Example output (D3D11 host -- on a non-D3D11 host the `target_gpu_us`
   block reads `(no samples)`):

   ```
   matched frames: 1842

   target_cpu_us      (microseconds, CPU):
       count  1842
       mean      12.34
       median     8.10
       p95       45.20
       p99      128.40
       min        0.30
       max      341.70

   target_cpu_pct     (% of frame interval):
       count  1841
       mean      0.111
       median    0.073
       p95       0.407
       p99       1.156
       min       0.003
       max       3.078

   target_gpu_us      (microseconds, GPU):
       count  1839
       mean      84.00
       median    73.20
       p95      198.40
       p99      512.30
       min       21.00
       max     2150.00

   target_gpu_pct     (% of frame interval):
       count  1839
       mean      0.756
       median    0.659
       p95       1.787
       p99       4.616
       min       0.189
       max      19.350

   frame_interval_us median: 11100.00  (~90.1 Hz)
   wrote frames-merged.csv
   ```

   `--gpu-pre` / `--gpu-post` override the sibling-discovery default if
   you copy the CSVs to a different directory before analyzing.

### Real-time tracing (ETW)

If you want to watch the layer while it runs, capture ETW:

```powershell
wpr -start scripts\Tracing.wprp
# launch your app, play, exit
wpr -stop monitor.etl
```

Open `monitor.etl` in Windows Performance Analyzer. Each `xrEndFrame`
event carries `Side` (`pre` / `post`), `FrameIdx`, `QpcEntry`, `QpcExit`,
and `QpcDelta`.

### Disable without uninstalling

The standard OpenXR loader escape hatch works for both halves:

```cmd
set DISABLE_XR_APILAYER_MLEDOUR_layer_monitor_pre=1
set DISABLE_XR_APILAYER_MLEDOUR_layer_monitor_post=1
```

Set these in the environment of the host process before launching it.
Both must be set; with only one disabled the chain is broken and you'd
get garbage timing data (or no data at all).

## Where the output goes

Both DLLs share a single `%LOCALAPPDATA%` folder, created on first
`xrCreateInstance`. The framework's `.log` files keep their full
per-side names so they coexist there; the per-frame CSVs carry a
`-pre` / `-post` suffix:

```
%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_layer_monitor\
    XR_APILAYER_MLEDOUR_layer_monitor_pre.log     init log (app name, runtime, QPC freq, GPU active y/n)
    XR_APILAYER_MLEDOUR_layer_monitor_post.log
    frames-<pid>-pre.csv                           CPU bracket per xrEndFrame call (truncated on each Ctrl+F9 start)
    frames-<pid>-post.csv
    gpu-<pid>-pre.csv                              GPU timestamp per xrEndFrame call -- D3D11 OR D3D12 hosts; absent on Vulkan / OpenGL
    gpu-<pid>-post.csv
    frames-merged-<pid>.csv                        auto-written by post on Ctrl+F9 stop or xrDestroyInstance fallback
```

CPU CSV format (header rows start with `#`):

```
# qpc_freq=10000000
# side=pre
# layer=XR_APILAYER_MLEDOUR_layer_monitor_pre
# fn=xrEndFrame
display_time,thread_id,qpc_entry,qpc_exit
33724160000000,12345,17834950123456,17834950456789
33724171111111,12345,17834951678901,17834951901234
...
```

GPU CSV format. `# gpu_clock=` carries the active backend name (either
`d3d11` or `d3d12`). `gpu_freq` is per row -- D3D11 reports the clock
rate on each disjoint query (it can change between frames on power-state
transitions), D3D12 fixes it once at queue creation and reports the same
value on every row. `valid=0` means the sample is unusable for the merge:
on D3D11 the disjoint query reported Disjoint or a zero frequency at the
moment of the timestamp; on D3D12 the `Map` of the readback buffer
failed (no per-query disjoint flag exists on D3D12 -- the queue's
`GetTimestampFrequency()` is trusted for the lifetime of the queue,
matching what OpenXR Toolkit and fpsVR do).

```
# gpu_clock=d3d11
# side=pre
# layer=XR_APILAYER_MLEDOUR_layer_monitor_pre
# fn=xrEndFrame
display_time,gpu_ticks,gpu_freq,valid
33724160000000,1234567890,12000000,1
33724171111111,1234580000,12000000,1
...
```

The `.log` files are intentionally quiet -- they capture init metadata
and nothing per-frame. (The framework's logger takes a kernel-wide named
mutex that can stall some compositors; per-frame info goes through ETW
and the async CSV writer instead.)

The per-side CSV is truncated when the writer thread starts, i.e. on
each Ctrl+F9 press that switches monitoring ON. The PID in the
filename lets concurrent OpenXR processes coexist; the side suffix
(`-pre` / `-post`) keeps the two halves distinct inside the shared
folder.

The first frame after a Ctrl+F9 start is intentionally **not**
recorded -- it pays for the writer-thread spawn cost on the post
side, which would otherwise land inside the pre bracket and inflate
`target_us` for frame 0. The first row in the CSV is therefore the
second-frame-after-start. The frame of the Ctrl+F9 **stop** press is
also not recorded (the writer is already shut down by the time the
record path would have appended), so the CSV row count is the number
of frames strictly between start and stop.

The merged CSV has a different schema -- eleven `#` comment lines at
the top with the session summary (seven CPU + four GPU), then the
column header, then one row per matched frame:

```
# frame_count=<int>
# target_cpu_ms_mean=<float>         ms
# target_cpu_ms_min=<float>          ms (may be negative -- see below)
# target_cpu_ms_max=<float>          ms
# target_cpu_pct_mean=<float>%       percentage of frame interval
# target_cpu_pct_min=<float>%        percentage of frame interval
# target_cpu_pct_max=<float>%        percentage of frame interval
# gpu_frame_count=<int>              frames with a valid GPU sample on both sides
# target_gpu_ms_mean=<float>         ms (over the gpu_frame_count subset only)
# target_gpu_ms_min=<float>          ms
# target_gpu_ms_max=<float>          ms
# target_gpu_pct_mean=<float>%       percentage of frame interval
# target_gpu_pct_min=<float>%        percentage of frame interval
# target_gpu_pct_max=<float>%        percentage of frame interval
display_time,thread_id,frame_interval_us,pre_us,post_us,target_us,target_cpu_pct_of_frame,target_gpu_us,target_gpu_pct_of_frame
<int>,<int>,<float>,<float>,<float>,<float>,<float>,<float>,<float>
...
```

All `#`-prefixed values are bare numbers (no unit) except the
`*_pct_*` lines which carry a literal `%` after the value for
visual clarity. The seven GPU stat lines are zero (and the
`target_gpu_us` / `target_gpu_pct_of_frame` columns are blank on every
row) when neither a D3D11 nor a D3D12 graphics binding was found in
`xrCreateSession` (Vulkan / OpenGL hosts) -- the schema stays the same
so a single parser handles every session. All line endings are LF (the C++ writer opens the file
in binary mode, the Python writer uses `lineterminator='\n'`).

## How it works

The monitor is two separate DLLs built from one source tree -- not one
DLL registered twice. The framework keeps a singleton `g_instance` per
DLL, so a single DLL loaded into two chain slots would have each slot
stomp the other's dispatch table. Splitting into `_pre.dll` and
`_post.dll` keeps state isolated.

Two `.sln` files at the repo root drive the split. Each one sets
`$(SolutionName)` to `..._pre` or `..._post`, which in turn:

- names the output DLL (`$(TargetName) = $(SolutionName)`)
- sets the `LAYER_NAME` preprocessor define
- selects which JSON manifest the post-build event consumes

`layer.cpp` detects which side it is via
`constexpr EndsWith(LAYER_NAME, "_post")`. No runtime branching, no
shared state -- each binary is self-contained.

The frame thread's only work inside `xrEndFrame` is:

1. Two `QueryPerformanceCounter` calls (~20 ns each).
2. One `TraceLoggingWrite` ETW event (~50 ns).
3. A brief `std::mutex` + push of a 32-byte POD onto a queue (~100 ns).

A dedicated writer thread drains the queue and writes the CSV. **No disk
I/O on the frame thread.** Synchronous I/O would inflate the pre-side
bracket by however long the post-side's write took, and the post-side
write happens *inside* the pre-side bracket, so its cost would land on
the target layer's score.

## Limitations

- **Frame thread only.** Off-thread CPU work spawned by the target layer
  (worker threads, fire-and-forget compute) is invisible to the sandwich
  -- it only sees what the layer does synchronously inside its
  `xrEndFrame` override.
- **GPU sandwich runs on D3D11 and D3D12; Vulkan / OpenGL hosts get
  blank `target_gpu_us`** (the CPU sandwich still works).
  Common to both backends:
    - Captures only work submitted **in command-stream order between
      the two timestamps** on the app's render queue (D3D12) or
      immediate context (D3D11). A target that renders on a deferred
      context, a private device, or a separate queue is invisible to
      the GPU measurement.
    - Each GPU sample is read back ~3 frames late through an async
      readback ring, so the last few frames of a session can be
      missing GPU rows -- the merge tolerates the gap (CPU row
      present, `target_gpu_us` blank).
    - The merge guards both sides at join time: `pre.gpu_freq ==
      post.gpu_freq`, `post_ticks >= pre_ticks`, and `valid != 0` on
      both rows. A frame failing any check is blanked rather than
      contributing a noisy value.
  Backend-specific caveats:
    - **D3D11**: the per-side disjoint query covers only the *instant*
      of each timestamp, so a GPU clock change happening between
      `T_pre` and `T_post` may go unflagged by the Disjoint bit. A
      frequency mismatch IS caught (and blanks the frame); a Disjoint
      flag on either side blanks the frame. Both events are rare.
    - **D3D12**: no per-query disjoint flag exists; the queue's
      `GetTimestampFrequency()` is trusted for the queue's lifetime
      (same convention OpenXR Toolkit / fpsVR / XrTelemetry use).
      `valid=1` on a D3D12 row only means the readback `Map` succeeded,
      *not* that the GPU clock was stable across the measured span.
      A driver TDR (device removed) between `Signal` and `Map` can in
      principle leave a row with `valid=1` holding the stale contents
      of the readback buffer; the merge's `post_ticks >= pre_ticks`
      and frequency-match guards catch the common shapes of corruption
      but a coincidental match-after-corruption would not be detected.
      Re-run any session that crossed a driver restart / TDR.
      Each frame's GPU instrumentation adds one tiny
      `ExecuteCommandLists` on the app's queue (an `EndQuery` + a
      `ResolveQueryData` of a single `UINT64`); CPU cost is a single-
      digit-microsecond syscall sequence, kept OUT of the pre-side
      bracket so it does not bias `target_us`. If the GPU stalls
      beyond the ring's depth (~44 ms at 90 Hz), the D3D12 backend
      SKIPS the frame's GPU sample rather than crash on a forbidden
      command-allocator reset, and bumps `# session_end
      gpu_ring_overflow` in the GPU CSV footer to surface it.
- **Layer ordering is on you.** If a fourth layer slips between pre and
  the target (or between target and post), its CPU gets billed to the
  target. Always sanity-check `reg query` after install.
- **The post-side writer is invisible to the math but visible on the
  wall clock.** Frame budget impact at 90 Hz with a ~256-entry queue is
  below 0.1 % in practice; it does not bias `target_us`.
- **Per-session CSV.** The per-side CSVs are truncated each time you
  press Ctrl+F9 to start a new monitoring session. The previous merged
  CSV is overwritten on the next Ctrl+F9 stop (or at xrDestroyInstance).
  If you want to keep an older session, move the file out before the
  next start.
- **Auto-merge requires a clean stop.** The merged CSV is written when
  the user presses Ctrl+F9 to stop or when xrDestroyInstance fires
  while still monitoring. If the host process is killed mid-session
  (Task Manager, crash, debugger detach) the per-side CSVs are still
  on disk (the writer flushes periodically), but no merged file is
  produced -- run `analyze.py` against the per-side files in that case.
- **Stop press stalls the frame thread for the merge.** Pressing
  Ctrl+F9 to stop joins both writer threads and runs MergeIntoOutput
  synchronously inside `xrEndFrame` -- typically 10-50 ms for a
  30-second session at 90 Hz. The user sees a 1-4 frame stutter on
  the stop press. Acceptable for a user-initiated action; running
  the merge on a background thread was rejected because the host
  often exits within milliseconds of the stop and the file would be
  lost.
- **Ctrl+F9 is a system-wide hotkey.** Polled via `GetAsyncKeyState`
  from inside xrEndFrame; no foreground-window gating (that gating
  used to exist but broke every runtime where the compositor /
  device-direct window holds the foreground -- Pimax OpenXR, SteamVR
  direct mode, hello_xr). Three mitigations are in place:

  - **AltGr+F9** is masked (AltGr's synthetic LCtrl is detected via
    VK_RMENU). AZERTY / QWERTZ / Nordic users keep their debugger
    run key without toggling the recording.
  - **500 ms debounce** between successful toggles. A rapid-fire
    binding (ShadowPlay / OBS instant replay sending Ctrl+F9 twice
    in tens of ms) cannot churn the recording.
  - **`DISABLE_XR_APILAYER_MLEDOUR_layer_monitor_pre=1` /
    `..._post=1`** env vars bypass the layer entirely for a target
    process if a permanent conflict appears.

  Residual surface: an unrelated Ctrl+F9 in another app (Discord
  screenshot, screen recorder, IDE shortcut) will still fire the
  toggle. Two-tier protection:

  - A parasitic toggle ON-then-OFF that records **zero frames** (the
    user catches it within ~one xrEndFrame) is **safe**. The writer
    thread opens its CSV lazily on first row, and `MergeIntoOutput`
    bails on `g_frameCounter == 0`. Both the previous session's
    per-side CSVs and the previous `frames-merged-<pid>.csv` survive.
  - A parasitic toggle that records **N ≥ 1 frames** before the user
    cancels DOES overwrite the previous session's per-side CSVs (on
    the first `Append`) and replaces the previous merged CSV (on
    the cancel-toggle). To recover any earlier session, **move
    important `frames-*-<pid>-*.csv` and `frames-merged-<pid>.csv`
    out of `%LOCALAPPDATA%` before leaving the host process running
    in the background.** Per-session filenames (timestamps) are
    a planned follow-up that would eliminate this case entirely.
- **Multi-process Ctrl+F9 bleed.** Each running OpenXR process polls
  Ctrl+F9 independently. A single press toggles every active host
  that has the layer loaded. If you run a second OpenXR process for
  debug (OpenXR Tools, hello_xr, a unit-test harness), set
  `DISABLE_XR_APILAYER_MLEDOUR_layer_monitor_pre=1` /
  `..._post=1` in its environment to keep it out of the global
  toggle. Rebinding the hotkey itself (away from Ctrl+F9) requires
  a code change today.
- **64-bit only in the released CI artifacts.** A 32-bit target exists
  in the .vcxproj but is not currently tested.

## License

MIT -- see [`LICENSE`](LICENSE). Original template by Matthieu Bucchianeri,
also MIT.
