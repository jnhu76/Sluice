#!/usr/bin/env python3
"""Counterfactual selftest for e1_verdict.py (research-only).

Builds a synthetic clean campaign, then injects one defect at a time and
asserts the reducer catches it with the right verdict/exit code. A reducer
that silently accepts any of these counterfactuals would invalidate the
formal campaign, so every case here must fail closed.

Run: python3 e1_verdict_selftest.py [path-to-e1_verdict.py]
"""

import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile

REDUCER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "e1_verdict.py")

CFGS = [
    ("4096", 20000, 16, 1), ("4096", 20000, 64, 1), ("4096", 20000, 256, 1),
    ("4096", 20000, 1024, 1), ("4096", 20000, 16, 8), ("4096", 20000, 1024, 8),
    ("4096", 20000, 64, 64), ("4096", 20000, 1024, 64),
    ("65536", 2000, 1024, 1), ("65536", 2000, 1024, 8),
    ("1048576", 500, 64, 8),
]
REPS = 15
HEADLINE = {("4096", 1024, 1), ("4096", 1024, 8)}


def base_cpu(cfg):
    block, _, capacity, depth = cfg
    return 1500.0 + capacity * 2.0 + depth * 3.0


def e1_cpu(cfg):
    block, _, capacity, depth = cfg
    b = base_cpu(cfg)
    return b * (0.90 if (block, capacity, depth) in HEADLINE else 0.99)


def make_row(cfg, rep, arm, phase, **over):
    block, requests, capacity, depth = cfg
    row = {
        "arm": arm, "backend": "uring", "block": int(block), "depth": depth,
        "capacity": capacity, "scope": depth, "workers": 4, "requests": requests,
        "phase": phase, "ok": True, "wall_s": 0.5 + rep * 0.001,
        "ops_per_sec": requests / 0.5, "cpu_s": 0.03, "cpu_ns_per_op": base_cpu(cfg),
        "utime_ns_per_op": 800.0, "stime_ns_per_op": 700.0, "nvcsw": 10, "nivcsw": 20,
        "max_outstanding": depth, "window_high_water": depth, "ops": requests,
        "bytes": requests * int(block), "allocations": 5, "p50_ns": 100.0,
        "p95_ns": 200.0, "p90_ns": 150.0, "p99_ns": 300.0, "submit_calls": 100,
        "poll_calls": 100, "completed_ops": requests, "completion_errors": 0,
        "canceled_ops": 0, "invalid_state": 0, "queue_full_retries": 0,
        "failure": "", "rep": rep, "order": "AB", "cfg": f"{block}-{capacity}-{depth}",
    }
    if arm == "e1":
        row["cpu_ns_per_op"] = e1_cpu(cfg)
    if phase == "oracle":
        row["cpu_ns_per_op"] = base_cpu(cfg) * 1.01
    row.update(over)
    return row


def campaign_rows():
    rows = []
    for rep in range(1, REPS + 1):
        for cfg in CFGS:
            for arm in ("base", "e1"):
                for phase in ("warmup", "timed", "oracle", "latency"):
                    rows.append(make_row(cfg, rep, arm, phase))
    return rows


