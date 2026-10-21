#!/usr/bin/env bash
# Independent validation of an M2 A/B JSONL before any timing claim.
#
# Success criteria: every (config, arm, rep) has exactly one timed row, the
# byte oracle passed in that rep, the timed phase itself passed with an empty
# failure, the op count matches the request count, and the timed phase
# performed no dynamic allocation. Exits nonzero otherwise.
set -euo pipefail

JSONL="${1:?usage: validate-results.sh <ab.jsonl> [reps]}"
REPS="${2:-15}"
HERE="$(cd "$(dirname "$0")" && pwd)"

python3 "$HERE/analyze.py" "$JSONL" --reps "$REPS" --json-out "${JSONL%.jsonl}-summary.json"
status=$?

rows=$(wc -l < "$JSONL")
oracle_fail=$(grep -c '"phase":"oracle","ok":false' "$JSONL" || true)
timed_alloc=$(grep '"phase":"timed"' "$JSONL" | grep -vc '"allocations":0' || true)
timed_fail=$(grep -c '"phase":"timed","ok":false' "$JSONL" || true)
echo "rows=$rows oracle_failures=$oracle_fail timed_failures=$timed_fail timed_rows_with_allocations=$timed_alloc"
if [ "$oracle_fail" -ne 0 ] || [ "$timed_fail" -ne 0 ] || [ "$timed_alloc" -ne 0 ]; then
  echo "VALIDATION FAILED"
  exit 1
fi
echo "VALIDATION PASSED"
exit "$status"
