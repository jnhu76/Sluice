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
#   scripts/run_test_repeated.sh application_runtime_identity_test 1000 c2_task_cannot_self_close
#
# case-filter is passed as $SLUICE_TEST_FILTER and must be an EXACT registered
# case name (tests/harness.hpp matches exact names only; a filter that matches
# zero cases makes the binary exit non-zero, so a stale or partial name cannot
# produce false stability evidence).
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

# count MUST be a positive integer. A non-numeric or <=0 count would otherwise
# make the loop body never execute and print a misleading "0/N passed, 0
# failed" summary that exits 0 (false stability evidence).
if ! [[ "$count" =~ ^[0-9]+$ ]] || ((count <= 0)); then
  printf 'error: <count> must be a positive integer, got: %s\n' "$count" >&2
  exit 2
fi

pass=0
fail=0
for ((i = 1; i <= count; ++i)); do
  # Capture the REAL process exit code. A test that prints "ALL TESTS PASSED"
  # and then crashes during teardown (destructor/background-thread abort) makes
  # xmake return non-zero even though the success banner appeared in stdout.
  # Treating that as PASS (the old `|| true` + banner-grep behavior) would
  # manufacture false N/N stability evidence. An iteration is PASS only when
  # BOTH hold: exit code is 0 AND the success banner is present.
  if [[ -n "$filter" ]]; then
    out=$(SLUICE_TEST_FILTER="$filter" xmake run "$target" 2>&1) && rc=0 || rc=$?
  else
    out=$(xmake run "$target" 2>&1) && rc=0 || rc=$?
  fi
  if ((rc == 0)) && printf '%s' "$out" | grep -q 'ALL TESTS PASSED'; then
    pass=$((pass + 1))
    printf '.'
  else
    fail=$((fail + 1))
    printf '\nITER %d FAILED (exit=%d):\n%s\n' "$i" "$rc" "$out" | tail -20
  fi
done
printf '\n=== %s: %d/%d passed, %d failed ===\n' "$target" "$pass" "$count" "$fail"
if ((fail != 0)); then exit 1; fi
