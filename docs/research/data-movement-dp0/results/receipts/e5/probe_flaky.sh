#!/bin/bash
# W-03 uring pinning reproducibility probe: is the taskset hang flaky or deterministic?
cd ~/Source/Sluice
export SLUICE_BENCH_TMP_DIR=/tmp/e5-fixture
export SLUICE_BENCH_CASE_FILTER="=W03-uring-4k-d4"
export SLUICE_BENCH_WARMUP=0
export SLUICE_BENCH_MEASURED=1
export SLUICE_BENCH_JSONL=/tmp/e5/probe-flaky.jsonl
: > /tmp/e5/probe_flaky.log
B=./build-e5/linux/x86_64/release/bench_w03_external_loop
for cfg in "2" "3" "2,3"; do
  for i in 1 2 3 4 5 6 7 8; do
    rm -f /tmp/e5/probe-flaky.jsonl
    timeout 45 taskset -c "$cfg" $B >/tmp/e5/probe_flaky_last.out 2>&1
    rc=$?
    lines=$(wc -l < /tmp/e5/probe-flaky.jsonl 2>/dev/null || echo 0)
    echo "cfg=cpu${cfg} trial=${i} rc=${rc} jsonl_lines=${lines}" >> /tmp/e5/probe_flaky.log
  done
done
echo PROBE_FLAKY_DONE >> /tmp/e5/probe_flaky.log
