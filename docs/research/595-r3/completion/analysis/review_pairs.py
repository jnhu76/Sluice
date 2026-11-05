#!/usr/bin/env python3
"""Independent statistical review of the #595 R3 continuation run (two-arm blocks).

Reviewer: independent statistics reviewer; did not participate in measurement.
Recomputes, from raw results.jsonl + bench jsonl + logs only:
  - per-block validity (exit, arm readback, binary SHA, 20k ops/invocation,
    byte-oracle gate, AB/BA order balance, 24 pairs, attempts ledger)
  - pair-relative diffs (B-A)/A, tput (A-B)/A, p50/p95/p99
  - bootstrap 10,000 / seed 5950 / percentile 95% CI (own implementation)
  - A/A half-width gates (5/3/3 pp), negative-control one-sided gates (5/5/5/3)
    with Holm-Bonferroni
  - cross-check against the executor's summary.json
No benchmark is run here.
"""
import json
import math
import os
import random
import sys

ROOT = "/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z"
BOOT = 10000
SEED = 5950
AA_LIMITS = {"cpu": 0.05, "tput": 0.03, "p99": 0.03}
NC_LIMITS = {"cpu": 0.05, "p95": 0.05, "p99": 0.05, "tput": 0.03}

R3_SHA = "0ebfb7f0c204579fe2cc9e8d7e88cee2de9112e25c0cfb11e8502e59fc4fe2d1"
R2_SHA = "32beabcd891801eb65dab1d6e2df72aa0b89a98a2c16a114d398527cb821589a"

BLOCKS = [
    # label, dir, arms, expected case, expected sha, expected ops/rep
    ("AA-W03-D16", "raw/calibration/AA-W03-D16", "AA", "W03-uring-4k-d16", R3_SHA, 20000),
    ("AA-W02-D2", "raw/calibration/AA-W02-D2", "AA", "W02-uring-4k-d2", R2_SHA, 20000),
    ("R2-D2", "raw/calibration/R2-D2", "AB", "W02-uring-4k-d2", R2_SHA, 20000),
    ("PROT-W02-D3", "raw/protection/PROT-W02-D3", "AB", "W02-uring-4k-d3", R2_SHA, 20000),
    ("PROT-W02-D16", "raw/protection/PROT-W02-D16", "AB", "W02-uring-4k-d16", R2_SHA, 20000),
]


def load(path):
    recs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                recs.append(json.loads(line))
    return recs


