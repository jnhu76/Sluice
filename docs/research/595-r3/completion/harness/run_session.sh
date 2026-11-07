#!/bin/bash
# #595 R3-completion calibration session: exclusive flock -> R2 rebuild ->
# A/A (W-03 d16, W-02 d2) -> R2 A_R2/B_R2 D2 -> short strace diagnostic.
# Every CPU-bearing step (build, bench, strace) runs under the shared lock.
set -u

ROOT=/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z
HARNESS=$ROOT/harness
RAW=$ROOT/raw/calibration
LOCK=/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock
R3=/home/jnhu/sluice-595-e5/r3/worktree
R2=/home/jnhu/sluice-595-e5/worktree
R3_BENCH=$R3/build/linux/x86_64/release/bench_w03_external_loop
R3_SHA=0ebfb7f0c204579fe2cc9e8d7e88cee2de9112e25c0cfb11e8502e59fc4fe2d1
R2_BENCH=$R2/build/linux/x86_64/release/bench_w02_pipeline
TMPDIR_BENCH=/tmp/sluice-595-r3c

mkdir -p "$RAW/logs" "$RAW/build"
exec > >(tee -a "$RAW/logs/session.log") 2>&1

echo "=== session start $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="

# ---------- lock ----------
lock_start_epoch=$(date +%s)
lock_start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
exec 9>"$LOCK"
if ! flock -w 1800 9; then
    lock_end_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    python3 -I - "$LOCK" "$lock_start_utc" "$lock_end_utc" "$RAW/lock-log.jsonl" <<'PY'
import json, os, subprocess, sys
lock, start, end, out = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
def run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout
    except Exception as e:
        return f"error: {e}"
try:
    st = os.stat(lock)
    meta = {"lock_file": lock, "inode": st.st_ino, "mtime": st.st_mtime, "size": st.st_size}
except OSError as e:
    meta = {"lock_file": lock, "error": str(e)}
rec = {"event": "blocked_lock", "start_utc": start, "end_utc": end,
       "wait_s": 1800, "meta": meta,
       "fuser": run(["fuser", "-v", lock]),
       "lslocks": run(["lslocks"]),
       "ps": run(["ps", "-eo", "pid,psr,pcpu,comm", "--sort=-pcpu"])}
with open(out, "a") as f:
    f.write(json.dumps(rec) + "\n")
PY
    echo "BLOCKED_LOCK: could not acquire $LOCK within 1800s"
    exit 42
fi
lock_wait_s=$(( $(date +%s) - lock_start_epoch ))
python3 -I - "$LOCK" "$lock_start_utc" "$lock_wait_s" "$RAW/lock-log.jsonl" <<'PY'
import json, os, subprocess, sys
lock, start, wait_s, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
def run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout
    except Exception as e:
        return f"error: {e}"
st = os.stat(lock)
holders = []
want_major, want_minor = os.major(st.st_dev), os.minor(st.st_dev)
try:
    with open("/proc/locks") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 8 or parts[5].count(":") != 2:
                continue
            try:
                major, minor, inode = (int(x, 16) for x in parts[5].split(":"))
            except ValueError:
                continue
            if inode == st.st_ino and major == want_major and minor == want_minor:
                holders.append({"type": parts[1], "mode": parts[2], "pid": int(parts[4])})
except OSError:
    pass
rec = {"event": "acquired", "start_utc": start, "wait_s": wait_s,
       "acquired_utc": __import__("time").strftime("%Y-%m-%dT%H:%M:%SZ", __import__("time").gmtime()),
       "meta": {"lock_file": lock, "inode": st.st_ino, "mtime": st.st_mtime,
                "size": st.st_size},
       "holders": holders, "fuser": run(["fuser", "-v", lock])}
with open(out, "a") as f:
    f.write(json.dumps(rec) + "\n")
PY
echo "=== lock acquired after ${lock_wait_s}s $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="

