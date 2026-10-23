#!/usr/bin/env bash
# E5 attribution stage 2: W02-uring-4k-d1, base vs dp1 alone and base vs
# dp2b alone (locates which arm carries the d1 wait_for regression).
set -euo pipefail
E5=/tmp/e5
CPU=2
PAIRS=15
export SLUICE_BENCH_TMP_DIR=/tmp/e5-fixture
mkdir -p /tmp/e5-fixture

BASE=$HOME/Source/Sluice/build-e5/linux/x86_64/release
DP1=/tmp/e5/wt-dp1/build-e5/linux/x86_64/release
DP2B=/tmp/e5/wt-dp2b/build-e5/linux/x86_64/release

run_pair_set() { # armbin outdir
  local armbin=$1 outdir=$2
  mkdir -p "$outdir"
  for ((p = 0; p < PAIRS; p++)); do
    if ((p % 2 == 0)); then
      taskset -c $CPU env SLUICE_BENCH_TMP_DIR=/tmp/e5-fixture \
        SLUICE_BENCH_CASE_FILTER="=W02-uring-4k-d1" SLUICE_BENCH_WARMUP=1 \
        SLUICE_BENCH_MEASURED=3 SLUICE_BENCH_JSONL="$outdir/W02-uring-4k-d1-p$p-a.jsonl" \
        "$armbin" > "$outdir/W02-uring-4k-d1-p$p-a.stdout" 2>&1
      taskset -c $CPU env SLUICE_BENCH_TMP_DIR=/tmp/e5-fixture \
        SLUICE_BENCH_CASE_FILTER="=W02-uring-4k-d1" SLUICE_BENCH_WARMUP=1 \
        SLUICE_BENCH_MEASURED=3 SLUICE_BENCH_JSONL="$outdir/W02-uring-4k-d1-p$p-b.jsonl" \
        "$BASE/bench_w02_pipeline" > "$outdir/W02-uring-4k-d1-p$p-b.stdout" 2>&1
    else
      taskset -c $CPU env SLUICE_BENCH_TMP_DIR=/tmp/e5-fixture \
        SLUICE_BENCH_CASE_FILTER="=W02-uring-4k-d1" SLUICE_BENCH_WARMUP=1 \
        SLUICE_BENCH_MEASURED=3 SLUICE_BENCH_JSONL="$outdir/W02-uring-4k-d1-p$p-b.jsonl" \
        "$BASE/bench_w02_pipeline" > "$outdir/W02-uring-4k-d1-p$p-b.stdout" 2>&1
      taskset -c $CPU env SLUICE_BENCH_TMP_DIR=/tmp/e5-fixture \
        SLUICE_BENCH_CASE_FILTER="=W02-uring-4k-d1" SLUICE_BENCH_WARMUP=1 \
        SLUICE_BENCH_MEASURED=3 SLUICE_BENCH_JSONL="$outdir/W02-uring-4k-d1-p$p-a.jsonl" \
        "$armbin" > "$outdir/W02-uring-4k-d1-p$p-a.stdout" 2>&1
    fi
    echo "$(date -u +%H:%M:%S) pair $p done ($outdir)" >> "$outdir/campaign.log"
  done
}

run_pair_set "$DP1/bench_w02_pipeline"  /tmp/e5/results/attr-d1-dp1
run_pair_set "$DP2B/bench_w02_pipeline" /tmp/e5/results/attr-d1-dp2b
echo ATTR2_DONE
