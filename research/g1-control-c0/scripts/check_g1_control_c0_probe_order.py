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
  window:  boundary A (prepare -> update -> submit) is ordered by
           construction, and the post-completion update control is what it
           claims (update strictly after the CQE reap; Corrective-1 P1-2
           withdrew the old in-flight "retention" interpretation — the
           source and artifacts must not claim it);
  threaded: the prereg §5 park gate is ordered by construction — all
           workers ready BEFORE the measured span starts, released only
           AFTER the span ends (Corrective-1 P1-1).

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

    # ---- window: boundary A ordering + post-completion update control ----
    if "boundary_a_update_errno" not in src:
        fails.append("window: boundary-A update missing")
    if pos("boundary_a_update_errno") > pos("io_uring_submit(&ring) == 1"):
        fails.append("window: boundary-A submit precedes the slot update")
    # the evaluation marker (assignment) must come after the CQE reap
    if pos("io_uring_wait_cqe(D)") > pos("boundary_d_marker = 0x41"):
        fails.append("window: boundary-D marker evaluated before CQE")
    # Corrective-1 P1-2: the control step executes the update AFTER the
    # reap and must not claim in-flight retention anywhere. (The fdsB
    # update first appears in boundary A; search AFTER the D reap.)
    p_wait_d = pos("io_uring_wait_cqe(D)")
    p_upd_d = src.find("register_files_update(&ring, 0, fdsB, 1)",
                       max(p_wait_d, 0))
    if p_wait_d < 0 or p_upd_d < 0:
        fails.append("window: post-completion update control topology "
                     "missing (reap -> update)")
    elif p_upd_d > pos("boundary_d_marker = 0x41"):
        fails.append("window: post-completion update control does not "
                     "update after the reap (topology changed)")
    if "BOUNDARY-D RETENTION CONFIRMED" in src:
        fails.append("window: withdrawn in-flight retention claim still "
                     "present in source")
    if "POST-COMPLETION UPDATE CONTROL" not in src:
        fails.append("window: post-completion update control label missing")

    # ---- threaded park gate ordering (Corrective-1 P1-1) ----------------
    # all-ready wait BEFORE span start; release AFTER span end.
    p_ready = pos("gate_wait_ready(&gate")
    p_t0 = src.find("t0 = now_ns", p_ready) if p_ready >= 0 else -1
    p_end = src.find("transfer_ns = now_ns() - t0", p_t0) if p_t0 >= 0 else -1
    p_release = src.find("gate_release(&gate", p_end) if p_end >= 0 else -1
    if min(p_ready, p_t0, p_end, p_release) < 0:
        fails.append("threaded: park/release gate markers missing")
    elif not (p_ready < p_t0 < p_end < p_release):
        fails.append("threaded: gate ordering violated (ready must precede "
                     "span start; release must follow span end)")

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
    if (sd / "raw" / "replacement-window.json").is_file():
        w = json.loads((sd / "raw" / "replacement-window.json").read_text())
        dv = w.get("boundary_d_verdict", "")
        if "RETENTION CONFIRMED" in dv:
            fails.append("executed window artifact: withdrawn in-flight "
                         "retention claim still present")
        if "POST-COMPLETION UPDATE CONTROL" not in dv:
            fails.append("executed window artifact: post-completion update "
                         "control label missing")

    if fails:
        print(f"PROBE ORDER FAIL ({len(fails)}):")
        for f in fails:
            print(f"  - {f}")
        sys.exit(1)
    print(f"PROBE ORDER PASS: session {sid} — structural + executed ordering "
          f"checks clean.")


if __name__ == "__main__":
    main()