# ---------- pre-state ----------
python3 -I - "$RAW/pre-state.json" "$LOCK" <<'PY'
import json, os, subprocess, sys
out, lock = sys.argv[1], sys.argv[2]
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
rec = {"utc": __import__("time").strftime("%Y-%m-%dT%H:%M:%SZ", __import__("time").gmtime()),
       "loadavg": list(os.getloadavg()), "tmp": df(),
       "cpufreq": {c: cpufreq(c) for c in ("2", "12")},
       "nproc": os.cpu_count(),
       "df_h_tmp": subprocess.run(["df", "-h", "/tmp"], capture_output=True, text=True).stdout,
       "ps_top": subprocess.run(["ps", "-eo", "pid,psr,pcpu,comm", "--sort=-pcpu"],
                                capture_output=True, text=True).stdout.splitlines()[:15],
       "perf_event_paranoid": rd("/proc/sys/kernel/perf_event_paranoid"),
       "unprivileged_bpf_disabled": rd("/proc/sys/kernel/unprivileged_bpf_disabled"),
       "lock_file": lock}
json.dump(rec, open(out, "w"), indent=1)
PY

# ---------- R3 provenance ----------
{
    echo "HEAD $(git -C "$R3" rev-parse HEAD)"
    echo "STATUS_LINES $(git -C "$R3" status --porcelain | wc -l)"
    git -C "$R3" status --porcelain
    echo "DIFF_SHA $(git -C "$R3" diff HEAD | sha256sum)"
    sha256sum "$R3_BENCH" "$R3/build/linux/x86_64/release/bench_w02_pipeline"
} > "$RAW/build/r3-provenance.txt" 2>&1

# ---------- R2 rebuild (build only; sources untouched) ----------
R2_BUILD_OK=0
{
    echo "HEAD $(git -C "$R2" rev-parse HEAD)"
    echo "STATUS_BEFORE $(git -C "$R2" status --porcelain | wc -l)"
    git -C "$R2" status --porcelain
    echo "DIFF_SHA_BEFORE $(git -C "$R2" diff HEAD | sha256sum)"
    git -C "$R2" apply --reverse --check /home/jnhu/sluice-595-e5/evidence/measurement/patch.diff
    echo "REVERSE_CHECK_EXIT $?"
    sha256sum /home/jnhu/sluice-595-e5/evidence/measurement/patch.diff
} > "$RAW/build/r2-provenance-before.txt" 2>&1

echo "=== R2 build $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
( cd "$R2" && xmake f -m release --liburing=y ) > "$RAW/build/r2-xmake-config.log" 2>&1
cfg_rc=$?
( cd "$R2" && xmake build -y bench_w02_pipeline uring_595_bounded_progress_test ) \
    > "$RAW/build/r2-build.log" 2>&1
build_rc=$?
if [ $cfg_rc -eq 0 ] && [ $build_rc -eq 0 ] && [ -x "$R2_BENCH" ]; then
    R2_BUILD_OK=1
fi
echo "R2_CONFIG_RC=$cfg_rc R2_BUILD_RC=$build_rc R2_BUILD_OK=$R2_BUILD_OK"
{
    echo "CONFIG_RC $cfg_rc"
    echo "BUILD_RC $build_rc"
    sha256sum "$R2_BENCH" "$R2/build/linux/x86_64/release/libsluice_async.a" \
        "$R2/build/linux/x86_64/release/uring_595_bounded_progress_test" 2>&1
    echo "STATUS_AFTER $(git -C "$R2" status --porcelain | wc -l)"
    echo "DIFF_SHA_AFTER $(git -C "$R2" diff HEAD | sha256sum)"
} > "$RAW/build/r2-provenance-after.txt" 2>&1
R2_SHA=$(sha256sum "$R2_BENCH" | awk '{print $1}')
echo "R2_BENCH_SHA=$R2_SHA"

