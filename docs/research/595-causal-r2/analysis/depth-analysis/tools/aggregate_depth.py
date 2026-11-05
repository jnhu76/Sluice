#!/usr/bin/env python3
"""Cross-depth ledger aggregation for #595 causal-r2 depth-boundary analysis.

Inputs: run_blocks.py result files (one JSON record per invocation) for
D1x, D2, D3, D16 counter-ON full blocks, all built from the same binary
sha256 (verified by caller). Each invocation: warmup=1, measured=5, 20k op/rep.

Per-invocation counters = sum of SLUICE595_COUNTERS lines with tag=measured;
ops = sum of measured rep ops. Pair-level statistic = (B-A)/A with bootstrap
over pairs (seed 5950, 10000 resamples), same convention as analyze_cost.py.
"""

import json
import random
import statistics
import sys

BLOCKS = [
    ("D1", "/home/jnhu/sluice-595-e5/causal-r2/evidence/counters/raw/cnt-on-full-d1x/results.jsonl"),
    ("D2", "/home/jnhu/sluice-595-e5/causal-r2/evidence/counters/raw/cnt-on-full-d2/results.jsonl"),
    ("D3", "/home/jnhu/sluice-595-e5/causal-r2/evidence/depth-analysis/raw/cnt-d3-ab/results.jsonl"),
    ("D16", "/home/jnhu/sluice-595-e5/causal-r2/evidence/depth-analysis/raw/cnt-d16-ab/results.jsonl"),
]
COUNTERS = ["dispatch_passes", "poll_passes", "empty_polls", "sqe_preparations",
            "sqes_deferred", "flushes", "submit_attempts", "actual_enters",
            "wait_attempts", "cqe_reap_attempts", "cqes_reaped",
            "progress_handoffs", "wakeups", "wakeup_writes", "wakeup_coalesced"]


def boot_ci(values, seed=5950, boot=10000):
    n = len(values)
    rng = random.Random(seed)
    means = [sum(values[rng.randrange(n)] for _ in range(n)) / n for _ in range(boot)]
    means.sort()
    return statistics.mean(values), means[int(0.025 * boot)], means[int(0.975 * boot)]


def load_block(path):
    invs = []
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            cs = [c for c in r["counter_lines"] if c.get("tag") == "measured"]
            if not cs:
                print("missing counters", path, r["arm"], r["pair"], file=sys.stderr)
                continue
            ops = sum(rep["ops"] for rep in r["records"] if rep.get("measured"))
            cpu_s = sum(rep["cpu_s"] for rep in r["records"] if rep.get("measured"))
            lat = {k: [rep[k] for rep in r["records"] if rep.get("measured")]
                   for k in ("lat_p50_ns", "lat_p95_ns", "lat_p99_ns")}
            c = {k: sum(x.get(k, 0) for x in cs) for k in COUNTERS}
            invs.append({
                "pair": r["pair"], "pos": r["pos"], "arm": r["arm"], "rc": r["rc"],
                "ops": ops, "cpu_ns_per_op": cpu_s * 1e9 / ops if ops else None,
                "ops_per_sec": sum(rep["ops"] for rep in r["records"] if rep.get("measured"))
                / sum(rep["wall_s"] for rep in r["records"] if rep.get("measured")),
                "p50": statistics.mean(lat["lat_p50_ns"]),
                "p95": statistics.mean(lat["lat_p95_ns"]),
                "p99": statistics.mean(lat["lat_p99_ns"]),
                "poll_calls": sum(rep["stats_poll_calls"] for rep in r["records"] if rep.get("measured")),
                "wait_calls": sum(rep["stats_wait_calls"] for rep in r["records"] if rep.get("measured")),
                "max_outstanding": max(rep["stats_max_outstanding"] for rep in r["records"] if rep.get("measured")),
                "counters": c,
            })
    return invs


def per_op(inv, key):
    return inv["counters"][key] / inv["ops"]


