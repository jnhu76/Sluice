#!/usr/bin/env bash
# #593 attribution arms — SEPARATE executions, never mixed into the clean
# baseline numbers. Two observation classes here (strace census runs separately):
#   A) /usr/bin/time -v  : user/sys CPU split + voluntary/involuntary context
#      switches for whole filtered runs (2 warmup + 7 measured, stats on),
#      floor-subtracted (filter-matches-nothing run removes fixture+startup).
#      Denominator: 9 reps x 3 phases x request_count logical reads.
#   B) perf stat -x,     : user-space PMU per filtered 0-warmup/1-measured run,
#      floor-subtracted. Denominator: 3 phases x request_count logical reads.
#      The three phase passes are throughput + byte-oracle + latency-instrumented;
#      per-op PMU values are therefore a three-pass average, labeled as such.
# Every produced JSONL is validated by bench/check_run.py before it is used.
# All runs pinned taskset -c 2, same ring binary as the frozen baseline.
set -uo pipefail

wt=/home/jnhu/sluice-593-research/worktrees/baseline
bin=$wt/build-bench-ring/linux/x86_64/release
out=/home/jnhu/sluice-593-research/evidence/baseline/attribution
mkdir -p "$out"
cd "$wt"

case_id="${1:?case id or __floor__}"
target="${2:?bench_w02_pipeline|bench_w03_external_loop}"

if [ "$case_id" = "__floor__" ]; then
    filter="=__no_such_case__"
    expect=0
else
    filter="=$case_id"
    expect=1
fi
tag=$(echo "$case_id" | tr 'A-Z-' 'a-z_')

# ---- A: /usr/bin/time -v on 2/7 run ----
/usr/bin/time -v -o "$out/timev-$tag.timev.txt" \
    env SLUICE_BENCH_STATS=1 SLUICE_BENCH_CASE_FILTER="$filter" \
    SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7 \
    SLUICE_BENCH_JSONL="$out/timev-$tag.jsonl" \
    taskset -c 2 "$bin/$target" >"$out/timev-$tag.log" 2>&1
rc=$?
python3 bench/check_run.py "$out/timev-$tag.jsonl" --stage measured --exit-code "$rc" \
    --warmup 2 --measured 7 --expect-cases "$expect" || echo "CHECK_FAILED timev-$tag"
grep -E "User time|System time|Voluntary|Involuntary|Elapsed|Maximum resident" "$out/timev-$tag.timev.txt"

# ---- B: perf stat groups on 0/1 run ----
for g in "task-clock,instructions:u,cycles:u,branches:u,branch-misses:u" \
         "cache-references:u,cache-misses:u,LLC-loads:u,LLC-load-misses:u" \
         "L1-dcache-loads:u,L1-dcache-load-misses:u,dTLB-load-misses:u" \
         "page-faults,minor-faults,major-faults"; do
    gname=$(echo "$g" | tr ',:' '__')
    perf stat -x, -e "$g" -o "$out/perf-$tag-$gname.csv" \
        -- env SLUICE_BENCH_STATS=1 SLUICE_BENCH_CASE_FILTER="$filter" \
        SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 \
        SLUICE_BENCH_JSONL="$out/perfjsonl-$tag-$gname.jsonl" \
        taskset -c 2 "$bin/$target" >"$out/perf-$tag-$gname.log" 2>&1
    rc=$?
    python3 bench/check_run.py "$out/perfjsonl-$tag-$gname.jsonl" --stage measured \
        --exit-code "$rc" --warmup 0 --measured 1 --expect-cases "$expect" \
        || echo "CHECK_FAILED perf-$tag-$gname"
done
echo "attribution arms done for $case_id"
