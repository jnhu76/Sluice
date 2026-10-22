#!/usr/bin/env bash
# E1 native-Linux A/B campaign runner (research-only, fail-closed).
#
# Runs the paired BASE/E1 W-02 campaign on the frozen arm identities recorded
# in <results-dir>/arm-identity.txt. Every precondition is enforced: worktree
# SHAs, clean trees, non-overlapping paths, binary/library hashes, fixture
# identity. Any harness failure, missing row, unparseable row or failing
# oracle aborts the campaign with a FAILED status; a partial run can never
# look like a PASS.
#
# Usage: run_e1_e5.sh <results-dir> [reps] [cpu]

set -euo pipefail

RESULTS_DIR="${1:?usage: run_e1_e5.sh <results-dir> [reps] [cpu]}"
REPS="${2:-15}"
CPU="${3:-2}"
SCRIPTS_DIR="$(cd "$(dirname "$0")" && pwd)"
BASE_WT="${SLUICE_E1_BASE_WT:-$HOME/sluice-e1-e5/base}"
E1_WT="${SLUICE_E1_CANDIDATE_WT:-$HOME/sluice-e1-e5/candidate}"
FIXTURE="${SLUICE_E1_FIXTURE:-/tmp/sluice-e1-fixture/data.bin}"

BASE_SHA_EXPECT="381756370980f75e4beaf47310db4b8b9a99053d"
E1_SHA_EXPECT="2caead59f9317665510af72968439f1252bc5bb9"

die() { echo "RUNNER_FAIL: $*" >&2; echo "status=FAILED" >> "$RESULTS_DIR/campaign-status.txt"; exit 1; }

mkdir -p "$RESULTS_DIR"
if [ -e "$RESULTS_DIR/ab.jsonl" ]; then
    die "ab.jsonl already exists in $RESULTS_DIR; a frozen campaign is never overwritten"
fi

# Identity gates -----------------------------------------------------------------
base_sha="$(git -C "$BASE_WT" rev-parse HEAD)"
e1_sha="$(git -C "$E1_WT" rev-parse HEAD)"
[ "$base_sha" = "$BASE_SHA_EXPECT" ] || die "BASE worktree SHA $base_sha != frozen $BASE_SHA_EXPECT"
[ "$e1_sha" = "$E1_SHA_EXPECT" ] || die "E1 worktree SHA $e1_sha != frozen $E1_SHA_EXPECT"
[ -z "$(git -C "$BASE_WT" status --porcelain)" ] || die "BASE worktree is dirty"
[ -z "$(git -C "$E1_WT" status --porcelain)" ] || die "E1 worktree is dirty"

