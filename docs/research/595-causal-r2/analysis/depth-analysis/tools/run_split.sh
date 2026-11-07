#!/bin/sh
set -e
RUN=/home/jnhu/sluice-595-e5/causal-r2/evidence/counters/tools/run_blocks.py
BIN=/home/jnhu/sluice-595-e5/causal-r2/evidence/counters/bin/bench_w02_pipeline_pmu
OUT=/home/jnhu/sluice-595-e5/causal-r2/evidence/depth-analysis/raw
python3 $RUN --label split-d3 --mode direct --bin $BIN --pairs 12 --arms AB \
  --case '=W02-uring-4k-d3' --warmup 1 --measured 20 --pmu-only --outdir $OUT/split-d3
python3 $RUN --label split-d16 --mode direct --bin $BIN --pairs 12 --arms AB \
  --case '=W02-uring-4k-d16' --warmup 1 --measured 20 --pmu-only --outdir $OUT/split-d16
echo SPLIT_DONE
