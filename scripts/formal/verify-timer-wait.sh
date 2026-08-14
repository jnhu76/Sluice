#!/usr/bin/env bash
# verify-timer-wait.sh — reproducible E11 TLA+ / TLC formal gate.
#
# Source-safe: TLC runs in an isolated mktemp workspace.
#
# Runs the correct E11 safety + liveness models and all six negative models
# (NEG-1..NEG-6) through TLC, asserting each produces its expected result AND
# that each negative model violates its EXPECTED NAMED PROPERTY (not just any
# generic "Invariant violated"/"Property violated" message).
#
#   correct safety     -> all invariants PASS
#   correct liveness   -> DeadlineParkLiveness PASS (I6)
#   NEG-1              -> InvSingleResolutionWinner FAILS
#   NEG-2              -> InvSingleResolutionWinner FAILS
#   NEG-3              -> InvWaitEpochIsolation FAILS
#   NEG-4              -> InvTimerLifetimeClosure FAILS
#   NEG-5              -> DeadlineParkLiveness FAILS (liveness)
#   NEG-6              -> InvDeadlineAdmissionClosure FAILS
#
# Source-safe: TLC runs in an isolated mktemp workspace, NOT in spec/tla/.
#
# Exit status: 0 iff every model produced its expected verdict AND every negative
# model's expected named property was observed as the first violation.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/e11_timer_wait"
workers="${TLC_WORKERS:-1}"

# Resolve jar via shared helper (sets TLA2TOOLS_JAR).
source "$here/resolve-jar.sh"
JAR="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

# Isolated workspace — TLC never runs inside spec/tla/.
outroot="$(mktemp -d -t sluice-formal.e11-tlc.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] && [[ "$outroot" == *sluice-formal.e11-tlc.* ]]; then
    rm -rf -- "$outroot"
  fi
}
trap cleanup EXIT

