#!/usr/bin/env bash
# Captures the G0 (#403) attribution evidence: syscall counts, hardware counters,
# DSO/symbol attribution, the CPU-placement discriminator and the lock probe.
#
# Every command and its raw output is written to <outdir>; the profiling build is
# symbol-preserving (-O3 -g -DNDEBUG, unstripped) because the measured binaries
# are stripped, so its absolute throughput is NOT comparable to the baseline.
#
# Usage: bench/attribution.sh <outdir> [warmup] [measured]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

out="${1:?usage: attribution.sh <outdir> [warmup] [measured]}"
warmup="${2:-0}"
measured="${3:-1}"
prof_dir="${out}/profiling-build"
mkdir -p "$out" "$prof_dir"

mode="release"
libs="-Lbuild-bench-ring/linux/x86_64/$mode"
cxx_flags=(-m64 -fvisibility=hidden -fvisibility-inlines-hidden -Wall -O3 -g -std=c++20
           -Ibench -Iinclude -DSLUICE_LIBURING_PROFILE=on -DSLUICE_HAS_LIBURING -DNDEBUG)

echo "# attribution capture"
date -Is
echo "git_head=$(git rev-parse HEAD)"
echo "git_dirty=$(git status --porcelain | wc -l)"
echo "warmup=$warmup measured=$measured"

# --- profiling build ---------------------------------------------------------
for src in bench_w01_direct bench_w02_pipeline bench_w03_external_loop; do
    gcc -c "${cxx_flags[@]}" -o "$prof_dir/$src.o" "bench/$src.cpp"
    g++ -g -o "$prof_dir/$src" "$prof_dir/$src.o" -m64 $libs \
        -lsluice_async -luring -lsluice_core
    printf 'profiling_binary %s sha256=%s\n' "$src" \
        "$(sha256sum "$prof_dir/$src" | cut -d' ' -f1)"
done

export G0_WARMUP="$warmup"
export G0_MEASURED="$measured"
export G0_JSONL=

tp_w1="=W02-threadpool-4k-d16-w1"
tp_w4="=W02-threadpool-4k-d16-w4"
tp_w8="=W02-threadpool-4k-d16-w8"
ur_d16="=W02-uring-4k-d16"
direct="=W01-read_at-4k-seq"

run() {
    local label="$1" filter="$2" binary="$3"
    shift 3
    echo
    echo "## $label"
    echo "command: G0_CASE_FILTER='$filter' $* $binary"
    env G0_CASE_FILTER="$filter" "$@" "$binary" 2>&1
}

# --- syscall counts ---------------------------------------------------------
run "strace: direct 4 KiB read" "$direct" "$prof_dir/bench_w01_direct" \
    strace -c -o "$out/strace-direct.txt" >/dev/null 2>&1 || true
cp /dev/null /dev/null 2>/dev/null || true

strace_one() {
    local label="$1" filter="$2" binary="$3" file="$4"
    echo
    echo "## $label"
    echo "command: G0_CASE_FILTER='$filter' strace -f -c -o $file $binary"
    env G0_CASE_FILTER="$filter" strace -f -c -o "$out/$file" "$binary" >/dev/null 2>&1 || true
    sed 's/^/  /' "$out/$file"
}
strace_one "strace -c: direct read_at 4 KiB" "$direct" "$prof_dir/bench_w01_direct" strace-direct.txt
strace_one "strace -c: ThreadPool d16 w1" "$tp_w1" "$prof_dir/bench_w02_pipeline" strace-tp-w1.txt
strace_one "strace -c: ThreadPool d16 w4" "$tp_w4" "$prof_dir/bench_w02_pipeline" strace-tp-w4.txt
strace_one "strace -c: io_uring d16" "$ur_d16" "$prof_dir/bench_w02_pipeline" strace-uring.txt

echo
echo "## strace -y: which fd carries the per-operation notification (ThreadPool d16 w1)"
echo "command: G0_CASE_FILTER='$tp_w1' strace -f -y -e trace=write,read,poll,eventfd2 -o $out/strace-fd.txt $prof_dir/bench_w02_pipeline"
env G0_CASE_FILTER="$tp_w1" strace -f -y -e trace=write,read,poll,eventfd2 \
    -o "$out/strace-fd.txt" "$prof_dir/bench_w02_pipeline" >/dev/null 2>&1 || true
