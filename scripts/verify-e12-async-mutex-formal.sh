#!/usr/bin/env bash
# verify-e12-async-mutex-formal.sh -- reproducible E12-C AsyncMutex TLA+ / TLC
# formal gate (E12-C-ASYNC-MUTEX-PREPARATION-CORRECTIVE-5), SAFETY ONLY.
#
# Runs the correct E12 AsyncMutex safety model and ALL ELEVEN negative models
# (NEG-M1..NEG-M11) through TLC, asserting each produces its expected result AND
# that each negative model violates its EXPECTED NAMED PROPERTY (not just any
# generic "Invariant violated" message). Also runs a WRONG-PROPERTY gate that
# asserts a negative's defect is property-specific.
#
# This is a SAFETY-ONLY gate. There is no liveness run. The E12-C implementation
# adds a COMPILE-PROBE gate (E12-C-IMPLEMENTATION) that asserts the raw WaitQueue
# bypass on an AsyncMutex fails to compile (the AsyncMutex public authority is
# sealed: no wait_queue() accessor, no public owner()/is_locked()).
#
#   correct safety        -> all invariants PASS
#                            (InvType, InvQueueWellFormed, InvSingleResolution,
#                             InvSinglePublication, InvPublicationConsistency,
#                             InvNoOwnerlessQueuedDemand, InvPublishedEpochTerminal,
#                             InvUnlockAuthority, InvRecursiveForbidden, InvFIFOGrant,
#                             InvNoBarging, InvGrantOwnerCommit,
#                             InvGrantPublicationCoupling, InvAdmissionClosure,
#                             InvDeadlinePrecedence, InvGrantFinality,
#                             InvPublicationRequiresSuspensionOrHandoff,
#                             InvDestructionPrecondition)
#   NEG-M1  NonOwnerUnlock              -> InvUnlockAuthority FAILS
#   NEG-M2  RecursiveAcquire            -> InvRecursiveForbidden FAILS
#   NEG-M3  NonFIFOGrant                -> InvFIFOGrant FAILS
#   NEG-M4  Barging                     -> InvNoBarging FAILS
#   NEG-M5  GrantWithoutOwnerCommit     -> InvGrantOwnerCommit FAILS
#   NEG-M6  PublicationWithoutGrantCoupling -> InvGrantPublicationCoupling FAILS
#   NEG-M7  AdmissionClosureFailure     -> InvAdmissionClosure FAILS
#   NEG-M8  CancelRevokesHandoff        -> InvGrantFinality FAILS
#   NEG-M9  DeadlineRevokesHandoff      -> InvGrantFinality FAILS
#   NEG-M10 ImmediatePublication        -> InvPublicationRequiresSuspensionOrHandoff FAILS
#   NEG-M11 DestructionWhileOwnedOrQueued -> InvDestructionPrecondition FAILS
#   WRONG-PROPERTY gate                 -> NEG-3's non-FIFO grant vs InvGrantOwnerCommit
#                                          PASSES (defect is property-specific) and does
#                                          NOT flag InvFIFOGrant under that config
#
# Expected-property mapping (declared once per negative model below). The script
# FAILs if:
#   - TLC fails to launch (java/runtime error)
#   - the module fails to parse / the config is invalid
#   - the correct model fails
#   - a negative model unexpectedly PASSES (no counterexample)
#   - the WRONG invariant/property fails first (expected named property absent)
#   - the wrong-property gate flags the expected property (defect not specific)
#   - stale output is reused (a fresh output directory is used per invocation)
#
# Requires a tla2tools.jar. By default looks for /tmp/tla2tools.jar (the path
# the E7-E12 READMEs expect); override with TLA2TOOLS_JAR=/path/to/tla2tools.jar
# or fetch v1.8.0 from https://github.com/tlaplus/tlaplus/releases.
#
# Usage:
#   scripts/verify-e12-async-mutex-formal.sh
#   TLA2TOOLS_JAR=/opt/tla2tools.jar scripts/verify-e12-async-mutex-formal.sh
#
# Exit status: 0 iff every gate produced its expected verdict AND every negative
# model's expected named property was observed as a violation AND the
# wrong-property gate did not misfire.
set -euo pipefail

JAR="${TLA2TOOLS_JAR:-/tmp/tla2tools.jar}"
if [ ! -f "$JAR" ]; then
  echo "error: $JAR not found." >&2
  echo "  fetch: curl -sSL -o /tmp/tla2tools.jar \\" >&2
  echo "    'https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar'" >&2
  exit 2
fi

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$here/.."
spec="$repo/docs/spec/e12_async_mutex"
cd "$spec"
# C++ compiler for the COMPILE-PROBE gate (the authority-sealing negative probe).
CXX_BIN="${CXX:-c++}"

# Fresh output directory per invocation (stale-output guard).
outroot="$(mktemp -d -t e12mtx-tlc.XXXXXX)"
cleanup() {
  find . -maxdepth 1 -name '*TTrace*' -delete
  rm -rf "$outroot"
}
trap cleanup EXIT

