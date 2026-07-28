#!/usr/bin/env bash
# verify-e7-publication.sh — E7 runnable-publication protocol formal gate.
#
#   E7Correct  -> all invariants PASS (Inv1-Inv8)
#   E7Buggy    -> counterexample: InvDoneNoTicket violated
#
# Source-safe: TLC runs in an isolated mktemp workspace.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/e7_publication"
jar="${TLA2TOOLS_JAR:-$repo/tla2tools.jar}"
workers="${TLC_WORKERS:-1}"

if [[ ! -f "$jar" ]]; then
  echo "error: tla2tools.jar not found at $jar" >&2
  echo "  set TLA2TOOLS_JAR=/path/to/tla2tools.jar" >&2
  exit 2
fi
if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2
  exit 2
fi

outroot="$(mktemp -d -t e7-pub.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/e7-pub.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/e7-pub.* ]]; then
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
  if ! launched "$out"; then
    echo "FAIL  $label (TLC did not launch)"; tail -20 "$out"; return 1
  fi
  if passed "$out"; then
    echo "PASS  $label"; return 0
  fi
  echo "FAIL  $label (expected PASS)"; tail -20 "$out"; return 1
}

expect_fail() {
  local label="$1" model="$2" cfg="$3" expected="$4" tag="$5"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out"; then
    echo "FAIL  $label (TLC did not launch)"; tail -20 "$out"; return 1
  fi
  if passed "$out"; then
    echo "FAIL  $label (expected $expected violation, model PASSED)"; return 1
  fi
  if ! named_violation "$out" "$expected"; then
    echo "FAIL  $label (expected $expected NOT the violation)"; tail -8 "$out"; return 1
  fi
  echo "CEX   $label ($expected violated, as expected)"; return 0
}

rc=0
echo "=== E7 Publication formal gate (workers=$workers) ==="
expect_pass "E7Correct" E7Publication E7Correct.cfg correct || rc=1
expect_fail "E7Buggy (duplicate publication)" E7Buggy E7Buggy.cfg \
  InvDoneNoTicket buggy || rc=1

echo
if [[ "$rc" -eq 0 ]]; then
  echo "=== PASS: E7 publication protocol ==="
else
  echo "=== FAIL: E7 publication protocol ==="
fi
exit "$rc"
