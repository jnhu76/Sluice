#!/usr/bin/env bash
# P5 A+B four-configuration correctness matrix (P5_INTERACTION_PLAN.md §2/§4
# S0; the P1 §8 commands pinned by the P2 plan §2.5). Runs inside the stacked
# worktree; exit 0 only if every leg configures, builds, and reports 0 failed
# tests. Records the tree head and timestamps every archive.
set -uo pipefail

WT=/home/jnhu/Source/Sluice-direct-compose-stack-readpreval-writeexit
RAW=/home/jnhu/Source/Sluice/research/direct-beyond-parity/p5-raw/P5AB
XMAKE=${XMAKE:-xmake}

cd "$WT" || { echo "matrix: cannot cd $WT" >&2; exit 1; }
mkdir -p "$RAW"

STAMP=$(date +%Y%m%dT%H%M%S)
RUNLOG="$RAW/matrix-$STAMP"
mkdir -p "$RUNLOG"
git rev-parse HEAD >"$RUNLOG/matrix-head.txt" 2>&1 || true

failed=0
for m in release debug asan asanubsan; do
    log="$RUNLOG/matrix-$m.txt"
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
    sha256sum "$WT/build/linux/x86_64/release/libsluice_core.a" \
        >"$RAW/matrix-release-archive-sha256-$STAMP.txt" 2>&1 || true
    echo "matrix: all four legs green (record: $RUNLOG)"
    exit 0
fi
echo "matrix: FAILED" >&2
exit 1
