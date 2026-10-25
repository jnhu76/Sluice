#!/usr/bin/env bash
# K2-a full paired battery. One line per case; 15 interleaved AB/BA pairs each.
set -uo pipefail
cd "$(dirname "$0")"
rm -rf pairs-smoke-elide-vs-full pair-results.json

run() {
    local case_id="$1" target="$2" tag="$3" mode="${4:-elide-vs-full}"
    echo "=== $(date -Is) START $case_id $tag $mode" | tee -a battery.log
    ./pair-run.sh "$case_id" "$target" 15 "$mode" "$tag" 2>&1 | tee -a battery.log
    local rc=${PIPESTATUS[0]}
    echo "=== $(date -Is) END $case_id $tag $mode rc=$rc" | tee -a battery.log
    return 0
}

run "W02-uring-4k-d16" bench_w02_pipeline w02_uring_4k_d16
run "W02-uring-4k-d64" bench_w02_pipeline w02_uring_4k_d64
run "W02-uring-4k-d4" bench_w02_pipeline w02_uring_4k_d4
run "W02-uring-4k-d1" bench_w02_pipeline w02_uring_4k_d1
run "W02-uring-64k-d16" bench_w02_pipeline w02_uring_64k_d16
run "W02-uring-4k-d16-saturate" bench_w02_pipeline w02_uring_4k_d16_saturate
run "W03-uring-4k-d16" bench_w03_external_loop w03_uring_4k_d16
run "W03-uring-4k-d4" bench_w03_external_loop w03_uring_4k_d4
run "W03-uring-4k-d64" bench_w03_external_loop w03_uring_4k_d64
run "W02-threadpool-4k-d16-w4" bench_w02_pipeline w02_threadpool_4k_d16_w4
run "W03-threadpool-4k-d16-w4" bench_w03_external_loop w03_threadpool_4k_d16_w4
run "W02-uring-4k-d16" bench_w02_pipeline w02_uring_4k_d16 aa-full
run "W03-uring-4k-d16" bench_w03_external_loop w03_uring_4k_d16 aa-full
echo "=== $(date -Is) BATTERY DONE" | tee -a battery.log
