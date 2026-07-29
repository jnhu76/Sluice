#!/usr/bin/env bash
# run_test_repeated.sh — reproducible focused-test repeat runner.
#
# Runs a single xmake test target N times (optionally filtered to one case via
# SLUICE_TEST_FILTER) and reports the pass/fail count. Used for stability
# evidence where a single run is insufficient (concurrency / close-owner
# election / startup-abort races). This is the COMMITTED reproducible runner
# (not an ad-hoc shell loop): the command below is exactly reproducible.
#
# Usage:
#   scripts/run_test_repeated.sh <test-target> <count> [case-filter]
#
# Example:
#   scripts/run_test_repeated.sh application_runtime_resource_test 1000
#   scripts/run_test_repeated.sh application_runtime_identity_test 1000 c2_task
#
# Exit non-zero if any iteration fails. Prints a one-line per-iteration trace
# and a summary. The test target must already be built (xmake build <target>).
set -euo pipefail

if [[ $# -lt 2 ]]; then
  printf 'usage: %s <test-target> <count> [case-filter]\n' "$0" >&2
  exit 2
fi
target="$1"
count="$2"
filter="${3:-}"

pass=0
fail=0
for ((i = 1; i <= count; ++i)); do
  if [[ -n "$filter" ]]; then
    out=$(SLUICE_TEST_FILTER="$filter" xmake run "$target" 2>&1) || true
  else
    out=$(xmake run "$target" 2>&1) || true
  fi
  if printf '%s' "$out" | grep -q 'ALL TESTS PASSED'; then
    pass=$((pass + 1))
    printf '.'
  else
    fail=$((fail + 1))
    printf '\nITER %d FAILED:\n%s\n' "$i" "$out" | tail -20
  fi
done
printf '\n=== %s: %d/%d passed, %d failed ===\n' "$target" "$pass" "$count" "$fail"
if ((fail != 0)); then exit 1; fi
