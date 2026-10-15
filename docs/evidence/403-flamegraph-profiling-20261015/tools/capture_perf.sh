#!/usr/bin/env bash
# Captures the profiling runs behind this evidence directory.
#
# Needs root: with perf_event_paranoid=2 an unprivileged perf records user-space
# samples only, and kernel stacks need /proc/kallsyms, which is zeroed for
# unprivileged readers. The workload therefore also runs as root, so every
# fixture/output path is pinned explicitly instead of coming from HOME.
#
# Environment:
#   SLUICE_ROOT   repository root (default: three levels above this directory)
#   BENCH_BUILD   build directory holding the bench binaries
#   PERF_FREQ     sampling frequency in Hz (default 4999)
set -uo pipefail

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
EVIDENCE_DIR="$(cd "$TOOLS_DIR/.." && pwd)"
DATA_DIR="$EVIDENCE_DIR/data"
SLUICE_ROOT="${SLUICE_ROOT:-$(cd "$EVIDENCE_DIR/../../.." && pwd)}"
BENCH_BUILD="${BENCH_BUILD:-$SLUICE_ROOT/build-bench-prof/linux/x86_64/releasedbg}"
PERF_FREQ="${PERF_FREQ:-4999}"

# The benchmark writes a 64 MiB tmpfs fixture and, for W-01, a disk fixture.
export G0_TMP_DIR="${G0_TMP_DIR:-/tmp/sluice-g0-prof}"
export G0_DISK_DIR="${G0_DISK_DIR:-/var/tmp/sluice-g0-prof}"
mkdir -p "$DATA_DIR/perf" "$G0_TMP_DIR" "$G0_DISK_DIR"

profile() {
    local name="$1" binary="$2" case_id="$3" reps="$4"
    echo "=== $name  case=$case_id  reps=$reps  $(date -Is)"
    G0_CASE_FILTER="=$case_id" G0_WARMUP=1 G0_MEASURED="$reps" \
        G0_JSONL="$DATA_DIR/perf/$name.jsonl" \
        perf record -F "$PERF_FREQ" -g --call-graph fp \
        -o "$DATA_DIR/perf/$name.data" -- "$BENCH_BUILD/$binary" \
        >"$DATA_DIR/perf/$name.run.log" 2>&1
    echo "    rc=$? size=$(du -h "$DATA_DIR/perf/$name.data" 2>/dev/null | cut -f1)"
}

profile w02-tp-4k-d16-w4   bench_w02_pipeline       W02-threadpool-4k-d16-w4  40
profile w02-uring-4k-d16   bench_w02_pipeline       W02-uring-4k-d16          40
profile w02-tp-4k-d16-w1   bench_w02_pipeline       W02-threadpool-4k-d16-w1  20
profile w02-tp-4k-d16-w8   bench_w02_pipeline       W02-threadpool-4k-d16-w8  20
profile w02-uring-4k-d64   bench_w02_pipeline       W02-uring-4k-d64          20
profile w02-tp-64k-d16-w4  bench_w02_pipeline       W02-threadpool-64k-d16-w4 20
profile w03-tp-4k-d16-w4   bench_w03_external_loop  W03-threadpool-4k-d16-w4  20
profile w03-uring-4k-d16   bench_w03_external_loop  W03-uring-4k-d16          20
profile w01-read-4k-seq    bench_w01_direct         W01-read_at-4k-seq        20
profile w01-write-4k-seq   bench_w01_direct         W01-write_at-4k-seq       20
profile w01-sync-data-4k   bench_w01_direct         W01-sync_data-4k          20

echo "=== done $(date -Is)"
