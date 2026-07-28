#!/usr/bin/env bash
# verify-e10-waitnode.sh — E10 WaitNode formal gate.
#
#   E10WaitNode (safety)          -> all invariants PASS
#   E10WaitNodeLiveness           -> liveness PASS
#   BuggyNoWinner                 -> InvNoDoubleCompletion violated
#
# Source-safe: TLC runs in an isolated mktemp workspace.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/e10_waitnode"
workers="${TLC_WORKERS:-1}"

# Resolve jar via shared helper (sets TLA2TOOLS_JAR).
source "$here/resolve-jar.sh"
jar="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

outroot="$(mktemp -d -t sluice-formal.e10-wn.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] && [[ "$outroot" == *sluice-formal.e10-wn.* ]]; then
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
named_violation() { grep -Eq "Invariant $2 is violated" "$1"; }

expect_pass() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "PASS  $label"; return 0; fi
  echo "FAIL  $label (expected PASS)"; tail -20 "$out"; return 1
}

expect_fail() {
  local label="$1" model="$2" cfg="$3" expected="$4" tag="$5"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "FAIL  $label (expected $expected, PASSED)"; return 1; fi
  if ! named_violation "$out" "$expected"; then
    echo "FAIL  $label (expected $expected NOT the violation)"; tail -8 "$out"; return 1
  fi
  echo "CEX   $label ($expected violated)"; return 0
}

rc=0
echo "=== E10 WaitNode formal gate (workers=$workers) ==="

# Run the correct safety model.
expect_pass "E10WaitNode [safety]" E10WaitNode E10WaitNode.cfg safety || rc=1

# Run the correct liveness model.
expect_pass "E10WaitNode [liveness]" E10WaitNode E10WaitNodeLiveness.cfg liveness || rc=1

# Run the buggy model — must violate InvNoDoubleCompletion.
expect_fail "BuggyNoWinner" E10WaitNodeBuggyNoWinner \
  E10WaitNodeBuggyNoWinner.cfg InvNoDoubleCompletion buggy || rc=1

echo
if [[ "$rc" -eq 0 ]]; then echo "=== PASS ==="; else echo "=== FAIL ==="; fi
exit "$rc"
