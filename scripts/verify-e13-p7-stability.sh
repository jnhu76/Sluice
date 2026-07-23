#!/usr/bin/env bash
# verify-e13-p7-stability.sh — reproducible E13 P7 rollback stability gate.
#
# Reuses the generic stability-loop discipline (see verify-e8-stability.sh):
# run ONE test binary N times, stop on the first failure, print one result per
# iteration + a summary. A test case may be selected via $SLUICE_TEST_FILTER
# (substring; see tests/harness.hpp). This makes the P7 stability gates
# reproducible from committed artifacts without ad hoc shell loops.
#
# Usage:
#   scripts/verify-e13-p7-stability.sh MODE BINARY [FILTER] [COUNT]
#   scripts/verify-e13-p7-stability.sh debug   e13_select_rollback p7_t1 1000
#   scripts/verify-e13-p7-stability.sh release e13_select_rollback p7_t2 1000
#   scripts/verify-e13-p7-stability.sh asan    e13_select_rollback p7_t8 2000
#
# Arguments:
#   MODE    build mode: release | debug | tsan | asan | ubsan | asanubsan
#   BINARY  test binary name (e.g. e13_select_rollback). Must be an xmake target.
#   FILTER  optional SLUICE_TEST_FILTER token (substring). Empty = whole binary.
#   COUNT   iterations (default 1000). Stop on first failure.
#
# Exit status: 0 iff every iteration passed; 1 otherwise.
set -euo pipefail

if [ "$#" -lt 2 ]; then
  echo "usage: $0 MODE BINARY [FILTER] [COUNT]" >&2
  echo "  e.g. $0 debug e13_select_rollback p7_t1 1000" >&2
  exit 2
fi

MODE="$1"
BINARY="$2"
FILTER="${3:-}"
COUNT="${4:-1000}"

BIN_PATH="build/$(uname -s | tr '[:upper:]' '[:lower:]')/$(uname -m)/${MODE}/${BINARY}"

if [ ! -x "$BIN_PATH" ]; then
  echo "error: binary not found (or not executable): $BIN_PATH" >&2
  echo "       build it first: xmake f -m $MODE && xmake build $BINARY" >&2
  exit 2
fi

export SLUICE_TEST_FILTER="$FILTER"

echo "# verify-e13-p7-stability" >&2
echo "# binary:   $BIN_PATH" >&2
echo "# filter:   '${FILTER:-<none>}'  (SLUICE_TEST_FILTER substring; empty = whole binary)" >&2
echo "# count:    $COUNT  (stop on first failure)" >&2
echo "# host:     $(uname -srm)" >&2
echo "# started:  $(date -u +%FT%TZ)" >&2

passed=0
failed_iter=0
failed_status=0
fail_out=""
start=$(date +%s)

for ((i=1; i<=COUNT; i++)); do
  if ! "$BIN_PATH" >/dev/null 2>&1; then
    failed_iter=$i
    fail_out=$("$BIN_PATH" 2>&1 || true)
    failed_status=1
    break
  fi
  passed=$i
  if (( i % 200 == 0 )); then
    printf '  [%d/%d] ok\n' "$i" "$COUNT" >&2
  fi
done

end=$(date +%s)
elapsed=$((end - start))

echo "# ----- summary -----" >&2
echo "# binary=$BINARY filter='${FILTER:-}' requested=$COUNT passed=$passed" >&2
if [ "$failed_status" -ne 0 ]; then
  echo "# FAILED at iteration $failed_iter/$COUNT" >&2
  echo "# exit status: 1" >&2
  echo "# failure output (last run):" >&2
  printf '%s\n' "$fail_out" | sed 's/^/#   /' >&2
  exit 1
fi
echo "# exit status: 0" >&2
echo "# elapsed: ${elapsed}s" >&2
echo "PASS $BINARY filter='${FILTER:-}' $passed/$COUNT ($MODE)"
exit 0
