#!/usr/bin/env bash
# verify-e12-async-condition-formal.sh -- reproducible E12-D AsyncCondition TLA+ / TLC
# formal gate (E12-D-ASYNC-CONDITION-PREPARATION-CORRECTIVE-5), SAFETY ONLY.
#
# Runs the correct E12 AsyncCondition safety model and ALL TEN negative models
# (NEG-C1..NEG-C10) through TLC, asserting each produces its expected result AND
# that each negative model violates its EXPECTED NAMED PROPERTY (not just any
# generic "Invariant violated" message). Also runs a WRONG-PROPERTY gate that
# asserts a negative's defect is property-specific.
#
# This is a SAFETY-ONLY gate. There is no liveness run.
#
#   correct safety        -> all invariants PASS (Inv)
#   NEG-C1  NonOwnerWait              -> InvConditionWaiterDoesNotOwnMutex FAILS
#   NEG-C2  NotifyAnyNonRegistered    -> InvConditionResolvedFinality FAILS
#   NEG-C3  ReturnOwnedNoGrant        -> InvReturnedOwnsMutex FAILS
#   NEG-C4  CancelReacquireEpoch     -> InvTerminalAttemptFinality FAILS
#   NEG-C5  NotifyAllNoDrain         -> InvConditionQueueWellFormed FAILS
#   NEG-C6  ReacquireNonFIFO         -> InvFIFOGrant FAILS
#   NEG-C7  DestroyWithActiveWaiters -> InvDestructionPrecondition FAILS
#   NEG-C8  WaitReleaseBeforeRegister -> InvNoLostNotifyWindow FAILS
#   NEG-C9  HandoffNonFIFO           -> InvEligiblePreMutexQueue FAILS
#   NEG-C10 SeparateQueues           -> InvOrdinaryAndReacquireFIFO FAILS
#   WRONG-PROPERTY gate              -> NEG-C6's ReacquireNonFIFO vs
#                                       InvEligiblePreMutexQueue PASSES (defect
#                                       is property-specific) and does NOT flag
#                                       InvFIFOGrant under that config
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
#   scripts/verify-e12-async-condition-formal.sh
#   TLA2TOOLS_JAR=/opt/tla2tools.jar scripts/verify-e12-async-condition-formal.sh
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
spec="$repo/docs/spec/e12_async_condition"
cd "$spec"

# C++ compiler for the COMPILE-PROBE authority-seal gate (§1.4).
CXX_BIN="${CXX:-c++}"
# Fresh output directory per invocation (stale-output guard).
outroot="$(mktemp -d -t e12cnd-tlc.XXXXXX)"
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
# does NOT target must NOT flag the EXPECTED property. NEG-C6's defect (non-FIFO
# reacquire) does not violate InvEligiblePreMutexQueue; checking the wrong
# property must PASS and must not name InvFIFOGrant. This proves the defect is
# property-specific (the expected named violation is not a generic artifact).
wrong_property_gate() {
  local out="$outroot/wrongprop.out"
  cat > "$outroot/E12AsyncConditionNegC6WrongProp.cfg" <<'EOF'
SPECIFICATION Spec
INVARIANT InvEligiblePreMutexQueue

CONSTANTS
F1 = F1
F2 = F2
F3 = F3
Fibers = {F1, F2, F3}
C1 = C1
C2 = C2
C3 = C3
ConditionEpochs = {C1, C2, C3}
R1 = R1
R2 = R2
R3 = R3
ReacquireEpochs = {R1, R2, R3}
O1 = O1
O2 = O2
O3 = O3
OrdinaryEpochs = {O1, O2, O3}
EOF
  run E12AsyncConditionNegC6 "$outroot/E12AsyncConditionNegC6WrongProp.cfg" "$out"
  if ! tlc_launched "$out"; then
    echo "FAIL  WRONG-PROPERTY gate (TLC did not launch)"
    tail -20 "$out"
    return 1
  fi
  if ! tlc_passed "$out"; then
    echo "FAIL  WRONG-PROPERTY gate (InvEligiblePreMutexQueue unexpectedly violated by NEG-C6's non-FIFO reacquire)"
    grep -m1 -E 'Invariant .+ is violated|Property .+ is violated' "$out" || true
    tail -8 "$out"
    return 1
  fi
  if named_violation "$out" "InvFIFOGrant"; then
    echo "FAIL  WRONG-PROPERTY gate (InvFIFOGrant flagged under a wrong-property config)"
    return 1
  fi
  echo "OK    WRONG-PROPERTY gate (InvEligiblePreMutexQueue passes under NEG-C6; expected property not mis-flagged)"
  return 0
}

tlc_version() {
  grep -m1 '^TLC2 Version' "$outroot/E12AsyncCondition.safety.out" \
    | sed 's/^TLC2 Version/  TLC runtime version:/' || true
}

