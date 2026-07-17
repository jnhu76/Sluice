#!/usr/bin/env bash
# verify-e12-queue-formal.sh -- reproducible E12-E Queue TLA+ / TLC formal gate
# (E12-E-QUEUE-FORMAL-MODEL-1 / B4), SAFETY ONLY.
#
# Runs the two correct E12 Queue safety models (Model A: bounded MPMC FIFO;
# Model B: Open/Closed monotonicity) and ALL SEVEN in-scope negative models
# (NEG-QUEUE-1..7) through TLC, asserting each produces its expected result AND
# that each negative model violates its EXPECTED NAMED PROPERTY (not just any
# generic "Invariant violated" message). Also runs a WRONG-PROPERTY gate that
# asserts a negative's defect is property-specific.
#
# This is a SAFETY-ONLY gate. There is no liveness run. Liveness / progress
# (every admitted item is eventually delivered or returned; every waiter is
# eventually granted or released) is out of scope for B4 and is deferred to a
# later model-C/task as documented in docs/spec/e12_queue/README.md.
#
#   Model A (E12Queue)     -> 12 named invariants PASS
#                             (CapacityBound, UniqueItemOwner, UniqueRingItem,
#                              NoLostItem, NoDuplicatedItem, FIFOBufferOrder,
#                              ProducerWaiterFIFO, ConsumerWaiterFIFO, NoBarging,
#                              CommittedBeforePublished,
#                              NoPublishedPendingCompletion,
#                              LocationConsistency)
#   Model B (E12QueueClosed) -> 7 named invariants PASS
#                             (ClosedAbsorbing, NoCommitAfterClose,
#                              CommittedBeforeCloseRemainsDrainable,
#                              FailedPushRetainsOriginalItem,
#                              ClosedEmptyConsumerTerminal,
#                              NoBufferedItemDiscardOnClose,
#                              CloseProducerRaceLinearizable)
#   NEG-QUEUE-1 DuplicateLease        -> UniqueRingItem FAILS (double-append)
#   NEG-QUEUE-2 MoveNotEmptied        -> UniqueItemOwner FAILS (prodItem not cleared)
#   NEG-QUEUE-3 Barging               -> NoBarging FAILS (dropped eligibility guard)
#   NEG-QUEUE-4 PublishBeforeCommit   -> NoPublishedPendingCompletion FAILS
#   NEG-QUEUE-5 CommitAfterClose      -> NoCommitAfterClose FAILS
#   NEG-QUEUE-6 CloseDiscardsBuffer   -> NoBufferedItemDiscardOnClose FAILS
#   NEG-QUEUE-7 FailedPushLosesItem   -> FailedPushRetainsOriginalItem FAILS
#   WRONG-PROPERTY gate   -> NEG-1's duplicate-lease vs NoBarging PASSES
#                            (defect is property-specific) and does NOT flag
#                            UniqueRingItem under that config
#
# Expected-property mapping (declared once per negative model below). The script
# FAILs if:
#   - TLC fails to launch (java/runtime error)
#   - the module fails to parse / the config is invalid
#   - a correct model fails
#   - a negative model unexpectedly PASSES (no counterexample)
#   - the WRONG invariant/property fails first (expected named property absent)
#   - the wrong-property gate flags the expected property (defect not specific)
#   - a negative DEADLOCKS instead of producing a counterexample
#   - stale output is reused (a fresh output directory is used per invocation)
#
# Requires a tla2tools.jar. By default uses the repo-root $repo/tla2tools.jar
# (TLA2 Version 2.19 of 08 August 2024, rev: 5a47802); override with
# TLA2TOOLS_JAR=/path/to/tla2tools.jar.
#
# Usage:
#   scripts/verify-e12-queue-formal.sh
#   TLA2TOOLS_JAR=/opt/tla2tools.jar scripts/verify-e12-queue-formal.sh
#   TLC_WORKERS=4 scripts/verify-e12-queue-formal.sh
#
# Exit status: 0 iff every gate produced its expected verdict AND every negative
# model's expected named property was observed as a violation AND the
# wrong-property gate did not misfire.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$here/.."
spec="$repo/docs/spec/e12_queue"
JAR="${TLA2TOOLS_JAR:-$repo/tla2tools.jar}"
WORKERS="${TLC_WORKERS:-auto}"

if [ ! -f "$JAR" ]; then
  echo "error: tla2tools.jar not found at $JAR" >&2
  echo "  set TLA2TOOLS_JAR=/path/to/tla2tools.jar" >&2
  exit 2
fi
if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2
  exit 2
fi
if [ ! -d "$spec" ]; then
  echo "error: spec dir $spec missing" >&2
  exit 2
fi

cd "$spec"

# Fresh output directory per invocation (stale-output guard). TLC writes its
# disk-backed state queue / fpset here, and we must never reuse a prior run's.
outroot="$(mktemp -d -t e12queue-tlc.XXXXXX)"
cleanup() {
  find . -maxdepth 1 -name '*TTrace*' -delete
  rm -rf "$outroot"
}
trap cleanup EXIT

