#!/bin/bash
# C2 liburing-shim ON/OFF block (native speed; LD_PRELOAD interposer).
set -u
RAW=/home/jnhu/sluice-595-e5/causal-r2/evidence/traces/raw-c2
BIN=/home/jnhu/sluice-595-e5/causal-r2/worktree/build/linux/x86_64/release/bench_w02_pipeline
SHIM=/tmp/c2-shim/c2_liburing_shim.so
cd "$RAW" || exit 1

run_one() { # $1=mode(shim|noshim) $2=arm $3=idx
  local mode=$1 arm=$2 idx=$3
  local tag="c2shim-${mode}-${arm}-${idx}"
  local base="$RAW/${tag}"
  local la
  la=$(awk '{print $1}' /proc/loadavg)
  if ! awk -v l="$la" 'BEGIN{exit !(l < 1.5)}'; then sleep 10; fi
  local t0 t1 rc
  t0=$(date +%s.%N)
  local armenv=()
  if [ "$arm" = B ]; then armenv=(SLUICE_595_ARM=B); else armenv=(-u SLUICE_595_ARM); fi
  if [ "$mode" = shim ]; then
    env "${armenv[@]}" \
      LD_PRELOAD="$SHIM" SLUICE595_C2_SHIM_OUT="${base}.shim" \
      SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 \
      'SLUICE_BENCH_CASE_FILTER==W02-uring-4k-d2' SLUICE_BENCH_595_DEPTHS=1 \
      SLUICE_BENCH_STATS=1 SLUICE_BENCH_PMU_ONLY=1 SLUICE_BENCH_REUSE_FIXTURE=1 \
      SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-bench \
      SLUICE_BENCH_JSONL="${base}.jsonl" \
      taskset -c 2 "$BIN" > "${base}.stdout" 2> "${base}.stderr"
    rc=$?
  else
    env "${armenv[@]}" \
      SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 \
      'SLUICE_BENCH_CASE_FILTER==W02-uring-4k-d2' SLUICE_BENCH_595_DEPTHS=1 \
      SLUICE_BENCH_STATS=1 SLUICE_BENCH_PMU_ONLY=1 SLUICE_BENCH_REUSE_FIXTURE=1 \
      SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-bench \
      SLUICE_BENCH_JSONL="${base}.jsonl" \
      taskset -c 2 "$BIN" > "${base}.stdout" 2> "${base}.stderr"
    rc=$?
  fi
  t1=$(date +%s.%N)
  echo "tag=${tag} rc=${rc} loadavg='$(cat /proc/loadavg)' elapsed_s=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')" | tee -a "$RAW/shim-runlog.txt"
}

: > "$RAW/shim-runlog.txt"
run_one shim  A 1
run_one shim  B 1
run_one noshim A 1
run_one noshim B 1
run_one shim  B 2
run_one shim  A 2
run_one noshim B 2
run_one noshim A 2
echo "SHIM BLOCK DONE $(date -Is)" | tee -a "$RAW/shim-runlog.txt"
