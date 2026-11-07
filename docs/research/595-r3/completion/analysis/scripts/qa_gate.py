#!/usr/bin/env python3
"""Repeatable numeric QA gate: figure-data CSVs == recomputed.json values,
accounting identities, NOT_RUN rows carry no numbers, tables non-empty.
Writes analysis/NUMERIC_QA.md and exits non-zero on any failure."""
import csv
import json
import os
import sys

ROOT = "/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z"
ANA = os.path.join(ROOT, "analysis")
FD = os.path.join(ROOT, "figures", "figure-data")

R = json.load(open(os.path.join(ANA, "recomputed.json")))


def close(a, b, tol=5e-4):
    return abs(float(a) - float(b)) <= tol


checks = []


def ck(tag, name, cond):
    checks.append((tag, name, bool(cond)))


def main():
    # 1. tables non-empty
    for t in ("metrics.csv", "effects.csv", "gates.csv"):
        n = sum(1 for _ in open(os.path.join(ANA, t))) - 1
        ck("tables", f"{t} rows={n}", n > 0)

    # 2. NOT_RUN rows carry no numbers (no zeros masquerading as data)
    bad_zero = 0
    for r in csv.DictReader(open(os.path.join(ANA, "effects.csv"))):
        if r["coverage"] in ("NOT_RUN_NO_W02_R3_ARM", "BLOCKED_SEMANTICS_NOT_READY"):
            if r["est_pct"] != "" or r["baseline_abs"] != "":
                bad_zero += 1
    ck("tables", "NOT_RUN rows numeric-free", bad_zero == 0)

    # 3. gate-grade effects count
    n_gate = sum(1 for r in csv.DictReader(open(os.path.join(ANA, "effects.csv")))
                 if r["coverage"] == "GATE_GRADE_24PAIRS")
    ck("tables", f"gate-grade effect rows={n_gate} (5 blocks x 5 metrics)", n_gate == 25)

    # 4. F2a values
    mm = {"CPU / op": "cpu", "throughput": "tput", "p50": "p50", "p95": "p95", "p99": "p99"}
    m = R["paired_blocks"]["LEGACY-W02-D2 (R2-D2)"]["metrics"]
    for r in csv.DictReader(open(os.path.join(FD, "F2a_d2_single_refill_effects.csv"))):
        k = mm[r["metric"]]
        st = m[k]
        est = -st["mean"] * 100 if k == "tput" else st["mean"] * 100
        lo = -st["ci_hi"] * 100 if k == "tput" else st["ci_lo"] * 100
        hi = -st["ci_lo"] * 100 if k == "tput" else st["ci_hi"] * 100
        ck("F2a", r["metric"], close(r["est_pct"], est) and close(r["ci95_lo_pct"], lo)
           and close(r["ci95_hi_pct"], hi))

    # 5. F2b d2pos
    ent = R["mechanism"]["enter"]
    ck("F2b", "B 3 enters x2", ent["d2pos-B"]["pure_submit"] == 3
       and ent["d2pos-B"]["pure_submit_ops"] == 6)
    ck("F2b", "A0/A1 6 enters x1", all(ent[f"d2pos-{a}"]["pure_submit"] == 6 for a in ("A0", "A1")))

    # 6. F3a/F3b/F3c/F3d
    e = ent["d16-B"]
    for r in csv.DictReader(open(os.path.join(FD, "F3a_pure_submit_enters_d16.csv"))):
        ck("F3a", r["arm"], int(r["pure_submit_enters"]) == ent[f"d16-{r['arm']}"]["pure_submit"])
    h = {int(k): v for k, v in e["to_submit_hist"].items()}
    ck("F3b", "sum(counts)==enters", sum(h.values()) == e["pure_submit"])
    ck("F3b", "sum(k*counts)==ops", sum(k * v for k, v in h.items()) == e["pure_submit_ops"])
    bt = e["bench_timed"]
    ck("F3c", "accepted==20000", bt["r3_accepted"] == 20000)
    ck("F3c", "1355*8+single==20000", 1355 * 8 + bt["r3_single_calls"] == 20000)
    ck("F3c", "untouched==suffix==9160",
       bt["r3_untouched"] == bt["r3_suffix_single_calls"] == 9160)
    ck("F3c", "group+single==entry==flush",
       bt["r3_group_calls"] + bt["r3_single_calls"] == bt["r3_entry_calls"] == bt["r3_flush_attempts"])
    for r in csv.DictReader(open(os.path.join(FD, "F3d_pmu_diagnostic.csv"))):
        ck("F3d", r["arm"] + "/" + r["event"],
           float(r["count"]) == R["mechanism"]["pmu"][f"d16-{r['arm']}"][r["event"]])

    # 7. F4
    nm = {"W-02 D2 $B_{R2}-A_{R2}$": "LEGACY-W02-D2 (R2-D2)", "W-02 D3 $B_{R2}-A_{R2}$": "PROT-W02-D3 (R2)",
          "W-02 D16 $B_{R2}-A_{R2}$": "PROT-W02-D16 (R2)", "A/A W-03 d16 (A0 vs A0)": "AA-W03-D16",
          "A/A W-02 D2 ($A_{R2}$ vs $A_{R2}$)": "AA-W02-D2"}
    seen = set()
    for r in csv.DictReader(open(os.path.join(FD, "F4_tail_effects.csv"))):
        key = nm[r["block"]]
        if (key, r["metric"]) in seen:
            continue
        seen.add((key, r["metric"]))
        st = R["paired_blocks"][key]["metrics"][r["metric"].lower()]
        ck("F4", f"{key} {r['metric']}", close(r["est_pct"], st["mean"] * 100)
           and close(r["ci95_lo_pct"], st["ci_lo"] * 100)
           and close(r["ci95_hi_pct"], st["ci_hi"] * 100))

    # 8. figures exist
    for f in ("F2_d2_regression", "F3_batch_mechanism", "F4_tail_guardrails", "F5_observed_regimes"):
        for ext in (".svg", ".png"):
            ck("files", f + ext, os.path.exists(os.path.join(ROOT, "figures", f + ext)))

    failed = [c for c in checks if not c[2]]
    with open(os.path.join(ANA, "NUMERIC_QA.md"), "w") as f:
        f.write("# NUMERIC_QA — 图/CSV/正文数值一致性（qa_gate.py 自动生成）\n\n")
        f.write(f"检查总数：{len(checks)}；通过：{len(checks) - len(failed)}；失败：{len(failed)}。\n\n")
        f.write("| 组 | 检查 | 结果 |\n|---|---|---|\n")
        for tag, name, okk in checks:
            f.write(f"| {tag} | {name} | {'OK' if okk else 'MISMATCH'} |\n")
    for tag, name, okk in checks:
        print(f"  {tag:7s} {name:40s} {'OK' if okk else 'MISMATCH'}")
    print(f"TOTAL {len(checks)} FAILED {len(failed)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
