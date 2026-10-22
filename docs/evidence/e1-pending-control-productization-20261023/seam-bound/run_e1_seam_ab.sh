#!/usr/bin/env bash
# P>0 seam-bound A/B campaign runner (gate R4, Path B).
# Compiles the async TUs from each arm's worktree with the internal test
# seam, builds the identical harness twice (E1 build adds only the snapshot
# reads in its validation block), and interleaves 15 AB/BA rep pairs on a
# pinned CPU. Refuses to overwrite a finished campaign.
set -euo pipefail

BASE_WT="${BASE_WT:-$HOME/sluice-e1-prodbase}"
E1_WT="${E1_WT:-$HOME/sluice-e1-product}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="${RESULTS_DIR:-$SCRIPT_DIR/results}"
CPU="${CPU:-2}"
REPS="${REPS:-15}"
CYCLES="${CYCLES:-200}"
REPEATS="${REPEATS:-2}"

die() { echo "error: $*" >&2; exit 1; }

base_sha="$(git -C "$BASE_WT" rev-parse HEAD)"
e1_sha="$(git -C "$E1_WT" rev-parse HEAD)"
[ "$(git -C "$BASE_WT" rev-parse HEAD)" != "$(git -C "$E1_WT" rev-parse HEAD)" ] || die "arm HEADs identical"
git -C "$BASE_WT" status --porcelain | grep -q . && die "BASE worktree dirty"
git -C "$E1_WT" status --porcelain | grep -q . && die "E1 worktree dirty"

mkdir -p "$RESULTS_DIR"
[ -f "$RESULTS_DIR/seam.jsonl" ] && die "seam.jsonl already exists: refusing to overwrite a finished campaign"
: > "$RESULTS_DIR/seam.jsonl"

build_arm() {
    local arm_dir=$1 define=$2 out=$3
    g++ -std=c++20 -O3 -DNDEBUG -I"$arm_dir/include" -I"$arm_dir/src/async" \
        -DSLUICE_ASYNC_INTERNAL_TESTING -DSLUICE_HAS_LIBURING $define \
        "$SCRIPT_DIR/pending_control_seam_ab.cpp" \
        "$arm_dir/src/async/async_io_context.cpp" \
        "$arm_dir/src/async/uring_transport.cpp" \
        "$arm_dir/src/async/uring_completion.cpp" \
        "$arm_dir/src/async/uring_backend.cpp" \
        "$arm_dir/src/async/fail_fast.cpp" \
        "$arm_dir/src/async/detail/context_identity.cpp" \
        "$arm_dir/src/async/detail/request_core.cpp" \
        -o "$out" -luring
}

build_arm "$BASE_WT" "" "$RESULTS_DIR/seam_base"
build_arm "$E1_WT" "-DSLUICE_E1_SEAM_ARM" "$RESULTS_DIR/seam_e1"

{
  echo "runner=run_e1_seam_ab.sh"
  echo "runner_sha256=$(sha256sum "$0" | awk '{print $1}')"
  echo "harness_sha256=$(sha256sum "$SCRIPT_DIR/pending_control_seam_ab.cpp" | awk '{print $1}')"
  echo "seam_base_sha256=$(sha256sum "$RESULTS_DIR/seam_base" | awk '{print $1}')"
  echo "seam_e1_sha256=$(sha256sum "$RESULTS_DIR/seam_e1" | awk '{print $1}')"
  echo "host=$(hostname)"
  echo "kernel=$(uname -r)"
  echo "compiler=$(g++ --version | head -1)"
  echo "pinned_cpu=$CPU"
  echo "reps=$REPS cycles=$CYCLES repeats_per_invocation=$REPEATS"
  echo "base_sha=$base_sha"
  echo "e1_sha=$e1_sha"
  echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$RESULTS_DIR/provenance.txt"

phase_params() {
    case "$1" in
        low_p)           echo "pressured=1" ;;
        high_p)          echo "pressured=8" ;;
        repeated_cancel) echo "pressured=8 cancels=3" ;;
        high_rotation)   echo "pressured=8 cycles=400" ;;
        *) die "unknown phase $1" ;;
    esac
}

run_arm() {
    local bin=$1 arm=$2 phase=$3 rep=$4 extra=$5
    taskset -c "$CPU" "$bin" --arm="$arm" --phase="$phase" --rep="$rep" \
        --cycles="$CYCLES" --repeats="$REPEATS" --capacity=64 --depth=8 --passes=24 $extra
}

for rep in $(seq 1 "$REPS"); do
    if [ $((rep % 2)) -eq 1 ]; then ORDER="base e1"; else ORDER="e1 base"; fi
    for phase in low_p high_p repeated_cancel high_rotation; do
        extra="$(phase_params "$phase")"
        for arm in $ORDER; do
            if [ "$arm" = base ]; then
                run_arm "$RESULTS_DIR/seam_base" base "$phase" "$rep" "$extra" \
                    >> "$RESULTS_DIR/seam.jsonl"
            else
                run_arm "$RESULTS_DIR/seam_e1" e1 "$phase" "$rep" "$extra" \
                    >> "$RESULTS_DIR/seam.jsonl"
            fi
        done
    done
done

echo "finished_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$RESULTS_DIR/provenance.txt"
echo "rows=$(wc -l < "$RESULTS_DIR/seam.jsonl")" >> "$RESULTS_DIR/provenance.txt"
echo "campaign complete: $RESULTS_DIR/seam.jsonl"
