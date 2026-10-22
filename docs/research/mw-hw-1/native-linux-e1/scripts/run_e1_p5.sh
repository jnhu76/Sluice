#!/usr/bin/env bash
# E1 P>0 pending-control pressure A/B campaign runner (research-only,
# fail-closed). Same gate discipline as the other campaign runners.
#
# Usage: run_e1_p5.sh <results-dir> [reps] [cpu]

set -euo pipefail

RESULTS_DIR="${1:?usage: run_e1_p5.sh <results-dir> [reps] [cpu]}"
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
[ -e "$RESULTS_DIR/p5.jsonl" ] && die "p5.jsonl already exists; a frozen campaign is never overwritten"

base_sha="$(git -C "$BASE_WT" rev-parse HEAD)"
e1_sha="$(git -C "$E1_WT" rev-parse HEAD)"
[ "$base_sha" = "$BASE_SHA_EXPECT" ] || die "BASE worktree SHA drifted"
[ "$e1_sha" = "$E1_SHA_EXPECT" ] || die "E1 worktree SHA drifted"
[ -z "$(git -C "$BASE_WT" status --porcelain)" ] || die "BASE worktree is dirty"
[ -z "$(git -C "$E1_WT" status --porcelain)" ] || die "E1 worktree is dirty"
[ -f "$FIXTURE" ] || die "fixture missing"

declare -A ARM_BIN=()
ARM_BIN[base]="$RESULTS_DIR/pending_control_pressure_base"
ARM_BIN[e1]="$RESULTS_DIR/pending_control_pressure_candidate"
for arm in base e1; do
    [ -f "${ARM_BIN[$arm]}" ] || die "missing P5 harness binary for arm $arm"
done

echo "status=RUNNING" > "$RESULTS_DIR/campaign-status.txt"
{
  echo "runner=run_e1_p5.sh"
  echo "runner_sha256=$(sha256sum "$SCRIPTS_DIR/run_e1_p5.sh" | awk '{print $1}')"
  echo "harness_source_sha256=$(sha256sum "$SCRIPTS_DIR/pending_control_pressure_ab.cpp" | awk '{print $1}')"
  echo "host=$(hostname)"
  echo "kernel=$(uname -r)"
  echo "pinned_cpu=$CPU"
  echo "reps=$REPS"
  echo "workload=file-cancel-congestion C32-Q8-W24"
  echo "base_sha=$base_sha"
  echo "e1_sha=$e1_sha"
  echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$RESULTS_DIR/provenance-p5.txt"

JSONL="$RESULTS_DIR/p5.jsonl"
: > "$JSONL"

validate_row() {
    python3 - "$1" <<'PYEOF'
import json, sys
row = json.loads(sys.argv[1])
assert row.get("ok") is True, "row not ok"
assert row.get("phase") == "timed", "unknown phase"
assert row.get("arm") in ("base", "e1"), "unknown arm"
assert isinstance(row.get("cpu_ns_per_op"), (int, float)), "non-numeric cpu"
assert row.get("invariant_ok") is True, "pending-control invariant not asserted"
PYEOF
}

for rep in $(seq 1 "$REPS"); do
    if [ $((rep % 2)) -eq 1 ]; then ORDER="AB"; ARMS=(base e1); else ORDER="BA"; ARMS=(e1 base); fi
    for arm in "${ARMS[@]}"; do
        set +e
        out="$(taskset -c "$CPU" "${ARM_BIN[$arm]}" --mode=perf --arm="$arm" \
            --requests=2000 --window=24 --warmup=1 --fixture="$FIXTURE" 2>&1)"
        rc=$?
        set -e
        [ $rc -eq 0 ] || die "arm $arm rep $rep rc=$rc: $out"
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            validate_row "$line" || die "invalid row: $line"
            printf '%s\n' "$line" | sed "s/^{/{\"rep\":$rep,\"order\":\"$ORDER\",/" >> "$JSONL"
        done <<< "$out"
    done
    echo "p5 rep $rep/$REPS complete" >&2
done

# Seam probe: the E1-only direct evidence of whether sticky pending-control
# states occur in this real-kernel workload. Recorded per campaign; its
# p_positive_samples decide the P_POSITIVE_COST status, not the perf rows.
PROBE_OUT="$RESULTS_DIR/p5-probe.jsonl"
: > "$PROBE_OUT"
set +e
probe_out="$(taskset -c "$CPU" "$RESULTS_DIR/pending_control_pressure_probe" --mode=probe \
    --arm=e1-probe --requests=1000 --window=24 --fixture="$FIXTURE" 2>&1)"
probe_rc=$?
set -e
printf '%s\n' "$probe_out" | sed "s/^{/{\"rep\":0,/" >> "$PROBE_OUT"
[ $probe_rc -eq 0 ] || echo "probe reports p_positive_samples==0 or failure (rc=$probe_rc): $probe_out" \
    >> "$RESULTS_DIR/p5-sticky-not-observed.txt"

python3 - "$JSONL" "$REPS" <<'PYEOF'
import json, sys, collections
path, reps = sys.argv[1], int(sys.argv[2])
agg = collections.Counter()
with open(path) as fh:
    for line in fh:
        r = json.loads(line)
        agg[(r["rep"], r["arm"])] += 1
for rep in range(1, reps + 1):
    for arm in ("base", "e1"):
        if agg[(rep, arm)] != 1:
            sys.exit(f"cell incomplete: rep={rep} arm={arm} count={agg[(rep, arm)]}")
print("p5 cell completeness OK")
PYEOF

echo "status=COMPLETE" > "$RESULTS_DIR/campaign-status.txt"
{
  echo "finished_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "p5_jsonl_sha256=$(sha256sum "$JSONL" | awk '{print $1}')"
  echo "p5_probe_sha256=$(sha256sum "$PROBE_OUT" | awk '{print $1}')"
} >> "$RESULTS_DIR/provenance-p5.txt"
echo "p5 campaign complete: $JSONL"
