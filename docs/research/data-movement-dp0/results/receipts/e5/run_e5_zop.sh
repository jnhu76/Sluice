#!/usr/bin/env bash
# E5 stage 1: DP-0 zop deterministic instrument + w01ctl negative control on
# native Linux (E5). Interleaved AB/BA pairs, perf stat user-space events.
# Research-only /tmp driver; evidence is the emitted JSONL + perf outputs.
set -euo pipefail

E5=/tmp/e5
BIN=$E5/bin
OUT=$E5/results/zop
FIX=/tmp/e5-fixture
CPU=2
PAIRS=15
ITERS=200000
export SLUICE_BENCH_TMP_DIR=$FIX
mkdir -p "$OUT" "$FIX"

STAMP=$(date -u +%Y%m%dT%H%M%S)
LOG=$OUT/campaign-$STAMP.log
{
  echo "campaign_stage=zop stamp=$STAMP"
  uname -srmo
  g++ --version | head -1
  cat /proc/sys/kernel/perf_event_paranoid
  sha256sum $BIN/dp0e5_base $BIN/dp0e5_dp1dp2b $BIN/dp0e5_threearm
} > "$LOG"

perf_run() { # bin case iters tag
  local bin=$1 case=$2 iters=$3 tag=$4
  SLUICE_DP0_REUSE_FIXTURE=1 taskset -c $CPU \
    perf stat -x, -o "$OUT/$tag.perf" \
    -e instructions:u -e cycles:u -e task-clock:u \
    -- "$bin" "$case" "$iters" > "$OUT/$tag.bench.json" 2> "$OUT/$tag.stderr"
  echo "$(date -u +%H:%M:%S) $tag loadavg=$(awk '{print $1}' /proc/loadavg) freq=$(cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_cur_freq)" >> "$LOG"
}

pair() { # armbin armname tagprefix
  local abin=$1 aname=$2 pfx=$3
  for ((p = 0; p < PAIRS; p++)); do
    if ((p % 2 == 0)); then
      perf_run "$abin"  zop $ITERS "$pfx-p$p-a"
      perf_run "$BIN/dp0e5_base" zop $ITERS "$pfx-p$p-b"
    else
      perf_run "$BIN/dp0e5_base" zop $ITERS "$pfx-p$p-b"
      perf_run "$abin"  zop $ITERS "$pfx-p$p-a"
    fi
  done
}

pair "$BIN/dp0e5_dp1dp2b"  dp1dp2b  "zop-dp1dp2b"
pair "$BIN/dp0e5_threearm" threearm "zop-threearm"

# w01ctl negative control: arm-independent executed code, 5 pairs
for ((p = 0; p < 5; p++)); do
  if ((p % 2 == 0)); then
    perf_run "$BIN/dp0e5_threearm" w01ctl 200000 "w01ctl-p$p-a"
    perf_run "$BIN/dp0e5_base"     w01ctl 200000 "w01ctl-p$p-b"
  else
    perf_run "$BIN/dp0e5_base"     w01ctl 200000 "w01ctl-p$p-b"
    perf_run "$BIN/dp0e5_threearm" w01ctl 200000 "w01ctl-p$p-a"
  fi
done

echo "ZOP_STAGE_DONE" >> "$LOG"
echo "zop stage complete: $OUT"
