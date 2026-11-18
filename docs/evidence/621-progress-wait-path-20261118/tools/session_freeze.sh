#!/usr/bin/env bash
# Freeze CPU governor and irqbalance for a #621 measurement session.
# Usage: session_freeze.sh <pwfile>
# The password file is only ever passed to sudo through stdin.
set -uo pipefail

usage() {
    printf 'usage: %s <pwfile>\n' "$(basename "$0")" >&2
}

if [ "$#" -ne 1 ]; then
    usage
    exit 2
fi

PWFILE=$1
if [ ! -f "$PWFILE" ] || [ ! -r "$PWFILE" ]; then
    printf 'freeze: password file is not a readable regular file: %s\n' "$PWFILE" >&2
    exit 2
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd) || exit 1
SNAPSHOT_DIR="$SCRIPT_DIR/../p1"
SNAPSHOT="$SNAPSHOT_DIR/session-snapshot.txt"
mkdir -p "$SNAPSHOT_DIR" || {
    printf 'freeze: cannot create %s\n' "$SNAPSHOT_DIR" >&2
    exit 1
}

SUDO=(sudo -S -p '')

if ! "${SUDO[@]}" -v < "$PWFILE"; then
    printf 'freeze: sudo preflight failed; no changes made\n' >&2
    exit 1
fi

mapfile -t CPU_DIRS < <(
    for d in /sys/devices/system/cpu/cpu[0-9]*; do
        [ -e "$d/cpufreq/scaling_governor" ] && printf '%s\n' "$d"
    done | sort -V
)
if [ "${#CPU_DIRS[@]}" -eq 0 ]; then
    printf 'freeze: no cpufreq-capable CPUs found\n' >&2
    exit 1
fi

declare -a CPU_NAMES=()
declare -a GOV_ORIG=()
declare -a MIN_ORIG=()
declare -a MAX_ORIG=()
for d in "${CPU_DIRS[@]}"; do
    name=$(basename "$d")
    gov=$(cat "$d/cpufreq/scaling_governor" 2>/dev/null) || {
        printf 'freeze: cannot read %s/cpufreq/scaling_governor\n' "$d" >&2
        exit 1
    }
    min=$(cat "$d/cpufreq/scaling_min_freq" 2>/dev/null) || {
        printf 'freeze: cannot read %s/cpufreq/scaling_min_freq\n' "$d" >&2
        exit 1
    }
    max=$(cat "$d/cpufreq/scaling_max_freq" 2>/dev/null) || {
        printf 'freeze: cannot read %s/cpufreq/scaling_max_freq\n' "$d" >&2
        exit 1
    }
    CPU_NAMES+=("$name")
    GOV_ORIG+=("$gov")
    MIN_ORIG+=("$min")
    MAX_ORIG+=("$max")
done

IRQ_PRESENT=no
if systemctl cat irqbalance >/dev/null 2>&1; then
    IRQ_PRESENT=yes
fi
IRQ_ACTIVE=$(systemctl is-active irqbalance 2>/dev/null || true)
IRQ_ENABLED=$(systemctl is-enabled irqbalance 2>/dev/null || true)

if [ -f "$SNAPSHOT" ]; then
    prev_gov=$(awk -F'[ =]' '/^cpu[0-9]+ governor=/{print $3; exit}' "$SNAPSHOT")
    current_performance=yes
    for gov in "${GOV_ORIG[@]}"; do
        [ "$gov" = performance ] || current_performance=no
    done
    if [ "$prev_gov" != "performance" ] && [ "$current_performance" = yes ] \
        && [ "${SLUICE621_FREEZE_FORCE:-0}" != "1" ]; then
        printf 'freeze: a previous freeze appears active (snapshot records %s, CPUs are performance)\n' \
            "$prev_gov" >&2
        printf 'freeze: run session_restore.sh first, or set SLUICE621_FREEZE_FORCE=1\n' >&2
        exit 1
    fi
    preserved="$SNAPSHOT_DIR/session-snapshot.$(date +%Y%m%dT%H%M%S).prev.txt"
    if ! mv "$SNAPSHOT" "$preserved"; then
        printf 'freeze: cannot preserve existing snapshot as %s\n' "$preserved" >&2
        exit 1
    fi
    printf 'freeze: preserved previous snapshot at %s\n' "$preserved" >&2
fi

TMP_SNAPSHOT=$(mktemp "$SNAPSHOT_DIR/.session-snapshot.XXXXXX") || exit 1
cleanup_tmp() {
    rm -f "$TMP_SNAPSHOT"
}
trap cleanup_tmp EXIT

{
    printf '# sluice621 session snapshot v1\n'
    printf '# generated: %s\n' "$(date -Is)"
    printf 'cpu_count=%s\n' "${#CPU_DIRS[@]}"
    printf 'irqbalance_present=%s\n' "$IRQ_PRESENT"
    printf 'irqbalance_active=%s\n' "$IRQ_ACTIVE"
    printf 'irqbalance_enabled=%s\n' "$IRQ_ENABLED"
    for i in "${!CPU_DIRS[@]}"; do
        printf '%s governor=%s min=%s max=%s\n' \
            "${CPU_NAMES[$i]}" "${GOV_ORIG[$i]}" "${MIN_ORIG[$i]}" "${MAX_ORIG[$i]}"
    done
} > "$TMP_SNAPSHOT" || {
    printf 'freeze: cannot write snapshot\n' >&2
    exit 1
}
if ! mv "$TMP_SNAPSHOT" "$SNAPSHOT"; then
    printf 'freeze: cannot install snapshot at %s\n' "$SNAPSHOT" >&2
    exit 1
