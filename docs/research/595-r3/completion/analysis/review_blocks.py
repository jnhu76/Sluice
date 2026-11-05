#!/usr/bin/env python3
"""Independent review of SAFE-A0 / DIAG / RESOURCE blocks (raw/protection).

Recomputes enter classification from raw strace files, checks ops/exit/oracle,
thread sampling, and the ANALYSIS-SELFTEST log. No benchmark is run.
"""
import glob
import json
import os
import re
import sys

ROOT = "/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z"
P = os.path.join(ROOT, "raw", "protection")
R3_SHA = "0ebfb7f0c204579fe2cc9e8d7e88cee2de9112e25c0cfb11e8502e59fc4fe2d1"

ENTER_RE = re.compile(
    r"io_uring_enter\((\d+),\s*(\d+),\s*(\d+),\s*([^,]*),")


def classify_strace(path):
    """Return (total, pure_submit, to_submit_hist, getevents_lines)."""
    total = pure = ge = 0
    hist = {}
    with open(path, errors="replace") as f:
        for line in f:
            m = ENTER_RE.search(line)
            if not m:
                continue
            total += 1
            ts, mc, flags = int(m.group(2)), int(m.group(3)), m.group(4)
            hist[ts] = hist.get(ts, 0) + 1
            if "GETEVENTS" in flags:
                ge += 1
            if ts > 0 and mc == 0 and "GETEVENTS" not in flags:
                pure += 1
    return {"total": total, "pure_submit": pure, "to_submit_hist": hist,
            "getevents_lines": ge}


def load(path):
    return [json.loads(l) for l in open(path) if l.strip()]


def check_diag(block):
    rec = load(os.path.join(P, block, "results.jsonl"))[0]
    strace = sorted(glob.glob(os.path.join(P, block, "logs", "*.strace")))
    mine = classify_strace(strace[0]) if strace else None
    claimed = rec.get("enter")
    ok = mine is not None and claimed is not None and (
        mine["total"] == claimed["lines_with_enter"] and
        mine["pure_submit"] == claimed["pure_submit"] and
        {str(k): v for k, v in mine["to_submit_hist"].items()} == claimed["to_submit_hist"])
    return {"block": block, "case": rec.get("case"), "exit": rec.get("exit"),
            "mine": mine, "claimed": claimed, "strace_match": ok,
            "ops": rec.get("metrics", {}).get("ops")}


def check_simple(block, expect_ops=None, expect_n=None, expect_pure_submit_zero=False):
    recs = load(os.path.join(P, block, "results.jsonl"))
    out = {"block": block, "n": len(recs), "exits": sorted(set(r["exit"] for r in recs)),
           "valid": all(r.get("valid") for r in recs),
           "sha_ok": all(r.get("binary_sha256") == R3_SHA for r in recs),
           "case": recs[0].get("case")}
    if expect_n is not None:
        out["n_ok"] = len(recs) == expect_n
    ops = set()
    ok_all = True
    for r in recs:
        meas = [x for x in r.get("reps", []) if x.get("measured")]
        for x in meas:
            ops.add(x.get("ops"))
            ok_all = ok_all and bool(x.get("ok"))
        m = r.get("metrics", {})
        if m.get("ops") is not None:
            ops.add(m.get("ops"))
    out["ops"] = sorted(o for o in ops if o is not None)
    out["oracle_ok"] = ok_all
    if expect_ops is not None:
        out["ops_ok"] = all(o == expect_ops for o in out["ops"])
    if expect_pure_submit_zero:
        ent = [r.get("enter") for r in recs if r.get("enter")]
        out["enter_totals"] = sorted(set(e["total"] for e in ent))
        out["enter_pure_submit"] = sorted(set(e["pure_submit"] for e in ent))
    return out


def main():
    report = {"diag": [], "safe": [], "misc": {}}

    for b in ("DIAG-R2-D3-A", "DIAG-R2-D3-B", "DIAG-R2-D16-A", "DIAG-R2-D16-B",
              "DIAG-64K-A0", "DIAG-THREADPOOL-NOCASE"):
        if os.path.exists(os.path.join(P, b, "results.jsonl")):
            report["diag"].append(check_diag(b))

    report["safe"].append(check_simple("SAFE-A0-W03-64K-D16", expect_ops=2000, expect_n=3))
    report["safe"].append(check_simple("SAFE-A0-W03-64K-D16-threads", expect_n=3))
    for d in ("d4", "d16", "d64"):
        report["safe"].append(check_simple(f"SAFE-A0-W03-THREADPOOL-{d}",
                                           expect_ops=20000, expect_n=3,
                                           expect_pure_submit_zero=True))
    for pl, mask in (("cpu2", "2"), ("cpu8", "8"), ("smt-2-12", "2,12"), ("xphys-2-8", "2,8")):
        r = check_simple(f"SAFE-A0-PLACEMENT-{pl}", expect_ops=20000, expect_n=8)
        recs = load(os.path.join(P, f"SAFE-A0-PLACEMENT-{pl}", "results.jsonl"))
        r["cpu_mask"] = sorted(set(x.get("cpu") for x in recs))
        r["cpu_ok"] = r["cpu_mask"] == [mask]
        report["safe"].append(r)
    sat = check_simple("SAFE-A0-W02-SAT-d16", expect_n=3)
    report["safe"].append(sat)

    # threads sampling for 64K
    thr = load(os.path.join(P, "SAFE-A0-W03-64K-D16-threads", "results.jsonl"))
    report["misc"]["64k_threads"] = [t.get("threads") for t in thr]

    # threadpool strace verification (independent recount)
    tp = {}
    for d in ("d4", "d16", "d64"):
        st = sorted(glob.glob(os.path.join(P, f"SAFE-A0-W03-THREADPOOL-{d}", "logs", "*.strace")))
        cls = [classify_strace(s) for s in st]
        tp[d] = {"straces": len(st),
                 "totals": sorted(set(c["total"] for c in cls)),
                 "pure": sorted(set(c["pure_submit"] for c in cls)),
                 "ge": sorted(set(c["getevents_lines"] for c in cls))}
    report["misc"]["threadpool_strace"] = tp

    # RESOURCE-SYSCALL-A0-D16
    p = os.path.join(P, "RESOURCE-SYSCALL-A0-D16", "a0.jsonl")
    if os.path.exists(p):
        report["misc"]["resource_syscall_a0"] = load(p)

    # ANALYSIS-SELFTEST
    logp = os.path.join(P, "ANALYSIS-SELFTEST", "selftest.log")
    if os.path.exists(logp):
        lines = open(logp).read().splitlines()
        report["misc"]["selftest"] = {
            "pass_lines": sum(1 for l in lines if l.startswith("PASS")),
            "fail_lines": sum(1 for l in lines if l.startswith("FAIL")),
            "failures_line": [l for l in lines if "failures" in l],
        }

    # SAT run.jsonl diag
    dsp = os.path.join(P, "DIAG-SAT-A0", "run.jsonl")
    if os.path.exists(dsp):
        report["misc"]["diag_sat_run"] = load(dsp)

    print(json.dumps(report, indent=1, sort_keys=True))
    with open(os.path.join(ROOT, "analysis", "review_blocks_out.json"), "w") as f:
        json.dump(report, f, indent=1, sort_keys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
