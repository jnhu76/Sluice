#!/usr/bin/env bash
# #600 E3 interleaved paired series: prints one ns/op line per invocation of
# the ladder binary, alternating arm order per pair (AB then BA).
# usage: paired.sh <armA> <armB> <size> <ops> <warmup> <reps> <pairs> <file>
set -euo pipefail
cd "$(dirname "$0")"
armA=$1 armB=$2 size=$3 ops=$4 warmup=$5 reps=$6 pairs=$7 file=$8
for ((p = 0; p < pairs; p++)); do
    if ((p % 2 == 0)); then
        taskset -c 2 ./ladder "$armA" "$size" "$ops" "$warmup" "$reps" "$file"
        taskset -c 2 ./ladder "$armB" "$size" "$ops" "$warmup" "$reps" "$file"
    else
        taskset -c 2 ./ladder "$armB" "$size" "$ops" "$warmup" "$reps" "$file"
        taskset -c 2 ./ladder "$armA" "$size" "$ops" "$warmup" "$reps" "$file"
    fi
done
