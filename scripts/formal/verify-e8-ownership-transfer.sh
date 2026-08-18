#!/usr/bin/env bash
# verify-e8-ownership-transfer.sh — E8 ownership transfer formal gate.
#
#   E8OwnershipTransfer            -> all invariants PASS
#   E8OwnershipTransferBuggyOwner  -> counterexample (Inv or refinement violated)
#
# Source-safe: TLC runs in an isolated mktemp workspace.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/e8_ownership_transfer"
workers="${TLC_WORKERS:-1}"

# Resolve jar via shared helper (sets TLA2TOOLS_JAR). Does NOT fall back to
# the repo-root jar, which is not checksum-verified; use bootstrap.py instead.
source "$here/resolve-jar.sh"
jar="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

outroot="$(mktemp -d -t e8-own.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/e8-own.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/e8-own.* ]]; then
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

expect_pass() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "PASS  $label"; return 0; fi
  echo "FAIL  $label (expected PASS)"; tail -20 "$out"; return 1
}

# BuggyOwner: the single-line StealRunnableBuggy mutation (ownerRecord not
# transferred with the ticket) must violate the named steal-consistency
# invariant InvLocalMatchesOwner — NOT just "some invariant". A different
# regression surfacing another counterexample would otherwise still pass this
# gate (formal-audit finding: the manifest claims a named-invariant gate).
named_violation() { grep -Eq "Invariant $2 is violated" "$1"; }

expect_fail() {
  local label="$1" model="$2" cfg="$3" expected="$4" tag="$5"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "FAIL  $label (expected $expected violation, model passed)"; return 1; fi
  if ! named_violation "$out" "$expected"; then
    echo "FAIL  $label (expected $expected, got another failure)"
    tail -12 "$out"
    return 1
  fi
  echo "CEX   $label ($expected violated, as expected)"
}

rc=0
echo "=== E8 Ownership Transfer formal gate (workers=$workers) ==="
expect_pass "E8OwnershipTransfer" E8OwnershipTransfer \
  E8OwnershipTransfer.cfg correct || rc=1
expect_fail "E8OwnershipTransferBuggyOwner" E8OwnershipTransferBuggyOwner \
  E8OwnershipTransferBuggyOwner.cfg InvLocalMatchesOwner buggy || rc=1

echo
if [[ "$rc" -eq 0 ]]; then echo "=== PASS ==="; else echo "=== FAIL ==="; fi
exit "$rc"
