#!/usr/bin/env bash
# Executes the preregistered G0 (#403) measurement matrix and captures raw
# evidence. See docs/evidence/g0-403-performance-baseline-20261015/PREREGISTRATION.md
# for the frozen design this script implements.
#
# Usage: bench/run_matrix.sh <outdir> [warmup] [measured]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

out="${1:?usage: run_matrix.sh <outdir> [warmup] [measured] [stage] [suffix]}"
warmup="${2:-2}"
measured="${3:-7}"
stage="${4:-all}"
suffix="${5:-}"
mode="release"

mkdir -p "$out/raw" "$out/logs"

export G0_WARMUP="$warmup"
export G0_MEASURED="$measured"

bin() { printf 'build-bench-%s/linux/x86_64/%s/%s' "$1" "$mode" "$2"; }

provenance() {
    echo "# g0 matrix run provenance (stage=$stage suffix=$suffix)"
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

# A benchmark process exits non-zero when any case failed; for the no-ring
# profile that is the *expected* outcome for the io_uring cases, so the exit code
# is recorded rather than treated as a script failure.
run_case() {
    local label="$1" profile="$2" target="$3" output="$4"
    shift 4
    local b rc
    b="$(bin "$profile" "$target")"
    echo "== $label :: $profile/$target -> $output" | tee -a "$out/matrix-run.log"
    date -Is | tee -a "$out/matrix-run.log"
    rc=0
    G0_JSONL="$out/raw/$output.jsonl" "$b" >"$out/logs/$output.log" 2>&1 || rc=$?
    echo "exit=$rc records=$(wc -l <"$out/raw/$output.jsonl" 2>/dev/null || echo 0)" \
        | tee -a "$out/matrix-run.log"
}

if [ "$stage" = "all" ] || [ "$stage" = "measured" ]; then
# W-01 is profile-independent (core only); the no-ring binary is used. It carries
# no diagnostic cases, so the diagnostics stage does not re-run it.
run_case "w01 direct" noring bench_w01_direct "w01$suffix"
fi

if [ "$stage" = "diagnostics" ]; then
# Post-hoc attribution diagnostics (opt-in case set): the two capacity
# isolations and the completion-order discriminator, at the full repetition
# count. The ring binary carries both backends.
G0_DIAGNOSTICS=1 run_case "w02 diagnostics" ring bench_w02_pipeline \
    "w02-diagnostics$suffix"
elif [ "$stage" = "all" ] || [ "$stage" = "measured" ]; then
# W-02/W-03 ring binaries carry both backends in one process, so threadpool and
# io_uring cases alternate inside the same repetition rotation.
run_case "w02 pipeline" ring bench_w02_pipeline "w02$suffix"
run_case "w03 external loop" ring bench_w03_external_loop "w03$suffix"
fi

if [ "$stage" = "all" ] || [ "$stage" = "notrun" ]; then
# Explicit NOT_RUN evidence for the io_uring cases in the no-ring profile. A
# backend that is not in the build profile must be reported as unavailable, not
# as a pass and not as a silent fallback.
G0_WARMUP=0 G0_MEASURED=1 run_case "w02 no-ring profile" noring bench_w02_pipeline \
    w02-noring-notrun
G0_WARMUP=0 G0_MEASURED=1 run_case "w03 no-ring profile" noring bench_w03_external_loop \
    w03-noring-notrun
fi

{
    echo "# load_after"
    uptime
    free -b | sed 's/^/  /'
    echo "finished=$(date -Is)"
} | tee -a "$out/matrix-run.log"
