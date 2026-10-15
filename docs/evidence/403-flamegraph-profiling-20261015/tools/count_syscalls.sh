#!/usr/bin/env bash
# Counts syscalls per thread role (main / worker / io-wq) while a case runs.
#
# bpftrace aggregates in the kernel, so unlike `perf trace` there is no ring
# buffer to overflow and no lost-event bias in the counts.
#
# Needs root for the raw_syscalls tracepoint.
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

count_case() {
    local name="$1" binary="$2" case_id="$3" reps="$4"
    echo "=== count $name ($case_id, $reps measured reps)"
    timeout -s INT 120 bpftrace -e "
tracepoint:raw_syscalls:sys_enter /$BENCH_COMMS/ {
    @syscalls[tid == pid ? \"main\" : (strncmp(comm, \"iou-wrk\", 7) == 0 ? \"iowq\" : \"worker\"), args->id] = count();
}" >"$DATA_DIR/syscalls/$name.syscalls.txt" 2>"$DATA_DIR/syscalls/$name.syscalls.err" &
    local bt=$!
    sleep 1
    G0_CASE_FILTER="=$case_id" G0_WARMUP=1 G0_MEASURED="$reps" \
        G0_JSONL="$DATA_DIR/syscalls/$name.jsonl" "$BENCH_BUILD/$binary" \
        >"$DATA_DIR/syscalls/$name.run.log" 2>&1
    echo "    bench rc=$?"
    sleep 0.5
    kill -INT "$bt" 2>/dev/null
    wait "$bt" 2>/dev/null
}

count_case w02-tp-4k-d16-w4   bench_w02_pipeline       W02-threadpool-4k-d16-w4 10
count_case w02-tp-4k-d16-w1   bench_w02_pipeline       W02-threadpool-4k-d16-w1 10
count_case w02-uring-4k-d16   bench_w02_pipeline       W02-uring-4k-d16         10count_case w03-tp-4k-d16-w4   bench_w03_external_loop  W03-threadpool-4k-d16-w4 10
count_case w01-read-4k-seq    bench_w01_direct         W01-read_at-4k-seq       10

echo "=== count done"