# COMPILE-PROBE authority-seal gate (construction authorization §1.4). Each of
# the seven sealed accessors / bypasses must FAIL to compile INDEPENDENTLY. The
# gate compiles the probe once per -DPROBE_CASE=N (1..7), expecting FAILURE each
# time. §1.4 forbids a single-file/single-error weak gate: every case is
# verified separately. If any case unexpectedly compiles, the AsyncCondition
# public authority has regressed (a sealed accessor was added or the Condition
# queue became reachable by ordinary code).
#
# Each case targets exactly one sealed surface:
#   1. wait_queue()        2. mutex()           3. waiting_count()
#   4. notify_n()          5. reacquire_node()  6. Scheduler private seam
#   7. wake_wait_one bypass via the private queue
compile_probe_gate() {
  local probe="$repo/tests/e12_async_condition_authority_probe.cpp"
  if [ ! -f "$probe" ]; then
    echo "FAIL  COMPILE-PROBE gate (probe file missing: $probe)"
    return 1
  fi
  local rc_probe=0
  local n
  for n in 1 2 3 4 5 6 7; do
    local out="$outroot/probe_case${n}.out"
    # Syntax-only compile of case N. MUST fail (sealed name / unreachable seam).
    if "$CXX_BIN" -std=c++20 -fsyntax-only -DPROBE_CASE="$n" \
        -I"$repo/include" "$probe" >"$out" 2>&1; then
      echo "FAIL  COMPILE-PROBE case ${n} (bypass COMPILED -- authority regressed)"
      tail -10 "$out"
      rc_probe=1
      continue
    fi
    echo "OK    COMPILE-PROBE case ${n} (sealed: fails to compile)"
  done
  return $rc_probe
}

echo "=== E12-D AsyncCondition formal gate (TLC2, jar=$JAR) -- SAFETY ONLY ==="
echo
rc=0

# Correct safety model (all invariants).
expect_pass "E12AsyncCondition [safety, Inv]" \
            E12AsyncCondition E12AsyncCondition.cfg E12AsyncCondition.safety || rc=1

# POSITIVE REACHABILITY GATES (C9/C10 enabling evidence).
# These prove MutexUnlockHandoff is reachable with >=2 eligible waiters in
# BOTH mixed-kind FIFO orders (Ordinary-then-Reacquire and Reacquire-then-
# Ordinary). Without OrdinaryEpochs >= 3 the contention topology is absent and
# the C9/C10 mutations are indistinguishable from correct FIFO behavior.
# Encoded as invariant-negation: expect_fail = the target state IS reachable.
expect_fail "REACH OrdThenReq" \
            E12AsyncCondition E12AsyncCondition.reach1.cfg \
            NoReachOrdThenReq E12AsyncCondition.reach1 || rc=1
expect_fail "REACH ReqThenOrd" \
            E12AsyncCondition E12AsyncCondition.reach2.cfg \
            NoReachReqThenOrd E12AsyncCondition.reach2 || rc=1

# NEG-C1: NonOwnerWait.
expect_fail "NEG-C1 NonOwnerWait" \
            E12AsyncConditionNegC1 E12AsyncConditionNegC1.cfg \
            InvConditionWaiterDoesNotOwnMutex E12AsyncConditionNegC1 || rc=1

# NEG-C2: NotifyAnyNonRegistered.
expect_fail "NEG-C2 NotifyAnyNonRegistered" \
            E12AsyncConditionNegC2 E12AsyncConditionNegC2.cfg \
            InvConditionResolvedFinality E12AsyncConditionNegC2 || rc=1

# NEG-C3: ReturnOwnedNoGrant.
expect_fail "NEG-C3 ReturnOwnedNoGrant" \
            E12AsyncConditionNegC3 E12AsyncConditionNegC3.cfg \
            InvReturnedOwnsMutex E12AsyncConditionNegC3 || rc=1

# NEG-C4: CancelReacquireEpoch.
expect_fail "NEG-C4 CancelReacquireEpoch" \
            E12AsyncConditionNegC4 E12AsyncConditionNegC4.cfg \
            InvTerminalAttemptFinality E12AsyncConditionNegC4 || rc=1

# NEG-C5: NotifyAllNoDrain.
expect_fail "NEG-C5 NotifyAllNoDrain" \
            E12AsyncConditionNegC5 E12AsyncConditionNegC5.cfg \
            InvConditionQueueWellFormed E12AsyncConditionNegC5 || rc=1

# NEG-C6: ReacquireNonFIFO.
expect_fail "NEG-C6 ReacquireNonFIFO" \
            E12AsyncConditionNegC6 E12AsyncConditionNegC6.cfg \
            InvFIFOGrant E12AsyncConditionNegC6 || rc=1

# NEG-C7: DestroyWithActiveWaiters.
expect_fail "NEG-C7 DestroyWithActiveWaiters" \
            E12AsyncConditionNegC7 E12AsyncConditionNegC7.cfg \
            InvDestructionPrecondition E12AsyncConditionNegC7 || rc=1

# NEG-C8: WaitReleaseBeforeRegister.
expect_fail "NEG-C8 WaitReleaseBeforeRegister" \
            E12AsyncConditionNegC8 E12AsyncConditionNegC8.cfg \
            InvNoLostNotifyWindow E12AsyncConditionNegC8 || rc=1

# NEG-C9: HandoffNonFIFO.
expect_fail "NEG-C9 HandoffNonFIFO" \
            E12AsyncConditionNegC9 E12AsyncConditionNegC9.cfg \
            InvEligiblePreMutexQueue E12AsyncConditionNegC9 || rc=1

# NEG-C10: SeparateQueues.
expect_fail "NEG-C10 SeparateQueues" \
            E12AsyncConditionNegC10 E12AsyncConditionNegC10.cfg \
            InvOrdinaryAndReacquireFIFO E12AsyncConditionNegC10 || rc=1

# Wrong-property gate (defect specificity).
wrong_property_gate || rc=1

# Authority-seal compile-probe gate (construction authorization §1.4). Enforced
# only when the production probe exists. Each of the seven sealed surfaces must
# fail to compile independently.
if [ -f "$repo/tests/e12_async_condition_authority_probe.cpp" ]; then
  compile_probe_gate || rc=1
else
  echo "SKIP  COMPILE-PROBE gate (production probe not yet present)"
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