def check_validity(label, d, arms, case, sha, ops_exp):
    problems = []
    notes = {"n_records": 0, "n_valid": 0, "attempts": 0, "order_violations": 0,
             "pairs": 0, "ops_bad": 0, "oracle_bad": 0, "exit_bad": 0,
             "sha_mismatch": 0, "readback_bad": 0, "rep_aggregation_bad": 0,
             "jsonl_mismatch": 0, "measured_reps_bad": 0, "sha_seen": set(),
             "arm_envs": set(), "request_counts": set()}
    p = os.path.join(ROOT, d, "results.jsonl")
    recs = load(p)
    notes["n_records"] = len(recs)
    by_pair = {}
    for r in recs:
        notes["n_valid"] += 1 if r.get("valid") else 0
        if r.get("exit") != 0:
            notes["exit_bad"] += 1
            problems.append(f"{label}: pair {r.get('pair')} pos {r.get('pos')} exit={r.get('exit')}")
        if not r.get("valid"):
            problems.append(f"{label}: pair {r.get('pair')} pos {r.get('pos')} invalid: {r.get('invalid_reasons')}")
        s = r.get("binary_sha256")
        notes["sha_seen"].add(s)
        if s != sha:
            notes["sha_mismatch"] += 1
        if r.get("before", {}).get("binary_sha256") != sha or r.get("after", {}).get("binary_sha256") != sha:
            notes["sha_mismatch"] += 1
        env_arm = r.get("env", {}).get("SLUICE_595_R3_ARM") or r.get("env", {}).get("SLUICE_595_ARM")
        rb = r.get("readback") or {}
        notes["arm_envs"].add(f"{r.get('member')}:{env_arm}:{rb.get('arm')}")
        if rb:
            if env_arm != rb.get("arm"):
                notes["readback_bad"] += 1
        else:
            # R2 bench prints no arm readback on stdout; arm identity rests on
            # the recorded env + member naming (recorded as limitation).
            notes.setdefault("readback_absent", 0)
            notes["readback_absent"] = notes.get("readback_absent", 0) + 1
        if r.get("case") != case:
            problems.append(f"{label}: case {r.get('case')} != {case}")
        m = r.get("measured_records")
        reps = [x for x in r.get("reps", []) if x.get("measured")]
        if m != 5 or len(reps) != 5:
            notes["measured_reps_bad"] += 1
        for x in reps:
            if x.get("ops") != ops_exp or not x.get("ok"):
                notes["ops_bad"] += 1
                notes["oracle_bad"] += 0 if x.get("ok") else 1
            notes["request_counts"].add(x.get("request_count"))
        # aggregation formula checks (prereg 6.4)
        mm = r.get("metrics", {})
        cpu = sum(x["cpu_s"] for x in reps); ops = sum(x["ops"] for x in reps)
        wall = sum(x["wall_s"] for x in reps)
        def close(a, b, tol=2e-6):
            return abs(a - b) <= tol * max(1.0, abs(a), abs(b))
        if not close(mm.get("cpu_ns_per_op", 0), cpu / ops * 1e9, 1e-4) or \
           not close(mm.get("ops", 0), ops) or \
           not close(mm.get("ops_per_sec", 0), ops / wall, 1e-4):
            notes["rep_aggregation_bad"] += 1
            problems.append(f"{label}: pair {r.get('pair')} pos {r.get('pos')} aggregation mismatch")
        for q in ("p50_ns", "p95_ns", "p99_ns"):
            want = sum(x["lat_" + q] for x in reps) / len(reps)
            if not close(mm.get(q, 0), want, 1e-4):
                notes["rep_aggregation_bad"] += 1
                problems.append(f"{label}: pair {r.get('pair')} {q} mismatch")
        # raw bench jsonl correspondence
        jp = r.get("jsonl")
        if jp and os.path.exists(jp):
            jreps = [json.loads(l) for l in open(jp) if l.strip()]
            jmeas = [x for x in jreps if x.get("measured") and x.get("case_id") == case]
            if len(jmeas) != 5:
                notes["jsonl_mismatch"] += 1
                problems.append(f"{label}: {os.path.basename(jp)} measured count {len(jmeas)}")
            else:
                for a, b in zip(jmeas, reps):
                    for k in ("ops", "cpu_s", "wall_s", "lat_p50_ns", "lat_p95_ns", "lat_p99_ns", "ok"):
                        if a.get(k) != b.get(k):
                            notes["jsonl_mismatch"] += 1
                            problems.append(f"{label}: {os.path.basename(jp)} field {k} differs from results record")
                            break
        else:
            notes["jsonl_mismatch"] += 1
            problems.append(f"{label}: missing bench jsonl {jp}")
        by_pair.setdefault(r["pair"], []).append(r)
    # order balance
    for pair, rs in sorted(by_pair.items()):
        rs.sort(key=lambda x: x["pos"])
        if len(rs) != 2:
            problems.append(f"{label}: pair {pair} has {len(rs)} records")
            continue
        first, second = rs[0]["member"], rs[1]["member"]
        if arms == "AA":
            want = ("m1", "m2") if pair % 2 == 0 else ("m2", "m1")
        else:
            want = ("A", "B") if pair % 2 == 0 else ("B", "A")
        if (first, second) != want:
            notes["order_violations"] += 1
            problems.append(f"{label}: pair {pair} order {first},{second} != {want}")
        if rs[0].get("order") != f"{want[0]}{want[1]}":
            notes["order_violations"] += 1
    notes["pairs"] = len(by_pair)
    att = os.path.join(ROOT, d, "attempts.jsonl")
    if os.path.exists(att):
        notes["attempts"] = sum(1 for l in open(att) if l.strip())
    else:
        notes["attempts"] = None
        problems.append(f"{label}: no attempts.jsonl")
    notes["sha_seen"] = sorted(notes["sha_seen"])
    notes["arm_envs"] = sorted(notes["arm_envs"])
    notes["request_counts"] = sorted(notes["request_counts"])
    # arm-env sanity: for AB, members must be A and B; both with matching readback
    return notes, problems


def pair_diffs(recs, arms):
    by_pair = {}
    for r in recs:
        by_pair.setdefault(r["pair"], {})[r["pos"]] = r
    out = []
    for pair in sorted(by_pair):
        dd = by_pair[pair]
        if 0 not in dd or 1 not in dd:
            continue
        a = dd[0] if dd[0]["member"] in ("m1", "A") else dd[1]
        b = dd[1] if dd[1]["member"] in ("m2", "B") else dd[0]
        if arms == "AA" and (a["member"], b["member"]) != ("m1", "m2"):
            continue
        if arms == "AB" and (a["member"], b["member"]) != ("A", "B"):
            continue
        ma, mb = a["metrics"], b["metrics"]
        row = {
            "cpu": (mb["cpu_ns_per_op"] - ma["cpu_ns_per_op"]) / ma["cpu_ns_per_op"],
            "p50": (mb["p50_ns"] - ma["p50_ns"]) / ma["p50_ns"],
            "p95": (mb["p95_ns"] - ma["p95_ns"]) / ma["p95_ns"],
            "p99": (mb["p99_ns"] - ma["p99_ns"]) / ma["p99_ns"],
            "tput": (ma["ops_per_sec"] - mb["ops_per_sec"]) / ma["ops_per_sec"],
            "ru": (b["ru_total"] - a["ru_total"]) / a["ru_total"],
            "rss": (b["ru_maxrss_kb"] - a["ru_maxrss_kb"]) / a["ru_maxrss_kb"],
        }
        out.append(row)
    return out


