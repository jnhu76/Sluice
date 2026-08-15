#!/usr/bin/env bash
# verify-e9-park-wake.sh — E9 park/wake protocol formal gate.
#
#   E9ParkWake (safety, split-wait)     -> all invariants PASS
#   E9ParkWake (liveness, split-wait)   -> liveness PASS
#   E9ParkWake (reference safety)       -> non-split E9 rule PASS (DIV-05)
#   BuggyDrainParks                     -> counterexample (liveness property violated)
#   BuggyMixedSource                    -> Inv7StateForm violated
#   BuggyPrePark                        -> Inv2NoLostWake / Inv4ExternalReadyWakes violated
#   BuggyNoBridge (Phase G)             -> Inv8BridgeReachesBackendPark violated
#                                         (model-level M1: bridge disabled)
#
# Source-safe: TLC runs in an isolated mktemp workspace.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/e9_park_wake"
workers="${TLC_WORKERS:-1}"

# Resolve jar via shared helper (sets TLA2TOOLS_JAR). Does NOT fall back to
# the repo-root jar, which is not checksum-verified; use bootstrap.py instead.
source "$here/resolve-jar.sh"
jar="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

outroot="$(mktemp -d -t e9-pw.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/e9-pw.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/e9-pw.* ]]; then
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
named_violation() { grep -Eq "Invariant $2 is violated" "$1"; }

expect_pass() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "PASS  $label"; return 0; fi
  echo "FAIL  $label (expected PASS)"; tail -20 "$out"; return 1
}

expect_fail() {
  local label="$1" model="$2" cfg="$3" expected="$4" tag="$5"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "FAIL  $label (expected $expected, PASSED)"; return 1; fi
  if ! named_violation "$out" "$expected"; then
    echo "FAIL  $label (expected $expected NOT the violation)"; tail -8 "$out"; return 1
  fi
  echo "CEX   $label ($expected violated)"; return 0
}

# BuggyDrainParks uses a liveness PROPERTY, not an INVARIANT.
expect_cex() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "FAIL  $label (expected counterexample, PASSED)"; return 1; fi
  echo "CEX   $label (counterexample found)"; return 0
}

rc=0
echo "=== E9 Park/Wake formal gate (workers=$workers) ==="
expect_pass "E9ParkWake [safety, split-wait]" E9ParkWake E9ParkWake.cfg safety || rc=1
expect_pass "E9ParkWake [liveness, split-wait]" E9ParkWake E9ParkWakeLiveness.cfg liveness || rc=1
expect_pass "E9ParkWake [safety, reference]" E9ParkWake E9ParkWakeReference.cfg reference || rc=1
expect_pass "E9ParkWake [liveness, reference]" E9ParkWake E9ParkWakeReferenceLiveness.cfg ref_liveness || rc=1
expect_cex "BuggyDrainParks" E9ParkWakeBuggyDrainParks \
  E9ParkWakeBuggyDrainParks.cfg buggy_drain || rc=1
expect_fail "BuggyMixedSource" E9ParkWakeBuggyMixedSource \
  E9ParkWakeBuggyMixedSource.cfg Inv7StateForm buggy_mix || rc=1
expect_fail "BuggyPrePark" E9ParkWakeBuggyPrePark \
  E9ParkWakeBuggyPrePark.cfg Inv2NoLostWake buggy_prepark || rc=1
expect_fail "BuggyNoBridge" E9ParkWakeBuggyNoBridge \
  E9ParkWakeBuggyNoBridge.cfg Inv8BridgeReachesBackendPark buggy_nobridge || rc=1

echo
if [[ "$rc" -eq 0 ]]; then echo "=== PASS ==="; else echo "=== FAIL ==="; fi
exit "$rc"
