#!/usr/bin/env bash
# verify-e11-formal.sh — reproducible E11 TLA+ / TLC formal gate (M8).
#
# Runs the correct E11 safety + liveness models and all five negative models
# (NEG-1..NEG-5) through TLC, asserting each produces its expected result:
#
#   correct safety     -> all invariants PASS
#   correct liveness   -> DeadlineParkLiveness PASS (I6)
#   NEG-1..NEG-4       -> the named invariant FAILS (counterexample)
#   NEG-5              -> DeadlineParkLiveness FAILS (liveness counterexample)
#
# Requires a tla2tools.jar. By default looks for /tmp/tla2tools.jar (the path
# the E7-E10 READMEs expect); override with TLA2TOOLS_JAR=/path/to/tla2tools.jar
# or fetch v1.8.0 from https://github.com/tlaplus/tlaplus/releases.
#
# Usage:
#   scripts/verify-e11-formal.sh
#   TLA2TOOLS_JAR=/opt/tla2tools.jar scripts/verify-e11-formal.sh
#
# Exit status: 0 iff every model produced its expected verdict.
set -euo pipefail

JAR="${TLA2TOOLS_JAR:-/tmp/tla2tools.jar}"
if [ ! -f "$JAR" ]; then
  echo "error: $JAR not found." >&2
  echo "  fetch: curl -sSL -o /tmp/tla2tools.jar \\)" >&2
  echo "    'https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar'" >&2
  exit 2
fi

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
spec="$here/../docs/spec/e11_timer_wait"
cd "$spec"

# TLC writes *_TTrace* trace-exploration files on counterexamples; clean them.
cleanup() { find . -maxdepth 1 -name '*TTrace*' -delete; }
trap cleanup EXIT

run() {  # run MODEL CFG  -> prints the tail; returns TLC's exit status
  local model="$1" cfg="$2"
  java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -nowarning \
       -config "$cfg" "$model" 2>&1
}

expect_pass() {  # expect_pass MODEL CFG
  local model="$1" cfg="$2" out
  out="$(run "$model" "$cfg")" || { echo "FAIL: $model: TLC errored"; return 1; }
  if printf '%s' "$out" | grep -q 'Model checking completed. No error has been found'; then
    printf 'PASS  %s\n' "$model"
  else
    printf 'FAIL  %s (expected PASS, got error/violation)\n' "$model"
    printf '%s\n' "$out" | tail -20
    return 1
  fi
}

expect_counterexample() {  # expect_counterexample MODEL CFG
  local model="$1" cfg="$2" out
  out="$(run "$model" "$cfg")" || true   # TLC exits nonzero on a counterexample
  if printf '%s' "$out" | grep -Eq 'Invariant|Property.*violated|Temporal'; then
    if ! printf '%s' "$out" | grep -q 'Model checking completed. No error has been found'; then
      printf 'CEX   %s (counterexample observed, as expected)\n' "$model"
      return 0
    fi
  fi
  printf 'FAIL  %s (expected a counterexample, none found)\n' "$model"
  printf '%s\n' "$out" | tail -8
  return 1
}

echo "=== E11 formal gate (TLC2, jar=$JAR) ==="
rc=0
expect_pass           "E11TimerWait"                           "E11TimerWait.cfg"                           || rc=1
expect_pass           "E11TimerWait"                           "E11TimerWaitLiveness.cfg"                   || rc=1
expect_counterexample "E11TimerWaitNeg1DoublePublication"      "E11TimerWaitNeg1DoublePublication.cfg"      || rc=1
expect_counterexample "E11TimerWaitNeg2TimerCancelDoublePublication" "E11TimerWaitNeg2TimerCancelDoublePublication.cfg" || rc=1
expect_counterexample "E11TimerWaitNeg3StaleCrossEpoch"        "E11TimerWaitNeg3StaleCrossEpoch.cfg"        || rc=1
expect_counterexample "E11TimerWaitNeg4CallbackAfterRetirement" "E11TimerWaitNeg4CallbackAfterRetirement.cfg" || rc=1
expect_counterexample "E11TimerWaitNeg5DeadlineLostParked"     "E11TimerWaitNeg5DeadlineLostParked.cfg"     || rc=1

echo "=== gate ${rc}-ed (0 = all expected verdicts observed) ==="
exit "$rc"
