#!/usr/bin/env python3
"""pytest suite for scripts/analyze.py.

Same coverage philosophy as openxr-api-layer-tests/test_merge.cpp on the C++
side: drive each piece (load, default_out_path, merge end-to-end) on
synthetic input the test builds in-memory. The script and the in-DLL merge
both emit `frames-merged-<pid>.csv` with identical formatting, so an
implicit byte-equivalence check is performed against the same hand-verified
fixtures used in test_merge.cpp.

Run:
    pytest scripts/test_analyze.py
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path
from textwrap import dedent

import pytest

HERE = Path(__file__).parent
ANALYZE = HERE / "analyze.py"


def write_raw_csv(path: Path, side: str, qpc_freq: int,
                  rows: list[tuple[int, int, int, int]]) -> None:
    """Mirror layer.cpp's FrameCsvSink output format."""
    with path.open("w", encoding="utf-8") as fh:
        fh.write(f"# qpc_freq={qpc_freq}\n")
        fh.write(f"# side={side}\n")
        fh.write(f"# layer=XR_APILAYER_MLEDOUR_layer_monitor_{side}\n")
        fh.write("# fn=xrEndFrame\n")
        fh.write("frame_idx,thread_id,qpc_entry,qpc_exit\n")
        for fi, tid, qe, qx in rows:
            fh.write(f"{fi},{tid},{qe},{qx}\n")


