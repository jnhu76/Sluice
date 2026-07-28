#!/usr/bin/env bash
# verify-async-rwlock.sh -- E12-F AsyncRwLock TLA+ / TLC formal gate.
#
# Runs the correct E12 RwLock safety model and the negative model through TLC:
#
#   E12RwLock (correct)           -> 10 named invariants PASS
#   E12RwLockNegReaderBypass      -> NoReaderBarging FAILS (reader barging bug)
#
# Source-safe: TLC runs in an isolated mktemp workspace.
#
# Usage:
#   scripts/formal/verify-async-rwlock.sh
#   TLA2TOOLS_JAR=/opt/tla2tools.jar scripts/formal/verify-async-rwlock.sh
#
# Exit status: 0 iff the correct model passes AND the negative model produces
# a counterexample for the expected property.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/spec/tla/e12_rwlock"
WORKERS="${TLC_WORKERS:-auto}"

# Resolve jar via shared helper (sets TLA2TOOLS_JAR).
source "$here/resolve-jar.sh"
JAR="$TLA2TOOLS_JAR"

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2
  exit 2
fi
if [ ! -d "$spec" ]; then
  echo "error: spec dir $spec missing" >&2
  exit 2
fi

outroot="$(mktemp -d -t sluice-formal.e12-rwlock.XXXXXX)"
cleanup() {
  if [[ -n "$outroot" ]] && [[ "$outroot" == *sluice-formal.e12-rwlock.* ]]; then
    rm -rf -- "$outroot"
  fi
}
trap cleanup EXIT

workdir="$outroot/work"
mkdir -p "$workdir"
cp "$spec"/*.tla "$spec"/*.cfg "$workdir/"
cd "$workdir"

run() {
  local model="$1" cfg="$2" outfile="$3"
  local metadir="$outroot/meta-$model"
  mkdir -p "$metadir"
  java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -nowarning \
       -config "$cfg" -cleanup -workers "$WORKERS" \
       -metadir "$metadir" "$model" >"$outfile" 2>&1
  return 0
}

tlc_launched() { grep -q '^Starting\.\.\.' "$1"; }
tlc_passed() { grep -q 'Model checking completed. No error has been found' "$1"; }
tlc_deadlocked() { grep -qiE '^(Error: )*Deadlock reached|is deadlocked' "$1"; }
named_violation() { grep -Eq "Invariant $2 is violated" "$1"; }
states_line() { grep -m1 'states generated' "$1" || true; }

expect_pass() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/${tag}.out"
  run "$model" "$cfg" "$out"
  if ! tlc_launched "$out"; then
    echo "FAIL  $label (TLC did not launch)"
    tail -20 "$out"
    return 1
  fi
  if tlc_passed "$out"; then
    echo "PASS  $label  ($(states_line "$out"))"
    return 0
  fi
  echo "FAIL  $label (expected PASS, got violation)"
  tail -20 "$out"
  return 1
}

expect_fail() {
  local label="$1" model="$2" cfg="$3" expected="$4" tag="$5"
  local out="$outroot/${tag}.out"
  run "$model" "$cfg" "$out"
  if ! tlc_launched "$out"; then
    echo "FAIL  $label (TLC did not launch)"
    tail -20 "$out"
    return 1
  fi
  if tlc_deadlocked "$out"; then
    echo "FAIL  $label (expected $expected violation, got DEADLOCK)"
    tail -12 "$out"
    return 1
  fi
  if tlc_passed "$out"; then
    echo "FAIL  $label (expected $expected violation, model PASSED)"
    return 1
  fi
  if ! named_violation "$out" "$expected"; then
    echo "FAIL  $label (expected property $expected NOT the violation)"
    grep -m1 -E 'Invariant .+ is violated' "$out" || true
    tail -8 "$out"
    return 1
  fi
  echo "CEX   $label ($expected violated, as expected)  ($(states_line "$out"))"
  return 0
}

echo "=== E12-F AsyncRwLock formal gate (TLC2, workers=$WORKERS) ==="
echo
rc=0

# Correct model: 10 invariants
expect_pass "E12RwLock [10 invariants]" \
            E12RwLock E12RwLock.cfg E12RwLock.safety || rc=1

# Negative: reader bypasses writer
expect_fail "NEG ReaderBypass" \
            E12RwLockNegReaderBypass E12RwLockNegReaderBypass.cfg \
            NoReaderBarging E12RwLockNeg1 || rc=1

echo
echo "=== gate ${rc}-ed (0 = all expected verdicts) ==="
exit "$rc"
