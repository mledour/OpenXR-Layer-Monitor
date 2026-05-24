#!/usr/bin/env python3
"""Merge pre/post frames-<pid>.csv from XR_APILAYER_MLEDOUR_layer_monitor and
emit one row per frame with the target layer's CPU cost both in microseconds
and as a percentage of the host's per-frame wall-clock budget.

Usage:
    python analyze.py <pre.csv> <post.csv> [--out frames-merged.csv]

Math:
    pre_us            = (pre_exit  - pre_entry)  / qpc_freq * 1e6
    post_us           = (post_exit - post_entry) / qpc_freq * 1e6
    target_us         = pre_us - post_us
    frame_interval_us = pre_entry[i+1] - pre_entry[i]      (per thread)
    target_pct_of_frame = target_us / frame_interval_us * 100

Output columns (one row per matched frame, sorted by thread then frame_idx):
    frame_idx,thread_id,frame_interval_us,pre_us,post_us,target_us,target_pct_of_frame

The last frame per thread has no successor, so frame_interval_us and
target_pct_of_frame are left blank for that row.

Rows are paired by (frame_idx, thread_id). Mismatched counts are reported
as warnings and the script joins on the common subset.
"""
from __future__ import annotations

import argparse
import csv
import math
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Frames:
    side: str
    qpc_freq: int
    rows: list[tuple[int, int, int, int]]  # frame_idx, thread_id, qpc_entry, qpc_exit


def load(path: Path) -> Frames:
    meta: dict[str, str] = {}
    rows: list[tuple[int, int, int, int]] = []
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
                header_found = True
                continue
            rows.append((int(raw[0]), int(raw[1]), int(raw[2]), int(raw[3])))
    return Frames(
        side=meta.get("side", path.stem),
        qpc_freq=int(meta.get("qpc_freq", "10000000")),
        rows=rows,
    )


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
    ap.add_argument("pre", type=Path, help="pre-side frames-<pid>.csv")
    ap.add_argument("post", type=Path, help="post-side frames-<pid>.csv")
    ap.add_argument("--out", type=Path, default=Path("frames-merged.csv"),
                    help="merged per-frame output (default: frames-merged.csv)")
    args = ap.parse_args()

    pre = load(args.pre)
    post = load(args.post)

    if pre.side != "pre":
        print(f"warning: {args.pre} reports side={pre.side!r}, expected 'pre'", file=sys.stderr)
    if post.side != "post":
        print(f"warning: {args.post} reports side={post.side!r}, expected 'post'", file=sys.stderr)
    if pre.qpc_freq != post.qpc_freq:
        print(f"warning: qpc_freq mismatch pre={pre.qpc_freq} post={post.qpc_freq} -- using pre",
              file=sys.stderr)

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
    merged: list[tuple[int, int, float | None, float, float, float, float | None]] = []
    for fi, tid, pe, px in pre.rows:
        prow = post_index.pop((fi, tid), None)
        if prow is None:
            pre_only += 1
            continue
        _, _, poe, pox = prow
        pre_us = qpc_to_us(px - pe)
        post_us = qpc_to_us(pox - poe)
        target_us = pre_us - post_us

        pe_next = next_entry.get((fi, tid))
        if pe_next is not None:
            frame_interval_us = qpc_to_us(pe_next - pe)
            target_pct = target_us / frame_interval_us * 100.0 if frame_interval_us > 0 else None
        else:
            frame_interval_us = None
            target_pct = None

        merged.append((fi, tid, frame_interval_us, pre_us, post_us, target_us, target_pct))

    post_only = len(post_index)
    if pre_only or post_only:
        print(f"warning: unmatched rows pre_only={pre_only} post_only={post_only}",
              file=sys.stderr)

    if not merged:
        print("error: no rows matched -- check the input files", file=sys.stderr)
        return 1

    # Sort for deterministic output: by thread, then frame_idx.
    merged.sort(key=lambda r: (r[1], r[0]))

    target_values = [r[5] for r in merged]
    interval_values = [r[2] for r in merged if r[2] is not None]
    pct_values = [r[6] for r in merged if r[6] is not None]

    print(f"matched frames: {len(merged)}")
    print()
    print_stats("target_us         ", target_values, "(microseconds)")
    print()
    print_stats("target_pct_of_frame", pct_values, "(% of frame interval)", fmt="8.3f")
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
    target_ms_values = [v / 1000.0 for v in target_values]
    target_ms_mean = statistics.fmean(target_ms_values) if target_ms_values else 0.0
    target_ms_min = min(target_ms_values) if target_ms_values else 0.0
    target_ms_max = max(target_ms_values) if target_ms_values else 0.0
    target_pct_mean = statistics.fmean(pct_values) if pct_values else 0.0
    target_pct_min = min(pct_values) if pct_values else 0.0
    target_pct_max = max(pct_values) if pct_values else 0.0

    with args.out.open("w", newline="", encoding="utf-8") as fh:
        fh.write(f"# frame_count={len(merged)}\n")
        fh.write(f"# target_ms_mean={target_ms_mean:.4f}\n")
        fh.write(f"# target_ms_min={target_ms_min:.4f}\n")
        fh.write(f"# target_ms_max={target_ms_max:.4f}\n")
        fh.write(f"# target_pct_mean={target_pct_mean:.4f}\n")
        fh.write(f"# target_pct_min={target_pct_min:.4f}\n")
        fh.write(f"# target_pct_max={target_pct_max:.4f}\n")
        w = csv.writer(fh)
        w.writerow(["frame_idx", "thread_id", "frame_interval_us",
                    "pre_us", "post_us", "target_us", "target_pct_of_frame"])
        for fi, tid, fiv, pu, postu, tu, pct in merged:
            w.writerow([
                fi,
                tid,
                "" if fiv is None or math.isnan(fiv) else f"{fiv:.3f}",
                f"{pu:.3f}",
                f"{postu:.3f}",
                f"{tu:.3f}",
                "" if pct is None or math.isnan(pct) else f"{pct:.4f}",
            ])
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
