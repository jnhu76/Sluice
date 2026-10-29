#!/usr/bin/env bash
# P2C1 four-configuration correctness matrix (P2 plan §2.5, P1 §8 commands).
# Runs inside the candidate worktree, executes the exact P1 matrix, and exits
# 0 only if every leg configures, builds, and reports 0 failed tests.
set -uo pipefail

WT=/home/jnhu/Source/Sluice-direct-read-compose-prevalidated-steps
RAW=/home/jnhu/Source/Sluice/research/direct-beyond-parity/p2-raw/P2C1
XMAKE=${XMAKE:-xmake}

cd "$WT" || { echo "matrix: cannot cd $WT" >&2; exit 1; }
mkdir -p "$RAW"

git rev-parse HEAD >"$RAW/matrix-head.txt" 2>&1 || true

failed=0
for m in release debug asan asanubsan; do
    log="$RAW/matrix-$m.txt"
    echo "=== leg $m ==="
    if ! $XMAKE f -c -m "$m" --liburing=n -y >"$log" 2>&1; then
        echo "matrix: leg $m configure FAILED (see $log)" >&2
        failed=1
        continue
    fi
    if ! $XMAKE -y >>"$log" 2>&1; then
        echo "matrix: leg $m build FAILED (see $log)" >&2
        failed=1
        continue
    fi
    if ! $XMAKE test -y >>"$log" 2>&1; then
        echo "matrix: leg $m test run FAILED (see $log)" >&2
        failed=1
        continue
    fi
    failedcount=$(sed 's/\x1b\[[0-9;]*m//g' "$log" | grep -oE '[0-9]+ test\(s\) failed' | tail -1 | grep -oE '^[0-9]+')
    if [[ "$failedcount" != "0" ]]; then
        echo "matrix: leg $m reported ${failedcount:-unparsed} failed tests (see $log)" >&2
        failed=1
        continue
    fi
    echo "leg $m: 0 failed tests"
done

if [[ "$failed" == "0" ]]; then
    sha256sum "$WT/build/linux/x86_64/release/libsluice_core.a" >"$RAW/matrix-release-archive-sha256.txt" 2>&1 || true
    echo "matrix: all four legs green"
    exit 0
fi
echo "matrix: FAILED" >&2
exit 1