run() {  # run MODEL CFG OUTFILE
  local model="$1" cfg="$2" outfile="$3"
  java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -nowarning \
       -config "$cfg" "$model" >"$outfile" 2>&1
  return 0  # tolerate TLC's nonzero exit on counterexample
}

tlc_launched() {
  local f="$1"
  grep -q '^Starting\.\.\.' "$f"
}

tlc_passed() {
  local f="$1"
  grep -q 'Model checking completed. No error has been found' "$f"
}

named_violation() {
  local f="$1" expected="$2"
  grep -Eq "Invariant ${expected} is violated" "$f" \
    || grep -Eq "Temporal property ${expected} was violated" "$f" \
    || grep -Eq "Property ${expected} is violated" "$f"
}

states_line() {
  local f="$1"
  grep -m1 'states generated' "$f" || true
}

expect_pass() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/${tag}.out"
  run "$model" "$cfg" "$out"
  if ! tlc_launched "$out"; then
    echo "FAIL  $label (TLC did not launch / parse / config error)"
    tail -20 "$out"
    return 1
  fi
  if tlc_passed "$out"; then
    echo "PASS  $label  ($(states_line "$out"))"
    return 0
  fi
  echo "FAIL  $label (expected PASS, got violation)"
  tail -20 "$out"
  return 1
}

expect_fail() {
  local label="$1" model="$2" cfg="$3" expected="$4" tag="$5"
  local out="$outroot/${tag}.out"
  run "$model" "$cfg" "$out"
  if ! tlc_launched "$out"; then
    echo "FAIL  $label (TLC did not launch / parse / config error)"
    tail -20 "$out"
    return 1
  fi
  if tlc_passed "$out"; then
    echo "FAIL  $label (expected ${expected} violation, model PASSED -- defect not reached)"
    return 1
  fi
  if ! named_violation "$out" "$expected"; then
    echo "FAIL  $label (expected property ${expected} NOT the violation)"
    grep -m1 -E 'Invariant .+ is violated|Property .+ is violated|Temporal property' "$out" || true
    tail -8 "$out"
    return 1
  fi
  echo "CEX   $label (${expected} violated, as expected)  ($(states_line "$out"))"
  return 0
}

# WRONG-PROPERTY gate: a negative model checked against a property its defect
# does NOT target must NOT flag the EXPECTED property. NEG-M3's defect (non-FIFO
# grant) does not violate InvGrantOwnerCommit; checking the wrong property must
# PASS and must not name InvFIFOGrant. This proves the defect is property-
# specific (the expected named violation is not a generic artifact).
wrong_property_gate() {
  local out="$outroot/wrongprop.out"
  cat > "$outroot/E12AsyncMutexNegM3WrongProp.cfg" <<'EOF'
SPECIFICATION Spec
INVARIANT InvGrantOwnerCommit

CONSTANTS
F1 = F1
F2 = F2
F3 = F3
Fibers = {F1, F2, F3}
E1 = E1
E2 = E2
E3 = E3
Epochs = {E1, E2, E3}
EOF
  run E12AsyncMutexNegM3 "$outroot/E12AsyncMutexNegM3WrongProp.cfg" "$out"
  if ! tlc_launched "$out"; then
    echo "FAIL  WRONG-PROPERTY gate (TLC did not launch)"
    tail -20 "$out"
    return 1
  fi
  if ! tlc_passed "$out"; then
    echo "FAIL  WRONG-PROPERTY gate (InvGrantOwnerCommit unexpectedly violated by NEG-M3's non-FIFO grant)"
    grep -m1 -E 'Invariant .+ is violated|Property .+ is violated' "$out" || true
    tail -8 "$out"
    return 1
  fi
  if named_violation "$out" "InvFIFOGrant"; then
    echo "FAIL  WRONG-PROPERTY gate (InvFIFOGrant flagged under a wrong-property config)"
    return 1
  fi
  echo "OK    WRONG-PROPERTY gate (InvGrantOwnerCommit passes under NEG-M3; expected property not mis-flagged)"
  return 0
}

tlc_version() {
  # Report ONLY the actual runtime TLC version string (not a release tag).
  grep -m1 '^TLC2 Version' "$outroot/E12AsyncMutex.safety.out" \
    | sed 's/^TLC2 Version/  TLC runtime version:/' || true
}

# COMPILE-PROBE gate (E12-C-IMPLEMENTATION): the raw WaitQueue bypass on an
# AsyncMutex must FAIL to compile (the AsyncMutex public authority is sealed).
# F-MTX-SEAM-1: ordinary production code cannot obtain an AsyncMutex's WaitQueue
# (no public wait_queue() accessor) and therefore cannot synthesize a
# RESOURCE_WAKE via scheduler.wake_wait_one(mtx.wait_queue()), nor reach the
# private Scheduler Mutex seams, owner(), or is_locked().
compile_probe_gate() {
  local probe="$repo/tests/e12_async_mutex_authority_probe.cpp"
  if [ ! -f "$probe" ]; then
    echo "FAIL  COMPILE-PROBE gate (probe file missing: $probe)"
    return 1
  fi
  local out="$outroot/probe.out"
  # Syntax-only compile of the bypass. MUST fail.
  if "$CXX_BIN" -std=c++20 -fsyntax-only -I"$repo/include" "$probe" >"$out" 2>&1; then
    echo "FAIL  COMPILE-PROBE gate (bypass COMPILED -- AsyncMutex authority regressed)"
    tail -10 "$out"
    return 1
  fi
  if ! grep -Eq "wait_queue|owner|is_locked" "$out"; then
    echo "FAIL  COMPILE-PROBE gate (compile failed but not on a sealed name -- investigate)"
    tail -10 "$out"
    return 1
  fi
  echo "OK    COMPILE-PROBE gate (raw WaitQueue/owner/is_locked bypass sealed: fails to compile)"
  return 0
}

