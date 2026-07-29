#!/usr/bin/env bash
# verify-e16-application-runtime.sh — E16 Application Runtime formal gate.
#
#   E16ApplicationRuntime (safety)          -> all invariants PASS
#   E16ApplicationRuntime (liveness)        -> liveness PASS
#   E16ApplicationRuntime (wide)            -> wide-domain safety PASS
#   BuggyBoundaryWake                       -> NoStrandedSuccessfulAdmission violated
#   BuggyStopClose                          -> NoCancelAfterGroupDestroy violated
#   BuggyStartupAbort                       -> StartupAbortNeverRuns violated
#   BuggyCloseOwner                         -> AtMostOneCloseOwner violated
#   R1-R16 reachability witnesses           -> NotReach_* violated (reachable)
#   Wrong-property control                  -> unrelated invariant NOT violated
#
# Source-safe: TLC runs in an isolated mktemp workspace.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/e16_application_runtime"
workers="${TLC_WORKERS:-1}"

# Resolve jar via shared helper (sets TLA2TOOLS_JAR). Does NOT fall back to
# the repo-root jar, which is not checksum-verified; use bootstrap.py instead.
source "$here/resolve-jar.sh"
jar="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

outroot="$(mktemp -d -t sluice-formal.e16-app-runtime.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] && [[ "$outroot" == *sluice-formal.e16-app-runtime.* ]]; then
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
launched() { grep -q '^Starting\.\.\.' "$1" || grep -q 'TLC2 Version' "$1"; }
passed()   { grep -q 'Model checking completed. No error has been found' "$1"; }
named_violation() { grep -Eq "Invariant $2 is violated" "$1"; }
temporal_violation() { grep -Eq "Temporal propert(y|ies).* (was|were) violated" "$1"; }
state_count() { grep -oP '\d+ states generated' "$1" | head -1 || echo "states: ?"; }

expect_pass() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "PASS  $label ($(state_count "$out"))"; return 0; fi
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

expect_temporal_cex() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "FAIL  $label (expected counterexample, PASSED)"; return 1; fi
  if ! temporal_violation "$out"; then
    echo "FAIL  $label (expected temporal violation, got other error)"; tail -8 "$out"; return 1
  fi
  echo "CEX   $label (temporal property violated)"; return 0
}

expect_reach() {
  local label="$1" model="$2" cfg="$3" expected="$4" tag="$5"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "FAIL  $label (expected $expected reachable, PASSED)"; return 1; fi
  if ! named_violation "$out" "$expected"; then
    echo "FAIL  $label (expected $expected NOT the violation)"; tail -8 "$out"; return 1
  fi
  echo "REACH $label ($expected violated = state reachable)"; return 0
}

rc=0
echo "=== E16 Application Runtime formal gate (workers=$workers) ==="
echo

# --- Correct safety ---
echo "--- Correct safety ---"
expect_pass "E16 safety" E16ApplicationRuntime E16ApplicationRuntime.cfg safety || rc=1

# --- Correct liveness ---
echo "--- Correct liveness ---"
expect_pass "E16 liveness" E16ApplicationRuntime E16ApplicationRuntimeLiveness.cfg liveness || rc=1

# --- Wide-domain safety ---
echo "--- Wide-domain safety ---"
expect_pass "E16 wide" E16ApplicationRuntime E16ApplicationRuntimeWide.cfg wide || rc=1

# --- Negative models ---
echo "--- Negative models ---"
expect_temporal_cex "NEG-E16-1 BoundaryWake" \
  E16ApplicationRuntimeBuggyBoundaryWake \
  E16ApplicationRuntimeBuggyBoundaryWake.cfg neg1 || rc=1

expect_fail "NEG-E16-2 StopClose" \
  E16ApplicationRuntimeBuggyStopClose \
  E16ApplicationRuntimeBuggyStopClose.cfg NoCancelAfterGroupDestroy neg2 || rc=1

expect_fail "NEG-E16-3 StartupAbort" \
  E16ApplicationRuntimeBuggyStartupAbort \
  E16ApplicationRuntimeBuggyStartupAbort.cfg StartupAbortNeverRuns neg3 || rc=1

expect_fail "NEG-E16-4 CloseOwner" \
  E16ApplicationRuntimeBuggyCloseOwner \
  E16ApplicationRuntimeBuggyCloseOwner.cfg AtMostOneCloseOwner neg4 || rc=1

expect_fail "NEG-E16-5 EarlyCloseBeforeDrain" \
  E16ApplicationRuntimeBuggyCloseOwnerBeforeDrain \
  E16ApplicationRuntimeBuggyCloseOwnerBeforeDrain.cfg Inv25StoppedAfterDrain neg5 || rc=1

# --- Reachability scenes (R2,R3,R11-R15,R17,R18) ---
echo "--- Reachability scenes ---"
for i in 2 3 11 12 13 14 15 18; do
  # Generate a per-scene cfg with only the target NotReach invariant
  scene_cfg="$workdir/E16ApplicationRuntime.reach_r$i.cfg"
  cat > "$scene_cfg" <<EOF
SPECIFICATION Spec
INVARIANTS
    NotReach_R$i
CONSTANTS
    Tasks = {T0, T1}
    Callers = {C0, C1}
    MaxIO = 2
    E0 = E0
    E1 = E1
    NONE = NONE
    T0 = T0
    T1 = T1
    C0 = C0
    C1 = C1
EOF
  expect_reach "R$i" E16ApplicationRuntime "E16ApplicationRuntime.reach_r$i.cfg" "NotReach_R$i" "reach_r$i" || rc=1
done

# --- Wrong-property control ---
echo "--- Wrong-property control ---"
# NEG-E16-3 (StartupAbort) should NOT violate NoCancelAfterGroupDestroy
# (that invariant is unrelated to startup abort)
out="$outroot/wrong_prop.out"
run_tlc E16ApplicationRuntimeBuggyStartupAbort \
  E16ApplicationRuntimeBuggyStartupAbort.cfg wrong_prop
if ! launched "$out"; then
  echo "FAIL  wrong-property control (no launch)"; tail -20 "$out"
  rc=1
elif passed "$out"; then
  echo "FAIL  wrong-property (expected StartupAbortNeverRuns violation, got PASS)"
  rc=1
elif named_violation "$out" NoCancelAfterGroupDestroy; then
  echo "FAIL  wrong-property control (unrelated invariant NoCancelAfterGroupDestroy violated)"
  rc=1
elif named_violation "$out" StartupAbortNeverRuns; then
  echo "PASS  wrong-property control (StartupAbort violates only its own invariant)"
else
  echo "FAIL  wrong-property control (unexpected outcome)"; tail -20 "$out"
  rc=1
fi

echo
if [[ "$rc" -eq 0 ]]; then echo "=== PASS ==="; else echo "=== FAIL ==="; fi
exit "$rc"
