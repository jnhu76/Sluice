#!/bin/bash
# #595 R3 continuation measurement session (post-flip).
# Order: SMOKE-W03 -> AA-W03-D16 -> AA-W02-D2 (both A/A before any B timing,
# per PREREG_ADDENDUM_CONTINUATION §3) -> TRIPLET-W03-D16 -> AB-W02R3-D2.
# All blocks run under the shared E5 MEASUREMENT.lock (fd 9 held throughout).
set -u
LOCK=/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock
ROOT=/home/jnhu/sluice-595-e5/r3-flip-20261105T1428Z
HARNESS=$ROOT/harness
RAW=$ROOT/raw
NEWBIN=/tmp/r3c2-build/linux/x86_64/release
W03=$NEWBIN/bench_w03_external_loop
W02=$NEWBIN/bench_w02_pipeline
W03_SHA=bb142d69788a5931406190022d9774b3c99e1db8f74a523748730271522e73d3
W02_SHA=5104ce250e073f3403eaec3cab5d441d68965817fa802e37450d62cdcd3b12ab
TMPDIR_BENCH=/tmp/sluice-595-r3flip-bench
FROZEN_AN=/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/harness/analyze_pairs.py
mkdir -p "$RAW/build" "$TMPDIR_BENCH"

exec 9>"$LOCK"
if ! flock -w 1800 9; then
    echo "BLOCKED_LOCK: could not acquire $LOCK within 1800s"
    exit 42
fi
echo "=== session lock acquired $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="

python3 -I - "$RAW/pre-state.json" "$LOCK" <<'PY'
import json, os, subprocess, time
lock = os.sys.argv[2]
def rd(p):
    try:
        return open(p).read().strip()
    except OSError:
        return None
def df():
    st = os.statvfs("/tmp")
    return {"total": st.f_blocks * st.f_frsize, "avail": st.f_bavail * st.f_frsize}
def cpufreq(cpu):
    b = f"/sys/devices/system/cpu/cpu{cpu}/cpufreq"
    return {"governor": rd(b + "/scaling_governor"),
            "scaling_cur_freq": rd(b + "/scaling_cur_freq")}
rec = {"utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
       "loadavg": list(os.getloadavg()), "tmp": df(),
       "cpufreq": {c: cpufreq(c) for c in ("2", "12")},
       "ps_top": subprocess.run(["ps", "-eo", "pid,psr,pcpu,comm", "--sort=-pcpu"],
                                capture_output=True, text=True).stdout.splitlines()[:15],
       "lock_file": lock}
json.dump(rec, open(os.sys.argv[1], "w"), indent=1)
PY

{
    echo "HEAD $(git -C /home/jnhu/sluice-595-e5/r3/worktree rev-parse HEAD)"
    echo "STATUS_LINES $(git -C /home/jnhu/sluice-595-e5/r3/worktree status --porcelain | wc -l)"
    echo "DIFF_SHA $(git -C /home/jnhu/sluice-595-e5/r3/worktree diff HEAD | sha256sum)"
    sha256sum "$W03" "$W02"
} > "$RAW/build/provenance.txt" 2>&1
cat "$RAW/build/provenance.txt"

run_block() {
    local phase="$1"; shift
    echo "=== block $phase start $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    taskset -c 0 python3 -I "$HARNESS/measure_pairs_cont.py" \
        --phase "$phase" \
        --phase-dir "$RAW/$phase" \
        --lock-file "$LOCK" \
        --cpu 2 --cpus 2,12 \
        --warmup 1 --measured 5 --stats 0 --expected-ops 20000 \
        --tmp-dir "$TMPDIR_BENCH" "$@"
    local rc=$?
    echo "=== block $phase rc=$rc end $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    return $rc
}

# ---------- smoke (fixture creation + sanity) ----------
taskset -c 0 python3 -I "$HARNESS/measure_pairs_cont.py" \
    --phase SMOKE-W03 --phase-dir "$RAW/smoke-w03" \
    --case W03-uring-4k-d16 --pairs 1 \
    --binary "$W03" --expect-sha "$W03_SHA" \
    --cpu 2 --cpus 2,12 --arms AA \
    --arm-var SLUICE_595_R3_ARM --arm-a A0 --arm-b A0 \
    --warmup 0 --measured 1 --stats 0 --expected-ops 20000 \
    --tmp-dir "$TMPDIR_BENCH" --lock-file "$LOCK"
smoke1=$?
echo "SMOKE-W03 rc=$smoke1"

