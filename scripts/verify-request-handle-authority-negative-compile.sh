#!/usr/bin/env bash
# verify-request-handle-authority-negative-compile.sh
#
# ADR-public-request-handle — negative-compile authority gate.
#
# Verifies that the public RequestHandle is construction-controlled
# (non-forgeable): ordinary application code cannot manufacture a valid handle,
# read its private identity components (context/slot/generation), flip validity,
# convert an internal detail::RequestKey into a handle, or reach the sealed
# AsyncBackend identity seam (identity_of / request_handle_state / a concrete
# backend's resolve_identity_state) through a raw backend pointer. Each
# NEG_<KIND> macro in the probe selects one forbidden usage; the script asserts
# compiling the probe with that macro defined FAILS with the EXPECTED
# private-access / no-conversion diagnostic.
#
# Usage:
#   scripts/verify-request-handle-authority-negative-compile.sh
#
# Exit codes:
#   0 — all negative cases failed to compile (authority enforced)
#   1 — at least one negative case compiled or failed for the wrong reason
#   2 — harness error (missing probe, positive control failed)
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

probe="$repo_root/tests/request_handle_authority_negative_probe.cpp"

cxx_bin=${CXX:-c++}
tmp_root=$(mktemp -d "${TMPDIR:-/tmp}/sluice-request-handle-neg-compile.XXXXXX")
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

# Each negative case is "NEG_MACRO:diagnostic_regex". The identity ctor, the
# private members, and the sealed AsyncBackend seam are friend-AsyncIoContext /
# friend-AsyncBackend only; a public caller gets a private / inaccessible /
# no-conversion diagnostic.
negative_cases=(
  "NEG_FORGE_HANDLE_CTOR:private|inaccessible|no matching constructor|cannot be referenced"
  "NEG_READ_HANDLE_CONTEXT:private|inaccessible|no member|not a member"
  "NEG_READ_HANDLE_SLOT:private|inaccessible|no member|not a member"
  "NEG_READ_HANDLE_GENERATION:private|inaccessible|no member|not a member"
  "NEG_SET_HANDLE_VALID:private|inaccessible|no member|not a member"
  "NEG_CONVERT_REQUEST_KEY:no viable conversion|no matching constructor|conversion from.*RequestKey.*RequestHandle|private|inaccessible|cannot be referenced"
  "NEG_CALL_BACKEND_IDENTITY_OF:private|inaccessible|protected"
  "NEG_CALL_BACKEND_REQUEST_HANDLE_STATE:private|inaccessible|protected"
  "NEG_CALL_CONCRETE_RESOLVE_IDENTITY_STATE:private|inaccessible|protected"
)

echo "=== ADR-public-request-handle negative-compile gate ==="
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
    echo "$macro: FAIL (compiled successfully — authority NOT enforced)"
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
  echo "RESULT: FAIL ($failures negative case(s) did not enforce authority)"
  exit 1
fi
echo "RESULT: PASS (all ${#negative_cases[@]} negative cases enforce authority)"
exit 0
