#!/usr/bin/env bash
# verify-request-arena-negative-compile.sh
#
# Phase B (ADR-explicit-io-request-contract, Accepted) — negative-compile gate
# for the bounded RequestSlot arena.
#
# Verifies that the slot-lifecycle authority is compile-enforced: ordinary
# application code cannot directly mutate a RequestSlot's state, generation,
# enqueue-in-flight pin, terminal result, or waiter registration. Every
# transition is owned by RequestArena under the leaf slot-lifecycle mutex
# (ADR Decision 3 / Decision 5); the mutating fields are private (friend
# RequestArena only).
#
# Each NEG_<KIND> macro in the probe source selects one forbidden field write;
# the script asserts that compiling the probe with that macro defined FAILS with
# a private-access / inaccessible diagnostic.
#
# Usage:
#   scripts/verify-request-arena-negative-compile.sh
#
# Exit codes:
#   0 — all negative cases failed to compile (authority enforced)
#   1 — at least one negative case compiled or failed for the wrong reason
#   2 — harness error (missing probe, positive control failed)
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

probe="$repo_root/tests/request_arena_negative_compile_probe.cpp"

cxx_bin=${CXX:-c++}
tmp_root=$(mktemp -d "${TMPDIR:-/tmp}/sluice-arena-neg-compile.XXXXXX")
trap 'rm -rf -- "$tmp_root"' EXIT

if [[ ! -f "$probe" ]]; then
  printf 'FATAL: probe source not found: %s\n' "$probe" >&2
  exit 2
fi

common_flags=(
  -std=c++20
  -Wall
  -Werror
  -fsyntax-only
  -I"$repo_root/include"
)

# Each negative case is "NEG_MACRO:diagnostic_regex". The regex is matched
# (case-insensitive) against the compiler output. All cases here are
# private-field-writes on RequestSlot, so they all expect a private-access /
# inaccessible diagnostic. Kept as separate entries so each forbidden field is
# asserted independently (a regression that makes ONE field public is caught
# specifically, not masked).
negative_cases=(
  "NEG_SLOT_STATE_PRIVATE:private|declared private|inaccessible|not accessible"
  "NEG_SLOT_GENERATION_PRIVATE:private|declared private|inaccessible|not accessible"
  "NEG_SLOT_PIN_PRIVATE:private|declared private|inaccessible|not accessible"
  "NEG_SLOT_TERMINAL_PRIVATE:private|declared private|inaccessible|not accessible"
  "NEG_SLOT_REGISTRATION_PRIVATE:private|declared private|inaccessible|not accessible"
)

echo "=== Phase B RequestArena negative-compile gate ==="
echo
printf 'PROBE=%s\n' "$probe"
printf 'CXX=%s\n' "$cxx_bin"
echo

# --- Positive control: compile with NO NEG_* macro. Must SUCCEED. ---
log="$tmp_root/positive.log"
if "$cxx_bin" "${common_flags[@]}" "$probe" >"$log" 2>&1; then
  echo "POSITIVE CONTROL: PASS (probe compiles with no NEG_* macro)"
else
  echo "POSITIVE CONTROL: FAIL (probe did not compile even with no NEG_* macro)"
  sed -n '1,20p' "$log"
  exit 2
fi
echo

# --- Negative cases: each must FAIL to compile with its EXPECTED diagnostic. ---
failures=0
for entry in "${negative_cases[@]}"; do
  macro="${entry%%:*}"
  pattern="${entry#*:}"
  log="$tmp_root/$macro.log"
  if "$cxx_bin" "${common_flags[@]}" -D"$macro" "$probe" >"$log" 2>&1; then
    echo "$macro: FAIL (compiled successfully — field is public, authority NOT enforced)"
    ((failures++))
  else
    if grep -qiE "$pattern" "$log"; then
      echo "$macro: PASS (compile rejected with expected diagnostic)"
    else
      echo "$macro: FAIL (compile failed, but not with the expected diagnostic)"
      echo "  expected pattern: $pattern"
      sed -n '1,12p' "$log"
      ((failures++))
    fi
  fi
done

echo
if ((failures > 0)); then
  echo "RESULT: FAIL ($failures negative case(s) did not enforce slot authority)"
  exit 1
fi
echo "RESULT: PASS (all ${#negative_cases[@]} negative cases enforce slot authority)"
exit 0
