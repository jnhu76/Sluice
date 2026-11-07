#!/bin/bash
# CF-2 deferoff C2-shim companion (LD_PRELOAD interposer, single instrumentation).
# frozen-B and deferoff-B, 2 invocations each, WARMUP=1 MEASURED=3 (C2 block shape).
set -u
RAW=/home/jnhu/sluice-595-e5/causal-r2/evidence/counterfactual/raw/shim-companion-cf2
SHIM=/tmp/c2-shim2/c2_liburing_shim.so
mkdir -p "$RAW"
cd "$RAW" || exit 1

run_one() { # $1=label $2=binary $3=idx
  local label=$1 bin=$2 idx=$3
  local base="$RAW/c2shim-${label}-B-${idx}"
  local la
  la=$(awk '{print $1}' /proc/loadavg)
  if ! awk -v l="$la" 'BEGIN{exit !(l < 1.5)}'; then sleep 10; fi
  local rc
  env SLUICE_595_ARM=B \
    LD_PRELOAD="$SHIM" SLUICE595_C2_SHIM_OUT="${base}.shim" \
    SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 \
    'SLUICE_BENCH_CASE_FILTER==W02-uring-4k-d2' SLUICE_BENCH_595_DEPTHS=1 \
    SLUICE_BENCH_STATS=1 SLUICE_BENCH_PMU_ONLY=1 SLUICE_BENCH_REUSE_FIXTURE=1 \
    SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-bench \
    SLUICE_BENCH_JSONL="${base}.jsonl" \
    taskset -c 2 "$bin" > "${base}.stdout" 2> "${base}.stderr"
  rc=$?
  echo "label=${label} arm=B idx=${idx} rc=${rc} loadavg='$(cat /proc/loadavg)'" \
    | tee -a "$RAW/runlog.txt"
}

: > "$RAW/runlog.txt"
run_one frozen  "$RAW/../sysio-cf2/bench_w02_pipeline_frozen"  1
run_one deferoff "$RAW/../sysio-cf2/bench_w02_pipeline_deferoff" 1
run_one deferoff "$RAW/../sysio-cf2/bench_w02_pipeline_deferoff" 2
run_one frozen  "$RAW/../sysio-cf2/bench_w02_pipeline_frozen"  2
echo "SHIM CF2 DONE $(date -Is)" | tee -a "$RAW/runlog.txt"
