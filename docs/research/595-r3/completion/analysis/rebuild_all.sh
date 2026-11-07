#!/bin/bash
# #595 R3-completion paper analysis: single rebuild entry from frozen raw data.
# Reproduces statistics (metrics/effects/gates.csv, recomputed.json,
# NUMERIC_CROSSCHECK.json), figures (figures/* + figure-data/*), and the
# numeric QA gate (NUMERIC_QA.md). Reads raw/ only; writes analysis/ figures/.
set -euo pipefail
ROOT=/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z
cd "$ROOT"
python3 analysis/scripts/recompute_stats.py > analysis/rebuild_stats.log
python3 analysis/scripts/build_figures.py > analysis/rebuild_figures.log
python3 analysis/scripts/qa_gate.py > analysis/rebuild_qa.log
tail -1 analysis/rebuild_qa.log
echo "rebuilt: analysis/{metrics,effects,gates}.csv recomputed.json figures/F2-F5 (+figure-data)"
