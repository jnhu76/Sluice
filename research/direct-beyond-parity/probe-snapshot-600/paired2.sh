#!/usr/bin/env bash
# #600 paired binary series: alternates two ladder binaries ABBA-style, tagging
# each output line with its binary identity.
# usage: paired2.sh <binBase> <binCand> <arm> <size> <ops> <warmup> <reps> <pairs> <file>
set -euo pipefail
cd "$(dirname "$0")"
binBase=$1 binCand=$2 arm=$3 size=$4 ops=$5 warmup=$6 reps=$7 pairs=$8 file=$9
for ((p = 0; p < pairs; p++)); do
    if ((p % 2 == 0)); then
        taskset -c 2 "$binBase"  "$arm" "$size" "$ops" "$warmup" "$reps" "$file" | sed "s/^arm=/arm=B0_base_/"
        taskset -c 2 "$binCand"  "$arm" "$size" "$ops" "$warmup" "$reps" "$file" | sed "s/^arm=/arm=C0_cand_/"
    else
        taskset -c 2 "$binCand"  "$arm" "$size" "$ops" "$warmup" "$reps" "$file" | sed "s/^arm=/arm=C0_cand_/"
        taskset -c 2 "$binBase"  "$arm" "$size" "$ops" "$warmup" "$reps" "$file" | sed "s/^arm=/arm=B0_base_/"
    fi
done