taskset -c 0 python3 -I "$HARNESS/measure_pairs_cont.py" \
    --phase SMOKE-W02 --phase-dir "$RAW/smoke-w02" \
    --case W02-uring-4k-d2 --pairs 1 \
    --binary "$W02" --expect-sha "$W02_SHA" \
    --cpu 2 --cpus 2,12 --arms AA \
    --arm-var SLUICE_595_R3_ARM --arm-a A0 --arm-b A0 \
    --env SLUICE_BENCH_595_DEPTHS=1 \
    --warmup 0 --measured 1 --stats 0 --expected-ops 20000 \
    --tmp-dir "$TMPDIR_BENCH" --lock-file "$LOCK"
smoke2=$?
echo "SMOKE-W02 rc=$smoke2"
if [ $smoke1 -ne 0 ] || [ $smoke2 -ne 0 ]; then
    echo "smoke failed; aborting before timed blocks"
    flock -u 9
    exit 44
fi

# ---------- fresh A/A (both before any B timing) ----------
run_block AA-W03-D16 \
    --case W03-uring-4k-d16 --pairs 24 \
    --binary "$W03" --expect-sha "$W03_SHA" \
    --arms AA --arm-var SLUICE_595_R3_ARM --arm-a A0 --arm-b A0
AA_W03_RC=$?
python3 -I "$FROZEN_AN" --phase-dir "$RAW/AA-W03-D16" --arms AA \
    --out "$RAW/AA-W03-D16/summary.json" > "$RAW/AA-W03-D16/analysis.log" 2>&1
echo "AA-W03-D16 analysis rc=$?"

run_block AA-W02-D2 \
    --case W02-uring-4k-d2 --pairs 24 \
    --binary "$W02" --expect-sha "$W02_SHA" \
    --arms AA --arm-var SLUICE_595_R3_ARM --arm-a A0 --arm-b A0 \
    --env SLUICE_BENCH_595_DEPTHS=1
AA_W02_RC=$?
python3 -I "$FROZEN_AN" --phase-dir "$RAW/AA-W02-D2" --arms AA \
    --out "$RAW/AA-W02-D2/summary.json" > "$RAW/AA-W02-D2/analysis.log" 2>&1
echo "AA-W02-D2 analysis rc=$?"

# ---------- W-03 d16 24-triplet (frozen §6.2(b) order) ----------
echo "=== block TRIPLET-W03-D16 start $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
taskset -c 0 python3 -I "$HARNESS/measure_triplets.py" \
    --phase TRIPLET-W03-D16 \
    --phase-dir "$RAW/TRIPLET-W03-D16" \
    --case W03-uring-4k-d16 --triplets 24 \
    --binary "$W03" --expect-sha "$W03_SHA" \
    --cpu 2 --cpus 2,12 \
    --warmup 1 --measured 5 --stats 0 --expected-ops 20000 \
    --tmp-dir "$TMPDIR_BENCH" --lock-file "$LOCK"
TRIPLET_RC=$?
echo "=== block TRIPLET-W03-D16 rc=$TRIPLET_RC end $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
python3 -I "$HARNESS/analyze_triplets.py" --phase-dir "$RAW/TRIPLET-W03-D16" \
    --out "$RAW/TRIPLET-W03-D16/summary.json" > "$RAW/TRIPLET-W03-D16/analysis.log" 2>&1
echo "TRIPLET analysis rc=$?"

# ---------- W02 D2 new B - A0 (descriptive per addendum §2) ----------
run_block AB-W02R3-D2 \
    --case W02-uring-4k-d2 --pairs 24 \
    --binary "$W02" --expect-sha "$W02_SHA" \
    --arms AB --arm-var SLUICE_595_R3_ARM --arm-a A0 --arm-b B \
    --env SLUICE_BENCH_595_DEPTHS=1
AB_W02_RC=$?
python3 -I "$FROZEN_AN" --phase-dir "$RAW/AB-W02R3-D2" --arms AB \
    --out "$RAW/AB-W02R3-D2/summary.json" > "$RAW/AB-W02R3-D2/analysis.log" 2>&1
echo "AB-W02R3-D2 analysis rc=$?"

# ---------- post state ----------
{
    echo "HEAD $(git -C /home/jnhu/sluice-595-e5/r3/worktree rev-parse HEAD)"
    echo "DIFF_SHA $(git -C /home/jnhu/sluice-595-e5/r3/worktree diff HEAD | sha256sum)"
    sha256sum "$W03" "$W02"
    echo "AA_W03_RC=$AA_W03_RC AA_W02_RC=$AA_W02_RC TRIPLET_RC=$TRIPLET_RC AB_W02_RC=$AB_W02_RC"
    date -u +%Y-%m-%dT%H:%M:%SZ
} > "$RAW/build/post-state.txt" 2>&1

echo "=== session complete; releasing lock $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
flock -u 9
exit 0
