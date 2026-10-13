#!/usr/bin/env bash
# W-03 differential stress campaign (blocker-w03-stall.md reproduction shape).
# taskset -c 0,1, ten concurrent copies of the suite per round; every run
# recorded individually (tree, round, copy, exit, duration_ms, fail_line).
set -u
OUT=/home/jnhu/f2-execution/w03-rootcause/campaign-finalhead
mkdir -p "$OUT/runs"
: > "$OUT/results.tsv"
printf 'tree\tround\tcopy\texit\tduration_ms\tfail_line\n' >> "$OUT/results.tsv"

BASE=/home/jnhu/f2-execution/w03-rootcause/build-baseline/linux/x86_64/debug/threadpool_external_loop_test
PREFIX=/home/jnhu/f2-execution/w03-rootcause/build-prefix/linux/x86_64/debug/threadpool_external_loop_test
CORRECTED=/home/jnhu/f2-execution/final-matrix/debug-y/linux/x86_64/debug/threadpool_external_loop_test

run_round() {
    local label=$1 bin=$2 round=$3
    for copy in $(seq 1 10); do
        (
            local log="$OUT/runs/${label}-r${round}-c${copy}.log"
            local start end rc
            start=$(date +%s%3N)
            taskset -c 0,1 "$bin" >"$log" 2>&1
            rc=$?
            end=$(date +%s%3N)
            local fail
            fail=$(grep -m1 '^FAIL' "$log" 2>/dev/null || true)
            [ -z "$fail" ] && fail=-
            printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$round" "$copy" "$rc" "$((end - start))" "$fail" >> "$OUT/results.tsv"
        ) &
    done
    wait
}

{
    echo "nproc=$(nproc)"
    echo "cpu=$(grep -m1 'model name' /proc/cpuinfo)"
    echo "load_before=$(cat /proc/loadavg)"
    echo "baseline_sha=0ed16588b7101f108aa7282074ba3d568f9d34e9"
    echo "prefix_sha=996f434fd1517b80d83d0bd432301f7b832aa4f8"
    echo "corrected_sha=25734314f70401472067250e68a1f9227093b7eb"
    date -Is
} > "$OUT/environment.txt"

# Primary campaign: four rounds per tree, original tree order.
for round in 1 2 3 4; do
    run_round baseline "$BASE" "$round"
    run_round prefix "$PREFIX" "$round"
    run_round corrected "$CORRECTED" "$round"
done

# Additional independent interleaved rounds for stability.
for round in 5 6 7 8 9 10 11 12 13 14 15 16; do
    case $(( (round - 5) % 3 )) in
        0) run_round baseline "$BASE" "$round" ;;
        1) run_round prefix "$PREFIX" "$round" ;;
        2) run_round corrected "$CORRECTED" "$round" ;;
    esac
done

{
    date -Is
    echo "load_after=$(cat /proc/loadavg)"
    echo "totals:"
    awk -F'\t' 'NR>1 { total[$1]++; if ($4 != 0 || $6 != "-") fail[$1]++ } END { for (t in total) printf "  %s runs=%d failures=%d\n", t, total[t], fail[t]+0 }' "$OUT/results.tsv"
} >> "$OUT/environment.txt"
echo "campaign complete"
