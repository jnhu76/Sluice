#!/usr/bin/env python3
"""#595 R3-completion paper analysis: independent recomputation from raw records.

Reads ONLY raw layers (per-rep bench JSONL, bpftrace/perf/time outputs,
results.jsonl used solely for exit-code/validity cross-check), rebuilds every
paired statistic with the frozen prereg method (bootstrap 10,000, seed 5950,
identical resampling loop to harness/analyze_pairs.py), and emits:

  analysis/metrics.csv   absolute per-arm values (gate-grade + references)
  analysis/effects.csv   paired relative effects with CI / one-sided bounds
  analysis/gates.csv     gate adjudications incl. NOT_RUN registry
  analysis/recomputed.json  machine-readable source for figures
  analysis/NUMERIC_CROSSCHECK.json  agreement vs recorded summary.json

Conventions (user-facing):
  cpu / p50 / p95 / p99:  est = (cand - base) / base, NEGATIVE = improvement
  tput:                   est = (cand - base) / base, POSITIVE = improvement
Gates are evaluated internally in worse-positive orientation exactly as the
prereg defines them; the bound column names which side is used.
"""
import csv
import glob
import json
import math
import os
import random
import re
import sys

ROOT = "/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z"
RAW = os.path.join(ROOT, "raw")
OUT = os.path.join(ROOT, "analysis")

BOOT = 10000
SEED = 5950
AA_LIMITS = {"cpu": 0.05, "tput": 0.03, "p99": 0.03}
NC_LIMITS = {"cpu": 0.05, "p95": 0.05, "p99": 0.05, "tput": 0.03}

R3_BENCH_SHA = "0ebfb7f0c204579fe2cc9e8d7e88cee2de9112e25c0cfb11e8502e59fc4fe2d1"
R2_BENCH_SHA = "32beabcd891801eb65dab1d6e2df72aa0b89a98a2c16a114d398527cb821589a"

BLOCKS = [
    # phase_dir, block, case, arms(AB/AA), base member, cand member, tree, binsha, gate_family
    ("calibration/AA-W03-D16", "AA-W03-D16", "W03-uring-4k-d16", "AA", "m1", "m2",
     "R3 (0a22a791 + r3-completion.patch)", R3_BENCH_SHA, "AA"),
    ("calibration/AA-W02-D2", "AA-W02-D2", "W02-uring-4k-d2", "AA", "m1", "m2",
     "R2 (271b81bf + R2 patch 7be9d014)", R2_BENCH_SHA, "AA"),
    ("calibration/R2-D2", "LEGACY-W02-D2 (R2-D2)", "W02-uring-4k-d2", "AB", "A", "B",
     "R2 (271b81bf + R2 patch 7be9d014)", R2_BENCH_SHA, "NC-D2"),
    ("protection/PROT-W02-D3", "PROT-W02-D3 (R2)", "W02-uring-4k-d3", "AB", "A", "B",
     "R2 (271b81bf + R2 patch 7be9d014)", R2_BENCH_SHA, "PROT"),
    ("protection/PROT-W02-D16", "PROT-W02-D16 (R2)", "W02-uring-4k-d16", "AB", "A", "B",
     "R2 (271b81bf + R2 patch 7be9d014)", R2_BENCH_SHA, "PROT"),
]

FNAME_RE = re.compile(r"^p(\d+)-(\d+)-(m1|m2|A|B)-a(\d+)\.jsonl$")


def load_reps(path):
    recs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                recs.append(json.loads(line))
    return recs


def invocation_metrics(measured):
    ops = sum(r["ops"] for r in measured)
    cpu = sum(r["cpu_s"] for r in measured)
    wall = sum(r["wall_s"] for r in measured)
    n = len(measured)
    return {
        "cpu_ns_per_op": 1e9 * cpu / ops if ops else None,
        "ops_per_sec": ops / wall if wall else None,
        "p50_ns": sum(r["lat_p50_ns"] for r in measured) / n,
        "p95_ns": sum(r["lat_p95_ns"] for r in measured) / n,
        "p99_ns": sum(r["lat_p99_ns"] for r in measured) / n,
        "wall_s": wall,
        "cpu_s": cpu,
        "ops": ops,
    }


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
        "upper1": means[min(int(0.95 * BOOT), BOOT - 1)],
        "lower1": means[int(0.05 * BOOT)],
        "half_width": (means[min(int(0.975 * BOOT), BOOT - 1)] - means[int(0.025 * BOOT)]) / 2,
        "boot_means": means,
    }


