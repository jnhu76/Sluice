#!/usr/bin/env bash
# G1 A/B runner: interleaved repetition-level comparison of the frozen B0
# baseline binaries against a candidate build.
#
# Protocol (preregistered): per case and repetition r, run one measured
# bench repetition per side; even r runs B0 first, odd r runs the arm first,
# so thermally caused drift does not align with one side. warmup=1,
# measured=1 per invocation; the repetition count per case is the
# preregistered one (15 for the noisy cases, otherwise 7).
#
# Usage: ab_run.sh <outdir> <b0_dir> <arm_dir> <specfile>
#   specfile lines: <case> <profile> <target> <reps>
set -euo pipefail

root="$(cd "$(dirname "$0")/../../../.." && pwd)"
cd "$root"

out="${1:?usage: ab_run.sh <outdir> <b0_dir> <arm_dir> <specfile>}"
b0="${2:?b0 build dir}"
arm="${3:?arm build dir}"
specfile="${4:?specfile}"
mode="release"

mkdir -p "$out"
: >"$out/run.log"

binpath() { printf '%s/linux/x86_64/%s/%s' "$1" "$mode" "$2"; }

{
    echo "# g1 A/B run provenance"
    echo "started=$(date -Is)"
    echo "git_head=$(git rev-parse HEAD)"
    echo "git_status_dirty=$(git status --porcelain | wc -l)"
    echo "b0_dir=$b0 arm_dir=$arm"
    echo "b0_w02_ring=$(sha256sum "$(binpath "$b0" bench_w02_pipeline)" | cut -d' ' -f1)"
    echo "arm_w02_ring=$(sha256sum "$(binpath "$arm" bench_w02_pipeline)" | cut -d' ' -f1)"
    echo "b0_w03_ring=$(sha256sum "$(binpath "$b0" bench_w03_external_loop)" | cut -d' ' -f1)"
    echo "arm_w03_ring=$(sha256sum "$(binpath "$arm" bench_w03_external_loop)" | cut -d' ' -f1)"
    echo "b0_w01_noring=$(sha256sum "$(binpath "$b0" bench_w01_direct)" | cut -d' ' -f1)"
    echo "arm_w01_noring=$(sha256sum "$(binpath "$arm" bench_w01_direct)" | cut -d' ' -f1)"
    uptime
} | tee -a "$out/run.log"

failures=0
run_one() {
    local case_id="$1" side_dir="$2" side="$3" target="$4" rep="$5"
    local jsonl rc
    jsonl="$out/${case_id}.${side}.r${rep}.jsonl"
    rm -f "$jsonl"
    rc=0
    env G0_WARMUP=1 G0_MEASURED=1 "G0_CASE_FILTER==${case_id}" \
        "G0_JSONL=${jsonl}" "$(binpath "$side_dir" "$target")" \
        >>"$out/run.log" 2>&1 || rc=$?
    local measured
    measured="$(python3 -c "
import json
n=0
try:
    with open('${jsonl}') as fh:
        for line in fh:
            r=json.loads(line)
            if r.get('case_id')=='${case_id}' and r.get('measured') is True:
                n+=1
except FileNotFoundError:
    print(0); raise SystemExit
print(n)
" 2>/dev/null || echo 0)"
    printf '%s rep=%s side=%s rc=%s measured_reps=%s\n' \
        "$case_id" "$rep" "$side" "$rc" "$measured" | tee -a "$out/run.log"
    if [ "$rc" -ne 0 ] || [ "$measured" != "1" ]; then
        failures=$((failures + 1))
    fi
}

while read -r case_id profile target reps; do
    [ -z "$case_id" ] && continue
    case "$case_id" in \#*) continue;; esac
    # W-01 is profile-independent and built from the noring profile.
    if [ "$profile" = "noring" ]; then
        b0_side="$b0"; arm_side="$arm"
    else
        b0_side="$b0"; arm_side="$arm"
    fi
    for rep in $(seq 0 $((reps - 1))); do
        if [ $((rep % 2)) -eq 0 ]; then
            run_one "$case_id" "$b0_side" b0 "$target" "$rep"
            run_one "$case_id" "$arm_side" arm "$target" "$rep"
        else
            run_one "$case_id" "$arm_side" arm "$target" "$rep"
            run_one "$case_id" "$b0_side" b0 "$target" "$rep"
        fi
    done
done <"$specfile"

{
    echo "finished=$(date -Is)"
    echo "failures=$failures"
} | tee -a "$out/run.log"
[ "$failures" -eq 0 ] || exit 1
