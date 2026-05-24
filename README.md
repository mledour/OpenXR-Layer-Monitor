# OpenXR-Layer-Monitor

Measure the per-frame CPU cost an OpenXR API layer adds to your application,
without modifying the target layer. You install two tiny layers around the
one you want to profile, run your app, and get a CSV that tells you how
many microseconds the target layer burned on the frame thread, per frame.

```
app -> ..._pre -> <target layer> -> ..._post -> runtime
```

Each side records a `QueryPerformanceCounter` timestamp on entry to and
exit from its own `xrEndFrame`. The runtime + compositor live below both
brackets and cancel out in the subtraction; what's left is the target's
own work:

```
target_cpu = (pre exit - pre entry) - (post exit - post entry)
```

## What you can do with it

- Compare two versions of the same layer after an optimization
  ("did my change actually reduce CPU?").
- Spot frame spikes (p95 / p99 / max) instead of just averages.
- Sanity-check that a third-party layer isn't burning more CPU than it claims.
- Establish a baseline before writing your own layer.

The tool produces a CSV per process and ETW events. A small Python script
merges the two halves into a single `target_us` column.

## Status

| Capability                       | Status |
| -------------------------------- | ------ |
| CPU sandwich on `xrEndFrame`     | yes |
| Other per-frame functions        | not yet (planned: `xrWaitFrame`, `xrBeginFrame`, `xrLocateViews`) |
| GPU timestamps (D3D11 / D3D12)   | not yet |
| ETW emission                     | yes (`TraceLoggingWrite`, capture with `scripts\Tracing.wprp`) |
| CSV per process                  | yes |
| Python analyzer                  | yes |

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
   but stay **idle** until you press the hotkey -- they add zero overhead
   to xrEndFrame in that state.
2. Inside the app, press **Ctrl+F9** to start monitoring. The two DLLs
   detect the keypress on their next xrEndFrame poll (~one frame of
   latency) and begin recording. You will see a
   "Monitoring STARTED" line in each side's `.log`.
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

   The top seven lines are a session-wide summary (also greppable from
   the shell):

   ```
   # frame_count=1842
   # target_ms_mean=0.0123
   # target_ms_min=0.0001
   # target_ms_max=0.3417
   # target_pct_mean=0.1110%
   # target_pct_min=0.0030%
   # target_pct_max=3.0780%
   ```

   `frame_count` is the number of matched frames between pre and post.
   `target_ms_*` is the CPU cost the target layer added per frame, in
   milliseconds. `target_pct_*` is that cost expressed as a percentage
   of the host's frame interval. The min / max bound the variability:
   useful for spotting bursty or sparse layers where the mean dilutes
   the actual per-call cost. `target_pct_*` aggregates only over frames
   that have a successor (every frame except the last per thread).

   **`target_ms_min` can be negative.** `target_us = pre_us - post_us`
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
   | `frame_idx`             | matched index between pre and post |
   | `thread_id`             | thread that called `xrEndFrame` |
   | `frame_interval_us`     | wall-clock between this and the next frame's pre-entry (blank on the last row) |
   | `pre_us`                | pre-side bracket = target + post + runtime |
   | `post_us`               | post-side bracket = runtime |
   | `target_us`             | `pre_us − post_us` -- the target layer's CPU cost |
   | `target_pct_of_frame`   | `target_us / frame_interval_us * 100` (blank on the last row) |

   Drop it into a spreadsheet / pandas / your plotting tool of choice.
   The `target_pct_of_frame` column answers "what slice of the host's
   frame budget did this layer eat" directly; useful for triaging
   spikes (filter `target_pct_of_frame > 1.0` to find frames where the
   layer ate more than 1 % of the budget).

7. **For stats at the command line**, run `scripts\analyze.py` against
   the per-side CSVs (the script and the in-DLL merge produce the same
   `frames-merged-<pid>.csv`; the script additionally prints mean /
   median / p95 / p99 / max to stdout):

   ```powershell
   $dir = "$env:LOCALAPPDATA\XR_APILAYER_MLEDOUR_layer_monitor"
   python scripts\analyze.py "$dir\frames-<pid>-pre.csv" "$dir\frames-<pid>-post.csv"
   ```

   Example output:

   ```
   matched frames: 1842

   target_us          (microseconds):
       count  1842
       mean      12.34
       median     8.10
       p95       45.20
       p99      128.40
       min        0.30
       max      341.70

   target_pct_of_frame (% of frame interval):
       count  1841
       mean      0.111
       median    0.073
       p95       0.407
       p99       1.156
       min       0.003
       max       3.078

   frame_interval_us median: 11100.00  (~90.1 Hz)
   wrote frames-merged.csv
   ```

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
    XR_APILAYER_MLEDOUR_layer_monitor_pre.log     init log (app name, runtime, QPC freq)
    XR_APILAYER_MLEDOUR_layer_monitor_post.log
    frames-<pid>-pre.csv                           one row per xrEndFrame call (truncated on each Ctrl+F9 start)
    frames-<pid>-post.csv
    frames-merged-<pid>.csv                        auto-written by post on Ctrl+F9 stop or xrDestroyInstance fallback
```

CSV format (header rows start with `#`):

```
# qpc_freq=10000000
# side=pre
# layer=XR_APILAYER_MLEDOUR_layer_monitor_pre
# fn=xrEndFrame
frame_idx,thread_id,qpc_entry,qpc_exit
0,12345,17834950123456,17834950456789
1,12345,17834951678901,17834951901234
...
```

The `.log` files are intentionally quiet -- they capture init metadata
and nothing per-frame. (The framework's logger takes a kernel-wide named
mutex that can stall some compositors; per-frame info goes through ETW
and the async CSV writer instead.)

The per-side CSV is truncated when the writer thread starts, i.e. at
every `xrCreateInstance`. The PID in the filename lets concurrent
OpenXR processes coexist.

The merged CSV has a different schema -- seven `#` comment lines at
the top with the session summary, then the column header, then one
row per matched frame:

```
# frame_count=<int>
# target_ms_mean=<float>             ms
# target_ms_min=<float>              ms (may be negative -- see below)
# target_ms_max=<float>              ms
# target_pct_mean=<float>%           percentage of frame interval
# target_pct_min=<float>%            percentage of frame interval
# target_pct_max=<float>%            percentage of frame interval
frame_idx,thread_id,frame_interval_us,pre_us,post_us,target_us,target_pct_of_frame
<int>,<int>,<float>,<float>,<float>,<float>,<float>
...
```

All `#`-prefixed values are bare numbers (no unit) except
`target_pct_*` which carries a literal `%` after the value for
visual clarity. All line endings are LF (the C++ writer opens the
file in binary mode, the Python writer uses `lineterminator='\n'`).

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

- **CPU only, frame thread only.** Off-thread work spawned by the target
  layer (worker threads, fire-and-forget compute) is invisible to the
  sandwich -- it only sees what the layer does synchronously inside its
  `xrEndFrame` override.
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
- **Hotkey conflict.** The hotkey is Ctrl+F9, polled via
  `GetAsyncKeyState` from inside xrEndFrame. If the host app binds
  Ctrl+F9 to something else, both will fire. Rebinding requires a
  code change today.
- **64-bit only in the released CI artifacts.** A 32-bit target exists
  in the .vcxproj but is not currently tested.

## License

MIT -- see [`LICENSE`](LICENSE). Original template by Matthieu Bucchianeri,
also MIT.
