#!/bin/zsh
# DP-3A confirmation round for the single outlier shape W03-threadpool-4k-d16-w4
# (first sample +6.3% unpinned, CI above 1). Three comparisons, 15 pairs each:
#   c1: arm vs base, UNPINNED (same condition as the first sample)
#   c2: arm vs base, PINNED CPU2 (affinity-sensitivity attribution)
#   c3: base vs base, UNPINNED (shape-inherent noise floor)
set -uo pipefail
OUT=/tmp/e5r2/results/w03tp-conf
FIX=/tmp/dp3a-fixture
CPU=2
PAIRS=15
B=build/linux/x86_64/release
BB=$HOME/Source/e5r2/wt-base/$B
AB=$HOME/Source/e5r2/wt-arm/$B
mkdir -p $OUT $FIX
STAMP=$(date -u +%Y%m%dT%H%M%S)
LOG=$OUT/campaign-$STAMP.log
{
  echo "campaign=dp3a-w03tp-conf stamp=$STAMP pairs=$PAIRS"
  echo "base_sha=$(git -C $HOME/Source/e5r2/wt-base rev-parse HEAD)"
  echo "arm_sha=$(git -C $HOME/Source/e5r2/wt-arm rev-parse HEAD)"
  uname -srmo; g++ --version | head -1
  sha256sum $BB/bench_w03_external_loop $AB/bench_w03_external_loop | awk '{print "  binary "$2" "$1}'
} > $LOG 2>&1

run_bench() { # bin case tag pinned
  local bin=$1 case=$2 tag=$3 pinned=$4
  if [[ $pinned == pinned ]]; then
    taskset -c $CPU env SLUICE_BENCH_TMP_DIR=$FIX SLUICE_BENCH_CASE_FILTER="=$case" \
      SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 SLUICE_BENCH_STATS=0 \
      SLUICE_BENCH_JSONL="$OUT/$tag.jsonl" "$bin" > "$OUT/$tag.stdout" 2> "$OUT/$tag.stderr"
  else
    env SLUICE_BENCH_TMP_DIR=$FIX SLUICE_BENCH_CASE_FILTER="=$case" \
      SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 SLUICE_BENCH_STATS=0 \
      SLUICE_BENCH_JSONL="$OUT/$tag.jsonl" "$bin" > "$OUT/$tag.stdout" 2> "$OUT/$tag.stderr"
  fi
  echo "$(date -u +%H:%M:%S) $tag loadavg=$(awk '{print $1}' /proc/loadavg)" >> $LOG
}

comparison() { # dir a_bin b_bin pinned
  local d=$1 abin=$2 bbin=$3 pinned=$4
  mkdir -p $OUT/$d
  for ((p = 0; p < PAIRS; p++)); do
    if ((p % 2 == 0)); then
      run_bench "$abin" W03-threadpool-4k-d16-w4 "$d/W03-threadpool-4k-d16-w4-p$p-a" $pinned
      run_bench "$bbin" W03-threadpool-4k-d16-w4 "$d/W03-threadpool-4k-d16-w4-p$p-b" $pinned
    else
      run_bench "$bbin" W03-threadpool-4k-d16-w4 "$d/W03-threadpool-4k-d16-w4-p$p-b" $pinned
      run_bench "$abin" W03-threadpool-4k-d16-w4 "$d/W03-threadpool-4k-d16-w4-p$p-a" $pinned
    fi
  done
  echo "COMPARISON $d DONE $(date -u +%H:%M:%S)" >> $LOG
}

comparison c1-unpinned "$AB/bench_w03_external_loop" "$BB/bench_w03_external_loop" unpinned
comparison c2-pinned   "$AB/bench_w03_external_loop" "$BB/bench_w03_external_loop" pinned
comparison c3-baseline "$BB/bench_w03_external_loop" "$BB/bench_w03_external_loop" unpinned
echo "CONF_ALL_DONE $(date -u +%H:%M:%S)" >> $LOG
