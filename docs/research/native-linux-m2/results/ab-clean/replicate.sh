#!/usr/bin/env bash
# Idle-window replication used for results/ab-clean.
#
# This file is reconstructed from the heredoc that was executed on E5; the
# frozen output is results/ab-clean/ab.jsonl (not committed, SHA-256 in
# end-to-end-ab.md). It uses the already-built ab-15 binaries and does not
# rebuild or re-apply patches.
set -euo pipefail

OUT="$HOME/sluice-m2/out/ab-clean"
BIN="$HOME/sluice-m2/out/ab-15"
FIX=/tmp/sluice-m2-fixture/data.bin
mkdir -p "$OUT"
: > "$OUT/ab.jsonl"
: > "$OUT/frequency.jsonl"

CONFIGS=(
  "4096 20000 1024 1"
  "4096 20000 1024 4"
  "4096 20000 1024 8"
  "4096 20000 64 1"
  "4096 20000 64 8"
  "65536 2000 1024 1"
  "65536 2000 64 8"
)
ARMS=(base h1 h2a h2b h2ab)

for rep in $(seq 1 15); do
  for ci in $(seq 0 $((${#CONFIGS[@]} - 1))); do
    idx=$(( (ci + rep - 1) % ${#CONFIGS[@]} ))
    read -r block requests capacity depth <<< "${CONFIGS[$idx]}"
    for ai in $(seq 0 4); do
      arm_idx=$(( (ai + rep - 1) % 5 ))
      arm="${ARMS[$arm_idx]}"
      f0=$(cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
      taskset -c 2 "$BIN/pipeline_ab_$arm" --arm="$arm" --backend=uring \
        --block="$block" --depth="$depth" --capacity="$capacity" \
        --requests="$requests" --warmup=1 --fixture="$FIX" \
        | sed "s/^{/{\"rep\":$rep,/" >> "$OUT/ab.jsonl"
      f1=$(cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
      printf '{"rep":%d,"arm":"%s","block":%s,"capacity":%s,"depth":%s,"freq_before":%s,"freq_after":%s,"loadavg":"%s"}\n' \
        "$rep" "$arm" "$block" "$capacity" "$depth" "$f0" "$f1" \
        "$(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT/frequency.jsonl"
    done
  done
done
