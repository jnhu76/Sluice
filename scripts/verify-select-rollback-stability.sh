#!/usr/bin/env bash
# verify-select-rollback-stability.sh — reproducible Select rollback stability gate.
#
# Reuses the generic stability-loop discipline (see verify-e8-stability.sh):
# run ONE test binary N times, stop on the first failure, print one result per
# iteration + a summary. A test case may be selected via $SLUICE_TEST_FILTER
# (substring; see tests/harness.hpp). This makes the rollback stability gates
# reproducible from committed artifacts without ad hoc shell loops.
#
# Usage:
#   scripts/verify-select-rollback-stability.sh MODE BINARY [FILTER] [COUNT]
#   scripts/verify-select-rollback-stability.sh debug   select_registration_rollback_test p7_t1 1000
#   scripts/verify-select-rollback-stability.sh release select_registration_rollback_test p7_t2 1000
#   scripts/verify-select-rollback-stability.sh asan    select_registration_rollback_test p7_t8 2000
#
# Arguments:
#   MODE    build mode: release | debug | tsan | asan | ubsan | asanubsan
#   BINARY  test binary name (e.g. select_registration_rollback_test). Must be an xmake target.
#   FILTER  optional SLUICE_TEST_FILTER token (substring). Empty = whole binary.
#   COUNT   iterations (default 1000). Stop on first failure.
#
# Exit status: 0 iff every iteration passed; 1 otherwise.
set -euo pipefail

if [ "$#" -lt 2 ]; then
  echo "usage: $0 MODE BINARY [FILTER] [COUNT]" >&2
  echo "  e.g. $0 debug select_registration_rollback_test p7_t1 1000" >&2
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

echo "# verify-select-rollback-stability" >&2
echo "# binary:   $BIN_PATH" >&2
echo "# filter:   '${FILTER:-<none>}'  (SLUICE_TEST_FILTER substring; empty = whole binary)" >&2
echo "# count:    $COUNT  (stop on first failure)" >&2
echo "# host:     $(uname -srm)" >&2
echo "# started:  $(date -u +%FT%TZ)" >&2

passed=0
failed_iter=0
failed_status=0
# Capture the FIRST failure's stdout+stderr verbatim. A flaky failure may not
# reproduce on a second run, so we must NOT re-run the binary to fetch output —
# we record the real first-run output + exit status and never overwrite them.
fail_tmp=$(mktemp -t sluice-stability-fail.XXXXXX)
trap 'rm -f "$fail_tmp"' EXIT
start=$(date +%s)

for ((i=1; i<=COUNT; i++)); do
  # Run ONCE, capturing combined output to the capture file. Preserve the REAL
  # exit status (no `|| true`, no masking): if the binary fails, this is the
  # authoritative failure现场. We read $? immediately into failed_status so a
  # later `set -e` cannot erase it.
  "$BIN_PATH" >"$fail_tmp" 2>&1 || failed_status=$?
  if [ "$failed_status" -ne 0 ]; then
    failed_iter=$i
    passed=$((i - 1))
    break
  fi
  passed=$i
  if (( i % 200 == 0 )); then
    printf '  [%d/%d] ok\n' "$i" "$COUNT" >&2
  fi
done

end=$(date +%s)
elapsed=$((end - start))

# Sanitizer env summary (only the ones that change binary behavior).
san_env=""
for v in ASAN_OPTIONS TSAN_OPTIONS UBSAN_OPTIONS MSAN_OPTIONS; do
  if [ -n "${!v:-}" ]; then san_env+="${v}=${!v} "; fi
done

echo "# ----- summary -----" >&2
echo "# binary=$BINARY filter='${FILTER:-}' requested=$COUNT passed=$passed" >&2
if [ "$failed_iter" -ne 0 ]; then
  echo "# FAILED at iteration $failed_iter/$COUNT" >&2
  echo "# exit code:   $failed_status" >&2
  echo "# sanitizer:   ${san_env:-<none>}" >&2
  echo "# commit:      $(git -C "$(dirname "$0")/.." rev-parse --short HEAD 2>/dev/null || echo unknown)" >&2
  echo "# first-failure output (run ${failed_iter}, captured once, NOT re-run):" >&2
  sed 's/^/#   /' "$fail_tmp" >&2
  exit 1
fi
echo "# exit status: 0" >&2
echo "# elapsed: ${elapsed}s" >&2
echo "PASS $BINARY filter='${FILTER:-}' $passed/$COUNT ($MODE)"
exit 0
