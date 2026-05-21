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

1. Launch your OpenXR app normally. The two layers attach automatically.
2. Use the app for **at least 30 seconds**. The first frames are noisy
   from startup; you want a steady-state sample.
3. Close the app cleanly (don't kill the process -- a clean shutdown
   flushes the CSV write queue).
4. Find the process ID your app ran under (Task Manager / Process
   Explorer / a saved log file). Each run produces a `frames-<pid>.csv`.
5. Run the analyzer:

```powershell
python scripts\analyze.py `
  "$env:LOCALAPPDATA\XR_APILAYER_MLEDOUR_layer_monitor_pre\frames-<pid>.csv" `
  "$env:LOCALAPPDATA\XR_APILAYER_MLEDOUR_layer_monitor_post\frames-<pid>.csv"
```

Example output:

```
matched frames:   1842
target_us mean:     12.34
target_us median:    8.10
target_us p95:      45.20
target_us p99:     128.40
target_us max:     341.70
target_us min:      0.30
wrote target_cpu.csv
```

`target_cpu.csv` has one row per frame with `pre_us`, `post_us`, and
`target_us`. Drop it into a spreadsheet / pandas / your plotting tool of
choice for distribution charts.

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

Each DLL writes into its own `%LOCALAPPDATA%` folder, created on first
`xrCreateInstance`:

```
%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_layer_monitor_pre\
    XR_APILAYER_MLEDOUR_layer_monitor_pre.log    init log (app name, runtime, QPC freq)
    frames-<pid>.csv                              one row per xrEndFrame call

%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_layer_monitor_post\
    XR_APILAYER_MLEDOUR_layer_monitor_post.log
    frames-<pid>.csv
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

The CSV is truncated when the writer thread starts, i.e. at every
`xrCreateInstance`. The PID in the filename lets concurrent OpenXR
processes coexist.

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
- **Process-lifetime CSV.** Each `xrCreateInstance` truncates the CSV.
  Probe-then-real init flows (OpenComposite, OXR-Toolkit) will overwrite
  the probe's data.
- **64-bit only in the released CI artifacts.** A 32-bit target exists
  in the .vcxproj but is not currently tested.

## License

MIT -- see [`LICENSE`](LICENSE). Original template by Matthieu Bucchianeri,
also MIT.
