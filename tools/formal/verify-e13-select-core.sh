#!/usr/bin/env bash
# Reproducible PR #17 gate for the layered E13 Select formal core.
# Positive model + refinement + causal reachability only; no negative/liveness.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/docs/spec/e13_select"
jar="${TLA2TOOLS_JAR:-$repo/tla2tools.jar}"
workers="${TLC_WORKERS:-1}"

if [[ ! -f "$jar" ]]; then
  echo "error: tla2tools.jar not found at $jar" >&2
  echo "  set TLA2TOOLS_JAR=/path/to/tla2tools.jar" >&2
  exit 2
fi
if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2
  exit 2
fi

outroot="$(mktemp -d -t e13-select-core.XXXXXX)"
workdir="$outroot/work"
cleanup() {
  # PR #18 corrective-1 (§I3) portable tmpdir hardening: combine the
  # non-empty check, the prefix check, AND the rm into a single guarded
  # command so `set -e` cannot short-circuit between the check and the rm.
  # The prefix check accepts both /tmp and ${TMPDIR} parents, and the
  # mktemp template guarantees the e13-select-core.* basename prefix.
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/e13-select-core.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/e13-select-core.* ]]; then
    rm -rf -- "$outroot"
  fi
}
trap cleanup EXIT

mkdir -p "$workdir"
cp "$spec"/*.tla "$workdir/"
cp "$spec"/*.cfg "$workdir/"

cd "$workdir"
pwd -P >/dev/null

run_tlc() {
  local model="$1" cfg="$2" tag="$3"
  local metadir="$outroot/$tag.meta"
  mkdir -p "$metadir"
  java -XX:+UseParallelGC -cp "$jar" tlc2.TLC -nowarning \
       -workers "$workers" -metadir "$metadir" -config "$cfg" "$model" \
       >"$outroot/$tag.out" 2>&1 || true
}

launched() { grep -q '^Starting\.\.\.' "$1"; }
passed() { grep -q 'Model checking completed. No error has been found' "$1"; }
deadlocked() { grep -qiE 'Deadlock reached|is deadlocked' "$1"; }
named_violation() { grep -Eq "Invariant $2 is violated" "$1"; }

metrics() {
  local file="$1"
  local states depth runtime
  states="$(grep 'states generated' "$file" | tail -1 | sed 's/^[[:space:]]*//' || true)"
  depth="$(grep 'The depth of the complete state graph search is' "$file" | tail -1 | sed 's/^[[:space:]]*//' || true)"
  runtime="$(grep '^Finished in' "$file" | tail -1 | sed 's/ at .*//' || true)"
  printf '%s; %s; %s' "$states" "$depth" "$runtime"
}

expect_pass() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then
    echo "FAIL  $label (TLC did not launch / parse / configure)"
    tail -20 "$out"
    return 1
  fi
  if deadlocked "$out"; then
    echo "FAIL  $label (deadlock)"
    tail -20 "$out"
    return 1
  fi
  if ! passed "$out"; then
    echo "FAIL  $label (expected PASS)"
    tail -20 "$out"
    return 1
  fi
  echo "PASS  $label  ($(metrics "$out"))"
}

expect_reach() {
  local label="$1" property="$2" cfg="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc E13Select "$cfg" "$tag"
  if ! launched "$out"; then
    echo "FAIL  $label (TLC did not launch / parse / configure)"
    tail -20 "$out"
    return 1
  fi
  if deadlocked "$out"; then
    echo "FAIL  $label (deadlock instead of causal witness)"
    tail -20 "$out"
    return 1
  fi
  if passed "$out"; then
    echo "FAIL  $label ($property was not reachable)"
    return 1
  fi
  if ! named_violation "$out" "$property"; then
    echo "FAIL  $label (wrong property violated; expected $property)"
    grep -m1 -E 'Invariant .+ is violated|Temporal property .+ violated' "$out" || true
    return 1
  fi
  echo "REACH $label  ($property violated as expected; $(metrics "$out"))"
}

