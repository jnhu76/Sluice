#!/usr/bin/env bash
# M1 mechanism experiment runner: provenance capture, pinned-CPU execution,
# decision pass, and per-arm perf-stat attribution passes.
#
# Standing duty: re-runs the admitted M0/M1 mechanism experiments in
# bench/mechanisms/ (consumers: docs/research/state-complexity/m1-results.md
# and any future owner-approved proposal validation). Success criteria: all
# three binaries exit 0, every negative control prints CAUGHT, no decision
# row prints ORACLE FAIL / SELFCHECK FAIL / oracle divergence, and outputs
# land machine-readable in <out-dir>. The perf section degrades gracefully
# when PMU access is restricted (perf_event_paranoid >= 2 restricts counters
# to user space; perf reports :u event suffixes).
#
# Usage: bench/mechanisms/run_mechanisms.sh <output-dir> [reps] [ops-scale]
set -euo pipefail

OUT_DIR="${1:?usage: run_mechanisms.sh <output-dir> [reps] [ops-scale]}"
REPS="${2:-9}"
OPS_SCALE="${3:-1}"
MECH_DIR="$(cd "$(dirname "$0")" && pwd)"
CPU="${SLUICE_M1_CPU:-2}"
PERF_EVENTS="${SLUICE_M1_PERF_EVENTS:-task-clock,cycles,instructions,cache-references,cache-misses,branch-misses}"

mkdir -p "$OUT_DIR"
cd "$MECH_DIR"

{
  echo "git_sha=$(git rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "git_dirty=$(git status --porcelain 2>/dev/null | wc -l)"
  echo "dirty_paths=$(git status --porcelain 2>/dev/null | awk '{print $2}' | tr '\n' ',')"
  echo "compiler=$(g++ --version | head -1)"
  echo "kernel=$(uname -r)"
  echo "cpu=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
  echo "pinned_cpu=$CPU"
  echo "cxx_flags=-std=c++20 -O2 -Wall -Wextra"
  echo "reps=$REPS ops_scale=$OPS_SCALE"
  echo "perf_events=$PERF_EVENTS"
} > "$OUT_DIR/provenance.txt"
cat "$OUT_DIR/provenance.txt"

CXX_FLAGS="-std=c++20 -O2 -Wall -Wextra"
g++ $CXX_FLAGS h1_router_index_bench.cpp -o "$OUT_DIR/h1"
g++ $CXX_FLAGS h2_owed_visibility_bench.cpp -o "$OUT_DIR/h2"
g++ $CXX_FLAGS h3_scope_slots_bench.cpp -o "$OUT_DIR/h3"

export SLUICE_M1_REPS="$REPS"
export SLUICE_M1_JSONL="$OUT_DIR/mechanisms.jsonl"
: > "$SLUICE_M1_JSONL"

echo "=== decision pass (interleaved arms where supported, wall ns/op) ==="
SLUICE_M1_OPS=$((200000 * OPS_SCALE)) taskset -c "$CPU" "$OUT_DIR/h1" | tee "$OUT_DIR/h1_decision.txt"
SLUICE_M1_PASSES=$((20000 * OPS_SCALE)) taskset -c "$CPU" "$OUT_DIR/h2" | tee "$OUT_DIR/h2_decision.txt"
SLUICE_M1_OPS=$((100000 * OPS_SCALE)) taskset -c "$CPU" "$OUT_DIR/h3" | tee "$OUT_DIR/h3_decision.txt"

grep -q "CAUGHT" "$OUT_DIR/h1_decision.txt"
grep -q "CAUGHT" "$OUT_DIR/h2_decision.txt"
grep -q "CAUGHT" "$OUT_DIR/h3_decision.txt"
! grep -q "ORACLE FAIL" "$OUT_DIR"/h*_decision.txt
! grep -q "SELFCHECK FAIL" "$OUT_DIR"/h*_decision.txt
! grep -q "oracle divergence" "$OUT_DIR"/h*_decision.txt

echo "=== per-arm perf-stat attribution (whole process; includes oracle + warmup) ==="
rm -f "$OUT_DIR/perf_stat.txt"
for solo in baseline_scan direct_index; do
  echo "solo_run=h1:$solo" >> "$OUT_DIR/perf_stat.txt"
  if ! SLUICE_M1_SOLO="$solo" SLUICE_M1_REPS=3 SLUICE_M1_OPS=200000 taskset -c "$CPU" \
    perf stat -e "$PERF_EVENTS" -x, -o "$OUT_DIR/perf_stat.txt" --append -- \
    "$OUT_DIR/h1" >> /dev/null; then
    echo "perf stat unavailable for h1:$solo (PMU restricted?)" >> "$OUT_DIR/perf_stat.txt"
  fi
done
for solo in baseline_single_lock_sweep baseline_per_slot_lock_sweep counter_snapshot_sweep; do
  echo "solo_run=h2:$solo" >> "$OUT_DIR/perf_stat.txt"
  if ! SLUICE_M1_SOLO="$solo" SLUICE_M1_REPS=3 SLUICE_M1_PASSES=20000 taskset -c "$CPU" \
    perf stat -e "$PERF_EVENTS" -x, -o "$OUT_DIR/perf_stat.txt" --append -- \
    "$OUT_DIR/h2" >> /dev/null; then
    echo "perf stat unavailable for h2:$solo (PMU restricted?)" >> "$OUT_DIR/perf_stat.txt"
  fi
done
for solo in baseline_first_free_scan freelist_lifo; do
  echo "solo_run=h3:$solo" >> "$OUT_DIR/perf_stat.txt"
  if ! SLUICE_M1_SOLO="$solo" SLUICE_M1_REPS=3 SLUICE_M1_OPS=100000 taskset -c "$CPU" \
    perf stat -e "$PERF_EVENTS" -x, -o "$OUT_DIR/perf_stat.txt" --append -- \
    "$OUT_DIR/h3" >> /dev/null; then
    echo "perf stat unavailable for h3:$solo (PMU restricted?)" >> "$OUT_DIR/perf_stat.txt"
  fi
done

echo "outputs in $OUT_DIR (mechanisms.jsonl, *_decision.txt, perf_stat.txt, provenance.txt)"
