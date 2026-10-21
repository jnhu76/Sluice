#!/usr/bin/env bash
# M2 native-Linux A/B runner.
#
# Compiles the same research harness against each arm's static library and
# measures a paired, rotated arm order over the real Sluice RequestScope
# pipeline. Every repetition runs the byte oracle; a failing oracle aborts
# the campaign instead of producing a timing row.
#
# Usage: run_ab.sh <worktree-root> <output-dir> [reps] [cpu]
#   <worktree-root> holds base/ h1/ h2a/ h2b/ h2ab/ worktrees built with
#   `xmake f -m release --liburing=y`.
set -euo pipefail

WT_ROOT="${1:?usage: run_ab.sh <worktree-root> <output-dir> [reps] [cpu]}"
OUT_DIR="${2:?usage: run_ab.sh <worktree-root> <output-dir> [reps] [cpu]}"
REPS="${3:-15}"
CPU="${4:-2}"
HARNESS_DIR="$(cd "$(dirname "$0")" && pwd)"
FIXTURE_DIR="${SLUICE_M2_FIXTURE_DIR:-/tmp/sluice-m2-fixture}"
FIXTURE="$FIXTURE_DIR/data.bin"
ARMS=(base h1 h2a h2b h2ab)

mkdir -p "$OUT_DIR" "$FIXTURE_DIR"
cd "$HARNESS_DIR"

{
  echo "git_sha=$(git rev-parse HEAD)"
  echo "git_dirty=$(git status --porcelain | wc -l)"
  echo "compiler=$(g++ --version | head -1)"
  echo "kernel=$(uname -r)"
  echo "cpu_model=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
  echo "pinned_cpu=$CPU"
  echo "reps=$REPS"
  echo "governor=$(cat /sys/devices/system/cpu/cpu$CPU/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
  echo "harness_flags=-std=c++20 -O3 -DNDEBUG"
  echo "fixture=$FIXTURE"
} | tee "$OUT_DIR/provenance.txt"

for arm in "${ARMS[@]}"; do
  wt="$WT_ROOT/$arm"
  lib="$wt/build/linux/x86_64/release/libsluice_async.a"
  if [ ! -f "$lib" ]; then
    echo "building arm $arm" >&2
    (cd "$wt" && xmake f -m release --liburing=y -y >/dev/null && xmake build -g bench -y >/dev/null)
  fi
  mkdir -p "$wt/bench/native-linux-m2"
  cp pipeline_ab.cpp "$wt/bench/native-linux-m2/pipeline_ab.cpp"
  g++ -std=c++20 -O3 -DNDEBUG -I"$wt/include" -I"$wt/bench" \
    "$wt/bench/native-linux-m2/pipeline_ab.cpp" \
    "$wt/build/linux/x86_64/release/libsluice_async.a" \
    "$wt/build/linux/x86_64/release/libsluice_core.a" \
    -luring -lpthread -o "$OUT_DIR/pipeline_ab_$arm"
  {
    echo "arm=$arm"
    echo "binary_sha256=$(sha256sum "$OUT_DIR/pipeline_ab_$arm" | awk '{print $1}')"
    echo "lib_async_sha256=$(sha256sum "$wt/build/linux/x86_64/release/libsluice_async.a" | awk '{print $1}')"
    echo "lib_core_sha256=$(sha256sum "$wt/build/linux/x86_64/release/libsluice_core.a" | awk '{print $1}')"
  } >> "$OUT_DIR/arm-identity.txt"
  echo "--- $arm ---" >> "$OUT_DIR/arm-identity.txt"
done

"$OUT_DIR/pipeline_ab_base" --fixture="$FIXTURE" --create-fixture

CONFIGS=(
  "4096 20000 16 1"
  "4096 20000 16 4"
  "4096 20000 16 8"
  "4096 20000 64 1"
  "4096 20000 64 4"
  "4096 20000 64 8"
  "4096 20000 256 1"
  "4096 20000 256 4"
  "4096 20000 256 8"
  "4096 20000 1024 1"
  "4096 20000 1024 4"
  "4096 20000 1024 8"
  "65536 2000 64 1"
  "65536 2000 64 8"
  "65536 2000 1024 1"
  "65536 2000 1024 8"
  "1048576 500 64 1"
  "1048576 500 64 8"
  "1048576 500 1024 1"
  "1048576 500 1024 8"
)

JSONL="$OUT_DIR/ab.jsonl"
FREQ="$OUT_DIR/frequency.jsonl"
: > "$JSONL"
: > "$FREQ"

read_freq() {
  cat "/sys/devices/system/cpu/cpu$CPU/cpufreq/scaling_cur_freq" 2>/dev/null || echo 0
}

for rep in $(seq 1 "$REPS"); do
  for ci in $(seq 0 $((${#CONFIGS[@]} - 1))); do
    idx=$(( (ci + rep - 1) % ${#CONFIGS[@]} ))
    read -r block requests capacity depth <<< "${CONFIGS[$idx]}"
    for ai in $(seq 0 $((${#ARMS[@]} - 1))); do
      arm_idx=$(( (ai + rep - 1) % ${#ARMS[@]} ))
      arm="${ARMS[$arm_idx]}"
      f0=$(read_freq)
      taskset -c "$CPU" "$OUT_DIR/pipeline_ab_$arm" \
        --arm="$arm" --backend=uring --block="$block" --depth="$depth" \
        --capacity="$capacity" --requests="$requests" --warmup=1 \
        --fixture="$FIXTURE" | sed "s/^{/{\"rep\":$rep,/" >> "$JSONL"
      f1=$(read_freq)
      printf '{"rep":%d,"arm":"%s","block":%s,"capacity":%s,"depth":%s,"freq_before":%s,"freq_after":%s,"loadavg":"%s"}\n' \
        "$rep" "$arm" "$block" "$capacity" "$depth" "$f0" "$f1" "$(cut -d' ' -f1-3 /proc/loadavg)" >> "$FREQ"
    done
  done
  echo "rep $rep/$REPS complete" >&2
done

echo "outputs in $OUT_DIR (ab.jsonl, frequency.jsonl, provenance.txt, arm-identity.txt)"
