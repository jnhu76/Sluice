#!/usr/bin/env bash
# verify-request-arena.sh — RequestArena / RequestSlot explicit-I/O lifecycle
# formal gate (closes the manifest's former request-arena-lifecycle
# ACCEPTED FORMAL-DEBT gap).
#
#   RequestArena (FAULT="None")  -> full invariant conjunction PASS
#                                    (doubles as the fault restore gate)
#   RequestArenaLiveness         -> WF(Enqueue) + WF(Reap) liveness PASS
#   Fault* cfgs (NEG-RA-1..6)    -> each violates its NAMED invariant
#   WrongProp1/2                 -> fault runs must NOT trip unrelated laws
#   SceneW1..W5                  -> NotReach_* witnesses VIOLATED (reachable)
#
# Source-safe: TLC runs in an isolated mktemp workspace.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/request_arena"
workers="${TLC_WORKERS:-1}"

# Resolve jar via shared helper (sets TLA2TOOLS_JAR). Does NOT fall back to
# the repo-root jar, which is not checksum-verified; use bootstrap.py instead.
source "$here/resolve-jar.sh"
jar="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

outroot="$(mktemp -d -t ra.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/ra.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/ra.* ]]; then
    rm -rf -- "$outroot"
  fi
}
trap cleanup EXIT

workdir="$outroot/work"
mkdir -p "$workdir"
cp "$spec"/*.tla "$spec"/*.cfg "$workdir/"
cd "$workdir"

run_tlc() {
  local cfg="$1" tag="$2"
  local metadir="$outroot/$tag.meta"
  mkdir -p "$metadir"
  local rc_tlc=0
  java -XX:+UseParallelGC -cp "$jar" tlc2.TLC -nowarning \
       -workers "$workers" -metadir "$metadir" \
       -config "$cfg" RequestArena >"$outroot/$tag.out" 2>&1 || rc_tlc=$?
  printf '%s\n' "$rc_tlc" >"$outroot/$tag.rc"
}

launched() { grep -q '^Starting\.\.\.' "$1"; }
passed()   { grep -q 'Model checking completed. No error has been found' "$1"; }
named_violation() { grep -Eq "Invariant $2 is violated" "$1"; }

expect_pass() {
  local label="$1" cfg="$2" tag="$3"
  local out="$outroot/$tag.out"
  run_tlc "$cfg" "$tag"
  if ! launched "$out"; then
    echo "FAIL  $label (TLC did not launch; exit=$(<"$outroot/$tag.rc"))"
    tail -20 "$out"
    return 1
  fi
  if passed "$out"; then echo "PASS  $label"; return 0; fi
  echo "FAIL  $label (expected PASS; exit=$(<"$outroot/$tag.rc"))"
  tail -20 "$out"
  return 1
}

expect_fail() {
  local label="$1" cfg="$2" expected="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc "$cfg" "$tag"
  if ! launched "$out"; then
    echo "FAIL  $label (TLC did not launch; exit=$(<"$outroot/$tag.rc"))"
    tail -20 "$out"
    return 1
  fi
  if passed "$out"; then
    echo "FAIL  $label (expected $expected violation, model passed)"; return 1
  fi
  if ! named_violation "$out" "$expected"; then
    echo "FAIL  $label (expected $expected, got another failure; exit=$(<"$outroot/$tag.rc"))"
    tail -12 "$out"
    return 1
  fi
  echo "CEX   $label ($expected violated, as expected)"
}

rc=0
echo "=== RequestArena explicit-I/O lifecycle formal gate (workers=$workers) ==="

# Positive gates (the FAULT="None" run is also the restore gate for every
# fault cfg: with faults disabled the full conjunction re-passes).
expect_pass "correct lifecycle (full Inv conjunction)" \
  RequestArena.cfg correct || rc=1
expect_pass "liveness: acked backend_ready published / pin acked" \
  RequestArenaLiveness.cfg liveness || rc=1

# Negative gates (NEG-RA-1..6): single-mutation faults, named violations.
expect_fail "NEG-RA-1 second terminal winner overwrites" \
  RequestArenaFaultDoubleTerminal.cfg InvNoDoubleTerminal f1 || rc=1
expect_fail "NEG-RA-2 stale-generation cancel mutates new occupant" \
  RequestArenaFaultStaleCancel.cfg InvTerminalRequiresAccepted f2 || rc=1
expect_fail "NEG-RA-3 worker publishes Completion before/without reap" \
  RequestArenaFaultDirectPublish.cfg InvPublishedCompleteness f3 || rc=1
expect_fail "NEG-RA-4 release skips generation increment" \
  RequestArenaFaultNoGenIncrement.cfg InvGenAdvanceOnFree f4 || rc=1
expect_fail "NEG-RA-5 reap publishes a pinned backend_ready head" \
  RequestArenaFaultReapIgnoresPin.cfg InvNoPinnedPublication f5 || rc=1
expect_fail "NEG-RA-6 running cancel stores canceled terminal (not intent)" \
  RequestArenaFaultRunningCancelStores.cfg InvCanceledTerminalSource f6 || rc=1

# Wrong-property controls: a fault that trips every invariant proves nothing.
expect_pass "wrong-prop control: DoubleTerminal leaves publication-once" \
  RequestArenaWrongProp1.cfg wrongprop1 || rc=1
expect_pass "wrong-prop control: StaleCancel leaves ring-membership law" \
  RequestArenaWrongProp2.cfg wrongprop2 || rc=1

# Reachability / non-vacuity witnesses: each NotReach_* must be VIOLATED.
expect_fail "W1 Scheme-B cancel won pending (pin live)" \
  RequestArenaSceneW1.cfg NotReach_W1_SchemeBCancelWonPending w1 || rc=1
expect_fail "W2 enqueue terminal-noop acked the pin" \
  RequestArenaSceneW2.cfg NotReach_W2_EnqueueNoopAfterCancel w2 || rc=1
expect_fail "W3 ordinary result verbatim after cancel intent" \
  RequestArenaSceneW3.cfg NotReach_W3_OrdinaryVerbatimAfterIntent w3 || rc=1
expect_fail "W4 reused slot carries a second committed occupant" \
  RequestArenaSceneW4.cfg NotReach_W4_ReusedSlotCommitted w4 || rc=1
expect_fail "W5 registered waiter delivered by reap" \
  RequestArenaSceneW5.cfg NotReach_W5_WaiterDeliveredByReap w5 || rc=1
expect_fail "W6 kernel-canceled verbatim terminal without caller intent (#262)" \
  RequestArenaSceneW6.cfg NotReach_W6_KernelCanceledNoIntent w6 || rc=1

echo
if [[ "$rc" -eq 0 ]]; then
  echo "=== PASS: RequestArena explicit-I/O lifecycle protocol ==="
else
  echo "=== FAIL ==="
fi
exit "$rc"
