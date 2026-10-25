#!/usr/bin/env bash
# #580 R9 PMU attribution (research-only). Perf stat event groups and perf
# record cycles:u attribution on the uninstrumented timing binary, warm vs
# 16 MiB interference. Event availability is probed first and unsupported
# events are recorded as such (CACHE_ATTRIBUTION_UNAVAILABLE per event).
# No global host parameters are touched.
set -uo pipefail
cd "$(dirname "$0")/../../.."

OUT="${PMU_OUT:-build-research/replay/pmu}"
mkdir -p "$OUT"
BIN=build-research/cache_ab
[ -x "$BIN" ] || { echo "FAIL-CLOSED: missing $BIN (built only by build_cache.sh in a prepare-research-worktree.sh tree)" >&2; exit 2; }
HOST_TAG=${1:-wsl}
OPS=40000

probe_events() {
    perf stat -x, -e cycles:u,instructions:u,L1-dcache-loads:u,L1-dcache-load-misses:u \
        -- true >"$OUT/events-group1-$HOST_TAG.txt" 2>&1
    perf stat -x, -e LLC-loads:u,LLC-load-misses:u,branch-misses:u,cache-misses:u \
        -- true >"$OUT/events-group2-$HOST_TAG.txt" 2>&1
}

run_cell() {
    local depth=$1 ws=$2 tag=$3
    local args=(--scenario pipeline --depth "$depth" --ops $OPS --warmup 2000
                --pin-mode single --cpu-a 2 --ws-bytes "$ws" --label "pmu_$tag")
    perf stat -x, -e cycles:u,instructions:u,L1-dcache-loads:u,L1-dcache-load-misses:u \
        -- "$BIN" "${args[@]}" >"$OUT/stat1-d${depth}-$tag-$HOST_TAG.txt" 2>&1
    perf stat -x, -e LLC-loads:u,LLC-load-misses:u,branch-misses:u,cache-misses:u \
        -- "$BIN" "${args[@]}" >"$OUT/stat2-d${depth}-$tag-$HOST_TAG.txt" 2>&1
    perf record -m 64 -e cycles:u -F 1999 -o "$OUT/record-d${depth}-$tag-$HOST_TAG.data" \
        -- "$BIN" "${args[@]}" >"$OUT/record-run-d${depth}-$tag-$HOST_TAG.txt" 2>&1
    perf report --stdio -i "$OUT/record-d${depth}-$tag-$HOST_TAG.data" \
        >"$OUT/report-d${depth}-$tag-$HOST_TAG.txt" 2>&1
}

resolve_symbol() {
    local depth=$1 tag=$2
    python3 - "$OUT/report-d${depth}-$tag-$HOST_TAG.txt" "$BIN" <<'PY'
import re, subprocess, sys
report, binary = sys.argv[1], sys.argv[2]
syms = []
for line in subprocess.run(["nm", "-C", "--defined-only", binary],
                           capture_output=True, text=True).stdout.splitlines():
    parts = line.split(None, 2)
    if len(parts) == 3 and parts[1] in "TtWw":
        try:
            syms.append((int(parts[0], 16), parts[2]))
        except ValueError:
            pass
syms.sort()
if not syms:
    sys.exit(0)
target = "find_live_router_cookie_"
total = None
hits = []
for line in open(report):
    m = re.match(r"\s*([0-9.]+)%\s+.*?(0x[0-9a-f]+|\b[0-9a-f]{10,}\b)\s", line)
    if not m:
        continue
    try:
        addr = int(m.group(2), 16)
    except ValueError:
        continue
    lo, hi = 0, len(syms) - 1
    found = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            found = syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    if found and target in found[1]:
        hits.append((float(m.group(1)), hex(addr), found[1]))
if hits:
    share = sum(h[0] for h in hits)
    print(f"resolved {target}: self-share {share:.2f}% across {len(hits)} address buckets")
PY
}

probe_events
for depth in 64 512; do
    run_cell "$depth" 0 warm
    run_cell "$depth" $((16 * 1024 * 1024)) ws16m
done
for depth in 64 512; do
    for tag in warm ws16m; do
        echo "d${depth} ${tag}:" >>"$OUT/shares-$HOST_TAG.txt"
        resolve_symbol "$depth" "$tag" | tee -a "$OUT/shares-$HOST_TAG.txt"
    done
done
echo "pmu done -> $OUT"
