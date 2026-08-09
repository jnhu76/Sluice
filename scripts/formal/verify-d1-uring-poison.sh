#!/usr/bin/env bash
# Phase D1 io_uring poison/recovery formal gate. TLC always runs in an isolated
# temporary workspace; negative gates require the exact named invariant.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/d1_uring_poison"
workers="${TLC_WORKERS:-1}"

source "$here/resolve-jar.sh"
jar="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2
  exit 2
fi

outroot="$(mktemp -d -t d1-uring-poison.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/d1-uring-poison.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/d1-uring-poison.* ]]; then
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
  local rc_tlc=0
  mkdir -p "$metadir"
  java -XX:+UseParallelGC -cp "$jar" tlc2.TLC -nowarning \
       -workers "$workers" -metadir "$metadir" \
       -config "$cfg" D1UringPoison >"$outroot/$tag.out" 2>&1 || rc_tlc=$?
  printf '%s\n' "$rc_tlc" >"$outroot/$tag.rc"
}

launched() { grep -q '^Starting\.\.\.' "$1"; }
passed() { grep -q 'Model checking completed. No error has been found' "$1"; }
named_violation() { grep -Eq "Invariant $2 is violated" "$1"; }

expect_pass() {
  local label="$1" cfg="$2" tag="$3"
  local out="$outroot/$tag.out"
  local status_file="$outroot/$tag.rc"
  run_tlc "$cfg" "$tag"
  if ! launched "$out"; then
    echo "FAIL  $label (TLC did not launch; exit=$(<"$status_file"))"
    tail -20 "$out"
    return 1
  fi
  if passed "$out"; then
    echo "PASS  $label"; return 0
  fi
  echo "FAIL  $label (expected PASS; exit=$(<"$status_file"))"
  tail -20 "$out"
  return 1
}

expect_fail() {
  local label="$1" cfg="$2" expected="$3" tag="$4"
  local out="$outroot/$tag.out"
  local status_file="$outroot/$tag.rc"
  run_tlc "$cfg" "$tag"
  if ! launched "$out"; then
    echo "FAIL  $label (TLC did not launch; exit=$(<"$status_file"))"
    tail -20 "$out"
    return 1
  fi
  if passed "$out"; then
    echo "FAIL  $label (expected $expected violation, model passed)"; return 1
  fi
  if ! named_violation "$out" "$expected"; then
    echo "FAIL  $label (expected $expected, got another failure; exit=$(<"$status_file"))"
    tail -12 "$out"
    return 1
  fi
  echo "CEX   $label ($expected violated, as expected)"
}

rc=0
echo "=== D1 io_uring poison/recovery formal gate (workers=$workers) ==="
expect_pass "correct ledger/control protocol" D1UringPoison.cfg correct || rc=1
expect_fail "masked SQ slot used as identity" D1UringPoisonBuggyMasked.cfg \
  InvLogicalIdentity masked || rc=1
expect_fail "original terminal published before control retirement" \
  D1UringPoisonBuggyControl.cfg InvReadyControlQuiescent control || rc=1
expect_fail "transport submit issued after poison" \
  D1UringPoisonBuggyPostPoisonSubmit.cfg InvNoSubmitAfterPoison post-poison || rc=1

echo
if [[ "$rc" -eq 0 ]]; then
  echo "=== PASS: D1 io_uring poison/recovery protocol ==="
else
  echo "=== FAIL: D1 io_uring poison/recovery protocol ==="
fi
exit "$rc"
