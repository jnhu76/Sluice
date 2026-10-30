#!/usr/bin/env bash
# #593 Phase C — mechanism counter census on the observe binary.
# One case per process, SLUICE_OBSERVE=1, stats on, 2 warmup + 7 measured,
# taskset -c 2. Counters aggregate the whole process (warmup+measured x
# 3 phase passes); denominator = records x 3 x request_count.
set -uo pipefail

obs_wt=/home/jnhu/sluice-593-research/worktrees/observe
out=/home/jnhu/sluice-593-research/evidence/attribution/census
mkdir -p "$out"
cd "$obs_wt"

case_id="${1:?case id}"
target="${2:?bench_w02_pipeline|bench_w03_external_loop}"
tag=$(echo "$case_id" | tr 'A-Z-' 'a-z_')

env SLUICE_OBSERVE=1 SLUICE_OBSERVE_OUT="$out/${tag}.obs" \
    SLUICE_BENCH_STATS=1 SLUICE_BENCH_CASE_FILTER="=$case_id" \
    SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7 \
    SLUICE_BENCH_JSONL="$out/${tag}.jsonl" \
    taskset -c 2 "./build-bench-ring/linux/x86_64/release/$target" \
    >"$out/${tag}.log" 2>&1
rc=$?
python3 bench/check_run.py "$out/${tag}.jsonl" --stage measured --exit-code "$rc" \
    --warmup 2 --measured 7 --expect-cases 1 || echo "CHECK_FAILED $tag"
echo "census done for $case_id"
