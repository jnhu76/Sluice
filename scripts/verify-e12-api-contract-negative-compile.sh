#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
probe="$repo_root/tests/e12_api_contract_probes.cpp"
cxx_bin=${CXX:-c++}
tmp_root=$(mktemp -d "${TMPDIR:-/tmp}/sluice-e12-neg-compile.XXXXXX")
trap 'rm -rf -- "$tmp_root"' EXIT

common_flags=(
  -std=c++20
  -Wall
  -Werror
  -fsyntax-only
  -I"$repo_root/include"
)

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

printf 'CXX=%s\n' "$cxx_bin"
printf 'POSITIVE_COMMAND='
printf '%q ' "$cxx_bin" "${common_flags[@]}" "$probe"
printf '\n'
"$cxx_bin" "${common_flags[@]}" "$probe"
printf 'POSITIVE_EXIT=0 CLASSIFICATION=PASS\n'

failures=0
for macro in "${negative_macros[@]}"; do
  log="$tmp_root/$macro.log"
  printf 'NEGATIVE_COMMAND='
  printf '%q ' "$cxx_bin" "${common_flags[@]}" "-D$macro" "$probe"
  printf '\n'
  if "$cxx_bin" "${common_flags[@]}" "-D$macro" "$probe" >"$log" 2>&1; then
    printf '%s EXIT=0 CLASSIFICATION=FAIL — COMPILE UNEXPECTEDLY SUCCEEDED\n' "$macro"
    failures=$((failures + 1))
  elif ! grep -q 'deleted' "$log"; then
    printf '%s EXIT=nonzero CLASSIFICATION=FAIL — WRONG FAILURE REASON (no deleted-member diagnostic)\n' "$macro"
    sed -n '1,20p' "$log"
    failures=$((failures + 1))
  else
    printf '%s EXIT=nonzero CLASSIFICATION=PASS — deleted-member diagnostic confirmed\n' "$macro"
  fi
done

printf 'NEGATIVE_CASES=%d FAILURES=%d\n' "${#negative_macros[@]}" "$failures"
if ((failures != 0)); then
  exit 1
fi
