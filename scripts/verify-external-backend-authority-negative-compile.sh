#!/usr/bin/env bash
# verify-external-backend-authority-negative-compile.sh
#
# Phase C1 negative-compile gate — AsyncBackend protected-helper authority.
#
# NARROW SCOPE (per review): the existing completion-authority and
# request-arena negative-compile gates already prove that ordinary code
# cannot publish/claim via Completion<T> privates and cannot mutate
# RequestSlot private fields. This gate closes ONLY the narrower gap that the
# AsyncBackend PROTECTED publication helpers (try_claim / publish) are
# inaccessible to NON-DERIVED ordinary code. A class that does not inherit
# from AsyncBackend must not be able to call them.
#
# Each NEG_<KIND> macro in the probe source selects one forbidden usage; the
# script asserts that compiling the probe with that macro defined FAILS with
# the EXPECTED diagnostic (private / protected / inaccessible).
#
# Usage:
#   scripts/verify-external-backend-authority-negative-compile.sh
#
# Exit codes:
#   0 — all negative cases failed to compile (authority enforced)
#   1 — at least one negative case compiled or failed for the wrong reason
#   2 — harness error (missing probe, positive control failed)
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

probe="$repo_root/tests/external_backend_authority_negative_probe.cpp"

cxx_bin=${CXX:-c++}
tmp_root=$(mktemp -d "${TMPDIR:-/tmp}/sluice-ext-backend-neg-compile.XXXXXX")
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
# (case-insensitive) against the compiler output. Both cases expect a
# private/protected/inaccessible diagnostic because the protected static
# helpers are unreachable from a non-derived class. Kept as separate entries
# so the two distinct access categories (try_claim vs publish) are asserted
# independently.
negative_cases=(
  "NEG_TRY_CLAIM_AS_NON_BACKEND:protected|private|declared private|inaccessible|not accessible"
  "NEG_PUBLISH_AS_NON_BACKEND:protected|private|declared private|inaccessible|not accessible"
)

echo "=== Phase C1 external-backend-authority negative-compile gate ==="
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
    echo "$macro: FAIL (compiled successfully — protected helper reachable from non-backend code)"
    failures=$((failures + 1))
  else
    if grep -qiE "$pattern" "$log"; then
      echo "$macro: PASS (compile rejected with expected diagnostic)"
    else
      echo "$macro: FAIL (compile failed, but not with the expected diagnostic)"
      echo "  expected pattern: $pattern"
      sed -n '1,12p' "$log"
      failures=$((failures + 1))
    fi
  fi
done

echo
if ((failures > 0)); then
  echo "RESULT: FAIL ($failures negative case(s) did not enforce backend-helper authority)"
  exit 1
fi
echo "RESULT: PASS (all ${#negative_cases[@]} negative cases enforce backend-helper authority)"
exit 0
