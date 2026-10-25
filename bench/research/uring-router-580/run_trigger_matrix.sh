#!/usr/bin/env bash
# #580 R2-B trigger matrix (WSL2). Count-only evidence from the instrumented
# probe; timings are context, not performance claims.
set -uo pipefail
cd "$(dirname "$0")/../../.."
out="docs/research/uring-router-580/results/trigger-matrix.jsonl"
: > "$out"
run() {
  local tag="$1"; shift
  echo "== $tag" >&2
  for rep in 1 2; do
    taskset -c 2 ./build-research/lookup_probe "$@" --fixture-dir /tmp \
      | python3 -c "import sys,json; d=json.load(sys.stdin); d['rep']=$rep; d['matrix_tag']='$tag'; print(json.dumps(d))" >> "$out" \
      || echo "{\"matrix_tag\":\"$tag\",\"rep\":$rep,\"ok\":false}" >> "$out"
  done
}
# steady pipelines: capacity/depth coupling + payload
run c16d16        --scenario pipeline --capacity 16   --depth 16  --ops 20000 --payload 4096
run c64d16        --scenario pipeline --capacity 64   --depth 16  --ops 20000 --payload 4096
run c1024d1       --scenario pipeline --capacity 1024 --depth 1   --ops 20000 --payload 4096
run c1024d16      --scenario pipeline --capacity 1024 --depth 16  --ops 20000 --payload 4096
run c1024d64      --scenario pipeline --capacity 1024 --depth 64  --ops 20000 --payload 4096
run c1024d16-64k  --scenario pipeline --capacity 1024 --depth 16  --ops 5000  --payload 65536
run c1024d16-1m   --scenario pipeline --capacity 1024 --depth 16  --ops 2000  --payload 1048576
# churn: retire/reinstall storm at high capacity
run churn-c1024   --scenario churn   --capacity 1024 --depth 64  --ops 20000 --payload 4096
# cancel mix: in-flight cancels at two capacities
run cancel-c64    --scenario cancel  --capacity 64   --depth 64  --ops 10000 --payload 65536 --cancel-ratio 0.2
run cancel-c1024  --scenario cancel  --capacity 1024 --depth 64 --ops 10000 --payload 65536 --cancel-ratio 0.2
# metadata (statx)
run meta-c64d16   --scenario metadata --capacity 64  --depth 16  --ops 10000
# SEAM: deterministic stale-CQE misses
run miss-c1024    --scenario miss-inject --capacity 1024 --depth 1 --ops 100 --inject-misses 1000
run miss-c16      --scenario miss-inject --capacity 16   --depth 1 --ops 100 --inject-misses 1000
echo "written: $out"
