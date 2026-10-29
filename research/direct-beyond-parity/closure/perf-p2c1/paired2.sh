#!/usr/bin/env bash
# Two-binary ABBA interleaved paired series for one arm: same arm selector on
# two ladder builds (candidate vs baseline archive link), binary order
# alternating per pair, pinning cpu2 like paired.sh. One process invocation
# per arm sample; sample lines keep the single-binary format.
# Binary identity is the emission order under the ABBA schedule: pair p even
# emits ladderA then ladderB, pair p odd emits ladderB then ladderA. The
# per-pair statistic is always ladderA - ladderB (flip the odd pairs); the
# name-paired analyze_pairs.py cannot distinguish the two builds because both
# print the same arm name — analyze two-binary series order-aware.
# usage: paired2.sh <ladderA> <ladderB> <arm> <size> <ops> <warmup> <reps> <pairs> <file> [pattern] [seed] [state] [prime]
set -euo pipefail
ladderA=$1 ladderB=$2 arm=$3 size=$4 ops=$5 warmup=$6 reps=$7 pairs=$8 file=$9
shift 9 || true
extra=("$@")
for ((p = 0; p < pairs; p++)); do
    if ((p % 2 == 0)); then
        taskset -c 2 "$ladderA" "$arm" "$size" "$ops" "$warmup" "$reps" "$file" "${extra[@]+"${extra[@]}"}"
        taskset -c 2 "$ladderB" "$arm" "$size" "$ops" "$warmup" "$reps" "$file" "${extra[@]+"${extra[@]}"}"
    else
        taskset -c 2 "$ladderB" "$arm" "$size" "$ops" "$warmup" "$reps" "$file" "${extra[@]+"${extra[@]}"}"
        taskset -c 2 "$ladderA" "$arm" "$size" "$ops" "$warmup" "$reps" "$file" "${extra[@]+"${extra[@]}"}"
    fi
done
