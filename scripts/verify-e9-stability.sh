#!/usr/bin/env bash
# verify-e9-stability.sh — reproducible E9 stress-gate runner.
#
# Thin wrapper over verify-e8-stability.sh (which is the canonical, generic,
# binary-agnostic reproducible runner for sluice stress gates). Selects ONE
# test binary and runs it N times, stopping on the first failure, with an
# optional $SLUICE_TEST_FILTER to target a single case.
#
# The E9 load-bearing cases (external-thread wake, MIXED-WAKE) involve real
# OS-thread timing, so high-count stress is the liveness/correctness proof.
# Recommended gates:
#   scripts/verify-e9-stability.sh release e9_external_wake_test "" 500
#   scripts/verify-e9-stability.sh release e9_external_wake_test e9_t1 1000
#   scripts/verify-e9-stability.sh release e9_external_wake_test e9_t2 1000
#
# Usage / arguments / exit status: identical to verify-e8-stability.sh.
#   scripts/verify-e9-stability.sh MODE BINARY [FILTER] [COUNT]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$here/verify-e8-stability.sh" "$@"
