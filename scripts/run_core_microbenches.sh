#!/usr/bin/env bash
# run_core_microbenches.sh — builds (release) and runs all core microbenchmarks,
# concatenating their CSV streams into one file. See
# docs/core-microbench-methodology.md. Results are local, workload-specific
# observations, NOT portable performance claims.
set -euo pipefail

MODE="${1:-release}"
OUT="${2:-core_microbench_results.csv}"

echo "Building benches (mode=$MODE)..." >&2
xmake f -m "$MODE" >&2
xmake build -g bench >&2

RUN_DIR="build/$(uname -s | tr '[:upper:]' '[:lower:]')/$(uname -m)/${MODE}"
if [ ! -x "${RUN_DIR}/small_writes_bench" ]; then
  echo "error: expected bench binaries at ${RUN_DIR}/ (not found)" >&2
  exit 1
fi

{
  echo "# core microbench results (mode=$MODE, host=$(uname -srm), date=$(date -u +%FT%TZ))" >&2
  : > "$OUT"
  first=1
  for b in small_writes_bench copy_strategy_bench wal_write_bench sync_smoke_bench; do
    echo "Running $b..." >&2
    if [ "$first" -eq 1 ]; then
      "$RUN_DIR/$b" >> "$OUT" 2> >(cat >&2)
      first=0
    else
      "$RUN_DIR/$b" | tail -n +2 >> "$OUT" 2> >(cat >&2)
    fi
  done
}

echo "Wrote $OUT" >&2
wc -l "$OUT" >&2
