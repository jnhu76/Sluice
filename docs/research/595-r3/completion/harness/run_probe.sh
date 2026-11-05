#!/bin/bash
# Short thread-bucket probe under the shared exclusive flock (diagnostic only).
set -u
ROOT=/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z
HARNESS=$ROOT/harness
OUT=$ROOT/raw/calibration/bucket-probe
LOCK=/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock
R3=/home/jnhu/sluice-595-e5/r3/worktree/build/linux/x86_64/release/bench_w03_external_loop
R2=/home/jnhu/sluice-595-e5/worktree/build/linux/x86_64/release/bench_w02_pipeline
mkdir -p "$OUT/logs"
exec > >(tee -a "$OUT/logs/probe-session.log") 2>&1
echo "=== probe start $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
start=$(date -u +%Y-%m-%dT%H:%M:%SZ)
exec 9>"$LOCK"
if ! flock -w 1800 9; then
    echo "BLOCKED_LOCK"
    fuser -v "$LOCK" 2>&1
    exit 42
fi
echo "=== probe lock acquired $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
fuser -v "$LOCK" > "$OUT/logs/lock-fuser.txt" 2>&1
( exec 8>"$LOCK"; if flock -n 8; then echo "EXCLUSIVITY_CHECK: unexpectedly acquired"; else echo "EXCLUSIVITY_CHECK: blocked as expected"; fi ) > "$OUT/logs/lock-exclusivity.txt" 2>&1
cat "$OUT/logs/lock-exclusivity.txt"
rm -f "$OUT/results.jsonl"
taskset -c 0 python3 -I "$HARNESS/bucket_probe.py" \
    --binary "$R3" --case W03-uring-4k-d16 \
    --arm-var SLUICE_595_R3_ARM --arm A0 \
    --repeats 3 --out-dir "$OUT" --label R3-W03-D16-A0
taskset -c 0 python3 -I "$HARNESS/bucket_probe.py" \
    --binary "$R2" --case W02-uring-4k-d2 \
    --arm-var SLUICE_595_ARM --arm A \
    --env SLUICE_BENCH_595_DEPTHS=1 \
    --repeats 3 --out-dir "$OUT" --label R2-W02-D2-A
taskset -c 0 python3 -I "$HARNESS/bucket_probe.py" \
    --binary "$R2" --case W02-uring-4k-d2 \
    --arm-var SLUICE_595_ARM --arm B \
    --env SLUICE_BENCH_595_DEPTHS=1 \
    --repeats 3 --out-dir "$OUT" --label R2-W02-D2-B
echo "=== probe lock release $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
python3 -I - "$LOCK" "$OUT/lock-log.jsonl" "$start" <<'PY'
import json, os, sys, time
lock, out, start = sys.argv[1], sys.argv[2], sys.argv[3]
st = os.stat(lock)
rec = {"event": "acquired+released", "start_utc": start,
       "end_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
       "meta": {"lock_file": lock, "inode": st.st_ino,
                "dev_major": os.major(st.st_dev), "dev_minor": os.minor(st.st_dev)},
       "exclusivity_check": open(os.path.join(os.path.dirname(out),
                                              "logs", "lock-exclusivity.txt")).read().strip()}
with open(out, "a") as f:
    f.write(json.dumps(rec) + "\n")
PY
flock -u 9
exec 9>&-
echo "=== probe end $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