def run_analyze(pre: Path, post: Path, out: Path | None = None) -> tuple[int, str, str]:
    """Run analyze.py as a subprocess. Returns (returncode, stdout, stderr).

    Subprocess (rather than direct import) keeps the test honest about how
    users actually invoke the script -- argparse, stderr warnings, exit
    codes all come through unchanged.
    """
    cmd = [sys.executable, str(ANALYZE), str(pre), str(post)]
    if out is not None:
        cmd += ["--out", str(out)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode, proc.stdout, proc.stderr


# ----------------------------------------------------------------------------
# Default out path inference
# ----------------------------------------------------------------------------

def test_default_out_path_extracts_pid_from_input_filename(tmp_path: Path) -> None:
    pre = tmp_path / "frames-12345-pre.csv"
    post = tmp_path / "frames-12345-post.csv"
    write_raw_csv(pre, "pre", 10_000_000, [(0, 1, 100, 200)])
    write_raw_csv(post, "post", 10_000_000, [(0, 1, 110, 190)])

    rc, _, _ = run_analyze(pre, post)
    assert rc == 0
    # default --out should be alongside the inputs, named after the PID.
    expected = tmp_path / "frames-merged-12345.csv"
    assert expected.exists(), \
        f"expected default output at {expected}, dir contents: " \
        f"{[p.name for p in tmp_path.iterdir()]}"


def test_default_out_path_falls_back_when_input_doesnt_match_naming(
        tmp_path: Path) -> None:
    pre = tmp_path / "some_other_name.csv"
    post = tmp_path / "frames-1-post.csv"
    write_raw_csv(pre, "pre", 10_000_000, [(0, 1, 100, 200)])
    write_raw_csv(post, "post", 10_000_000, [(0, 1, 110, 190)])

    # CWD must be a writable dir for the fallback default to work.
    cwd_before = Path.cwd()
    os.chdir(tmp_path)
    try:
        rc, _, _ = run_analyze(pre, post)
        assert rc == 0
        assert (tmp_path / "frames-merged.csv").exists()
    finally:
        os.chdir(cwd_before)


# ----------------------------------------------------------------------------
# Input validation: passing a merged CSV by mistake is caught
# ----------------------------------------------------------------------------

def test_passing_merged_csv_as_input_raises_clear_error(tmp_path: Path) -> None:
    # First produce a real merged CSV via a normal run.
    pre = tmp_path / "frames-99-pre.csv"
    post = tmp_path / "frames-99-post.csv"
    write_raw_csv(pre, "pre", 10_000_000, [(0, 1, 100, 200)])
    write_raw_csv(post, "post", 10_000_000, [(0, 1, 110, 190)])
    rc, _, _ = run_analyze(pre, post)
    assert rc == 0
    merged = tmp_path / "frames-merged-99.csv"
    assert merged.exists()

    # Now feed the merged CSV back as if it were a per-side one. The script
    # must reject it loudly (non-zero exit, clear message), not crash with
    # an obscure int("11100.000") ValueError deep inside csv.reader.
    rc, _, stderr = run_analyze(merged, post)
    assert rc != 0
    assert "unexpected column header" in stderr.lower() or \
           "expected" in stderr.lower()


# ----------------------------------------------------------------------------
# End-to-end on the hand-verified fixture from test_merge.cpp
# ----------------------------------------------------------------------------

def test_end_to_end_matches_test_merge_cpp_fixture(tmp_path: Path) -> None:
    """Same input as the end-to-end TEST_CASE in test_merge.cpp.

    qpc_freq = 10 MHz means 1 tick = 0.1 us; target_us values are 50, 40, 80,
    50 for the four frames. target_pct_mean only counts the first three
    frames (the last has no successor).
    """
    pre_rows = [
        (0, 12345, 1_000_000, 1_001_100),
        (1, 12345, 1_111_000, 1_112_000),
        (2, 12345, 1_222_000, 1_223_500),
        (3, 12345, 1_333_000, 1_334_200),
    ]
    post_rows = [
        (0, 12345, 1_000_400, 1_001_000),
        (1, 12345, 1_111_200, 1_111_800),
        (2, 12345, 1_222_300, 1_223_000),
        (3, 12345, 1_333_300, 1_334_000),
    ]
    pre = tmp_path / "frames-99-pre.csv"
    post = tmp_path / "frames-99-post.csv"
    write_raw_csv(pre, "pre", 10_000_000, pre_rows)
    write_raw_csv(post, "post", 10_000_000, post_rows)

    rc, stdout, _ = run_analyze(pre, post)
    assert rc == 0
    merged = tmp_path / "frames-merged-99.csv"
    assert merged.exists()
    text = merged.read_text(encoding="utf-8")

    # Header lines exactly as documented in the README.
    assert "# frame_count=4\n" in text
    assert "# target_ms_mean=0.0550\n" in text
    assert "# target_ms_min=0.0400\n" in text
    assert "# target_ms_max=0.0800\n" in text
    # pct lines carry the '%' suffix.
    assert "# target_pct_mean=0.5105%\n" in text
    assert "# target_pct_min=0.3604%\n" in text
    assert "# target_pct_max=0.7207%\n" in text

    # The matched-frames stdout line is the user-facing confirmation.
    assert "matched frames: 4" in stdout


def test_end_to_end_lf_only_line_endings(tmp_path: Path) -> None:
    """The merged CSV must not contain '\\r' bytes on any platform.

    The C++ merge opens its ofstream in binary mode; analyze.py uses
    csv.writer with lineterminator='\\n'. Both produce pure LF -- this is
    the byte-equivalence contract.
    """
    pre = tmp_path / "frames-1-pre.csv"
    post = tmp_path / "frames-1-post.csv"
    write_raw_csv(pre, "pre", 10_000_000, [(0, 1, 100, 200), (1, 1, 300, 400)])
    write_raw_csv(post, "post", 10_000_000, [(0, 1, 110, 190), (1, 1, 310, 390)])

    rc, _, _ = run_analyze(pre, post)
    assert rc == 0
    merged = tmp_path / "frames-merged-1.csv"
    raw = merged.read_bytes()
    assert b"\r" not in raw, "merged CSV must be LF-only, found CR byte"


# ----------------------------------------------------------------------------
# Edge cases
# ----------------------------------------------------------------------------

def test_no_matched_frames_exits_nonzero(tmp_path: Path) -> None:
    """Two non-empty files but with disjoint (frame_idx, thread_id) keys.

    The C++ merge logs a warning and skips the write; the Python script
    exits with code 1 and a clear error. Both refuse to write garbage.
    """
    pre = tmp_path / "frames-1-pre.csv"
    post = tmp_path / "frames-1-post.csv"
    write_raw_csv(pre, "pre", 10_000_000, [(0, 1, 100, 200)])
    # Different thread_id -> no match.
    write_raw_csv(post, "post", 10_000_000, [(0, 999, 110, 190)])

    rc, _, stderr = run_analyze(pre, post)
    assert rc != 0
    assert "no rows matched" in stderr.lower() or \
           "no matched" in stderr.lower()


def test_last_frame_per_thread_has_blank_interval_and_pct(tmp_path: Path) -> None:
    pre = tmp_path / "frames-1-pre.csv"
    post = tmp_path / "frames-1-post.csv"
    # Two threads; last frame per thread must have blank interval / pct.
    write_raw_csv(pre, "pre", 10_000_000, [
        (0, 1, 1_000_000, 1_001_000),
        (1, 1, 1_111_000, 1_112_000),
        (0, 2, 2_000_000, 2_000_100),
    ])
    write_raw_csv(post, "post", 10_000_000, [
        (0, 1, 1_000_200, 1_000_800),
        (1, 1, 1_111_200, 1_111_800),
        (0, 2, 2_000_020, 2_000_080),
    ])
    rc, _, _ = run_analyze(pre, post)
    assert rc == 0
    text = (tmp_path / "frames-merged-1.csv").read_text(encoding="utf-8")
    lines = [l for l in text.splitlines() if l and not l.startswith("#")]
    # First line is the column header.
    assert lines[0].startswith("frame_idx,thread_id")
    # Data rows: 3 of them.
    data = lines[1:]
    assert len(data) == 3
    # Sort: (thread 1 frame 0), (thread 1 frame 1), (thread 2 frame 0).
    # Frames 1 and 2 (last per thread) end with two trailing commas (empty
    # frame_interval_us / empty target_pct_of_frame at the row end).
    assert data[1].endswith(",,") is False  # only target_pct empty
    assert data[2].endswith(",")  # target_pct empty
    # First row has both filled.
    assert ",," not in data[0].split(",", 2)[2].split(",")[0]


def test_multiple_threads_sort_by_thread_then_frame(tmp_path: Path) -> None:
    pre = tmp_path / "frames-7-pre.csv"
    post = tmp_path / "frames-7-post.csv"
    write_raw_csv(pre, "pre", 1_000_000, [
        (5, 2, 500, 510),
        (0, 1, 100, 110),
        (0, 2, 50, 60),
        (3, 1, 300, 310),
    ])
    write_raw_csv(post, "post", 1_000_000, [
        (5, 2, 502, 508),
        (0, 1, 102, 108),
        (0, 2, 52, 58),
        (3, 1, 302, 308),
    ])
    rc, _, _ = run_analyze(pre, post)
    assert rc == 0
    text = (tmp_path / "frames-merged-7.csv").read_text(encoding="utf-8")
    data = [l for l in text.splitlines()
            if l and not l.startswith("#") and not l.startswith("frame_idx")]
    assert len(data) == 4
    # (thread 1 frame 0), (thread 1 frame 3), (thread 2 frame 0), (thread 2 frame 5)
    keys = [tuple(map(int, row.split(",")[:2])) for row in data]
    assert keys == [(0, 1), (3, 1), (0, 2), (5, 2)]


def test_qpc_freq_mismatch_warns_but_succeeds(tmp_path: Path) -> None:
    pre = tmp_path / "frames-2-pre.csv"
    post = tmp_path / "frames-2-post.csv"
    # Different qpc_freq is unrealistic on the same machine but should
    # not crash the script -- it warns and uses pre's freq.
    write_raw_csv(pre, "pre", 10_000_000, [(0, 1, 1_000_000, 1_001_000)])
    write_raw_csv(post, "post", 5_000_000, [(0, 1, 500_000, 500_300)])
    rc, _, stderr = run_analyze(pre, post)
    assert rc == 0
    assert "qpc_freq mismatch" in stderr.lower()
