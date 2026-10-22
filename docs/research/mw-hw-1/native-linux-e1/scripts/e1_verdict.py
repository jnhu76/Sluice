#!/usr/bin/env python3
"""E1 native-Linux formal reducer (research-only, fail-closed).

Rebuilds every preregistered (rep, cfg, arm, phase) cell from the campaign
JSONL, validates workload and compile identity, rejects missing rows,
duplicates, unknown configurations, non-numeric measurements and failing
oracles, applies the preregistered thresholds, and emits verdict.json.

Preregistered thresholds (frozen before the formal campaign):
  headline gain      : cfg 4096-1024-1 and 4096-1024-8 must show median
                       paired cpu_ns_per_op ratio <= 0.95 with >= 12/15
                       pairs strictly below 1.0
  non-regression     : every other non-P>0 cfg must show median ratio <= 1.03
  depth audit        : every timed row must report max_outstanding == depth
  correctness        : every timed and oracle row must be ok with zero
                       completion errors / invalid-state rejections and
                       ops == requests
  W-03 (if supplied) : >= 15 valid interleaved pairs per cfg, median ratio
                       <= 1.03, loop_kind must be "epoll+eventfd", and every
                       verify row must carry no_lost_wake=true
  P>0   (if supplied): P>0 events must actually occur (p_events > 0), the
                       pending-control invariant must hold on every row, and
                       the paired median ratio must be <= 1.05
Exit code 0 iff identity, correctness and W-02 gates all pass; the overall
research verdict (NATIVE_GO / CONDITIONAL / BLOCKED / ...) is carried in
verdict.json and accounts for W-03 and P>0 evidence presence.
"""

import argparse
import collections
import json
import math
import os
import statistics
import sys

HEADLINE = [("4096-1024-1", 1), ("4096-1024-8", 8)]
HEADLINE_GAIN_MAX = 0.95
HEADLINE_PAIRS_BELOW = 12
NONREG_MAX = 1.03
P5_REGRESSION_MAX = 1.05
W03_REGRESSION_MAX = 1.03
MIN_PAIRS = 15

EXPECTED_CFGS = {
    "4096-16-1", "4096-64-1", "4096-256-1", "4096-1024-1",
    "4096-16-8", "4096-1024-8", "4096-64-64", "4096-1024-64",
    "65536-1024-1", "65536-1024-8", "1048576-64-8",
}
REQUESTS_BY_BLOCK = {"4096": 20000, "65536": 2000, "1048576": 500}


def fail(issues, msg):
    issues.append(msg)


def load_rows(path, issues, label):
    if not os.path.exists(path):
        fail(issues, f"{label}: file missing: {path}")
        return []
    rows = []
    with open(path) as fh:
        for n, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as e:
                fail(issues, f"{label}: line {n} is not JSON: {e}")
    return rows


def check_identity(results_dir, issues):
    prov = {}
    prov_path = os.path.join(results_dir, "provenance.txt")
    if os.path.exists(prov_path):
        for line in open(prov_path):
            if "=" in line:
                k, v = line.rstrip("\n").split("=", 1)
                prov[k] = v
    else:
        fail(issues, "identity: provenance.txt missing")
    ident_path = os.path.join(results_dir, "arm-identity.txt")
    if not os.path.exists(ident_path):
        fail(issues, "identity: arm-identity.txt missing")
        return prov
    ident = open(ident_path).read()
    for key in ("base_sha", "e1_sha", "runner_sha256", "harness_source_sha256"):
        if key not in prov:
            fail(issues, f"identity: provenance lacks {key}")
    if prov.get("base_sha") == prov.get("e1_sha"):
        fail(issues, "identity: both arms claim the same SHA")
    status_path = os.path.join(results_dir, "campaign-status.txt")
    if os.path.exists(status_path):
        status = open(status_path).read().strip()
        if status != "status=COMPLETE":
            fail(issues, f"identity: campaign status is {status!r}")
    else:
        fail(issues, "identity: campaign-status.txt missing")
    if "lib_async_sha256=" not in ident or "binary_sha256=" not in ident:
        fail(issues, "identity: arm-identity.txt lacks library/binary hashes")
    return prov


