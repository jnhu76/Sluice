#!/bin/sh
set -e
BIN=/home/jnhu/sluice-595-e5/causal-r2/evidence/counters/bin/bench_w02_pipeline_frozen
BASE=/home/jnhu/sluice-595-e5/causal-r2/evidence/depth-analysis/raw
for D in 3 16; do
  CASE="W02-uring-4k-d$D"
  for ARM in A B; do
    for I in 0 1 2; do
      O="$BASE/strace-d$D/$ARM-$I"
      mkdir -p "$O"
      if [ "$ARM" = B ]; then export SLUICE_595_ARM=B; else unset SLUICE_595_ARM; fi
      env SLUICE_BENCH_595_DEPTHS=1 SLUICE_BENCH_CASE_FILTER="=$CASE" \
          SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=1 SLUICE_BENCH_STATS=1 \
          SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-cost \
          SLUICE_BENCH_JSONL="$O/bench.jsonl" \
          taskset -c 2 strace -f -c -e trace=io_uring_enter,io_uring_setup,io_uring_register,write,read,ppoll,poll,futex,eventfd2 \
          -o "$O/summary.txt" "$BIN" > "$O/stdout.txt" 2> "$O/stderr.txt"
      echo "done $CASE $ARM $I rc=$?"
    done
  done
done
# detailed enter argument trace (to_submit distribution)
for D in 3 16; do
  CASE="W02-uring-4k-d$D"
  for ARM in A B; do
    for I in 0 1; do
      O="$BASE/enterargs-d$D/$ARM-$I"
      mkdir -p "$O"
      if [ "$ARM" = B ]; then export SLUICE_595_ARM=B; else unset SLUICE_595_ARM; fi
      env SLUICE_BENCH_595_DEPTHS=1 SLUICE_BENCH_CASE_FILTER="=$CASE" \
          SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=1 SLUICE_BENCH_STATS=1 \
          SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-cost \
          SLUICE_BENCH_JSONL="$O/bench.jsonl" \
          taskset -c 2 strace -f -e trace=io_uring_enter -o "$O/enter.trace" "$BIN" \
          > "$O/stdout.txt" 2> "$O/stderr.txt"
      echo "done enterargs $CASE $ARM $I rc=$?"
    done
  done
done
echo SYSCALL_DONE
