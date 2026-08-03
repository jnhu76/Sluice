#!/usr/bin/env bash
# verify-completion-authority-negative-compile.sh
#
# ADR-explicit-io-completion-authority — negative-compile gate.
#
# Verifies that the Completion<T> publication authority is compile-enforced:
# ordinary application code cannot call try_claim_for_backend(),
# publish_from_reap(), rollback_claim_before_accept(), or reap_seq(), and a
# value type that violates the Completion<T> noexcept value-type contract
# cannot instantiate Completion<T>.
#
# Each NEG_<KIND> macro in the probe source selects one forbidden usage; the
# script asserts that compiling the probe with that macro defined FAILS with the
# EXPECTED diagnostic for that case. Each case carries its own diagnostic
# pattern (name:regex) so that a failure is attributed to the right reason
# rather than masked by a single blanket regex.
#
#   - access-control cases expect a private-access / no-member / inaccessible
#     diagnostic;
#   - NEG_THROWING_COMPLETION_VALUE expects a static_assert / nothrow-trait
#     diagnostic.
#
# Usage:
#   scripts/verify-completion-authority-negative-compile.sh
#
# Exit codes:
#   0 — all negative cases failed to compile (authority enforced)
#   1 — at least one negative case compiled or failed for the wrong reason
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

# Each negative case is "NEG_MACRO:diagnostic_regex". The regex is matched
# (case-insensitive, via grep -qiE) against the compiler output to confirm the
# compile failed for the EXPECTED reason. Keep the patterns distinct per case so
# a wrong-reason failure is never masked by a blanket regex.
#
# Access-control cases: the publication mutators / reap_seq are private to
# Completion<T> (friend AsyncBackend / friend Batch only). A removed public
# method surfaces as a no-member diagnostic; a private one as private/inaccessible.
#
# Value-contract case: a throwing move-assign T triggers the static_assert traits
# declared in completion.hpp.
negative_cases=(
  "NEG_MARK_OUTSTANDING:no member|mark_outstanding|not a member|has not been declared"
  "NEG_COMPLETE_WITH:no member|complete_with|not a member|has not been declared"
  "NEG_TRY_CLAIM_PRIVATE:private|no member|not accessible|inaccessible"
  "NEG_PUBLISH_PRIVATE:private|no member|not accessible|inaccessible"
  "NEG_ROLLBACK_PRIVATE:private|no member|not accessible|inaccessible"
  "NEG_REAP_SEQ_PRIVATE:private|no member|not accessible|inaccessible"
  "NEG_THROWING_COMPLETION_VALUE:static assertion|static_assert|nothrow|nothrow_move_assignable"
  # Phase B binding-protocol authority (ADR-explicit-io-request-contract Decision 5):
  # ordinary application code cannot forge a binding / commit one / roll one back /
  # install or clear the slot-release capability (Decision 7 / I2).
  "NEG_BEGIN_BINDING_PRIVATE:private|no member|not accessible|inaccessible"
  "NEG_COMMIT_BINDING_PRIVATE:private|no member|not accessible|inaccessible"
  "NEG_ROLLBACK_BINDING_PRIVATE:private|no member|not accessible|inaccessible"
  "NEG_INSTALL_BINDING_PRIVATE:private|no member|not accessible|inaccessible"
  "NEG_CLEAR_BINDING_PRIVATE:private|no member|not accessible|inaccessible"
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

# --- Negative cases: each must FAIL to compile with its EXPECTED diagnostic. ---
failures=0
for entry in "${negative_cases[@]}"; do
  macro="${entry%%:*}"
  pattern="${entry#*:}"
  log="$tmp_root/$macro.log"
  if "$cxx_bin" "${common_flags[@]}" -D"$macro" "$probe" >"$log" 2>&1; then
    echo "$macro: FAIL (compiled successfully — authority/contract NOT enforced)"
    failures=$((failures + 1))
  else
    # Verify the failure is for the EXPECTED reason for THIS case.
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
  echo "RESULT: FAIL ($failures negative case(s) did not enforce authority/contract)"
  exit 1
fi
echo "RESULT: PASS (all ${#negative_cases[@]} negative cases enforce authority/contract)"
exit 0
