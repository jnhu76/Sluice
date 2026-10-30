#!/usr/bin/env bash
# #593 Phase C — instrumentation overhead measurement.
# Interleaved arms, one case per process, taskset -c 2, stats on, 2/7:
#   full        = frozen baseline ring binary (performance authority)
#   observe-off = observe-patch binary, SLUICE_OBSERVE unset
#   observe-on  = observe-patch binary, SLUICE_OBSERVE=1 (counters, no trace)
# Rounds rotate arm order per round to cancel drift.
set -uo pipefail

full_wt=/home/jnhu/sluice-593-research/worktrees/baseline
obs_wt=/home/jnhu/sluice-593-research/worktrees/observe
full_bin=$full_wt/build-bench-ring/linux/x86_64/release
obs_bin=$obs_wt/build-bench-ring/linux/x86_64/release
out=/home/jnhu/sluice-593-research/evidence/attribution
mkdir -p "$out/overhead"
cd "$full_wt"

case_id="${1:?case id}"
target="${2:?bench_w02_pipeline|bench_w03_external_loop}"
rounds="${3:-5}"
tag=$(echo "$case_id" | tr 'A-Z-' 'a-z_')

run_arm() {
    local arm="$1" idx="$2" bin="$3"
    local extra_env=()
    if [ "$arm" = "observe-on" ]; then
        extra_env=(SLUICE_OBSERVE=1 SLUICE_OBSERVE_OUT="$out/overhead/${tag}-${arm}-${idx}.obs")
    fi
    env "${extra_env[@]}" SLUICE_BENCH_STATS=1 SLUICE_BENCH_CASE_FILTER="=$case_id" \
        SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7 \
        SLUICE_BENCH_JSONL="$out/overhead/${tag}-${arm}-${idx}.jsonl" \
        taskset -c 2 "$bin/$target" >"$out/overhead/${tag}-${arm}-${idx}.log" 2>&1
    local rc=$?
    python3 bench/check_run.py "$out/overhead/${tag}-${arm}-${idx}.jsonl" --stage measured \
        --exit-code "$rc" --warmup 2 --measured 7 --expect-cases 1 \
        || echo "CHECK_FAILED ${tag}-${arm}-${idx}"
}

i=0
while [ "$i" -lt "$rounds" ]; do
    # rotate arm order each round: full,off,on / off,on,full / on,full,off ...
    case $((i % 3)) in
    0) order="full observe-off observe-on" ;;
    1) order="observe-off observe-on full" ;;
    2) order="observe-on full observe-off" ;;
    esac
    for arm in $order; do
        if [ "$arm" = "full" ]; then
            run_arm "$arm" "$i" "$full_bin"
        else
            run_arm "$arm" "$i" "$obs_bin"
        fi
    done
    i=$((i + 1))
done
echo "overhead arms done for $case_id ($rounds rounds)"