def run_case(name, rows, prov_status="status=COMPLETE", expect_exit_zero=True,
             expect_issue=None, extra_args=None, omit_prov_key=None):
    tmp = tempfile.mkdtemp(prefix="e1-selftest-")
    try:
        with open(os.path.join(tmp, "ab.jsonl"), "w") as fh:
            for r in rows:
                fh.write(json.dumps(r) + "\n")
        keys = ["base_sha=aaa", "e1_sha=bbb", "runner_sha256=cc", "harness_source_sha256=dd"]
        if omit_prov_key:
            keys = [k for k in keys if not k.startswith(omit_prov_key)]
        with open(os.path.join(tmp, "provenance.txt"), "w") as fh:
            fh.write("\n".join(keys) + "\n")
        open(os.path.join(tmp, "arm-identity.txt"), "w").write(
            "lib_async_sha256=deadbeef\nbinary_sha256=feedface\n")
        with open(os.path.join(tmp, "campaign-status.txt"), "w") as fh:
            fh.write(prov_status + "\n")
        out_path = os.path.join(tmp, "verdict.json")
        proc = subprocess.run(
            [sys.executable, REDUCER, "--w02", tmp, "--out", out_path] + (extra_args or []),
            capture_output=True, text=True)
        verdict = {}
        if os.path.exists(out_path):
            with open(out_path) as fh:
                verdict = json.load(fh)
        problems = []
        if expect_exit_zero and proc.returncode != 0:
            problems.append(f"expected exit 0, got {proc.returncode}")
        if not expect_exit_zero and proc.returncode == 0:
            problems.append("expected nonzero exit, got 0")
        if expect_issue and expect_issue not in (proc.stderr + json.dumps(verdict)):
            problems.append(f"expected issue containing {expect_issue!r}, got: {proc.stderr.strip()[:300]}")
        if problems:
            print(f"FAIL {name}: " + "; ".join(problems))
            return False
        print(f"ok {name}")
        return True
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    results = []

    good = campaign_rows()
    results.append(run_case("positive_control_clean_campaign_passes", good,
                            expect_issue=None))

    rows = campaign_rows()
    rows = [r for r in rows if not (r["rep"] == 7 and r["cfg"] == "4096-64-1" and
                                    r["arm"] == "e1" and r["phase"] == "timed")]
    results.append(run_case("missing_timed_row_rejected", rows,
                            expect_exit_zero=False, expect_issue="missing timed pair"))

    rows = campaign_rows()
    dup = [r for r in rows if r["rep"] == 3 and r["cfg"] == "4096-64-64" and
           r["arm"] == "base" and r["phase"] == "timed"][0]
    rows.append(dict(dup))
    results.append(run_case("duplicate_row_rejected", rows,
                            expect_exit_zero=False, expect_issue="duplicate cell"))

    rows = campaign_rows()
    for r in rows:
        if r["rep"] == 2 and r["arm"] == "base" and r["phase"] == "oracle":
            r["ok"] = False
            r["failure"] = "byte mismatch"
    results.append(run_case("failing_oracle_rejected", rows,
                            expect_exit_zero=False, expect_issue="not ok"))

    rows = campaign_rows()
    for r in rows:
        if r["cfg"] == "4096-64-1" and r["arm"] == "e1" and r["phase"] == "timed":
            r["cpu_ns_per_op"] = r["cpu_ns_per_op"] * 1.06
    results.append(run_case("uniform_regression_rejected", rows,
                            expect_exit_zero=False, expect_issue="median ratio"))

    rows = campaign_rows()
    for r in rows:
        if r["cfg"] == "4096-1024-8" and r["arm"] == "e1" and r["phase"] == "timed":
            r["cpu_ns_per_op"] = r["cpu_ns_per_op"] * 1.10
    results.append(run_case("headline_gain_miss_rejected", rows,
                            expect_exit_zero=False, expect_issue="gain gate"))

    rows = campaign_rows()
    for r in rows:
        if r["rep"] == 5 and r["cfg"] == "4096-16-8" and r["arm"] == "e1":
            r["arm"] = "base"
    results.append(run_case("same_arm_pair_rejected", rows,
                            expect_exit_zero=False, expect_issue="duplicate cell"))

    rows = campaign_rows()
    rows.append(make_row(("4096", 20000, 512, 4), 16, "base", "timed"))
    results.append(run_case("unknown_config_rejected", rows,
                            expect_exit_zero=False, expect_issue="unknown config"))

    rows = campaign_rows()
    for r in rows:
        if r["rep"] == 9 and r["cfg"] == "65536-1024-8" and r["phase"] == "timed":
            r["cpu_ns_per_op"] = "fast"
    results.append(run_case("non_numeric_measurement_rejected", rows,
                            expect_exit_zero=False, expect_issue="illegal cpu_ns_per_op"))

    rows = campaign_rows()
    for r in rows:
        if r["cfg"] == "4096-256-1" and r["arm"] == "e1" and r["phase"] == "timed" and r["rep"] == 4:
            r["max_outstanding"] = 999
    results.append(run_case("depth_gap_rejected", rows,
                            expect_exit_zero=False, expect_issue="achieved in-flight"))

    rows = campaign_rows()
    for r in rows:
        if r["rep"] == 6 and r["arm"] == "base" and r["phase"] == "timed":
            r["completed_ops"] = r["completed_ops"] - 1
    results.append(run_case("short_campaign_ops_rejected", rows,
                            expect_exit_zero=False, expect_issue="completed_ops"))

    results.append(run_case("incomplete_campaign_status_rejected", campaign_rows(),
                            prov_status="status=FAILED", expect_exit_zero=False,
                            expect_issue="campaign status"))

    results.append(run_case("campaign_without_identity_rejected", campaign_rows(),
                            expect_exit_zero=False, expect_issue="provenance lacks",
                            omit_prov_key="base_sha"))

    # W-03 and P>0 presence rules: absent -> CONDITIONAL, never NATIVE_GO.
    tmp = tempfile.mkdtemp(prefix="e1-selftest-final-")
    try:
        with open(os.path.join(tmp, "ab.jsonl"), "w") as fh:
            for r in good:
                fh.write(json.dumps(r) + "\n")
        with open(os.path.join(tmp, "provenance.txt"), "w") as fh:
            fh.write("base_sha=aaa\ne1_sha=bbb\nrunner_sha256=cc\nharness_source_sha256=dd\n")
        open(os.path.join(tmp, "arm-identity.txt"), "w").write(
            "lib_async_sha256=deadbeef\nbinary_sha256=feedface\n")
        with open(os.path.join(tmp, "campaign-status.txt"), "w") as fh:
            fh.write("status=COMPLETE\n")
        out_path = os.path.join(tmp, "verdict.json")
        proc = subprocess.run([sys.executable, REDUCER, "--w02", tmp, "--out", out_path],
                              capture_output=True, text=True)
        with open(out_path) as fh:
            verdict = json.load(fh)
        ok = (proc.returncode == 0 and verdict["overall"] == "CONDITIONAL" and
              verdict["w03"]["status"] == "NOT_RUN" and
              verdict["p_positive"]["status"] == "NOT_MEASURED")
        print(("ok " if ok else "FAIL ") +
              "missing_w03_p5_yields_conditional_not_native_go")
        results.append(ok)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    failed = sum(1 for r in results if not r)
    print(f"{len(results) - failed}/{len(results)} reducer selftest cases passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
