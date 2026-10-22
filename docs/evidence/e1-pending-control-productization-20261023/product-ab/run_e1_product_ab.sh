#!/usr/bin/env bash
# E1 product A/B campaign runner (gate R5) against pinned HEADs.
# BASE = master 859e4b56, E1 = product HEAD. 15 interleaved AB/BA rep pairs
# per config; per-row JSON validation; refuse-overwrite; hash-pinned libs,
# binaries and fixture. W-02 pipeline configs, W-03 real epoll+eventfd
# external loop with verify rows, perf/strace attribution on the headline.
set -euo pipefail

BASE_WT="${BASE_WT:-$HOME/sluice-e1-prodbase}"
E1_WT="${E1_WT:-$HOME/sluice-e1-product}"
SCRIPTS="$E1_WT/docs/research/mw-hw-1/native-linux-e1/scripts"
RESULTS_DIR="${RESULTS_DIR:-$(cd "$(dirname "$0")" && pwd)/results}"
CPU="${CPU:-2}"
REPS="${REPS:-15}"

die() { echo "error: $*" >&2; exit 1; }

[ -d "$SCRIPTS" ] || die "research scripts dir missing"
base_sha="$(git -C "$BASE_WT" rev-parse HEAD)"
e1_sha="$(git -C "$E1_WT" rev-parse HEAD)"
[ "$base_sha" != "$e1_sha" ] || die "arm HEADs identical"
[ -z "$(git -C "$BASE_WT" status --porcelain)" ] || die "BASE worktree dirty"
[ -z "$(git -C "$E1_WT" status --porcelain)" ] || die "E1 worktree dirty"

mkdir -p "$RESULTS_DIR"
for f in ab.jsonl w03.jsonl w03-verify.jsonl; do
    [ -f "$RESULTS_DIR/$f" ] && die "$f already exists: refusing to overwrite a finished campaign"
done
: > "$RESULTS_DIR/ab.jsonl"
: > "$RESULTS_DIR/w03.jsonl"
: > "$RESULTS_DIR/w03-verify.jsonl"

# Clean rebuilds of both arms -------------------------------------------------
for wt in "$BASE_WT" "$E1_WT"; do
    (cd "$wt" && rm -rf build && xmake f -m release --liburing=y -y >/dev/null 2>&1 \
        && xmake build sluice_core >/dev/null 2>&1 && xmake build sluice_async >/dev/null 2>&1) \
        || die "clean build failed in $wt"
done

build_harness() {
    local src=$1 arm_dir=$2 out=$3
    g++ -std=c++20 -O3 -DNDEBUG -I"$arm_dir/include" \
        "$src" "$SCRIPTS/bench_common.hpp" -o "$out" \
        "$arm_dir/build/linux/x86_64/release/libsluice_async.a" \
        "$arm_dir/build/linux/x86_64/release/libsluice_core.a" -luring
}
build_harness "$SCRIPTS/pipeline_ab_e1.cpp" "$BASE_WT" "$RESULTS_DIR/pipeline_base"
build_harness "$SCRIPTS/pipeline_ab_e1.cpp" "$E1_WT" "$RESULTS_DIR/pipeline_e1"
build_harness "$SCRIPTS/external_loop_ab_e1.cpp" "$BASE_WT" "$RESULTS_DIR/extloop_base"
build_harness "$SCRIPTS/external_loop_ab_e1.cpp" "$E1_WT" "$RESULTS_DIR/extloop_e1"

FIXTURE="/tmp/e1-product-ab-fixture"
rm -f "$FIXTURE"
"$RESULTS_DIR/pipeline_base" --fixture="$FIXTURE" --create-fixture >/dev/null

