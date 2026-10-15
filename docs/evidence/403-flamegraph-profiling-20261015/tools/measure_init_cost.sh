#!/usr/bin/env bash
# Measures the fixed per-process syscall cost of each counted case.
#
# The harness creates its fixture before any repetition runs, so
# G0_WARMUP=0 G0_MEASURED=0 executes the startup and fixture path and no
# pipeline pass at all. What strace counts there is therefore cost that does not
# scale with the operation count; report_syscalls.py subtracts it so `per op`
# describes pipeline execution rather than amortised startup.
#
# strace counts every syscall (no sampling) and needs no root. It serialises the
# traced threads, which changes timing but not syscall counts.
#
# The counts include the `mkdir -p` helper processes the harness spawns through
# system(3); the bpftrace counting runs filter by comm and exclude those. The
# helper contribution is small enough to report as part of the measured init
# figure and is called out in data/INIT-COST.md.
#
# Environment: same as capture_perf.sh.
set -uo pipefail

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
EVIDENCE_DIR="$(cd "$TOOLS_DIR/.." && pwd)"
DATA_DIR="$EVIDENCE_DIR/data"
OUT_DIR="$DATA_DIR/syscalls/init"
SLUICE_ROOT="${SLUICE_ROOT:-$(cd "$EVIDENCE_DIR/../../.." && pwd)}"
BENCH_BUILD="${BENCH_BUILD:-$SLUICE_ROOT/build-bench-prof/linux/x86_64/releasedbg}"

export G0_TMP_DIR="${G0_TMP_DIR:-/tmp/sluice-g0-prof}"
export G0_DISK_DIR="${G0_DISK_DIR:-/var/tmp/sluice-g0-prof}"
mkdir -p "$OUT_DIR" "$G0_TMP_DIR" "$G0_DISK_DIR"

failures=0

init_case() {
    local name="$1" binary="$2" case_id="$3"
    local agg="$OUT_DIR/$name.init.strace.txt"
    local log="$OUT_DIR/$name.init.log"
    echo "=== init $name ($case_id)"

    G0_CASE_FILTER="=$case_id" G0_WARMUP=0 G0_MEASURED=0 \
        strace -f -c -o "$agg" "$BENCH_BUILD/$binary" >"$log" 2>&1
    local rc=$?

    local reps total verdict
    reps="$(grep -c 'rep=' "$log")"
    total="$(awk '/^100\.00/{print $4}' "$agg")"
    verdict=OK
    if [ "$rc" -ne 0 ]; then verdict=BENCH_RC_NONZERO; fi
    if [ "$reps" -ne 0 ]; then verdict=REPETITIONS_RAN; fi
    if [ -z "$total" ] || [ "$total" -eq 0 ]; then verdict=NO_SYSCALLS; fi
    if [ "$verdict" != OK ]; then failures=$((failures + 1)); fi

    {
        echo "case_requested  = $case_id"
        echo "measured_reps   = 0 (setup path only)"
        echo "bench_rc        = $rc"
        echo "repetition_lines= $reps"
        echo "syscalls_total  = ${total:-0}"
        echo "verdict         = $verdict"
    } >"$OUT_DIR/$name.init.verify.txt"
    echo "    rc=$rc reps=$reps syscalls=${total:-0} $verdict"
    if [ "$verdict" != OK ]; then
        echo "    MEASURE_INIT_FAILED $name $verdict" >&2
    fi
}

init_case w02-tp-4k-d16-w4   bench_w02_pipeline       W02-threadpool-4k-d16-w4
init_case w02-tp-4k-d16-w1   bench_w02_pipeline       W02-threadpool-4k-d16-w1
init_case w02-uring-4k-d16   bench_w02_pipeline       W02-uring-4k-d16
init_case w03-tp-4k-d16-w4   bench_w03_external_loop  W03-threadpool-4k-d16-w4
init_case w01-read-4k-seq    bench_w01_direct         W01-read_at-4k-seq

echo "=== init done, failures=$failures"
[ "$failures" -eq 0 ] || exit 1
