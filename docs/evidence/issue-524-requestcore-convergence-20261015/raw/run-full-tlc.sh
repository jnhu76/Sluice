#!/usr/bin/env bash
# #524 R6 complete runs on the frozen model snapshot (MODEL_HEAD_SHA 28f0fe33).
# One TLC JVM at a time, persistent logs and metadir, exit codes recorded.
# Host (the "X99" box of the issue): hostname E5, 192.168.31.75,
# Intel Xeon E5-2666 v3, 10C/20T, single socket/NUMA, 62 GiB RAM.
set -u
root="/home/jnhu/Source/Sluice-524"
tla="$root/formal/tla"
jar="$tla/tla2tools.jar"
ev="$root/docs/evidence/issue-524-requestcore-convergence-20261015"
logs="$ev/raw/tlc"
meta="/home/jnhu/.cache/sluice/tla524"
workers="${WORKERS:-16}"
heap="${HEAP:-48g}"
mkdir -p "$logs" "$meta"

run() {
    local label="$1" cfg="$2"
    java "-Xmx$heap" -cp "$jar" tlc2.TLC -workers "$workers" -deadlock \
        -config "$tla/$cfg" -metadir "$meta/$label" "$tla/RequestCore.tla" \
        > "$logs/$label.log" 2>&1
    printf '%s\t%s\n' "$label" "$?" >> "$logs/exit-codes.tsv"
}

: > "$logs/exit-codes.tsv"
printf 'label\texit_code\n' >> "$logs/exit-codes.tsv"
echo "start $(date -Is) host=$(hostname) workers=$workers heap=$heap jar_sha=$(sha256sum "$jar" | cut -d' ' -f1)"
run safety RequestCore.cfg
run liveness RequestCoreLive.cfg
echo "done $(date -Is)"
cat "$logs/exit-codes.tsv"
