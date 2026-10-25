#!/usr/bin/env bash
# combo K4-b+K1(a+b) mechanism census (counts only).
set -uo pipefail
full_bin=/home/jnhu/sluice-593-research/worktrees/baseline/build-bench-ring/linux/x86_64/release
var_bin=/home/jnhu/sluice-593-research/worktrees/combo-K4-b-K1ab/build-bench-ring/linux/x86_64/release
checker=/home/jnhu/sluice-593-research/worktrees/baseline/bench/check_run.py
out="$(cd "$(dirname "$0")" && pwd)/census"
case_id="${1:?case id}"; target="${2:?target}"; tag="${3:?tag}"; reps="${4:-1}"
dir="$out/$tag"; mkdir -p "$dir"
run_one() {
    local arm="$1" bin="$2" rep="$3" kind="$4"
    local p="$dir/${arm}-${kind}-${rep}"
    local trace_opts=(-f -c -e trace=io_uring_enter,io_uring_setup,io_uring_register,eventfd2,read,write,futex,epoll_wait,epoll_pwait,poll,ppoll)
    local perf_events=cycles:u,instructions:u,branches:u,branch-misses:u,cache-references:u,cache-misses:u,L1-dcache-loads:u,L1-dcache-load-misses:u,LLC-loads:u,LLC-load-misses:u
    case "$kind" in
        time) /usr/bin/time -v -o "$p.time.txt" env SLUICE_BENCH_STATS=1 SLUICE_BENCH_CASE_FILTER="=$case_id" SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 SLUICE_BENCH_JSONL="$p.jsonl" taskset -c 2 "$bin/$target" >"$p.log" 2>&1 ;;
        strace) strace "${trace_opts[@]}" -o "$p.strace.txt" env SLUICE_BENCH_STATS=1 SLUICE_BENCH_CASE_FILTER="=$case_id" SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 SLUICE_BENCH_JSONL="$p.jsonl" taskset -c 2 "$bin/$target" >"$p.log" 2>&1 ;;
        perf) perf stat -x, -e "$perf_events" -o "$p.perf.txt" env SLUICE_BENCH_STATS=1 SLUICE_BENCH_CASE_FILTER="=$case_id" SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 SLUICE_BENCH_JSONL="$p.jsonl" taskset -c 2 "$bin/$target" >"$p.log" 2>&1 ;;
    esac
    local rc=$?
    python3 "$checker" "$p.jsonl" --stage measured --exit-code "$rc" --warmup 0 --measured 1 --expect-cases 1 >"$p.check" 2>&1
    local check_rc=$?
    echo "$tag $case_id $arm $kind rep=$rep rc=$rc check_rc=$check_rc" >>"$dir/status.log"
    return 0
}
{ echo "# census $tag case=$case_id target=$target reps=$reps"; echo "started=$(date -Is) host=$(hostname) kernel=$(uname -r)"; echo "var_w02=$(sha256sum "$var_bin/bench_w02_pipeline" | cut -d' ' -f1)"; } >"$dir/provenance.txt"
for rep in $(seq 1 "$reps"); do
    for arm in base var; do
        bin="$full_bin"; [ "$arm" = "var" ] && bin="$var_bin"
        run_one "$arm" "$bin" "$rep" time
        run_one "$arm" "$bin" "$rep" strace
        run_one "$arm" "$bin" "$rep" perf
    done
done
echo "DONE census $tag reps=$reps"
