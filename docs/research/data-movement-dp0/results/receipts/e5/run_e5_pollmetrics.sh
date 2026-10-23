#!/usr/bin/env bash
# E5 stage 3: poll_calls/op and completion-visibility metrics from the
# stats-wired harness copies (upstream bench leaves its AsyncStats local
# unbound, so stats_* fields are zeros there — patched copies in /tmp/e5/harness
# read ctx.stats() directly). 5 pairs per case; W-02 pinned, W-03 unpinned
# (see the stage-2b pinning finding).
set -euo pipefail
E5=/tmp/e5
OUT=$E5/results/pollmetrics
FIX=/tmp/e5-fixture
CPU=2
PAIRS=5
export SLUICE_BENCH_TMP_DIR=$FIX
mkdir -p "$OUT" "$FIX"

BASELIB=$HOME/Source/Sluice/build-e5/linux/x86_64/release
THREELIB=/tmp/e5/wt-threearm/build-e5/linux/x86_64/release
INC=$HOME/Source/Sluice/include

# Compile patched harness against each arm's archives
for arm in base threearm; do
  if [ $arm = base ]; then lib=$BASELIB; else lib=$THREELIB; fi
  g++ -std=c++20 -O3 -DNDEBUG -DSLUICE_HAS_LIBURING -I$INC -I/tmp/e5/harness \
    /tmp/e5/harness/bench_w02_pipeline.cpp -o /tmp/e5/bin/w02s_$arm \
    $lib/libsluice_async.a $lib/libsluice_core.a -luring -lpthread
  g++ -std=c++20 -O3 -DNDEBUG -DSLUICE_HAS_LIBURING -I$INC -I/tmp/e5/harness \
    /tmp/e5/harness/bench_w03_external_loop.cpp -o /tmp/e5/bin/w03s_$arm \
    $lib/libsluice_async.a $lib/libsluice_core.a -luring -lpthread
done
sha256sum /tmp/e5/bin/w02s_base /tmp/e5/bin/w02s_threearm /tmp/e5/bin/w03s_base /tmp/e5/bin/w03s_threearm > $OUT/binhashes.txt

STAMP=$(date -u +%Y%m%dT%H%M%S)
LOG=$OUT/campaign-$STAMP.log
echo "pollmetrics stamp=$STAMP pairs=$PAIRS" > "$LOG"

W02CASES=(W02-uring-4k-d1 W02-uring-4k-d16 W02-threadpool-4k-d16-w4)
W03CASES=(W03-uring-4k-d4 W03-uring-4k-d16 W03-threadpool-4k-d16-w4)

run_case() { # arm bench case tag pin
  local arm=$1 bench=$2 case=$3 tag=$4 pin=$5
  local bin=/tmp/e5/bin/${bench}s_$arm
  local prefix=""
  [ "$pin" = pinned ] && prefix="taskset -c $CPU"
  $prefix env SLUICE_BENCH_TMP_DIR=$FIX \
    SLUICE_BENCH_CASE_FILTER="=$case" \
    SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 \
    SLUICE_BENCH_JSONL="$OUT/$tag.jsonl" \
    "$bin" > "$OUT/$tag.stdout" 2> "$OUT/$tag.stderr"
  echo "$(date -u +%H:%M:%S) $tag done" >> "$LOG"
}

for case in "${W02CASES[@]}"; do
  for ((p = 0; p < PAIRS; p++)); do
    if ((p % 2 == 0)); then
      run_case threearm w02 "$case" "$case-p$p-a" pinned
      run_case base     w02 "$case" "$case-p$p-b" pinned
    else
      run_case base     w02 "$case" "$case-p$p-b" pinned
      run_case threearm w02 "$case" "$case-p$p-a" pinned
    fi
  done
  echo "CASE_DONE $case" >> "$LOG"
done
for case in "${W03CASES[@]}"; do
  for ((p = 0; p < PAIRS; p++)); do
    if ((p % 2 == 0)); then
      run_case threearm w03 "$case" "$case-p$p-a" unpinned
      run_case base     w03 "$case" "$case-p$p-b" unpinned
    else
      run_case base     w03 "$case" "$case-p$p-b" unpinned
      run_case threearm w03 "$case" "$case-p$p-a" unpinned
    fi
  done
  echo "CASE_DONE $case" >> "$LOG"
done
echo "POLLMETRICS_DONE" >> "$LOG"
echo "pollmetrics complete: $OUT"