def noninf_p(boot_means, threshold):
    ge = sum(1 for m in boot_means if m >= threshold)
    return (ge + 1) / (len(boot_means) + 1)


def holm(pvalues, alpha=0.05):
    m = len(pvalues)
    order = sorted(range(m), key=lambda i: pvalues[i])
    reject = [False] * m
    adj = [None] * m
    stop = False
    for rank, idx in enumerate(order):
        if stop:
            adj[idx] = None
            continue
        adj[idx] = alpha / (m - rank)
        if pvalues[idx] <= adj[idx]:
            reject[idx] = True
        else:
            stop = True
    return reject, adj


def analyze_block(phase_dir, case, arms_mode, base_m, cand_m):
    d = os.path.join(RAW, phase_dir)
    invocations = {}
    for jf in sorted(glob.glob(os.path.join(d, "jsonl", "*.jsonl"))):
        mo = FNAME_RE.match(os.path.basename(jf))
        if not mo:
            continue
        pair, pos, member, attempt = int(mo.group(1)), int(mo.group(2)), mo.group(3), int(mo.group(4))
        reps = [r for r in load_reps(jf) if r.get("case_id") == case and r.get("measured")]
        warm = [r for r in load_reps(jf) if r.get("case_id") == case and not r.get("measured")]
        reasons = []
        if len(reps) != 5:
            reasons.append(f"measured_records={len(reps)}")
        if len(warm) != 1:
            reasons.append(f"warmup_records={len(warm)}")
        for r in reps:
            if not r.get("ok"):
                reasons.append("bench_failure")
            if int(r.get("ops", -1)) != 20000:
                reasons.append(f"ops={r.get('ops')}")
        invocations[(pair, pos, member, attempt)] = {
            "metrics": invocation_metrics(reps) if reps else None,
            "invalid_reasons_self": reasons,
            "jsonl": jf,
        }
    # recorded results.jsonl: exit codes + validity flags (cross-check only)
    recorded = {}
    respath = os.path.join(d, "results.jsonl")
    for line in open(respath):
        r = json.loads(line)
        recorded[(r["pair"], r["pos"], r["member"], r.get("attempt", 1))] = r
    mismatches = []
    for key, inv in invocations.items():
        rec = recorded.get(key)
        if rec is None:
            mismatches.append(f"{key}: no results.jsonl record")
            inv["valid"] = False
            continue
        inv["valid"] = bool(rec.get("valid", True))
        if not inv["valid"]:
            continue
        if rec.get("exit") != 0:
            mismatches.append(f"{key}: exit={rec.get('exit')} but flagged valid")
            inv["valid"] = False
        if inv["invalid_reasons_self"]:
            mismatches.append(f"{key}: self-check invalid {inv['invalid_reasons_self']}")
            inv["valid"] = False
        # metric agreement with recorded aggregation
        if inv["metrics"] and rec.get("metrics"):
            for k in ("cpu_ns_per_op", "ops_per_sec", "p99_ns"):
                a, b = inv["metrics"][k], rec["metrics"].get(k)
                if a is not None and b is not None and abs(a - b) > 1e-6 * max(1.0, abs(a)):
                    mismatches.append(f"{key}: metric {k} self={a} recorded={b}")
    # build pairs
    by_pm = {}
    for (pair, pos, member, attempt), inv in invocations.items():
        if not inv["valid"]:
            continue
        by_pm.setdefault((pair, member), inv)
    pairs = []
    for pair in sorted({p for p, _ in by_pm}):
        a = by_pm.get((pair, base_m))
        b = by_pm.get((pair, cand_m))
        if a and b:
            pairs.append((pair, a["metrics"], b["metrics"]))
    return pairs, mismatches, d


