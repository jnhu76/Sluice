#!/usr/bin/env bash
# governor three-column evidence, unique filenames per round (measurement-audit §2-8 lesson).
# usage: governor.sh save|set|restore <round-tag>
set -euo pipefail
GOV=/home/jnhu/sluice-e5-elim/results/e5m28-s1-20261118/governor
mkdir -p "$GOV"
TAG="$2"
NCPUS=20

save() {
    : > "$GOV/original-$TAG.txt"
    for c in $(seq 0 $((NCPUS-1))); do
        printf 'cpu%s %s\n' "$c" "$(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor)" >> "$GOV/original-$TAG.txt"
    done
    echo "saved $(wc -l < "$GOV/original-$TAG.txt") cpus"
}

setperf() {
    printf '%s\n' 121212 | sudo -S -p '' sh -c '
        for c in $(seq 0 19); do
            echo performance > /sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor
        done
    ' >/dev/null
    : > "$GOV/set-verify-$TAG.txt"
    for c in $(seq 0 $((NCPUS-1))); do
        printf 'cpu%s %s\n' "$c" "$(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor)" >> "$GOV/set-verify-$TAG.txt"
    done
    n=$(grep -c performance "$GOV/set-verify-$TAG.txt" || true)
    echo "set-verify performance: $n/$NCPUS"
}

restore() {
    printf '%s\n' 121212 | sudo -S -p '' sh -c '
        while read -r line; do
            c=${line%% *}
            g=${line#* }
            echo "$g" > /sys/devices/system/cpu/$c/cpufreq/scaling_governor
        done < '"$GOV/original-$TAG.txt"'
    ' >/dev/null
    : > "$GOV/restore-verify-$TAG.txt"
    for c in $(seq 0 $((NCPUS-1))); do
        printf 'cpu%s %s\n' "$c" "$(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor)" >> "$GOV/restore-verify-$TAG.txt"
    done
    if diff -q "$GOV/original-$TAG.txt" "$GOV/restore-verify-$TAG.txt" >/dev/null; then
        echo "restore match=yes"
    else
        echo "restore match=NO"
        diff "$GOV/original-$TAG.txt" "$GOV/restore-verify-$TAG.txt" || true
    fi
}

case "$1" in
    save) save ;;
    set) setperf ;;
    restore) restore ;;
    *) echo "usage: governor.sh save|set|restore <tag>"; exit 2 ;;
esac
