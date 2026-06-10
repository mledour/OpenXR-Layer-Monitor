#!/usr/bin/env python3
"""Merge pre/post frames-<pid>.csv from XR_APILAYER_MLEDOUR_layer_monitor and
emit one row per frame with the target layer's CPU cost (microseconds + % of
the host's per-frame budget) and, when GPU CSVs are present, its GPU cost.

Usage:
    python analyze.py <pre.csv> <post.csv> [--out OUT]
                       [--gpu-pre GPU_PRE] [--gpu-post GPU_POST]

By default --out is `frames-merged-<pid>.csv` next to the inputs (the PID
is extracted from the `frames-<pid>-pre.csv` naming convention), matching
the path the in-DLL auto-merge writes to. --gpu-pre / --gpu-post default to
the sibling `gpu-<pid>-{pre,post}.csv` files; if they are absent (the normal
case on D3D12 / Vulkan / OpenGL hosts) the GPU column is left blank.

Math:
    pre_us            = (pre_exit  - pre_entry)  / qpc_freq * 1e6
    post_us           = (post_exit - post_entry) / qpc_freq * 1e6
    target_us         = pre_us - post_us
    frame_interval_us = pre_entry[i+1] - pre_entry[i]      (per thread)
    target_cpu_pct_of_frame = target_us / frame_interval_us * 100
    target_gpu_us     = (gpu_post_ticks - gpu_pre_ticks) / gpu_freq * 1e6
                        (joined by display_time; blank unless both GPU rows are
                         valid, share a frequency, and post_ticks >= pre_ticks)
    target_gpu_pct_of_frame = target_gpu_us / frame_interval_us * 100

Output columns (one row per matched frame, sorted by thread then display_time):
    display_time,thread_id,frame_interval_us,pre_us,post_us,target_us,
    target_cpu_pct_of_frame,target_gpu_us,target_gpu_pct_of_frame

The last frame per thread has no successor, so frame_interval_us,
target_cpu_pct_of_frame and target_gpu_pct_of_frame are left blank for that
row. target_gpu_us is blank for any frame without a valid GPU sample on both
sides.

Rows are paired by (display_time, thread_id). Mismatched counts are reported
as warnings and the script joins on the common subset.
"""
from __future__ import annotations

import argparse
import csv
import math
import re
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path

RAW_COLUMNS = ("display_time", "thread_id", "qpc_entry", "qpc_exit")
GPU_COLUMNS = ("display_time", "gpu_ticks", "gpu_freq", "valid")

# Mirrors merge.cpp's kDefaultQpcFreq: used when a per-side CSV has no
# (or a malformed) "# qpc_freq=" header, so a garbage value degrades to a
# known default instead of crashing the analyzer.
DEFAULT_QPC_FREQ = 10_000_000


@dataclass
class Frames:
    side: str
    qpc_freq: int
    rows: list[tuple[int, int, int, int]]  # display_time, thread_id, qpc_entry, qpc_exit


@dataclass
class GpuFrames:
    side: str
    rows: list[tuple[int, int, int, int]]  # display_time, gpu_ticks, gpu_freq, valid


def _parse_int4(raw: list[str]) -> tuple[int, int, int, int] | None:
    """Parse a four-integer CSV row, or None if it doesn't cleanly parse.

    Mirrors the C++ merge (ReadFrameCsv / ReadGpuCsv), which SKIPS a row whose
    four fields don't all extract -- a truncated or half-flushed row from a
    crash -- rather than aborting. Python's int() is strict, so without this a
    single bad row would raise ValueError (or IndexError on too few columns)
    and crash the whole analyzer where the in-DLL merge just drops the row.
    """
    try:
        return (int(raw[0]), int(raw[1]), int(raw[2]), int(raw[3]))
    except (ValueError, IndexError):
        return None


def _summarize(values: list[float]) -> tuple[float, float, float]:
    """(mean, min, max) over values, or (0.0, 0.0, 0.0) when empty.

    Mirrors merge.cpp's Summarize() -- a single source for the reduction so a
    change cannot drift between the ms / pct / gpu stat groups, keeping the
    merged-CSV header byte-equivalent regardless of which path produced it.
    """
    if not values:
        return (0.0, 0.0, 0.0)
    return (statistics.fmean(values), min(values), max(values))


