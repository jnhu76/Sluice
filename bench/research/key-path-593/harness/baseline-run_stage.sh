#!/usr/bin/env bash
# #593 baseline stage runner: executes the frozen G0 matrix stages via the
# repo's bench/run_matrix.sh from the frozen worktree, pinned to one CPU, with
# optional stats wiring and fixture location. Every run is validated by
# bench/check_run.py inside run_matrix.sh; this wrapper only adds observation
# (affinity, cpu MHz, loadavg) around the frozen harness and never alters it.
set -euo pipefail

wt=/home/jnhu/sluice-593-research/worktrees/baseline
cd "$wt"

run="${1:?label e.g. r1}"
stats="${2:?1|0}"
stage="${3:-measured}"
suffix="${4:-$run}"
tmpdir="${5:-}"

out=/home/jnhu/sluice-593-research/evidence/baseline/$run
mkdir -p "$out"

observe() {
    {
        echo "[$(date -Is)] observe: $(uptime)"
        awk '/^processor/{p=$3} /^cpu MHz/{if(p==2) print "cpu2_MHz="$4}' /proc/cpuinfo
    } >> "$out/observe.log"
}

observe
export SLUICE_BENCH_STATS="$stats"
if [ -n "$tmpdir" ]; then
    mkdir -p "$tmpdir"
    export SLUICE_BENCH_TMP_DIR="$tmpdir"
else
    unset SLUICE_BENCH_TMP_DIR 2>/dev/null || true
fi
echo "run=$run stats=$stats stage=$stage suffix=$suffix tmpdir=${tmpdir:-<default /tmp/sluice-bench>} taskset=cpu2" \
    | tee -a "$out/observe.log"

rc=0
taskset -c 2 ./bench/run_matrix.sh "$out" 2 7 "$stage" "$suffix" || rc=$?
observe
echo "run_matrix rc=$rc" >> "$out/observe.log"
exit "$rc"
