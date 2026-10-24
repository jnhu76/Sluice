#!/usr/bin/env bash
# #575 native-E5 paired interleaved A/B runner (frozen before measurement).
#
# Protocol (adapted from the frozen M2 run_ab.frozen.sh methodology):
#   - arms are pre-built Release benchmark binaries from isolated worktrees
#   - per repetition: config order rotates, arm order rotates (anti-drift)
#   - every invocation runs the product benchmark with one case selected,
#     WARMUP=1 MEASURED=1, wrapped in perf stat (kernel+user counters,
#     paranoid=1 owner override) and /usr/bin/time -v
#   - the benchmark's own byte oracle runs inside every repetition; a failed
#     oracle aborts the campaign
#
# Usage: run_575_ab.sh <out-dir> <reps> <campaign> [<stats>]
#   campaign: tp | ur | tp-stats | ur-stats | aa-tp | aa-ur | w01
#   stats:    0 (default) | 1
set -euo pipefail

OUT_DIR="${1:?usage: run_575_ab.sh <out-dir> <reps> <campaign> [stats]}"
REPS="${2:?reps}"
CAMPAIGN="${3:?campaign}"
STATS="${4:-0}"
WT=/tmp/sluice-575
FIXTURE_ROOT="${SLUICE_575_FIXTURE_DIR:-/dev/shm/sluice-575-fixtures}"

mkdir -p "$OUT_DIR" "$FIXTURE_ROOT"

case "$CAMPAIGN" in
  tp)
    ARMS=(base tp-oponly tp-full)
    CONFIGS=(
      "W02-threadpool-4k-d1-w1|w02|2"
      "W02-threadpool-4k-d4-w4|w02|2-5"
      "W02-threadpool-4k-d16-w8|w02|2-9"
      "W02-threadpool-4k-d16-w4|w02|2-5"
      "W02-threadpool-64k-d16-w4|w02|2-5"
      "W03-threadpool-4k-d16|w03|2-5"
    )
    ;;
  ur)
    ARMS=(base ur-oponly ur-full)
    CONFIGS=(
      "W02-uring-4k-d1|w02|2"
      "W02-uring-4k-d4|w02|2"
      "W02-uring-4k-d16|w02|2"
      "W02-uring-64k-d16|w02|2"
      "W03-uring-4k-d16|w03|2"
    )
    ;;
  aa-tp)
    ARMS=(base base2)
    CONFIGS=(
      "W02-threadpool-4k-d1-w1|w02|2"
      "W02-threadpool-4k-d16-w8|w02|2-9"
    )
    ;;
  aa-ur)
    ARMS=(base base2)
    CONFIGS=(
      "W02-uring-4k-d1|w02|2"
      "W02-uring-4k-d16|w02|2"
    )
    ;;
  tp-stats)
    ARMS=(base tp-oponly tp-full)
    CONFIGS=(
      "W02-threadpool-4k-d1-w1|w02|2"
      "W02-threadpool-4k-d4-w4|w02|2-5"
      "W02-threadpool-4k-d16-w8|w02|2-9"
    )
    ;;
  ur-stats)
    ARMS=(base ur-oponly ur-full)
    CONFIGS=(
      "W02-uring-4k-d1|w02|2"
      "W02-uring-4k-d4|w02|2"
      "W02-uring-4k-d16|w02|2"
    )
    ;;
  w01)
    ARMS=(base tp-oponly tp-full)
    CONFIGS=("W01-read_at-4k-seq|w01|2")
    ;;
  *) echo "unknown campaign $CAMPAIGN" >&2; exit 2 ;;
esac

# aa arms: base2 is the same base binary under a second label (A/A pair).
arm_bin() {
  case "$1" in
    base2) echo "$WT/base" ;;
    *) echo "$WT/$1" ;;
  esac
}

# W-01 case filter: bench_w01 selects cases by id prefix; read_at exists.
w01_case_id() { echo "W01-read_at"; }

JSONL="$OUT_DIR/ab.jsonl"
PERFCSV="$OUT_DIR/perf.csv"
TIMELOG="$OUT_DIR/time.txt"
META="$OUT_DIR/meta.jsonl"
: > "$JSONL"; : > "$PERFCSV"; : > "$META"

read_freq() {
  cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_cur_freq 2>/dev/null || echo 0
}

{
  echo "campaign=$CAMPAIGN"
  echo "reps=$REPS"
  echo "stats=$STATS"
  echo "date=$(date -Is)"
  echo "kernel=$(uname -r)"
  echo "compiler=$(g++ --version | head -1)"
  echo "cpu_model=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
  echo "governor=$(cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
  echo "perf_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid)"
  echo "loadavg_start=$(cut -d' ' -f1-3 /proc/loadavg)"
  for arm in "${ARMS[@]}"; do
    wt="$(arm_bin "$arm")"
    echo "arm=$arm worktree=$wt"
    echo "arm=$arm bench_w02_sha256=$(sha256sum "$wt/build/linux/x86_64/release/bench_w02_pipeline" | awk '{print $1}')"
    echo "arm=$arm bench_w03_sha256=$(sha256sum "$wt/build/linux/x86_64/release/bench_w03_external_loop" | awk '{print $1}')"
    echo "arm=$arm bench_w01_sha256=$(sha256sum "$wt/build/linux/x86_64/release/bench_w01_direct" | awk '{print $1}')"
    echo "arm=$arm lib_async_sha256=$(sha256sum "$wt/build/linux/x86_64/release/libsluice_async.a" | awk '{print $1}')"
  done
} | tee "$OUT_DIR/provenance.txt"

