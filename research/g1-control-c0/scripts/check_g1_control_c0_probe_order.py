#!/usr/bin/env python3
"""check_g1_control_c0_probe_order.py — probe ordering guard (#279).

RBUF-E0 lesson: a capability probe that co-submits dependent operations can
race. This guard verifies STRUCTURALLY (source, whitespace-normalized) and
from the executed session artifacts that:

  probe:   the fixed read is submitted, its CQE reaped and content
           validated BEFORE the fixed write SQE is prepared (no
           IOSQE_IO_LINK, no co-submission);
  fileid:  each witness step is serialized on CQE reaping (dup2 before the
           stale-fd read; registration before the fixed read; the
           replacement update before its read);
  window:  boundary A (prepare -> update -> submit) and boundary D
           (submit -> reap -> update) are ordered by construction.

Usage: python3 check_g1_control_c0_probe_order.py <session-id>
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
BENCH_SRC = REPO / "bench/g1_control_c0_bench.cpp"
RESULTS = REPO / "research/g1-control-c0/results"


def flat(s: str) -> str:
    """Whitespace-normalized source: comparison text must not depend on
    line breaks / indentation."""
    return re.sub(r"\s+", " ", s)


def main() -> None:
    if len(sys.argv) != 2:
        print("usage: check_g1_control_c0_probe_order.py <session-id>",
              file=sys.stderr)
        sys.exit(2)
    sid = sys.argv[1]
    fails: list[str] = []
    src = flat(BENCH_SRC.read_text())

    def pos(needle: str) -> int:
        return src.find(needle)

    # ---- probe: read-before-write, no IO_LINK (scoped to run_probe body) -
    probe_body = src[src.find("int run_probe"):src.find("// ---- formal run")]
    if "probe write submit" not in src:
        fails.append("probe: write submission missing")
    if "probe read submit" not in src:
        fails.append("probe: read submission missing")
    if pos("probe read submit") > pos("probe write submit"):
        fails.append("probe: write submit precedes read submit")
    if pos("probe read submit") > pos("io_uring_wait_cqe(probe read)"):
        fails.append("probe: read submit precedes its CQE wait")
    if "IOSQE_IO_LINK" in probe_body:
        fails.append("probe: uses IOSQE_IO_LINK (ordering not structural)")

    # ---- fileid: serialized witness steps ----
    if "dup2(B, N)" not in src:
        fails.append("fileid: dup2-forced reuse missing")
    if pos("dup2(B, N)") > pos("read_first_page(&ring, fdN"):
        fails.append("fileid: stale-fd read precedes dup2 reuse")
    if pos("io_uring_register_files(&ring, fds, 1)") > pos(
            "fixed_marker = read_first_page"):
        fails.append("fileid: fixed read precedes registration")
    if pos("io_uring_register_files_update(&ring, 0, fds2, 1)") > pos(
            "replaced_marker = read_first_page"):
        fails.append("fileid: replaced read precedes the slot update")

    # ---- window: boundary A and D ordering ----
    if "boundary_a_update_errno" not in src:
        fails.append("window: boundary-A update missing")
    if pos("boundary_a_update_errno") > pos("io_uring_submit(&ring) == 1"):
        fails.append("window: boundary-A submit precedes the slot update")
    # the evaluation marker (assignment) must come after the CQE reap
    if pos("io_uring_wait_cqe(D)") > pos("boundary_d_marker = 0x41"):
        fails.append("window: boundary-D marker evaluated before CQE")

    # ---- executed-artifact checks ----
    sd = RESULTS / sid
    if not (sd / "raw" / "probe.json").is_file():
        fails.append("session lacks raw/probe.json (probe not executed)")
    else:
        cap = json.loads((sd / "raw" / "probe.json").read_text())
        if not cap.get("write_submitted_after_read_cqe"):
            fails.append("executed probe: write not after read CQE")
        if not cap.get("capable"):
            fails.append("executed probe: not capable")
    for art in ("fileid.json", "replacement-window.json"):
        if not (sd / "raw" / art).is_file():
            fails.append(f"session lacks raw/{art}")

    if fails:
        print(f"PROBE ORDER FAIL ({len(fails)}):")
        for f in fails:
            print(f"  - {f}")
        sys.exit(1)
    print(f"PROBE ORDER PASS: session {sid} — structural + executed ordering "
          f"checks clean.")


if __name__ == "__main__":
    main()
