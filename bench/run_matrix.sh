#!/usr/bin/env bash
# Executes the preregistered G0 (#403) measurement matrix and captures raw
# evidence. See docs/evidence/g0-403-performance-baseline-20261015/PREREGISTRATION.md
# for the frozen design this script implements.
#
# Usage: bench/run_matrix.sh <outdir> [warmup] [measured] [stage] [suffix]
#
# Every run is validated by bench/check_run.py against the frozen expectation
# before the next one starts. A measured stage must exit 0 with the frozen case
# set and the requested repetition counts; a control run must exit 1 with exactly
# the expected NOT_RUN set and no other failure. Any deviation aborts this script
# with a non-zero status, so a partly failed matrix cannot be read as a success.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

out="${1:?usage: run_matrix.sh <outdir> [warmup] [measured] [stage] [suffix]}"
warmup="${2:-2}"
measured="${3:-7}"
stage="${4:-all}"
suffix="${5:-}"
mode="release"

reference_dir="${SLUICE_BENCH_REFERENCE_DIR:-docs/evidence/g0-403-performance-baseline-20261015/raw}"
checker="$root/bench/check_run.py"

if [ -z "$suffix" ] && [[ "$(realpath -m "$out")" == "$root"/docs/evidence/* ]]; then
    echo "refusing to write canonical evidence file names inside the evidence directory;" >&2
    echo "pass a suffix or an outdir outside docs/evidence/" >&2
    exit 1
fi

mkdir -p "$out/raw" "$out/logs"

export SLUICE_BENCH_WARMUP="$warmup"
export SLUICE_BENCH_MEASURED="$measured"

# SLUICE_BENCH_BIN_DIR is a test seam for bench/runner_failure_control.sh, which points it
# at deliberately failing shims; a real matrix run must leave it unset.
bin() { printf '%s/%s' "${SLUICE_BENCH_BIN_DIR:-build-bench-$1/linux/x86_64/$mode}" "$2"; }

provenance() {
    echo "# bench matrix run provenance (stage=$stage suffix=$suffix)"
    echo "started=$(date -Is)"
    echo "git_head=$(git rev-parse HEAD)"
    echo "git_status_dirty=$(git status --porcelain | wc -l)"
    echo "warmup=$warmup measured=$measured stage=$stage suffix=$suffix"
    echo "host_manifest=host-manifest.txt"
    for profile in noring ring; do
        for target in bench_w01_direct bench_w02_pipeline bench_w03_external_loop; do
            local b
            b="$(bin "$profile" "$target")"
            printf 'binary %s %s sha256=%s\n' "$profile" "$target" "$(sha256sum "$b" | cut -d' ' -f1)"
        done
    done
    echo "# load_before"
    uptime
    free -b | sed 's/^/  /'
}

provenance | tee -a "$out/matrix-run.log"

run_case() {
    local label="$1" profile="$2" target="$3" output="$4"
    local check_stage="$5" reference="$6" run_warmup="$7" run_measured="$8" expect_cases="$9"
    local b rc verdict
    b="$(bin "$profile" "$target")"
    echo "== $label :: $profile/$target -> $output (check=$check_stage)" \
        | tee -a "$out/matrix-run.log"
    date -Is | tee -a "$out/matrix-run.log"
    rc=0
    SLUICE_BENCH_WARMUP="$run_warmup" SLUICE_BENCH_MEASURED="$run_measured" \
        SLUICE_BENCH_JSONL="$out/raw/$output.jsonl" "$b" \
        >"$out/logs/$output.log" 2>&1 || rc=$?
    echo "exit=$rc records=$(wc -l <"$out/raw/$output.jsonl" 2>/dev/null || echo 0)" \
        | tee -a "$out/matrix-run.log"

    if ! verdict="$(python3 "$checker" "$out/raw/$output.jsonl" --stage "$check_stage" \
            --exit-code "$rc" --warmup "$run_warmup" --measured "$run_measured" \
            --expect-cases "$expect_cases" --reference "$reference" 2>&1)"; then
        printf '%s\n' "$verdict" | tee -a "$out/matrix-run.log" >&2
        echo "# FAIL: $label did not match the frozen expectation (raw=$out/raw/$output.jsonl log=$out/logs/$output.log)" \
            | tee -a "$out/matrix-run.log" >&2
        exit 1
    fi
    printf '%s\n' "$verdict" | tee -a "$out/matrix-run.log"
}

# Preregistered case counts: W-01 15, W-02 16 (14 primary + 2 -scope64), W-03 6,
# the diagnostics set 19, and the no-ring controls 16 and 6. They are restated
# here so a truncated reference file cannot silently become the expectation.
if [ "$stage" = "all" ] || [ "$stage" = "measured" ]; then
# W-01 is profile-independent (core only); the no-ring binary is used. It carries
# no diagnostic cases, so the diagnostics stage does not re-run it.
run_case "w01 direct" noring bench_w01_direct "w01$suffix" measured \
    "$reference_dir/w01.jsonl" "$warmup" "$measured" 15
fi

if [ "$stage" = "diagnostics" ]; then
# Post-hoc attribution diagnostics (opt-in case set): the two capacity
# isolations and the completion-order discriminator, at the full repetition
# count. The ring binary carries both backends.
SLUICE_BENCH_DIAGNOSTICS=1 run_case "w02 diagnostics" ring bench_w02_pipeline \
    "w02-diagnostics$suffix" diagnostics "$reference_dir/w02-diagnostics.jsonl" \
    "$warmup" "$measured" 19
elif [ "$stage" = "all" ] || [ "$stage" = "measured" ]; then
# W-02/W-03 ring binaries carry both backends in one process, so threadpool and
# io_uring cases alternate inside the same repetition rotation.
run_case "w02 pipeline" ring bench_w02_pipeline "w02$suffix" measured \
    "$reference_dir/w02.jsonl" "$warmup" "$measured" 16
run_case "w03 external loop" ring bench_w03_external_loop "w03$suffix" measured \
    "$reference_dir/w03.jsonl" "$warmup" "$measured" 6
fi

if [ "$stage" = "all" ] || [ "$stage" = "notrun" ]; then
# Explicit NOT_RUN evidence for the io_uring cases in the no-ring profile. A
# backend that is not in the build profile must be reported as unavailable, not
# as a pass and not as a silent fallback; the checker requires exactly the frozen
# NOT_RUN set and the exit status that means "these cases failed".
run_case "w02 no-ring profile" noring bench_w02_pipeline w02-noring-notrun notrun \
    "$reference_dir/w02-noring-notrun.jsonl" 0 1 16
run_case "w03 no-ring profile" noring bench_w03_external_loop w03-noring-notrun notrun \
    "$reference_dir/w03-noring-notrun.jsonl" 0 1 6
fi

{
    echo "# load_after"
    uptime
    free -b | sed 's/^/  /'
    echo "finished=$(date -Is)"
} | tee -a "$out/matrix-run.log"
