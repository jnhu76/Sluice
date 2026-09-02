#!/usr/bin/env python3
"""check_rbuf_e0_analysis.py — RBUF-E0 analysis hygiene + causal-isolation
regression checks (#272 adversarial-review remediation). Read-only over the
immutable sessions; exits nonzero on the first failed class.

  A. setup_plus_teardown_fraction identity: for every amortization horizon
     row with data, the stored fraction equals the frozen formula recomputed
     from raw runs.jsonl — median(setup_ns + teardown_ns) /
     median(end-to-end), rounded to 5 decimals.
  B. lifecycle reports ABSOLUTE registration timings (register_ns_median /
     unregister_ns_median) — no derived lifecycle ratio is required, and
     no `registration_fraction` field exists anywhere in analysis.json.
  C. U1/U2 causal isolation recomputed from raw runs.jsonl: identical
     aligned storage evidence (align_remainder == 0, slot_stride == chunk),
     identical ring geometry and identical gated same-work counters per
     cell across arms; only U2 carries the registration fields.
  D. verdicts stay inside the frozen prereg vocabulary.

Frozen constants are pinned here independently of the driver on purpose:
the check must not drift when the driver is refactored.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
RESULTS = REPO / "research/rbuf-e0/results"

FILE_BYTES = 1_073_741_824
HORIZONS = [1, 4, 16, 64]
PRIMARY_CELL = (2_097_152, 2)
CELLS = [(524_288, 2), (1_048_576, 2), (2_097_152, 1), (2_097_152, 2),
         (2_097_152, 4), (4_194_304, 2)]

STEADY_VOCAB = {
    "REGISTERED BUFFER STEADY-STATE MATERIAL",
    "REGISTERED BUFFER REGIME-SPECIFIC",
    "REGISTERED BUFFER STEADY-STATE NOT MATERIAL",
    "REGISTERED BUFFER MIXED / UNSTABLE",
    "URING QUALIFICATION FAILED — #262 BLOCKING",
}
AMORT_VOCAB = {
    "AMORTIZATION CROSSOVER LOCATED @ H=1", "AMORTIZATION CROSSOVER LOCATED "
    "@ H=4", "AMORTIZATION CROSSOVER LOCATED @ H=16",
    "AMORTIZATION CROSSOVER LOCATED @ H=64",
    "AMORTIZATION CROSSOVER NOT LOCATED IN TESTED RANGE",
    "REGISTRATION NEVER RECOVERS SETUP COST IN TESTED RANGE",
    "N/A — no amortization runs in this session",
}


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    sys.exit(1)


def median(vals):
    s = sorted(vals)
    return s[len(s) // 2]


def load_runs(session: str) -> list[dict]:
    raw = RESULTS / session / "raw" / "runs.jsonl"
    return [json.loads(l) for l in raw.read_text().splitlines() if l.strip()]


def ok_runs(session: str) -> list[dict]:
    return [r for r in load_runs(session) if r.get("ok")]


def check_fraction(session: str, analysis: dict) -> None:
    runs = ok_runs(session)
    amort: dict[tuple, list[dict]] = {}
    for r in runs:
        amort.setdefault((r["arm"], r["transfers"]), []).append(r)
    rows = analysis["amortization"]["horizons"]
    if len(rows) != len(HORIZONS):
        fail(f"{session}: {len(rows)} horizon rows != {len(HORIZONS)}")
    for row in rows:
        h = row["horizon_transfers"]
        for arm in ("u1", "u2"):
            stored = row.get(arm)
            rs = amort.get((arm.upper(), h), [])
            if not rs:
                if stored is not None:
                    fail(f"{session} H={h} {arm}: stored value without raw "
                         f"runs")
                continue
            costs = [r["bench"]["setup_ns"] + r["bench"]["teardown_ns"] +
                     sum(r["bench"]["transfer_ns"]) for r in rs]
            num = median([r["bench"]["setup_ns"] + r["bench"]["teardown_ns"]
                          for r in rs])
            expect = round(num / median(costs), 5) if median(costs) else 0
            if stored["setup_plus_teardown_fraction"] != expect:
                fail(f"{session} H={h} {arm}: setup_plus_teardown_fraction "
                     f"{stored['setup_plus_teardown_fraction']} != "
                     f"recomputed {expect}")
            if stored["end_to_end_ns_median"] != median(costs):
                fail(f"{session} H={h} {arm}: end_to_end_ns_median "
                     f"{stored['end_to_end_ns_median']} != "
                     f"recomputed {median(costs)}")
    print(f"PASS fraction-identity  {session}: every stored "
          f"setup_plus_teardown_fraction equals the frozen formula "
          f"recomputed from raw runs")


def check_lifecycle(session: str, analysis: dict) -> None:
    if "registration_fraction" in json.dumps(analysis):
        fail(f"{session}: forbidden derived lifecycle ratio "
             f"`registration_fraction` present")
    lc = analysis.get("lifecycle", {})
    u2 = lc.get("U2")
    if not u2:
        fail(f"{session}: lifecycle has no U2 entry")
    if u2.get("register_ns_median") is None or \
            u2.get("unregister_ns_median") is None:
        fail(f"{session}: lifecycle must report ABSOLUTE register_ns/"
             f"unregister_ns for U2")
    if u2.get("registered_bytes") != \
            PRIMARY_CELL[0] * PRIMARY_CELL[1]:
        fail(f"{session}: U2 registered_bytes != chunk*depth")
    print(f"PASS lifecycle-absolute {session}: register_ns="
          f"{u2['register_ns_median']} unregister_ns="
          f"{u2['unregister_ns_median']} (absolute ns; no ratio field)")


def check_causal_isolation(session: str, analysis: dict) -> None:
    runs = ok_runs(session)
    # Group by (chunk, depth, transfers): steady runs (transfers=1) and every
    # amortization horizon must each show U1/U2 same-work equality.
    groups: dict[tuple, dict[str, list[dict]]] = {}
    for r in runs:
        groups.setdefault((r["chunk"], r["depth"], r["transfers"]), {}) \
            .setdefault(r["arm"], []).append(r)
    compared = 0
    for (chunk, depth, transfers), arms in groups.items():
        if "U1" not in arms or "U2" not in arms:
            continue
        compared += 1
        label = f"{chunk}x{depth} H={transfers}"
        for run in arms["U1"] + arms["U2"]:
            b = run["bench"]
            if b["align_remainder"] != 0 or b["slot_stride"] != chunk:
                fail(f"{session} {label}: aligned-storage evidence "
                     f"broken (align={b['align_remainder']}, "
                     f"stride={b['slot_stride']})")
        def sig(b: dict) -> tuple:
            return (b["ring_entries_requested"], b["ring_entries"],
                    b["chunks_per_transfer"], b["read_ops"], b["write_ops"],
                    b["cqe_count"], b["bytes_read"], b["bytes_written"])
        s1 = {sig(r["bench"]) for r in arms["U1"]}
        s2 = {sig(r["bench"]) for r in arms["U2"]}
        if len(s1) != 1 or len(s2) != 1 or s1 != s2:
            fail(f"{session} {label}: U1/U2 ring geometry or same-work "
                 f"counters diverge ({s1} vs {s2})")
        for r in arms["U1"]:
            b = r["bench"]
            if b["registered_buffers"] != 0 or b["registered_bytes"] != 0:
                fail(f"{session} {label}: U1 carries registration state")
        for r in arms["U2"]:
            b = r["bench"]
            if b["registered_buffers"] != depth or \
                    b["registered_bytes"] != chunk * depth:
                fail(f"{session} {label}: U2 registration fields wrong")
    if compared == 0:
        fail(f"{session}: no U1-vs-U2 comparison group found in raw runs")
    print(f"PASS causal-isolation {session}: U1/U2 identical on aligned "
          f"storage, ring geometry and same-work counters in all "
          f"{compared} U1-vs-U2 groups; only registered_buffers/"
          f"registered_bytes (+ fixed opcodes) differ")


def check_verdicts(session: str, analysis: dict) -> None:
    steady = analysis.get("steady_state_verdict")
    has_steady = any(c.get("u1_vs_u2") for c in analysis["cells"])
    if has_steady:
        if steady not in STEADY_VOCAB:
            fail(f"{session}: steady verdict outside frozen vocabulary: "
                 f"{steady!r}")
    elif steady != "REGISTERED BUFFER MIXED / UNSTABLE (no U2 data)":
        # Steady-free sessions carry the driver's no-data marker, not a
        # primary verdict; the primary verdict lives in the steady session.
        fail(f"{session}: steady-free session must carry the '(no U2 data)' "
             f"marker, got {steady!r}")
    if analysis["amortization"]["verdict"] not in AMORT_VOCAB:
        fail(f"{session}: amortization verdict outside frozen vocabulary: "
             f"{analysis['amortization']['verdict']!r}")
    print(f"PASS verdict-vocab    {session}: both verdicts inside the "
          f"frozen prereg vocabulary")


def main() -> None:
    sessions = sys.argv[1:] or [
        d.name for d in sorted(RESULTS.iterdir()) if d.is_dir() and
        (d / "analysis.json").is_file()]
    if not sessions:
        fail("no RBUF-E0 sessions with analysis.json found")
    for session in sessions:
        analysis = json.loads(
            (RESULTS / session / "analysis.json").read_text())
        has_amort = any(h.get("u1") and h.get("u2")
                        for h in analysis["amortization"]["horizons"])
        if has_amort:
            check_fraction(session, analysis)
        else:
            print(f"SKIP fraction-identity  {session}: no amortization "
                  f"data (steady-only session)")
        if analysis.get("lifecycle"):
            check_lifecycle(session, analysis)
        check_causal_isolation(session, analysis)
        check_verdicts(session, analysis)
    print("check_rbuf_e0_analysis: ALL CHECKS PASS")


if __name__ == "__main__":
    main()
