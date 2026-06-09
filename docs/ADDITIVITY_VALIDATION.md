# D3D12 GPU-sandwich — additivity validation (TEMPORARY harness)

**Goal.** Decide whether the D3D12 GPU sandwich's ~270 µs noise floor (seen on
an empty `pre → post` run: `target_gpu_us ≈ 270 µs` with no target between the
layers) is an **additive constant** we can calibrate-and-subtract, or an
**artifact a real target's GPU work absorbs**. The answer decides whether the
"measure baseline & subtract" feature is worth building.

This whole harness is temporary. Once the question is settled, delete:
`utils/synthetic_load.{h,cpp}`, the `g_synthLoad` block + call sites in
`layer.cpp`, the two `synthetic_load` lines in the `.vcxproj`,
`scripts/validate_additivity.py`, and this file.

## What the harness does

The monitor's **pre** side, *only* when `MLEDOUR_GPULOAD_ITERS` is set, submits
a self-timed loop of `CopyBufferRegion` on the app's D3D12 queue **after** it
records `T_pre` and **before** it forwards downstream. On the queue the copies
land between the sandwich's two markers:

```
[app draws] → [T_pre] → [gap_a][copy loop = K][gap_b] → [T_post]
```

- The merge's `target_gpu_us` measures `S = T_post − T_pre = K + O`.
- The copy loop self-times `K` inline (one command list, no submission gap) →
  written to `gpuload-<pid>.csv`.
- Per frame, the overhead is `O = S − K`. Sweep the iteration count → several
  `K` levels → see how `O` behaves.

Default OFF: with the env var unset, none of this code runs and the monitor is
behaviourally identical to today.

## Build & deploy

1. Rebuild **both** `*_pre` and `*_post` (the harness compiles into both, runs
   only in pre) — `utils\synthetic_load.cpp` is already in the `.vcxproj`.
2. Redeploy/register the rebuilt DLLs as usual (`scripts\Install-Layer.ps1`).
   The layer order must stay `app → …_pre → …_post → runtime` with **nothing**
   between pre and post (the harness *is* the thing between them).

## Run the sweep (DX12 only)

The harness is D3D12-only, so run hello_xr with its **D3D12** graphics plugin
(e.g. `-g D3D12`, per your hello_xr build). A DX11 run simply won't build the
load (no D3D12 binding) and writes no `gpuload` CSV.

For each level below, in a fresh console:

```powershell
$env:MLEDOUR_GPULOAD_ITERS = "0"      # then 8, then 40, then 160, then 480
# optional: $env:MLEDOUR_GPULOAD_BYTES = "8388608"   # 8 MiB default
# launch hello_xr (D3D12), press Ctrl+F9 to START monitoring,
# let it run ~30 s, press Ctrl+F9 to STOP, then quit hello_xr.
```

- Monitoring **must be ON** during the run: the load only runs while recording.
- Each run is a new process → new `<pid>` → files don't clobber. Collect both
  `frames-merged-<pid>.csv` **and** `gpuload-<pid>.csv` from
  `%LOCALAPPDATA%\<your shared layer folder>\` after each run.
- `iters=0` is the **control** run: `K ≈ 0`, so `S` is the bare overhead. It
  should reproduce your ~270 µs.
- The `iters` values are a starting point. The analyzer prints the actual
  median `K` per run; if the `K` levels come out too clustered, re-run with
  larger spacing (the regression just needs a spread of `K`).

## Analyze

Pass every collected CSV (any order — paired by `<pid>`):

```bash
python scripts/validate_additivity.py \
    frames-merged-*.csv gpuload-*.csv
```

It prints a per-run table (`med K`, `med S`, `med O`), the least-squares fit
`S = a·K + b`, and a verdict:

| Observation | Verdict | Action |
|---|---|---|
| slope `a ≈ 1`, `O` ~constant across the sweep | **ADDITIVE** | subtract `b` (≈ `O`) |
| `O` collapses toward 0 as `K` grows | **ABSORBED** | don't subtract; document/flag the floor |
| `b` at `K≈0` ≈ 270 µs | empty calibration representative | calibrate at startup |
| `b` ≈ 2× the empty baseline | per-boundary cost | calibrate *with* a target present |
| anything else | **INCONCLUSIVE** | widen sweep / profile with PIX·GPUView |

Send me the analyzer output (and the CSVs if you want) and I'll wire up whatever
the verdict calls for — the subtract-in-merge path, or the document-the-floor
path.