fi
trap - EXIT
rm -f "$TMP_SNAPSHOT"

FREEZE_DONE=0
ROLLING_BACK=0
ROLLBACK_SCRIPT=""

rollback() {
    local rc=0
    printf 'freeze: rolling back governor/irqbalance changes\n' >&2
    if [ -n "$ROLLBACK_SCRIPT" ]; then
        rm -f "$ROLLBACK_SCRIPT"
        ROLLBACK_SCRIPT=""
    fi
    ROLLBACK_SCRIPT=$(mktemp "${TMPDIR:-/tmp}/sluice621-rollback.XXXXXX") || return 1
    {
        printf '#!/bin/sh\nrc=0\n'
        for i in "${!CPU_DIRS[@]}"; do
            printf 'echo %s > %s/cpufreq/scaling_max_freq 2>/dev/null || rc=1\n' \
                "${MAX_ORIG[$i]}" "${CPU_DIRS[$i]}"
            printf 'echo %s > %s/cpufreq/scaling_min_freq 2>/dev/null || rc=1\n' \
                "${MIN_ORIG[$i]}" "${CPU_DIRS[$i]}"
            printf 'echo %s > %s/cpufreq/scaling_governor 2>/dev/null || rc=1\n' \
                "${GOV_ORIG[$i]}" "${CPU_DIRS[$i]}"
        done
        printf 'exit $rc\n'
    } > "$ROLLBACK_SCRIPT" || return 1
    chmod 700 "$ROLLBACK_SCRIPT" || return 1
    "${SUDO[@]}" sh "$ROLLBACK_SCRIPT" < "$PWFILE" || rc=1
    rm -f "$ROLLBACK_SCRIPT"
    ROLLBACK_SCRIPT=""
    if [ "$IRQ_PRESENT" = yes ]; then
        if [ "$IRQ_ACTIVE" = active ]; then
            "${SUDO[@]}" systemctl start irqbalance < "$PWFILE" || rc=1
        else
            "${SUDO[@]}" systemctl stop irqbalance < "$PWFILE" || rc=1
        fi
    fi
    return "$rc"
}

on_exit() {
    local rc=$?
    if [ "$FREEZE_DONE" -ne 1 ] && [ "$ROLLING_BACK" -ne 1 ]; then
        ROLLING_BACK=1
        trap - EXIT INT TERM HUP
        rollback || printf 'freeze: ROLLBACK FAILED; inspect governor/irqbalance manually\n' >&2
    fi
    exit "$rc"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

if ! "${SUDO[@]}" sh -c '
    rc=0
    for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo performance > "$f" 2>/dev/null || rc=1
    done
    exit $rc
' < "$PWFILE"; then
    printf 'freeze: setting performance governor failed\n' >&2
    exit 1
fi

for d in "${CPU_DIRS[@]}"; do
    gov=$(cat "$d/cpufreq/scaling_governor" 2>/dev/null) || {
        printf 'freeze: cannot read back %s\n' "$d" >&2
        exit 1
    }
    if [ "$gov" != performance ]; then
        printf 'freeze: readback mismatch at %s: %s\n' "$d" "$gov" >&2
        exit 1
    fi
done

if [ "$IRQ_PRESENT" = yes ]; then
    if ! "${SUDO[@]}" systemctl stop irqbalance < "$PWFILE"; then
        printf 'freeze: stopping irqbalance failed\n' >&2
        exit 1
    fi
    state=$(systemctl is-active irqbalance 2>/dev/null || true)
    if [ "$state" = active ]; then
        printf 'freeze: irqbalance still active after stop\n' >&2
        exit 1
    fi
fi

if ! {
    printf '\n[freeze_state]\n'
    printf 'governor_all_performance=yes\n'
    printf 'irqbalance_active_after_freeze=%s\n' "$(systemctl is-active irqbalance 2>/dev/null || true)"
    printf '\n[freq_snapshot]\n'
    for d in "${CPU_DIRS[@]}"; do
        printf '%s %s\n' "$(basename "$d")" \
            "$(cat "$d/cpufreq/scaling_cur_freq" 2>/dev/null || printf NA)"
    done
    printf '\n[thermal_snapshot]\n'
    for z in /sys/class/thermal/thermal_zone*; do
        [ -e "$z/temp" ] || continue
        printf '%s %s %s\n' "$(basename "$z")" \
            "$(cat "$z/type" 2>/dev/null || printf NA)" \
            "$(cat "$z/temp" 2>/dev/null || printf NA)"
    done
    printf '\n[ps_snapshot]\n'
    ps -eo pid,ppid,user,comm,%cpu,%mem --sort=-%cpu | head -n 40
    printf '\n[proc_stat]\n'
    cat /proc/stat
} >> "$SNAPSHOT"; then
    printf 'freeze: environment snapshot append failed\n' >&2
    exit 1
fi

FREEZE_DONE=1
trap - EXIT INT TERM HUP
printf 'freeze: %s CPUs set to performance, irqbalance=%s, snapshot=%s\n' \
    "${#CPU_DIRS[@]}" "$(systemctl is-active irqbalance 2>/dev/null || true)" "$SNAPSHOT"
exit 0
