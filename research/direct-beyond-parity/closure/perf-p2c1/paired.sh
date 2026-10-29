#!/usr/bin/env bash
# ABBA interleaved paired series for one ladder binary: alternates arm order
# per pair (AB then BA), pinning cpu2 like the #600 precedent.
# usage: paired.sh <ladder> <armA> <armB> <size> <ops> <warmup> <reps> <pairs> <file> [pattern] [seed] [state]
set -euo pipefail
ladder=$1 armA=$2 armB=$3 size=$4 ops=$5 warmup=$6 reps=$7 pairs=$8 file=$9
shift 9 || true
extra=("$@")
for ((p = 0; p < pairs; p++)); do
    if ((p % 2 == 0)); then
        taskset -c 2 "$ladder" "$armA" "$size" "$ops" "$warmup" "$reps" "$file" "${extra[@]+"${extra[@]}"}"
        taskset -c 2 "$ladder" "$armB" "$size" "$ops" "$warmup" "$reps" "$file" "${extra[@]+"${extra[@]}"}"
    else
        taskset -c 2 "$ladder" "$armB" "$size" "$ops" "$warmup" "$reps" "$file" "${extra[@]+"${extra[@]}"}"
        taskset -c 2 "$ladder" "$armA" "$size" "$ops" "$warmup" "$reps" "$file" "${extra[@]+"${extra[@]}"}"
    fi
done
