#!/bin/zsh
# DP-3A final incremental E5 A/B, stage 1: W02-uring-4k-d1 arbitration.
# BASE = master 4221c7e3 (DP-1 included), ARM = PR #573 head da602ccc.
# Design: 15 interleaved AB/BA pairs, taskset CPU2, 1 warmup + 3 measured per
# invocation, SLUICE_BENCH_STATS=0, perf stat -x, around every invocation.
# Negative controls: BASE vs BASE self-pairs on the same shape; zop w01ctl.
# Preregistered acceptance (fixed before any measurement):
#   primary  = wall ns/op paired-ratio median, secondary = cpu ns/op;
#   PASS(A)  = median CI includes or is below 1.0 AND instr/op not increased;
#   ACCEPT_WITH_MEASURED_SMALL_REGRESSION(B) = median <= 1.03 AND
#     pairs-above-1.03 <= 10/15 (standing h2b engineering bound);
#   NO_GO(C) = median > 1.03 or > 10/15 pairs above 1.03;
#   INCONCLUSIVE(D) = base-vs-base control noise of the same magnitude as the
#     signal or conflicting batches.
set -uo pipefail
OUT=/tmp/e5r2/results/d1
FIX=/tmp/dp3a-fixture
CPU=2
PAIRS=15
export SLUICE_BENCH_STATS=0
B=build/linux/x86_64/release
BB=$HOME/Source/e5r2/wt-base/$B
AB=$HOME/Source/e5r2/wt-arm/$B
mkdir -p $OUT $FIX

STAMP=$(date -u +%Y%m%dT%H%M%S)
LOG=$OUT/campaign-$STAMP.log
{
  echo "campaign=dp3a-stage1 stamp=$STAMP pairs=$PAIRS stats=off cpu=$CPU"
  echo "base_sha=$(git -C $HOME/Source/e5r2/wt-base rev-parse HEAD)"
  echo "arm_sha=$(git -C $HOME/Source/e5r2/wt-arm rev-parse HEAD)"
  echo "governor=$(cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor 2>/dev/null) cpu2_siblings=$(cat /sys/devices/system/cpu/cpu2/topology/thread_siblings_list 2>/dev/null)"
  echo "perf_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid)"
  uname -srmo
  g++ --version | head -1
  echo "liburing=$(pkg-config --modversion liburing)"
  sha256sum $BB/bench_w02_pipeline $AB/bench_w02_pipeline /tmp/dp3a-tools/bench_dp0_wt-base /tmp/dp3a-tools/bench_dp0_wt-arm | awk '{print "  binary "$2" "$1}'
  ldd $BB/bench_w02_pipeline | grep -i uring
  sha256sum /tmp/dp3a-tools/bench_dp0_movement.cpp | awk '{print "  zop_harness "$2" "$1}'
} > $LOG 2>&1

run_bench() { # bin case tag
  local bin=$1 case=$2 tag=$3
  taskset -c $CPU \
    perf stat -x, -o "$OUT/$tag.perf" \
    -e instructions:u -e cycles:u -e task-clock:u \
    -- env SLUICE_BENCH_TMP_DIR=$FIX \
           SLUICE_BENCH_CASE_FILTER="=$case" \
           SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 \
           SLUICE_BENCH_STATS=0 \
           SLUICE_BENCH_JSONL="$OUT/$tag.jsonl" \
           "$bin" > "$OUT/$tag.stdout" 2> "$OUT/$tag.stderr"
  echo "$(date -u +%H:%M:%S) $tag loadavg=$(awk '{print $1}' /proc/loadavg)" >> $LOG
}

run_zop() { # bin case tag
  local bin=$1 zcase=$2 tag=$3
  taskset -c $CPU \
    perf stat -x, -o "$OUT/$tag.perf" \
    -e instructions:u -e cycles:u -e task-clock:u \
    -- env SLUICE_BENCH_TMP_DIR=$FIX "$bin" "$zcase" 200000 \
    > "$OUT/$tag.bench.json" 2> "$OUT/$tag.stderr"
  echo "$(date -u +%H:%M:%S) $tag loadavg=$(awk '{print $1}' /proc/loadavg)" >> $LOG
}

# m1: DP-3A direct incremental, W02-uring-4k-d1 (a = ARM, b = BASE)
mkdir -p $OUT/m1
for ((p = 0; p < PAIRS; p++)); do
  if ((p % 2 == 0)); then
    run_bench "$AB/bench_w02_pipeline" W02-uring-4k-d1 m1/W02-uring-4k-d1-p$p-a
    run_bench "$BB/bench_w02_pipeline" W02-uring-4k-d1 m1/W02-uring-4k-d1-p$p-b
  else
    run_bench "$BB/bench_w02_pipeline" W02-uring-4k-d1 m1/W02-uring-4k-d1-p$p-b
    run_bench "$AB/bench_w02_pipeline" W02-uring-4k-d1 m1/W02-uring-4k-d1-p$p-a
  fi
done
echo "M1 DONE $(date -u +%H:%M:%S)" >> $LOG

# m2: BASE vs BASE self-pair negative control, same shape (a/b both BASE)
mkdir -p $OUT/m2-base
for ((p = 0; p < PAIRS; p++)); do
  if ((p % 2 == 0)); then
    run_bench "$BB/bench_w02_pipeline" W02-uring-4k-d1 m2-base/W02-uring-4k-d1-p$p-a
    run_bench "$BB/bench_w02_pipeline" W02-uring-4k-d1 m2-base/W02-uring-4k-d1-p$p-b
  else
    run_bench "$BB/bench_w02_pipeline" W02-uring-4k-d1 m2-base/W02-uring-4k-d1-p$p-b
    run_bench "$BB/bench_w02_pipeline" W02-uring-4k-d1 m2-base/W02-uring-4k-d1-p$p-a
  fi
done
echo "M2 DONE $(date -u +%H:%M:%S)" >> $LOG

# m3: deterministic zero-op instrument (a = ARM, b = BASE) + w01ctl negative control
mkdir -p $OUT/m3-zop
for ((p = 0; p < PAIRS; p++)); do
  if ((p % 2 == 0)); then
    run_zop /tmp/dp3a-tools/bench_dp0_wt-arm  zop    m3-zop/zop-dp3a-p$p-a
    run_zop /tmp/dp3a-tools/bench_dp0_wt-base zop    m3-zop/zop-dp3a-p$p-b
  else
    run_zop /tmp/dp3a-tools/bench_dp0_wt-base zop    m3-zop/zop-dp3a-p$p-b
    run_zop /tmp/dp3a-tools/bench_dp0_wt-arm  zop    m3-zop/zop-dp3a-p$p-a
  fi
done
for ((p = 0; p < PAIRS; p++)); do
  if ((p % 2 == 0)); then
    run_zop /tmp/dp3a-tools/bench_dp0_wt-base w01ctl m3-zop/w01ctl-base-p$p-a
    run_zop /tmp/dp3a-tools/bench_dp0_wt-base w01ctl m3-zop/w01ctl-base-p$p-b
  else
    run_zop /tmp/dp3a-tools/bench_dp0_wt-base w01ctl m3-zop/w01ctl-base-p$p-b
    run_zop /tmp/dp3a-tools/bench_dp0_wt-base w01ctl m3-zop/w01ctl-base-p$p-a
  fi
done
echo "M3 DONE $(date -u +%H:%M:%S)" >> $LOG
echo "STAGE1_ALL_DONE $(date -u +%H:%M:%S)" >> $LOG
