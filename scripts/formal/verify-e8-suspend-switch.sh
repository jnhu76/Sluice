#!/usr/bin/env bash
# verify-e8-suspend-switch.sh — E8 suspend-switch steal-exclusion formal gate
# (MODEL-007a / I47-F2; issue #172).
#
#   E8SuspendSwitch (positive, as-built constants)        -> all 4 invariants PASS
#   NegIgnorePendingSteal        (H1) -> InvNoResumeBeforeContextSaved violated
#   NegIgnorePendingStealSpec    (H1) -> remaining 3 laws PASS (specificity)
#   NegRaiseTooLate              (H2) -> InvUnsavedSuspensionProtected violated
#   NegRaiseTooLateChain         (H2) -> InvNoResumeBeforeContextSaved violated
#   NegRaiseTooLateSpec          (H2) -> ticket/pending-binding laws PASS
#   NegClearTooEarly             (H3) -> InvNoResumeBeforeContextSaved violated
#   NegClearTooEarlySpec         (H3) -> ticket/pending-binding laws PASS
#   ReachWakeBeforeSave          (R1) -> NoReachWakeBeforeSave violated
#   ReachStealRefusal            (R2) -> NoReachStealRefusal violated
#   ReachSafeMigration           (R3) -> NoReachSafePostSaveMigration violated
#
# Every expected failure must be the EXACT named invariant — not "some
# invariant", not a deadlock, not a parse error.
#
# Source-safe: TLC runs in an isolated mktemp workspace.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/e8_suspend_switch"
workers="${TLC_WORKERS:-1}"

# Resolve jar via shared helper (sets TLA2TOOLS_JAR). Does NOT fall back to
# the repo-root jar, which is not checksum-verified; use bootstrap.py instead.
source "$here/resolve-jar.sh"
jar="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

outroot="$(mktemp -d -t e8-ss.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/e8-ss.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/e8-ss.* ]]; then
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
  if passed "$out"; then echo "FAIL  $label (expected $expected violation, model passed)"; return 1; fi
  if deadlocked "$out"; then echo "FAIL  $label (deadlock, not $expected)"; tail -12 "$out"; return 1; fi
  if ! named_violation "$out" "$expected"; then
    echo "FAIL  $label (expected $expected, got another failure)"
    tail -12 "$out"
    return 1
  fi
  echo "CEX   $label ($expected violated, as expected)"
}

M=E8SuspendSwitch
rc=0
echo "=== E8 Suspend-Switch formal gate (workers=$workers) ==="
expect_pass "positive (as-built)" "$M" E8SuspendSwitch.cfg pos || rc=1

expect_fail "NEG-SS1 ignore-pending steal" "$M" \
  E8SuspendSwitchNegIgnorePendingSteal.cfg InvNoResumeBeforeContextSaved neg1 || rc=1
expect_pass "NEG-SS1 specificity" "$M" \
  E8SuspendSwitchNegIgnorePendingStealSpecificity.cfg neg1spec || rc=1

expect_fail "NEG-SS2 raise-too-late (authority)" "$M" \
  E8SuspendSwitchNegRaiseTooLate.cfg InvUnsavedSuspensionProtected neg2 || rc=1
expect_fail "NEG-SS2 chain (unsafe resume)" "$M" \
  E8SuspendSwitchNegRaiseTooLateChain.cfg InvNoResumeBeforeContextSaved neg2chain || rc=1
expect_pass "NEG-SS2 specificity" "$M" \
  E8SuspendSwitchNegRaiseTooLateSpecificity.cfg neg2spec || rc=1

expect_fail "NEG-SS3 clear-too-early" "$M" \
  E8SuspendSwitchNegClearTooEarly.cfg InvNoResumeBeforeContextSaved neg3 || rc=1
expect_pass "NEG-SS3 specificity" "$M" \
  E8SuspendSwitchNegClearTooEarlySpecificity.cfg neg3spec || rc=1

expect_fail "R1 reach wake-before-save" "$M" \
  E8SuspendSwitchReachWakeBeforeSave.cfg NoReachWakeBeforeSave reach1 || rc=1
expect_fail "R2 reach steal refusal" "$M" \
  E8SuspendSwitchReachStealRefusal.cfg NoReachStealRefusal reach2 || rc=1
expect_fail "R3 reach safe migration" "$M" \
  E8SuspendSwitchReachSafeMigration.cfg NoReachSafePostSaveMigration reach3 || rc=1

echo
if [[ "$rc" -eq 0 ]]; then echo "=== PASS ==="; else echo "=== FAIL ==="; fi
exit "$rc"