workdir="$outroot/work"
mkdir -p "$workdir"
cp "$spec"/*.tla "$spec"/*.cfg "$workdir/"
cd "$workdir"

run() {  # run MODEL CFG OUTFILE  -> prints nothing; writes TLC stdout+stderr to OUTFILE
  local model="$1" cfg="$2" outfile="$3"
  local metadir="$outroot/${model}.meta"
  mkdir -p "$metadir"
  java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -nowarning \
       -workers "$workers" -metadir "$metadir" \
       -config "$cfg" "$model" >"$outfile" 2>&1 || true
}

# A TLC launch failure (java error, module parse failure, config error) leaves
# NO "Starting..." line and typically a non-"Model checking completed" status.
# Returns 0 if TLC actually launched (reached the state-search phase).
tlc_launched() {  # tlc_launched OUTFILE
  grep -q '^Starting\.\.\.' "$1"
}

# A model that fully PASSED (no error): TLC prints the canonical completion line.
tlc_passed() {  # tlc_passed OUTFILE
  grep -q 'Model checking completed. No error has been found' "$1"
}

# TLC's violation names the property in one of these forms:
#   safety:     "Invariant <X> is violated."
#   liveness:   "Temporal property <X> was violated."  (past tense)
# TLC prints the name verbatim from the config. An EXPECTED name matches if any
# form names it — the model declares exactly one kind.
named_violation() {  # named_violation OUTFILE EXPECTED_PROPERTY
  local outfile="$1" expected="$2"
  grep -Eq "Invariant ${expected} is violated" "$outfile" \
  || grep -Eq "Temporal property ${expected} was violated" "$outfile" \
  || grep -Eq "Property ${expected} is violated" "$outfile"
}

# TLC 2.19 (stable v1.7.4 pin) reports temporal-property violations with the
# GENERIC message "Temporal properties were violated." and does NOT name the
# property (newer rolling builds print "Temporal property <X> was violated").
# For a liveness negative whose config declares EXACTLY ONE PROPERTY, the
# generic message can only refer to that property, so the named assertion is
# preserved by binding it to the config instead of the output phrasing; a
# deadlock (a different defect class) is still rejected.
liveness_named_by_config() {  # liveness_named_by_config CFG OUTFILE EXPECTED
  local cfg="$1" outfile="$2" expected="$3"
  grep -q 'Temporal properties were violated' "$outfile" \
    && ! grep -q 'Deadlock reached' "$outfile" \
    && [[ "$(grep -c '^PROPERTY ' "$cfg")" == "1" ]] \
    && grep -q "^PROPERTY ${expected}$" "$cfg"
}

expect_pass() {  # expect_pass LABEL MODEL CFG OUTFILE_TAG
  local label="$1" model="$2" cfg="$3" tag="${4:-$2}" outfile
  outfile="$outroot/${tag}.out"
  run "$model" "$cfg" "$outfile"
  if ! tlc_launched "$outfile"; then
    printf 'FAIL  %s: TLC failed to launch / parse error\n' "$label"; return 1
  fi
  if tlc_passed "$outfile"; then
    printf 'PASS  %s\n' "$label"
  else
    printf 'FAIL  %s (expected PASS, got error/violation)\n' "$label"
    tail -20 "$outfile"
    return 1
  fi
}

# expect_fail LABEL MODEL CFG EXPECTED_PROPERTY
# Asserts: TLC launched, the model did NOT pass, AND the expected named property
# is the violation (not some other property/invariant).
expect_fail() {
  local label="$1" model="$2" cfg="$3" expected="$4" outfile
  outfile="$outroot/${model}.out"
  run "$model" "$cfg" "$outfile"
  if ! tlc_launched "$outfile"; then
    printf 'FAIL  %s: TLC failed to launch / parse error\n' "$label"; return 1
  fi
  if tlc_passed "$outfile"; then
    printf 'FAIL  %s (expected %s violation, model PASSED — defect not reached)\n' \
           "$label" "$expected"
    return 1
  fi
  if ! named_violation "$outfile" "$expected" \
     && ! liveness_named_by_config "$workdir/$cfg" "$outfile" "$expected"; then
    printf 'FAIL  %s (expected property %s NOT the violation)\n' "$label" "$expected"
    printf '  --- first violation in output: ---\n'
    grep -m1 -E 'Invariant .+ is violated|Property .+ is violated|Temporal property' "$outfile" || true
    tail -8 "$outfile"
    return 1
  fi
  printf 'CEX   %s (%s violated, as expected)\n' "$label" "$expected"
}

# Extract and print the actual TLC version reported by the jar (NOT hard-coded).
# Read from the captured safety-model output.
tlc_version() {
  grep -m1 '^TLC2 Version' "$outroot/E11TimerWait.safety.out" 2>/dev/null \
    | sed 's/^TLC2 /  /' || printf '  (TLC version not captured)\n'
}

echo "=== E11 formal gate (TLC2, jar=$JAR) ==="
rc=0

# --- correct models ---
expect_pass "E11TimerWait [safety, I1-I5,I7]" "E11TimerWait" "E11TimerWait.cfg" \
  "E11TimerWait.safety" || rc=1
expect_pass "E11TimerWait [liveness, I6]"     "E11TimerWait" "E11TimerWaitLiveness.cfg" \
  "E11TimerWait.liveness" || rc=1

# --- negative models: each declares its EXPECTED NAMED property ---
expect_fail "NEG-1 DoublePublication" \
  "E11TimerWaitNeg1DoublePublication" "E11TimerWaitNeg1DoublePublication.cfg" \
  "InvSingleResolutionWinner" || rc=1
expect_fail "NEG-2 TimerCancelDoublePublication" \
  "E11TimerWaitNeg2TimerCancelDoublePublication" "E11TimerWaitNeg2TimerCancelDoublePublication.cfg" \
  "InvSingleResolutionWinner" || rc=1
expect_fail "NEG-3 StaleCrossEpoch" \
  "E11TimerWaitNeg3StaleCrossEpoch" "E11TimerWaitNeg3StaleCrossEpoch.cfg" \
  "InvWaitEpochIsolation" || rc=1
expect_fail "NEG-4 CallbackAfterRetirement" \
  "E11TimerWaitNeg4CallbackAfterRetirement" "E11TimerWaitNeg4CallbackAfterRetirement.cfg" \
  "InvTimerLifetimeClosure" || rc=1
expect_fail "NEG-5 DeadlineLostParked" \
  "E11TimerWaitNeg5DeadlineLostParked" "E11TimerWaitNeg5DeadlineLostParked.cfg" \
  "DeadlineParkLiveness" || rc=1
expect_fail "NEG-6 DeadlineLostAtAdmission" \
  "E11TimerWaitNeg6DeadlineLostAtAdmission" "E11TimerWaitNeg6DeadlineLostAtAdmission.cfg" \
  "InvDeadlineAdmissionClosure" || rc=1

echo "--- TLC version (actual) ---"
tlc_version

echo "=== gate ${rc}-ed (0 = all expected verdicts + named properties observed) ==="
exit "$rc"
