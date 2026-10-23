#!/usr/bin/env bash
# E5 attribution: W02-uring-4k-d1 base vs dp1dp2b (isolates dp3a's share of
# the d1 regression seen for the three-arm combo).
set -euo pipefail
E5=/tmp/e5
OUT=$E5/results/attr-d1
FIX=/tmp/e5-fixture
CPU=2
PAIRS=15
export SLUICE_BENCH_TMP_DIR=$FIX
mkdir -p "$OUT" "$FIX"

BASE=$HOME/Source/Sluice/build-e5/linux/x86_64/release
DP12=/tmp/e5/wt-dp1dp2b/build-e5/linux/x86_64/release

STAMP=$(date -u +%Y%m%dT%H%M%S)
LOG=$OUT/campaign-$STAMP.log
echo "attribution W02-uring-4k-d1 base vs dp1dp2b stamp=$STAMP" > "$LOG"

run_bench() { # benchbin tag
  local bin=$1 tag=$2
  taskset -c $CPU \
    perf stat -x, -o "$OUT/$tag.perf" \
    -e instructions:u -e cycles:u -e task-clock:u \
    -- env SLUICE_BENCH_TMP_DIR=$FIX \
           SLUICE_BENCH_CASE_FILTER="=W02-uring-4k-d1" \
           SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 \
           SLUICE_BENCH_JSONL="$OUT/$tag.jsonl" \
           "$bin" > "$OUT/$tag.stdout" 2> "$OUT/$tag.stderr"
  echo "$(date -u +%H:%M:%S) $tag loadavg=$(awk '{print $1}' /proc/loadavg)" >> "$LOG"
}

for ((p = 0; p < PAIRS; p++)); do
  if ((p % 2 == 0)); then
    run_bench "$DP12/bench_w02_pipeline" "W02-uring-4k-d1-p$p-a"
    run_bench "$BASE/bench_w02_pipeline" "W02-uring-4k-d1-p$p-b"
  else
    run_bench "$BASE/bench_w02_pipeline" "W02-uring-4k-d1-p$p-b"
    run_bench "$DP12/bench_w02_pipeline" "W02-uring-4k-d1-p$p-a"
  fi
done
echo "ATTR_DONE" >> "$LOG"
echo "attribution complete: $OUT"