base_real="$(readlink -f "$BASE_WT")"
e1_real="$(readlink -f "$E1_WT")"
[ "$base_real" != "$e1_real" ] || die "arm worktree paths are identical"
case "$e1_real/" in "$base_real"/*) die "E1 worktree lives inside the BASE worktree";; esac
case "$base_real/" in "$e1_real"/*) die "BASE worktree lives inside the E1 worktree";; esac

# Stale-build gates ---------------------------------------------------------------
for arm in base candidate; do
    lib="$HOME/sluice-e1-e5/$arm/build/linux/x86_64/release/libsluice_async.a"
    core="$HOME/sluice-e1-e5/$arm/build/linux/x86_64/release/libsluice_core.a"
    bin="$RESULTS_DIR/pipeline_ab_e1_$arm"
    [ -f "$lib" ] || die "missing libsluice_async.a for arm $arm"
    [ -f "$core" ] || die "missing libsluice_core.a for arm $arm"
    [ -f "$bin" ] || die "missing harness binary for arm $arm"
    grep -qx "lib_async_sha256=$(sha256sum "$lib" | awk '{print $1}')" \
        "$RESULTS_DIR/arm-identity.txt" || die "lib_async hash drift for arm $arm vs arm-identity.txt"
    grep -qx "lib_core_sha256=$(sha256sum "$core" | awk '{print $1}')" \
        "$RESULTS_DIR/arm-identity.txt" || die "lib_core hash drift for arm $arm vs arm-identity.txt"
    grep -qx "binary_sha256=$(sha256sum "$bin" | awk '{print $1}')" \
        "$RESULTS_DIR/arm-identity.txt" || die "harness binary hash drift for arm $arm vs arm-identity.txt"
done

[ -f "$FIXTURE" ] || die "fixture missing: $FIXTURE"
grep -qx "fixture_sha256=$(sha256sum "$FIXTURE" | awk '{print $1}')" \
    "$RESULTS_DIR/arm-identity.txt" || die "fixture hash drift vs arm-identity.txt"

# Async-library arms must differ (same-library arms are a same-arm comparison).
async_base="$(sha256sum "$HOME/sluice-e1-e5/base/build/linux/x86_64/release/libsluice_async.a" | awk '{print $1}')"
async_e1="$(sha256sum "$HOME/sluice-e1-e5/candidate/build/linux/x86_64/release/libsluice_async.a" | awk '{print $1}')"
[ "$async_base" != "$async_e1" ] || die "both arms link the same libsluice_async.a; refusing same-arm comparison"

echo "status=RUNNING" > "$RESULTS_DIR/campaign-status.txt"

{
  echo "runner=run_e1_e5.sh"
  echo "runner_sha256=$(sha256sum "$SCRIPTS_DIR/run_e1_e5.sh" | awk '{print $1}')"
  echo "harness_source_sha256=$(sha256sum "$SCRIPTS_DIR/pipeline_ab_e1.cpp" | awk '{print $1}')"
  echo "bench_common_sha256=$(sha256sum "$SCRIPTS_DIR/bench_common.hpp" | awk '{print $1}')"
  echo "host=$(hostname)"
  echo "compiler=$(g++ --version | head -1)"
  echo "kernel=$(uname -r)"
  echo "pinned_cpu=$CPU"
  echo "reps=$REPS"
  echo "governor=$(cat /sys/devices/system/cpu/cpu$CPU/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
  echo "harness_flags=-std=c++20 -O3 -DNDEBUG"
  echo "fixture=$FIXTURE"
  echo "base_sha=$base_sha"
  echo "e1_sha=$e1_sha"
  echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$RESULTS_DIR/provenance.txt"

# Campaign matrix: block requests capacity depth ---------------------------------
CONFIGS=(
  "4096 20000 16 1"
  "4096 20000 64 1"
  "4096 20000 256 1"
  "4096 20000 1024 1"
  "4096 20000 16 8"
  "4096 20000 1024 8"
  "4096 20000 64 64"
  "4096 20000 1024 64"
  "65536 2000 1024 1"
  "65536 2000 1024 8"
  "1048576 500 64 8"
)

JSONL="$RESULTS_DIR/ab.jsonl"
FREQ="$RESULTS_DIR/frequency.jsonl"
: > "$JSONL"
: > "$FREQ"

read_freq() { cat "/sys/devices/system/cpu/cpu$CPU/cpufreq/scaling_cur_freq" 2>/dev/null || echo 0; }

validate_row() {
    # A row must parse, carry ok=true and name a known phase; anything else
    # aborts the campaign instead of producing a timing row.
    python3 - "$1" <<'PYEOF'
import json, sys
row = json.loads(sys.argv[1])
assert row.get("ok") is True, "row not ok"
assert row.get("phase") in ("timed", "oracle", "latency", "warmup"), "unknown phase"
assert row.get("arm") in ("base", "e1"), "unknown arm"
assert isinstance(row.get("cpu_ns_per_op"), (int, float)), "non-numeric cpu"
assert isinstance(row.get("wall_s"), (int, float)), "non-numeric wall"
assert row.get("failure") in ("", "(null)", None), "row carries a failure string"
assert row.get("completion_errors", 1) == 0, "completion errors"
assert row.get("invalid_state", 1) == 0, "invalid state rejections"
PYEOF
}

declare -A ARM_BIN=()
ARM_BIN[base]="$RESULTS_DIR/pipeline_ab_e1_base"
ARM_BIN[e1]="$RESULTS_DIR/pipeline_ab_e1_candidate"

for rep in $(seq 1 "$REPS"); do
    for ci in $(seq 0 $((${#CONFIGS[@]} - 1))); do
        idx=$(( (ci + rep - 1) % ${#CONFIGS[@]} ))
        read -r block requests capacity depth <<< "${CONFIGS[$idx]}"
        if [ $((rep % 2)) -eq 1 ]; then ORDER="AB"; ARMS=(base e1); else ORDER="BA"; ARMS=(e1 base); fi
        for arm in "${ARMS[@]}"; do
            f0=$(read_freq)
            set +e
            out="$(taskset -c "$CPU" "${ARM_BIN[$arm]}" \
                --arm="$arm" --backend=uring --block="$block" --depth="$depth" \
                --capacity="$capacity" --requests="$requests" --warmup=1 \
                --fixture="$FIXTURE" 2>&1)"
            rc=$?
            set -e
            f1=$(read_freq)
            [ $rc -eq 0 ] || die "arm $arm rep $rep config $block/$capacity/$depth exited rc=$rc: $out"
            while IFS= read -r line; do
                [ -n "$line" ] || continue
                validate_row "$line" || die "invalid row for arm $arm rep $rep config $block/$capacity/$depth: $line"
                printf '%s\n' "$line" | sed "s/^{/{\"rep\":$rep,\"order\":\"$ORDER\",\"cfg\":\"$block-$capacity-$depth\",/" >> "$JSONL"
            done <<< "$out"
            printf '{"rep":%d,"order":"%s","block":%s,"capacity":%s,"depth":%s,"arm":"%s","freq_before":%s,"freq_after":%s,"loadavg":"%s"}\n' \
                "$rep" "$ORDER" "$block" "$capacity" "$depth" "$arm" "$f0" "$f1" "$(cut -d' ' -f1-3 /proc/loadavg)" >> "$FREQ"
        done
    done
    echo "rep $rep/$REPS complete" >&2
done

# Cell completeness: every (rep, cfg, arm) cell must hold exactly one timed,
# oracle and latency row; a duplicate or a gap is a campaign bug, never a
# reducible artifact. Successful warmups emit no row by design.
python3 - "$JSONL" "$REPS" <<'PYEOF'
import json, sys, collections
path, reps = sys.argv[1], int(sys.argv[2])
agg = collections.Counter()
with open(path) as fh:
    for line in fh:
        r = json.loads(line)
        agg[(r["rep"], r["cfg"], r["arm"], r["phase"])] += 1
for rep in range(1, reps + 1):
    for cfg in {c for (_, c, _, _) in agg}:
        for arm in ("base", "e1"):
            for phase in ("timed", "oracle", "latency"):
                n = agg[(rep, cfg, arm, phase)]
                if n != 1:
                    sys.exit(f"cell incomplete: rep={rep} cfg={cfg} arm={arm} phase={phase} count={n}")
print("cell completeness OK")
PYEOF

echo "status=COMPLETE" > "$RESULTS_DIR/campaign-status.txt"
{
  echo "finished_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "ab_jsonl_sha256=$(sha256sum "$JSONL" | awk '{print $1}')"
  echo "frequency_sha256=$(sha256sum "$FREQ" | awk '{print $1}')"
} >> "$RESULTS_DIR/provenance.txt"
echo "campaign complete: $JSONL"
