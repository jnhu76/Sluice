#!/usr/bin/env python3
"""Reports what a benchmark run actually executed, from its own JSONL records.

The capture scripts ask the harness for one case and a repetition count; this
turns that request into a fact by reading back the records the harness emitted.
A mistyped case filter, a swallowed argument or a crash mid-run then fails the
capture instead of producing empty or mislabelled evidence.

Usage: verify_capture.py <jsonl> [<expected-case-id> <expected-measured-reps>]
Output: one `key=value` line per field, ending with `verdict=...`
With only the JSONL path it describes the run instead of checking it.
"""
import json
import sys


def main():
    if len(sys.argv) not in (2, 4):
        print("usage: verify_capture.py <jsonl> [<case-id> <measured-reps>]",
              file=sys.stderr)
        return 2
    path = sys.argv[1]
    describe_only = len(sys.argv) == 2
    expected_case = None if describe_only else sys.argv[2]
    expected_reps = None if describe_only else int(sys.argv[3])
    try:
        records = [json.loads(l) for l in open(path)]
    except Exception as exc:  # noqa: BLE001 - the reason is the useful output
        print(f"case_ran=UNREADABLE")
        print(f"verdict=JSONL_UNREADABLE ({exc})")
        return 1
    cases = {r["case_id"] for r in records if r["case_id"] != "CAL-oracle-self-test"}
    case = next(iter(cases)) if len(cases) == 1 else ",".join(sorted(cases)) or "NONE"
    tail = [r for r in records if r["case_id"] == case]
    warmup = [r for r in tail if not r["measured"]]
    measured = [r for r in tail if r["measured"]]
    failing = [r for r in tail if not r.get("ok", True)]
    ops = sum(r["ops"] for r in tail)

    verdict = "OK"
    if describe_only:
        pass
    elif case != expected_case:
        verdict = "CASE_MISMATCH"
    elif len(warmup) != 1 or len(measured) != expected_reps:
        verdict = "REP_COUNT_MISMATCH"
    elif failing:
        verdict = "FAILING_REPETITIONS"

    print(f"case_ran={case}")
    print(f"warmup_reps={len(warmup)}")
    print(f"measured_reps={len(measured)}")
    print(f"failing_reps={len(failing)}")
    print(f"ops_recorded={int(ops)}")
    print(f"verdict={verdict}")
    return 0 if verdict == "OK" else 1


if __name__ == "__main__":
    raise SystemExit(main())
