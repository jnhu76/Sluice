#!/bin/bash
# #595 R5 Phase A diagnostic sessions: exclusive flock wrapper with
# LOCK_RECEIPT entries per the R4 convention (env-context.md section 9).
# Usage: lock_session.sh <label> <command...>
# The command runs with the lock held on fd 9. Receipt goes to
# /home/jnhu/sluice-595-e5/r5/raw-diag/LOCK_RECEIPT.md.
set -u
LOCK=/home/jnhu/sluice-595-e5/MEASUREMENT.lock
RECEIPT=/home/jnhu/sluice-595-e5/r5/raw-diag/LOCK_RECEIPT.md
LABEL=${1:?label}
shift

state_json() {
  python3 - "$@" <<'EOF'
import json, time, os
def read(p):
    try:
        return open(p).read().strip()
    except OSError:
        return None
def busy(cpus, interval=0.1):
    def tot():
        out={}
        for line in open('/proc/stat'):
            if line.startswith('cpu') and line[3].isdigit():
                n=line.split(); idle=int(n[4])+int(n[5]) if len(n)>5 else int(n[4])
                out[n[0][3:]]=(sum(int(x) for x in n[1:]) - idle, sum(int(x) for x in n[1:]))
        return out
    a=tot(); time.sleep(interval); b=tot(); r={}
    for c in cpus:
        k='cpu'+c
        if k in a and k in b and b[k][1]-a[k][1] > 0:
            r[c]=round((b[k][0]-a[k][0])/(b[k][1]-a[k][1]),6)
    return r
st=os.statvfs('/tmp')
print(json.dumps({
  "cpu_busy_100ms": busy(["2","12"]),
  "cpufreq": {c: {"governor": read(f"/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_governor"),
                   "scaling_cur_freq": read(f"/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_cur_freq")}
              for c in ("2","12")},
  "loadavg": os.getloadavg(),
  "tmp_avail_bytes": st.f_bavail*st.f_frsize,
  "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
}, separators=(',',':')))
EOF
}

mkdir -p "$(dirname "$RECEIPT")"
exec 9>"$LOCK"
T0=$(date -u +%s)
if ! flock -w 1800 9; then
  echo "BLOCKED_LOCK: could not acquire $LOCK within 1800s (label=$LABEL)" >> "$RECEIPT"
  exit 42
fi
WAITED=$(python3 -c "print(round($(date -u +%s)-$T0,1))")
ACQ=$(date -u +%Y-%m-%dT%H:%M:%SZ)
FUSER=$(fuser -v "$LOCK" 2>&1 || true)
EXCL=$(flock -n "$LOCK" -c 'true' 2>/dev/null; echo $?)
{
  echo ""
  echo "## ${ACQ} ${LABEL} — acquire"
  echo "- label: ${LABEL}"
  echo "- waited_s: ${WAITED}"
  echo "- lock_file: $(stat -c '%n inode=%i dev=%d size=%s' "$LOCK")"
  echo "- fuser -v: $(echo "$FUSER" | tr '\n' ' ' | sed 's/  */ /g')"
  echo "- second-fd flock -n rc: ${EXCL} (must be nonzero = blocked: OK)"
  echo "- command: $*"
  echo "- pre-state: $(state_json)"
} >> "$RECEIPT"

"$@"
RC=$?

REL=$(date -u +%Y-%m-%dT%H:%M:%SZ)
PROBE=$(flock -n "$LOCK" -c 'echo reacquired' 2>/dev/null)
PRC=$?
{
  echo "## ${REL} ${LABEL} — release"
  echo "- held_since: ${ACQ}"
  echo "- note: command_rc=${RC} $([ $RC -eq 0 ] && echo normal || echo check-output)"
  echo "- post-state: $(state_json)"
  echo "- post-release probe rc=${PRC} out='${PROBE}'"
} >> "$RECEIPT"
exit $RC
