#!/usr/bin/env bash
# Counts syscalls per thread role (main / worker / io-wq) while a case runs.
#
# bpftrace aggregates in the kernel, so unlike `perf trace` there is no ring
# buffer to overflow and no lost-event bias in the counts.
#
# Needs root for the raw_syscalls tracepoint.
#
# Every case writes <name>.verify.txt: the requested case, the case the harness
# actually ran, the repetition counts, the bench exit code, the probe attach
# status and the counted-event total. A case whose verification fails is
# reported as COUNT_SYSCALLS_FAILED and makes the script exit non-zero, so a
# silently empty or mis-filtered count cannot enter the evidence tree.
#
# Environment: same as capture_perf.sh.
set -uo pipefail

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
EVIDENCE_DIR="$(cd "$TOOLS_DIR/.." && pwd)"
DATA_DIR="$EVIDENCE_DIR/data"
SLUICE_ROOT="${SLUICE_ROOT:-$(cd "$EVIDENCE_DIR/../../.." && pwd)}"
BENCH_BUILD="${BENCH_BUILD:-$SLUICE_ROOT/build-bench-prof/linux/x86_64/releasedbg}"

export G0_TMP_DIR="${G0_TMP_DIR:-/tmp/sluice-g0-prof}"
export G0_DISK_DIR="${G0_DISK_DIR:-/var/tmp/sluice-g0-prof}"
mkdir -p "$DATA_DIR/syscalls" "$G0_TMP_DIR" "$G0_DISK_DIR"

BENCH_COMMS='comm == "bench_w02_pipel" || comm == "bench_w03_exter" || comm == "bench_w01_direc" || strncmp(comm, "iou-wrk", 7) == 0'

failures=0

count_case() {
    local name="$1" binary="$2" case_id="$3" reps="$4"
    local txt="$DATA_DIR/syscalls/$name.syscalls.txt"
    local err="$DATA_DIR/syscalls/$name.syscalls.err"
    local log="$DATA_DIR/syscalls/$name.run.log"
    local jsonl="$DATA_DIR/syscalls/$name.jsonl"
    local verify="$DATA_DIR/syscalls/$name.verify.txt"
    echo "=== count $name ($case_id, $reps measured reps)"

    timeout -s INT 120 bpftrace -e "
tracepoint:raw_syscalls:sys_enter /$BENCH_COMMS/ {
    @syscalls[tid == pid ? \"main\" : (strncmp(comm, \"iou-wrk\", 7) == 0 ? \"iowq\" : \"worker\"), args->id] = count();
}" >"$txt" 2>"$err" &
    local bt=$!
    sleep 1
    G0_CASE_FILTER="=$case_id" G0_WARMUP=1 G0_MEASURED="$reps" \
        G0_JSONL="$jsonl" "$BENCH_BUILD/$binary" >"$log" 2>&1
    local bench_rc=$?
    sleep 0.5
    kill -INT "$bt" 2>/dev/null
    wait "$bt" 2>/dev/null
    local bt_rc=$?

    local attached events verdict
    local -A seen=()
    while IFS='=' read -r key value; do seen["$key"]="$value"; done \
        < <(python3 "$TOOLS_DIR/verify_capture.py" "$jsonl" "$case_id" "$reps" 2>/dev/null)
    attached="$(grep -c '^Attached [1-9]' "$err")"
    events="$(awk -F': ' '/^@syscalls/{s+=$2} END{print s+0}' "$txt")"

    verdict="${seen[verdict]:-JSONL_UNREADABLE}"
    if [ "$verdict" = OK ] && [ "$bench_rc" -ne 0 ]; then verdict=BENCH_RC_NONZERO; fi
    if [ "$verdict" = OK ] && [ "$attached" -eq 0 ]; then verdict=PROBE_NOT_ATTACHED; fi
    if [ "$verdict" = OK ] && [ "$events" -eq 0 ]; then verdict=NO_EVENTS; fi
    if [ "$verdict" != OK ]; then failures=$((failures + 1)); fi

    {
        echo "case_requested = $case_id"
        echo "case_ran       = ${seen[case_ran]:-UNREADABLE}"
        echo "warmup_reps    = ${seen[warmup_reps]:-0}"
        echo "measured_reps  = ${seen[measured_reps]:-0}"
        echo "failing_reps   = ${seen[failing_reps]:-0}"
        echo "ops_recorded   = ${seen[ops_recorded]:-0}"
        echo "bench_rc       = $bench_rc"
        echo "bpftrace_rc    = $bt_rc"
        echo "probes_attached= $attached"
        echo "events_counted = $events"
        echo "verdict        = $verdict"
    } >"$verify"
    echo "    rc=$bench_rc case=${seen[case_ran]:-UNREADABLE} reps=${seen[warmup_reps]:-0}+${seen[measured_reps]:-0} probes=$attached events=$events $verdict"
    if [ "$verdict" != OK ]; then
        echo "    COUNT_SYSCALLS_FAILED $name $verdict" >&2
    fi
}

count_case w02-tp-4k-d16-w4   bench_w02_pipeline       W02-threadpool-4k-d16-w4 10
count_case w02-tp-4k-d16-w1   bench_w02_pipeline       W02-threadpool-4k-d16-w1 10
count_case w02-uring-4k-d16   bench_w02_pipeline       W02-uring-4k-d16         10
count_case w03-tp-4k-d16-w4   bench_w03_external_loop  W03-threadpool-4k-d16-w4 10
count_case w01-read-4k-seq    bench_w01_direct         W01-read_at-4k-seq       10

echo "=== count done, failures=$failures"
[ "$failures" -eq 0 ] || exit 1
