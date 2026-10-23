#!/bin/bash
cd ~/Source/Sluice
export SLUICE_BENCH_TMP_DIR=/tmp/e5-fixture
export SLUICE_BENCH_CASE_FILTER="=W03-uring-4k-d4"
export SLUICE_BENCH_WARMUP=0
export SLUICE_BENCH_MEASURED=1
B=./build-e5/linux/x86_64/release/bench_w03_external_loop
: > /tmp/e5/capture_hang2.log
for attempt in $(seq 1 20); do
  rm -f /tmp/e5/capture-run.jsonl
  SLUICE_BENCH_JSONL=/tmp/e5/capture-run.jsonl timeout 90 taskset -c 2 $B >/tmp/e5/capture_last.out 2>&1 &
  tpid=$!
  sleep 12
  if kill -0 $tpid 2>/dev/null; then
    bpid=$(pgrep -P $tpid | head -1)
    echo "attempt=$attempt HUNG timeout_pid=$tpid bench_pid=$bpid" >> /tmp/e5/capture_hang2.log
    {
      echo "=== attempt=$attempt bench_pid=$bpid $(date -Is)"
      echo "--- bench threads"
      for t in /proc/$bpid/task/*; do
        tid=${t##*/}
        echo "TID=$tid comm=$(cat $t/comm 2>/dev/null) state=$(awk '{print $3}' $t/stat 2>/dev/null) wchan=$(cat $t/wchan 2>/dev/null)"
        echo "  syscall: $(cat $t/syscall 2>/dev/null)"
      done
      echo "--- fds"
      ls -l /proc/$bpid/fd 2>/dev/null | grep -E "eventfd|eventpoll|io_uring"
      for f in /proc/$bpid/fd/*; do
        tgt=$(readlink "$f" 2>/dev/null)
        case "$tgt" in
          *eventfd*|*io_uring*)
            echo "-- fd ${f##*/} -> $tgt"
            cat /proc/$bpid/fdinfo/${f##*/} 2>/dev/null
            ;;
        esac
      done
      echo "--- jsonl lines: $(wc -l < /tmp/e5/capture-run.jsonl 2>/dev/null)"
    } >> /tmp/e5/capture_hang2.log 2>&1
    kill -9 $tpid $bpid 2>/dev/null
    sleep 1
    echo CAPTURED >> /tmp/e5/capture_hang2.log
    break
  else
    wait $tpid; rc=$?
    echo "attempt=$attempt completed rc=$rc" >> /tmp/e5/capture_hang2.log
  fi
done
echo CAPTURE_DONE >> /tmp/e5/capture_hang2.log