def main():
    report = {}
    for depth, path in BLOCKS:
        invs = load_block(path)
        by = {"A": {}, "B": {}}
        for inv in invs:
            by[inv["arm"]][inv["pair"]] = inv
        pairs = sorted(set(by["A"]) & set(by["B"]))
        arms = {}
        for arm in ("A", "B"):
            arms[arm] = {}
            for key in COUNTERS:
                arms[arm][key] = statistics.mean(per_op(by[arm][p], key) for p in pairs)
            arms[arm]["cpu_ns_per_op"] = statistics.mean(by[arm][p]["cpu_ns_per_op"] for p in pairs)
            arms[arm]["ops_per_sec"] = statistics.mean(by[arm][p]["ops_per_sec"] for p in pairs)
            for k in ("p50", "p95", "p99"):
                arms[arm][k] = statistics.mean(by[arm][p][k] for p in pairs)
            arms[arm]["poll_per_op"] = statistics.mean(by[arm][p]["poll_calls"] / by[arm][p]["ops"] for p in pairs)
            arms[arm]["wait_per_op"] = statistics.mean(by[arm][p]["wait_calls"] / by[arm][p]["ops"] for p in pairs)
            arms[arm]["max_outstanding"] = statistics.mean(by[arm][p]["max_outstanding"] for p in pairs)
        deltas = {}
        for key in list(COUNTERS) + ["cpu_ns_per_op", "ops_per_sec", "p50", "p95", "p99",
                                     "poll_per_op", "wait_per_op"]:
            if key in COUNTERS:
                a = [per_op(by["A"][p], key) for p in pairs]
                b = [per_op(by["B"][p], key) for p in pairs]
            elif key in ("poll_per_op", "wait_per_op"):
                src = "poll_calls" if key == "poll_per_op" else "wait_calls"
                a = [by["A"][p][src] / by["A"][p]["ops"] for p in pairs]
                b = [by["B"][p][src] / by["B"][p]["ops"] for p in pairs]
            else:
                a = [by["A"][p][key] for p in pairs]
                b = [by["B"][p][key] for p in pairs]
            rel = [(bb - aa) / aa if aa else 0.0 for aa, bb in zip(a, b)]
            mean, lo, hi = boot_ci(rel)
            deltas[key] = {"mean_pct": 100 * mean, "ci_lo": 100 * lo, "ci_hi": 100 * hi,
                           "A": statistics.mean(a), "B": statistics.mean(b), "n_pairs": len(pairs)}
        report[depth] = {"arms": arms, "deltas": deltas,
                         "sqe_per_enter": {
                             "A": arms["A"]["sqe_preparations"] / arms["A"]["actual_enters"],
                             "B": arms["B"]["sqe_preparations"] / arms["B"]["actual_enters"]},
                         "flushes_per_enter": {
                             "A": arms["A"]["flushes"] / arms["A"]["actual_enters"],
                             "B": arms["B"]["flushes"] / arms["B"]["actual_enters"]},
                         "deferred_share": {
                             "A": arms["A"]["sqes_deferred"] / arms["A"]["sqe_preparations"],
                             "B": arms["B"]["sqes_deferred"] / arms["B"]["sqe_preparations"]},
                         "write_share_of_wakeups": {
                             "A": (arms["A"]["wakeup_writes"] / arms["A"]["wakeups"]) if arms["A"]["wakeups"] else 0,
                             "B": (arms["B"]["wakeup_writes"] / arms["B"]["wakeups"]) if arms["B"]["wakeups"] else 0},
                         "n_pairs": len(pairs)}
        print("== depth", depth, "pairs", len(pairs))
        for key in ("sqes_deferred", "flushes", "actual_enters", "submit_attempts",
                    "poll_passes", "wait_attempts", "empty_polls", "wakeups",
                    "wakeup_writes", "wakeup_coalesced", "cpu_ns_per_op",
                    "poll_per_op", "wait_per_op"):
            d = deltas[key]
            print(f"  {key:18s} A={d['A']:10.4f} B={d['B']:10.4f} d={d['mean_pct']:+7.2f}% [{d['ci_lo']:+.2f},{d['ci_hi']:+.2f}]")
        print(f"  SQE/enter A={report[depth]['sqe_per_enter']['A']:.3f} B={report[depth]['sqe_per_enter']['B']:.3f}"
              f"  deferred share A={report[depth]['deferred_share']['A']:.3f} B={report[depth]['deferred_share']['B']:.3f}"
              f"  write/wakeup A={report[depth]['write_share_of_wakeups']['A']:.3f} B={report[depth]['write_share_of_wakeups']['B']:.3f}")
    out = "/home/jnhu/sluice-595-e5/causal-r2/evidence/depth-analysis/depth_ledger_summary.json"
    with open(out, "w") as f:
        json.dump(report, f, indent=1)
    print("wrote", out)


if __name__ == "__main__":
    main()
