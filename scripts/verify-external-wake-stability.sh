#!/usr/bin/env bash
# verify-external-wake-stability.sh — reproducible E9 stress-gate runner.
#
# Thin wrapper over verify-e8-stability.sh (which is the canonical, generic,
# binary-agnostic reproducible runner for sluice stress gates). Selects ONE
# test binary and runs it N times, stopping on the first failure, with an
# optional $SLUICE_TEST_FILTER to target a single case.
#
# The E9 load-bearing cases (external-thread wake, MIXED-WAKE) involve real
# OS-thread timing, so high-count stress is the liveness/correctness proof.
# Recommended gates:
#   scripts/verify-external-wake-stability.sh release external_wake_test "" 500
#   scripts/verify-external-wake-stability.sh release external_wake_test wake_external_thread_wakes_parked_scheduler 1000
#   scripts/verify-external-wake-stability.sh release external_wake_test wake_mixed_wake_external_wake_not_blocked_by_backend 1000
#
# Usage / arguments / exit status: identical to verify-e8-stability.sh.
#   scripts/verify-external-wake-stability.sh MODE BINARY [FILTER] [COUNT]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$here/verify-e8-stability.sh" "$@"
