#!/usr/bin/env bash
# Provisional paired A/B campaign for issue #564 on the local WSL2 host.
# NOT the formal E5 acceptance run: the E5 credentials are unavailable in
# this session. Paired, interleaved, rotated arm order per repetition.
set -euo pipefail
cd /tmp/sluice-h2b-ab
OUT=/tmp/sluice-h2b-ab/results
mkdir -p "$OUT"
FIXTURE=/tmp/sluice-h2b-ab/fixture.bin
REPS=15
CPU=2
{
  echo "host=$(uname -n)"
  echo "kernel=$(uname -r)"
  echo "git_base=$(git -C /tmp/sluice-h2b-ab/base rev-parse HEAD)"
  echo "git_h2b=$(git -C /tmp/sluice-h2b-ab/h2b rev-parse HEAD)"
  echo "cpu_model=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
  echo "pinned_cpu=$CPU"
  echo "reps=$REPS"
  echo "compiler=$(g++ --version | head -1)"
  echo "harness_flags=-std=c++20 -O3 -DNDEBUG"
  echo "fixture=$FIXTURE"
  echo "status=PROVISIONAL_WSL2_NOT_E5"
} | tee "$OUT/provenance.txt"

: > "$OUT/ab.jsonl"
DATA_CONFIGS=(
  "4096 20000 16 1"
  "4096 20000 64 1"
  "4096 20000 256 1"
  "4096 20000 1024 1"
  "4096 20000 1024 8"
  "4096 20000 16 8"
  "65536 2000 1024 1"
  "65536 2000 64 1"
  "1048576 500 64 1"
)
ZERO_CONFIGS=(
  "20000 64 1024"
  "20000 8 1024"
)
for rep in $(seq 1 $REPS); do
  ci=0
  for cfg in "${DATA_CONFIGS[@]}"; do
    read -r block requests capacity depth <<< "$cfg"
    idx=$(( (ci + rep - 1) % 2 ))
    if [ $idx -eq 0 ]; then order="base h2b"; else order="h2b base"; fi
    for arm in $order; do
      taskset -c $CPU ./pipeline_ab_$arm --arm=$arm --backend=uring --block=$block \
        --depth=$depth --capacity=$capacity --requests=$requests --warmup=1 \
        --fixture=$FIXTURE | sed "s/^{/{\"rep\":$rep,/" >> "$OUT/ab.jsonl"
    done
    ci=$((ci+1))
  done
  zi=0
  for cfg in "${ZERO_CONFIGS[@]}"; do
    read -r requests depth capacity <<< "$cfg"
    idx=$(( (zi + rep - 1) % 2 ))
    if [ $idx -eq 0 ]; then order="base h2b"; else order="h2b base"; fi
    for arm in $order; do
      taskset -c $CPU ./zero_burst_$arm --arm=$arm --depth=$depth --capacity=$capacity \
        --requests=$requests --warmup=1 --fixture=$FIXTURE | sed "s/^{/{\"rep\":$rep,/" >> "$OUT/ab.jsonl"
    done
    zi=$((zi+1))
  done
  echo "rep $rep/$REPS complete" >&2
done
echo "campaign complete"
