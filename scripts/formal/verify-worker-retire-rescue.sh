#!/usr/bin/env bash
# verify-worker-retire-rescue.sh - worker-retirement ticket rescue formal
# gate (MODEL-007e, issue #178, umbrella #171).
#
#   WorkerRetireRescue (positive)      -> all 4 as-built laws PASS
#   NegNoRescue                        -> InvNoTicketOnRetiredWorker CEX (pre-G1)
#   NegNoRescueSpecificity             -> unrelated laws PASS
#   NegRescueCopies                    -> InvSingleTicket CEX
#   NegRescueCopiesSpecificity         -> unrelated laws PASS
#   NegNoRerecord                      -> InvOwnerLocationConsistency CEX
#   NegNoRerecordSpecificity           -> other laws PASS
#   Reach{InitialTicket,RetiredTicketUnconsumed,Pending,Redispatched,
#         Resumed,RescueMove}          -> NoReach* CEX = witness
#
# Source-safe: TLC runs in an isolated mktemp workspace. Fail-closed on
# no-launch, unexpected pass, wrong named invariant, and deadlock.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/worker_retire_rescue"
workers="${TLC_WORKERS:-1}"

source "$here/resolve-jar.sh"
jar="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

outroot="$(mktemp -d -t wrr.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/wrr.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/wrr.* ]]; then
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

expect_pass() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if deadlocked "$out"; then echo "FAIL  $label (deadlock)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "PASS  $label"; return 0; fi
  echo "FAIL  $label (expected PASS)"; tail -20 "$out"; return 1
}

named_violation() { grep -Eq "Invariant $2 is violated" "$1"; }

expect_fail() {
  local label="$1" model="$2" cfg="$3" expected="$4" tag="$5"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if deadlocked "$out"; then echo "FAIL  $label (deadlock, not the named CEX)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "FAIL  $label (expected $expected violation, model passed)"; return 1; fi
  if ! named_violation "$out" "$expected"; then
    echo "FAIL  $label (expected $expected, got another failure)"
    tail -12 "$out"
    return 1
  fi
  echo "CEX   $label ($expected violated, as expected)"
}

rc=0
echo "=== Worker retire-rescue formal gate (workers=$workers) ==="
expect_pass "WorkerRetireRescue" WorkerRetireRescue WorkerRetireRescue.cfg positive || rc=1

expect_fail "NegNoRescue (pre-G1 strand)" WorkerRetireRescue \
  WorkerRetireRescueNegNoRescue.cfg InvNoTicketOnRetiredWorker neg1 || rc=1
expect_pass "NegNoRescueSpecificity" WorkerRetireRescue \
  WorkerRetireRescueNegNoRescueSpecificity.cfg neg1spec || rc=1

expect_fail "NegRescueCopies" WorkerRetireRescue \
  WorkerRetireRescueNegRescueCopies.cfg InvSingleTicket neg2 || rc=1
expect_pass "NegRescueCopiesSpecificity" WorkerRetireRescue \
  WorkerRetireRescueNegRescueCopiesSpecificity.cfg neg2spec || rc=1

expect_fail "NegNoRerecord" WorkerRetireRescue \
  WorkerRetireRescueNegNoRerecord.cfg InvOwnerLocationConsistency neg3 || rc=1
expect_pass "NegNoRerecordSpecificity" WorkerRetireRescue \
  WorkerRetireRescueNegNoRerecordSpecificity.cfg neg3spec || rc=1

expect_fail "ReachInitialTicket" WorkerRetireRescue \
  WorkerRetireRescueReachInitialTicket.cfg NoReachInitialTicket r1 || rc=1
expect_fail "ReachRetiredTicketUnconsumed" WorkerRetireRescue \
  WorkerRetireRescueReachRetiredTicketUnconsumed.cfg NoReachRetiredTicketUnconsumed r2 || rc=1
expect_fail "ReachPending" WorkerRetireRescue \
  WorkerRetireRescueReachPending.cfg NoReachPending r3 || rc=1
expect_fail "ReachRedispatched" WorkerRetireRescue \
  WorkerRetireRescueReachRedispatched.cfg NoReachRedispatched r4 || rc=1
expect_fail "ReachResumed" WorkerRetireRescue \
  WorkerRetireRescueReachResumed.cfg NoReachResumed r5 || rc=1
expect_fail "ReachRescueMove" WorkerRetireRescue \
  WorkerRetireRescueReachRescueMove.cfg NoReachRescueMove r6 || rc=1

echo
if [[ "$rc" -eq 0 ]]; then echo "=== PASS ==="; else echo "=== FAIL ==="; fi
exit "$rc"
