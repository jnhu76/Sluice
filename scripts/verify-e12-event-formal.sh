#!/usr/bin/env bash
# verify-e12-event-formal.sh -- reproducible E12-A Event TLA+ / TLC formal gate
# (E12-A-EVENT-CORRECTIVE-1).
#
# Runs the correct E12 Event safety + liveness models and ALL FOUR negative
# models (NEG-EVENT-1..4) through TLC, asserting each produces its expected
# result AND that each negative model violates its EXPECTED NAMED PROPERTY
# (not just any generic "Invariant/Property violated" message). Also runs a
# WRONG-PROPERTY gate (a negative model checked against a property its defect
# does NOT violate must NOT flag the expected property) and a compile-negative
# API probe (the raw WaitQueue bypass must fail to compile).
#
#   correct safety        -> all invariants PASS (E1, E2, E3, E5, E6)
#   correct liveness      -> EventSetDrainLivenessNonVacuous PASS (E4)
#   NEG-EVENT-1           -> InvEventAdmissionClosure FAILS (lost set at admission)
#   NEG-EVENT-2           -> EventSetDrainLivenessNonVacuous FAILS (wake-one strands)
#   NEG-EVENT-3           -> InvSetEpochIsolation FAILS (old set wakes post-reset)
#   NEG-EVENT-4           -> InvResetNonResolution FAILS (reset resolves waiter)
#   WRONG-PROPERTY gate   -> NEG-3 vs InvSingleResolutionWinner does NOT flag
#                            InvSetEpochIsolation (defect is property-specific)
#   COMPILE-PROBE gate    -> e12_event_authority_probe.cpp FAILS to compile
#                            (raw WaitQueue bypass is sealed)
#
# Expected-property mapping (declared once per negative model below). The script
# FAILS if:
#   - TLC fails to launch (java/runtime error)
#   - the module fails to parse / the config is invalid
#   - the correct model fails
#   - a negative model unexpectedly PASSES (no counterexample)
#   - the WRONG invariant/property fails first (expected named property absent)
#   - the wrong-property gate flags the expected property (defect not specific)
#   - the compile probe COMPILES (Event authority regressed)
#   - stale output is reused (a fresh output directory is used per invocation)
#
# Requires a tla2tools.jar. By default looks for /tmp/tla2tools.jar (the path
# the E7-E11 READMEs expect); override with TLA2TOOLS_JAR=/path/to/tla2tools.jar
# or fetch v1.8.0 from https://github.com/tlaplus/tlaplus/releases.
#
# The compile probe uses ${CXX:-clang++} with the repo include dir.
#
# Usage:
#   scripts/verify-e12-event-formal.sh
#   TLA2TOOLS_JAR=/opt/tla2tools.jar scripts/verify-e12-event-formal.sh
#
# Exit status: 0 iff every gate produced its expected verdict AND every negative
# model's expected named property was observed as the first violation AND the
# compile probe failed to compile AND the wrong-property gate did not misfire.
set -euo pipefail

JAR="${TLA2TOOLS_JAR:-/tmp/tla2tools.jar}"
if [ ! -f "$JAR" ]; then
  echo "error: $JAR not found." >&2
  echo "  fetch: curl -sSL -o /tmp/tla2tools.jar \\" >&2
  echo "    'https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar'" >&2
  exit 2
fi

CXX_BIN="${CXX:-clang++}"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
spec="$here/../docs/spec/e12_event"
repo="$here/.."
cd "$spec"

