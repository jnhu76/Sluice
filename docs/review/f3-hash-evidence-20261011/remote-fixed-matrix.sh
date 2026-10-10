#!/usr/bin/env bash
set -eu
mode=$1
uring=$2
sha=14758dcdbe8072d6cfad45b1f187e61b26fe8bbc
root=$HOME/Source/sluice-f3-hash-14758dcd-$mode-$uring
logs=/tmp/sluice-f3-hash-evidence-20261011
exec > "$logs/candidate-fixed-$mode-$uring.log" 2>&1
trap 'rc=$?; printf "MATRIX_EXIT=%s\n" "$rc"' EXIT
set -x
cd "$root"
test "$(git rev-parse HEAD)" = "$sha"
test -z "$(git status --porcelain)"
xmake f -m "$mode" --liburing="$uring" -y
for target in sluice-hash app_hash_consumption_test blocking_file_read_test direct_cursor_position_test direct_w01_consumer_probe; do
    xmake build -j4 "$target"
done
for target in app_hash_consumption_test blocking_file_read_test direct_cursor_position_test direct_w01_consumer_probe; do
    xmake run "$target"
done
python3 tests/hash_cli_oracle.py "build/linux/x86_64/$mode/sluice-hash" --early-bounds
strace -f -e trace=clone,clone3 -o "$logs/threads-fixed-$mode-$uring-1.trace" "build/linux/x86_64/$mode/sluice-hash" --workers 1 AGENTS.md
strace -f -e trace=clone,clone3 -o "$logs/threads-fixed-$mode-$uring-2.trace" "build/linux/x86_64/$mode/sluice-hash" --workers 2 AGENTS.md
if grep -Eq 'clone(3)?\(' "$logs/threads-fixed-$mode-$uring-1.trace"; then exit 1; fi
grep -Eq 'clone(3)?\(' "$logs/threads-fixed-$mode-$uring-2.trace"
