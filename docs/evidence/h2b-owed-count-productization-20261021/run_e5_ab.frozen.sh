#!/usr/bin/env bash
# Formal E5 A/B acceptance runner for issue #564 (H2b productization).
#
# Paired, interleaved, rotation-by-repetition arm order; byte oracle gates
# every repetition; per-arm binary/library hashes recorded for arm identity;
# perf stat attribution on the headline config when counters are permitted.
#
# Usage: run_e5_ab.frozen.sh <worktree-root> <output-dir> [reps] [cpu]
#   <worktree-root>/base  worktree at the BASE sha (unmodified master),
#   <worktree-root>/h2b   worktree at the H2b merge sha; both configured
#   `xmake f -m release --liburing=y -y` with sanitizers EMPTY (verify
#   .xmake/linux/x86_64/xmake.conf before building).
#
# Acceptance rule (fixed before the run, not re-litigable):
#   PASS  = every config's median cpu_ns_per_op ratio <= 1.03
#           AND the sign test over the 15 paired ratios rejects a
#           majority-regression at the 5% level for every config
#           (i.e. at most 10 of 15 pairs with ratio > 1.03).
#   The K>0 zero-op burst configs are mandatory and use the same rule.
set -euo pipefail

WT_ROOT="${1:?usage: run_e5_ab.frozen.sh <worktree-root> <output-dir> [reps] [cpu]}"
OUT_DIR="${2:?usage: run_e5_ab.frozen.sh <worktree-root> <output-dir> [reps] [cpu]}"
REPS="${3:-15}"
CPU="${4:-2}"
HERE="$(cd "$(dirname "$0")" && pwd)"
FIXTURE_DIR="${SLUICE_H2B_FIXTURE_DIR:-/tmp/sluice-h2b-fixture}"
FIXTURE="$FIXTURE_DIR/fixture.bin"
ARMS=(base h2b)
CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"

mkdir -p "$OUT_DIR" "$FIXTURE_DIR"
cd "$HERE"

{
  echo "status=FORMAL_E5"
  echo "host=$(uname -n)"
  echo "kernel=$(uname -r)"
  echo "cpu_model=$CPU_MODEL"
  echo "pinned_cpu=$CPU"
  echo "reps=$REPS"
  echo "compiler=$(g++ --version | head -1)"
  echo "harness_flags=-std=c++20 -O3 -DNDEBUG"
  echo "fixture=$FIXTURE"
  echo "acceptance=median<=1.03 AND sign-test<=10/15 per config"
  echo "perf_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
} | tee "$OUT_DIR/provenance.txt"

for arm in "${ARMS[@]}"; do
  wt="$WT_ROOT/$arm"
  lib="$wt/build/linux/x86_64/release/libsluice_async.a"
  if [ ! -f "$lib" ]; then
    echo "building arm $arm" >&2
    (cd "$wt" && xmake f -m release --liburing=y -y >/dev/null && xmake build -y >/dev/null)
  fi
  if grep -q 'sanitizers = ""' "$wt/.xmake/linux/x86_64/xmake.conf"; then :; else
    echo "refusing: arm $arm config is not sanitizer-clean" >&2; exit 2
  fi
  mkdir -p "$wt/bench/native-linux-m2"
  cp "$(git rev-parse --show-toplevel 2>/dev/null || echo .)/bench/native-linux-m2/pipeline_ab.cpp" \
     "$wt/bench/native-linux-m2/pipeline_ab.cpp"
  g++ -std=c++20 -O3 -DNDEBUG -I"$wt/include" -I"$wt/bench" \
    "$wt/bench/native-linux-m2/pipeline_ab.cpp" \
    "$wt/build/linux/x86_64/release/libsluice_async.a" \
    "$wt/build/linux/x86_64/release/libsluice_core.a" \
    -luring -lpthread -o "$OUT_DIR/pipeline_ab_$arm"
  g++ -std=c++20 -O3 -DNDEBUG -I"$wt/include" -I"$wt/bench" \
    "$HERE/provisional-wsl2-ab/zero_burst_ab.cpp" \
    "$wt/build/linux/x86_64/release/libsluice_async.a" \
    "$wt/build/linux/x86_64/release/libsluice_core.a" \
    -luring -lpthread -o "$OUT_DIR/zero_burst_$arm"
  {
    echo "arm=$arm"
    echo "worktree_sha=$(git -C "$wt" rev-parse HEAD)"
    echo "worktree_dirty=$(git -C "$wt" status --porcelain | wc -l)"
    echo "binary_sha256=$(sha256sum "$OUT_DIR/pipeline_ab_$arm" | awk '{print $1}')"
    echo "zero_binary_sha256=$(sha256sum "$OUT_DIR/zero_burst_$arm" | awk '{print $1}')"
    echo "lib_async_sha256=$(sha256sum "$wt/build/linux/x86_64/release/libsluice_async.a" | awk '{print $1}')"
    echo "lib_core_sha256=$(sha256sum "$wt/build/linux/x86_64/release/libsluice_core.a" | awk '{print $1}')"
  } >> "$OUT_DIR/arm-identity.txt"
done

"$OUT_DIR/pipeline_ab_base" --fixture="$FIXTURE" --create-fixture

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

JSONL="$OUT_DIR/ab.jsonl"
: > "$JSONL"

for rep in $(seq 1 "$REPS"); do
  ci=0
  for cfg in "${DATA_CONFIGS[@]}"; do
    read -r block requests capacity depth <<< "$cfg"
    if (( (ci + rep - 1) % 2 == 0 )); then order="base h2b"; else order="h2b base"; fi
    for arm in $order; do
      taskset -c "$CPU" "$OUT_DIR/pipeline_ab_$arm" --arm="$arm" --backend=uring \
        --block="$block" --depth="$depth" --capacity="$capacity" \
        --requests="$requests" --warmup=1 --fixture="$FIXTURE" \
        | sed "s/^{/{\"rep\":$rep,/" >> "$JSONL"
    done
    ci=$((ci + 1))
  done
  zi=0
  for cfg in "${ZERO_CONFIGS[@]}"; do
    read -r requests depth capacity <<< "$cfg"
    if (( (zi + rep - 1) % 2 == 0 )); then order="base h2b"; else order="h2b base"; fi
    for arm in $order; do
      taskset -c "$CPU" "$OUT_DIR/zero_burst_$arm" --arm="$arm" --depth="$depth" \
        --capacity="$capacity" --requests="$requests" --warmup=1 --fixture="$FIXTURE" \
        | sed "s/^{/{\"rep\":$rep,/" >> "$JSONL"
    done
    zi=$((zi + 1))
  done
  echo "rep $rep/$REPS complete" >&2
done

# perf attribution on the headline config, both arms, when counters permit.
if [ "$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 3)" -le 0 ]; then
  for pair in 1 2 3; do
    for arm in base h2b; do
      perf stat -e task-clock,instructions,cycles -x, -C "$CPU" \
        -o "$OUT_DIR/perf_${arm}_${pair}.txt" -- \
        taskset -c "$CPU" "$OUT_DIR/pipeline_ab_$arm" --arm="$arm" --backend=uring \
        --block=4096 --depth=1 --capacity=1024 --requests=20000 --warmup=1 \
        --fixture="$FIXTURE" > /dev/null
    done
  done
else
  echo "perf attribution skipped: perf_event_paranoid blocks hardware counters" \
    >> "$OUT_DIR/provenance.txt"
fi

echo "outputs in $OUT_DIR (ab.jsonl, arm-identity.txt, provenance.txt, perf_*)"
echo "apply the acceptance rule above to ab.jsonl before recording PASS/FAIL."
