#!/usr/bin/env python3
# MIT License
#
# Copyright (c) 2026 Michael Ledour
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions :
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
"""Validate whether the D3D12 GPU-sandwich overhead is ADDITIVE.

Companion to the synthetic-load harness (utils/synthetic_load.*). For each
hello_xr DX12 run with MLEDOUR_GPULOAD_ITERS set, two files land in
%LOCALAPPDATA%\\<layer-folder>\\:

  frames-merged-<pid>.csv  -> target_gpu_us = S = T_post - T_pre  (the sandwich)
  gpuload-<pid>.csv        -> known_ticks   = K = T1   - T0       (inline truth)

Per frame, the inter-submission overhead is  O = S - K. Sweeping the iteration
count gives several K levels; this script joins each run on display_time,
regresses S against K across runs, and prints the verdict:

  * slope a ~= 1, intercept b ~constant > 0   -> ADDITIVE: subtract b (~= O)
  * O collapses toward 0 as K grows           -> ABSORBED: do not subtract
  * b at K~=0 ~= the empty-sandwich baseline   -> empty calibration is enough
  * otherwise                                  -> document the floor only

Usage:
  python validate_additivity.py FILE [FILE ...]
Pass every frames-merged-<pid>.csv and gpuload-<pid>.csv from all sweep runs
(any order). Files are classified by header and paired by <pid>.
"""
from __future__ import annotations

import re
import statistics
import sys
from pathlib import Path

_PID_RE = re.compile(r"(\d+)\.csv$")


def _pid(path: Path) -> str | None:
    m = _PID_RE.search(path.name)
    return m.group(1) if m else None


def _classify(path: Path) -> str | None:
    """'merged', 'gpuload', or None -- decided by the column header line."""
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if line.startswith("display_time,") and "target_gpu_us" in line:
                    return "merged"
                if line.startswith("display_time,") and "known_ticks" in line:
                    return "gpuload"
                return None  # first non-comment line was not a header we know
    except OSError:
        return None
    return None


def _header_value(path: Path, key: str) -> str | None:
    """Read a '# key=value' header line (e.g. iters)."""
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line.startswith("#"):
                    if line.startswith("display_time,"):
                        break  # past the header block
                    continue
                body = line[1:].strip()
                if body.startswith(key + "="):
                    return body[len(key) + 1:].strip()
    except OSError:
        return None
    return None


def _load_known_us(path: Path) -> dict[int, float]:
    """display_time -> K in microseconds, for valid==1 rows only."""
    out: dict[int, float] = {}
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("display_time,"):
                continue
            c = line.split(",")
            if len(c) < 4:
                continue
            try:
                dt = int(c[0])
                ticks = int(c[1])
                freq = int(c[2])
                valid = int(c[3])
            except ValueError:
                continue
            if valid != 1 or freq == 0:
                continue
            out[dt] = ticks / freq * 1e6
    return out


def _load_target_gpu_us(path: Path) -> dict[int, float]:
    """display_time -> S (target_gpu_us, column 7) for rows that have it."""
    out: dict[int, float] = {}
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("display_time,"):
                continue
            c = line.split(",")
            if len(c) < 8:
                continue
            cell = c[7].strip()
            if not cell:
                continue  # blank target_gpu_us (no GPU row that frame)
            try:
                out[int(c[0])] = float(cell)
            except ValueError:
                continue
    return out


def _median(xs: list[float]) -> float:
    return statistics.median(xs) if xs else float("nan")


def _linfit(xs: list[float], ys: list[float]) -> tuple[float, float, float]:
    """Least-squares S = a*K + b. Returns (a, b, r2). a=nan if K has no spread."""
    n = len(xs)
    if n < 2:
        return float("nan"), float("nan"), float("nan")
    sx, sy = sum(xs), sum(ys)
    sxx = sum(x * x for x in xs)
    sxy = sum(x * y for x, y in zip(xs, ys))
    denom = n * sxx - sx * sx
    if abs(denom) < 1e-9:
        return float("nan"), float("nan"), float("nan")
    a = (n * sxy - sx * sy) / denom
    b = (sy - a * sx) / n
    mean_y = sy / n
    ss_tot = sum((y - mean_y) ** 2 for y in ys)
    ss_res = sum((y - (a * x + b)) ** 2 for x, y in zip(xs, ys))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 1e-12 else float("nan")
    return a, b, r2


class Run:
    def __init__(self, pid: str, iters: int, ks: list[float], ss: list[float]):
        self.pid = pid
        self.iters = iters
        self.ks = ks
        self.ss = ss
        self.os = [s - k for s, k in zip(ss, ks)]

    @property
    def n(self) -> int:
        return len(self.ks)


