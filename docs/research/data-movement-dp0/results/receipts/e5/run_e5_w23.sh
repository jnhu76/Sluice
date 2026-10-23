#!/usr/bin/env bash
# E5 stage 2: W-02 (blocking-wait pipeline) and W-03 (non-spinning external
# epoll loop) production-path A/B, base vs three-arm combo. Per invocation:
# 1 warmup + 3 measured repetitions, oracle and latency passes included by the
# bench harness; JSONL per invocation; perf stat wraps the whole process.
set -euo pipefail

E5=/tmp/e5
OUT=$E5/results/w23
FIX=/tmp/e5-fixture
CPU=2
PAIRS=15
export SLUICE_BENCH_TMP_DIR=$FIX
mkdir -p "$OUT" "$FIX"

BASE=$HOME/Source/Sluice/build-e5/linux/x86_64/release
THREE=/tmp/e5/wt-threearm/build-e5/linux/x86_64/release

STAMP=$(date -u +%Y%m%dT%H%M%S)
LOG=$OUT/campaign-$STAMP.log
{
  echo "campaign_stage=w23 stamp=$STAMP pairs=$PAIRS"
  uname -srmo
  g++ --version | head -1
  sha256sum $BASE/bench_w02_pipeline $BASE/bench_w03_external_loop \
            $THREE/bench_w02_pipeline $THREE/bench_w03_external_loop
} > "$LOG"

CASES=(
  W02-uring-4k-d1
  W02-uring-4k-d16
  W02-uring-64k-d16
  W02-threadpool-4k-d16-w4
  W03-uring-4k-d4
  W03-uring-4k-d16
  W03-threadpool-4k-d16-w4
)

run_bench() { # benchbin case tag
  local bin=$1 case=$2 tag=$3
  taskset -c $CPU \
    perf stat -x, -o "$OUT/$tag.perf" \
    -e instructions:u -e cycles:u -e task-clock:u \
    -- env SLUICE_BENCH_TMP_DIR=$FIX \
           SLUICE_BENCH_CASE_FILTER="=$case" \
           SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 \
           SLUICE_BENCH_JSONL="$OUT/$tag.jsonl" \
           "$bin" > "$OUT/$tag.stdout" 2> "$OUT/$tag.stderr"
  echo "$(date -u +%H:%M:%S) $tag loadavg=$(awk '{print $1}' /proc/loadavg) freq=$(cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_cur_freq)" >> "$LOG"
}

for case in "${CASES[@]}"; do
  bench=bench_w02_pipeline
  [[ $case == W03-* ]] && bench=bench_w03_external_loop
  for ((p = 0; p < PAIRS; p++)); do
    if ((p % 2 == 0)); then
      run_bench "$THREE/$bench" "$case" "$case-p$p-a"
      run_bench "$BASE/$bench"  "$case" "$case-p$p-b"
    else
      run_bench "$BASE/$bench"  "$case" "$case-p$p-b"
      run_bench "$THREE/$bench" "$case" "$case-p$p-a"
    fi
  done
  echo "CASE_DONE $case" >> "$LOG"
done

echo "W23_STAGE_DONE" >> "$LOG"
echo "w23 stage complete: $OUT"
