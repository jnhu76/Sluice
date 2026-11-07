#!/bin/bash
# r5-repair-measure: clean release builds of the three frozen arms.
# No measurement lock needed: no timing is in progress while this runs.
set -euo pipefail
R=/home/jnhu/sluice-595-e5/r5-repair-measure
gcc --version | head -1 > "$R/logs/toolchain.txt"
xmake --version | head -1 >> "$R/logs/toolchain.txt"
for w in worktree-original worktree-cleanup; do
  cd "$R/$w"
  xmake f -y -c -m release --liburing=y > "$R/logs/build-$w-config.log" 2>&1
  xmake build -y bench_w03_external_loop bench_w02_pipeline > "$R/logs/build-$w.log" 2>&1
  for b in bench_w03_external_loop bench_w02_pipeline; do
    sha256sum "build/linux/x86_64/release/$b" >> "$R/logs/binary-sha256.txt"
  done
  echo "DONE $w $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$R/logs/build-progress.log"
done
echo "ALL_CLEAN_BUILDS_DONE $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$R/logs/build-progress.log"