def rel_worse(a, b, key):
    x, y = a[key], b[key]
    if not x:
        return None
    return (y - x) / x


def tput_loss(a, b):
    x, y = a["ops_per_sec"], b["ops_per_sec"]
    if not x:
        return None
    return (x - y) / x


def paired_stats(pairs):
    """All in worse-positive orientation (positive = candidate worse)."""
    fam = {}
    for metric, fn in (("cpu", lambda a, b: rel_worse(a, b, "cpu_ns_per_op")),
                       ("p50", lambda a, b: rel_worse(a, b, "p50_ns")),
                       ("p95", lambda a, b: rel_worse(a, b, "p95_ns")),
                       ("p99", lambda a, b: rel_worse(a, b, "p99_ns")),
                       ("tput", tput_loss)):
        vals = [fn(a, b) for _, a, b in pairs]
        vals = [v for v in vals if v is not None and math.isfinite(v)]
        if vals:
            fam[metric] = bootstrap(vals)
    return fam


# ---------------- mechanism parsers ----------------

def parse_enter_run(rund):
    events = []
    maps = {}
    for line in open(os.path.join(rund, "bpftrace.out")):
        line = line.strip()
        if line.startswith("E "):
            parts = line.split()
            pid, tid, fd, ts, mc, fl = (int(x) for x in parts[1:7])
            events.append((pid, tid, fd, ts, mc, fl))
        m = re.match(r"^@([\w_]+):\s+(\d+)$", line)
        if m:
            maps[m.group(1)] = int(m.group(2))
    pure = [e for e in events if e[3] > 0 and e[4] == 0 and e[5] == 0]
    hist = {}
    for e in pure:
        hist[e[3]] = hist.get(e[3], 0) + 1
    bucket = {}
    for e in events:
        k = f"{e[3]},{e[4]},{e[5]}"
        bucket[k] = bucket.get(k, 0) + 1
    submitter_tid_pure = {e[1] for e in pure}
    bench_timed = None
    bj = os.path.join(rund, "bench.jsonl")
    for line in open(bj):
        r = json.loads(line)
        if r.get("case_id", "").startswith("W03") and r.get("measured"):
            bench_timed = r
    return {
        "total_enter": len(events),
        "pure_submit": len(pure),
        "pure_submit_ops": sum(e[3] for e in pure),
        "to_submit_hist": dict(sorted(hist.items())),
        "bucket": dict(sorted(bucket.items())),
        "maps": maps,
        "pure_submit_tids": sorted(submitter_tid_pure),
        "bench_timed": {k: bench_timed.get(k) for k in
                        ("r3_entry_calls", "r3_group_calls", "r3_single_calls", "r3_accepted",
                         "r3_prefix_max", "r3_suffix_single_calls", "r3_flush_attempts",
                         "r3_untouched", "notifications", "stats_submitted_ops", "ops")}
            if bench_timed else None,
    }


def parse_perf_csv(path):
    out = {}
    for line in open(path):
        parts = [p for p in line.strip().split(",")]
        if len(parts) >= 3 and parts[2] and not parts[0].startswith("#"):
            try:
                out[parts[2]] = float(parts[0])
            except ValueError:
                pass
    return out


def parse_alloc(rund):
    maps = {}
    for line in open(os.path.join(rund, "bpftrace.out")):
        m = re.match(r"^@([\w_]+):\s+(\d+)$", line.strip())
        if m:
            maps[m.group(1)] = int(m.group(2))
    return maps


def parse_timev(path):
    out = {}
    for line in open(path):
        m = re.search(r"Maximum resident set size \(kbytes\):\s+(\d+)", line)
        if m:
            out["max_rss_kb"] = int(m.group(1))
        m = re.search(r"Voluntary context switches:\s+(\d+)", line)
        if m:
            out["voluntary_csw"] = int(m.group(1))
        m = re.search(r"Involuntary context switches:\s+(\d+)", line)
        if m:
            out["involuntary_csw"] = int(m.group(1))
    return out


