#!/bin/sh
set -e
RUN=/home/jnhu/sluice-595-e5/causal-r2/evidence/counters/tools/run_blocks.py
BIN=/home/jnhu/sluice-595-e5/causal-r2/evidence/counters/bin/bench_w02_pipeline_counters
OUT=/home/jnhu/sluice-595-e5/causal-r2/evidence/depth-analysis/raw
python3 $RUN --label cnt-d3-ab --mode direct --bin $BIN --pairs 12 --arms AB \
  --case '=W02-uring-4k-d3' --warmup 1 --measured 5 --counters 1 \
  --outdir $OUT/cnt-d3-ab
python3 $RUN --label cnt-d16-ab --mode direct --bin $BIN --pairs 12 --arms AB \
  --case '=W02-uring-4k-d16' --warmup 1 --measured 5 --counters 1 \
  --outdir $OUT/cnt-d16-ab
echo ALL_DONE
