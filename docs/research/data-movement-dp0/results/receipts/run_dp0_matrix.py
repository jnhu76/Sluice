#!/usr/bin/env python3
"""DP-0 R4 interleaved A/B matrix runner (research-only; /tmp tool).

Executes the frozen protocol in protocol_frozen.md: per (case, candidate-arm)
config, 15 interleaved AB/BA pairs, each run in its own process wrapped by
perf stat. Writes one JSONL record per run immediately (crash-safe), then a
per-config summary JSONL.
"""
import json
import os
import random
import statistics
import subprocess
import sys
import time

DP0 = "/tmp/dp0"
BIN = f"{DP0}/bin"
RESULTS = f"{DP0}/results/raw"
PERF_EVENTS = ["instructions:u", "cycles:u", "task-clock",
               "context-switches", "cpu-migrations"]

CASE_ITERS = {
    "zop": 200000,
    "cons4k": 20000,
    "thr64": 10000,
    "write4k": 20000,
    "finfo": 20000,
    "constp": 10000,
    "w01ctl": 200000,
}
CANDIDATE_ARMS = ["dp1", "dp2", "dp2b", "dp3a"]
PAIRS = 15

CONFIGS = [(c, a) for c in CASE_ITERS for a in CANDIDATE_ARMS]
CONFIGS.append(("cons4k", "base"))  # self-pair noise floor
rng = random.Random(0xD0D0)
rng.shuffle(CONFIGS)


def run_one(arm, case, iters):
    perf_out = f"{DP0}/tmp/perf_stat_last.txt"
    env = dict(os.environ)
    env["SLUICE_BENCH_TMP_DIR"] = f"{DP0}/tmp"
    env["SLUICE_DP0_REUSE_FIXTURE"] = "1"
    cmd = ["perf", "stat", "-x,", "-o", perf_out]
    for ev in PERF_EVENTS:
        cmd += ["-e", ev]
    cmd += ["--", f"{BIN}/dp0b_{arm}", case, str(iters)]
    t0 = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    wall_overhead = time.time() - t0
    rec = {"ts": time.time(), "arm": arm, "case": case, "iters": iters,
           "rc": proc.returncode}
    if proc.returncode == 0:
        try:
            body = json.loads(proc.stdout.strip().splitlines()[-1])
            rec["bench"] = body
        except (ValueError, IndexError):
            rec["rc"] = "bench_json_unparsable"
    if proc.returncode == 0 and "bench" in rec:
        counts = {}
        with open(perf_out) as f:
            for line in f:
                parts = line.rstrip("\n").split(",")
                if len(parts) >= 3 and parts[0]:
                    try:
                        counts[parts[2]] = float(parts[0])
                    except ValueError:
                        pass
        rec["perf"] = counts
        rec["perf_missing"] = [e for e in PERF_EVENTS if e.rstrip(":u") not in counts
                               and e not in counts]
    else:
        rec["stderr_tail"] = proc.stderr[-300:]
    rec["wall_overhead_s"] = round(wall_overhead, 4)
    return rec


def main():
    os.makedirs(RESULTS, exist_ok=True)
    stamp = time.strftime("%Y%m%dT%H%M%S")
    raw_path = f"{RESULTS}/dp0_matrix_{stamp}.jsonl"
    summary_path = f"{RESULTS}/dp0_matrix_summary_{stamp}.jsonl"
    n_excluded = 0
    with open(raw_path, "w") as rawf, open(summary_path, "w") as sumf:
        for cfg_i, (case, arm) in enumerate(CONFIGS):
            iters = CASE_ITERS[case]
            diffs = {"instructions": [], "cycles": [], "wall": []}
            pair_records = []
            for pair in range(PAIRS):
                order = ["base", arm] if pair % 2 == 0 else [arm, "base"]
                runs = []
                for pos, a in enumerate(order):
                    rec = run_one(a, case, iters)
                    rec.update({"pair": pair, "position": pos,
                                "config_index": cfg_i})
                    rawf.write(json.dumps(rec) + "\n")
                    rawf.flush()
                    runs.append(rec)
                ok = all(("perf" in r and r["rc"] == 0 and
                          r["perf"].get("instructions:u", 0) > 0) for r in runs)
                if ok:
                    ipc = runs[0]["perf"]["cycles:u"] / runs[0]["perf"]["instructions:u"]
                    if not (0 < ipc < 6):
                        ok = False
                if not ok:
                    n_excluded += 1
                    continue
                b, a = (runs[0], runs[1]) if order[0] == "base" else (runs[1], runs[0])
                diffs["instructions"].append((b["perf"]["instructions:u"] - a["perf"]["instructions:u"]) / iters)
                diffs["cycles"].append((b["perf"]["cycles:u"] - a["perf"]["cycles:u"]) / iters)
                diffs["wall"].append((a["bench"]["wall_s"] - b["bench"]["wall_s"]) / iters)
                pair_records.append({
                    "pair": pair, "order": order,
                    "base_instr": runs[0]["perf"]["instructions:u"] if order[0] == "base"
                    else runs[1]["perf"]["instructions:u"],
                    "arm_instr": runs[1]["perf"]["instructions:u"] if order[0] == "base"
                    else runs[0]["perf"]["instructions:u"],
                })
            s = {"case": case, "arm": arm, "iters": iters,
                 "pairs_ok": len(diffs["instructions"]), "pairs_excluded": PAIRS - len(diffs["instructions"])}
            for k, v in diffs.items():
                if v:
                    s[k] = {"median_delta_per_iter": statistics.median(v),
                            "positive_pairs": sum(1 for x in v if x > 0),
                            "negative_pairs": sum(1 for x in v if x < 0)}
                    if k == "instructions":
                        base_med = statistics.median([r["base_instr"] for r in pair_records]) / iters
                        s["median_rel_reduction_pct"] = (s[k]["median_delta_per_iter"] / base_med) * 100.0 if base_med else None
            sumf.write(json.dumps(s) + "\n")
            sumf.flush()
            print(f"[{cfg_i+1}/{len(CONFIGS)}] {case} x {arm}: "
                  f"{s.get('median_rel_reduction_pct', float('nan')):.3f}% instr, "
                  f"pairs {s['pairs_ok']}/{PAIRS}", flush=True)
    print(f"DONE raw={raw_path} summary={summary_path} excluded_runs={n_excluded}")


if __name__ == "__main__":
    sys.exit(main())