def parse_bucket_results(path):
    arms = {}
    for line in open(path):
        r = json.loads(line)
        a = r["arm"]
        arms.setdefault(a, []).append({
            "main_ticks": r["buckets"]["main"]["ticks"],
            "driver_ticks": r["buckets"]["driver"]["ticks"],
            "io_wq_ticks": r["buckets"]["io_wq"]["ticks"],
            "ru_total": r["ru_total"],
            "bench_cpu_s": r["bench"]["cpu_s"],
            "max_user_threads": r["max_user_threads"],
            "unattributed_ticks_est": r["unattributed_ticks_est"],
        })
    summ = {}
    for a, rows in arms.items():
        def mean(k):
            return sum(x[k] for x in rows) / len(rows)
        summ[a] = {"runs": len(rows),
                   "main_ticks": mean("main_ticks"),
                   "driver_ticks": mean("driver_ticks"),
                   "io_wq_ticks": mean("io_wq_ticks"),
                   "ru_total_s": mean("ru_total"),
                   "bench_cpu_s": mean("bench_cpu_s"),
                   "unattributed_ticks": mean("unattributed_ticks_est"),
                   "max_user_threads": max(x["max_user_threads"] for x in rows)}
    return summ


def parse_single_arm_results(path):
    """SAFE-A0 / DIAG job-rep records."""
    rows = []
    for line in open(path):
        r = json.loads(line)
        b = r.get("bench", {})
        rows.append({
            "job": r.get("job"), "case": r.get("case"), "exit": r.get("exit"),
            "binary_sha256": r.get("binary_sha256"),
            "env": r.get("env_overrides", {}),
            "ru_total_s": r.get("ru_total"),
            "wall_s": r.get("wall_s"),
            "bench": {k: b.get(k) for k in ("cpu_ns_per_op", "ops_per_sec", "p50_ns",
                                            "p95_ns", "p99_ns", "ops")} if b else None,
        })
    return rows