run() {  # run MODEL CFG OUTFILE
  local model="$1" cfg="$2" outfile="$3"
  java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -nowarning \
       -config "$cfg" -cleanup -workers "$WORKERS" \
       -metadir "$outroot" "$model" >"$outfile" 2>&1
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

tlc_deadlocked() {
  local f="$1"
  grep -qiE '^(Error: )*Deadlock reached|is deadlocked' "$f"
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
  if tlc_deadlocked "$out"; then
    echo "FAIL  $label (expected ${expected} violation, got DEADLOCK -- defect not reachable)"
    tail -12 "$out"
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
# does NOT target must NOT flag the EXPECTED property. NEG-1's defect
# (duplicate-lease / double-append of one ItemId) only mutates the ring; it does
# NOT drop the producer-eligibility guard. So checking NEG-1 against NoBarging
# (which pins the eligibility guard, not ring contents) must PASS and must not
# name UniqueRingItem. This proves the defect is property-specific (the expected
# named violation is not a generic artifact).
wrong_property_gate() {
  local out="$outroot/wrongprop.out"
  cat > "$outroot/E12QueueNeg1WrongProp.cfg" <<'EOF'
SPECIFICATION Spec
INVARIANT NoBarging

CONSTANTS
P0 = P0
P1 = P1
P2 = P2
C0 = C0
C1 = C1
C2 = C2
I0 = I0
I1 = I1
I2 = I2
PNodes = {P0, P1, P2}
CNodes = {C0, C1, C2}
Items = {I0, I1, I2}
Capacity = 1
EOF
  run E12QueueNegDuplicateLease "$outroot/E12QueueNeg1WrongProp.cfg" "$out"
  if ! tlc_launched "$out"; then
    echo "FAIL  WRONG-PROPERTY gate (TLC did not launch)"
    tail -20 "$out"
    return 1
  fi
  if ! tlc_passed "$out"; then
    echo "FAIL  WRONG-PROPERTY gate (NoBarging unexpectedly violated by NEG-1's duplicate-lease)"
    grep -m1 -E 'Invariant .+ is violated|Property .+ is violated' "$out" || true
    tail -8 "$out"
    return 1
  fi
  if named_violation "$out" "UniqueRingItem"; then
    echo "FAIL  WRONG-PROPERTY gate (UniqueRingItem flagged under a wrong-property config)"
    return 1
  fi
  echo "OK    WRONG-PROPERTY gate (NoBarging passes under NEG-1; expected property not mis-flagged)"
  return 0
}

tlc_version() {
  # Report ONLY the actual runtime TLC version string (from the Model A run).
  grep -m1 '^TLC2 Version' "$outroot/E12Queue.safety.out" \
    | sed 's/^TLC2 Version/  TLC runtime version:/' || true
}

echo "=== E12-E Queue formal gate (TLC2, jar=$JAR; workers=$WORKERS) -- SAFETY ONLY ==="
echo
rc=0

# Model A: bounded MPMC FIFO safety (12 named invariants).
expect_pass "E12Queue [Model A, 12 invariants]" \
            E12Queue E12Queue.cfg E12Queue.safety || rc=1

# Model B: Open/Closed monotonicity safety (7 named invariants).
expect_pass "E12QueueClosed [Model B, 7 invariants]" \
            E12QueueClosed E12QueueClosed.cfg E12QueueClosed.safety || rc=1

# NEG-QUEUE-1: DuplicateLease (FastPushCommit appends the item twice).
expect_fail "NEG-QUEUE-1 DuplicateLease" \
            E12QueueNegDuplicateLease E12QueueNegDuplicateLease.cfg \
            UniqueRingItem E12QueueNeg1 || rc=1

# NEG-QUEUE-2: MoveNotEmptied (ProducerGrantCommit leaves prodItem[p] = it).
expect_fail "NEG-QUEUE-2 MoveNotEmptied" \
            E12QueueNegMoveNotEmptied E12QueueNegMoveNotEmptied.cfg \
            UniqueItemOwner E12QueueNeg2 || rc=1

# NEG-QUEUE-3: Barging (FastPushCommit drops the ProdEligibleSet = {} guard).
expect_fail "NEG-QUEUE-3 Barging" \
            E12QueueNegBarging E12QueueNegBarging.cfg \
            NoBarging E12QueueNeg3 || rc=1

# NEG-QUEUE-4: PublishBeforeCommit (ProducerGrantCommit leaves completion Pending).
expect_fail "NEG-QUEUE-4 PublishBeforeCommit" \
            E12QueueNegPublishBeforeCommit E12QueueNegPublishBeforeCommit.cfg \
            NoPublishedPendingCompletion E12QueueNeg4 || rc=1

# NEG-QUEUE-5: CommitAfterClose (FastPushCommit drops queueState = "Open").
expect_fail "NEG-QUEUE-5 CommitAfterClose" \
            E12QueueNegCommitAfterClose E12QueueNegCommitAfterClose.cfg \
            NoCommitAfterClose E12QueueNeg5 || rc=1

# NEG-QUEUE-6: CloseDiscardsBuffer (CloseLinearize clears the ring + releases).
expect_fail "NEG-QUEUE-6 CloseDiscardsBuffer" \
            E12QueueNegCloseDiscardsBuffer E12QueueNegCloseDiscardsBuffer.cfg \
            NoBufferedItemDiscardOnClose E12QueueNeg6 || rc=1

# NEG-QUEUE-7: FailedPushLosesItem (PushClosed records NoItem, not the original).
expect_fail "NEG-QUEUE-7 FailedPushLosesItem" \
            E12QueueNegFailedPushLosesItem E12QueueNegFailedPushLosesItem.cfg \
            FailedPushRetainsOriginalItem E12QueueNeg7 || rc=1

# Wrong-property gate (defect specificity).
wrong_property_gate || rc=1

echo
echo "--- TLC runtime version (actual) ---"
tlc_version
echo
echo "=== gate ${rc}-ed (0 = all expected verdicts + named properties + wrong-property gate) ==="
exit "$rc"
