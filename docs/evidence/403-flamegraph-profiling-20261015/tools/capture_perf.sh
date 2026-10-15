#!/usr/bin/env bash
# Captures the profiling runs behind this evidence directory.
#
# Needs root: with perf_event_paranoid=2 an unprivileged perf records user-space
# samples only, and kernel stacks need /proc/kallsyms, which is zeroed for
# unprivileged readers. The workload therefore also runs as root, so every
# fixture/output path is pinned explicitly instead of coming from HOME.
#
# Every case writes <name>.verify.txt: the requested case, the case the harness
# actually ran, the repetition counts, the perf and bench exit codes, the
# recorded sample count and whether the perf.data header parses. A case whose
# verification fails is reported as CAPTURE_PERF_FAILED and makes the script
# exit non-zero, so a truncated or mis-filtered capture cannot enter the
# evidence tree unnoticed.
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

failures=0

profile() {
    local name="$1" binary="$2" case_id="$3" reps="$4"
    local data="$DATA_DIR/perf/$name.data"
    local log="$DATA_DIR/perf/$name.run.log"
    local jsonl="$DATA_DIR/perf/$name.jsonl"
    local verify="$DATA_DIR/perf/$name.verify.txt"
    echo "=== $name  case=$case_id  reps=$reps  $(date -Is)"

    G0_CASE_FILTER="=$case_id" G0_WARMUP=1 G0_MEASURED="$reps" \
        G0_JSONL="$jsonl" \
        perf record -F "$PERF_FREQ" -g --call-graph fp \
        -o "$data" -- "$BENCH_BUILD/$binary" \
        >"$log" 2>&1
    local perf_rc=$?
    local bench_complete
    bench_complete="$(grep -c 'failing repetitions=0' "$log")"

    local -A seen=()
    while IFS='=' read -r key value; do seen["$key"]="$value"; done \
        < <(python3 "$TOOLS_DIR/verify_capture.py" "$jsonl" "$case_id" "$reps" 2>/dev/null)
    local samples size header
    samples="$(perf report --stats -i "$data" 2>/dev/null \
        | awk '/SAMPLE events/{print $3; exit}')"
    size="$(stat -c %s "$data" 2>/dev/null || echo 0)"
    if perf report --header-only -i "$data" >/dev/null 2>&1; then header=ok; else header=UNREADABLE; fi

    local verdict="${seen[verdict]:-JSONL_UNREADABLE}"
    if [ "$verdict" = OK ] && [ "$perf_rc" -ne 0 ]; then verdict=PERF_RC_NONZERO; fi
    if [ "$verdict" = OK ] && [ "$bench_complete" -eq 0 ]; then verdict=BENCH_INCOMPLETE; fi
    if [ "$verdict" = OK ] && [ "$header" != ok ]; then verdict=PERF_DATA_UNREADABLE; fi
    if [ "$verdict" = OK ] && [ "${samples:-0}" -eq 0 ]; then verdict=NO_SAMPLES; fi
    if [ "$verdict" != OK ]; then failures=$((failures + 1)); fi

    {
        echo "case_requested   = $case_id"
        echo "case_ran         = ${seen[case_ran]:-UNREADABLE}"
        echo "warmup_reps      = ${seen[warmup_reps]:-0}"
        echo "measured_reps    = ${seen[measured_reps]:-0}"
        echo "failing_reps     = ${seen[failing_reps]:-0}"
        echo "ops_recorded     = ${seen[ops_recorded]:-0}"
        echo "perf_rc          = $perf_rc"
        echo "bench_complete   = $bench_complete"
        echo "perf_data_bytes  = $size"
        echo "perf_data_header = $header"
        echo "sample_events    = ${samples:-0}"
        echo "verdict          = $verdict"
    } >"$verify"
    echo "    rc=$perf_rc case=${seen[case_ran]:-UNREADABLE} samples=${samples:-0} size=$((size / 1024 / 1024))MiB $verdict"
    if [ "$verdict" != OK ]; then
        echo "    CAPTURE_PERF_FAILED $name $verdict" >&2
    fi
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

echo "=== done, failures=$failures $(date -Is)"
[ "$failures" -eq 0 ] || exit 1