grep -oE "(write|read)\([0-9]+<[^>]*>" "$out/strace-fd.txt" | sort | uniq -c | sort -rn | head -5 | sed 's/^/  /' || true
echo "  sample write payload:"
grep -m2 "write(4<anon_inode:\[eventfd\]>" "$out/strace-fd.txt" | sed 's/^/    /'
echo "  writing threads (count of eventfd writes per thread):"
grep -oE "^[0-9]+ +write\(4" "$out/strace-fd.txt" | awk '{print $1}' | sort | uniq -c | sed 's/^/    /'

# --- hardware counters ------------------------------------------------------
perf_one() {
    local label="$1" filter="$2"
    echo
    echo "## perf stat: $label"
    echo "command: G0_CASE_FILTER='$filter' perf stat -e cycles,instructions,context-switches,cpu-migrations,cache-misses,branches,branch-misses $prof_dir/bench_w02_pipeline"
    env G0_CASE_FILTER="$filter" perf stat \
        -e cycles,instructions,context-switches,cpu-migrations,cache-misses,branches,branch-misses \
        -o "$out/perf-stat-$(basename "$filter" | tr -d '=').txt" \
        "$prof_dir/bench_w02_pipeline" >/dev/null 2>&1 || true
    sed 's/^/  /' "$out/perf-stat-$(basename "$filter" | tr -d '=').txt"
}
perf_one "ThreadPool d16 w1" "$tp_w1"
perf_one "ThreadPool d16 w4" "$tp_w4"
perf_one "ThreadPool d16 w8" "$tp_w8"

# --- DSO split and symbol attribution ---------------------------------------
echo
echo "## perf record: DSO split (ThreadPool d16 w4)"
echo "command: G0_CASE_FILTER='$tp_w4' perf record -F 4999 -g -o $out/perf-w4.data $prof_dir/bench_w02_pipeline"
env G0_CASE_FILTER="$tp_w4" perf record -F 4999 -g -o "$out/perf-w4.data" \
    "$prof_dir/bench_w02_pipeline" >/dev/null 2>&1 || true
perf report --stdio -i "$out/perf-w4.data" -g none --no-children --sort dso 2>/dev/null \
    | grep -E "^ +[0-9]+\.[0-9]+%" | head -8 | sed 's/^/  /' || true
echo "  top in-binary addresses resolved with addr2line:"
perf report --stdio -i "$out/perf-w4.data" -g none --no-children --dsos bench_w02_pipeline 2>/dev/null \
    | grep -E "^ +[0-9]+\.[0-9]+% +bench_w02" | awk '{print $1, $NF}' | head -14 \
    | while read -r pct addr; do
        printf '  %s %s %s\n' "$pct" "$addr" \
            "$(addr2line -f -C -e "$prof_dir/bench_w02_pipeline" "$addr" 2>/dev/null | paste -sd' @ ')"
      done || true

# --- CPU placement discriminator --------------------------------------------
place() {
    local label="$1" filter="$2" cpus="$3"
    echo
    echo "## placement: $label (taskset -c $cpus)"
    echo "command: G0_CASE_FILTER='$filter' taskset -c $cpus $prof_dir/bench_w02_pipeline"
    env G0_CASE_FILTER="$filter" taskset -c "$cpus" "$prof_dir/bench_w02_pipeline" 2>&1 \
        | grep -E "rep=[0-9]+\]" | sed 's/^/  /' || true
}
place "ThreadPool d16 w1, unpinned" "$tp_w1" "0-7"
place "ThreadPool d16 w1, one physical core (0,1)" "$tp_w1" "0,1"
place "ThreadPool d16 w1, distinct cores (0,2)" "$tp_w1" "0,2"
place "ThreadPool d16 w4, unpinned" "$tp_w4" "0-7"
place "ThreadPool d16 w4, two physical cores (0-3)" "$tp_w4" "0-3"
place "ThreadPool d16 w4, distinct cores (0,2,4,6)" "$tp_w4" "0,2,4,6"

# --- lock probe -------------------------------------------------------------
echo
echo "## lock probe (LD_PRELOAD shim, test-side)"
echo "shim source: doc/evidence .../logs/mutex_probe.c, built as mutex_probe.so"
echo "command: G0_CASE_FILTER='<case>' LD_PRELOAD=mutex_probe.so $prof_dir/bench_w02_pipeline"
for filter in "$tp_w1" "$tp_w4" "$tp_w8" "$ur_d16" "=W02-threadpool-4k-d1-w1"; do
    env G0_CASE_FILTER="$filter" LD_PRELOAD=/tmp/g0prof/mutex_probe.so \
        "$prof_dir/bench_w02_pipeline" 2>&1 | grep -E "MUTEX_SHIM|rep=0\]" | grep -v "locks=0 " | sed 's/^/  /' || true
done

echo
echo "finished=$(date -Is)"
