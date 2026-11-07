#!/usr/bin/env python3
"""Empty/missing-table check across evidence/measurement-* + mirror consistency.

Required tables per the ask: measurement-main / measurement-negative /
measurement-legacy / measurement-mechanism (and measurement-product HOLD).
A table that exists but has no data rows -> empty. A prereg block directory
that does not exist at all -> missing (reported separately from empty).
"""
import filecmp
import json
import os

ROOT = "/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z"
E = os.path.join(ROOT, "evidence")


def jsonl_rows(p):
    if not os.path.exists(p):
        return None
    return sum(1 for l in open(p) if l.strip())


def main():
    out = {"trees": {}, "mirrors": {}, "missing_blocks": [], "completion_gate_json": None}
    trees = {
        "measurement-main": ["aa/aa-w03-d16/summary.json", "aa/aa-w03-d16/results.jsonl"],
        "measurement-legacy": ["aa-w02-d2/summary.json", "w02-d2/summary.json",
                               "diag-r2/summary.json", "bucket-probe/RECEIPT.md"],
        "measurement-negative": ["protection/SUMMARY.json", "protection/RECEIPT.md",
                                 "protection/PROT-W02-D3/summary.json",
                                 "protection/PROT-W02-D16/summary.json"],
        "measurement-mechanism": ["MECHANISM_SUMMARY.md"],
        "measurement-product": ["HOLD.md"],
    }
    empty = []
    for t, files in trees.items():
        info = {}
        for f in files:
            p = os.path.join(E, t, f)
            if not os.path.exists(p):
                info[f] = "MISSING"
                empty.append(f"{t}/{f} missing")
            elif f.endswith(".jsonl"):
                n = jsonl_rows(p)
                info[f] = f"{n} rows"
                if n == 0:
                    empty.append(f"{t}/{f} empty")
            else:
                sz = os.path.getsize(p)
                info[f] = f"{sz} bytes"
                if sz == 0:
                    empty.append(f"{t}/{f} empty")
        out["trees"][t] = info

    # main 3-arm (A0/A1/B) triplet performance tables: required for the
    # B-A0 main gate; prereg 14.1 freezes them as blocked
    for blk in ("w03-d16-a1", "w03-d16-b", "w03-a0-baseline", "w03-64k-a0",
                "w03-threadpool-a0", "placement-a0"):
        p = os.path.join(E, "measurement-main", blk)
        if not os.path.exists(p):
            out["missing_blocks"].append(f"measurement-main/{blk}")
    p = os.path.join(ROOT, "analysis", "COMPLETION_GATE.json")
    out["completion_gate_json"] = "present" if os.path.exists(p) else "MISSING"
    for extra in ("measurement-mechanism/d2pos", "measurement-mechanism/d2serial",
                  "measurement-mechanism/enter-cqe",
                  "measurement-negative/protection/w03-d4",
                  "measurement-negative/protection/w03-d16",
                  "measurement-negative/protection/w03-d64",
                  "measurement-negative/w02-d16-a0", "measurement-negative/w02-sat-a0",
                  "measurement-legacy/w02-d3", "measurement-legacy/w02-d16"):
        q = os.path.join(E, extra)
        if not os.path.exists(q):
            out["missing_blocks"].append(extra)

    # mirror consistency raw <-> evidence
    pairs = [
        ("raw/calibration/AA-W03-D16/summary.json", "evidence/measurement-main/aa/aa-w03-d16/summary.json"),
        ("raw/calibration/AA-W03-D16/results.jsonl", "evidence/measurement-main/aa/aa-w03-d16/results.jsonl"),
        ("raw/calibration/AA-W02-D2/summary.json", "evidence/measurement-legacy/aa-w02-d2/summary.json"),
        ("raw/calibration/R2-D2/summary.json", "evidence/measurement-legacy/w02-d2/summary.json"),
        ("raw/protection/PROT-W02-D3/summary.json", "evidence/measurement-negative/protection/PROT-W02-D3/summary.json"),
        ("raw/protection/PROT-W02-D16/summary.json", "evidence/measurement-negative/protection/PROT-W02-D16/summary.json"),
    ]
    for a, b in pairs:
        aa, bb = os.path.join(ROOT, a), os.path.join(ROOT, b)
        if os.path.exists(aa) and os.path.exists(bb):
            out["mirrors"][f"{a} <-> {b}"] = "identical" if filecmp.cmp(aa, bb, shallow=False) else "DIFFER"
        else:
            out["mirrors"][f"{a} <-> {b}"] = "one side missing"

    out["empty_required_tables"] = empty
    print(json.dumps(out, indent=1, sort_keys=True))
    with open(os.path.join(ROOT, "analysis", "review_tables_out.json"), "w") as f:
        json.dump(out, f, indent=1, sort_keys=True)
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())
