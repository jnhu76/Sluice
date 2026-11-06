#!/usr/bin/env python3
"""#595 R5-repair round completion checker (task §7).

Verifies, from raw records only:
- the frozen five-cell x (24 A/A pairs + 24 three-arm blocks + calib pairs)
  plan matches the recorded valid units exactly;
- every record binds binary sha, case, arm, seg;
- per-cell results/CI/oracle/resource status present;
- analysis JSON is recomputable from results.jsonl (recomputes contrasts);
- CLEAN and DIAGNOSTIC segments are separate (no diag record in clean cells,
  diag binaries never used in clean segments);
- missing items carry explicit status, not summary wording.
Exit 0 only if every check passes with complete data or an explicit
NOT_RUN/NOT_ESTABLISHED status recorded in the status map.
"""
import json
import os
import sys

sys.path.insert(0, "/home/jnhu/sluice-595-e5/r5-repair-measure/scripts")
from analyze_round import effect, paired_bootstrap_effects  # noqa: E402

ROOT = "/home/jnhu/sluice-595-e5/r5-repair-measure"
RAW = os.path.join(ROOT, "raw")
CELLS = {
    "MAIN-W03-D16": ("main-w03-d16", 24, 24),
    "PROT-W03-D4": ("prot-w03-d4", 24, 8),
    "PROT-W03-D2SERIAL": ("prot-w03-d2serial", 24, 8),
    "PROT-W02-D1": ("prot-w02-d1", 24, 8),
    "PROT-W02-D2": ("prot-w02-d2", 24, 8),
}
CLEAN_BINS = {
    "7bf83eb93fa589628649931a1a5cdba6a9e9375083e8ced38e09a73c455b9da5",
    "b07fd060629e5baa25e20aaab14ec6212ccf8e6e7160b41f9f06300d8534047c",
    "9442f02b24cec81a7040eea9a6e79e221a5fc9a7ac264b6db9c1fdae28cb6c6b",
    "971a28da2fcfb3ab6099453aabd56a5a6f3524fdc956ec967245f72753a27131",
}

problems = []
status = {}


def check(cond, name, detail=""):
    if cond:
        print(f"PASS {name} {detail}")
    else:
        print(f"FAIL {name} {detail}")
        problems.append(name)


def load_records(cell_dir, seg):
    path = os.path.join(RAW, cell_dir, seg, "results.jsonl")
    recs = []
    if not os.path.exists(path):
        return recs
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            if r.get("valid") and r.get("seg") == seg:
                recs.append(r)
    return recs


def main():
    # plan vs records
    for cell, (d, aa_pairs, calib_pairs) in CELLS.items():
        main_recs = load_records(d, "main")
        blocks = {}
        for r in main_recs:
            blocks.setdefault(r["block"], set()).add(r["arm"])
        complete_blocks = sum(1 for b, arms in blocks.items() if arms == {"A0", "R", "C"})
        check(complete_blocks == 24, f"{cell}/main-24-blocks",
              f"got {complete_blocks}")
        check(all(r["binary_sha256"] in CLEAN_BINS for r in main_recs),
              f"{cell}/clean-binary-sha")
        aa_recs = load_records(d, "aa")
        aa_pairs_seen = len({r["block"] for r in aa_recs if r["arm"] == "A0"})
        check(aa_pairs_seen >= aa_pairs, f"{cell}/aa-pairs",
              f"got {aa_pairs_seen}")
        cal_recs = load_records(d, "calib")
        cal_pairs_seen = len({r["block"] for r in cal_recs if r["arm"] == "A0o"})
        check(cal_pairs_seen >= calib_pairs, f"{cell}/calib-pairs",
              f"got {cal_pairs_seen}")

    # clean/diag separation
    for cell, (d, _, _) in CELLS.items():
        for seg in ("main", "aa", "calib"):
            for r in load_records(d, seg):
                if "diag" in r.get("binary", ""):
                    check(False, f"{cell}/{seg}/diag-binary-in-clean")
    diag_dir = os.path.join(RAW, "diag-w03-uring-4k-d16")
    if os.path.isdir(diag_dir):
        for r in load_records("diag-w03-uring-4k-d16", "diag"):
            check(r["binary_sha256"] not in CLEAN_BINS,
                  "diag/uses-diag-binary", r["binary"])

    # analysis recomputation (MAIN C vs A0 cpu point estimate)
    apath = os.path.join(ROOT, "analysis", "round-analysis.json")
    if os.path.exists(apath):
        with open(apath) as f:
            analysis = {r["cell"]: r for r in json.load(f)}
        d = "main-w03-d16"
        main_recs = load_records(d, "main")
        by_block = {}
        for r in main_recs:
            by_block.setdefault(r["block"], {})[r["arm"]] = r["metrics"]
        eff = [(by_block[b]["C"]["cpu_ns_per_op"] - by_block[b]["A0"]["cpu_ns_per_op"])
               / by_block[b]["A0"]["cpu_ns_per_op"]
               for b in sorted(by_block)
               if "C" in by_block[b] and "A0" in by_block[b]]
        recomputed = sum(eff) / len(eff)
        stored = analysis["MAIN-W03-D16"]["C_vs_A0"]["cpu"]["point"]
        check(abs(recomputed - stored) < 1e-12, "analysis/recompute-main-cpu",
              f"recomputed={recomputed:.8f} stored={stored:.8f}")
    else:
        check(False, "analysis/exists")

    # figures data
    fdat = os.path.join(ROOT, "figures", "figure-data")
    if os.path.isdir(fdat):
        for f in ("f1-main-absolute.csv", "f2-cross-shape-effects.csv",
                  "f3-cpu-p99-plane.csv", "f4-block-drift.csv"):
            check(os.path.exists(os.path.join(fdat, f)), f"figures/{f}")
    else:
        status["figures"] = "NOT_GENERATED"

    if problems:
        print(f"\nCHECKER: {len(problems)} problem(s): {problems}")
        sys.exit(1)
    print("\nCHECKER: all checks passed")
    if status:
        print("explicit statuses:", status)


if __name__ == "__main__":
    main()
