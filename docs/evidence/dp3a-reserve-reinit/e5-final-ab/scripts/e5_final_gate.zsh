#!/bin/zsh
# DP-3A final E5 gate, stage 2 (runs only after stage-1 D1 arbitration passes):
# remaining W-02/W-03 product shapes, W-01 negative control, stats-on
# pollmetrics, full regression battery, the six DP-3A mutants on GCC 16, and
# the reserve() machine-code proof from the measured binaries.
# W-03 runs UNPINNED (fixed stop-order bench, preregistered condition, recorded).
set -uo pipefail
OUT=/tmp/e5r2/results/gate
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
  echo "campaign=dp3a-stage2 stamp=$STAMP pairs=$PAIRS"
  echo "base_sha=$(git -C $HOME/Source/e5r2/wt-base rev-parse HEAD)"
  echo "arm_sha=$(git -C $HOME/Source/e5r2/wt-arm rev-parse HEAD)"
  echo "cpu=$CPU pinned for W-02/W-01; W-03 UNPINNED by design"
  echo "perf_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid)"
  uname -srmo; g++ --version | head -1
  sha256sum $BB/bench_w02_pipeline $AB/bench_w02_pipeline $BB/bench_w03_external_loop $AB/bench_w03_external_loop $BB/bench_w01_direct $AB/bench_w01_direct | awk '{print "  binary "$2" "$1}'
} > $LOG 2>&1

run_bench() { # bin case tag pinned statsmode
  local bin=$1 case=$2 tag=$3 pinned=$4 stats=$5
  local wrap=(env SLUICE_BENCH_TMP_DIR=$FIX SLUICE_BENCH_CASE_FILTER="=$case"
              SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3
              SLUICE_BENCH_STATS=$stats
              SLUICE_BENCH_JSONL="$OUT/$tag.jsonl" "$bin")
  if [[ $pinned == pinned ]]; then
    taskset -c $CPU perf stat -x, -o "$OUT/$tag.perf" \
      -e instructions:u -e cycles:u -e task-clock:u -- "${wrap[@]}" \
      > "$OUT/$tag.stdout" 2> "$OUT/$tag.stderr"
  else
    env "${wrap[@]}" > "$OUT/$tag.stdout" 2> "$OUT/$tag.stderr"
  fi
  echo "$(date -u +%H:%M:%S) $tag loadavg=$(awk '{print $1}' /proc/loadavg)" >> $LOG
}

comparison() { # dir case armbin basebin pinned stats pairs
  local d=$1 case=$2 abin=$3 bbin=$4 pinned=$5 stats=$6 n=$7
  mkdir -p $OUT/$d
  for ((p = 0; p < n; p++)); do
    if ((p % 2 == 0)); then
      run_bench "$abin" "$case" "$d/$case-p$p-a" $pinned $stats
      run_bench "$bbin" "$case" "$d/$case-p$p-b" $pinned $stats
    else
      run_bench "$bbin" "$case" "$d/$case-p$p-b" $pinned $stats
      run_bench "$abin" "$case" "$d/$case-p$p-a" $pinned $stats
    fi
  done
  echo "COMPARISON $d DONE $(date -u +%H:%M:%S)" >> $LOG
}

# s1: remaining W-02 product gate shapes (pinned, stats off, perf-wrapped)
comparison s1-w02-d16 W02-uring-4k-d16          "$AB/bench_w02_pipeline" "$BB/bench_w02_pipeline" pinned  0 $PAIRS
comparison s1-w02-64k W02-uring-64k-d16         "$AB/bench_w02_pipeline" "$BB/bench_w02_pipeline" pinned  0 $PAIRS
comparison s1-w02-tp  W02-threadpool-4k-d16-w4  "$AB/bench_w02_pipeline" "$BB/bench_w02_pipeline" pinned  0 $PAIRS

# s2: W-03 gate (fixed stop-order bench; UNPINNED — recorded condition)
comparison s2-w03-d4  W03-uring-4k-d4           "$AB/bench_w03_external_loop" "$BB/bench_w03_external_loop" unpinned 0 $PAIRS
comparison s2-w03-d16 W03-uring-4k-d16          "$AB/bench_w03_external_loop" "$BB/bench_w03_external_loop" unpinned 0 $PAIRS
comparison s2-w03-tp  W03-threadpool-4k-d16-w4  "$AB/bench_w03_external_loop" "$BB/bench_w03_external_loop" unpinned 0 $PAIRS

