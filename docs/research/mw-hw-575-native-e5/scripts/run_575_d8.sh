#!/usr/bin/env bash
# #575 supplementary D8 evidence: the frozen W-02 case list has no uring d8
# case, so the M2 research harness (pipeline_ab.cpp, identical source for
# every arm; only the linked static library differs) drives depth-8 pipelines
# with capacity varied independently (8 and 1024). Paired interleaved, 15
# reps, same pinning and wrappers as the main runner.
set -euo pipefail
WT=/tmp/sluice-575
OUT="${1:?usage: run_575_d8.sh <out-dir>}"
REPS="${2:-15}"
HARNESS="$WT/base/bench/native-linux-m2/pipeline_ab.cpp"
FIXTURE=/dev/shm/sluice-575-fixtures/d8-data.bin
mkdir -p "$OUT"

for arm in base ur-oponly ur-full; do
  g++ -std=c++20 -O3 -DNDEBUG -I"$WT/$arm/include" -I"$WT/$arm/bench" \
    "$HARNESS" \
    "$WT/$arm/build/linux/x86_64/release/libsluice_async.a" \
    "$WT/$arm/build/linux/x86_64/release/libsluice_core.a" \
    -luring -lpthread -o "$OUT/pipeline_ab_$arm"
  echo "arm=$arm bin_sha256=$(sha256sum "$OUT/pipeline_ab_$arm" | awk '{print $1}')"
done

"$OUT/pipeline_ab_base" --fixture="$FIXTURE" --create-fixture

JSONL="$OUT/d8.jsonl"
: > "$JSONL"
CONFIGS=("8 8" "1024 8")
ARMS=(base ur-oponly ur-full)
for rep in $(seq 1 "$REPS"); do
  for ci in 0 1; do
    cidx=$(( (ci + rep - 1) % 2 ))
    read -r CAP DEPTH <<< "${CONFIGS[$cidx]}"
    for ai in 0 1 2; do
      aidx=$(( (ai + rep - 1) % 3 ))
      arm="${ARMS[$aidx]}"
      perf stat -x, -e instructions,cycles -o "$OUT/perf.tmp" -- \
        taskset -c 2 "$OUT/pipeline_ab_$arm" \
          --arm="$arm" --backend=uring --block=4096 --depth="$DEPTH" \
          --capacity="$CAP" --requests=20000 --warmup=1 \
          --fixture="$FIXTURE" > "$OUT/out.tmp" 2>"$OUT/err.tmp" || {
            cat "$OUT/err.tmp" >&2; exit 1; }
      python3 - "$OUT/out.tmp" "$OUT/perf.tmp" "$rep" "$CAP" "$DEPTH" "$arm" "$JSONL" <<'PY'
import json, sys
outline, perffile, rep, cap, depth, arm, out = sys.argv[1:]
row = json.loads(open(outline).read().strip().splitlines()[-1])
perf = {}
for line in open(perffile):
    p = line.rstrip('\n').split(',')
    if len(p) >= 3 and p[0] and not p[0].startswith('#'):
        try: perf[p[2]] = float(p[0])
        except (ValueError, IndexError): pass
row.update(rep=int(rep), capacity=int(cap), depth=int(depth), arm=arm,
           instructions=perf.get('instructions'), cycles=perf.get('cycles'))
assert row.get('ok'), f"oracle failed rep={rep} arm={arm}"
with open(out, 'a') as f:
    f.write(json.dumps(row) + '\n')
PY
    done
  done
  echo "rep $rep/$REPS done" >&2
done
