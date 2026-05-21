#!/usr/bin/env python3
"""Merge pre/post frames-<pid>.csv from XR_APILAYER_MLEDOUR_layer_monitor and
report per-frame CPU cost attributed to the target layer.

Usage:
    python analyze.py <pre.csv> <post.csv> [--out merged.csv]

Math (per the README):
    target_us = ((pre_exit - pre_entry) - (post_exit - post_entry)) / qpc_freq * 1e6

Rows are paired by frame_idx + thread_id. Mismatched counts are reported as
warnings; the script joins on the common subset.
"""
from __future__ import annotations

import argparse
import csv
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("pre", type=Path, help="pre-side frames-<pid>.csv")
    ap.add_argument("post", type=Path, help="post-side frames-<pid>.csv")
    ap.add_argument("--out", type=Path, default=Path("target_cpu.csv"),
                    help="merged output (default: target_cpu.csv)")
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

    post_index = {(r[0], r[1]): r for r in post.rows}
    pre_only = 0
    matched: list[tuple[int, int, float, float, float]] = []
    for fi, tid, pe, px in pre.rows:
        prow = post_index.pop((fi, tid), None)
        if prow is None:
            pre_only += 1
            continue
        _, _, poe, pox = prow
        pre_us = (px - pe) / pre.qpc_freq * 1e6
        post_us = (pox - poe) / pre.qpc_freq * 1e6
        target_us = pre_us - post_us
        matched.append((fi, tid, pre_us, post_us, target_us))

    post_only = len(post_index)
    if pre_only or post_only:
        print(f"warning: unmatched rows pre_only={pre_only} post_only={post_only}",
              file=sys.stderr)

    if not matched:
        print("error: no rows matched -- check the input files", file=sys.stderr)
        return 1

    target_values = sorted(r[4] for r in matched)
    n = len(target_values)
    print(f"matched frames:   {n}")
    print(f"target_us mean:   {statistics.fmean(target_values):8.2f}")
    print(f"target_us median: {percentile(target_values, 50):8.2f}")
    print(f"target_us p95:    {percentile(target_values, 95):8.2f}")
    print(f"target_us p99:    {percentile(target_values, 99):8.2f}")
    print(f"target_us max:    {target_values[-1]:8.2f}")
    print(f"target_us min:    {target_values[0]:8.2f}")

    negative = sum(1 for v in target_values if v < 0)
    if negative:
        print(f"note: {negative} frames have negative target_us "
              f"(post bracket > pre bracket -- noise floor / counter jitter)",
              file=sys.stderr)

    with args.out.open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["frame_idx", "thread_id", "pre_us", "post_us", "target_us"])
        for row in matched:
            w.writerow(row)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