def main(argv: list[str]) -> int:
    paths = [Path(a) for a in argv]
    if not paths:
        print(__doc__)
        return 2

    merged: dict[str, Path] = {}
    gpuload: dict[str, Path] = {}
    for p in paths:
        if not p.exists():
            print(f"warning: {p} does not exist, skipping", file=sys.stderr)
            continue
        kind = _classify(p)
        pid = _pid(p)
        if kind is None or pid is None:
            print(f"warning: {p.name} is neither a merged nor a gpuload CSV "
                  f"(or has no <pid>), skipping", file=sys.stderr)
            continue
        (merged if kind == "merged" else gpuload)[pid] = p

    pids = sorted(set(merged) & set(gpuload), key=lambda x: int(x))
    unpaired = (set(merged) ^ set(gpuload))
    for pid in sorted(unpaired):
        which = "gpuload" if pid in merged else "frames-merged"
        print(f"warning: pid {pid} has no matching {which} CSV, skipping",
              file=sys.stderr)
    if not pids:
        print("error: no pid had BOTH a frames-merged and a gpuload CSV.",
              file=sys.stderr)
        return 1

    runs: list[Run] = []
    for pid in pids:
        S = _load_target_gpu_us(merged[pid])
        K = _load_known_us(gpuload[pid])
        iters_s = _header_value(gpuload[pid], "iters")
        iters = int(iters_s) if iters_s and iters_s.isdigit() else -1
        common = sorted(set(S) & set(K))
        if not common:
            print(f"warning: pid {pid} had no display_time overlap between "
                  f"merged and gpuload, skipping", file=sys.stderr)
            continue
        ks = [K[dt] for dt in common]
        ss = [S[dt] for dt in common]
        runs.append(Run(pid, iters, ks, ss))

    if not runs:
        print("error: no run produced a usable join.", file=sys.stderr)
        return 1

    runs.sort(key=lambda r: _median(r.ks))

    print("=" * 78)
    print("D3D12 GPU-SANDWICH ADDITIVITY  (all values in microseconds)")
    print("=" * 78)
    print(f"{'iters':>6} {'pid':>7} {'n':>6} {'med K':>10} {'med S':>10} "
          f"{'med O=S-K':>11} {'O p25..p75':>16}")
    print("-" * 78)
    for r in runs:
        os_sorted = sorted(r.os)
        p25 = os_sorted[len(os_sorted) // 4] if os_sorted else float("nan")
        p75 = os_sorted[(3 * len(os_sorted)) // 4] if os_sorted else float("nan")
        print(f"{r.iters:>6} {r.pid:>7} {r.n:>6} {_median(r.ks):>10.1f} "
              f"{_median(r.ss):>10.1f} {_median(r.os):>11.1f} "
              f"{p25:>7.1f}..{p75:<7.1f}")
    print("-" * 78)

    med_ks = [_median(r.ks) for r in runs]
    med_ss = [_median(r.ss) for r in runs]
    med_os = [_median(r.os) for r in runs]
    a, b, r2 = _linfit(med_ks, med_ss)

    print(f"\nLeast-squares  S = a*K + b  over {len(runs)} run medians:")
    print(f"  slope a    = {a:.3f}   (1.0 => the sandwich tracks real work 1:1)")
    print(f"  intercept b= {b:.1f} us   (overhead extrapolated to K=0)")
    print(f"  R^2        = {r2:.4f}")

    o_lo, o_hi = med_os[0], med_os[-1]   # O at lowest vs highest K
    print(f"\n  O at lowest K ({med_ks[0]:.0f} us) = {o_lo:.1f} us")
    print(f"  O at highest K ({med_ks[-1]:.0f} us) = {o_hi:.1f} us")

    # ---- verdict -----------------------------------------------------------
    print("\n" + "=" * 78)
    EMPTY_BASELINE = 270.0  # the empty pre/post run you already measured
    slope_ok = not (a != a) and abs(a - 1.0) < 0.15          # a==a guards NaN
    o_const = o_hi > 0.6 * o_lo and o_lo > 30.0
    o_collapses = o_hi < 0.5 * o_lo

    if slope_ok and o_const:
        print("VERDICT: ADDITIVE.")
        print(f"  Overhead is ~constant (~{statistics.median(med_os):.0f} us) and")
        print(f"  independent of the target's GPU work. SAFE to subtract a")
        print(f"  calibrated baseline of ~{b:.0f} us from target_gpu_us.")
        rel = abs(b - EMPTY_BASELINE) / EMPTY_BASELINE if EMPTY_BASELINE else 1
        if rel < 0.25:
            print(f"  b ~= the {EMPTY_BASELINE:.0f} us empty baseline: an empty-")
            print(f"  sandwich calibration at startup is representative.")
        else:
            print(f"  b differs from the {EMPTY_BASELINE:.0f} us empty baseline:")
            print(f"  calibrate WITH a target present (per-boundary effect),")
            print(f"  not with the 2-submission empty sandwich.")
    elif o_collapses:
        print("VERDICT: ABSORBED (NOT additive).")
        print("  The overhead shrinks as real GPU work grows -- the idle gap is")
        print("  filled by the target. Do NOT subtract a constant baseline; it")
        print("  would over-correct. Instead, document the floor and blank/flag")
        print("  target_gpu_us when it falls inside the noise band.")
    else:
        print("VERDICT: COMPLEX / INCONCLUSIVE.")
        print(f"  slope a={a:.2f}, O went {o_lo:.0f}->{o_hi:.0f} us across the sweep.")
        print("  Neither a clean constant nor a clean collapse. Widen the sweep")
        print("  (more iters levels, longer runs) or profile with PIX/GPUView")
        print("  before trusting any subtraction. Safest: document the floor.")
    print("=" * 78)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
