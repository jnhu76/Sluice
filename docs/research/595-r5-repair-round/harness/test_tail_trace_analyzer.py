#!/usr/bin/env python3
"""Deterministic counterexamples for the v2 tail-trace analyzer (review R4).

Builds tiny synthetic v2 dumps and asserts the analyzer's failure handling:

  case_good       two ops built and admitted in order -> exit 0, all join=ok;
  case_reject     first build rejected, only the second admitted -> the old
                  analyzer misbound built[0] to the admitted event and still
                  wrote join=ok; the fixed analyzer must pair NOTHING
                  (counts differ), mark every row fifo_count_mismatch with
                  slot=null, keep T5 (seq-keyed) intact, and exit 1;
  case_abort      header-only failure dump (flag bit 4, n_writers 0) from the
                  quiesce-breach instrument path -> exit 1, no ops.

Each case independently reproduces a failure mode rather than mirroring the
implementation: the reject case's expected row fields are stated from the
event semantics (seq/slot/t5), not from analyzer output.
"""
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ANALYZER = ROOT / "scripts" / "analyze_tail_trace_v2.py"
HEADER = "<QIIIII"
WHDR = "<QQQQ"
EVENT = "<qQQIIIBxH"
MAGIC = 0x32435254544C4941


def ev(kind, t, seq=0, slot=0, gen=0, aux=0, cookie=0):
    return struct.pack(EVENT, t, gen, cookie, seq, slot, aux, kind, 0)


def write_dump(path, tag, flags, writers):
    out = struct.pack(HEADER, MAGIC, 2, 40, len(writers), flags, len(tag))
    out += tag.encode()
    out += struct.pack("<QQQQ", 4242, 0x1234, 0, 0)
    for tid, events in writers:
        out += struct.pack(WHDR, tid, len(events), 0, 0) + b"".join(events)
    Path(path).write_bytes(out)


def run_analyzer(dump, ops_out):
    r = subprocess.run([sys.executable, str(ANALYZER), "--ops-out", ops_out,
                        str(dump)], capture_output=True, text=True)
    rows = []
    if Path(ops_out).exists():
        import csv
        with open(ops_out) as f:
            rows = list(csv.DictReader(f))
    return r.returncode, rows


def main():
    failures = []
    with tempfile.TemporaryDirectory() as td:
        # case_good: seq 0,1 built (cw 8); admitted slot0/gen1, slot1/gen1;
        # cookie/cqe/published/stop keyed; taken by seq.
        good = [
            ev(1, 100, seq=0, aux=8), ev(1, 101, seq=1, aux=8),          # built
            ev(2, 200, slot=0, gen=1), ev(2, 201, slot=1, gen=1),        # admitted
            ev(4, 300, slot=0, gen=1, cookie=77),
            ev(4, 301, slot=1, gen=1, cookie=78),                        # cookie
            ev(5, 400, slot=0, gen=1), ev(5, 401, slot=1, gen=1),        # cqe
            ev(6, 500, slot=0, gen=1), ev(6, 501, slot=1, gen=1),        # published
            ev(8, 0, slot=0, gen=1, aux=1), ev(8, 0, slot=1, gen=1, aux=1),  # stop
            ev(7, 600, seq=0), ev(7, 601, seq=1),                        # taken
        ]
        write_dump(f"{td}/good.bin", "case-good", 0, [(100, good)])
        rc, rows = run_analyzer(f"{td}/good.bin", f"{td}/good.csv")
        if rc != 0:
            failures.append(f"good: expected exit 0, got {rc}")
        if len(rows) != 2 or any(r["join"] != "ok" for r in rows):
            failures.append(f"good: expected 2 rows join=ok, got {rows}")
        if rows and (rows[0]["slot"] != "0" or rows[1]["slot"] != "1"
                     or rows[1]["t5"] != "601"):
            failures.append(f"good: wrong keyed fields: {rows}")

        # case_reject: built seq 0 and 1; ONLY seq 1's op (slot 1) admitted.
        # FIFO would bind built(0) to admitted(slot=1): seq 0 % 16 != 1.
        # Counts differ (2 built vs 1 admitted) -> no pairing at all.
        reject = [
            ev(1, 100, seq=0, aux=0), ev(1, 101, seq=1, aux=0),
            ev(2, 201, slot=1, gen=1),
            ev(5, 401, slot=1, gen=1),
            ev(7, 601, seq=1),
        ]
        write_dump(f"{td}/reject.bin", "case-reject", 0, [(100, reject)])
        rc, rows = run_analyzer(f"{td}/reject.bin", f"{td}/reject.csv")
        if rc == 0:
            failures.append("reject: expected nonzero exit, got 0")
        if len(rows) != 2:
            failures.append(f"reject: expected 2 op rows, got {len(rows)}")
        else:
            for r in rows:
                if r["join"] != "fifo_count_mismatch":
                    failures.append(f"reject: row join={r['join']} != fifo_count_mismatch")
                if r["slot"] != "":
                    failures.append(f"reject: fabricated slot {r['slot']} on join-degraded row")
            by_seq = {r["seq"]: r for r in rows}
            if by_seq.get("1", {}).get("t5") != "601":
                failures.append("reject: seq-keyed T5 must survive degraded pairing")

        # case_equal_count_duplicate_key: counts match, but the second
        # admitted event repeats the first (slot,gen) -> admit_key_duplicate,
        # the duplicate is not paired, its op row stays built_never_admitted,
        # exit 1. (An equal-count CROSS-bind after rejection-with-rebuild is
        # a documented blind spot of the recorded fields — admitted events
        # carry the adapter's slot/gen, not the bench seq — and is therefore
        # deliberately NOT asserted here; see the analyzer docstring.)
        dupkey = [
            ev(1, 100, seq=0, aux=0), ev(1, 101, seq=1, aux=0),
            ev(2, 201, slot=1, gen=1), ev(2, 202, slot=1, gen=1),
            ev(5, 401, slot=1, gen=1),
        ]
        write_dump(f"{td}/dupkey.bin", "case-dupkey", 0, [(100, dupkey)])
        rc, rows = run_analyzer(f"{td}/dupkey.bin", f"{td}/dupkey.csv")
        if rc == 0:
            failures.append("dupkey: expected nonzero exit, got 0")
        joins = sorted(r["join"] for r in rows)
        if joins != ["built_never_admitted", "ok"]:
            failures.append(f"dupkey: expected one ok + one built_never_admitted, got {joins}")

        # case_abort: header-only failure dump written by the disabled
        # instrument (quiesce breach -> flag bit 4, n_writers 0).
        write_dump(f"{td}/abort.bin", "case-abort", 0b100, [])
        rc, rows = run_analyzer(f"{td}/abort.bin", f"{td}/abort.csv")
        if rc == 0:
            failures.append("abort: header-flag failure dump must exit nonzero")
        if rows:
            failures.append(f"abort: no op rows expected, got {len(rows)}")

    if failures:
        print("TEST FAILURES:")
        for f in failures:
            print(" -", f)
        return 1
    print("analyzer failure-handling tests: 4/4 PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
