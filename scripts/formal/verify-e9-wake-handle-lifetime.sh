#!/usr/bin/env bash
# verify-e9-wake-handle-lifetime.sh — E9 wake-handle lifetime formal gate.
#
#   E9WakeHandleLifetime (safety)      -> all invariants PASS
#   E9WakeHandleLifetimeLiveness       -> liveness PASS
#   BuggySnapshot                      -> counterexample
#
# Source-safe: TLC runs in an isolated mktemp workspace.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/e9_wake_handle_lifetime"
workers="${TLC_WORKERS:-1}"

# Resolve jar via shared helper (sets TLA2TOOLS_JAR). Does NOT fall back to
# the repo-root jar, which is not checksum-verified; use bootstrap.py instead.
source "$here/resolve-jar.sh"
jar="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

outroot="$(mktemp -d -t e9-wh.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/e9-wh.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/e9-wh.* ]]; then
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

expect_cex() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then echo "FAIL  $label (no launch)"; tail -20 "$out"; return 1; fi
  if passed "$out"; then echo "FAIL  $label (expected counterexample, PASSED)"; return 1; fi
  echo "CEX   $label (counterexample found)"; return 0
}

rc=0
echo "=== E9 Wake-Handle Lifetime formal gate (workers=$workers) ==="
expect_pass "E9WakeHandleLifetime [safety]" E9WakeHandleLifetime \
  E9WakeHandleLifetime.cfg safety || rc=1
expect_pass "E9WakeHandleLifetime [liveness]" E9WakeHandleLifetime \
  E9WakeHandleLifetimeLiveness.cfg liveness || rc=1
expect_cex "BuggySnapshot" E9WakeHandleLifetimeBuggySnapshot \
  E9WakeHandleLifetimeBuggySnapshot.cfg buggy || rc=1

echo
if [[ "$rc" -eq 0 ]]; then echo "=== PASS ==="; else echo "=== FAIL ==="; fi
exit "$rc"
