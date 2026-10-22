#!/usr/bin/env bash
# E1 W-03 external-loop A/B campaign runner (research-only, fail-closed).
#
# Same gate discipline as run_e1_e5.sh: frozen arm identities, clean trees,
# per-arm binaries, interleaved AB/BA pairing, per-row validation, no
# overwriting of a finished campaign.
#
# Usage: run_e1_w03.sh <results-dir> [reps] [cpu]

set -euo pipefail

RESULTS_DIR="${1:?usage: run_e1_w03.sh <results-dir> [reps] [cpu]}"
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
[ -e "$RESULTS_DIR/w03.jsonl" ] && die "w03.jsonl already exists; a frozen campaign is never overwritten"

base_sha="$(git -C "$BASE_WT" rev-parse HEAD)"
e1_sha="$(git -C "$E1_WT" rev-parse HEAD)"
[ "$base_sha" = "$BASE_SHA_EXPECT" ] || die "BASE worktree SHA drifted"
[ "$e1_sha" = "$E1_SHA_EXPECT" ] || die "E1 worktree SHA drifted"
[ -z "$(git -C "$BASE_WT" status --porcelain)" ] || die "BASE worktree is dirty"
[ -z "$(git -C "$E1_WT" status --porcelain)" ] || die "E1 worktree is dirty"
[ -f "$FIXTURE" ] || die "fixture missing"

declare -A ARM_BIN=()
ARM_BIN[base]="$RESULTS_DIR/external_loop_ab_e1_base"
ARM_BIN[e1]="$RESULTS_DIR/external_loop_ab_e1_candidate"
for arm in base e1; do
    [ -f "${ARM_BIN[$arm]}" ] || die "missing W-03 harness binary for arm $arm"
done

echo "status=RUNNING" > "$RESULTS_DIR/campaign-status.txt"
{
  echo "runner=run_e1_w03.sh"
  echo "runner_sha256=$(sha256sum "$SCRIPTS_DIR/run_e1_w03.sh" | awk '{print $1}')"
  echo "harness_source_sha256=$(sha256sum "$SCRIPTS_DIR/external_loop_ab_e1.cpp" | awk '{print $1}')"
  echo "host=$(hostname)"
  echo "kernel=$(uname -r)"
  echo "pinned_cpu=$CPU"
  echo "reps=$REPS"
  echo "loop_kind=epoll+eventfd"
  echo "base_sha=$base_sha"
  echo "e1_sha=$e1_sha"
  echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$RESULTS_DIR/provenance-w03.txt"

CONFIGS=(
  "4096 20000 1024 1"
  "4096 20000 1024 8"
  "65536 2000 1024 8"
)

JSONL="$RESULTS_DIR/w03.jsonl"
: > "$JSONL"

validate_row() {
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

for rep in $(seq 1 "$REPS"); do
    for ci in $(seq 0 $((${#CONFIGS[@]} - 1))); do
        idx=$(( (ci + rep - 1) % ${#CONFIGS[@]} ))
        read -r block requests capacity depth <<< "${CONFIGS[$idx]}"
        if [ $((rep % 2)) -eq 1 ]; then ORDER="AB"; ARMS=(base e1); else ORDER="BA"; ARMS=(e1 base); fi
        for arm in "${ARMS[@]}"; do
            set +e
            out="$(taskset -c "$CPU" "${ARM_BIN[$arm]}" \
                --mode=perf --arm="$arm" --block="$block" --depth="$depth" \
                --capacity="$capacity" --requests="$requests" --warmup=1 \
                --fixture="$FIXTURE" 2>&1)"
            rc=$?
            set -e
            [ $rc -eq 0 ] || die "arm $arm rep $rep config $block/$capacity/$depth rc=$rc: $out"
            while IFS= read -r line; do
                [ -n "$line" ] || continue
                validate_row "$line" || die "invalid row: $line"
                printf '%s\n' "$line" | sed "s/^{/{\"rep\":$rep,\"order\":\"$ORDER\",\"cfg\":\"$block-$capacity-$depth\",/" >> "$JSONL"
            done <<< "$out"
        done
    done
    echo "w03 rep $rep/$REPS complete" >&2
done

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
            for phase in ("timed", "oracle"):
                if agg[(rep, cfg, arm, phase)] != 1:
                    sys.exit(f"cell incomplete: rep={rep} cfg={cfg} arm={arm} phase={phase}")
print("w03 cell completeness OK")
PYEOF

# Verify phase: the interleave obligations must pass on both arms.
VERIFY_OUT="$RESULTS_DIR/w03-verify.jsonl"
: > "$VERIFY_OUT"
for arm in base e1; do
    set +e
    out="$(taskset -c "$CPU" "${ARM_BIN[$arm]}" --mode=verify --arm="$arm" --fixture="$FIXTURE" 2>&1)"
    rc=$?
    set -e
    [ $rc -eq 0 ] || die "verify phase failed for arm $arm rc=$rc: $out"
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        printf '%s\n' "$line" >> "$VERIFY_OUT"
    done <<< "$out"
done

echo "status=COMPLETE" > "$RESULTS_DIR/campaign-status.txt"
{
  echo "finished_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "w03_jsonl_sha256=$(sha256sum "$JSONL" | awk '{print $1}')"
  echo "w03_verify_sha256=$(sha256sum "$VERIFY_OUT" | awk '{print $1}')"
} >> "$RESULTS_DIR/provenance-w03.txt"
echo "w03 campaign complete: $JSONL"
