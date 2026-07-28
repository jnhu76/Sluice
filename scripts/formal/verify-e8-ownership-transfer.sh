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
jar="${TLA2TOOLS_JAR:-$repo/tla2tools.jar}"
workers="${TLC_WORKERS:-1}"

if [[ ! -f "$jar" ]]; then
  echo "error: tla2tools.jar not found at $jar" >&2; exit 2
fi
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

# BuggyOwner: a counterexample is expected for one of the safety invariants.
# The cfg lists several invariants; TLC will report the first violated one.
expect_cex() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "FAIL  $label (expected counterexample, PASSED)"; return 1; fi
  echo "CEX   $label (counterexample found)"; return 0
}

rc=0
echo "=== E8 Ownership Transfer formal gate (workers=$workers) ==="
expect_pass "E8OwnershipTransfer" E8OwnershipTransfer \
  E8OwnershipTransfer.cfg correct || rc=1
expect_cex "E8OwnershipTransferBuggyOwner" E8OwnershipTransferBuggyOwner \
  E8OwnershipTransferBuggyOwner.cfg buggy || rc=1

echo
if [[ "$rc" -eq 0 ]]; then echo "=== PASS ==="; else echo "=== FAIL ==="; fi
exit "$rc"
