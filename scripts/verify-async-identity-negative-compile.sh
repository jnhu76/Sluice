#!/usr/bin/env bash
# verify-async-identity-negative-compile.sh
#
# E16-POST-MERGE-CORRECTIVE-1 (C2-T4) — installed-header authority audit for
# the Fiber-local Runtime execution-identity tag.
#
# Verifies that the WRITE path for the execution tag is NOT reachable by
# ordinary application code:
#   - Fiber::set_execution_tag         is PRIVATE (friend Scheduler only)
#   - Scheduler::set_current_fiber_execution_tag is PRIVATE
#                                          (friend ApplicationRuntime only)
#
# The probe (tests/async_identity_negative_compile_probe.cpp) defines a
# NEG_<KIND> macro per forbidden usage. This script:
#   - runs a positive control (compile with NO NEG_* macro) and requires it to
#     SUCCEED — proves the harness works before any negative case;
#   - classifies each negative case's compile failure as
#       PASS  — private/deleted-member diagnostic confirmed
#       FAIL  — compile unexpectedly SUCCEEDED (the write guard regressed)
#       FAIL  — failed for the WRONG reason (no private/deleted diagnostic)
#   - prints a concise per-case verdict and exits non-zero on any failure.
#
# This is a STRUCTURAL authority proof: it is the C2-T4 installed-header audit.
# The behavioral C2 cases (task cannot self-close, identity preserved across
# concurrent tasks) live in tests/application_runtime_identity_test.cpp.
set -euo pipefail

# Resolve the repository root. `dirname` must be applied to BASH_SOURCE itself
# BEFORE appending "/..": the earlier form `dirname "${BASH_SOURCE[0]}/.."`
# leaves the "/.." as the final component, so dirname returns the script path
# unchanged (the script FILE), `cd <scriptfile>` fails, and `repo_root` becomes
# empty — producing a probe path like "/tests/..." that never exists. Match the
# form used by scripts/verify-async-api-negative-compile.sh.
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# The probe source. If you rename it, update this single line.
probe="$repo_root/tests/async_identity_negative_compile_probe.cpp"

cxx_bin=${CXX:-c++}
tmp_root=$(mktemp -d "${TMPDIR:-/tmp}/sluice-identity-neg-compile.XXXXXX")
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

# Each NEG_<KIND> macro selects ONE forbidden usage in the probe.
negative_macros=(
  NEG_FIBER_SET_EXECUTION_TAG
  NEG_FIBER_SET_EXECUTION_TAG_VALUE
  NEG_SCHEDULER_SET_CURRENT_FIBER_TAG
)

echo "=== E16 C2 identity-authority negative-compile gate ==="
echo

# --- Positive control: compile with NO macro. Must SUCCEED. ---
log="$tmp_root/positive.log"
if "$cxx_bin" "${common_flags[@]}" "$probe" >"$log" 2>&1; then
  echo "POSITIVE CONTROL: PASS (probe compiles with no NEG_* macro)"
else
  echo "POSITIVE CONTROL: FAIL (probe did not compile even with no NEG_* macro)"
  sed -n '1,20p' "$log"
  exit 1
fi
echo

# --- Negative cases: each macro must make compilation FAIL with private/deleted. ---
failures=0
for macro in "${negative_macros[@]}"; do
  log="$tmp_root/$macro.log"
  if "$cxx_bin" "${common_flags[@]}" "-D$macro" "$probe" >"$log" 2>&1; then
    printf '%s EXIT=0 CLASSIFICATION=FAIL — COMPILE UNEXPECTEDLY SUCCEEDED (private write guard regressed)\n' "$macro"
    failures=$((failures + 1))
  elif grep -Eq 'private|protected|deleted|inaccessible' "$log"; then
    printf '%s EXIT=nonzero CLASSIFICATION=PASS — private/inaccessible-member diagnostic confirmed\n' "$macro"
  else
    printf '%s EXIT=nonzero CLASSIFICATION=FAIL — WRONG FAILURE REASON (no private/inaccessible diagnostic)\n' "$macro"
    sed -n '1,12p' "$log"
    failures=$((failures + 1))
  fi
done

echo
printf 'NEGATIVE_CASES=%d FAILURES=%d\n' "${#negative_macros[@]}" "$failures"
if ((failures != 0)); then
  echo "=== FAIL ==="
  exit 1
fi
echo "=== PASS ==="
