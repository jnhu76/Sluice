#!/usr/bin/env bash
# E5 stage 2b: W-03 non-spinning external epoll loop A/B, base vs three-arm.
# DEVIATION vs stage-2 W-02 runs: no taskset pinning. Under `taskset -c 2`
# both arms' W-03 external-loop bench deadlocks on this host (base rc=124 at
# 60s timeout, threearm killed after 13 min in the stage-2 campaign; both
# arms complete unpinned and under perf stat). Recorded as an E5 finding;
# the unpinned runs keep arm pairing and rotation.
set -euo pipefail

E5=/tmp/e5
OUT=$E5/results/w03
FIX=/tmp/e5-fixture
PAIRS=15
export SLUICE_BENCH_TMP_DIR=$FIX
mkdir -p "$OUT" "$FIX"

BASE=$HOME/Source/Sluice/build-e5/linux/x86_64/release
THREE=/tmp/e5/wt-threearm/build-e5/linux/x86_64/release

STAMP=$(date -u +%Y%m%dT%H%M%S)
LOG=$OUT/campaign-$STAMP.log
{
  echo "campaign_stage=w03-unpinned stamp=$STAMP pairs=$PAIRS"
  uname -srmo
  g++ --version | head -1
  sha256sum $BASE/bench_w03_external_loop $THREE/bench_w03_external_loop
} > "$LOG"

CASES=(
  W03-uring-4k-d4
  W03-uring-4k-d16
  W03-threadpool-4k-d16-w4
)

run_bench() { # benchbin case tag
  local bin=$1 case=$2 tag=$3
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
  for ((p = 0; p < PAIRS; p++)); do
    if ((p % 2 == 0)); then
      run_bench "$THREE/bench_w03_external_loop" "$case" "$case-p$p-a"
      run_bench "$BASE/bench_w03_external_loop"  "$case" "$case-p$p-b"
    else
      run_bench "$BASE/bench_w03_external_loop"  "$case" "$case-p$p-b"
      run_bench "$THREE/bench_w03_external_loop" "$case" "$case-p$p-a"
    fi
  done
  echo "CASE_DONE $case" >> "$LOG"
done

echo "W03_STAGE_DONE" >> "$LOG"
echo "w03 stage complete: $OUT"
