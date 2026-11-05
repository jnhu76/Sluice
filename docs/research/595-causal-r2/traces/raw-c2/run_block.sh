#!/bin/bash
# C2 syscall-path evidence block: strace ON vs OFF, arms A/B, order-balanced.
# Machine-exclusive phase; each invocation is a fresh bench process on cpu2.
set -u
RAW=/home/jnhu/sluice-595-e5/causal-r2/evidence/traces/raw-c2
BIN=/home/jnhu/sluice-595-e5/causal-r2/worktree/build/linux/x86_64/release/bench_w02_pipeline
STRACE=/usr/sbin/strace
cd "$RAW" || exit 1

wait_load() {
  for _ in $(seq 1 60); do
    local one
    one=$(awk '{print $1}' /proc/loadavg)
    if awk -v l="$one" 'BEGIN{exit !(l < 1.5)}'; then return 0; fi
    sleep 5
  done
  echo "LOADAVG GUARD FAILED" >&2
  return 1
}

run_one() { # $1=mode(on|off) $2=arm(A|B) $3=index(1|2)
  local mode=$1 arm=$2 idx=$3
  local tag="c2d2-${mode}-${arm}-${idx}"
  local base="$RAW/${tag}"
  wait_load || return 1
  local la_before
  la_before=$(cat /proc/loadavg)
  local t0 t1 rc
  t0=$(date +%s.%N)
  if [ "$mode" = on ]; then
    if [ "$arm" = B ]; then
      env SLUICE_595_ARM=B \
        SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 \
        'SLUICE_BENCH_CASE_FILTER==W02-uring-4k-d2' SLUICE_BENCH_595_DEPTHS=1 \
        SLUICE_BENCH_STATS=1 SLUICE_BENCH_PMU_ONLY=1 SLUICE_BENCH_REUSE_FIXTURE=1 \
        SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-bench \
        SLUICE_BENCH_JSONL="${base}.jsonl" \
        taskset -c 2 "$STRACE" -f -ttt -T \
        -e trace=io_uring_setup,io_uring_register,io_uring_enter,eventfd2,read,write,poll,ppoll,futex \
        -o "${base}.strace.txt" "$BIN" > "${base}.stdout" 2> "${base}.stderr"
      rc=$?
    else
      env -u SLUICE_595_ARM \
        SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 \
        'SLUICE_BENCH_CASE_FILTER==W02-uring-4k-d2' SLUICE_BENCH_595_DEPTHS=1 \
        SLUICE_BENCH_STATS=1 SLUICE_BENCH_PMU_ONLY=1 SLUICE_BENCH_REUSE_FIXTURE=1 \
        SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-bench \
        SLUICE_BENCH_JSONL="${base}.jsonl" \
        taskset -c 2 "$STRACE" -f -ttt -T \
        -e trace=io_uring_setup,io_uring_register,io_uring_enter,eventfd2,read,write,poll,ppoll,futex \
        -o "${base}.strace.txt" "$BIN" > "${base}.stdout" 2> "${base}.stderr"
      rc=$?
    fi
  else
    if [ "$arm" = B ]; then
      env SLUICE_595_ARM=B \
        SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 \
        'SLUICE_BENCH_CASE_FILTER==W02-uring-4k-d2' SLUICE_BENCH_595_DEPTHS=1 \
        SLUICE_BENCH_STATS=1 SLUICE_BENCH_PMU_ONLY=1 SLUICE_BENCH_REUSE_FIXTURE=1 \
        SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-bench \
        SLUICE_BENCH_JSONL="${base}.jsonl" \
        taskset -c 2 "$BIN" > "${base}.stdout" 2> "${base}.stderr"
      rc=$?
    else
      env -u SLUICE_595_ARM \
        SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=3 \
        'SLUICE_BENCH_CASE_FILTER==W02-uring-4k-d2' SLUICE_BENCH_595_DEPTHS=1 \
        SLUICE_BENCH_STATS=1 SLUICE_BENCH_PMU_ONLY=1 SLUICE_BENCH_REUSE_FIXTURE=1 \
        SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-bench \
        SLUICE_BENCH_JSONL="${base}.jsonl" \
        taskset -c 2 "$BIN" > "${base}.stdout" 2> "${base}.stderr"
      rc=$?
    fi
  fi
  t1=$(date +%s.%N)
  echo "tag=${tag} rc=${rc} loadavg_before='${la_before}' loadavg_after='$(cat /proc/loadavg)' elapsed_s=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')" | tee -a "$RAW/block-runlog.txt"
  return 0
}

: > "$RAW/block-runlog.txt"
run_one on  A 1
run_one on  B 1
run_one off A 1
run_one off B 1
run_one on  B 2
run_one on  A 2
run_one off B 2
run_one off A 2
echo "BLOCK DONE $(date -Is)" | tee -a "$RAW/block-runlog.txt"
