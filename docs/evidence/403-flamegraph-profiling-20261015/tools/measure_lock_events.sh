#!/usr/bin/env bash
# Observes the synchronization and submission events themselves with strace.
#
# Two claims in the sampled evidence need a directly observed counterpart:
# which futex operations the mutex and condition-variable traffic actually
# issues, and how many submission queue entries one io_uring_enter carries. A
# syscall count alone answers neither -- `1 io_uring_enter per operation` says
# nothing about the number of SQEs inside that enter.
#
# The benchmark runs with G0_WARMUP=0 G0_MEASURED=1 so the trace stays small.
# strace serialises the traced threads, which perturbs timing and therefore
# contention; these are event identities and mixes, not throughput numbers. The
# unperturbed per-operation counts come from the bpftrace census in
# data/SYSCALLS.md.
#
# Environment: same as capture_perf.sh. No root needed.
set -uo pipefail

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
EVIDENCE_DIR="$(cd "$TOOLS_DIR/.." && pwd)"
DATA_DIR="$EVIDENCE_DIR/data"
OUT_DIR="$DATA_DIR/lock-events"
SLUICE_ROOT="${SLUICE_ROOT:-$(cd "$EVIDENCE_DIR/../../.." && pwd)}"
BENCH_BUILD="${BENCH_BUILD:-$SLUICE_ROOT/build-bench-prof/linux/x86_64/releasedbg}"

export G0_TMP_DIR="${G0_TMP_DIR:-/tmp/sluice-g0-prof}"
export G0_DISK_DIR="${G0_DISK_DIR:-/var/tmp/sluice-g0-prof}"
mkdir -p "$OUT_DIR" "$G0_TMP_DIR" "$G0_DISK_DIR"

failures=0

run_case() {
    local name="$1" binary="$2" case_id="$3"
    local trace="$OUT_DIR/$name.strace.txt"
    local log="$OUT_DIR/$name.strace.log"
    echo "=== lock events $name ($case_id)"

    G0_CASE_FILTER="=$case_id" G0_WARMUP=0 G0_MEASURED=1 \
        strace -f -e trace=futex,io_uring_enter -o "$trace" \
        "$BENCH_BUILD/$binary" >"$log" 2>&1
    local rc=$?
    local reps lines
    reps="$(grep -c 'rep=' "$log")"
    lines="$(wc -l <"$trace")"

    local verdict=OK
    if [ "$rc" -ne 0 ]; then verdict=BENCH_RC_NONZERO; fi
    if [ "$reps" -ne 1 ]; then verdict=REP_COUNT_MISMATCH; fi
    if [ "$lines" -eq 0 ]; then verdict=EMPTY_TRACE; fi
    if [ "$verdict" != OK ]; then failures=$((failures + 1)); fi

    {
        echo "case_requested = $case_id"
        echo "measured_reps  = $reps"
        echo "bench_rc       = $rc"
        echo "trace_lines    = $lines"
        echo "verdict        = $verdict"
    } >"$OUT_DIR/$name.verify.txt"
    echo "    rc=$rc reps=$reps trace_lines=$lines $verdict"
    if [ "$verdict" != OK ]; then
        echo "    MEASURE_LOCK_EVENTS_FAILED $name $verdict" >&2
    fi
}

run_case w02-tp-4k-d16-w4 bench_w02_pipeline W02-threadpool-4k-d16-w4
run_case w02-uring-4k-d16 bench_w02_pipeline W02-uring-4k-d16

python3 "$TOOLS_DIR/report_lock_events.py" || failures=$((failures + 1))

echo "=== lock events done, failures=$failures"
[ "$failures" -eq 0 ] || exit 1
