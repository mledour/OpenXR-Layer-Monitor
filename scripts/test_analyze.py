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
        fh.write("display_time,thread_id,qpc_entry,qpc_exit\n")
        for fi, tid, qe, qx in rows:
            fh.write(f"{fi},{tid},{qe},{qx}\n")


def write_gpu_csv(path: Path, side: str,
                  rows: list[tuple[int, int, int, int]]) -> None:
    """Mirror layer.cpp's GpuCsvSink output format.

    Rows: (display_time, gpu_ticks, gpu_freq, valid). No file-level frequency
    header -- gpu_freq lives in the per-row column (each disjoint query
    reports its own clock rate at submission time).
    """
    with path.open("w", encoding="utf-8") as fh:
        fh.write("# gpu_clock=d3d11\n")
        fh.write(f"# side={side}\n")
        fh.write(f"# layer=XR_APILAYER_MLEDOUR_layer_monitor_{side}\n")
        fh.write("# fn=xrEndFrame\n")
        fh.write("display_time,gpu_ticks,gpu_freq,valid\n")
        for fi, ticks, freq, valid in rows:
            fh.write(f"{fi},{ticks},{freq},{valid}\n")


def run_analyze(pre: Path, post: Path, out: Path | None = None,
                cwd: Path | None = None) -> tuple[int, str, str]:
    """Run analyze.py as a subprocess. Returns (returncode, stdout, stderr).

    Subprocess (rather than direct import) keeps the test honest about how
    users actually invoke the script -- argparse, stderr warnings, exit
    codes all come through unchanged.

    encoding='utf-8' is explicit so the test does not blow up on a
    Windows runner whose locale.getpreferredencoding() is cp1252 the
    moment analyze.py grows a non-ASCII character in any error message.

    cwd is forwarded to subprocess.run rather than mutating the parent
    process's working directory via os.chdir, so pytest-xdist (or any
    future concurrent test) cannot race on a global resource.
    """
    cmd = [sys.executable, str(ANALYZE), str(pre), str(post)]
    if out is not None:
        cmd += ["--out", str(out)]
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        cwd=str(cwd) if cwd is not None else None,
    )
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

    # Forward cwd= to subprocess instead of mutating the test process's
    # working directory -- the latter races with pytest-xdist workers and
    # any other test that reads Path.cwd().
    rc, _, _ = run_analyze(pre, post, cwd=tmp_path)
    assert rc == 0
    assert (tmp_path / "frames-merged.csv").exists()


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
    50 for the four frames. target_cpu_pct_mean only counts the first three
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
    assert "# target_cpu_ms_mean=0.0550\n" in text
    assert "# target_cpu_ms_min=0.0400\n" in text
    assert "# target_cpu_ms_max=0.0800\n" in text
    # pct lines carry the '%' suffix.
    assert "# target_cpu_pct_mean=0.5105%\n" in text
    assert "# target_cpu_pct_min=0.3604%\n" in text
    assert "# target_cpu_pct_max=0.7207%\n" in text
    # No GPU CSVs were written by this test, so the GPU stat block is all
    # zeros and gpu_frame_count is 0. The 7 lines must still appear so the
    # merged-CSV schema stays uniform across D3D11 / non-D3D11 sessions.
    assert "# gpu_frame_count=0\n" in text
    assert "# target_gpu_ms_mean=0.0000\n" in text
    assert "# target_gpu_ms_min=0.0000\n" in text
    assert "# target_gpu_ms_max=0.0000\n" in text
    assert "# target_gpu_pct_mean=0.0000%\n" in text
    assert "# target_gpu_pct_min=0.0000%\n" in text
    assert "# target_gpu_pct_max=0.0000%\n" in text

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
    """Two non-empty files but with disjoint (display_time, thread_id) keys.

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

    # Sort order produced by analyze.py is (thread_id, display_time):
    #   row 0 = thread 1, frame 0  (has successor on thread 1)
    #   row 1 = thread 1, frame 1  (no successor on thread 1)
    #   row 2 = thread 2, frame 0  (only frame on thread 2)
    # Columns: display_time, thread_id, frame_interval_us, pre_us, post_us,
    #          target_us, target_cpu_pct_of_frame.
    rows = [
        line.split(",")
        for line in text.splitlines()
        if line and not line.startswith("#") and not line.startswith("display_time")
    ]
    assert len(rows) == 3

    # Row 0 (thread 1, frame 0): has successor -> interval AND pct populated.
    assert rows[0][:2] == ["0", "1"]
    assert rows[0][2] != "", "thread1/frame0 must have a frame_interval_us"
    assert rows[0][6] != "", "thread1/frame0 must have a target_cpu_pct_of_frame"

    # Row 1 (thread 1, frame 1): no successor on thread 1 -> both blank.
    assert rows[1][:2] == ["1", "1"]
    assert rows[1][2] == "", "thread1/frame1 must have empty frame_interval_us"
    assert rows[1][6] == "", "thread1/frame1 must have empty target_cpu_pct_of_frame"

    # Row 2 (thread 2, frame 0): only frame on thread 2 -> both blank.
    assert rows[2][:2] == ["0", "2"]
    assert rows[2][2] == "", "thread2/frame0 must have empty frame_interval_us"
    assert rows[2][6] == "", "thread2/frame0 must have empty target_cpu_pct_of_frame"


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
            if l and not l.startswith("#") and not l.startswith("display_time")]
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


# ----------------------------------------------------------------------------
# GPU join (D3D11 sandwich)
# ----------------------------------------------------------------------------


def _data_rows(text: str) -> list[list[str]]:
    """Return only the data rows of a merged CSV (skip # lines + column header)."""
    return [
        line.split(",")
        for line in text.splitlines()
        if line and not line.startswith("#") and not line.startswith("display_time")
    ]


def test_gpu_join_populates_target_gpu_us_when_files_present(tmp_path: Path) -> None:
    """Drop sibling gpu-<pid>-{pre,post}.csv files and verify the join fills
    target_gpu_us on every frame whose GPU sample is valid on both sides,
    that the gpu_frame_count + ms stats land at the documented .4f rounding,
    and that the column header carries the new target_gpu_us suffix."""
    pid = "9001"
    pre = tmp_path / f"frames-{pid}-pre.csv"
    post = tmp_path / f"frames-{pid}-post.csv"
    write_raw_csv(pre, "pre", 1_000_000, [
        (0, 100, 1000, 2000),  # pre_us = 1000
        (1, 100, 2000, 3000),  # pre_us = 1000
    ])
    write_raw_csv(post, "post", 1_000_000, [
        (0, 100, 1200, 1700),  # post_us = 500 -> target = 500
        (1, 100, 2200, 2600),  # post_us = 400 -> target = 600
    ])
    # 1 GHz GPU clock so a 1 us delta = 1000 ticks.
    write_gpu_csv(tmp_path / f"gpu-{pid}-pre.csv", "pre", [
        (0, 10_000, 1_000_000_000, 1),
        (1, 20_000, 1_000_000_000, 1),
    ])
    write_gpu_csv(tmp_path / f"gpu-{pid}-post.csv", "post", [
        (0, 15_000, 1_000_000_000, 1),  # delta 5000 ticks = 5 us
        (1, 28_000, 1_000_000_000, 1),  # delta 8000 ticks = 8 us
    ])

    rc, _, _ = run_analyze(pre, post)
    assert rc == 0
    text = (tmp_path / f"frames-merged-{pid}.csv").read_text(encoding="utf-8")

    # Column header gained the target_gpu_us + target_gpu_pct_of_frame columns.
    assert "display_time,thread_id,frame_interval_us,pre_us,post_us,target_us," \
           "target_cpu_pct_of_frame,target_gpu_us,target_gpu_pct_of_frame\n" in text

    # GPU stats over the two valid samples (5 us + 8 us = 13 us; mean 6.5 us
    # = 0.0065 ms). Both extremes hit gpu_ms_min / gpu_ms_max.
    assert "# gpu_frame_count=2\n" in text
    assert "# target_gpu_ms_mean=0.0065\n" in text
    assert "# target_gpu_ms_min=0.0050\n" in text
    assert "# target_gpu_ms_max=0.0080\n" in text
    # GPU pct: only frame 0 has an interval (1000 us), so 5/1000*100 = 0.5000%
    # is the sole contributing sample -> mean = min = max. Frame 1 is the last
    # per thread (no interval), so it contributes no GPU pct.
    assert "# target_gpu_pct_mean=0.5000%\n" in text
    assert "# target_gpu_pct_min=0.5000%\n" in text
    assert "# target_gpu_pct_max=0.5000%\n" in text

    rows = _data_rows(text)
    assert len(rows) == 2
    # target_gpu_us is index 7; target_gpu_pct_of_frame is the new last col (8).
    assert rows[0][7] == "5.000"
    assert rows[1][7] == "8.000"
    assert rows[0][8] == "0.5000", "frame 0 has an interval -> GPU pct"
    assert rows[1][8] == "", "frame 1 is last per thread -> blank GPU pct"


def test_gpu_absent_emits_zero_stats_and_blank_column(tmp_path: Path) -> None:
    """No GPU CSVs at all (non-D3D11 host). The 7 GPU header lines must
    still appear so the schema is uniform, but they all read zero and every
    target_gpu_us cell is blank."""
    pid = "9002"
    pre = tmp_path / f"frames-{pid}-pre.csv"
    post = tmp_path / f"frames-{pid}-post.csv"
    write_raw_csv(pre, "pre", 1_000_000, [(0, 1, 1000, 2000)])
    write_raw_csv(post, "post", 1_000_000, [(0, 1, 1200, 1700)])

    rc, _, _ = run_analyze(pre, post)
    assert rc == 0
    text = (tmp_path / f"frames-merged-{pid}.csv").read_text(encoding="utf-8")

    assert "# gpu_frame_count=0\n" in text
    assert "# target_gpu_ms_mean=0.0000\n" in text
    assert "# target_gpu_ms_min=0.0000\n" in text
    assert "# target_gpu_ms_max=0.0000\n" in text

    rows = _data_rows(text)
    assert len(rows) == 1
    # The CSV row keeps the trailing comma + empty cell so the column count
    # is uniform on every row.
    assert rows[0][7] == ""


def test_gpu_invalid_row_blanks_target_gpu_us(tmp_path: Path) -> None:
    """A valid=0 row on EITHER side (disjoint clock) blanks that frame."""
    pid = "9003"
    pre = tmp_path / f"frames-{pid}-pre.csv"
    post = tmp_path / f"frames-{pid}-post.csv"
    write_raw_csv(pre, "pre", 1_000_000, [(0, 1, 1000, 2000), (1, 1, 2000, 3000)])
    write_raw_csv(post, "post", 1_000_000, [(0, 1, 1200, 1700), (1, 1, 2200, 2600)])
    write_gpu_csv(tmp_path / f"gpu-{pid}-pre.csv", "pre", [
        (0, 10_000, 1_000_000_000, 1),
        (1, 20_000, 1_000_000_000, 0),  # pre invalid -> frame 1 blank
    ])
    write_gpu_csv(tmp_path / f"gpu-{pid}-post.csv", "post", [
        (0, 15_000, 1_000_000_000, 1),
        (1, 28_000, 1_000_000_000, 1),  # post valid alone is not enough
    ])

    rc, _, _ = run_analyze(pre, post)
    assert rc == 0
    text = (tmp_path / f"frames-merged-{pid}.csv").read_text(encoding="utf-8")
    rows = _data_rows(text)
    assert len(rows) == 2
    assert rows[0][7] == "5.000"
    assert rows[1][7] == ""
    # Only one valid GPU sample feeds the aggregates.
    assert "# gpu_frame_count=1\n" in text


def test_gpu_freq_mismatch_blanks_target_gpu_us(tmp_path: Path) -> None:
    """Different gpu_freq between the two sides means the GPU clock was
    disjoint across the measured span -- we can't trust the delta."""
    pid = "9004"
    pre = tmp_path / f"frames-{pid}-pre.csv"
    post = tmp_path / f"frames-{pid}-post.csv"
    write_raw_csv(pre, "pre", 1_000_000, [(0, 1, 1000, 2000)])
    write_raw_csv(post, "post", 1_000_000, [(0, 1, 1200, 1700)])
    write_gpu_csv(tmp_path / f"gpu-{pid}-pre.csv", "pre", [
        (0, 10_000, 1_000_000_000, 1),
    ])
    write_gpu_csv(tmp_path / f"gpu-{pid}-post.csv", "post", [
        (0, 15_000, 2_000_000_000, 1),  # different freq -> blank
    ])

    rc, _, _ = run_analyze(pre, post)
    assert rc == 0
    text = (tmp_path / f"frames-merged-{pid}.csv").read_text(encoding="utf-8")
    rows = _data_rows(text)
    assert rows[0][7] == ""
    assert "# gpu_frame_count=0\n" in text


def test_gpu_backwards_counter_blanks_target_gpu_us(tmp_path: Path) -> None:
    """post.gpu_ticks < pre.gpu_ticks is a driver bug -- treating the
    unsigned wrap as a huge delta would poison stats. We blank it."""
    pid = "9005"
    pre = tmp_path / f"frames-{pid}-pre.csv"
    post = tmp_path / f"frames-{pid}-post.csv"
    write_raw_csv(pre, "pre", 1_000_000, [(0, 1, 1000, 2000)])
    write_raw_csv(post, "post", 1_000_000, [(0, 1, 1200, 1700)])
    write_gpu_csv(tmp_path / f"gpu-{pid}-pre.csv", "pre", [
        (0, 20_000, 1_000_000_000, 1),
    ])
    write_gpu_csv(tmp_path / f"gpu-{pid}-post.csv", "post", [
        (0, 10_000, 1_000_000_000, 1),  # less than pre -> blank
    ])

    rc, _, _ = run_analyze(pre, post)
    assert rc == 0
    text = (tmp_path / f"frames-merged-{pid}.csv").read_text(encoding="utf-8")
    rows = _data_rows(text)
    assert rows[0][7] == ""


def test_gpu_malformed_header_warns_and_continues_cpu_only(tmp_path: Path) -> None:
    """A garbled GPU column header MUST NOT abort the CPU merge -- the C++
    behaviour is to log + skip. analyze.py matches: warns on stderr,
    returns 0, leaves target_gpu_us blank."""
    pid = "9006"
    pre = tmp_path / f"frames-{pid}-pre.csv"
    post = tmp_path / f"frames-{pid}-post.csv"
    write_raw_csv(pre, "pre", 1_000_000, [(0, 1, 1000, 2000)])
    write_raw_csv(post, "post", 1_000_000, [(0, 1, 1200, 1700)])
    # Hand-write a garbled GPU pre with a wrong column header so
    # load_gpu returns None instead of raising.
    bad = tmp_path / f"gpu-{pid}-pre.csv"
    with bad.open("w", encoding="utf-8") as fh:
        fh.write("# gpu_clock=d3d11\n")
        fh.write("# side=pre\n")
        fh.write("wrong,column,header,line\n")
        fh.write("0,10000,1000000000,1\n")
    write_gpu_csv(tmp_path / f"gpu-{pid}-post.csv", "post", [
        (0, 15_000, 1_000_000_000, 1),
    ])

    rc, _, stderr = run_analyze(pre, post)
    assert rc == 0
    assert "unexpected gpu column header" in stderr.lower()
    text = (tmp_path / f"frames-merged-{pid}.csv").read_text(encoding="utf-8")
    rows = _data_rows(text)
    # CPU merge proceeded; GPU column is blank because the pre side was
    # skipped. frame_interval_us is blank too because this is the only frame
    # on its thread (no successor -> no interval).
    assert rows[0][:6] == ["0", "1", "", "1000.000", "500.000", "500.000"]
    assert rows[0][7] == ""
    assert "# gpu_frame_count=0\n" in text


def test_gpu_only_one_side_present_blanks_target_gpu_us(tmp_path: Path) -> None:
    """A frame that has a GPU row on only one side cannot be joined.
    Verifies the join skips it (rather than crashing with KeyError)."""
    pid = "9007"
    pre = tmp_path / f"frames-{pid}-pre.csv"
    post = tmp_path / f"frames-{pid}-post.csv"
    write_raw_csv(pre, "pre", 1_000_000, [(0, 1, 1000, 2000), (1, 1, 2000, 3000)])
    write_raw_csv(post, "post", 1_000_000, [(0, 1, 1200, 1700), (1, 1, 2200, 2600)])
    # Only the pre side has a GPU row for frame 1; the post side has no row.
    # Symmetrically, frame 0 has only a post GPU row, no pre.
    write_gpu_csv(tmp_path / f"gpu-{pid}-pre.csv", "pre", [
        (1, 20_000, 1_000_000_000, 1),
    ])
    write_gpu_csv(tmp_path / f"gpu-{pid}-post.csv", "post", [
        (0, 15_000, 1_000_000_000, 1),
    ])

    rc, _, _ = run_analyze(pre, post)
    assert rc == 0
    text = (tmp_path / f"frames-merged-{pid}.csv").read_text(encoding="utf-8")
    rows = _data_rows(text)
    assert rows[0][7] == ""
    assert rows[1][7] == ""
    assert "# gpu_frame_count=0\n" in text


def test_gpu_lf_only_line_endings(tmp_path: Path) -> None:
    """The full merged CSV (with GPU column populated) must remain LF-only,
    same byte-equivalence contract as the CPU-only case."""
    pid = "9008"
    pre = tmp_path / f"frames-{pid}-pre.csv"
    post = tmp_path / f"frames-{pid}-post.csv"
    write_raw_csv(pre, "pre", 1_000_000, [(0, 1, 1000, 2000)])
    write_raw_csv(post, "post", 1_000_000, [(0, 1, 1200, 1700)])
    write_gpu_csv(tmp_path / f"gpu-{pid}-pre.csv", "pre", [
        (0, 10_000, 1_000_000_000, 1),
    ])
    write_gpu_csv(tmp_path / f"gpu-{pid}-post.csv", "post", [
        (0, 15_000, 1_000_000_000, 1),
    ])

    rc, _, _ = run_analyze(pre, post)
    assert rc == 0
    raw = (tmp_path / f"frames-merged-{pid}.csv").read_bytes()
    assert b"\r" not in raw, "merged CSV with GPU column must remain LF-only"


# ----------------------------------------------------------------------------
# Tolerant parsing (#8): malformed input degrades, it does not crash
# ----------------------------------------------------------------------------

def _write_lines(path: Path, lines: list[str]) -> None:
    with path.open("w", encoding="utf-8") as fh:
        fh.write("".join(lines))


def test_malformed_qpc_freq_header_falls_back_instead_of_crashing(
        tmp_path: Path) -> None:
    """A non-integer qpc_freq value ("1e7") must NOT crash the analyzer with a
    ValueError; it falls back to the 10 MHz default, matching merge.cpp's
    StrictStoll. Both sides fall back to the same default, so the merge still
    produces output."""
    header = ("# qpc_freq=1e7\n# side={side}\n# layer=test\n# fn=xrEndFrame\n"
              "display_time,thread_id,qpc_entry,qpc_exit\n")
    pre = tmp_path / "frames-810-pre.csv"
    post = tmp_path / "frames-810-post.csv"
    _write_lines(pre, [header.format(side="pre"), "100,1,1000000,1001000\n"])
    _write_lines(post, [header.format(side="post"), "100,1,1000400,1001000\n"])

    rc, _, stderr = run_analyze(pre, post)
    assert rc == 0, f"analyzer crashed on malformed qpc_freq: {stderr}"
    assert (tmp_path / "frames-merged-810.csv").exists()
    assert "qpc_freq" in stderr.lower()  # warned about the fallback


def test_truncated_data_row_is_skipped_instead_of_crashing(
        tmp_path: Path) -> None:
    """A half-flushed / short data row must be skipped (not crash), matching
    the C++ merge which drops rows whose four fields don't all parse."""
    header = ("# qpc_freq=10000000\n# side={side}\n# layer=test\n"
              "# fn=xrEndFrame\ndisplay_time,thread_id,qpc_entry,qpc_exit\n")
    pre = tmp_path / "frames-811-pre.csv"
    post = tmp_path / "frames-811-post.csv"
    _write_lines(pre, [
        header.format(side="pre"),
        "100,1,1000000,1001000\n",
        "101,1,1010000\n",            # truncated (3 fields) -> skipped
        "102,1,1020000,1021000\n",
    ])
    _write_lines(post, [
        header.format(side="post"),
        "100,1,1000400,1001000\n",
        "102,1,1020400,1021000\n",
    ])

    rc, _, stderr = run_analyze(pre, post)
    assert rc == 0, f"analyzer crashed on a truncated row: {stderr}"
    text = (tmp_path / "frames-merged-811.csv").read_text(encoding="utf-8")
    # Only the two well-formed frames (100, 102) survive; 101 was dropped.
    assert "# frame_count=2\n" in text
    assert "skipped" in stderr.lower()
