#!/bin/sh
set -e
BIN=/home/jnhu/sluice-595-e5/causal-r2/evidence/counterfactual/raw/sysio-cf2/bench_w02_pipeline_deferoff
BASE=/home/jnhu/sluice-595-e5/causal-r2/evidence/depth-analysis/raw
for D in 3 16; do
  O=$BASE/deferoff-d$D; mkdir -p $O
  for Q in 0 1 2 3 4 5 6 7; do
    if [ $((Q % 2)) -eq 0 ]; then ORDER="A B"; else ORDER="B A"; fi
    P=0
    for ARM in $ORDER; do
      J=$O/p$Q-$P-$ARM.jsonl
      if [ "$ARM" = B ]; then export SLUICE_595_ARM=B; else unset SLUICE_595_ARM; fi
      env SLUICE_BENCH_595_DEPTHS=1 SLUICE_BENCH_CASE_FILTER="=W02-uring-4k-d$D" \
          SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=20 SLUICE_BENCH_STATS=1 \
          SLUICE_BENCH_PMU_ONLY=1 SLUICE_BENCH_REUSE_FIXTURE=1 \
          SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-cost SLUICE_BENCH_JSONL=$J \
          taskset -c 2 $BIN > $O/p$Q-$P-$ARM.out 2> $O/p$Q-$P-$ARM.err || echo "FAIL d$D p$Q $ARM"
      P=$((P+1))
    done
  done
  echo "deferoff-d$D done"
done
echo DEFEROFF_DONE