# s3: W-01 negative control (pinned; must be arm-independent)
comparison s3-w01 W01 "$AB/bench_w01_direct" "$BB/bench_w01_direct" pinned 0 8

# s4: stats-on pollmetrics (stats arithmetic must be arm-independent)
comparison s4-poll-d1  W02-uring-4k-d1  "$AB/bench_w02_pipeline" "$BB/bench_w02_pipeline" pinned  1 5
comparison s4-poll-d16 W02-uring-4k-d16 "$AB/bench_w02_pipeline" "$BB/bench_w02_pipeline" pinned  1 5

# s5: full regression battery on the ARM tree (release+liburing, then debug+liburing)
{
  echo "cell: release-liburing | config: -m release --liburing=y"
  echo "command: xmake build -y && xmake test"
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%S%z)"
  ( cd $HOME/Source/e5r2/wt-arm && xmake f -c -y >/dev/null 2>&1 && xmake f -m release --liburing=y -y >/dev/null 2>&1 && xmake build -y >/dev/null 2>&1 && xmake test 2>&1 | tail -1 )
} >> $LOG 2>&1
echo "S5 RELEASE BATTERY DONE $(date -u +%H:%M:%S)" >> $LOG
{
  echo "cell: debug-liburing | config: -m debug --liburing=y"
  echo "command: xmake f -m debug --liburing=y && xmake build -y && xmake test"
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%S%z)"
  ( cd $HOME/Source/e5r2/wt-arm && xmake f -m debug --liburing=y -y >/dev/null 2>&1 && xmake build -y >/dev/null 2>&1 && xmake test 2>&1 | tail -1 )
} >> $LOG 2>&1
echo "S5 DEBUG BATTERY DONE $(date -u +%H:%M:%S)" >> $LOG
( cd $HOME/Source/e5r2/wt-arm && xmake f -c -y >/dev/null 2>&1 && xmake f -m release --liburing=y -y >/dev/null 2>&1 && for tgt in sluice_core sluice_async bench_w01_direct bench_w02_pipeline bench_w03_external_loop; do xmake build -y $tgt >/dev/null 2>&1; done )
echo "S5 REBUILD RELEASE ARMS DONE $(date -u +%H:%M:%S)" >> $LOG

# s6: the six DP-3A mutants on GCC 16 (kill signatures must match the oracle)
{
  for m in release_skips_outcome_clear release_skips_metadata_clear release_skips_observer_clear rollback_without_release exhausted_slot_reused constructor_dirty_slots; do
    xmake build -y request_core_mut_$m >/dev/null 2>&1
    echo "=== mutant $m ==="
    xmake run -q request_core_mut_$m 2>&1 | sort | uniq -c | sort -rn | head -8
    echo "exit=$?"
  done
} > $OUT/e5-mutants.txt 2>&1
echo "S6 MUTANTS DONE $(date -u +%H:%M:%S)" >> $LOG

# s7: reserve() machine-code proof from the measured release binaries
{
  echo "=== wt-arm bench_w02_pipeline (final measured binary) RequestCore::reserve ==="
  objdump -d --no-show-raw-insn $AB/bench_w02_pipeline | awk '/<_ZN6sluice5async6detail11RequestCore7reserveEv>:/{f=1} f{print} f&&/^$/{exit}'
  echo "=== wt-base bench_w02_pipeline RequestCore::reserve ==="
  objdump -d --no-show-raw-insn $BB/bench_w02_pipeline | awk '/<_ZN6sluice5async6detail11RequestCore7reserveEv>:/{f=1} f{print} f&&/^$/{exit}'
} > $OUT/e5-reserve-objdump.txt 2>&1
echo "S7 OBJDUMP DONE $(date -u +%H:%M:%S)" >> $LOG
echo "STAGE2_ALL_DONE $(date -u +%H:%M:%S)" >> $LOG