def load(path: Path) -> Frames:
    meta: dict[str, str] = {}
    rows: list[tuple[int, int, int, int]] = []
    skipped = 0
    with path.open("r", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        header_found = False
        for raw in reader:
            if not raw:
                continue
            cell = raw[0]
            if cell.startswith("#"):
                key, _, value = cell.lstrip("# ").partition("=")
                meta[key.strip()] = value.strip()
                continue
            if not header_found:
                # The raw per-side CSV has exactly four columns. A user
                # passing the merged CSV by mistake (nine columns,
                # starts with display_time,thread_id,frame_interval_us...)
                # would otherwise crash with an obscure ValueError on
                # int("11100.000"). Catch it here with a clear message.
                if tuple(raw) != RAW_COLUMNS:
                    raise SystemExit(
                        f"{path}: unexpected column header {raw!r}, "
                        f"expected {list(RAW_COLUMNS)!r}. Did you pass "
                        f"the merged CSV instead of a per-side one?"
                    )
                header_found = True
                continue
            parsed = _parse_int4(raw)
            if parsed is None:
                skipped += 1
                continue
            rows.append(parsed)
    if skipped:
        print(f"warning: {path}: skipped {skipped} malformed/truncated row(s)",
              file=sys.stderr)
    # Tolerate a malformed "# qpc_freq=" value (e.g. "1e7", "10 MHz") the way
    # merge.cpp's StrictStoll does -- fall back to the default rather than
    # crashing on int().
    freq_raw = meta.get("qpc_freq", str(DEFAULT_QPC_FREQ))
    try:
        qpc_freq = int(freq_raw)
    except ValueError:
        print(f"warning: {path}: ignoring malformed qpc_freq={freq_raw!r}, "
              f"using {DEFAULT_QPC_FREQ}", file=sys.stderr)
        qpc_freq = DEFAULT_QPC_FREQ
    return Frames(
        side=meta.get("side", path.stem),
        qpc_freq=qpc_freq,
        rows=rows,
    )


def load_gpu(path: Path) -> GpuFrames | None:
    """Load a per-side GPU CSV (gpu-<pid>-{pre,post}.csv).

    Returns None when the file is absent -- the normal case on non-D3D11
    hosts, where the layer never creates a GPU timer -- so the caller merges
    CPU-only with every target_gpu_us blank. A present-but-malformed column
    header is warned about and also treated as absent (returns None), matching
    the C++ merge, which logs and proceeds CPU-only rather than aborting.
    """
    if not path.exists():
        return None
    meta: dict[str, str] = {}
    rows: list[tuple[int, int, int, int]] = []
    skipped = 0
    with path.open("r", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        header_found = False
        for raw in reader:
            if not raw:
                continue
            cell = raw[0]
            if cell.startswith("#"):
                key, _, value = cell.lstrip("# ").partition("=")
                meta[key.strip()] = value.strip()
                continue
            if not header_found:
                if tuple(raw) != GPU_COLUMNS:
                    print(
                        f"warning: {path}: unexpected GPU column header "
                        f"{raw!r}, expected {list(GPU_COLUMNS)!r}; "
                        f"skipping GPU join",
                        file=sys.stderr,
                    )
                    return None
                header_found = True
                continue
            parsed = _parse_int4(raw)
            if parsed is None:
                skipped += 1
                continue
            rows.append(parsed)
    if skipped:
        print(f"warning: {path}: skipped {skipped} malformed/truncated "
              f"GPU row(s)", file=sys.stderr)
    return GpuFrames(side=meta.get("side", path.stem), rows=rows)


def default_out_path(pre_path: Path) -> Path:
    """Pick the merged-CSV path that mirrors the in-DLL output naming.

    `frames-<pid>-pre.csv` -> `frames-merged-<pid>.csv` next to it. Falls
    back to `frames-merged.csv` in the CWD if the input doesn't follow
    that convention.
    """
    m = re.match(r"frames-(\d+)-pre\.csv$", pre_path.name)
    if m:
        return pre_path.parent / f"frames-merged-{m.group(1)}.csv"
    return Path("frames-merged.csv")


def default_gpu_path(frames_path: Path) -> Path:
    """Sibling GPU CSV for a per-side frames CSV.

    `frames-<pid>-pre.csv` -> `gpu-<pid>-pre.csv` next to it (same for post).
    For an unconventional name we still point at a `gpu-`-prefixed sibling,
    which simply won't exist and so resolves to "no GPU data".
    """
    name = frames_path.name
    if name.startswith("frames-"):
        return frames_path.with_name("gpu-" + name[len("frames-"):])
    return frames_path.with_name("gpu-" + name)


def gpu_target_us(
    fi: int,
    pre_idx: dict[int, tuple[int, int, int]],
    post_idx: dict[int, tuple[int, int, int]],
) -> float | None:
    """target_gpu_us for one frame, or None if it can't be computed.

    Blank (None) unless BOTH sides have a row for this display_time, both are
    valid, the frequencies match and are non-zero, and post_ticks >=
    pre_ticks. Mirrors the C++ JoinGpu guards exactly.
    """
    p = pre_idx.get(fi)
    q = post_idx.get(fi)
    if p is None or q is None:
        return None
    pre_ticks, pre_freq, pre_valid = p
    post_ticks, post_freq, post_valid = q
    if not pre_valid or not post_valid:
        return None
    if pre_freq == 0 or pre_freq != post_freq:
        return None
    if post_ticks < pre_ticks:
        return None
    return (post_ticks - pre_ticks) / pre_freq * 1e6


def percentile(sorted_values: list[float], pct: float) -> float:
    if not sorted_values:
        return float("nan")
    if len(sorted_values) == 1:
        return sorted_values[0]
    rank = pct / 100.0 * (len(sorted_values) - 1)
    lo, hi = int(rank), min(int(rank) + 1, len(sorted_values) - 1)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (rank - lo)


def print_stats(label: str, values: list[float], unit: str, fmt: str = "8.2f") -> None:
    if not values:
        print(f"{label} {unit}: (no samples)")
        return
    s = sorted(values)
    n = len(s)
    print(f"{label} {unit}:")
    print(f"    count  {n}")
    print(f"    mean   {statistics.fmean(s):{fmt}}")
    print(f"    median {percentile(s, 50):{fmt}}")
    print(f"    p95    {percentile(s, 95):{fmt}}")
    print(f"    p99    {percentile(s, 99):{fmt}}")
    print(f"    min    {s[0]:{fmt}}")
    print(f"    max    {s[-1]:{fmt}}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("pre", type=Path, help="pre-side frames-<pid>-pre.csv")
    ap.add_argument("post", type=Path, help="post-side frames-<pid>-post.csv")
    ap.add_argument("--out", type=Path, default=None,
                    help="merged per-frame output "
                         "(default: frames-merged-<pid>.csv alongside the inputs)")
    ap.add_argument("--gpu-pre", type=Path, default=None,
                    help="pre-side gpu-<pid>-pre.csv "
                         "(default: sibling gpu-<pid>-pre.csv; absent = no GPU)")
    ap.add_argument("--gpu-post", type=Path, default=None,
                    help="post-side gpu-<pid>-post.csv "
                         "(default: sibling gpu-<pid>-post.csv; absent = no GPU)")
    args = ap.parse_args()
    if args.out is None:
        args.out = default_out_path(args.pre)
    if args.gpu_pre is None:
        args.gpu_pre = default_gpu_path(args.pre)
    if args.gpu_post is None:
        args.gpu_post = default_gpu_path(args.post)

    pre = load(args.pre)
    post = load(args.post)
    gpu_pre = load_gpu(args.gpu_pre)
    gpu_post = load_gpu(args.gpu_post)

    if pre.side != "pre":
        print(f"warning: {args.pre} reports side={pre.side!r}, expected 'pre'", file=sys.stderr)
    if post.side != "post":
        print(f"warning: {args.post} reports side={post.side!r}, expected 'post'", file=sys.stderr)
    if pre.qpc_freq != post.qpc_freq:
        print(f"warning: qpc_freq mismatch pre={pre.qpc_freq} post={post.qpc_freq} -- using pre",
              file=sys.stderr)

    # Index GPU rows by display_time (GPU rows carry no thread_id; D3D11 GPU
    # submission is single-threaded). last-wins on duplicate keys, matching
    # the C++ std::map[key] = r join.
    gpu_pre_idx: dict[int, tuple[int, int, int]] = {}
    gpu_post_idx: dict[int, tuple[int, int, int]] = {}
    if gpu_pre is not None:
        gpu_pre_idx = {r[0]: (r[1], r[2], r[3]) for r in gpu_pre.rows}
    if gpu_post is not None:
        gpu_post_idx = {r[0]: (r[1], r[2], r[3]) for r in gpu_post.rows}

    freq = pre.qpc_freq
    qpc_to_us = lambda ticks: ticks / freq * 1e6  # noqa: E731

    # Frame interval comes from pre's qpc_entry deltas. pre sits at the top of
    # the chain so its entry timestamp is the closest proxy we have for "the
    # host called xrEndFrame at time T". Computed per thread because in
    # multi-session scenarios different threads can have independent frame
    # sequences -- mixing their timestamps would produce nonsense intervals.
    pre_entry_by_thread: dict[int, list[tuple[int, int]]] = {}
    for fi, tid, pe, _ in pre.rows:
        pre_entry_by_thread.setdefault(tid, []).append((fi, pe))
    next_entry: dict[tuple[int, int], int] = {}
    for tid, items in pre_entry_by_thread.items():
        items.sort()
        for (fi, pe), (_, pe_next) in zip(items, items[1:]):
            next_entry[(fi, tid)] = pe_next

    post_index = {(r[0], r[1]): r for r in post.rows}
    pre_only = 0
    merged: list[
        tuple[int, int, float | None, float, float, float, float | None,
              float | None, float | None]
    ] = []
    for fi, tid, pe, px in pre.rows:
        prow = post_index.pop((fi, tid), None)
        if prow is None:
            pre_only += 1
            continue
        _, _, poe, pox = prow
        pre_us = qpc_to_us(px - pe)
        post_us = qpc_to_us(pox - poe)
        target_us = pre_us - post_us

        # Both frame_interval_us and target_pct stay None when the
        # interval is missing OR non-positive. Non-invariant TSC across
        # a core migration can produce qpc_entry[i+1] < qpc_entry[i],
        # in which case "frame interval" is undefined; emitting a
        # negative number would mislead any downstream tool dividing by
        # it. Matches the C++ ComputeMerge guard.
        frame_interval_us = None
        target_pct = None
        pe_next = next_entry.get((fi, tid))
        if pe_next is not None:
            fiv = qpc_to_us(pe_next - pe)
            if fiv > 0:
                frame_interval_us = fiv
                target_pct = target_us / fiv * 100.0

        # GPU join by display_time -- blank when no valid GPU sample on both
        # sides. Matches the C++ JoinGpu guards.
        target_gpu = gpu_target_us(fi, gpu_pre_idx, gpu_post_idx)
        # GPU duration as % of the frame interval, mirroring target_pct (CPU).
        # Only when this frame has a GPU sample AND an interval. Matches the
        # C++ JoinGpu pct computation.
        target_gpu_pct = None
        if target_gpu is not None and frame_interval_us is not None:
            target_gpu_pct = target_gpu / frame_interval_us * 100.0

        merged.append((fi, tid, frame_interval_us, pre_us, post_us, target_us,
                       target_pct, target_gpu, target_gpu_pct))

    post_only = len(post_index)
    if pre_only or post_only:
        print(f"warning: unmatched rows pre_only={pre_only} post_only={post_only}",
              file=sys.stderr)

    if not merged:
        print("error: no rows matched -- check the input files", file=sys.stderr)
        return 1

    # Sort for deterministic output: by thread, then display_time.
    merged.sort(key=lambda r: (r[1], r[0]))

    target_values = [r[5] for r in merged]
    interval_values = [r[2] for r in merged if r[2] is not None]
    pct_values = [r[6] for r in merged if r[6] is not None]
    gpu_values = [r[7] for r in merged if r[7] is not None]
    gpu_pct_values = [r[8] for r in merged if r[8] is not None]

    print(f"matched frames: {len(merged)}")
    print()
    print_stats("target_cpu_us      ", target_values, "(microseconds, CPU)")
    print()
    print_stats("target_cpu_pct     ", pct_values, "(% of frame interval)", fmt="8.3f")
    print()
    print_stats("target_gpu_us      ", gpu_values, "(microseconds, GPU)")
    print()
    print_stats("target_gpu_pct     ", gpu_pct_values, "(% of frame interval)", fmt="8.3f")
    print()
    if interval_values:
        fps_median = 1e6 / percentile(sorted(interval_values), 50)
        print(f"frame_interval_us median: {percentile(sorted(interval_values), 50):8.2f}  "
              f"(~{fps_median:.1f} Hz)")

    negative = sum(1 for v in target_values if v < 0)
    if negative:
        print(f"note: {negative} frames have negative target_us "
              f"(post bracket > pre bracket -- noise floor / counter jitter)",
              file=sys.stderr)

    # Header lines at the top of the merged CSV -- byte-equivalent to what
    # the in-DLL merge in layer.cpp emits, so users get the same summary
    # numbers regardless of which path produced the file. pct_* excludes
    # the last frame per thread (no successor -> no interval -> no pct),
    # matching the C++ merge.
    target_cpu_ms_values = [v / 1000.0 for v in target_values]
    target_cpu_ms_mean, target_cpu_ms_min, target_cpu_ms_max = _summarize(
        target_cpu_ms_values)
    target_cpu_pct_mean, target_cpu_pct_min, target_cpu_pct_max = _summarize(
        pct_values)

    # GPU aggregates over frames with a valid target_gpu_us. gpu_frame_count
    # == 0 means GPU was not captured (non-D3D11 host); the ms_*/pct_* lines
    # are then 0.0000 and every target_gpu_us cell is blank. The pct subset
    # also requires a frame_interval. Matches C++ ComputeStats.
    gpu_ms_values = [v / 1000.0 for v in gpu_values]
    target_gpu_ms_mean, target_gpu_ms_min, target_gpu_ms_max = _summarize(gpu_ms_values)
    target_gpu_pct_mean, target_gpu_pct_min, target_gpu_pct_max = _summarize(
        gpu_pct_values)

    # newline="" disables the file object's translation; lineterminator='\n'
    # disables csv.writer's default '\r\n'. Together they keep the output
    # LF-only on every platform, matching the C++ merge (binary mode + '\n')
    # so frames-merged-<pid>.csv is byte-equivalent regardless of which
    # path produced it.
    with args.out.open("w", newline="", encoding="utf-8") as fh:
        fh.write(f"# frame_count={len(merged)}\n")
        fh.write(f"# target_cpu_ms_mean={target_cpu_ms_mean:.4f}\n")
        fh.write(f"# target_cpu_ms_min={target_cpu_ms_min:.4f}\n")
        fh.write(f"# target_cpu_ms_max={target_cpu_ms_max:.4f}\n")
        fh.write(f"# target_cpu_pct_mean={target_cpu_pct_mean:.4f}%\n")
        fh.write(f"# target_cpu_pct_min={target_cpu_pct_min:.4f}%\n")
        fh.write(f"# target_cpu_pct_max={target_cpu_pct_max:.4f}%\n")
        fh.write(f"# gpu_frame_count={len(gpu_values)}\n")
        fh.write(f"# target_gpu_ms_mean={target_gpu_ms_mean:.4f}\n")
        fh.write(f"# target_gpu_ms_min={target_gpu_ms_min:.4f}\n")
        fh.write(f"# target_gpu_ms_max={target_gpu_ms_max:.4f}\n")
        fh.write(f"# target_gpu_pct_mean={target_gpu_pct_mean:.4f}%\n")
        fh.write(f"# target_gpu_pct_min={target_gpu_pct_min:.4f}%\n")
        fh.write(f"# target_gpu_pct_max={target_gpu_pct_max:.4f}%\n")
        w = csv.writer(fh, lineterminator="\n")
        w.writerow(["display_time", "thread_id", "frame_interval_us",
                    "pre_us", "post_us", "target_us", "target_cpu_pct_of_frame",
                    "target_gpu_us", "target_gpu_pct_of_frame"])
        for fi, tid, fiv, pu, postu, tu, pct, gpu, gpu_pct in merged:
            w.writerow([
                fi,
                tid,
                "" if fiv is None or math.isnan(fiv) else f"{fiv:.3f}",
                f"{pu:.3f}",
                f"{postu:.3f}",
                f"{tu:.3f}",
                "" if pct is None or math.isnan(pct) else f"{pct:.4f}",
                "" if gpu is None or math.isnan(gpu) else f"{gpu:.3f}",
                "" if gpu_pct is None or math.isnan(gpu_pct) else f"{gpu_pct:.4f}",
            ])
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