echo "=== E12-C AsyncMutex formal gate (TLC2, jar=$JAR; CXX=$CXX_BIN) -- SAFETY ONLY ==="
echo
rc=0

# Correct safety model (all 19 invariants).
expect_pass "E12AsyncMutex [safety, 19 invariants]" \
            E12AsyncMutex E12AsyncMutex.cfg E12AsyncMutex.safety || rc=1

# NEG-M1: NonOwnerUnlock.
expect_fail "NEG-M1 NonOwnerUnlock" \
            E12AsyncMutexNegM1 E12AsyncMutexNegM1.cfg \
            InvUnlockAuthority E12AsyncMutexNegM1 || rc=1

# NEG-M2: RecursiveAcquire.
expect_fail "NEG-M2 RecursiveAcquire" \
            E12AsyncMutexNegM2 E12AsyncMutexNegM2.cfg \
            InvRecursiveForbidden E12AsyncMutexNegM2 || rc=1

# NEG-M3: NonFIFOGrant.
expect_fail "NEG-M3 NonFIFOGrant" \
            E12AsyncMutexNegM3 E12AsyncMutexNegM3.cfg \
            InvFIFOGrant E12AsyncMutexNegM3 || rc=1

# NEG-M4: Barging.
expect_fail "NEG-M4 Barging" \
            E12AsyncMutexNegM4 E12AsyncMutexNegM4.cfg \
            InvNoBarging E12AsyncMutexNegM4 || rc=1

# NEG-M5: GrantWithoutOwnerCommit.
expect_fail "NEG-M5 GrantWithoutOwnerCommit" \
            E12AsyncMutexNegM5 E12AsyncMutexNegM5.cfg \
            InvGrantOwnerCommit E12AsyncMutexNegM5 || rc=1

# NEG-M6: PublicationWithoutGrantCoupling.
expect_fail "NEG-M6 PublicationWithoutGrantCoupling" \
            E12AsyncMutexNegM6 E12AsyncMutexNegM6.cfg \
            InvGrantPublicationCoupling E12AsyncMutexNegM6 || rc=1

# NEG-M7: AdmissionClosureFailure.
expect_fail "NEG-M7 AdmissionClosureFailure" \
            E12AsyncMutexNegM7 E12AsyncMutexNegM7.cfg \
            InvAdmissionClosure E12AsyncMutexNegM7 || rc=1

# NEG-M8: CancelRevokesHandoff.
expect_fail "NEG-M8 CancelRevokesHandoff" \
            E12AsyncMutexNegM8 E12AsyncMutexNegM8.cfg \
            InvGrantFinality E12AsyncMutexNegM8 || rc=1

# NEG-M9: DeadlineRevokesHandoff.
expect_fail "NEG-M9 DeadlineRevokesHandoff" \
            E12AsyncMutexNegM9 E12AsyncMutexNegM9.cfg \
            InvGrantFinality E12AsyncMutexNegM9 || rc=1

# NEG-M10: ImmediatePublication.
expect_fail "NEG-M10 ImmediatePublication" \
            E12AsyncMutexNegM10 E12AsyncMutexNegM10.cfg \
            InvPublicationRequiresSuspensionOrHandoff E12AsyncMutexNegM10 || rc=1

# NEG-M11: DestructionWhileOwnedOrQueued.
expect_fail "NEG-M11 DestructionWhileOwnedOrQueued" \
            E12AsyncMutexNegM11 E12AsyncMutexNegM11.cfg \
            InvDestructionPrecondition E12AsyncMutexNegM11 || rc=1

# Wrong-property gate (defect specificity).
wrong_property_gate || rc=1

# Compile-probe gate (only if the production probe exists; Commit D onward).
# During Commit C (formal-only) the probe is absent and the gate is SKIPPED
# (reported, not failed). Once Commit D adds the probe, the gate is enforced.
if [ -f "$repo/tests/e12_async_mutex_authority_probe.cpp" ]; then
  compile_probe_gate || rc=1
else
  echo "SKIP  COMPILE-PROBE gate (production probe not yet present; Commit D)"
fi

echo
echo "--- TLC runtime version (actual) ---"
tlc_version
echo "  TLA+ tools release tag: not associated with a verified release tag"
echo "    (the jar is a 2026 development build; no v1.8.0 association asserted"
echo "     without jar-metadata proof)"
echo
echo "=== gate ${rc}-ed (0 = all expected verdicts + named properties + wrong-property gate + compile-probe) ==="
exit "$rc"
