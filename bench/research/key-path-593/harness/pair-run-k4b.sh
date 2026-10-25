#!/usr/bin/env bash
# #593 K2-a native paired experiment — interleaved AB/BA pair runner.
#
# Arms: full  = frozen baseline binary (worktrees/baseline, authority hashes)
#       var   = K2-a deferred-kernel-submission arm (worktrees/K2-a)
#       aa    = baseline binary on both sides (calibration / negative control)
#
# One pair = one run per arm. Each run: 2 warmup + 7 measured repetitions,
# stats on, taskset -c 2, single-case filter. Every run is validated by the
# frozen G0 checker (which also requires the in-process correctness oracle
# self-test and every per-repetition byte-verification phase to pass).
# Arm order alternates by pair index (even: full first, odd: var first).
#
# Usage: pair-run.sh <case_id> <bench_w02_pipeline|bench_w03_external_loop> <pairs> <mode> <tag>
set -uo pipefail

full_bin=/home/jnhu/sluice-593-research/worktrees/K4-b/build-base/linux/x86_64/release
var_bin=/home/jnhu/sluice-593-research/worktrees/K4-b/build-bench-ring/linux/x86_64/release
checker=/home/jnhu/sluice-593-research/worktrees/baseline/bench/check_run.py
out=/home/jnhu/sluice-593-research/evidence/ab/K4-b

case_id="${1:?case id}"
target="${2:?target}"
pairs="${3:-15}"
mode="${4:-elide-vs-full}"
tag="${5:?tag}"

run_dir="$out/pairs-$tag-$mode"
invalid_dir="$run_dir/invalid"
mkdir -p "$run_dir" "$invalid_dir"

{
    echo "# K2-a paired run: case=$case_id target=$target pairs=$pairs mode=$mode"
    echo "started=$(date -Is)"
    echo "host=$(hostname) kernel=$(uname -r)"
    echo "git_base=$(git -C /home/jnhu/sluice-593-research/worktrees/baseline rev-parse HEAD)"
    echo "git_var=$(git -C /home/jnhu/sluice-593-research/worktrees/K2-a rev-parse HEAD)"
    echo "full_w02=$(sha256sum "$full_bin/bench_w02_pipeline" | cut -d' ' -f1)"
    echo "full_w03=$(sha256sum "$full_bin/bench_w03_external_loop" | cut -d' ' -f1)"
    echo "var_w02=$(sha256sum "$var_bin/bench_w02_pipeline" | cut -d' ' -f1)"
    echo "var_w03=$(sha256sum "$var_bin/bench_w03_external_loop" | cut -d' ' -f1)"
    echo "affinity=$(taskset -pc $$ 2>&1)"
    echo "load_before=$(uptime)"
} >"$run_dir/provenance.txt"

run_arm() {
    local arm="$1" idx="$2" bin="$3" write_as="$4"
    local prefix="$run_dir/${write_as}-${idx}"
    echo "pair=$idx arm=$arm order=$(date -Is) load=$(uptime | sed 's/.*load average: //')" \
        >>"$run_dir/order.log"
    env SLUICE_BENCH_STATS=1 \
        SLUICE_BENCH_CASE_FILTER="=$case_id" \
        SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7 \
        SLUICE_BENCH_JSONL="$prefix.jsonl" \
        timeout 300 taskset -c 2 "$bin/$target" >"$prefix.log" 2>&1
    local rc=$?
    python3 "$checker" "$prefix.jsonl" --stage measured --exit-code "$rc" \
        --warmup 2 --measured 7 --expect-cases 1 >"$prefix.check" 2>&1
    local check_rc=$?
    echo "pair=$idx arm=$arm rc=$rc check_rc=$check_rc $(tail -n 1 "$prefix.check")" \
        >>"$run_dir/order.log"
    if [ "$rc" -ne 0 ] || [ "$check_rc" -ne 0 ]; then
        return 1
    fi
    return 0
}

run_pair_ordered() {
    local i="$1"
    if [ "$mode" = "aa-full" ]; then
        if [ $((i % 2)) -eq 0 ]; then
            run_arm full "$i" "$full_bin" full && run_arm full "$i" "$full_bin" full2
        else
            run_arm full "$i" "$full_bin" full2 && run_arm full "$i" "$full_bin" full
        fi
    else
        if [ $((i % 2)) -eq 0 ]; then
            run_arm full "$i" "$full_bin" full && run_arm var "$i" "$var_bin" var
        else
            run_arm var "$i" "$var_bin" var && run_arm full "$i" "$full_bin" full
        fi
    fi
}

valid=0
attempted=0
i=0
while [ "$i" -lt "$pairs" ]; do
    ok=0
    attempt=0
    while [ "$attempt" -lt 3 ]; do
        attempted=$((attempted + 1))
        if run_pair_ordered "$i"; then
            ok=1
            break
        fi
        echo "pair=$i attempt=$attempt INVALID -> invalid/" | tee -a "$run_dir/order.log"
        for f in "$run_dir/full-$i.jsonl" "$run_dir/full-$i.log" "$run_dir/full-$i.check" \
                 "$run_dir/full2-$i.jsonl" "$run_dir/full2-$i.log" "$run_dir/full2-$i.check" \
                 "$run_dir/var-$i.jsonl" "$run_dir/var-$i.log" "$run_dir/var-$i.check"; do
            [ -e "$f" ] && mv "$f" "$invalid_dir/attempt${attempt}-$(basename "$f")"
        done
        attempt=$((attempt + 1))
    done
    if [ "$ok" -ne 1 ]; then
        echo "pair=$i EXHAUSTED: no valid pair after 3 attempts" | tee -a "$run_dir/order.log"
        exit 1
    fi
    valid=$((valid + 1))
    echo "pair=$i valid ($valid/$pairs)" >>"$run_dir/order.log"
    i=$((i + 1))
done

{
    echo "finished=$(date -Is)"
    echo "valid_pairs=$valid attempted_pairs=$attempted"
    echo "load_after=$(uptime)"
} >>"$run_dir/provenance.txt"
echo "DONE $tag $mode valid_pairs=$valid attempted=$attempted"