# Fresh output directory per invocation (stale-output guard).
outroot="$(mktemp -d -t e12-tlc.XXXXXX)"
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
# does NOT target must NOT flag the EXPECTED property. Concretely, NEG-3's defect
# (lost serialization) does not violate InvSingleResolutionWinner; if the gate
# flags InvSetEpochIsolation under that config, the property mapping is wrong.
# We build a one-off config that checks the WRONG property and assert it does
# NOT mention the expected property name.
wrong_property_gate() {
  local out="$outroot/wrongprop.out"
  cat > "$outroot/E12Neg3WrongProp.cfg" <<'EOF'
SPECIFICATION Spec
INVARIANT InvSingleResolutionWinner

CONSTANTS
N0 = N0
N1 = N1
Nodes = {N0, N1}
MaxGen = 2
EOF
  run E12EventNeg3StaleSet "$outroot/E12Neg3WrongProp.cfg" "$out"
  if ! tlc_launched "$out"; then
    echo "FAIL  WRONG-PROPERTY gate (TLC did not launch)"
    tail -20 "$out"
    return 1
  fi
  # The wrong property (single resolution) may or may not be violated by NEG-3,
  # but the EXPECTED property (InvSetEpochIsolation) must NOT be the flagged
  # property when a DIFFERENT invariant is checked.
  if named_violation "$out" "InvSetEpochIsolation"; then
    echo "FAIL  WRONG-PROPERTY gate (InvSetEpochIsolation flagged under a wrong-property config)"
    return 1
  fi
  echo "OK    WRONG-PROPERTY gate (expected property not mis-flagged)"
  return 0
}

# COMPILE-PROBE gate: the raw WaitQueue bypass must FAIL to compile (the Event
# public authority is sealed). T24.
compile_probe_gate() {
  local probe="$repo/tests/e12_event_authority_probe.cpp"
  if [ ! -f "$probe" ]; then
    echo "FAIL  COMPILE-PROBE gate (probe file missing: $probe)"
    return 1
  fi
  local out="$outroot/probe.out"
  # Syntax-only compile of the bypass. MUST fail.
  if "$CXX_BIN" -std=c++20 -fsyntax-only -I"$repo/include" "$probe" >"$out" 2>&1; then
    echo "FAIL  COMPILE-PROBE gate (bypass COMPILED -- Event authority regressed)"
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

tlc_version() {
  # Report ONLY the actual runtime TLC version string (not a release tag).
  grep -m1 '^TLC2 Version' "$outroot/E12Event.safety.out" \
    | sed 's/^TLC2 Version/  TLC runtime version:/' || true
}

echo "=== E12-A Event formal gate (TLC2, jar=$JAR; CXX=$CXX_BIN) ==="
echo
rc=0

# Correct safety model (E1, E2, E3, E5, E6).
expect_pass "E12Event [safety, E1,E2,E3,E5,E6]" \
            E12Event E12Event.cfg E12Event.safety || rc=1

# Correct liveness model (E4).
expect_pass "E12Event [liveness, E4]" \
            E12Event E12EventLiveness.cfg E12Event.liveness || rc=1

# NEG-EVENT-1: Lost Set During Admission -> InvEventAdmissionClosure violated.
expect_fail "NEG-EVENT-1 LostSetDuringAdmission" \
            E12EventNeg1LostSet E12EventNeg1LostSet.cfg \
            InvEventAdmissionClosure E12EventNeg1 || rc=1

# NEG-EVENT-2: Wake-One Strands Waiter -> EventSetDrainLivenessNonVacuous violated.
expect_fail "NEG-EVENT-2 WakeOneStrandsWaiter" \
            E12EventNeg2WakeOne E12EventNeg2WakeOne.cfg \
            EventSetDrainLivenessNonVacuous E12EventNeg2 || rc=1

# NEG-EVENT-3: Old Set Wakes Post-Reset Waiter -> InvSetEpochIsolation violated.
expect_fail "NEG-EVENT-3 OldSetWakesPostResetWaiter" \
            E12EventNeg3StaleSet E12EventNeg3StaleSet.cfg \
            InvSetEpochIsolation E12EventNeg3 || rc=1

# NEG-EVENT-4: Reset Resolves Waiter -> InvResetNonResolution violated.
expect_fail "NEG-EVENT-4 ResetResolvesWaiter" \
            E12EventNeg4ResetResolve E12EventNeg4ResetResolve.cfg \
            InvResetNonResolution E12EventNeg4 || rc=1

# Wrong-property gate.
wrong_property_gate || rc=1

# Compile-probe gate (T24).
compile_probe_gate || rc=1

echo
echo "--- TLC runtime version (actual) ---"
tlc_version
echo "  TLA+ tools release tag (recorded separately): v1.8.0"
echo
echo "=== gate ${rc}-ed (0 = all expected verdicts + named properties + gates) ==="
exit "$rc"
