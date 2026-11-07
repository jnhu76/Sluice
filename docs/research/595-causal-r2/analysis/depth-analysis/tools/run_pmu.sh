#!/bin/sh
set -e
RUN=/home/jnhu/sluice-595-e5/causal-r2/evidence/counters/tools/run_blocks.py
BIN=/home/jnhu/sluice-595-e5/causal-r2/evidence/counters/bin/bench_w02_pipeline_pmu
OUT=/home/jnhu/sluice-595-e5/causal-r2/evidence/depth-analysis/raw
python3 $RUN --label pmu-d3-ab --mode pmu --bin $BIN --pairs 8 --arms AB \
  --case '=W02-uring-4k-d3' --warmup 1 --measured 1 --pmu-only --outdir $OUT/pmu-d3-ab
python3 $RUN --label pmu-d16-ab --mode pmu --bin $BIN --pairs 8 --arms AB \
  --case '=W02-uring-4k-d16' --warmup 1 --measured 1 --pmu-only --outdir $OUT/pmu-d16-ab
echo PMU_DONE
