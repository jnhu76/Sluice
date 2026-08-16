#!/usr/bin/env bash
# ci-hang-watchdog.sh — bounded hang watchdog for the CI test step.
#
# Runs "<cmd...>" in the background. If the command has not exited within
# TIMEOUT_SECS, every surviving test binary is frozen (SIGSTOP), dumped with
# a gdb all-thread backtrace into $HANG_ARTIFACTS_DIR (default: the current
# directory), and killed. The wrapper then exits non-zero (fail-closed: a
# hang is a FAIL, never a silent green).
#
# This converts a rare CI stall (e.g. sluice_copy_pipeline_stress_test,
# issue #67: a lost-wake/ticket race that only manifests under the GitHub
# runner's host-level timing distortion and has resisted local load
# reproduction) into a root-cause capture: the backtrace shows exactly which
# thread is parked where.
#
# Prerequisite: gdb must be able to attach to the surviving binaries. On the
# GitHub runner this requires `sudo sysctl -w kernel.yama.ptrace_scope=0`
# (the runner user has passwordless sudo); the CI step does that first.
#
# Usage:
#   bash scripts/ci-hang-watchdog.sh <timeout-secs> <cmd...>
# Exit: 0 iff <cmd...> exited 0 within the budget; 124-on-hang becomes 1.
set -u

TIMEOUT_SECS="${1:?usage: ci-hang-watchdog.sh <timeout-secs> <cmd...>}"
shift
OUT_DIR="${HANG_ARTIFACTS_DIR:-.}"
mkdir -p "$OUT_DIR"

echo "[hang-watchdog] budget=${TIMEOUT_SECS}s cmd: $*"
"$@" &
child=$!
deadline=$((SECONDS + TIMEOUT_SECS))

# The watchdog must never treat itself or one of its ancestors as a
# "survivor". pgrep -f matches full command lines, and an outer wrapper
# (agent shell, CI dispatcher) can legitimately embed the build-path pattern
# in its own arguments; freezing/killing it would kill the watchdog's session
# (observed locally 2026-08-16 during an offline watchdog test). Walk our own
# PPid chain once and skip those pids during survivor capture.
self_ancestry=" $$"
walk=$$
while [ "$walk" -gt 1 ]; do
    walk=$(awk '/^PPid:/{print $2}' "/proc/$walk/status" 2>/dev/null)
    [ -z "$walk" ] && break
    self_ancestry=" $walk$self_ancestry"
done

while kill -0 "$child" 2>/dev/null; do
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "[hang-watchdog] TIMEOUT after ${TIMEOUT_SECS}s — capturing survivors"
        # Process-tree snapshot first: which test binaries are alive, their
        # parentage (xmake -> test binary), state, and elapsed time. This is
        # the minimum evidence to say WHO hung and for HOW LONG before the
        # backtraces, without needing gdb.
        echo "[hang-watchdog]   process tree (xmake/test procs):"
        ps -e -o pid,ppid,stat,etime,comm,args --forest 2>/dev/null \
            | grep -E 'xmake|_test|hang-watchdog' | grep -v grep \
            | head -40 | sed 's/^/[hang-watchdog]     /'
        survivors=0
        for p in $(pgrep -f 'build/linux/x86_64/debug/.*_test' || true); do
            case " $self_ancestry " in
                *" $p "*) continue ;;
            esac
            cmd=$(tr '\0' ' ' <"/proc/$p/cmdline" 2>/dev/null || echo "unknown")
            echo "[hang-watchdog]   survivor pid=$p: $cmd"
            if [ -r "/proc/$p/status" ]; then
                ppid=$(awk '/^PPid:/{print $2}' "/proc/$p/status")
                pstate=$(awk '/^State:/{print $2}' "/proc/$p/status")
                # Elapsed seconds from /proc starttime (field 22 of stat,
                # clock ticks) vs /proc/uptime; degrade to '?' on any gap.
                st_ticks=$(awk '{print $22}' "/proc/$p/stat" 2>/dev/null)
                up_secs=$(awk '{print int($1)}' /proc/uptime 2>/dev/null)
                if [ -n "$st_ticks" ] && [ -n "$up_secs" ]; then
                    pelapsed=$(( up_secs - st_ticks / 100 ))
                else
                    pelapsed='?'
                fi
                echo "[hang-watchdog]     ppid=$ppid state=$pstate elapsed=${pelapsed}s"
            fi
            # SIGSTOP first: freeze the process so the backtrace shows the
            # stuck state, not the state during gdb's own attach.
            kill -STOP "$p" 2>/dev/null || continue
            bt="$OUT_DIR/hang-pid${p}.bt"
            : >"$bt"
            if command -v gdb >/dev/null 2>&1; then
                gdb -batch -p "$p" -ex "thread apply all bt" >>"$bt" 2>&1
            else
                # gdb is not always present on the runner image (observed
                # 2026-08-15: ubuntu-24.04 image shipped without gdb, the
                # capture produced "gdb: command not found" and the hang had
                # NO backtrace). Degrade to per-thread kernel state — enough
                # to distinguish parked-in-futex from userspace spin, which
                # is the first branch point of any hang triage.
                echo "(gdb unavailable — /proc fallback)" >>"$bt"
                for t in /proc/$p/task/*; do
                    {
                        echo "== thread ${t##*/} =="
                        echo "state: $(cat "$t/stat" 2>/dev/null | cut -d' ' -f3)"
                        echo "wchan: $(cat "$t/wchan" 2>/dev/null)"
                        echo "syscall: $(cat "$t/syscall" 2>/dev/null | cut -c1-60)"
                    } >>"$bt" 2>/dev/null
                done
            fi
            echo "[hang-watchdog]   backtrace -> $bt ($(wc -l <"$bt") lines)"
            # Print the capture into the job log so it is visible in the
            # Actions UI without an artifact download step.
            echo "[hang-watchdog]   ---- begin $bt ----"
            cat "$bt"
            echo "[hang-watchdog]   ---- end $bt ----"
            kill -9 "$p" 2>/dev/null || true
            survivors=$((survivors + 1))
        done
        if [ "$survivors" -eq 0 ]; then
            echo "[hang-watchdog]   no surviving test binaries found (hang may be inside xmake itself)"
        fi
        kill -9 "$child" 2>/dev/null || true
        echo "[hang-watchdog] FAIL (hang detected)"
        exit 1
    fi
    sleep 5
done

wait "$child"
rc=$?
echo "[hang-watchdog] command exited rc=$rc"
exit "$rc"
