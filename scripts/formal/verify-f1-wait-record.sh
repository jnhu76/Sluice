#!/usr/bin/env bash
# verify-f1-wait-record.sh — F1 WaitRecord registry formal gate (MODEL-007b,
# issue #174, umbrella #171).
#
#   F1WaitRecord (positive)                  -> all 7 as-built laws PASS
#   NegNoGenCheck                            -> InvGenerationIsolation CEX
#   NegNoGenCheckSpecificity                 -> unrelated laws PASS
#   NegReuseWhileDelivered                   -> InvLiveRecordAccounting CEX
#   NegReuseWhileDeliveredSpecificity        -> other laws PASS
#   NegCancelKeepsDelivery                   -> InvSingleAuthority CEX
#   NegCancelKeepsDeliverySpecificity        -> unrelated laws PASS
#   Reach{AuthorityWindow,CancelWon,DeliveryWon,Reuse,StaleDropped}
#                                            -> NoReach* CEX = witness
#
# Source-safe: TLC runs in an isolated mktemp workspace. Fail-closed on
# no-launch, unexpected pass, wrong named invariant, and deadlock.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/f1_wait_record"
workers="${TLC_WORKERS:-1}"

# Resolve jar via shared helper (sets TLA2TOOLS_JAR). Does NOT fall back to
# the repo-root jar, which is not checksum-verified; use bootstrap.py instead.
source "$here/resolve-jar.sh"
jar="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

outroot="$(mktemp -d -t f1-wr.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/f1-wr.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/f1-wr.* ]]; then
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
echo "=== F1 WaitRecord registry formal gate (workers=$workers) ==="
expect_pass "F1WaitRecord" F1WaitRecord F1WaitRecord.cfg positive || rc=1

expect_fail "NegNoGenCheck" F1WaitRecord \
  F1WaitRecordNegNoGenCheck.cfg InvGenerationIsolation neg1 || rc=1
expect_pass "NegNoGenCheckSpecificity" F1WaitRecord \
  F1WaitRecordNegNoGenCheckSpecificity.cfg neg1spec || rc=1

expect_fail "NegReuseWhileDelivered" F1WaitRecord \
  F1WaitRecordNegReuseWhileDelivered.cfg InvLiveRecordAccounting neg2 || rc=1
expect_pass "NegReuseWhileDeliveredSpecificity" F1WaitRecord \
  F1WaitRecordNegReuseWhileDeliveredSpecificity.cfg neg2spec || rc=1

expect_fail "NegCancelKeepsDelivery" F1WaitRecord \
  F1WaitRecordNegCancelKeepsDelivery.cfg InvSingleAuthority neg3 || rc=1
expect_pass "NegCancelKeepsDeliverySpecificity" F1WaitRecord \
  F1WaitRecordNegCancelKeepsDeliverySpecificity.cfg neg3spec || rc=1

expect_fail "ReachAuthorityWindow" F1WaitRecord \
  F1WaitRecordReachAuthorityWindow.cfg NoReachAuthorityWindow r1 || rc=1
expect_fail "ReachCancelWon" F1WaitRecord \
  F1WaitRecordReachCancelWon.cfg NoReachCancelWon r2 || rc=1
expect_fail "ReachDeliveryWon" F1WaitRecord \
  F1WaitRecordReachDeliveryWon.cfg NoReachDeliveryWon r3 || rc=1
expect_fail "ReachReuse" F1WaitRecord \
  F1WaitRecordReachReuse.cfg NoReachReuse r4 || rc=1
expect_fail "ReachStaleDropped" F1WaitRecord \
  F1WaitRecordReachStaleDropped.cfg NoReachStaleDropped r5 || rc=1

echo
if [[ "$rc" -eq 0 ]]; then echo "=== PASS ==="; else echo "=== FAIL ==="; fi
exit "$rc"