def rebuild_w02(results_dir, issues):
    rows = load_rows(os.path.join(results_dir, "ab.jsonl"), issues, "w02")
    if not rows:
        fail(issues, "w02: no rows")
        return None
    cells = collections.defaultdict(list)
    seen = collections.Counter()
    for row in rows:
        try:
            key = (row["rep"], row["cfg"], row["arm"], row["phase"])
        except KeyError as e:
            fail(issues, f"w02: row lacks key field {e}")
            continue
        seen[key] += 1
        if seen[key] > 1:
            fail(issues, f"w02: duplicate cell {key}")
            continue
        block = row["cfg"].split("-")[0]
        depth = int(row["cfg"].split("-")[2])
        requests = REQUESTS_BY_BLOCK.get(block)
        if row["cfg"] not in EXPECTED_CFGS:
            fail(issues, f"w02: unknown config {row['cfg']}")
            continue
        if requests is None or row["ops"] != requests:
            fail(issues, f"w02: ops {row['ops']} != requests {requests} in {key}")
        if row["phase"] in ("timed", "oracle"):
            if not row.get("ok"):
                fail(issues, f"w02: {key} not ok: {row.get('failure')}")
            if row.get("completion_errors", 1) != 0:
                fail(issues, f"w02: {key} has completion errors")
            if row.get("invalid_state", 1) != 0:
                fail(issues, f"w02: {key} has invalid-state rejections")
            if row.get("completed_ops", -1) != requests:
                fail(issues, f"w02: {key} completed_ops != requests")
        if not isinstance(row.get("cpu_ns_per_op"), (int, float)) or \
                not math.isfinite(row["cpu_ns_per_op"]) or row["cpu_ns_per_op"] <= 0:
            fail(issues, f"w02: {key} has an illegal cpu_ns_per_op {row.get('cpu_ns_per_op')}")
            continue
        if not isinstance(row.get("wall_s"), (int, float)) or \
                not math.isfinite(row["wall_s"]) or row["wall_s"] <= 0:
            fail(issues, f"w02: {key} has an illegal wall_s")
            continue
        if row["phase"] == "timed" and row["arm"] in ("base", "e1"):
            if row.get("max_outstanding") != depth:
                fail(issues,
                     f"w02: {key} achieved in-flight {row.get('max_outstanding')} != depth {depth}")
        cells[key].append(row)

    reps = sorted({k[0] for k in cells})
    per_cfg = {}
    for cfg in sorted(EXPECTED_CFGS):
        pairs = []
        for rep in reps:
            b = cells.get((rep, cfg, "base", "timed"))
            e = cells.get((rep, cfg, "e1", "timed"))
            if not b or not e:
                fail(issues, f"w02: missing timed pair rep={rep} cfg={cfg}")
                continue
            pairs.append(e[0]["cpu_ns_per_op"] / b[0]["cpu_ns_per_op"])
        if pairs:
            per_cfg[cfg] = {
                "valid_pairs": len(pairs),
                "median_ratio": statistics.median(pairs),
                "pairs_below_1": sum(1 for r in pairs if r < 1.0),
                "ratios": [round(r, 6) for r in pairs],
            }
    return per_cfg


def gate_w02(per_cfg, issues):
    if per_cfg is None:
        return "NOT_RUN"
    w02_pass = True
    for cfg, depth in HEADLINE:
        s = per_cfg.get(cfg)
        if not s or s["valid_pairs"] < MIN_PAIRS:
            fail(issues, f"w02: headline cfg {cfg} has {s and s['valid_pairs']} valid pairs < {MIN_PAIRS}")
            w02_pass = False
            continue
        if s["median_ratio"] > HEADLINE_GAIN_MAX or s["pairs_below_1"] < HEADLINE_PAIRS_BELOW:
            fail(issues,
                 f"w02: headline cfg {cfg} median {s['median_ratio']:.4f} "
                 f"pairs_below {s['pairs_below_1']} misses the preregistered gain gate")
            w02_pass = False
    for cfg, s in per_cfg.items():
        if any(cfg == h for h, _ in HEADLINE):
            continue
        if s["valid_pairs"] < MIN_PAIRS:
            fail(issues, f"w02: cfg {cfg} has {s['valid_pairs']} valid pairs < {MIN_PAIRS}")
            w02_pass = False
            continue
        if s["median_ratio"] > NONREG_MAX:
            fail(issues, f"w02: cfg {cfg} median ratio {s['median_ratio']:.4f} > {NONREG_MAX}")
            w02_pass = False
    return "PASS" if w02_pass else "FAIL"


