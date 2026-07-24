#!/usr/bin/env bash
# verify-e12-rwlock-formal.sh -- E12-F AsyncRwLock TLA+ / TLC formal gate.
#
# Runs the correct E12 RwLock safety model and the negative model through TLC:
#
#   E12RwLock (correct)           -> 10 named invariants PASS
#   E12RwLockNegReaderBypass      -> NoReaderBarging FAILS (reader barging bug)
#
# Requires tla2tools.jar. By default uses $repo/tla2tools.jar; override with
# TLA2TOOLS_JAR=/path/to/tla2tools.jar.
#
# Usage:
#   scripts/verify-e12-rwlock-formal.sh
#   TLA2TOOLS_JAR=/opt/tla2tools.jar scripts/verify-e12-rwlock-formal.sh
#
# Exit status: 0 iff the correct model passes AND the negative model produces
# a counterexample for the expected property.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$here/.."
spec="$repo/docs/spec/e12_rwlock"
JAR="${TLA2TOOLS_JAR:-$repo/tla2tools.jar}"
WORKERS="${TLC_WORKERS:-auto}"

if [ ! -f "$JAR" ]; then
  echo "error: tla2tools.jar not found at $JAR" >&2
  echo "  set TLA2TOOLS_JAR=/path/to/tla2tools.jar" >&2
  exit 2
fi
if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2
  exit 2
fi
if [ ! -d "$spec" ]; then
  echo "error: spec dir $spec missing" >&2
  exit 2
fi

cd "$spec"

outroot="$(mktemp -d -t e12rwlock-tlc.XXXXXX)"
cleanup() { rm -rf "$outroot"; }
trap cleanup EXIT

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

echo "=== E12-F AsyncRwLock formal gate (TLC2, jar=$JAR; workers=$WORKERS) ==="
echo
rc=0

# Correct model: 10 invariants
expect_pass "E12RwLock [10 invariants]" \
            E12RwLock E12RwLock.cfg E12RwLock.safety || rc=1

# Negative: reader bypasses writer
expect_fail "NEG ReaderBypass" \
            E12RwLockNegReaderBypass E12RwLockNegReaderBypass.cfg \
            WriterFairness E12RwLockNeg1 || rc=1

echo
echo "=== gate ${rc}-ed (0 = all expected verdicts) ==="
exit "$rc"
