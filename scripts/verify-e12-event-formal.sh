#!/usr/bin/env bash
# verify-e12-event-formal.sh -- reproducible E12-A Event TLA+ / TLC formal gate.
#
# Runs the correct E12 Event safety + liveness models and both negative models
# (NEG-EVENT-1, NEG-EVENT-2) through TLC, asserting each produces its expected
# result AND that each negative model violates its EXPECTED NAMED PROPERTY
# (not just any generic "Invariant violated"/"Property violated" message).
#
#   correct safety     -> all invariants PASS (E1, E2, E3, E5, E6)
#   correct liveness   -> EventSetDrainLivenessNonVacuous PASS (E4)
#   NEG-EVENT-1        -> InvEventAdmissionClosure FAILS (lost set during admission)
#   NEG-EVENT-2        -> EventSetDrainLivenessNonVacuous FAILS (wake-one strands waiter)
#
# Expected-property mapping (declared once per negative model below). The script
# FAILS if:
#   - TLC fails to launch (java/runtime error)
#   - the module fails to parse
#   - the config is invalid
#   - the WRONG invariant/property fails first (expected named property absent)
#   - a negative model unexpectedly PASSES (no counterexample)
#   - stale output is parsed (a fresh output file is used per invocation)
#
# Requires a tla2tools.jar. By default looks for /tmp/tla2tools.jar (the path
# the E7-E11 READMEs expect); override with TLA2TOOLS_JAR=/path/to/tla2tools.jar
# or fetch v1.8.0 from https://github.com/tlaplus/tlaplus/releases.
#
# Usage:
#   scripts/verify-e12-event-formal.sh
#   TLA2TOOLS_JAR=/opt/tla2tools.jar scripts/verify-e12-event-formal.sh
#
# Exit status: 0 iff every model produced its expected verdict AND every negative
# model's expected named property was observed as the first violation.
set -euo pipefail

JAR="${TLA2TOOLS_JAR:-/tmp/tla2tools.jar}"
if [ ! -f "$JAR" ]; then
  echo "error: $JAR not found." >&2
  echo "  fetch: curl -sSL -o /tmp/tla2tools.jar \\" >&2
  echo "    'https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar'" >&2
  exit 2
fi

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
spec="$here/../docs/spec/e12_event"
cd "$spec"

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

expect_pass() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/${tag}.out"
  run "$model" "$cfg" "$out"
  if ! tlc_launched "$out"; then
    echo "FAIL  $label (TLC did not launch)"
    tail -20 "$out"
    return 1
  fi
  if tlc_passed "$out"; then
    echo "PASS  $label"
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
    echo "FAIL  $label (TLC did not launch)"
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
  echo "CEX   $label (${expected} violated, as expected)"
  return 0
}

tlc_version() {
  grep -m1 '^TLC2 Version' "$outroot/E12Event.safety.out" \
    | sed 's/^TLC2 Version/  TLC version:/' || true
}

echo "=== E12-A Event formal gate (TLC2, jar=$JAR) ==="
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

echo
echo "--- TLC version (actual) ---"
tlc_version
echo
echo "=== gate ${rc}-ed (0 = all expected verdicts + named properties observed) ==="
exit "$rc"