def read_w03(path, issues):
    if path is None:
        return {"status": "NOT_RUN", "reason": "no W-03 campaign was supplied"}
    w03_issues = []
    rows = load_rows(path, w03_issues, "w03")
    if not rows:
        return {"status": "FAIL", "reason": "W-03 file had no rows", "issues": w03_issues}
    loops = {r.get("loop_kind") for r in rows}
    if loops != {"epoll+eventfd"}:
        w03_issues.append(f"w03: loop_kind values {sorted(loops)} are not a real kernel event loop")
        return {"status": "FAIL", "reason": f"loop_kind {sorted(loops)} is not epoll+eventfd",
                "issues": w03_issues}
    verify = [r for r in rows if r.get("phase") == "verify"]
    for r in verify:
        if not r.get("ok"):
            w03_issues.append(f"w03: verify row rep={r.get('rep')} not ok: {r.get('failure')}")
        if not r.get("no_lost_wake"):
            w03_issues.append(f"w03: verify row rep={r.get('rep')} reports a lost wake")
    per_cfg = collections.defaultdict(lambda: {"perf": collections.defaultdict(list)})
    for r in rows:
        if r.get("phase") == "timed":
            per_cfg[r["cfg"]]["perf"][r["arm"]].append(r["cpu_ns_per_op"])
    summary = {"status": "PENDING", "per_cfg": {}, "verify_rows": len(verify),
               "issues": w03_issues}
    w03_pass = len(verify) >= 3
    for cfg, data in sorted(per_cfg.items()):
        b = data["perf"].get("base", [])
        e = data["perf"].get("e1", [])
        pairs = min(len(b), len(e))
        ratios = [x / y for x, y in zip(sorted(e), sorted(b))][:pairs] if pairs else []
        med = statistics.median(ratios) if ratios else None
        summary["per_cfg"][cfg] = {
            "base_runs": len(b), "e1_runs": len(e), "median_ratio": med,
        }
        if pairs < MIN_PAIRS:
            w03_issues.append(f"w03: cfg {cfg} has {pairs} pairs < {MIN_PAIRS}")
            w03_pass = False
        elif med is not None and med > W03_REGRESSION_MAX:
            w03_issues.append(f"w03: cfg {cfg} median ratio {med:.4f} > {W03_REGRESSION_MAX}")
            w03_pass = False
    summary["status"] = "PASS" if w03_pass and not w03_issues else "FAIL"
    summary["issues"] = w03_issues
    issues.extend(w03_issues)
    return summary


