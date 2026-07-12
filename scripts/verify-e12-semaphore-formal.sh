#!/usr/bin/env bash
# verify-e12-semaphore-formal.sh -- reproducible E12-B Semaphore TLA+ / TLC
# formal gate (E12-B-SEMAPHORE-PREPARATION-CORRECTIVE-1), SAFETY ONLY.
#
# Runs the correct E12 Semaphore safety model and ALL SEVEN negative models
# (NEG-SEM-1..7) through TLC, asserting each produces its expected result AND
# that each negative model violates its EXPECTED NAMED PROPERTY (not just any
# generic "Invariant violated" message). Also runs a WRONG-PROPERTY gate that
# asserts a negative's defect is property-specific.
#
# This is a SAFETY-ONLY gate. There is no liveness run. The E12-B
# implementation adds a COMPILE-PROBE gate (E12-B-IMPLEMENTATION) that asserts
# the raw WaitQueue bypass on a Semaphore fails to compile (the Semaphore public
# authority is sealed: no wait_queue() accessor).
#
#   correct safety        -> all invariants PASS
#                            (InvPermitConservation, InvPermitBounds,
#                             InvQueueWellFormed, InvSingleResolution,
#                             InvSinglePublication, InvGrantCommitCoupling,
#                             InvFIFOGrant, InvAdmissionClosure,
#                             InvOverflowNonMutation,
#                             InvNoIdlePermitWithEligibleWaiter,
#                             InvReleaseDisposition, InvPermitFirstDeadline)
#   NEG-SEM-1             -> InvAdmissionClosure FAILS
#   NEG-SEM-2             -> InvPermitConservation FAILS (release permit lost)
#   NEG-SEM-3             -> InvPermitConservation FAILS (double-store)
#   NEG-SEM-4             -> InvFIFOGrant FAILS (non-FIFO grant)
#   NEG-SEM-5             -> InvOverflowNonMutation FAILS (overflow mutates)
#   NEG-SEM-6             -> InvNoIdlePermitWithEligibleWaiter FAILS
#   NEG-SEM-7             -> InvPermitFirstDeadline FAILS
#   WRONG-PROPERTY gate   -> NEG-3's double-store vs InvFIFOGrant PASSES
#                            (defect is property-specific) and does NOT flag
#                            InvPermitConservation under that config
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
#   scripts/verify-e12-semaphore-formal.sh
#   TLA2TOOLS_JAR=/opt/tla2tools.jar scripts/verify-e12-semaphore-formal.sh
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
spec="$repo/docs/spec/e12_semaphore"
cd "$spec"
# C++ compiler for the COMPILE-PROBE gate (the authority-sealing negative probe).
CXX_BIN="${CXX:-c++}"

# Fresh output directory per invocation (stale-output guard).
outroot="$(mktemp -d -t e12sem-tlc.XXXXXX)"
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
# does NOT target must NOT flag the EXPECTED property. NEG-3's defect
# (double-store) does not violate InvFIFOGrant; checking the wrong property
# must PASS and must not name InvPermitConservation. This proves the defect is
# property-specific (the expected named violation is not a generic artifact).
wrong_property_gate() {
  local out="$outroot/wrongprop.out"
  cat > "$outroot/E12SemNeg3WrongProp.cfg" <<'EOF'
SPECIFICATION Spec
INVARIANT InvFIFOGrant

CONSTANTS
N0 = N0
N1 = N1
N2 = N2
Nodes = {N0, N1, N2}
MaxPermits = 2
MaxInit = 2
MaxDue = 2
EOF
  run E12SemNeg3DoubleStore "$outroot/E12SemNeg3WrongProp.cfg" "$out"
  if ! tlc_launched "$out"; then
    echo "FAIL  WRONG-PROPERTY gate (TLC did not launch)"
    tail -20 "$out"
    return 1
  fi
  if ! tlc_passed "$out"; then
    echo "FAIL  WRONG-PROPERTY gate (InvFIFOGrant unexpectedly violated by NEG-3's double-store)"
    grep -m1 -E 'Invariant .+ is violated|Property .+ is violated' "$out" || true
    tail -8 "$out"
    return 1
  fi
  if named_violation "$out" "InvPermitConservation"; then
    echo "FAIL  WRONG-PROPERTY gate (InvPermitConservation flagged under a wrong-property config)"
    return 1
  fi
  echo "OK    WRONG-PROPERTY gate (InvFIFOGrant passes under NEG-3; expected property not mis-flagged)"
  return 0
}

tlc_version() {
  # Report ONLY the actual runtime TLC version string (not a release tag).
  grep -m1 '^TLC2 Version' "$outroot/E12Semaphore.safety.out" \
    | sed 's/^TLC2 Version/  TLC runtime version:/' || true
}

