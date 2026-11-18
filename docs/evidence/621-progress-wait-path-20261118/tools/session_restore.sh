#!/usr/bin/env bash
# Restore CPU governor/min/max and irqbalance from the #621 session snapshot.
# Usage: session_restore.sh <pwfile>
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
    printf 'restore: password file is not a readable regular file: %s\n' "$PWFILE" >&2
    exit 2
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd) || exit 1
SNAPSHOT="$SCRIPT_DIR/../p1/session-snapshot.txt"
if [ ! -f "$SNAPSHOT" ]; then
    printf 'restore: no snapshot at %s\n' "$SNAPSHOT" >&2
    exit 1
fi

declare -a CPU_NAMES=()
declare -a GOV_SAVED=()
declare -a MIN_SAVED=()
declare -a MAX_SAVED=()
IRQ_PRESENT=no
IRQ_ACTIVE=unknown
IRQ_PRESENT_SEEN=0
IRQ_ACTIVE_SEEN=0
SNAPSHOT_CPU_COUNT=""

while IFS= read -r line; do
    if [[ $line =~ ^(cpu[0-9]+)\ governor=([^ ]+)\ min=([0-9]+)\ max=([0-9]+)$ ]]; then
        CPU_NAMES+=("${BASH_REMATCH[1]}")
        GOV_SAVED+=("${BASH_REMATCH[2]}")
        MIN_SAVED+=("${BASH_REMATCH[3]}")
        MAX_SAVED+=("${BASH_REMATCH[4]}")
    elif [[ $line =~ ^irqbalance_present=(.*)$ ]]; then
        if [ "$IRQ_PRESENT_SEEN" -eq 0 ]; then
            IRQ_PRESENT="${BASH_REMATCH[1]}"
            IRQ_PRESENT_SEEN=1
        fi
    elif [[ $line =~ ^irqbalance_active=(.*)$ ]]; then
        if [ "$IRQ_ACTIVE_SEEN" -eq 0 ]; then
            IRQ_ACTIVE="${BASH_REMATCH[1]}"
            IRQ_ACTIVE_SEEN=1
        fi
    elif [[ $line =~ ^cpu_count=(.*)$ ]]; then
        if [ -z "$SNAPSHOT_CPU_COUNT" ]; then
            SNAPSHOT_CPU_COUNT="${BASH_REMATCH[1]}"
        fi
    fi
done < "$SNAPSHOT"

if [ "${#CPU_NAMES[@]}" -eq 0 ]; then
    printf 'restore: snapshot contains no CPU governor entries\n' >&2
    exit 1
fi
if [ -n "$SNAPSHOT_CPU_COUNT" ] && [ "$SNAPSHOT_CPU_COUNT" != "${#CPU_NAMES[@]}" ]; then
    printf 'restore: snapshot cpu_count=%s but parsed %s CPU lines\n' \
        "$SNAPSHOT_CPU_COUNT" "${#CPU_NAMES[@]}" >&2
fi

SUDO=(sudo -S -p '')
if ! "${SUDO[@]}" -v < "$PWFILE"; then
    printf 'restore: sudo preflight failed; no changes made\n' >&2
    exit 1
fi

RESTORE_SCRIPT=$(mktemp "${TMPDIR:-/tmp}/sluice621-restore.XXXXXX") || exit 1
cleanup() {
    rm -f "$RESTORE_SCRIPT"
}
trap cleanup EXIT

{
    printf '#!/bin/sh\nrc=0\n'
    for i in "${!CPU_NAMES[@]}"; do
        root="/sys/devices/system/cpu/${CPU_NAMES[$i]}/cpufreq"
        printf 'echo %s > %s/scaling_max_freq 2>/dev/null || rc=1\n' \
            "${MAX_SAVED[$i]}" "$root"
        printf 'echo %s > %s/scaling_min_freq 2>/dev/null || rc=1\n' \
            "${MIN_SAVED[$i]}" "$root"
        printf 'echo %s > %s/scaling_governor 2>/dev/null || rc=1\n' \
            "${GOV_SAVED[$i]}" "$root"
    done
    printf 'exit $rc\n'
} > "$RESTORE_SCRIPT" || {
    printf 'restore: cannot write helper script\n' >&2
    exit 1
}
chmod 700 "$RESTORE_SCRIPT" || exit 1

RC=0
"${SUDO[@]}" sh "$RESTORE_SCRIPT" < "$PWFILE" || RC=1
rm -f "$RESTORE_SCRIPT"
trap - EXIT

if [ "$IRQ_PRESENT" = yes ]; then
    if ! systemctl cat irqbalance >/dev/null 2>&1; then
        printf 'restore: snapshot expected irqbalance, but the unit is absent\n' >&2
        RC=1
    elif [ "$IRQ_ACTIVE" = active ]; then
        "${SUDO[@]}" systemctl start irqbalance < "$PWFILE" || RC=1
    else
        "${SUDO[@]}" systemctl stop irqbalance < "$PWFILE" || RC=1
    fi
fi

MISMATCH=0
for i in "${!CPU_NAMES[@]}"; do
    root="/sys/devices/system/cpu/${CPU_NAMES[$i]}/cpufreq"
    gov=$(cat "$root/scaling_governor" 2>/dev/null || printf 'READ_ERROR')
    min=$(cat "$root/scaling_min_freq" 2>/dev/null || printf 'READ_ERROR')
    max=$(cat "$root/scaling_max_freq" 2>/dev/null || printf 'READ_ERROR')
    printf '%s governor=%s min=%s max=%s (snapshot governor=%s min=%s max=%s)\n' \
        "${CPU_NAMES[$i]}" "$gov" "$min" "$max" \
        "${GOV_SAVED[$i]}" "${MIN_SAVED[$i]}" "${MAX_SAVED[$i]}"
    if [ "$gov" != "${GOV_SAVED[$i]}" ] || [ "$min" != "${MIN_SAVED[$i]}" ] \
        || [ "$max" != "${MAX_SAVED[$i]}" ]; then
        MISMATCH=$((MISMATCH + 1))
    fi
done

IRQ_NOW=$(systemctl is-active irqbalance 2>/dev/null || true)
printf 'irqbalance active=%s (snapshot %s, present=%s)\n' \
    "$IRQ_NOW" "$IRQ_ACTIVE" "$IRQ_PRESENT"
if [ "$IRQ_PRESENT" = yes ]; then
    if [ "$IRQ_ACTIVE" = active ] && [ "$IRQ_NOW" != active ]; then
        MISMATCH=$((MISMATCH + 1))
    fi
    if [ "$IRQ_ACTIVE" != active ] && [ "$IRQ_NOW" = active ]; then
        MISMATCH=$((MISMATCH + 1))
    fi
fi

if [ "$RC" -ne 0 ] || [ "$MISMATCH" -ne 0 ]; then
    printf 'restore: FAILED (write_rc=%s mismatches=%s)\n' "$RC" "$MISMATCH" >&2
    exit 1
fi

printf 'restore: verified %s CPUs and irqbalance=%s\n' "${#CPU_NAMES[@]}" "$IRQ_NOW"
exit 0