{
  echo "runner=run_e1_product_ab.sh"
  echo "runner_sha256=$(sha256sum "$0" | awk '{print $1}')"
  echo "pipeline_src_sha256=$(sha256sum "$SCRIPTS/pipeline_ab_e1.cpp" | awk '{print $1}')"
  echo "extloop_src_sha256=$(sha256sum "$SCRIPTS/external_loop_ab_e1.cpp" | awk '{print $1}')"
  echo "bench_common_sha256=$(sha256sum "$SCRIPTS/bench_common.hpp" | awk '{print $1}')"
  for arm in base e1; do
      wt="$BASE_WT"; [ "$arm" = e1 ] && wt="$E1_WT"
      echo "lib_async_${arm}_sha256=$(sha256sum "$wt/build/linux/x86_64/release/libsluice_async.a" | awk '{print $1}')"
      echo "lib_core_${arm}_sha256=$(sha256sum "$wt/build/linux/x86_64/release/libsluice_core.a" | awk '{print $1}')"
  done
  echo "pipeline_base_sha256=$(sha256sum "$RESULTS_DIR/pipeline_base" | awk '{print $1}')"
  echo "pipeline_e1_sha256=$(sha256sum "$RESULTS_DIR/pipeline_e1" | awk '{print $1}')"
  echo "extloop_base_sha256=$(sha256sum "$RESULTS_DIR/extloop_base" | awk '{print $1}')"
  echo "extloop_e1_sha256=$(sha256sum "$RESULTS_DIR/extloop_e1" | awk '{print $1}')"
  echo "fixture_sha256=$(sha256sum "$FIXTURE" | awk '{print $1}')"
  echo "host=$(hostname) kernel=$(uname -r)"
  echo "compiler=$(g++ --version | head -1)"
  echo "liburing=$(rpm -q liburing)"
  echo "pinned_cpu=$CPU reps=$REPS"
  echo "base_sha=$base_sha e1_sha=$e1_sha"
  echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$RESULTS_DIR/provenance.txt"

async_base="$(sha256sum "$BASE_WT/build/linux/x86_64/release/libsluice_async.a" | awk '{print $1}')"
async_e1="$(sha256sum "$E1_WT/build/linux/x86_64/release/libsluice_async.a" | awk '{print $1}')"
[ "$async_base" != "$async_e1" ] || die "both arms link the same libsluice_async.a"

validate_w02_row() {
    python3 - "$1" <<'PYEOF'
import json, sys
row = json.loads(sys.argv[1])
assert row.get("ok") is True, "row not ok"
assert row.get("phase") in ("timed", "oracle", "latency"), "unknown phase"
assert row.get("arm") in ("base", "e1"), "unknown arm"
assert isinstance(row.get("cpu_ns_per_op"), (int, float)) and row["cpu_ns_per_op"] > 0
assert row.get("completion_errors", 0) == 0 and row.get("invalid_state_rejections", 0) == 0
PYEOF
}
validate_w03_row() {
    python3 - "$1" <<'PYEOF'
import json, sys
row = json.loads(sys.argv[1])
assert row.get("loop_kind") == "epoll+eventfd", "not a real kernel event loop"
assert row.get("ok") is True, "row not ok"
assert row.get("phase") in ("timed", "oracle"), "unknown phase"
assert row.get("arm") in ("base", "e1"), "unknown arm"
assert isinstance(row.get("cpu_ns_per_op"), (int, float)), "non-numeric cpu"
PYEOF
}

W02_CONFIGS=(
  "4096 20000 1024 1"
  "4096 20000 1024 8"
  "4096 20000 16 1"
  "1048576 500 64 8"
)
for rep in $(seq 1 "$REPS"); do
    for ci in $(seq 0 $((${#W02_CONFIGS[@]} - 1))); do
        idx=$(( (ci + rep - 1) % ${#W02_CONFIGS[@]} ))
        read -r block requests capacity depth <<< "${W02_CONFIGS[$idx]}"
        if [ $((rep % 2)) -eq 1 ]; then ORDER="AB"; ARMS=(base e1); else ORDER="BA"; ARMS=(e1 base); fi
        for arm in "${ARMS[@]}"; do
            bin="$RESULTS_DIR/pipeline_base"; [ "$arm" = e1 ] && bin="$RESULTS_DIR/pipeline_e1"
            set +e
            out="$(taskset -c "$CPU" "$bin" --arm="$arm" --block="$block" --requests="$requests" \
                --capacity="$capacity" --depth="$depth" --warmup=1 --fixture="$FIXTURE" 2>&1)"
            rc=$?
            set -e
            [ $rc -eq 0 ] || die "W02 arm $arm rep $rep cfg $block/$capacity/$depth rc=$rc: $out"
            while IFS= read -r line; do
                [ -n "$line" ] || continue
                validate_w02_row "$line" || die "invalid W02 row: $line"
                printf '%s\n' "$line" \
                    | sed "s/^{/{\"rep\":$rep,\"order\":\"$ORDER\",\"cfg\":\"$block-$capacity-$depth\",/" \
                    >> "$RESULTS_DIR/ab.jsonl"
            done <<< "$out"
        done
    done
    echo "w02 rep $rep/$REPS complete" >&2
done

W03_CONFIGS=(
  "4096 20000 1024 1"
  "4096 20000 1024 8"
  "65536 2000 1024 8"
)
for rep in $(seq 1 "$REPS"); do
    for ci in $(seq 0 $((${#W03_CONFIGS[@]} - 1))); do
        idx=$(( (ci + rep - 1) % ${#W03_CONFIGS[@]} ))
        read -r block requests capacity depth <<< "${W03_CONFIGS[$idx]}"
        if [ $((rep % 2)) -eq 1 ]; then ORDER="AB"; ARMS=(base e1); else ORDER="BA"; ARMS=(e1 base); fi
        for arm in "${ARMS[@]}"; do
            bin="$RESULTS_DIR/extloop_base"; [ "$arm" = e1 ] && bin="$RESULTS_DIR/extloop_e1"
            set +e
            out="$(taskset -c "$CPU" "$bin" --mode=perf --arm="$arm" --block="$block" --depth="$depth" \
                --capacity="$capacity" --requests="$requests" --warmup=1 --fixture="$FIXTURE" 2>&1)"
            rc=$?
            set -e
            [ $rc -eq 0 ] || die "W03 arm $arm rep $rep cfg $block/$capacity/$depth rc=$rc: $out"
            while IFS= read -r line; do
                [ -n "$line" ] || continue
                validate_w03_row "$line" || die "invalid W03 row: $line"
                printf '%s\n' "$line" \
                    | sed "s/^{/{\"rep\":$rep,\"order\":\"$ORDER\",\"cfg\":\"$block-$capacity-$depth\",/" \
                    >> "$RESULTS_DIR/w03.jsonl"
            done <<< "$out"
        done
    done
    echo "w03 rep $rep/$REPS complete" >&2
done

for arm in base e1; do
    bin="$RESULTS_DIR/extloop_base"; [ "$arm" = e1 ] && bin="$RESULTS_DIR/extloop_e1"
    set +e
    out="$(taskset -c "$CPU" "$bin" --mode=verify --arm="$arm" --fixture="$FIXTURE" 2>&1)"
    rc=$?
    set -e
    [ $rc -eq 0 ] || die "W03 verify failed for arm $arm rc=$rc: $out"
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        printf '%s\n' "$line" | sed "s/^{/{\"arm\":\"$arm\",/" >> "$RESULTS_DIR/w03-verify.jsonl"
    done <<< "$out"
done

# Attribution: perf user-space counters and syscall census on the headline cfg
for arm in base e1; do
    bin="$RESULTS_DIR/pipeline_base"; [ "$arm" = e1 ] && bin="$RESULTS_DIR/pipeline_e1"
    taskset -c "$CPU" perf stat -e instructions:u,cycles:u -x, -- "$bin" --arm="$arm" \
        --block=4096 --requests=20000 --capacity=1024 --depth=1 --warmup=0 \
        --fixture="$FIXTURE" > "$RESULTS_DIR/perf-4096-1024-1-$arm.txt" 2>&1 || true
    taskset -c "$CPU" strace -cf -o "$RESULTS_DIR/strace-4096-1024-1-$arm.txt" -- "$bin" \
        --arm="$arm" --block=4096 --requests=20000 --capacity=1024 --depth=1 --warmup=0 \
        --fixture="$FIXTURE" >/dev/null 2>&1 || true
done

echo "finished_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$RESULTS_DIR/provenance.txt"
echo "ab_rows=$(wc -l < "$RESULTS_DIR/ab.jsonl")" >> "$RESULTS_DIR/provenance.txt"
echo "w03_rows=$(wc -l < "$RESULTS_DIR/w03.jsonl")" >> "$RESULTS_DIR/provenance.txt"
echo "campaign complete: $RESULTS_DIR"
