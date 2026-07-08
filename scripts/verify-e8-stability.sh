#!/usr/bin/env bash
# verify-e8-stability.sh — reproducible E8/E7 stress-gate runner.
#
# Runs ONE selected test binary N times, stopping on the first failure, and
# prints a one-line result per iteration plus a summary. Selects an
# individual test case via $SLUICE_TEST_FILTER (substring match; see
# tests/harness.hpp). This makes the E8/E7 stability gates reproducible from
# committed artifacts without ad hoc shell loops.
#
# Usage:
#   scripts/verify-e8-stability.sh MODE BINARY [FILTER] [COUNT]
#   scripts/verify-e8-stability.sh release e8_steal_test e8_t3 1000
#   scripts/verify-e8-stability.sh debug   e8_steal_test e8_t11 2000
#   scripts/verify-e8-stability.sh release e7_worker_test "" 1000
#
# Arguments:
#   MODE    build mode: release | debug | tsan | asan | ubsan | asanubsan
#   BINARY  test binary name (e.g. e8_steal_test). Must be an xmake target.
#   FILTER  optional SLUICE_TEST_FILTER token (substring, e.g. e8_t3).
#           Use a precise token to avoid multi-match (e8_t1 also matches
#           e8_t10/e8_t11). Empty/unset => run the whole binary.
#   COUNT   iterations (default 1000). Stop on first failure.
#
# Exit status: 0 iff every iteration passed; 1 otherwise.
#
# NOTE on filter precision: SLUICE_TEST_FILTER is a substring allowlist
# (tests/harness.hpp). "e8_t3" matches only e8_t3_...; "e8_t11" matches only
# e8_t11_...; but "e8_t1" matches e8_t1, e8_t10, AND e8_t11. Always use the
# full case tag (e8_t3, e8_t4, e8_t11) — never an ambiguous prefix.
set -euo pipefail

if [ "$#" -lt 2 ]; then
  echo "usage: $0 MODE BINARY [FILTER] [COUNT]" >&2
  echo "  e.g. $0 release e8_steal_test e8_t3 1000" >&2
  exit 2
fi

MODE="$1"
BINARY="$2"
FILTER="${3:-}"
COUNT="${4:-1000}"

# Resolve the binary path the same way run_core_microbenches.sh does.
BIN_PATH="build/$(uname -s | tr '[:upper:]' '[:lower:]')/$(uname -m)/${MODE}/${BINARY}"

if [ ! -x "$BIN_PATH" ]; then
  echo "error: binary not found (or not executable): $BIN_PATH" >&2
  echo "       build it first: xmake f -m $MODE && xmake build $BINARY" >&2
  exit 2
fi

export SLUICE_TEST_FILTER="$FILTER"

echo "# verify-e8-stability" >&2
echo "# binary:   $BIN_PATH" >&2
echo "# filter:   '${FILTER:-<none>}'  (SLUICE_TEST_FILTER substring; empty = whole binary)" >&2
echo "# count:    $COUNT  (stop on first failure)" >&2
echo "# host:     $(uname -srm)" >&2
echo "# started:  $(date -u +%FT%TZ)" >&2

passed=0
failed_iter=0
failed_status=0
start=$(date +%s)

for ((i=1; i<=COUNT; i++)); do
  if ! "$BIN_PATH" >/dev/null 2>&1; then
    failed_iter=$i
    # Re-run capturing output so the summary shows which case/assert fell.
    fail_out=$("$BIN_PATH" 2>&1 || true)
    failed_status=1
    break
  fi
  passed=$i
  if (( i % 100 == 0 )); then
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
echo "# result: ALL PASSED ($passed/$COUNT) in ${elapsed}s" >&2
echo "# exit status: 0" >&2
exit 0
