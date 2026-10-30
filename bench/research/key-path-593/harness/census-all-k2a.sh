#!/usr/bin/env bash
# K2-a census battery (counts only; not used for timing/latency).
set -uo pipefail
cd "$(dirname "$0")"
rm -rf census
run() {
    local case_id="$1" target="$2" tag="$3" reps="$4"
    echo "=== $(date -Is) CENSUS $case_id $tag reps=$reps" | tee -a census-battery.log
    ./census.sh "$case_id" "$target" "$tag" "$reps" 2>&1 | tee -a census-battery.log
    echo "=== $(date -Is) CENSUS END $case_id rc=${PIPESTATUS[0]}" | tee -a census-battery.log
}
run "W02-uring-4k-d16" bench_w02_pipeline w02_uring_4k_d16 3
run "W02-uring-4k-d64" bench_w02_pipeline w02_uring_4k_d64 3
run "W02-uring-4k-d1" bench_w02_pipeline w02_uring_4k_d1 3
run "W03-uring-4k-d16" bench_w03_external_loop w03_uring_4k_d16 3
run "W02-uring-4k-d4" bench_w02_pipeline w02_uring_4k_d4 1
run "W02-uring-64k-d16" bench_w02_pipeline w02_uring_64k_d16 1
run "W02-uring-4k-d16-saturate" bench_w02_pipeline w02_uring_4k_d16_saturate 1
run "W03-uring-4k-d4" bench_w03_external_loop w03_uring_4k_d4 1
run "W03-uring-4k-d64" bench_w03_external_loop w03_uring_4k_d64 1
echo "=== $(date -Is) CENSUS BATTERY DONE" | tee -a census-battery.log