def read_p5(path, issues):
    if path is None:
        return {"status": "NOT_MEASURED", "reason": "no P>0 pressure campaign was supplied"}
    p5_issues = []
    rows = load_rows(path, p5_issues, "p5")
    if not rows:
        return {"status": "FAIL", "reason": "P>0 file had no rows", "issues": p5_issues}
    p_events_total = 0
    p_max_seen = 0
    for r in rows:
        if not r.get("invariant_ok"):
            p5_issues.append(f"p5: row rep={r.get('rep')} arm={r.get('arm')} "
                             "pending-control invariant broken")
        p_events_total += int(r.get("p_events", 0))
        p_max_seen = max(p_max_seen, int(r.get("p_max", 0)))
    if p_events_total == 0:
        p5_issues.append("p5: no row shows an actual P>0 observation; cost evidence is missing")
        return {"status": "NOT_MEASURED", "reason": "P>0 never actually occurred",
                "p_events_total": 0, "p_max": p_max_seen, "issues": p5_issues}
    per_cfg = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in rows:
        if r.get("phase") == "timed":
            per_cfg[r["cfg"]][r["arm"]].append(r["cpu_ns_per_op"])
    summary = {"status": "PENDING", "p_events_total": p_events_total,
               "p_max": p_max_seen, "per_cfg": {}, "issues": p5_issues}
    p_pass = True
    for cfg, arms in sorted(per_cfg.items()):
        b, e = arms.get("base", []), arms.get("e1", [])
        pairs = min(len(b), len(e))
        if pairs < MIN_PAIRS:
            p5_issues.append(f"p5: cfg {cfg} has {pairs} pairs < {MIN_PAIRS}")
            p_pass = False
            continue
        ratios = [x / y for x, y in zip(sorted(e), sorted(b))][:pairs]
        med = statistics.median(ratios)
        summary["per_cfg"][cfg] = {"pairs": pairs, "median_ratio": med}
        if med > P5_REGRESSION_MAX:
            p5_issues.append(f"p5: cfg {cfg} median ratio {med:.4f} > {P5_REGRESSION_MAX}")
            p_pass = False
    summary["status"] = "PASS" if p_pass and not p5_issues else "REVISE" if p_pass else "FAIL"
    summary["issues"] = p5_issues
    issues.extend(p5_issues)
    return summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--w02", required=True, help="campaign results dir")
    ap.add_argument("--w03", help="W-03 A/B JSONL (optional)")
    ap.add_argument("--p5", help="P>0 pressure JSONL (optional)")
    ap.add_argument("--semantic", help="R1 semantic report JSON (optional)")
    ap.add_argument("--out", default="verdict.json")
    args = ap.parse_args()

    issues = []
    prov = check_identity(args.w02, issues)
    per_cfg = rebuild_w02(args.w02, issues)
    w02_status = gate_w02(per_cfg, issues)

    semantic = None
    if args.semantic:
        with open(args.semantic) as fh:
            semantic = json.load(fh)
        if not semantic.get("suites_all_pass"):
            fail(issues, "semantic: a deterministic suite failed")
        if not semantic.get("mutants_all_killed"):
            fail(issues, "semantic: a mutant survived")
        if not semantic.get("named_oracle_kills", 0) >= 2:
            fail(issues, "semantic: missing-increment/decrement lack named-oracle kills")
        if not semantic.get("adversarial_pass"):
            fail(issues, "semantic: adversarial scenarios failed")

    w03 = read_w03(args.w03, issues)
    p5 = read_p5(args.p5, issues)

    correctness_fail = any("not ok" in i or "completion errors" in i or
                           "invalid-state" in i or "parity" in i or
                           "invariant" in i or "lost wake" in i for i in issues)
    w02_evidence_fail = any(i.startswith("w02:") or i.startswith("identity:") for i in issues)

    if correctness_fail:
        overall = "CORRECTNESS_FAIL"
    elif w02_status != "PASS" or w02_evidence_fail:
        overall = "BLOCKED_W02_FAIL"
    elif semantic is not None and any("semantic:" in i for i in issues):
        overall = "BLOCKED_SEMANTIC_FAIL"
    elif w03["status"] == "PASS" and p5["status"] == "PASS":
        overall = "NATIVE_GO"
    elif w03["status"] in ("PASS", "NOT_RUN") and p5["status"] in ("PASS", "NOT_MEASURED"):
        overall = "CONDITIONAL"
    else:
        overall = "BLOCKED"

    verdict = {
        "schema": "sluice-e1-native-linux-verdict/1",
        "w02": {
            "status": w02_status,
            "valid_pairs_required": MIN_PAIRS,
            "thresholds": {
                "headline_gain_max": HEADLINE_GAIN_MAX,
                "headline_pairs_below": HEADLINE_PAIRS_BELOW,
                "nonreg_max": NONREG_MAX,
            },
            "per_cfg": per_cfg,
            "base_sha": prov.get("base_sha"),
            "e1_sha": prov.get("e1_sha"),
        },
        "semantic": semantic,
        "w03": w03,
        "p_positive": p5,
        "issues": issues,
        "multiple_comparison_note":
            "each cfg is a separate paired test; headline and non-regression "
            "gates are preregistered, but 11 simultaneous comparisons inflate "
            "the family-wise error rate and ratios are not independent",
        "overall": overall,
        "exit_rule": "exit 0 iff identity, correctness and W-02 gates pass; "
                     "W-03/P>0 presence downgrades the overall research verdict to "
                     "CONDITIONAL, never to a silent NATIVE_GO",
    }
    with open(args.out, "w") as fh:
        json.dump(verdict, fh, indent=2, sort_keys=True)
        fh.write("\n")

    for i in issues:
        print(f"ISSUE: {i}", file=sys.stderr)
    print(f"w02={w02_status} w03={w03['status']} p5={p5['status']} overall={overall}")
    sys.exit(0 if (w02_status == "PASS" and not w02_evidence_fail and
                   not correctness_fail and
                   (semantic is None or not any("semantic:" in i for i in issues))) else 1)


if __name__ == "__main__":
    main()
