#!/bin/zsh
set -uo pipefail
OUT=/tmp/e5r2/results/w01-redo
FIX=/tmp/dp3a-fixture
CPU=2
PAIRS=8
B=build/linux/x86_64/release
BB=$HOME/Source/e5r2/wt-base/$B
AB=$HOME/Source/e5r2/wt-arm/$B
mkdir -p $OUT
STAMP=$(date -u +%Y%m%dT%H%M%S)
LOG=$OUT/campaign-$STAMP.log
echo "campaign=dp3a-w01-redo stamp=$STAMP pairs=$PAIRS filter=W01 substring" > $LOG
run_bench() {
  local bin=$1 tag=$2
  taskset -c $CPU env SLUICE_BENCH_TMP_DIR=$FIX SLUICE_BENCH_CASE_FILTER="W01" \
    SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 SLUICE_BENCH_STATS=0 \
    SLUICE_BENCH_JSONL="$OUT/$tag.jsonl" "$bin" > "$OUT/$tag.stdout" 2> "$OUT/$tag.stderr"
  echo "$(date -u +%H:%M:%S) $tag loadavg=$(awk "{print \$1}" /proc/loadavg)" >> $LOG
}
for ((p = 0; p < PAIRS; p++)); do
  if ((p % 2 == 0)); then
    run_bench "$AB/bench_w01_direct" "W01-p$p-a"
    run_bench "$BB/bench_w01_direct" "W01-p$p-b"
  else
    run_bench "$BB/bench_w01_direct" "W01-p$p-b"
    run_bench "$AB/bench_w01_direct" "W01-p$p-a"
  fi
done
echo "W01_REDO_DONE $(date -u +%H:%M:%S)" >> $LOG