NCFG=${#CONFIGS[@]}
NARM=${#ARMS[@]}

for rep in $(seq 1 "$REPS"); do
  for ci in $(seq 0 $((NCFG - 1))); do
    cidx=$(( (ci + rep - 1) % NCFG ))
    IFS='|' read -r CASE BENCH PIN <<< "${CONFIGS[$cidx]}"
    for ai in $(seq 0 $((NARM - 1))); do
      aidx=$(( (ai + rep - 1) % NARM ))
      arm="${ARMS[$aidx]}"
      wt="$(arm_bin "$arm")"
      case "$BENCH" in
        w01) bin="$wt/build/linux/x86_64/release/bench_w01_direct" ;;
        w02) bin="$wt/build/linux/x86_64/release/bench_w02_pipeline" ;;
        w03) bin="$wt/build/linux/x86_64/release/bench_w03_external_loop" ;;
      esac
      tmpdir="$FIXTURE_ROOT/${arm}_${CASE}"
      mkdir -p "$tmpdir"
      benchjson="$tmpdir/last.jsonl"
      : > "$benchjson"
      f0=$(read_freq)
      taskset -c "$PIN" \
        /usr/bin/time -v -o "$tmpdir/time.txt" \
        perf stat -x, -e instructions,cycles,context-switches,cpu-migrations \
          -o "$tmpdir/perf.txt" -- \
        env SLUICE_BENCH_TMP_DIR="$tmpdir" SLUICE_BENCH_STATS="$STATS" \
            SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=1 SLUICE_BENCH_SKIP_DISK=1 \
            SLUICE_BENCH_CASE_FILTER="=$CASE" \
            SLUICE_BENCH_JSONL="$benchjson" \
          "$bin" > "$tmpdir/stdout.txt" 2> "$tmpdir/stderr.txt" || {
            echo "REP $rep CASE $CASE ARM $arm FAILED" >&2
            cat "$tmpdir/stderr.txt" >&2
            exit 1
          }
      f1=$(read_freq)
      # the benchmark oracle already aborts on failure (exit != 0 above);
      # keep the measured record, the time -v rusage, and perf counters
      python3 - "$benchjson" "$tmpdir/time.txt" "$tmpdir/perf.txt" \
               "$rep" "$CASE" "$arm" "$STATS" "$f0" "$f1" "$JSONL" "$bin" <<'PY'
import json, re, sys, hashlib
benchjson, timefile, perffile, rep, case, arm, stats, f0, f1, out, binpath = sys.argv[1:]
recs = [json.loads(l) for l in open(benchjson) if l.strip()]
meas = [r for r in recs if r.get('measured')]
assert meas, f"no measured record for rep={rep} case={case} arm={arm}"
assert all(r.get('ok') for r in meas), f"oracle failed rep={rep} case={case} arm={arm}"
r = meas[-1]
timetx = open(timefile).read()
def tval(pat):
    m = re.search(re.escape(pat) + r':\s*((?:\d+:)?[\d.]+)', timetx)
    if not m:
        return None
    parts = m.group(1).split(':')
    return float(parts[-1]) + (float(parts[0]) * 60 if len(parts) == 2 else 0.0)
perf = {}
for line in open(perffile):
    parts = line.rstrip('\n').split(',')
    if len(parts) >= 3 and parts[0] and not parts[0].startswith('#'):
        try: perf[parts[2]] = float(parts[0].replace(',', ''))
        except (ValueError, IndexError): pass
row = {
    'rep': int(rep), 'case': case, 'arm': arm, 'stats': int(stats),
    'freq_before': int(f0), 'freq_after': int(f1),
    'loadavg': open('/proc/loadavg').read().split()[:3],
    'ok': r.get('ok'), 'ops': r.get('ops'), 'bytes': r.get('bytes'),
    'wall_s': r.get('wall_s'), 'ops_per_sec': r.get('ops_per_sec'),
    'mib_per_sec': r.get('mib_per_sec'),
    'cpu_s': r.get('cpu_s'), 'latency_p50_ns': r.get('latency_p50_ns'),
    'latency_p99_ns': r.get('latency_p99_ns'),
    
    'time_wall_s': tval('Elapsed (wall clock) time (h:mm:ss or m:ss)'),
    'time_user_s': tval('User time (seconds)'),
    'time_sys_s': tval('System time (seconds)'),
    'max_rss_kb': tval('Maximum resident set size (kbytes)'),
    'vol_ctxt': tval('Voluntary context switches'),
    'invol_ctxt': tval('Involuntary context switches'),
    'instructions': perf.get('instructions'),
    'cycles': perf.get('cycles'),
    'ctx_switches': perf.get('context-switches'),
    'cpu_migrations': perf.get('cpu-migrations'),
    'bin_sha256': hashlib.sha256(open(binpath, 'rb').read()).hexdigest(),
}
with open(out, 'a') as f:
    f.write(json.dumps(row) + '\n')
PY
      rm -rf "$tmpdir"
    done
  done
  echo "rep $rep/$REPS done ($(date -Is))" >&2
done

echo "outputs in $OUT_DIR" >&2