expect_layer_reach() {
  local label="$1" model="$2" property="$3" cfg="$4" tag="$5"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out" || deadlocked "$out" || passed "$out" \
     || ! named_violation "$out" "$property"; then
    echo "FAIL  $label (expected named reachability violation: $property)"
    tail -20 "$out"
    return 1
  fi
  echo "REACH $label  ($property violated as expected; $(metrics "$out"))"
}

rc=0
echo "=== E13 Select layered formal core (PR #17; workers=$workers) ==="

expect_pass "Contract semantics" E13SelectContract E13SelectContract.cfg contract || rc=1
expect_pass "Contract registration-rollback regression" E13SelectContract \
  E13SelectContract.rollback_regression.cfg contract_rollback_regression || rc=1
expect_pass "Central Claim + Contract refinement" \
  E13SelectCentralClaim E13SelectCentralClaim.cfg central || rc=1
expect_pass "Event/Timer adapters + Central Claim refinement" \
  E13SelectEventTimer E13SelectEventTimer.cfg adapters || rc=1
expect_pass "4-arm bounded mixed root" E13Select E13Select.cfg root4 || rc=1

expect_layer_reach "Contract inline completion" E13SelectContract \
  NotReachContractInline E13SelectContract.reach_inline.cfg contract_inline || rc=1
expect_layer_reach "Contract reversible-reservation suspended completion" \
  E13SelectContract NotReachContractReservationSuspended \
  E13SelectContract.reach_suspended.cfg contract_suspended || rc=1
expect_layer_reach "Central Claim snapshot tie-break" E13SelectCentralClaim \
  NotReachCentralTieBreak E13SelectCentralClaim.reach.cfg central_reach || rc=1

expect_reach "R1 Event admission-ready inline" NotReach_R1 \
  E13Select.scene_inline.cfg r1 || rc=1
expect_reach "R2 Timer admission-ready inline" NotReach_R2 \
  E13Select.scene_inline_timer.cfg r2 || rc=1
expect_reach "R3 Event post-suspension" NotReach_R3 \
  E13Select.scene_suspended.cfg r3 || rc=1
expect_reach "R4 Timer post-suspension" NotReach_R4 \
  E13Select.scene_suspended_timer.cfg r4 || rc=1
expect_reach "R5 Event winner + Timer loser" NotReach_R5 \
  E13Select.scene_2mix.cfg r5 || rc=1
expect_reach "R6 Timer winner + Event loser" NotReach_R6 \
  E13Select.scene_2mix_timer_winner.cfg r6 || rc=1
expect_reach "3-arm mixed Event winner + Timer losers" NotReach_R5 \
  E13Select.scene_3mix.cfg r5_3mix || rc=1
expect_reach "R7 claim snapshot tie-break" NotReach_R7 \
  E13Select.scene_4mix.cfg r7 || rc=1
expect_reach "R8 same-Event multi-arm broadcast" NotReach_R8 \
  E13Select.scene_sameevent.cfg r8 || rc=1
expect_reach "R9 partial-registration rollback" NotReach_R9 \
  E13Select.scene_rollback.cfg r9 || rc=1
expect_reach "R10 stale TimerRegistration skip" NotReach_R10 \
  E13Select.scene_staletimer.cfg r10 || rc=1
expect_reach "R11 inline result consumption" NotReach_R11 \
  E13Select.scene_consume_inline.cfg r11 || rc=1
expect_reach "R12 suspended result consumption" NotReach_R12 \
  E13Select.scene_consume_suspended.cfg r12 || rc=1

echo
grep -m1 '^TLC2 Version' "$outroot/contract.out" || true
if [[ "$rc" -eq 0 ]]; then
  echo "=== PASS: contract + refinements + R1-R12 causal witnesses ==="
else
  echo "=== FAIL: one or more E13 formal gates failed ==="
fi
exit "$rc"
