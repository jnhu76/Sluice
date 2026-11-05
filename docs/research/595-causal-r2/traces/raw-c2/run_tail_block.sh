#!/bin/bash
# C2 strace tail-impact sub-block: full 3-pass mode (latency pass included),
# MEASURED=1, to read the bench's own p50/p95/p99 under strace vs not.
set -u
RAW=/home/jnhu/sluice-595-e5/causal-r2/evidence/traces/raw-c2
BIN=/home/jnhu/sluice-595-e5/causal-r2/worktree/build/linux/x86_64/release/bench_w02_pipeline
cd "$RAW" || exit 1
run_one() { # $1=mode(on|off) $2=arm
  local mode=$1 arm=$2
  local tag="c2tail-${mode}-${arm}"
  local base="$RAW/${tag}"
  local armenv=()
  if [ "$arm" = B ]; then armenv=(SLUICE_595_ARM=B); else armenv=(-u SLUICE_595_ARM); fi
  if [ "$mode" = on ]; then
    env "${armenv[@]}" SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=1 \
      'SLUICE_BENCH_CASE_FILTER==W02-uring-4k-d2' SLUICE_BENCH_595_DEPTHS=1 \
      SLUICE_BENCH_STATS=1 SLUICE_BENCH_REUSE_FIXTURE=1 \
      SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-bench SLUICE_BENCH_JSONL="${base}.jsonl" \
      taskset -c 2 /usr/sbin/strace -f -ttt -T \
      -e trace=io_uring_setup,io_uring_register,io_uring_enter,eventfd2,read,write,poll,ppoll,futex \
      -o "${base}.strace.txt" "$BIN" > "${base}.stdout" 2> "${base}.stderr"
  else
    env "${armenv[@]}" SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=1 \
      'SLUICE_BENCH_CASE_FILTER==W02-uring-4k-d2' SLUICE_BENCH_595_DEPTHS=1 \
      SLUICE_BENCH_STATS=1 SLUICE_BENCH_REUSE_FIXTURE=1 \
      SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-bench SLUICE_BENCH_JSONL="${base}.jsonl" \
      taskset -c 2 "$BIN" > "${base}.stdout" 2> "${base}.stderr"
  fi
  echo "== ${tag} rc=$?"
  grep "W02-uring-4k-d2 rep=0" "${base}.stdout" || true
}
run_one off A
run_one off B
run_one on  A
run_one on  B
