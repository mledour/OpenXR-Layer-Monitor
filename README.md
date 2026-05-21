# OpenXR-Layer-Monitor

Sandwich CPU profiler for OpenXR API layers. Drops two thin layers around a
target layer in the OpenXR loader chain and measures how much per-frame CPU
time the target consumes.

```
app -> XR_APILAYER_MLEDOUR_layer_monitor_pre
    -> <target layer>
    -> XR_APILAYER_MLEDOUR_layer_monitor_post
    -> runtime
```

Each side records a QPC timestamp on entry to and exit from its own
`xrEndFrame` override. Subtracting the post-side bracket from the pre-side
bracket isolates the target layer's CPU cost on the frame thread:

```
pre.bracket  = entry..exit  =  target_work + post_overhead + runtime
post.bracket = entry..exit  =  runtime
target_cpu   = pre.bracket - post.bracket
```

Built on [`mbucchia/OpenXR-Layer-Template`](https://github.com/mbucchia/OpenXR-Layer-Template).

## Status

| Capability                 | Status |
| -------------------------- | ------ |
| CPU sandwich on xrEndFrame | yes (MVP) |
| Other per-frame functions  | not yet (xrWaitFrame / xrBeginFrame / xrLocateViews / xrSyncActions) |
| GPU timestamps (D3D11/12)  | not yet |
| ETW emission               | yes (TraceLoggingWrite, see `scripts/Tracing.wprp`) |
| CSV per process            | yes (`%LOCALAPPDATA%\<LayerName>\frames-<pid>.csv`) |

## How it works

The two layers are **two separate DLLs built from the same source**. They are
not one DLL registered twice: the framework keeps a singleton `g_instance` per
DLL, so a single DLL registered as two chain slots would have its own dispatch
table stomp itself. Splitting into `_pre.dll` + `_post.dll` keeps each side's
state isolated.

Source code knows which side it is via a `constexpr` check on `LAYER_NAME`
(suffix `_post` -> post side, anything else -> pre side). `LAYER_NAME` is set
to `$(SolutionName)` at compile time, so the two `.sln` files at the repo
root drive the side selection without any code edits.

The frame thread's only work inside `xrEndFrame` is:
1. Two `QueryPerformanceCounter` calls (~20 ns each).
2. `TraceLoggingWrite` ETW event (~50 ns).
3. Brief mutex + push of a 32-byte POD to a queue (~100 ns).

A dedicated background thread drains the queue and writes the CSV. **No disk
I/O on the frame thread**, because synchronous I/O inside the pre-side
bracket would inflate `target_cpu` by the post-side's CSV write time. See
`framework/log.h`'s DBWinMutex warning for why the framework's `Log()` is
also banned from the hot path.

## Build

Prerequisites: Visual Studio 2019+ with the Desktop C++ workload, Python 3,
PowerShell, and the OpenXR submodules:

```powershell
git submodule update --init --recursive
```

Build both halves in one go:

```powershell
pwsh scripts\Build-All.ps1                          # Release|x64
pwsh scripts\Build-All.ps1 -Configuration Debug
pwsh scripts\Build-All.ps1 -Platform Win32          # 32-bit pair
```

Or build each `.sln` independently from Visual Studio:

- `XR_APILAYER_MLEDOUR_layer_monitor_pre.sln`
- `XR_APILAYER_MLEDOUR_layer_monitor_post.sln`

Both solutions reference the same `openxr-api-layer/openxr-api-layer.vcxproj`
and share the same `bin\<Platform>\<Configuration>\` output folder, so after
building both you get four files side-by-side:

```
XR_APILAYER_MLEDOUR_layer_monitor_pre.dll
XR_APILAYER_MLEDOUR_layer_monitor_pre.json
XR_APILAYER_MLEDOUR_layer_monitor_post.dll
XR_APILAYER_MLEDOUR_layer_monitor_post.json
```

## Install

The post-build event copies `Install-Layer.ps1` and `Uninstall-Layer.ps1`
into the output folder. Run them from there:

```powershell
cd bin\x64\Release
.\Install-Layer.ps1
```

The script registers both pre and post manifests under
`HKLM\Software\Khronos\OpenXR\1\ApiLayers\Implicit`. The 32-bit equivalents
register under `HKLM\Software\WOW6432Node\...`.

## Positioning the target layer

The OpenXR loader walks `HKLM\Software\Khronos\OpenXR\1\ApiLayers\Implicit`
in insertion order. To measure a third-party layer, you need:

```
ApiLayers\Implicit:
  XR_APILAYER_MLEDOUR_layer_monitor_pre.json
  <target_layer>.json
  XR_APILAYER_MLEDOUR_layer_monitor_post.json
```

If `<target_layer>` is registered between pre and post in HKLM ordering, the
sandwich captures it correctly. If anything else sits in between, the
attribution becomes "everything between pre and post" rather than the target
alone.

To verify the order, use `reg query HKLM\Software\Khronos\OpenXR\1\ApiLayers\Implicit`
or the OpenXR Explorer tool.

## Output

Each side writes one CSV per host process to its own folder:

```
%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_layer_monitor_pre\frames-<pid>.csv
%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_layer_monitor_post\frames-<pid>.csv
```

Format (header lines start with `#`):

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

`frame_idx` matches between pre and post by call order. Merge with the
analyzer script:

```powershell
python scripts\analyze.py %LOCALAPPDATA%\XR_APILAYER_MLEDOUR_layer_monitor_pre\frames-<pid>.csv `
                          %LOCALAPPDATA%\XR_APILAYER_MLEDOUR_layer_monitor_post\frames-<pid>.csv
```

This prints summary stats (median, p95, p99, max in microseconds) and writes
a merged `target_cpu.csv` with a `target_us` column.

For continuous capture, the ETW provider is the same GUID for both sides;
events carry a `Side` field. Use `wpr -start scripts\Tracing.wprp` and
`wpr -stop monitor.etl`, then load `.etl` in WPA.

## Caveats

- **CPU only, on the frame thread only.** Off-thread work that the target
  layer kicks off (e.g. a worker thread, a fire-and-forget compute pass) is
  not captured. A naive sandwich cannot see what the layer doesn't do
  synchronously inside its xrEndFrame override.
- **The CSV write overhead in the post side is invisible to the math** but
  shows up as wall-clock drift on the host process. Frame-budget impact at
  90 Hz with a 256-entry queue is below 0.1% in practice.
- **Layer ordering is enforced by the user, not by this layer.** If a fourth
  layer slips between pre and target (or between target and post), its CPU
  gets attributed to the target.
- **Single XrSession assumption.** Frame counters are per-DLL globals.
  Re-running an OpenXR app in the same process (probe-then-real init) writes
  back-to-back rows for both runs; split them on a `frame_idx` reset.
- **Process lifetime, not session lifetime.** The CSV is truncated when the
  writer thread starts (on xrCreateInstance) and the writer thread joins on
  xrDestroyInstance. Process exit without xrDestroyInstance flushes the
  queue's tail by virtue of the destructor running through `ResetInstance()`.

## Disabling at runtime

The standard OpenXR loader escape hatch works:

```cmd
set DISABLE_XR_APILAYER_MLEDOUR_layer_monitor_pre=1
set DISABLE_XR_APILAYER_MLEDOUR_layer_monitor_post=1
```

Set these before launching the host application to bypass both layers
without touching the registry.

## License

MIT. See [`LICENSE`](LICENSE). Original template by Matthieu Bucchianeri,
also MIT.
