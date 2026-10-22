#!/usr/bin/env bash
# MW-HW-1 research-only paired A/B runner (WSL2).
# Arms: base (master 8e2aa5ab), e1, e2, e3 (isolated single-mechanism patches).
# Paired rotated repetitions; every repetition's oracle phase must pass.
set -euo pipefail
BIN=/tmp/mw-hw-1-out/bin
FIX=/tmp/mw-hw-1-out/fixture/data.bin
OUT=/tmp/mw-hw-1-out/ab
REPS=${1:-9}
CPU=${2:-2}
ARMS=(base e1 e2 e3)
# config: block:depth:capacity:requests
CONFIGS=(
  "4096:1:1024:20000"
  "4096:1:16:20000"
  "4096:8:1024:20000"
  "4096:64:64:20000"
  "65536:8:1024:2000"
  "1048576:8:64:500"
)
mkdir -p "$OUT"
: > "$OUT/rows.jsonl"
: > "$OUT/run-fails.txt"
for rep in $(seq 1 "$REPS"); do
  for cfg in "${CONFIGS[@]}"; do
    IFS=: read blk dep cap req <<< "$cfg"
    n=${#ARMS[@]}
    for ai in $(seq 0 $((n-1))); do
      arm_idx=$(( (ai + rep - 1) % n ))
      arm=${ARMS[$arm_idx]}
      taskset -c "$CPU" "$BIN/pipeline_ab_$arm" --arm=$arm --backend=uring \
        --block=$blk --depth=$dep --capacity=$cap --requests=$req --warmup=1 \
        --fixture="$FIX" > "$OUT/one.out" 2>/dev/null || { echo "RUN-FAIL $arm $cfg" >> "$OUT/run-fails.txt"; continue; }
      cat "$OUT/one.out" >> "$OUT/rows.jsonl"
    done
  done
  echo "rep $rep done" >&2
done
echo "rows: $(wc -l < "$OUT/rows.jsonl")"
