#!/usr/bin/env bash
# verify-cancel-token-epoch.sh — CancelToken request-epoch formal gate
# (MODEL-007c, issue #180, umbrella #171).
#
#   CancelTokenEpoch (positive)                  -> all 7 as-built laws PASS
#   NegStickyAck                                 -> InvAckIsRealEpoch CEX
#   NegStickyAckSpecificity                      -> unrelated laws PASS
#   NegClearKeepsPending                         -> InvClearRemovesIntent CEX
#   NegClearKeepsPendingSpecificity              -> other laws PASS
#   NegDropSingleShot                            -> InvSingleShotPerEpoch CEX
#   NegDropSingleShotSpecificity                 -> other laws PASS
#   NegDropPendingCheck                          -> InvNoDeliveryWhenIdle CEX
#   NegDropPendingCheckSpecificity               -> other laws PASS
#   NegDropProtection                            -> InvProtectionBlocksDelivery CEX
#   NegDropProtectionSpecificity                 -> other laws PASS
#   Reach{RequestCreated,Delivered,Reuse,NewRequestDelivered,SharedDelivered,
#         ProtectedRequestDelivered,RearmRedelivers,ClearedIdle}
#                                                -> NoReach* CEX = witness
#
# Source-safe: TLC runs in an isolated mktemp workspace. Fail-closed on
# no-launch, unexpected pass, wrong named invariant, and deadlock.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/cancel_token_epoch"
workers="${TLC_WORKERS:-1}"

# Resolve jar via shared helper (sets TLA2TOOLS_JAR). Does NOT fall back to
# the repo-root jar, which is not checksum-verified; use bootstrap.py instead.
source "$here/resolve-jar.sh"
jar="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

outroot="$(mktemp -d -t cte.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/cte.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/cte.* ]]; then
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

# A negative gate must violate the NAMED invariant — not "some invariant",
# not a parse error, not a deadlock masquerading as an expected CEX.
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
echo "=== CancelToken request-epoch formal gate (workers=$workers) ==="
expect_pass "CancelTokenEpoch" CancelTokenEpoch CancelTokenEpoch.cfg positive || rc=1

expect_fail "NegStickyAck" CancelTokenEpoch \
  CancelTokenEpochNegStickyAck.cfg InvAckIsRealEpoch neg1 || rc=1
expect_pass "NegStickyAckSpecificity" CancelTokenEpoch \
  CancelTokenEpochNegStickyAckSpecificity.cfg neg1spec || rc=1

expect_fail "NegClearKeepsPending" CancelTokenEpoch \
  CancelTokenEpochNegClearKeepsPending.cfg InvClearRemovesIntent neg2 || rc=1
expect_pass "NegClearKeepsPendingSpecificity" CancelTokenEpoch \
  CancelTokenEpochNegClearKeepsPendingSpecificity.cfg neg2spec || rc=1

expect_fail "NegDropSingleShot" CancelTokenEpoch \
  CancelTokenEpochNegDropSingleShot.cfg InvSingleShotPerEpoch neg3 || rc=1
expect_pass "NegDropSingleShotSpecificity" CancelTokenEpoch \
  CancelTokenEpochNegDropSingleShotSpecificity.cfg neg3spec || rc=1

expect_fail "NegDropPendingCheck" CancelTokenEpoch \
  CancelTokenEpochNegDropPendingCheck.cfg InvNoDeliveryWhenIdle neg4 || rc=1
expect_pass "NegDropPendingCheckSpecificity" CancelTokenEpoch \
  CancelTokenEpochNegDropPendingCheckSpecificity.cfg neg4spec || rc=1

expect_fail "NegDropProtection" CancelTokenEpoch \
  CancelTokenEpochNegDropProtection.cfg InvProtectionBlocksDelivery neg5 || rc=1
expect_pass "NegDropProtectionSpecificity" CancelTokenEpoch \
  CancelTokenEpochNegDropProtectionSpecificity.cfg neg5spec || rc=1

expect_fail "ReachRequestCreated" CancelTokenEpoch \
  CancelTokenEpochReachRequestCreated.cfg NoReachRequestCreated r1 || rc=1
expect_fail "ReachDelivered" CancelTokenEpoch \
  CancelTokenEpochReachDelivered.cfg NoReachDelivered r2 || rc=1
expect_fail "ReachReuse" CancelTokenEpoch \
  CancelTokenEpochReachReuse.cfg NoReachReuse r3 || rc=1
expect_fail "ReachNewRequestDelivered" CancelTokenEpoch \
  CancelTokenEpochReachNewRequestDelivered.cfg NoReachNewRequestDelivered r4 || rc=1
expect_fail "ReachSharedDelivered" CancelTokenEpoch \
  CancelTokenEpochReachSharedDelivered.cfg NoReachSharedDelivered r5 || rc=1
expect_fail "ReachProtectedRequestDelivered" CancelTokenEpoch \
  CancelTokenEpochReachProtectedRequestDelivered.cfg NoReachProtectedRequestDelivered r6 || rc=1
expect_fail "ReachRearmRedelivers" CancelTokenEpoch \
  CancelTokenEpochReachRearmRedelivers.cfg NoReachRearmRedelivers r7 || rc=1
expect_fail "ReachClearedIdle" CancelTokenEpoch \
  CancelTokenEpochReachClearedIdle.cfg NoReachClearedIdle r8 || rc=1

echo
if [[ "$rc" -eq 0 ]]; then echo "=== PASS ==="; else echo "=== FAIL ==="; fi
exit "$rc"
