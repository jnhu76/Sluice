#!/bin/bash
# #595 R3 continuation §9.3 mechanism diagnostic round (kernel io_uring_enter
# basis): 3 runs/arm, WARMUP=0 MEASURED=1, W03-uring-4k-d16, taskset -c 2,
# strace -f -ttt -e trace=io_uring_enter. Diagnostic only, never gate-timing.
set -u
LOCK=/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock
ROOT=/home/jnhu/sluice-595-e5/r3-flip-20261105T1428Z
W03=/tmp/r3c2-build/linux/x86_64/release/bench_w03_external_loop
OUT=$ROOT/raw/mechanism-9_3
mkdir -p "$OUT"
exec 9>"$LOCK"
if ! flock -w 1800 9; then echo BLOCKED_LOCK; exit 42; fi
echo "acquired_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$OUT/session.log"
for arm in A0 A1 B; do
    for i in 1 2 3; do
        SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 \
        SLUICE_BENCH_CASE_FILTER='=W03-uring-4k-d16' SLUICE_BENCH_STATS=1 \
        SLUICE_595_R3_ARM=$arm SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-r3flip-bench \
        /usr/bin/strace -f -ttt -e trace=io_uring_enter -o "$OUT/${arm}-${i}.trace" \
        /usr/sbin/taskset -c 2 "$W03" > "$OUT/${arm}-${i}.log" 2>&1
        echo "arm=$arm run=$i rc=$?" >> "$OUT/session.log"
    done
done
echo "released_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$OUT/session.log"
flock -u 9
python3 -I - "$OUT" <<'PY'
import json, os, re, sys
out_dir = sys.argv[1]
result = {}
for arm in ("A0", "A1", "B"):
    runs = []
    for i in (1, 2, 3):
        path = os.path.join(out_dir, f"{arm}-{i}.trace")
        pure, hist, flags_hist = 0, {}, {}
        with open(path) as f:
            for line in f:
                if "io_uring_enter" not in line:
                    continue
                m = re.search(r"to_submit=(\d+)", line)
                if not m:
                    continue
                ts = int(m.group(1))
                fl = re.search(r"flags=(0x[0-9a-f]+|\d+)", line)
                flv = fl.group(1) if fl else "?"
                if ts > 0:
                    pure += 1
                    hist[ts] = hist.get(ts, 0) + 1
                    flags_hist[flv] = flags_hist.get(flv, 0) + 1
        runs.append({"pure_submit_enters": pure, "to_submit_hist": hist,
                     "flags_hist": flags_hist,
                     "sum_to_submit": sum(k * v for k, v in hist.items())})
    result[arm] = runs
summary = {
    "note": "3 runs/arm; per run = 1 measured + oracle + instrumented phase "
            "(3 phases x 20000 ops = 60000 data SQE expected)",
    "arms": result,
}
json.dump(summary, open(os.path.join(out_dir, "enter_summary.json"), "w"), indent=1)
for arm, runs in result.items():
    for i, r in enumerate(runs, 1):
        print(arm, i, "pure=", r["pure_submit_enters"],
              "hist=", r["to_submit_hist"], "sum=", r["sum_to_submit"])
PY