def bootstrap(values):
    n = len(values)
    rng = random.Random(SEED)
    means = []
    for _ in range(BOOT):
        s = 0.0
        for _ in range(n):
            s += values[rng.randrange(n)]
        means.append(s / n)
    means.sort()
    return {
        "n": n,
        "mean": sum(values) / n,
        "ci_lo": means[int(0.025 * BOOT)],
        "ci_hi": means[min(int(0.975 * BOOT), BOOT - 1)],
        "ci_upper_one_sided_95": means[min(int(0.95 * BOOT), BOOT - 1)],
        "half_width": (means[min(int(0.975 * BOOT), BOOT - 1)] - means[int(0.025 * BOOT)]) / 2,
        "boot_means": means,
    }


def holm(entries, alpha=0.05):
    m = len(entries)
    order = sorted(range(m), key=lambda i: entries[i]["p"])
    stop = False
    for rank, idx in enumerate(order):
        if stop:
            entries[idx]["holm_alpha"] = None
            entries[idx]["holm_reject"] = False
            continue
        adj = alpha / (m - rank)
        entries[idx]["holm_alpha"] = adj
        if entries[idx]["p"] <= adj:
            entries[idx]["holm_reject"] = True
        else:
            entries[idx]["holm_reject"] = False
            stop = True
    return entries


def one_sided_p(stat, threshold):
    ge = sum(1 for m in stat["boot_means"] if m >= threshold)
    return (ge + 1) / (len(stat["boot_means"]) + 1)


def main():
    report = {"blocks": {}, "gates": {}, "problems": []}
    for label, d, arms, case, sha, ops_exp in BLOCKS:
        notes, problems = check_validity(label, d, arms, case, sha, ops_exp)
        report["problems"].extend(problems)
        recs = [r for r in load(os.path.join(ROOT, d, "results.jsonl")) if r.get("valid", True)]
        diffs = pair_diffs(recs, arms)
        stats = {}
        for k in ("cpu", "p50", "p95", "p99", "tput", "ru", "rss"):
            vals = [x[k] for x in diffs if math.isfinite(x[k])]
            if vals:
                stats[k] = bootstrap(vals)
        gates = None
        if arms == "AA":
            gates = {}
            ok = True
            for k, th in AA_LIMITS.items():
                m = stats[k]
                gates[k] = {"half_width_pp": round(m["half_width"] * 100, 4),
                            "threshold_pp": th * 100,
                            "pass": m["half_width"] <= th}
                ok = ok and gates[k]["pass"]
            gates["verdict"] = "PASS" if ok else "FAIL"
        else:
            entries = []
            for k, th in NC_LIMITS.items():
                m = stats[k]
                entries.append({"metric": k, "threshold": th,
                                "mean": m["mean"], "ci_lo": m["ci_lo"], "ci_hi": m["ci_hi"],
                                "ci_upper_one_sided_95": m["ci_upper_one_sided_95"],
                                "p": one_sided_p(m, th)})
            holm(entries)
            ok = True
            for e in entries:
                e["point_in_bound"] = e["mean"] <= e["threshold"]
                e["upper_in_bound"] = e["ci_upper_one_sided_95"] <= e["threshold"]
                e["pass"] = bool(e["point_in_bound"] and e["upper_in_bound"] and e["holm_reject"])
                ok = ok and e["pass"]
            gates = {"entries": entries, "verdict": "PASS" if ok else "FAIL"}
        # cross-check vs executor summary
        xcheck = None
        sp = os.path.join(ROOT, d, "summary.json")
        if os.path.exists(sp):
            ex = json.load(open(sp))
            xcheck = {"executor_verdict": ex.get("verdict"), "executor_n_pairs": ex.get("n_pairs")}
            diffs_vs_exec = []
            if arms == "AA":
                for k in AA_LIMITS:
                    hw = ex["aa_gate"][k]["half_width_pp"]
                    mine = stats[k]["half_width"] * 100
                    if abs(hw - mine) > 1e-6:
                        diffs_vs_exec.append(f"{k}: exec hw {hw} vs mine {mine}")
            else:
                exm = {e["metric"]: e for e in ex["negative_control"]}
                for e in gates["entries"]:
                    x = exm[e["metric"]]
                    for f in ("mean", "ci_lo", "ci_hi", "ci_upper_one_sided_95"):
                        if abs(x[f] - e[f]) > 1e-9:
                            diffs_vs_exec.append(f"{e['metric']}.{f}: exec {x[f]} vs mine {e[f]}")
            xcheck["numeric_diffs"] = diffs_vs_exec
        pub = {k: {kk: vv for kk, vv in v.items() if kk != "boot_means"} for k, v in stats.items()}
        report["blocks"][label] = {
            "dir": d, "arms": arms, "case": case, "expected_sha": sha,
            "validity": notes, "n_pairs_used": len(diffs), "stats": pub,
            "gate": gates, "executor_crosscheck": xcheck,
        }
    print(json.dumps(report, indent=1, sort_keys=True))
    with open(os.path.join(ROOT, "analysis", "review_pairs_out.json"), "w") as f:
        json.dump(report, f, indent=1, sort_keys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
