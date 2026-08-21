#!/usr/bin/env bash
# verify-e12-sched-liveness.sh — E12 x Scheduler combined liveness formal gate
# (issue #161: async_rwlock_test T22 multi-worker drain hang).
#
#   Positive (repaired protocol, RepairContributionGeneration=TRUE):
#     E12SchedLivenessSafety    -> all invariants PASS (incl. DrainStuckState)
#     E12SchedLivenessLiveness  -> all temporal properties PASS
#     B4NoBumpRecheckErase      -> PASS  (:958 self-guarded by :582)
#     B4NoBumpPubErase          -> PASS  (:1452 self-guarded by the
#                                          G-atomic erase+signal pair)
#     B4NoBumpDanceResetErase   -> PASS  (:1065 eraser stays active/re-loops)
#     M1/M2/M3 composition       -> PASS (documented closure: in the closed
#                          T22 scenario every publisher is worker-executed,
#                          so steal + the publisher's own loop-top drain the
#                          ticket; the M1/M2 hazard classes need a non-worker
#                          producer or an unbounded busy owner — carried by
#                          the e9 suite's negative models at its abstraction.
#                          These gates prove the toggles compose on the
#                          repaired base without re-breaking convergence.)
#
#   Negative (expected counterexamples):
#     M4 (as-built, Repair=FALSE) Safety   -> DrainStuckState violated; the
#                          counterexample must realize  PopTicket ->
#                          EraseIdle -> peer DanceContribute(not-last) ->
#                          stale contributor ParkCommit -> ArmBaseline ->
#                          both Parked  with all work complete.
#     M4 Liveness                          -> temporal violations
#     M5 (grant without runnable ticket)   -> temporal violation
#     B4NoBumpPopErase                     -> DrainStuckState violated
#                          (scheduler.cpp:550 is a genuine invalidation site)
#     B4NoBumpMwS1Erase                    -> DrainStuckState violated
#                          (scheduler.cpp:582 is a genuine invalidation site)
#
# Source-safe: TLC runs in an isolated mktemp workspace.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/e12_rwlock_scheduler_liveness"
workers="${TLC_WORKERS:-2}"

# Resolve jar via shared helper (sets TLA2TOOLS_JAR). Does NOT fall back to
# the repo-root jar, which is not checksum-verified; use bootstrap.py instead.
source "$here/resolve-jar.sh"
jar="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

outroot="$(mktemp -d -t e12-sl.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/e12-sl.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/e12-sl.* ]]; then
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

expect_pass() {
  local label="$1" cfg="$2" tag="$3"
  local out="$outroot/$tag.out"
  run_tlc E12SchedLiveness "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "PASS  $label"; return 0; fi
  echo "FAIL  $label (expected PASS)"; tail -20 "$out"; return 1
}

expect_cex() {
  local label="$1" cfg="$2" tag="$3"
  local out="$outroot/$tag.out"
  run_tlc E12SchedLiveness "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "FAIL  $label (expected counterexample, PASSED)"; return 1; fi
  echo "CEX   $label"; return 0
}

expect_fail_invariant() {
  local label="$1" cfg="$2" expected="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc E12SchedLiveness "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "FAIL  $label (expected $expected, PASSED)"; return 1; fi
  if ! grep -Eq "Invariant $expected is violated" "$out"; then
    echo "FAIL  $label (expected $expected NOT the violation)"; tail -8 "$out"; return 1
  fi
  echo "CEX   $label ($expected violated)"; return 0
}

rc=0
echo "=== E12 Scheduler-liveness formal gate (issue #161; workers=$workers) ==="
# --- Positive gates (repaired protocol) ---
expect_pass "E12SchedLiveness [safety, repaired]" \
  E12SchedLivenessSafety safety || rc=1
expect_pass "E12SchedLiveness [liveness, repaired]" \
  E12SchedLivenessLiveness liveness || rc=1
# --- B4 site-classification experiments (repaired base, one bump off) ---
expect_pass "B4: :958 recheck erase self-guarded (no bump needed)" \
  E12SchedLivenessB4NoBumpRecheckErase b4_recheck || rc=1
expect_pass "B4: :1452 pub erase self-guarded (no bump needed)" \
  E12SchedLivenessB4NoBumpPubErase b4_pub || rc=1
expect_pass "B4: :1065 dance reset self-guarded (no bump needed)" \
  E12SchedLivenessB4NoBumpDanceResetErase b4_dance || rc=1
# --- M1/M2/M3 composition on the repaired base (documented closure) ---
expect_pass "M1 composition [pub-without-signal; closed in-scope]" \
  E12SchedLivenessM1 m1 || rc=1
expect_pass "M2 composition [no-transport; closed in-scope]" \
  E12SchedLivenessM2 m2 || rc=1
expect_pass "M3 composition [no-commit-recheck; closed in-scope]" \
  E12SchedLivenessM3 m3 || rc=1
# --- Negative gates (expected counterexamples) ---
expect_fail_invariant "M4 as-built [safety]" \
  E12SchedLivenessM4 DrainStuckState m4_safety || rc=1
expect_cex "M4 as-built [liveness]" \
  E12SchedLivenessM4Liveness m4_liveness || rc=1
expect_cex "M5 grant-without-ticket [liveness]" \
  E12SchedLivenessM5 m5 || rc=1
expect_fail_invariant "B4: :550 pop erase IS an invalidation site" \
  E12SchedLivenessB4NoBumpPopErase DrainStuckState b4_pop || rc=1
expect_fail_invariant "B4: :582 mw_s1 erase IS an invalidation site" \
  E12SchedLivenessB4NoBumpMwS1Erase DrainStuckState b4_mws1 || rc=1

echo
if [[ "$rc" -eq 0 ]]; then echo "=== PASS ==="; else echo "=== FAIL ==="; fi
exit "$rc"
