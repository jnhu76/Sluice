#!/usr/bin/env bash
set -euo pipefail
arm="$1"
cd "/tmp/dp0/arms/$arm"
xmake f -m release -o build-ring --liburing=y -y >/dev/null 2>&1
xmake -y >/dev/null 2>&1
xmake build -g bench -y >/dev/null 2>&1
for t in bench_w01_direct bench_w02_pipeline bench_w03_external_loop; do
  b="build-ring/linux/x86_64/release/$t"
  printf '%s %s sha256=%s\n' "$arm" "$t" "$(sha256sum "$b" | cut -d' ' -f1)"
done
