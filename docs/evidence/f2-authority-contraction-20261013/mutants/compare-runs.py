#!/usr/bin/env python3
"""Compare the F2 mutant kill sets against the baseline campaign.

Inputs are campaign result files (`<mutant>\t<rc1>\t<rc2>`, two runs per mutant),
one per tree. A mutant is "killed" when a run exits non-zero; the kill set is
compared as a set, and the per-run detail is kept so a timing-flaky mutant can
be told apart from a coverage change.
"""
import argparse
import json
import pathlib

parser = argparse.ArgumentParser()
parser.add_argument("--baseline", required=True)
parser.add_argument("--before-review", required=True)
parser.add_argument("--head", required=True)
parser.add_argument("--output", required=True)
args = parser.parse_args()


def rows(path):
    out = {}
    for line in pathlib.Path(path).read_text().splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        out[parts[0]] = [parts[1], parts[2]]
    return out


trees = {"baseline:0ed16588": rows(args.baseline),
         "f2-before-review:912315ac": rows(args.before_review),
         "f2-head": rows(args.head)}
assert all(len(r) == 64 for r in trees.values()), {k: len(v) for k, v in trees.items()}


def killed(row):
    return row[0] != "0" or row[1] != "0"


def both_runs_killed(row):
    return row[0] != "0" and row[1] != "0"


kills = {name: {m for m, r in rows_.items() if killed(r)} for name, rows_ in trees.items()}
report = {"mutants": len(trees["f2-head"]),
          "any_run_deaths": {name: len(k) for name, k in kills.items()},
          "both_run_deaths": {name: sum(1 for r in rows_.values() if both_runs_killed(r))
                              for name, rows_ in trees.items()},
          "kill_set_equal_to_baseline": kills["baseline:0ed16588"] == kills["f2-head"],
          "survivors": sorted(set(trees["f2-head"]) - kills["f2-head"]),
          "survivors_on_baseline": sorted(set(trees["baseline:0ed16588"]) - kills["baseline:0ed16588"]),
          "one_run_only": {name: sorted(m for m, r in rows_.items() if killed(r) != both_runs_killed(r))
                           for name, rows_ in trees.items()},
          "per_tree": trees}
assert report["kill_set_equal_to_baseline"], report
assert report["survivors"] == report["survivors_on_baseline"], report
pathlib.Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({k: v for k, v in report.items() if k != "per_tree"}))
