#!/usr/bin/env python3
"""Validates one G0 benchmark run against the frozen preregistered expectation.

Exit status is the contract: 0 only when the run is exactly what the frozen
matrix predicts, otherwise 1 with FAIL lines naming every deviation. The runner
(`bench/run_matrix.sh`) aborts on a non-zero result.

Warmup and measured repetition counts are taken from the runner's arguments
because they are part of the frozen design; the expected case set and the
expected NOT_RUN set are read from the frozen raw evidence, so a case that
silently disappears, renames itself or stops being skipped is a failure here.
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze import REQUIRED_FIELDS  # noqa: E402

NOT_RUN_MARKER = "NOT_RUN_WITH_REASON"
SELF_TEST_CASE = "CAL-oracle-self-test"


def load(path: Path):
    """Parses a JSONL file. Returns (records, problems)."""
    problems = []
    if not path.is_file():
        problems.append(f"{path} was not produced")
        return [], problems
    records = []
    text = path.read_text()
    if not text.strip():
        problems.append(f"{path} is empty")
        return [], problems
    for lineno, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError as exc:
            problems.append(f"{path}:{lineno} is not valid JSON: {exc}")
    return records, problems


def case_ids(records, predicate=lambda r: True):
    return {r["case_id"] for r in records if predicate(r)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("jsonl", help="raw JSONL produced by one benchmark run")
    parser.add_argument("--stage", required=True,
                        choices=["measured", "diagnostics", "notrun"])
    parser.add_argument("--exit-code", type=int, required=True,
                        help="exit status the benchmark process returned")
    parser.add_argument("--warmup", type=int, required=True)
    parser.add_argument("--measured", type=int, required=True)
    parser.add_argument("--reference", help="frozen raw JSONL for the same stage")
    parser.add_argument("--expect-cases", type=int, required=True,
                        help="preregistered number of cases for this stage")
    args = parser.parse_args()

    path = Path(args.jsonl)
    problems = []
    records, load_problems = load(path)
    problems.extend(load_problems)

    if records:
        missing = sorted({f for r in records for f in REQUIRED_FIELDS if f not in r})
        if missing:
            problems.append(f"records are missing required fields: {missing}")

    if "failure" not in (records[0] if records else {}):
        problems.append("records carry no 'failure' field, cannot classify failures")

    by_case = defaultdict(list)
    for record in records:
        by_case[record["case_id"]].append(record)

    for case_id, group in sorted(by_case.items()):
        warmups = [r for r in group if not r["measured"]]
        measured_reps = [r for r in group if r["measured"]]
        if case_id == SELF_TEST_CASE:
            if len(group) != 1 or group[0]["measured"] or not group[0]["ok"]:
                problems.append(f"{case_id}: expected exactly one passing gate record")
            continue
        if len(warmups) != args.warmup:
            problems.append(
                f"{case_id}: {len(warmups)} warmup records, expected {args.warmup}")
        if len(measured_reps) != args.measured:
            problems.append(
                f"{case_id}: {len(measured_reps)} measured records, "
                f"expected {args.measured}")

    if records and SELF_TEST_CASE not in by_case:
        problems.append(f"{SELF_TEST_CASE} record is missing")

    workload_cases = [c for c in by_case if not c.startswith("CAL-")]
    if records and len(workload_cases) != args.expect_cases:
        problems.append(
            f"{len(workload_cases)} cases ran, but {args.expect_cases} are "
            "preregistered for this stage")

    not_run = sorted(case_ids(records, lambda r: not r["ok"]))
    unexplained = sorted(
        r["case_id"] for r in records
        if not r["ok"] and NOT_RUN_MARKER not in r.get("failure", ""))
    if unexplained:
        problems.append(f"cases failed without a reason marker: {unexplained}")
    for case_id in not_run:
        markers = {r.get("failure", "") for r in by_case[case_id]}
        if not any(NOT_RUN_MARKER in m for m in markers):
            problems.append(f"{case_id}: failing record does not state {NOT_RUN_MARKER}")

    if args.reference:
        expected, ref_problems = load(Path(args.reference))
        problems.extend(f"reference {p}" for p in ref_problems)
        if expected:
            expected_ids = case_ids(expected)
            if case_ids(records) != expected_ids:
                problems.append(
                    "case set differs from the frozen matrix: missing "
                    f"{sorted(expected_ids - case_ids(records))}, unexpected "
                    f"{sorted(case_ids(records) - expected_ids)}")
            expected_not_run = sorted(case_ids(expected, lambda r: not r["ok"]))
            if not_run != expected_not_run:
                problems.append(
                    f"NOT_RUN set differs from the frozen expectation: got {not_run}, "
                    f"expected {expected_not_run}")

    if args.stage == "notrun":
        if args.exit_code != 1:
            problems.append(
                f"exit={args.exit_code}, expected 1 (the expected NOT_RUN cases must "
                "fail the process, and no other status is a valid control result)")
        if not not_run:
            problems.append("control run reported no NOT_RUN case")
    elif args.exit_code != 0:
        problems.append(f"exit={args.exit_code}, expected 0")
    if args.stage != "notrun" and not_run:
        problems.append(f"NOT_RUN cases are not allowed in a {args.stage} run: {not_run}")

    for record in records:
        depth = record.get("queue_depth", 0)
        outstanding = record.get("stats_max_outstanding", 0)
        if depth and outstanding > depth:
            problems.append(
                f"{record['case_id']}: max_outstanding {outstanding} exceeded "
                f"the declared bound {depth}")

    if problems:
        for problem in problems:
            print(f"FAIL: {problem}")
        print(f"check_run: {path.name} rejected ({len(problems)} problem(s))")
        return 1

    cases = len(by_case) - (1 if SELF_TEST_CASE in by_case else 0)
    print(f"check_run: OK {path.name} stage={args.stage} cases={len(workload_cases)} "
          f"records={len(records)} warmup_per_case={args.warmup} "
          f"measured_per_case={args.measured} not_run={len(not_run)} "
          f"exit={args.exit_code}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
