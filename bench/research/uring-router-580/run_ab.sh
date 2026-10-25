#!/usr/bin/env bash
# #580 R6 paired A/B runner (WSL2). Disciplines inherited from #575:
#  - every timed run carries its own byte/ops oracle; failures are recorded,
#    never dropped;
#  - arms alternate AB/BA within each of 15 repetitions;
#  - A/A calibration uses byte-identical binaries under different paths
#    (path-name bias negative control);
#  - same fixture directory, same pinning (taskset -c 2), same warmup count.
set -uo pipefail
cd "$(dirname "$0")/../../.."
for bin in build-research/router_ab_baseline build-research/router_ab_we2; do
  [ -x "$bin" ] || { echo "FAIL-CLOSED: missing $bin (built only by build_arms.sh in a prepare-research-worktree.sh tree)" >&2; exit 2; }
done

REPS=${REPS:-15}
OUT=${OUT:-build-research/replay/ab-pairs.jsonl}
mkdir -p "$(dirname "$OUT")"
: > "$OUT"

run_one() { # tag arm_binary extra-args...
  local tag="$1" bin="$2"; shift 2
  taskset -c 2 "$bin" "$@" --fixture-dir /tmp \
    | python3 -c "import sys,json;d=json.load(sys.stdin);d['run_tag']='$tag';d['binary']='$(basename "$bin")';print(json.dumps(d))"
}

for shape in "cancel-c1024:cancel:--capacity 1024 --depth 64 --payload 65536 --cancel-ratio 0.2 --ops 6000 --warmup 600" \
             "cancel-c64:cancel:--capacity 64 --depth 64 --payload 65536 --cancel-ratio 0.2 --ops 6000 --warmup 600" \
             "pipeline-c1024d64:pipeline:--capacity 1024 --depth 64 --payload 4096 --ops 20000 --warmup 2000"; do
  name="${shape%%:*}"; rest="${shape#*:}"; scen="${rest%%:*}"; args="${rest#*:}"
  for rep in $(seq 1 "$REPS"); do
    if [ $((rep % 2)) -eq 1 ]; then order="AB"; else order="BA"; fi
    if [ "$order" = "AB" ]; then first=build-research/router_ab_baseline; second=build-research/router_ab_we2
    else first=build-research/router_ab_we2; second=build-research/router_ab_baseline; fi
    run_one "$name-rep$rep-a" "$first" --scenario "$scen" $args >> "$OUT" || echo "FAILED:$name:rep$rep:a" >> "$OUT"
    run_one "$name-rep$rep-b" "$second" --scenario "$scen" $args >> "$OUT" || echo "FAILED:$name:rep$rep:b" >> "$OUT"
  done
done

# A/A calibration + path-name bias control: byte-identical copies, two names.
cp build-research/router_ab_baseline build-research/router_ab_aa_alpha
cp build-research/router_ab_baseline build-research/router_ab_aa_beta
mkdir -p "$(dirname "${AA_OUT:-build-research/replay/aa-pairs.jsonl}")"
: > "${AA_OUT:-build-research/replay/aa-pairs.jsonl}"
AA_PATH="${AA_OUT:-build-research/replay/aa-pairs.jsonl}"
for shape in "cancel-c1024:cancel:--capacity 1024 --depth 64 --payload 65536 --cancel-ratio 0.2 --ops 6000 --warmup 600" \
             "pipeline-c1024d64:pipeline:--capacity 1024 --depth 64 --payload 4096 --ops 20000 --warmup 2000"; do
  name="${shape%%:*}"; rest="${shape#*:}"; scen="${rest%%:*}"; args="${rest#*:}"
  for rep in $(seq 1 "$REPS"); do
    if [ $((rep % 2)) -eq 1 ]; then first=build-research/router_ab_aa_alpha; second=build-research/router_ab_aa_beta
    else first=build-research/router_ab_aa_beta; second=build-research/router_ab_aa_alpha; fi
    taskset -c 2 "$first" --scenario "$scen" $args --fixture-dir /tmp \
      | python3 -c "import sys,json;d=json.load(sys.stdin);d['run_tag']='$name-rep$rep-a';d['binary']='$(basename "$first")';print(json.dumps(d))" >> "$AA_PATH"
    taskset -c 2 "$second" --scenario "$scen" $args --fixture-dir /tmp \
      | python3 -c "import sys,json;d=json.load(sys.stdin);d['run_tag']='$name-rep$rep-b';d['binary']='$(basename "$second")';print(json.dumps(d))" >> "$AA_PATH"
  done
done
echo "done: $OUT + aa-pairs.jsonl"
