#!/usr/bin/env python3
"""Phase 2 of the #595 R3 mechanism diagnostic session: bucket probe + resource
runs, under the same exclusive MEASUREMENT.lock. Password from stdin only."""
import fcntl
import importlib.util
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.dirname(HERE)

spec = importlib.util.spec_from_file_location("mech_session", os.path.join(HERE, "mech_session.py"))
ms = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ms)

pw = sys.stdin.readline().rstrip("\n") or None
ms._pw = pw
ms._log = open(os.path.join(RAW, "logs", "session.log"), "a", buffering=1)
ms.say("MECH_PHASE2_BEGIN")

lock_path = "/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock"
lock = open(lock_path, "rb+")
t0 = time.monotonic()
acquired = None
while time.monotonic() - t0 < 1800:
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        acquired = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        break
    except BlockingIOError:
        time.sleep(1)
if acquired is None:
    ms.say("LOCK_TIMEOUT_1800s")
    sys.exit(4)
waited = time.monotonic() - t0
st = os.fstat(lock.fileno())
rc, out, _ = ms.run(["/usr/sbin/fuser", "-v", lock_path], check=False)
ms.say(f"LOCK_ACQUIRED {acquired} waited={waited:.1f}s inode={st.st_ino} "
       f"fuser_rc={rc} fuser={(out or b'').decode(errors='replace').strip()!r}")
try:
    v_rc = ms.subprocess_run_validate = None
    import subprocess
    v = subprocess.run(["sudo", "-S", "-p", "", "-v"], input=(pw + "\n").encode(),
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=30)
    ms.say(f"SUDO_VALIDATE rc={v.returncode}")
    ms.bucket_runs(RAW)
    ms.resource_runs(RAW)
finally:
    fcntl.flock(lock, fcntl.LOCK_UN)
    ms.say(f"LOCK_RELEASED {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}")
    with open(os.path.join(RAW, "lock-log.jsonl"), "a") as f:
        f.write(json.dumps({"event": "mechanism-diagnostic-phase2",
                            "acquired_utc": acquired,
                            "released_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                            "wait_s": round(waited, 3), "inode": st.st_ino}) + "\n")
    ms._log.close()
ms.say("MECH_PHASE2_END")
