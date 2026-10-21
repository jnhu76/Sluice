#!/usr/bin/env bash
# Formal E5 A/B acceptance runner for the H2b owed-count productization
# (revision r2: fail-closed identity gates and a machine-computed verdict;
# r1 recorded arm identity but verified nothing and computed no verdict).
#
# Identity protocol (fail-closed):
#   * base and candidate trees are pinned by exact expected SHAs given on
#     the command line; HEAD, clean status and path distinctness are checked
#     before anything is built or measured, and identical arms are refused.
#   * both arms are rebuilt from an empty build/ on every invocation; stale
#     artifacts cannot reach a measurement.
#   * the benchmark harnesses are compiled from single read-only sources
#     (the data harness must hash-identical in both trees); the trees under
#     test are never written.
#   * the linked asynchronous libraries must differ between arms, otherwise
#     the run is not an A/B and is refused.
#   For the open PR the candidate is the PR branch HEAD and the base is the
#   frozen pre-PR master. A post-merge re-acceptance must pin the actual
#   merge commit and its actual base, never a precomputed merge SHA.
#
# Acceptance rule (fixed before the run; computed only by e5_verdict.py):
#   every config's 15 paired cpu_ns_per_op ratios (h2b/base, timed phase)
#   must have median <= 1.03 and at most 10 pairs above 1.03. The pair
#   threshold is an engineering bound: the one-sided binomial tail
#   P(X >= 11 | n = 15, p = 0.5) is 0.059, so the rule rejects a
#   majority-regression at roughly the 6% level, not at an exact 5%.
#   The K>0 zero-op burst configs are mandatory and use the same rule.
#   A run that ends without a verdict.json is not an acceptance.
#
# Usage:
#   run_e5_ab.frozen.sh [--verify-only] <base-tree> <h2b-tree> \
#       <expected-base-sha> <expected-h2b-sha> <output-dir> [cpu]
set -euo pipefail

VERIFY_ONLY=0
if [ "${1:-}" = "--verify-only" ]; then
  VERIFY_ONLY=1
  shift
fi
BASE_WT="${1:?usage: run_e5_ab.frozen.sh [--verify-only] <base-tree> <h2b-tree> <expected-base-sha> <expected-h2b-sha> <output-dir> [cpu]}"
H2B_WT="${2:?missing h2b tree}"
WANT_BASE_SHA="${3:?missing expected base sha}"
WANT_H2B_SHA="${4:?missing expected h2b sha}"
OUT_DIR="${5:?missing output dir}"
CPU="${6:-2}"
REPS=15
HERE="$(cd "$(dirname "$0")" && pwd)"
FIXTURE_DIR="${SLUICE_H2B_FIXTURE_DIR:-/tmp/sluice-h2b-fixture}"
FIXTURE="$FIXTURE_DIR/fixture.bin"
CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"

die() { echo "refusing: $*" >&2; exit 2; }

tree_of() { if [ "$1" = base ]; then printf '%s' "$BASE_WT"; else printf '%s' "$H2B_WT"; fi; }
want_of() { if [ "$1" = base ]; then printf '%s' "$WANT_BASE_SHA"; else printf '%s' "$WANT_H2B_SHA"; fi; }

mkdir -p "$OUT_DIR" "$FIXTURE_DIR"

{
  echo "status=FORMAL_E5"
  echo "runner=run_e5_ab.frozen.sh r2"
  echo "runner_sha256=$(sha256sum "$HERE/run_e5_ab.frozen.sh" | awk '{print $1}')"
  echo "verdict_sha256=$(sha256sum "$HERE/e5_verdict.py" | awk '{print $1}')"
  echo "host=$(uname -n)"
  echo "kernel=$(uname -r)"
  echo "cpu_model=$CPU_MODEL"
  echo "pinned_cpu=$CPU"
  echo "reps=$REPS"
  echo "compiler=$(g++ --version | head -1)"
  echo "harness_flags=-std=c++20 -O3 -DNDEBUG"
  echo "fixture=$FIXTURE"
  echo "acceptance=median<=1.03 AND pairs-above<=10/15 per config (engineering ~6% bound, not exact 5%)"
  echo "perf_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
} > "$OUT_DIR/provenance.txt"

want_sha() { printf '%s' "$1" | grep -Eq '^[0-9a-f]{40}$'; }
want_sha "$WANT_BASE_SHA" || die "expected base sha is not a 40-hex sha"
want_sha "$WANT_H2B_SHA" || die "expected h2b sha is not a 40-hex sha"
[ "$WANT_BASE_SHA" != "$WANT_H2B_SHA" ] || die "both arms pinned to the same sha; not an A/B"
[ "$(realpath "$BASE_WT")" != "$(realpath "$H2B_WT")" ] || die "both arms point at one tree"

for arm in base h2b; do
  wt="$(tree_of "$arm")"
  want="$(want_of "$arm")"
  [ -d "$wt/.git" ] || [ -f "$wt/.git" ] || die "$arm tree is not a git worktree: $wt"
  got="$(git -C "$wt" rev-parse HEAD)" || die "cannot resolve HEAD of $arm tree $wt"
  [ "$got" = "$want" ] || die "$arm tree HEAD $got != pinned $want"
  dirty="$(git -C "$wt" status --porcelain)"
  [ -z "$dirty" ] || die "$arm tree is dirty:
