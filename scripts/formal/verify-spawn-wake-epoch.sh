#!/usr/bin/env bash
# verify-spawn-wake-epoch.sh - spawn-to-busy-worker wake-epoch formal gate
# (MODEL-007d, issue #176, umbrella #171; historical defect #115).
#
#   SpawnWakeEpoch (positive)            -> all 4 as-built laws PASS
#   NegNoSignal                          -> InvWakeObligation CEX (#115 pre-fix)
#   NegNoSignalSpecificity               -> unrelated laws PASS
#   NegNoRecheck                         -> InvWakeObligation CEX (pre-G1)
#   NegNoRecheckSpecificity              -> unrelated laws PASS
#   Reach{Parked,PublishedWhileParked,EpochAdvanced,Rescued,RescuedAfterWake,ParkRefuse}
#                                        -> NoReach* CEX = witness
#
# Source-safe: TLC runs in an isolated mktemp workspace. Fail-closed on
# no-launch, unexpected pass, wrong named invariant, and deadlock.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/spawn_wake_epoch"
workers="${TLC_WORKERS:-1}"

source "$here/resolve-jar.sh"
jar="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

outroot="$(mktemp -d -t spwke.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/spwke.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/spwke.* ]]; then
    rm -rf -- "$outroot"
  fi
}
trap cleanup EXIT

workdir="$outroot/work"
mkdir -p "$workdir"
cp "$spec"/*.tla "$spec"/*.cfg "$workdir/"
cd "$workdir"

run_tlc() {
  local model="$1" cfg="$2" tag="$3"
  local metadir="$outroot/$tag.meta"
  mkdir -p "$metadir"
  java -XX:+UseParallelGC -cp "$jar" tlc2.TLC -nowarning \
       -workers "$workers" -metadir "$metadir" -config "$cfg" "$model" \
       >"$outroot/$tag.out" 2>&1 || true
}
launched() { grep -q '^Starting\.\.\.' "$1"; }
passed()   { grep -q 'Model checking completed. No error has been found' "$1"; }
deadlocked() { grep -q 'Deadlock reached' "$1"; }

expect_pass() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if deadlocked "$out"; then echo "FAIL  $label (deadlock)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "PASS  $label"; return 0; fi
  echo "FAIL  $label (expected PASS)"; tail -20 "$out"; return 1
}

named_violation() { grep -Eq "Invariant $2 is violated" "$1"; }

expect_fail() {
  local label="$1" model="$2" cfg="$3" expected="$4" tag="$5"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if deadlocked "$out"; then echo "FAIL  $label (deadlock, not the named CEX)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "FAIL  $label (expected $expected violation, model passed)"; return 1; fi
  if ! named_violation "$out" "$expected"; then
    echo "FAIL  $label (expected $expected, got another failure)"
    tail -12 "$out"
    return 1
  fi
  echo "CEX   $label ($expected violated, as expected)"
}

rc=0
echo "=== Spawn wake-epoch formal gate (workers=$workers) ==="
expect_pass "SpawnWakeEpoch" SpawnWakeEpoch SpawnWakeEpoch.cfg positive || rc=1

expect_fail "NegNoSignal (#115 pre-fix)" SpawnWakeEpoch \
  SpawnWakeEpochNegNoSignal.cfg InvWakeObligation neg1 || rc=1
expect_pass "NegNoSignalSpecificity" SpawnWakeEpoch \
  SpawnWakeEpochNegNoSignalSpecificity.cfg neg1spec || rc=1

expect_fail "NegNoRecheck (pre-G1)" SpawnWakeEpoch \
  SpawnWakeEpochNegNoRecheck.cfg InvWakeObligation neg4 || rc=1
expect_pass "NegNoRecheckSpecificity" SpawnWakeEpoch \
  SpawnWakeEpochNegNoRecheckSpecificity.cfg neg4spec || rc=1

expect_fail "ReachParked" SpawnWakeEpoch \
  SpawnWakeEpochReachParked.cfg NoReachParked r1 || rc=1
expect_fail "ReachPublishedWhileParked" SpawnWakeEpoch \
  SpawnWakeEpochReachPublishedWhileParked.cfg NoReachPublishedWhileParked r2 || rc=1
expect_fail "ReachEpochAdvanced" SpawnWakeEpoch \
  SpawnWakeEpochReachEpochAdvanced.cfg NoReachEpochAdvanced r3 || rc=1
expect_fail "ReachRescued" SpawnWakeEpoch \
  SpawnWakeEpochReachRescued.cfg NoReachRescued r4 || rc=1
expect_fail "ReachRescuedAfterWake" SpawnWakeEpoch \
  SpawnWakeEpochReachRescuedAfterWake.cfg NoReachRescuedAfterWake r6 || rc=1
expect_fail "ReachParkRefuse" SpawnWakeEpoch \
  SpawnWakeEpochReachParkRefuse.cfg NoReachParkRefuse r5 || rc=1

echo
if [[ "$rc" -eq 0 ]]; then echo "=== PASS ==="; else echo "=== FAIL ==="; fi
exit "$rc"
