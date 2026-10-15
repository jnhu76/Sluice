#!/usr/bin/env bash
# #524 formal liveness run (R6, authorised 2026-10-15 by the owner).
#
# Runs RequestCoreLive.cfg on a frozen model snapshot, not on a working tree
# that can move under the run: the model, the config and the TLC jar are copied
# into the snapshot directory first and their SHA-256 values are checked against
# the frozen ones recorded in resource-and-execution-manifest.md. One TLC JVM at
# a time; persistent log and metadir; the exit code is recorded.
#
# Host (the "X99" box of the issue): E5 / 192.168.31.75, Intel Xeon E5-2666 v3,
# 10C/20T, single socket/NUMA, 62 GiB RAM, ~393 GiB free on /home. The worker
# and heap values follow that topology, as the issue requires, not its planning
# values: 16 of 20 logical CPUs and 48 of 62 GiB, leaving headroom for the OS
# and the disk-backed fingerprint set (MSBDiskFPSet).
set -u

root="/home/jnhu/Source/Sluice-524"
tla="$root/formal/tla"
snap="${SNAP:-/home/jnhu/.cache/sluice/tla524/frozen-28f0fe33}"
meta="${METADIR:-/home/jnhu/.cache/sluice/tla524/liveness-formal}"
logdir="${LOGDIR:-/home/jnhu/.cache/sluice/tla524/logs}"
workers="${WORKERS:-16}"
heap="${HEAP:-48g}"

model_sha="4ad6dbe5cd4af19e12a846f06818622e099a37899474206fdd6984e445725aa1"
cfg_sha="062c8c3e6b873a5bfd518856952ceacd942dab2f08bf9e6145c34be4f771f09d"
safety_cfg_sha="82c31d615fdeb73d947342b2a8990072c738069cfa2e1363455a53314f7f8edf"
jar_sha="936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"

mkdir -p "$snap" "$meta" "$logdir"

if ps -eo args --no-headers | grep -q "[t]lc2.TLC"; then
    echo "refusing to start: a TLC process is already running" >&2
    exit 2
fi

cp "$tla/RequestCore.tla" "$tla/RequestCoreLive.cfg" "$tla/RequestCore.cfg" \
   "$tla/tla2tools.jar" "$snap/"

check() {
    local file="$1" want="$2" got
    got="$(sha256sum "$snap/$file" | cut -d' ' -f1)"
    if [[ "$got" != "$want" ]]; then
        echo "snapshot hash mismatch for $file: $got (want $want)" >&2
        exit 2
    fi
}

check RequestCore.tla "$model_sha"
check RequestCoreLive.cfg "$cfg_sha"
check RequestCore.cfg "$safety_cfg_sha"
check tla2tools.jar "$jar_sha"

log="$logdir/liveness-formal.log"
manifest="$logdir/liveness-formal-run.tsv"
: > "$log"

printf 'label\tstart\tend\texit_code\tworkers\theap\thost\tpid\tmetadir\tmodel_sha\tcfg_sha\tjar_sha\n' > "$manifest"
echo "snapshot $snap verified (model $model_sha, cfg $cfg_sha, jar $jar_sha)"
echo "log $log"
echo "metadir $meta"
echo "workers $workers heap $heap"
free -g | sed -n '2p'

start="$(date -Is)"
java "-Xmx$heap" -cp "$snap/tla2tools.jar" tlc2.TLC -workers "$workers" \
    -deadlock -config "$snap/RequestCoreLive.cfg" -metadir "$meta" \
    "$snap/RequestCore.tla" > "$log" 2>&1 &
pid=$!
echo "pid $pid start $start"
wait "$pid"
rc=$?
end="$(date -Is)"
printf 'liveness\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$start" "$end" "$rc" "$workers" "$heap" "$(hostname)" "$pid" "$meta" \
    "$model_sha" "$cfg_sha" "$jar_sha" >> "$manifest"
echo "end $end exit_code $rc"
exit "$rc"