$dirty"
done

DATA_HARNESS_REL=bench/native-linux-m2/pipeline_ab.cpp
[ -f "$BASE_WT/$DATA_HARNESS_REL" ] && [ -f "$H2B_WT/$DATA_HARNESS_REL" ] \
  || die "data harness missing from a tree"
b_harness=$(sha256sum "$BASE_WT/$DATA_HARNESS_REL" | awk '{print $1}')
h_harness=$(sha256sum "$H2B_WT/$DATA_HARNESS_REL" | awk '{print $1}')
[ "$b_harness" = "$h_harness" ] || die "data harness differs between trees; refusing to pick a side"
echo "data_harness_sha256=$b_harness" >> "$OUT_DIR/provenance.txt"

: > "$OUT_DIR/arm-identity.txt"
for arm in base h2b; do
  wt="$(tree_of "$arm")"
  lib="$wt/build/linux/x86_64/release/libsluice_async.a"
  echo "building arm $arm from an empty build/ and cleared config (stale artifacts excluded)" >&2
  rm -rf "$wt/build"
  (cd "$wt" && xmake f -c -y >/dev/null && xmake f -m release --liburing=y -y >/dev/null \
    && xmake build -y sluice_core >/dev/null && xmake build -y sluice_async >/dev/null)
  grep -q 'sanitizers = ""' "$wt/.xmake/linux/x86_64/xmake.conf" \
    || die "arm $arm config is not sanitizer-clean"
  [ -f "$lib" ] || die "arm $arm library was not produced"
  g++ -std=c++20 -O3 -DNDEBUG -I"$wt/include" -I"$wt/bench" \
    "$BASE_WT/$DATA_HARNESS_REL" \
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
    echo "expected_sha=$(want_of "$arm")"
    echo "binary_sha256=$(sha256sum "$OUT_DIR/pipeline_ab_$arm" | awk '{print $1}')"
    echo "zero_binary_sha256=$(sha256sum "$OUT_DIR/zero_burst_$arm" | awk '{print $1}')"
    echo "lib_async_sha256=$(sha256sum "$wt/build/linux/x86_64/release/libsluice_async.a" | awk '{print $1}')"
    echo "lib_core_sha256=$(sha256sum "$wt/build/linux/x86_64/release/libsluice_core.a" | awk '{print $1}')"
  } >> "$OUT_DIR/arm-identity.txt"
done

arm_field() {
  awk -v arm="$1" -v field="$2" \
    '$0 == "arm=" arm { seen = 1; next } seen && index($0, field "=") == 1 { sub(field "=", ""); print; exit }' \
    "$OUT_DIR/arm-identity.txt"
}
base_async=$(arm_field base lib_async_sha256)
h2b_async=$(arm_field h2b lib_async_sha256)
[ -n "$base_async" ] && [ "$base_async" != "$h2b_async" ] \
  || die "both arms linked an identical libsluice_async.a; not an A/B"

"$OUT_DIR/pipeline_ab_base" --fixture="$FIXTURE" --create-fixture

if [ "$VERIFY_ONLY" = 1 ]; then
  echo "verify-only: identity and clean-build gates passed; no measurement taken"
  exit 0
fi

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

# Hardware-counter attribution is decoupled from the acceptance verdict: the
# paired CPU-time experiment alone decides PASS/FAIL. A failed or absent
# perf stat is recorded as NOT_AVAILABLE and changes nothing.
perf_status="NOT_AVAILABLE"
if command -v perf >/dev/null 2>&1; then
  if perf stat -e task-clock,instructions,cycles -x, -o "$OUT_DIR/perf_probe.txt" \
       -- true >/dev/null 2>&1; then
    perf_status="AVAILABLE"
  else
    perf_status="NOT_AVAILABLE (probe failed)"
  fi
else
  perf_status="NOT_AVAILABLE (perf not installed)"
fi
echo "perf_stat_status=$perf_status" >> "$OUT_DIR/provenance.txt"
if [ "$perf_status" = "AVAILABLE" ]; then
  for pair in 1 2 3; do
    for arm in base h2b; do
      perf stat -e task-clock,instructions,cycles -x, \
        -o "$OUT_DIR/perf_${arm}_${pair}.txt" -- \
        taskset -c "$CPU" "$OUT_DIR/pipeline_ab_$arm" --arm="$arm" --backend=uring \
        --block=4096 --depth=1 --capacity=1024 --requests=20000 --warmup=1 \
        --fixture="$FIXTURE" > /dev/null
    done
  done
fi

python3 "$HERE/e5_verdict.py" "$JSONL" "$OUT_DIR/arm-identity.txt" \
  --expect-sha "base=$WANT_BASE_SHA" --expect-sha "h2b=$WANT_H2B_SHA" \
  --out "$OUT_DIR/verdict.json"
