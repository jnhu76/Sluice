#!/usr/bin/env bash
# G1 targeted PMU counting campaign (unprivileged).
#
# perf_event_paranoid=2 on this host permits per-process counting of
# user-space PMU events without root; kernel-space sampling/counting and
# bpftrace remain PERMISSION_REQUIRED (see PMU.md).
#
# One perf stat invocation per (case, repetition). Each invocation drives
# exactly one measured bench repetition of one case (G0_WARMUP=0,
# G0_MEASURED=1, G0_CASE_FILTER) so every PMU row has a matching JSONL
# record with the operation count.
#
# Usage: measure_pmu.sh <outdir> [repetitions]
set -euo pipefail

root="$(cd "$(dirname "$0")/../../../.." && pwd)"
cd "$root"

out="${1:?usage: measure_pmu.sh <outdir> [repetitions]}"
reps="${2:-5}"
scratch="${G1_PMU_SCRATCH:-/var/tmp/sluice-g1-pmu}"
mode="release"

mkdir -p "$out" "$scratch"
: >"$out/run.log"

release_bin() { printf 'build-bench-%s/linux/x86_64/release/%s' "$1" "$2"; }

# context-switches/cpu-migrations are kernel-context events; perf_event_paranoid=2
# restricts counting to user space, so they read 0 and are not requested.
events="task-clock,page-faults,cycles:u,instructions:u,branches:u,branch-misses:u,cache-references:u,cache-misses:u,LLC-load-misses:u,dTLB-load-misses:u"

provenance() {
    echo "# g1 pmu run provenance"
    echo "started=$(date -Is)"
    echo "git_head=$(git rev-parse HEAD)"
    echo "git_status_dirty=$(git status --porcelain | wc -l)"
    echo "repetitions=$reps"
    echo "perf=$(perf --version 2>&1)"
    echo "paranoid=$(cat /proc/sys/kernel/perf_event_paranoid)"
    for profile in noring ring; do
        for target in bench_w01_direct bench_w02_pipeline bench_w03_external_loop; do
            printf 'binary %s %s sha256=%s\n' "$profile" "$target" \
                "$(sha256sum "$(release_bin "$profile" "$target")" | cut -d' ' -f1)"
        done
    done
    uptime
}
provenance | tee -a "$out/run.log"

failures=0
measure_case() {
    local case_id="$1" profile="$2" target="$3"
    local b csv rc jsonl
    b="$(release_bin "$profile" "$target")"
    for rep in $(seq 0 $((reps - 1))); do
        csv="$out/${case_id}.r${rep}.perfstat.csv"
        jsonl="$scratch/${case_id}.r${rep}.jsonl"
        rm -f "$jsonl"
        rc=0
        perf stat -x, -o "$csv" -e "$events" -- \
            env G0_WARMUP=0 G0_MEASURED=1 "G0_CASE_FILTER==${case_id}" \
            "G0_JSONL=${jsonl}" "$b" >>"$out/run.log" 2>&1 || rc=$?
        local rows bench_rc case_ran
        rows="$(wc -l <"$csv")"
        bench_rc="absent"
        case_ran="no"
        if [ -s "$jsonl" ]; then
            case_ran="$(python3 -c "
import json,sys
n=0
with open('${jsonl}') as fh:
    for line in fh:
        r=json.loads(line)
        if r.get('case_id')=='${case_id}' and r.get('measured') is True:
            n+=1
print(n)
")"
            bench_rc="rc_not_recorded_in_jsonl"
        fi
        cp "$jsonl" "$out/${case_id}.r${rep}.jsonl"
        printf '%s rep=%s perf_rc=%s csv_rows=%s bench_rc=%s measured_reps=%s\n' \
            "$case_id" "$rep" "$rc" "$rows" "$bench_rc" "$case_ran" \
            | tee -a "$out/run.log"
        if [ "$rc" -ne 0 ] || [ "$rows" -lt 12 ] || [ "$case_ran" != "1" ]; then
            failures=$((failures + 1))
        fi
    done
}

measure_case W01-read_at-4k-seq noring bench_w01_direct
measure_case W02-threadpool-4k-d16-w4 ring bench_w02_pipeline
measure_case W02-uring-4k-d16 ring bench_w02_pipeline
measure_case W03-threadpool-4k-d16-w4 ring bench_w03_external_loop
measure_case W03-uring-4k-d16 ring bench_w03_external_loop

{
    echo "finished=$(date -Is)"
    echo "failures=$failures"
} | tee -a "$out/run.log"
[ "$failures" -eq 0 ] || exit 1