# ---------- block runner ----------
run_block() {
    local phase="$1"; shift
    echo "=== block $phase start $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    taskset -c 0 python3 -I "$HARNESS/measure_pairs.py" \
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

AA_RC=99
if [ -x "$R3_BENCH" ] && [ "$(sha256sum "$R3_BENCH" | awk '{print $1}')" = "$R3_SHA" ]; then
    echo "=== smoke R3 $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    taskset -c 0 python3 -I "$HARNESS/measure_pairs.py" \
        --phase SMOKE-W03 --phase-dir "$RAW/smoke-w03" \
        --case W03-uring-4k-d16 --pairs 1 \
        --binary "$R3_BENCH" --expect-sha "$R3_SHA" \
        --cpu 2 --cpus 2,12 --arms AA \
        --arm-var SLUICE_595_R3_ARM --arm-a A0 --arm-b A0 \
        --warmup 0 --measured 1 --stats 0 --expected-ops 20000 \
        --tmp-dir "$TMPDIR_BENCH" --lock-file "$LOCK"
    smoke_rc=$?
    echo "SMOKE-W03 rc=$smoke_rc"
    if [ $smoke_rc -ne 0 ]; then
        echo "SMOKE-W03 failed; aborting before timed blocks"
        exit 44
    fi
    run_block AA-W03-D16 \
        --case W03-uring-4k-d16 \
        --pairs 24 \
        --binary "$R3_BENCH" \
        --expect-sha "$R3_SHA" \
        --arms AA \
        --arm-var SLUICE_595_R3_ARM \
        --arm-a A0 --arm-b A0
    AA_RC=$?
    python3 -I "$HARNESS/analyze_pairs.py" --phase-dir "$RAW/AA-W03-D16" --arms AA \
        --out "$RAW/AA-W03-D16/summary.json" > "$RAW/AA-W03-D16/analysis.log" 2>&1
    echo "AA-W03-D16 analysis rc=$?"
else
    echo "AA-W03-D16 BLOCKED_PROVENANCE (R3 bench missing or sha mismatch)"
    AA_RC=41
fi

R2AA_RC=99
R2AB_RC=99
if [ $R2_BUILD_OK -eq 1 ]; then
    echo "=== smoke R2 $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    taskset -c 0 python3 -I "$HARNESS/measure_pairs.py" \
        --phase SMOKE-W02D2 --phase-dir "$RAW/smoke-w02d2" \
        --case W02-uring-4k-d2 --pairs 1 \
        --binary "$R2_BENCH" --expect-sha "$R2_SHA" \
        --cpu 2 --cpus 2,12 --arms AA \
        --arm-var SLUICE_595_ARM --arm-a A --arm-b A \
        --env SLUICE_BENCH_595_DEPTHS=1 \
        --warmup 0 --measured 1 --stats 0 --expected-ops 20000 \
        --tmp-dir "$TMPDIR_BENCH" --lock-file "$LOCK"
    smoke2_rc=$?
    echo "SMOKE-W02D2 rc=$smoke2_rc"
    if [ $smoke2_rc -ne 0 ]; then
        echo "SMOKE-W02D2 failed; R2 blocks BLOCKED_LEGACY_REPLAY"
        R2_BUILD_OK=0
    fi
fi
if [ $R2_BUILD_OK -eq 1 ]; then
    run_block AA-W02-D2 \
        --case W02-uring-4k-d2 \
        --pairs 24 \
        --binary "$R2_BENCH" \
        --expect-sha "$R2_SHA" \
        --arms AA \
        --arm-var SLUICE_595_ARM \
        --arm-a A --arm-b A \
        --env SLUICE_BENCH_595_DEPTHS=1
    R2AA_RC=$?
    python3 -I "$HARNESS/analyze_pairs.py" --phase-dir "$RAW/AA-W02-D2" --arms AA \
        --out "$RAW/AA-W02-D2/summary.json" > "$RAW/AA-W02-D2/analysis.log" 2>&1
    echo "AA-W02-D2 analysis rc=$?"

    run_block R2-D2 \
        --case W02-uring-4k-d2 \
        --pairs 24 \
        --binary "$R2_BENCH" \
        --expect-sha "$R2_SHA" \
        --arms AB \
        --arm-var SLUICE_595_ARM \
        --arm-a A --arm-b B \
        --env SLUICE_BENCH_595_DEPTHS=1
    R2AB_RC=$?
    python3 -I "$HARNESS/analyze_pairs.py" --phase-dir "$RAW/R2-D2" --arms AB \
        --out "$RAW/R2-D2/summary.json" > "$RAW/R2-D2/analysis.log" 2>&1
    echo "R2-D2 analysis rc=$?"

    echo "=== diag-r2 start $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    taskset -c 0 python3 -I "$HARNESS/diag_r2.py" \
        --binary "$R2_BENCH" \
        --phase-dir "$RAW/diag-r2" \
        --case W02-uring-4k-d2 --cpu 2 --repeats 3 \
        --tmp-dir "$TMPDIR_BENCH"
    echo "diag-r2 rc=$?"
    ( cd "$R2" && SLUICE_595_ARM=B ./build/linux/x86_64/release/uring_595_bounded_progress_test ) \
        > "$RAW/diag-r2/logs/bounded_progress_test.log" 2>&1
    echo "bounded_progress_test rc=$?"
else
    echo "LEGACY R2 blocks BLOCKED_LEGACY_REPLAY (rebuild failed; see build logs)"
fi

# ---------- post-state ----------
python3 -I - "$RAW/post-state.json" "$LOCK" <<'PY'
import json, os, subprocess, sys
out, lock = sys.argv[1], sys.argv[2]
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
rec = {"utc": __import__("time").strftime("%Y-%m-%dT%H:%M:%SZ", __import__("time").gmtime()),
       "loadavg": list(os.getloadavg()), "tmp": df(),
       "cpufreq": {c: cpufreq(c) for c in ("2", "12")},
       "df_h_tmp": subprocess.run(["df", "-h", "/tmp"], capture_output=True, text=True).stdout,
       "lock_file": lock}
json.dump(rec, open(out, "w"), indent=1)
PY

# ---------- evidence copies ----------
mkdir -p "$ROOT/evidence/measurement-main/aa" "$ROOT/evidence/measurement-legacy"
if [ -d "$RAW/AA-W03-D16" ]; then
    rm -rf "$ROOT/evidence/measurement-main/aa/aa-w03-d16"
    cp -a "$RAW/AA-W03-D16" "$ROOT/evidence/measurement-main/aa/aa-w03-d16"
fi
if [ -d "$RAW/AA-W02-D2" ]; then
    rm -rf "$ROOT/evidence/measurement-legacy/aa-w02-d2" \
           "$ROOT/evidence/measurement-main/aa/aa-w02-d2"
    cp -a "$RAW/AA-W02-D2" "$ROOT/evidence/measurement-legacy/aa-w02-d2"
    cp -a "$RAW/AA-W02-D2" "$ROOT/evidence/measurement-main/aa/aa-w02-d2"
fi
if [ -d "$RAW/R2-D2" ]; then
    rm -rf "$ROOT/evidence/measurement-legacy/w02-d2"
    cp -a "$RAW/R2-D2" "$ROOT/evidence/measurement-legacy/w02-d2"
fi
if [ -d "$RAW/diag-r2" ]; then
    rm -rf "$ROOT/evidence/measurement-legacy/diag-r2"
    cp -a "$RAW/diag-r2" "$ROOT/evidence/measurement-legacy/diag-r2"
fi

echo "=== lock release $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
python3 -I - "$LOCK" "$RAW/lock-log.jsonl" <<'PY'
import json, os, sys, time
lock, out = sys.argv[1], sys.argv[2]
st = os.stat(lock)
rec = {"event": "released",
       "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
       "meta": {"lock_file": lock, "inode": st.st_ino, "mtime": st.st_mtime}}
with open(out, "a") as f:
    f.write(json.dumps(rec) + "\n")
PY
flock -u 9
exec 9>&-
echo "=== session end $(date -u +%Y-%m-%dT%H:%M:%SZ) AA_RC=$AA_RC R2AA_RC=$R2AA_RC R2AB_RC=$R2AB_RC R2_BUILD_OK=$R2_BUILD_OK ==="