# COMPILE-PROBE gate (E12-B-IMPLEMENTATION): the raw WaitQueue bypass on a
# Semaphore must FAIL to compile (the Semaphore public authority is sealed).
# F-SEM-SEAM-1: ordinary production code cannot obtain a Semaphore's WaitQueue
# (no public wait_queue() accessor) and therefore cannot synthesize a
# RESOURCE_WAKE via scheduler.wake_wait_one(sem.wait_queue()).
compile_probe_gate() {
  local probe="$repo/tests/e12_semaphore_authority_probe.cpp"
  if [ ! -f "$probe" ]; then
    echo "FAIL  COMPILE-PROBE gate (probe file missing: $probe)"
    return 1
  fi
  local out="$outroot/probe.out"
  # Syntax-only compile of the bypass. MUST fail.
  if "$CXX_BIN" -std=c++20 -fsyntax-only -I"$repo/include" "$probe" >"$out" 2>&1; then
    echo "FAIL  COMPILE-PROBE gate (bypass COMPILED -- Semaphore authority regressed)"
    tail -10 "$out"
    return 1
  fi
  if ! grep -q "wait_queue" "$out"; then
    echo "FAIL  COMPILE-PROBE gate (compile failed but not on wait_queue -- investigate)"
    tail -10 "$out"
    return 1
  fi
  echo "OK    COMPILE-PROBE gate (raw WaitQueue bypass sealed: fails to compile)"
  return 0
}

echo "=== E12-B Semaphore formal gate (TLC2, jar=$JAR; CXX=$CXX_BIN) -- SAFETY ONLY ==="
echo
rc=0

# Correct safety model (all 12 invariants).
expect_pass "E12Semaphore [safety, 12 invariants]" \
            E12Semaphore E12Semaphore.cfg E12Semaphore.safety || rc=1

# NEG-SEM-1: AdmissionClosure (suspend despite available permit).
expect_fail "NEG-SEM-1 AdmissionClosure" \
            E12SemNeg1AdmissionClosure E12SemNeg1AdmissionClosure.cfg \
            InvAdmissionClosure E12SemNeg1 || rc=1

# NEG-SEM-2: ReleaseLoss (grant permit lost: acceptedRelease++ without acquired++).
expect_fail "NEG-SEM-2 ReleaseLoss" \
            E12SemNeg2ReleaseLoss E12SemNeg2ReleaseLoss.cfg \
            InvPermitConservation E12SemNeg2 || rc=1

# NEG-SEM-3: DoubleStore (one accepted release stores two permits).
expect_fail "NEG-SEM-3 DoubleStore" \
            E12SemNeg3DoubleStore E12SemNeg3DoubleStore.cfg \
            InvPermitConservation E12SemNeg3 || rc=1

# NEG-SEM-4: NonFIFOGrant (grant an eligible node that is not the FIFO head).
expect_fail "NEG-SEM-4 NonFIFOGrant" \
            E12SemNeg4NonFIFOGrant E12SemNeg4NonFIFOGrant.cfg \
            InvFIFOGrant E12SemNeg4 || rc=1

# NEG-SEM-5: OverflowMutation (overflow mutates available).
expect_fail "NEG-SEM-5 OverflowMutation" \
            E12SemNeg5OverflowMutation E12SemNeg5OverflowMutation.cfg \
            InvOverflowNonMutation E12SemNeg5 || rc=1

# NEG-SEM-6: IdlePermitEligibleWaiter (store a permit while an eligible waiter is queued).
expect_fail "NEG-SEM-6 IdlePermitEligibleWaiter" \
            E12SemNeg6IdlePermitEligibleWaiter E12SemNeg6IdlePermitEligibleWaiter.cfg \
            InvNoIdlePermitWithEligibleWaiter E12SemNeg6 || rc=1

# NEG-SEM-7: DeadlinePrecedence (admissible permit + due deadline resolves Expired).
expect_fail "NEG-SEM-7 DeadlinePrecedence" \
            E12SemNeg7DeadlinePrecedence E12SemNeg7DeadlinePrecedence.cfg \
            InvPermitFirstDeadline E12SemNeg7 || rc=1

# Wrong-property gate (defect specificity).
wrong_property_gate || rc=1

# Compile-probe gate (E12-B-IMPLEMENTATION, F-SEM-SEAM-1 authority sealing).
compile_probe_gate || rc=1

echo
echo "--- TLC runtime version (actual) ---"
tlc_version
echo "  TLA+ tools release tag: not associated with a verified release tag"
echo "    (the jar is a 2026 development build; no v1.8.0 association asserted"
echo "     without jar-metadata proof)"
echo
echo "=== gate ${rc}-ed (0 = all expected verdicts + named properties + wrong-property gate + compile-probe) ==="
exit "$rc"
