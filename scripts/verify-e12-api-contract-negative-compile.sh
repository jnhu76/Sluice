#!/usr/bin/env bash
# verify-e12-api-contract-negative-compile.sh
#
# Verifies that the deleted/private API guards on the E10-E12 async
# synchronization primitives are compile-enforced. Each NEG_<KIND> macro in
# the probe source selects one forbidden usage (copy/move of a non-copyable /
# non-movable type, or copy of a Result<T> returned from a queue method); the
# script asserts that compiling the probe with that macro defined FAILS with a
# deleted-member (or private/deleted-access) diagnostic.
#
# E15-P2-03 corrective: the probe path was wrong (e12_api_contract_probes.cpp
# never existed; the NEG_* macros live in async_sync_api_contract_probe.cpp),
# so the script failed at the positive-control step before any negative case
# ran. This version:
#   - resolves the probe path explicitly and rejects a missing probe up front
#     (a distinct, reported failure mode, NOT masquerading as a compile error);
#   - runs a positive control (compile with NO NEG_* macro) and requires it to
#     succeed — proves the harness itself works before any negative case;
#   - classifies each negative case's compile failure as
#       PASS  — deleted-member / private-access diagnostic confirmed
#       FAIL  — compile unexpectedly SUCCEEDED (the guard regressed)
#       FAIL  — failed for the WRONG reason (no deleted/private diagnostic;
#               e.g. a missing include, an unrelated syntax error, source-missing)
#   - prints a concise per-case verdict and exits non-zero on any failure.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# E15-P2-03: the probe source. If you rename it, update this single line.
probe="$repo_root/tests/async_sync_api_contract_probe.cpp"

cxx_bin=${CXX:-c++}
tmp_root=$(mktemp -d "${TMPDIR:-/tmp}/sluice-e12-neg-compile.XXXXXX")
trap 'rm -rf -- "$tmp_root"' EXIT

# E15-P2-03: distinguish "source file missing" from a compile failure. The
# probe path is a harness precondition; a missing probe is reported and the
# script exits non-zero WITHOUT attempting any compile.
if [[ ! -f "$probe" ]]; then
  printf 'FATAL: probe source not found: %s\n' "$probe" >&2
  printf 'FATAL: set $probe in %s to the file that defines the NEG_* macros.\n' \
    "${BASH_SOURCE[0]}" >&2
  exit 2
fi

common_flags=(
  -std=c++20
  -Wall
  -Werror
  -fsyntax-only
  -I"$repo_root/include"
)

# Each NEG_<KIND> macro selects ONE forbidden usage in the probe. The probe
# source defines them under #ifdef; the script arms exactly one at a time.
negative_macros=(
  NEG_WAITNODE_COPY
  NEG_WAITNODE_MOVE
  NEG_EVENT_COPY
  NEG_SEMAPHORE_MOVE
  NEG_ASYNCMUTEX_COPY
  NEG_ASYNCCONDITION_MOVE
  NEG_QUEUE_COPY
  NEG_QUEUE_PUSH_RESULT_COPY
  NEG_QUEUE_POP_RESULT_COPY
)

printf 'PROBE=%s\n' "$probe"
printf 'CXX=%s\n' "$cxx_bin"

# --- Positive control: the harness (probe + flags) compiles with no NEG_* ---
# This must SUCCEED before any negative case is run, otherwise the negative
# results are meaningless (a broken harness can't validate anything).
printf 'POSITIVE_COMMAND='
printf '%q ' "$cxx_bin" "${common_flags[@]}" "$probe"
printf '\n'
if ! "$cxx_bin" "${common_flags[@]}" "$probe" >"$tmp_root/positive.log" 2>&1; then
  printf 'POSITIVE_EXIT=nonzero CLASSIFICATION=FAIL — control compile failed (harness broken):\n' >&2
  sed -n '1,20p' "$tmp_root/positive.log" >&2
  exit 2
fi
printf 'POSITIVE_EXIT=0 CLASSIFICATION=PASS — harness compiles with no NEG_* armed\n'

# --- Negative cases ---
failures=0
for macro in "${negative_macros[@]}"; do
  log="$tmp_root/$macro.log"
  printf 'NEGATIVE_COMMAND='
  printf '%q ' "$cxx_bin" "${common_flags[@]}" "-D$macro" "$probe"
  printf '\n'
  if "$cxx_bin" "${common_flags[@]}" "-D$macro" "$probe" >"$log" 2>&1; then
    printf '%s EXIT=0 CLASSIFICATION=FAIL — COMPILE UNEXPECTEDLY SUCCEEDED (deleted/private guard regressed)\n' "$macro"
    failures=$((failures + 1))
  elif grep -Eq 'deleted|private' "$log"; then
    printf '%s EXIT=nonzero CLASSIFICATION=PASS — deleted/private-member diagnostic confirmed\n' "$macro"
  else
    printf '%s EXIT=nonzero CLASSIFICATION=FAIL — WRONG FAILURE REASON (no deleted/private diagnostic)\n' "$macro"
    sed -n '1,12p' "$log"
    failures=$((failures + 1))
  fi
done

printf 'NEGATIVE_CASES=%d FAILURES=%d\n' "${#negative_macros[@]}" "$failures"
if ((failures != 0)); then
  exit 1
fi