def main():
    os.makedirs(OUT, exist_ok=True)
    results = {"paired_blocks": {}, "crosscheck": {}, "mechanism": {}, "single_arm": {}}
    crosscheck = {"metric_agreement": [], "notes": []}

    metrics_rows = []
    effects_rows = []
    gates_rows = []

    for phase_dir, block, case, mode, base_m, cand_m, tree, binsha, gate_family in BLOCKS:
        pairs, mismatches, d = analyze_block(phase_dir, case, mode, base_m, cand_m)
        fam = paired_stats(pairs)
        results["paired_blocks"][block] = {
            "n_pairs": len(pairs), "dir": d, "tree": tree, "binary_sha256": binsha,
            "case": case, "metrics": {k: {kk: vv for kk, vv in v.items() if kk != "boot_means"}
                                      for k, v in fam.items()},
            "mismatches": mismatches,
            "pairs": [{"pair": p,
                       "base": {k: a[k] for k in ("cpu_ns_per_op", "ops_per_sec", "p50_ns", "p95_ns", "p99_ns")},
                       "cand": {k: b[k] for k in ("cpu_ns_per_op", "ops_per_sec", "p50_ns", "p95_ns", "p99_ns")}}
                      for p, a, b in pairs],
        }
        for m in mismatches:
            crosscheck["metric_agreement"].append(m)
        # absolute metrics rows
        for label, side in ((base_m, "base"), (cand_m, "cand")):
            invs = [c for _, b, c in pairs] if side == "cand" else [a for _, a, _ in pairs]
            if not invs:
                continue
            metrics_rows.append({
                "block": block, "member": label, "role": side, "case": case, "tree": tree,
                "binary_sha256": binsha, "n_invocations": len(invs),
                "cpu_ns_per_op": sum(x["cpu_ns_per_op"] for x in invs) / len(invs),
                "ops_per_sec": sum(x["ops_per_sec"] for x in invs) / len(invs),
                "p50_ns": sum(x["p50_ns"] for x in invs) / len(invs),
                "p95_ns": sum(x["p95_ns"] for x in invs) / len(invs),
                "p99_ns": sum(x["p99_ns"] for x in invs) / len(invs),
                "source": os.path.join(d, "jsonl"),
            })
        # gate evaluation
        gate = {"block": block, "family": gate_family, "n_pairs": len(pairs)}
        if gate_family == "AA":
            for metric, th in AA_LIMITS.items():
                st = fam.get(metric)
                if st:
                    gate[metric] = {"half_width_pct": st["half_width"] * 100,
                                    "threshold_pct": th * 100,
                                    "pass": st["half_width"] <= th}
            gate["verdict"] = "PASS" if all(g["pass"] for g in
                                            [gate[m] for m in AA_LIMITS if m in gate]) else "FAIL"
        else:
            limits = NC_LIMITS
            plist = []
            entries = {}
            for metric, th in limits.items():
                st = fam.get(metric)
                if st:
                    p = noninf_p(st["boot_means"], th)
                    plist.append(p)
                    entries[metric] = {"mean_pct": st["mean"] * 100,
                                       "upper1_pct": st["upper1"] * 100,
                                       "threshold_pct": th * 100,
                                       "p_noninf": p,
                                       "point_ok": st["mean"] <= th,
                                       "bound_ok": st["upper1"] <= th}
            reject, adj = holm(plist)
            for i, metric in enumerate([m for m in limits if m in entries]):
                entries[metric]["holm_reject"] = reject[i]
                entries[metric]["holm_alpha"] = adj[i]
                entries[metric]["pass"] = bool(entries[metric]["point_ok"] and
                                               entries[metric]["bound_ok"] and reject[i])
            gate["metrics"] = entries
            gate["verdict"] = "PASS" if all(e["pass"] for e in entries.values()) else "FAIL"
        results["paired_blocks"][block]["gate"] = gate
        # crosscheck vs recorded summary.json
        sumpath = os.path.join(d, "summary.json")
        if os.path.exists(sumpath):
            rec = json.load(open(sumpath))
            diffs = []
            for metric, st in fam.items():
                rm = (rec.get("metrics") or {}).get(metric)
                if rm:
                    for key in ("mean", "ci_lo", "ci_hi", "ci_upper_one_sided_95"):
                        if rm.get(key) is not None and st.get(key) is not None:
                            dv = abs(rm[key] - st[key])
                            if dv > 1e-9:
                                diffs.append(f"{block}.{metric}.{key}: self={st[key]:.9f} rec={rm[key]:.9f}")
            crosscheck["notes"].append({"block": block, "recorded_verdict": rec.get("verdict"),
                                        "self_verdict": gate["verdict"],
                                        "max_metric_diff": max([0.0] + [float(x.split("=")[1].split(" ")[0]) if "=" in x else 0.0 for x in diffs]) if diffs else 0.0,
                                        "diffs": diffs})
        gates_rows.append(gate)
        # effects rows (user orientation)
        pair_src = os.path.join(d, "jsonl")
        for metric, st in fam.items():
            if metric == "tput":
                est = -st["mean"]; lo = -st["ci_hi"]; hi = -st["ci_lo"]
                bound, bound_val, thr = "lower1_pct", -st["lower1"], -0.03
                bound_ok = st["lower1"] >= -0.03
                unit = "% (positive = improvement)"
            else:
                est = st["mean"]; lo = st["ci_lo"]; hi = st["ci_hi"]
                bound, bound_val, thr = "upper1_pct", st["upper1"], NC_LIMITS.get(metric, 0.05)
                bound_ok = st["upper1"] <= thr
                unit = "% (negative = improvement)"
            base_vals = [a for _, a, _ in pairs]
            cand_vals = [b for _, _, b in pairs]
            def mkey(v):
                return "cpu_ns_per_op" if metric == "cpu" else f"{metric}_ns" if metric != "tput" else "ops_per_sec"
            effects_rows.append({
                "block": block, "case": case, "comparison": f"{cand_m}-{base_m}", "metric": metric,
                "unit": unit, "n_pairs": len(pairs), "tree": tree,
                "baseline_abs": sum(x[mkey(metric)] for x in base_vals) / len(base_vals),
                "candidate_abs": sum(x[mkey(metric)] for x in cand_vals) / len(cand_vals),
                "est_pct": est * 100, "ci95_lo_pct": lo * 100, "ci95_hi_pct": hi * 100,
                "bound_used": bound, "bound_pct": bound_val * 100, "threshold_pct": thr * 100,
                "halfwidth_pct": st["half_width"] * 100,
                "gate_family": gate_family, "coverage": "GATE_GRADE_24PAIRS",
                "source": pair_src,
            })

    # ---------------- mechanism ----------------
    mech = {"enter": {}, "pmu": {}, "alloc": {}, "timev": {}, "bucket": {}}
    for rund in sorted(glob.glob(os.path.join(RAW, "mechanism/enter/*"))):
        name = os.path.basename(rund)
        mech["enter"][name] = parse_enter_run(rund)
    for arm in ("A0", "A1", "B"):
        mech["pmu"][f"d16-{arm}"] = parse_perf_csv(os.path.join(RAW, f"mechanism/pmu/d16-{arm}/perf.csv"))
        mech["alloc"][f"d16-{arm}"] = parse_alloc(os.path.join(RAW, f"mechanism/alloc/d16-{arm}"))
        mech["timev"][f"d16-{arm}"] = parse_timev(os.path.join(RAW, f"mechanism/resource/time-v/{arm}.txt"))
    mech["bucket"] = parse_bucket_results(os.path.join(RAW, "mechanism/bucket/results.jsonl"))
    results["mechanism"] = mech

    # ---------------- single-arm reference runs ----------------
    for pat in ("SAFE-A0-*", "DIAG-*"):
        for d in sorted(glob.glob(os.path.join(RAW, "protection", pat))):
            rp = os.path.join(d, "results.jsonl")
            if os.path.exists(rp):
                rows = parse_single_arm_results(rp)
                if rows:
                    results["single_arm"][os.path.basename(d)] = rows

    # ---------------- NOT_RUN registry (prereg §14) ----------------
    not_run = [
        ("MAIN-W03-D16", "B-A0", "W03-uring-4k-d16", "CPU point<=-10% & CI95 upper<=-5%; tput drop<=3%; p95/p99<=5%", "BLOCKED_SEMANTICS_NOT_READY"),
        ("MAIN-W03-D16", "B-A1", "W03-uring-4k-d16", "mechanism effect (B-A1)", "BLOCKED_SEMANTICS_NOT_READY"),
        ("MAIN-W03-D16", "A1-A0", "W03-uring-4k-d16", "adapter cost (A1-A0)", "BLOCKED_SEMANTICS_NOT_READY"),
        ("CROSS-W03-D4", "B-A0", "W03-uring-4k-d4", "protection bounds 5/5/5/3", "BLOCKED_SEMANTICS_NOT_READY"),
        ("CROSS-W03-D64", "B-A0", "W03-uring-4k-d64", "protection bounds 5/5/5/3 (CPU improvement not required)", "BLOCKED_SEMANTICS_NOT_READY"),
        ("PROT-W03-D16-B (R3)", "B-A0", "W03-uring-4k-d16", "protection bounds 5/5/5/3", "BLOCKED_SEMANTICS_NOT_READY"),
        ("PROT-64K-D16-B", "B-A0", "W03-uring-64k-d16", "protection bounds 5/5/5/3", "BLOCKED_SEMANTICS_NOT_READY"),
        ("PROT-THREADPOOL-B", "B-A0", "W03-uring-4k-d16-threadpool", "protection bounds 5/5/5/3", "BLOCKED_SEMANTICS_NOT_READY"),
        ("PROT-SMT-2-12-B", "B-A0", "W03-uring-4k-d16 @cpu2,12", "protection bounds 5/5/5/3", "BLOCKED_SEMANTICS_NOT_READY"),
        ("PROT-XPHYS-2-8-B", "B-A0", "W03-uring-4k-d16 @cpu2,8", "protection bounds 5/5/5/3", "BLOCKED_SEMANTICS_NOT_READY"),
        ("NEG-W02-D2-NEW-B", "B-A0", "W02-uring-4k-d2 (R3 tree has no W-02 arm)", "non-inferiority 5/5/5/3 + enter=20000", "NOT_RUN_NO_W02_R3_ARM"),
        ("MECH-POS-D2POS", "B vs A0/A1", "W03-uring-4k-d2pos", "3 independent invocations required (1 done)", "PARTIAL_DIAGNOSTIC_ONLY"),
        ("RESOURCE-B", "B vs A0", "d16", "peak RSS<=+2%; zero new queue/thread/timer/per-request alloc; valgrind slope", "PARTIAL_DIAGNOSTIC_ONLY"),
        ("PRODUCT-604", "real app", "#604 admitted application", "product gate", "HOLD_NO_REAL_APPLICATION"),
    ]
    for block, comp, case, rule, status in not_run:
        gates_rows.append({"block": block, "family": "PREREG_BLOCKED", "n_pairs": 0,
                           "verdict": status, "rule": rule,
                           "case": case, "comparison": comp})
        for metric in ("cpu", "tput", "p95", "p99"):
            effects_rows.append({
                "block": block, "case": case, "comparison": comp, "metric": metric,
                "unit": "% (see figure note)", "n_pairs": 0, "tree": "R3",
                "baseline_abs": "", "candidate_abs": "",
                "est_pct": "", "ci95_lo_pct": "", "ci95_hi_pct": "",
                "bound_used": "", "bound_pct": "", "threshold_pct": "",
                "halfwidth_pct": "", "gate_family": "PREREG_BLOCKED",
                "coverage": status, "source": "prereg §14 / MECHANISM_SUMMARY §7",
            })

    # ---------------- write outputs ----------------
    def fmt(x):
        if x == "" or x is None:
            return ""
        if isinstance(x, float):
            return f"{x:.4f}"
        return str(x)

    with open(os.path.join(OUT, "metrics.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["block", "member", "role", "case", "tree", "binary_sha256",
                    "n_invocations", "cpu_ns_per_op", "ops_per_sec", "p50_ns", "p95_ns",
                    "p99_ns", "source"])
        for r in metrics_rows:
            w.writerow([r["block"], r["member"], r["role"], r["case"], r["tree"],
                        r["binary_sha256"][:12], r["n_invocations"],
                        fmt(r["cpu_ns_per_op"]), fmt(r["ops_per_sec"]), fmt(r["p50_ns"]),
                        fmt(r["p95_ns"]), fmt(r["p99_ns"]), r["source"]])
        # diagnostic mechanism rows (absolute, instrumented)
        w.writerow(["# diagnostic (instrumented, single-window; NOT gate-grade)"])
        for name, e in mech["enter"].items():
            w.writerow([f"mech-enter/{name}", "run", "diag", "W03", "R3", R3_BENCH_SHA[:12], 1,
                        "", "", "", "", "",
                        f"pure_submit_enters={e['pure_submit']} pure_ops={e['pure_submit_ops']} hist={e['to_submit_hist']}"])
        for arm in ("A0", "A1", "B"):
            p = mech["pmu"][f"d16-{arm}"]
            w.writerow([f"mech-pmu/d16-{arm}", "run", "diag", "W03", "R3", R3_BENCH_SHA[:12], 1,
                        "", "", "", "", "",
                        f"cycles={p.get('cycles', 0):.0f} instructions={p.get('instructions', 0):.0f} csw={p.get('context-switches', 0):.0f}"])
        for arm, s in mech["bucket"].items():
            w.writerow([f"mech-bucket/d16-{arm}", "runs", "diag", "W03", "R3", R3_BENCH_SHA[:12], s["runs"],
                        fmt(s["bench_cpu_s"] * 1e9 / 20000), "", "", "", "",
                        f"main_ticks={s['main_ticks']:.1f} driver={s['driver_ticks']:.1f} io_wq={s['io_wq_ticks']:.1f} ru_total_s={s['ru_total_s']:.3f} unattr={s['unattributed_ticks']:.1f}"])

    with open(os.path.join(OUT, "effects.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["block", "case", "comparison", "metric", "unit", "n_pairs", "tree",
                    "baseline_abs", "candidate_abs", "est_pct", "ci95_lo_pct", "ci95_hi_pct",
                    "bound_used", "bound_pct", "threshold_pct", "halfwidth_pct",
                    "gate_family", "coverage", "source"])
        for r in effects_rows:
            w.writerow([r["block"], r["case"], r["comparison"], r["metric"], r["unit"],
                        r["n_pairs"], r["tree"], fmt(r["baseline_abs"]), fmt(r["candidate_abs"]),
                        fmt(r["est_pct"]), fmt(r["ci95_lo_pct"]), fmt(r["ci95_hi_pct"]),
                        r["bound_used"], fmt(r["bound_pct"]), fmt(r["threshold_pct"]),
                        fmt(r["halfwidth_pct"]), r["gate_family"], r["coverage"], r["source"]])

    with open(os.path.join(OUT, "gates.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["block", "family", "n_pairs", "verdict", "detail"])
        for g in gates_rows:
            if g["family"] == "AA":
                det = "; ".join(f"{m}: hw={g[m]['half_width_pct']:.3f}pp<= {g[m]['threshold_pct']:.0f}pp ({'PASS' if g[m]['pass'] else 'FAIL'})"
                                for m in AA_LIMITS if m in g)
            elif g["family"] in ("NC-D2", "PROT"):
                det = "; ".join(f"{m}: est={e['mean_pct']:+.3f}% ub1={e['upper1_pct']:+.3f}% thr={e['threshold_pct']:.0f}% p={e['p_noninf']:.4f} holm={e['holm_reject']} ({'PASS' if e['pass'] else 'FAIL'})"
                                for m, e in g["metrics"].items())
            else:
                det = g.get("rule", "") + " | " + g.get("comparison", "")
            w.writerow([g["block"], g["family"], g.get("n_pairs", ""), g["verdict"], det])

    with open(os.path.join(OUT, "recomputed.json"), "w") as f:
        json.dump(results, f, indent=1, sort_keys=True)
    with open(os.path.join(OUT, "NUMERIC_CROSSCHECK.json"), "w") as f:
        json.dump(crosscheck, f, indent=1, sort_keys=True)

    # console digest
    print("== paired blocks ==")
    for _, block, *_ in BLOCKS:
        g = results["paired_blocks"][block]
        print(f"{block}: n={g['n_pairs']} verdict={g['gate']['verdict']}")
        for m, st in g["metrics"].items():
            print(f"   {m:5s} est={st['mean']*100:+8.4f}%  CI[{st['ci_lo']*100:+8.4f},{st['ci_hi']*100:+8.4f}] ub1={st['upper1']*100:+8.4f} hw={st['half_width']*100:.4f}pp")
    print("== mismatches ==", len(crosscheck["metric_agreement"]))
    for m in crosscheck["metric_agreement"][:10]:
        print("  ", m)
    print("== crosscheck vs summary.json ==")
    for n in crosscheck["notes"]:
        print(f"  {n['block']}: recorded={n['recorded_verdict']} self={n['self_verdict']} diffs={len(n['diffs'])}")
    print("== mechanism enter ==")
    for name, e in mech["enter"].items():
        print(f"  {name}: total={e['total_enter']} pure={e['pure_submit']} ops={e['pure_submit_ops']} hist={e['to_submit_hist']}")
    print("== pmu ==")
    for k, p in mech["pmu"].items():
        print(f"  {k}: cycles={p.get('cycles'):.0f} instr={p.get('instructions'):.0f} csw={p.get('context-switches'):.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
