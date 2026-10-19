#!/usr/bin/env bash
# Reproduces the recorded M5 (signal-before-enqueue) deterministic kill from
# docs/evidence/issue-553-m5-deterministic-oracle-20261019/README.md on the
# commit that evidence was recorded against. Builds a pristine control and
# the M5 mutant from a throwaway git-archive stage, runs each once, and
# verifies the exact r4b causal signature. The repository itself is never
# modified; all artifacts stay in a private temp directory.
#
# Success = pristine control passes the suite AND the mutant exits 1 with the
# unforced r4 pass, the park-boundary signature and the named r4b FAIL line.
# Anything else (including a patch that no longer applies because
# threadpool_backend.cpp moved on) is a failure: this reproduces a frozen
# record, it is not a standing test.
set -euo pipefail

REPO_ROOT=${1:-$(cd "$(dirname "$0")/../../.." && pwd)}
HERE=$(cd "$(dirname "$0")" && pwd)
PATCH_FILE="$HERE/m5.diff"
PATCH_SHA256=1f74a027ce00cde6e3b26a6ac880e93b3716279b19be2c4b3e86bf0f5509639d

fail() { echo "reproduce-m5-kill: $*" >&2; exit 1; }

command -v g++ >/dev/null 2>&1 || fail "g++ not found"
[ -d "$REPO_ROOT/.git" ] || fail "$REPO_ROOT is not a git repository"

actual_sha=$(sha256sum "$PATCH_FILE" | cut -d' ' -f1)
[ "$actual_sha" = "$PATCH_SHA256" ] ||
    fail "m5.diff sha256 $actual_sha does not match the recorded $PATCH_SHA256"

TMP=$(mktemp -d /tmp/sluice-m5-repro.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/src"
git -C "$REPO_ROOT" archive HEAD | tar -x -C "$TMP/src"

SOURCES="tests/threadpool_progress_race_test.cpp src/async/threadpool_backend.cpp \
src/async/async_io_context.cpp src/async/fail_fast.cpp src/async/request_scope.cpp \
src/async/detail/context_identity.cpp src/async/detail/request_core.cpp \
src/blocking_file.cpp src/file_resource.cpp"
FLAGS=(-std=c++20 -O2 -g -pthread -DSLUICE_ASYNC_INTERNAL_TESTING
       -I"$TMP/src/include" -I"$TMP/src/src/async")

( cd "$TMP/src" && g++ "${FLAGS[@]}" $SOURCES -o "$TMP/pristine" ) ||
    fail "pristine build failed"

patch -d "$TMP/src" -p1 --silent < "$PATCH_FILE" ||
    fail "m5.diff no longer applies; the evidence is pinned to its recorded commit"

( cd "$TMP/src" && g++ "${FLAGS[@]}" $SOURCES -o "$TMP/m5" ) ||
    fail "mutant build failed"

"$TMP/pristine" >"$TMP/pristine.log" 2>&1 ||
    fail "pristine control failed (recipe or environment broken): $(tail -1 "$TMP/pristine.log")"
grep -q "all .* threadpool progress race tests passed" "$TMP/pristine.log" ||
    fail "pristine control did not pass the suite"

set +e
"$TMP/m5" >"$TMP/m5.log" 2>&1
rc=$?
set -e
[ "$rc" -eq 1 ] || fail "mutant exit code $rc, expected 1"

grep -q "^ok r4_worker_completion_signal_before_final_recheck_is_caught$" "$TMP/m5.log" ||
    fail "unforced r4 did not pass inside the mutant run"
grep -q "prepark=1 publication_pending=0 token 0/0 -> 1/0" "$TMP/m5.log" ||
    fail "park-boundary signature missing"
grep -q "^r4b: cleanup publication ok$" "$TMP/m5.log" ||
    fail "cleanup line missing"
grep -q "^FAIL: r4b_worker_held_past_completion_signal_is_serviced_without_parking$" "$TMP/m5.log" ||
    fail "r4b FAIL line missing"

echo "REPRODUCTION_OK (pristine suite green; M5 killed at r4b with the recorded signature)"
