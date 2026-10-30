#!/usr/bin/env bash
# #593 K1 discriminating counterfactual — interleaved AB/BA pair runner.
# Arms: full (frozen baseline binary, perf authority) vs elide (k1-elision
# variant). One pair = one run per arm, 2 warmup + 7 measured, stats on,
# taskset -c 2, single-case filter. Arm order alternates per pair (AB/BA).
set -uo pipefail

full_bin=/home/jnhu/sluice-593-research/worktrees/baseline/build-bench-ring/linux/x86_64/release
eli_bin=/home/jnhu/sluice-593-research/worktrees/k1-elision/build-bench-ring/linux/x86_64/release
full_wt=/home/jnhu/sluice-593-research/worktrees/baseline
out=/home/jnhu/sluice-593-research/evidence/discrimination/k1

case_id="${1:?case id}"
target="${2:?bench_w02_pipeline|bench_w03_external_loop}"
pairs="${3:-15}"
mode="${4:-elide-vs-full}"   # elide-vs-full | aa-full
tag=$(echo "$case_id" | tr 'A-Z-' 'a-z_')
run_dir="$out/pairs-$tag-$mode"
mkdir -p "$run_dir"

run_arm() {
    local arm="$1" idx="$2" bin="$3" write_as="${4:-$1}"
    env SLUICE_BENCH_STATS=1 SLUICE_BENCH_CASE_FILTER="=$case_id" \
        SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7 \
        SLUICE_BENCH_JSONL="$run_dir/${write_as}-${idx}.jsonl" \
        taskset -c 2 "$bin/$target" >"$run_dir/${write_as}-${idx}.log" 2>&1
    local rc=$?
    python3 "$full_wt/bench/check_run.py" "$run_dir/${write_as}-${idx}.jsonl" \
        --stage measured --exit-code "$rc" --warmup 2 --measured 7 \
        --expect-cases 1 >"$run_dir/${write_as}-${idx}.check" 2>&1 \
        || echo "CHECK_FAILED ${write_as}-${idx}"
}

i=0
while [ "$i" -lt "$pairs" ]; do
    if [ "$mode" = "aa-full" ]; then
        if [ $((i % 2)) -eq 0 ]; then
            run_arm full "$i" "$full_bin" full
            run_arm full "$i" "$full_bin" full2
        else
            run_arm full "$i" "$full_bin" full2
            run_arm full "$i" "$full_bin" full
        fi
    elif [ $((i % 2)) -eq 0 ]; then
        run_arm full "$i" "$full_bin"
        run_arm elide "$i" "$eli_bin"
    else
        run_arm elide "$i" "$eli_bin"
        run_arm full "$i" "$full_bin"
    fi
    i=$((i + 1))
done
echo "pairs done: $case_id $mode ($pairs pairs)"
