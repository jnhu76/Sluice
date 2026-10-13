#!/usr/bin/env python3
"""Bind every F2 capture in this directory to one implementation head.

Reads the captures' own summary files (not a hand-typed copy), asserts that they
all name the same revision, and writes gates.json. A capture that records a
different SHA fails here instead of being silently averaged into the record.
"""
import argparse
import hashlib
import json
import pathlib
import re

parser = argparse.ArgumentParser()
parser.add_argument("--matrix", required=True)
parser.add_argument("--b01", required=True)
parser.add_argument("--probes", required=True)
parser.add_argument("--tla-log", required=True)
parser.add_argument("--ci-json", required=True)
parser.add_argument("--output", required=True)
args = parser.parse_args()


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def read(path):
    return json.loads(pathlib.Path(path).read_text())


matrix_root = pathlib.Path(args.matrix).resolve()
b01_root = pathlib.Path(args.b01).resolve()
probes_root = pathlib.Path(args.probes).resolve()

matrix = read(matrix_root / "summary.json")
b01 = read(b01_root / "summary.json")
probes = read(probes_root / "probes.json")

heads = {"matrix": matrix["sha"],
         "b01-after-n": b01["after-n"]["source_sha"],
         "b01-after-y": b01["after-y"]["source_sha"]}
assert len(set(heads.values())) == 1, ("captures disagree on the head", heads)
head = heads["matrix"]

tla_text = pathlib.Path(args.tla_log).read_text()
tla_verdict = "PASS" if "VERIFY_TLA: PASS" in tla_text else "NOT_PASSED"
stages = len(re.findall(r"^== ", tla_text, re.M))

ci_runs = json.loads(pathlib.Path(args.ci_json).read_text())
ci = [r for r in ci_runs if r["headSha"] == head]
assert ci, ("no CI run recorded for the head", head, [r["headSha"] for r in ci_runs])

profiles = {}
for name, record in matrix["profiles"].items():
    profiles[name] = {
        "mode": record["mode"],
        "liburing": record["liburing"],
        "toolchain": record["toolchain"],
        "targets_run": record["total"],
        "pass_count": record["pass_count"],
        "xmake_test_exit": record.get("xmake_test_exit", "not_applicable_scope"),
        "unavailable": record.get("unavailable", []),
        "mutants_built": len(record["mutants_built"]),
        "results_tsv_sha256": sha256(matrix_root / name / "results.tsv"),
        "build_log_sha256": sha256(matrix_root / name / "build.log") if (matrix_root / name / "build.log").exists() else None,
        "xmake_test_log_sha256": sha256(matrix_root / name / "xmake-test.log") if (matrix_root / name / "xmake-test.log").exists() else None,
    }

gates = {
    "head": head,
    "baseline_master": "0ed16588b7101f108aa7282074ba3d568f9d34e9",
    "capture_heads": heads,
    "matrix": {"root": str(matrix_root), "profiles": profiles},
    "package_capture": {
        "root": str(b01_root),
        "before": b01["before-n"]["source_sha"],
        "after": b01["after-n"]["source_sha"],
        "counts": {stage: {key: b01[stage][key] for key in
                           ("library_objects", "async_archive_members", "installed_headers")}
                   for stage in ("before-n", "before-y", "after-n", "after-y")},
        "package_sha256": {stage: b01[stage]["package_sha256"]
                           for stage in ("before-n", "before-y", "after-n", "after-y")},
        "commands_json_sha256": sha256(b01_root / "commands.json"),
        "raw_manifest_sha256": sha256(b01_root / "raw-manifest.json"),
    },
    "installed_probes": {
        "root": str(probes_root),
        "installed_headers": probes["installed_headers"],
        "standalone_failures": len(probes["standalone_failures"]),
        "odr_run_exit": probes["odr_run"]["exit_code"],
        "removed_headers_present": len(probes["present_removed_headers"]),
        "control": probes["control_prefix"],
        "probes_json_sha256": sha256(probes_root / "probes.json"),
    },
    "tla": {"verdict": tla_verdict, "stage_headers": stages,
            "log_sha256": sha256(args.tla_log)},
    "ci": [{"database_id": r["databaseId"], "status": r["status"],
            "conclusion": r["conclusion"], "head_sha": r["headSha"]} for r in ci],
}
pathlib.Path(args.output).write_text(json.dumps(gates, indent=2) + "\n")
print(json.dumps({"head": head, "tla": tla_verdict,
                  "ci": [g["conclusion"] for g in gates["ci"]],
                  "profiles": {p: [v["pass_count"], v["targets_run"]] for p, v in profiles.items()}}))
