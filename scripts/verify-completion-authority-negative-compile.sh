#!/usr/bin/env bash
# verify-completion-authority-negative-compile.sh
#
# ADR-explicit-io-completion-authority — negative-compile gate.
#
# Verifies that the Completion<T> publication authority is compile-enforced:
# ordinary application code cannot call mark_outstanding(), complete_with(),
# try_claim_for_backend(), publish_from_reap(), or reap_seq().
#
# Each NEG_<KIND> macro in the probe source selects one forbidden usage; the
# script asserts that compiling the probe with that macro defined FAILS with a
# private-access or no-member diagnostic.
#
# Usage:
#   scripts/verify-completion-authority-negative-compile.sh
#
# Exit codes:
#   0 — all negative cases failed to compile (authority enforced)
#   1 — at least one negative case compiled (authority regressed)
#   2 — harness error (missing probe, positive control failed)
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

probe="$repo_root/tests/completion_authority_negative_compile_probe.cpp"

cxx_bin=${CXX:-c++}
tmp_root=$(mktemp -d "${TMPDIR:-/tmp}/sluice-completion-neg-compile.XXXXXX")
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

negative_macros=(
  NEG_MARK_OUTSTANDING
  NEG_COMPLETE_WITH
  NEG_TRY_CLAIM_PRIVATE
  NEG_PUBLISH_PRIVATE
  NEG_REAP_SEQ_PRIVATE
)

echo "=== ADR-explicit-io-completion-authority negative-compile gate ==="
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

# --- Negative cases: each must FAIL to compile. ---
failures=0
for macro in "${negative_macros[@]}"; do
  log="$tmp_root/$macro.log"
  if "$cxx_bin" "${common_flags[@]}" -D"$macro" "$probe" >"$log" 2>&1; then
    echo "$macro: FAIL (compiled successfully — authority NOT enforced)"
    ((failures++))
  else
    # Verify the failure is for the RIGHT reason (private/no-member diagnostic).
    if grep -qiE 'private|no member|not accessible|inaccessible' "$log"; then
      echo "$macro: PASS (compile rejected with access/no-member diagnostic)"
    else
      echo "$macro: FAIL (compile failed, but not with an access diagnostic)"
      sed -n '1,10p' "$log"
      ((failures++))
    fi
  fi
done

echo
if ((failures > 0)); then
  echo "RESULT: FAIL ($failures negative case(s) did not enforce authority)"
  exit 1
fi
echo "RESULT: PASS (all ${#negative_macros[@]} negative cases enforce authority)"
exit 0
