#!/bin/bash
# r5-repair-measure F5: three-state functional equivalence battery for
# original (f3b25329) vs cleanup (b191bb89) round builds.
# Functional/counting runs only - no timed measurement, no lock required.
set -uo pipefail
R=/home/jnhu/sluice-595-e5/r5-repair-measure
OUT=$R/raw/equiv
ORACLE_SRC=/home/jnhu/sluice-595-e5/r4/evidence/correctness/uring_ready_reuse_correctness_test.cpp
C2DIFF=/home/jnhu/sluice-595-e5/r5/raw-diag/equiv/c2/harness_c2name_adaptation.diff
SUITES="r4_arm_smoke uring_ready_batch_deterministic_test uring_ready_batch_research_test uring_ready_batch_fault_test uring_progress_race_test uring_external_loop_test uring_request_core_integration_test shutdown_lifecycle_test shutdown_lifecycle_uring_test submit_concurrency_uring_test"
SCENARIOS="empty_span single_read span_shapes residue_after_cap prefilled_residue partial_retry middle_rejection ineligible_mix capacity control_between poison_between concurrent overflow borrow state_aware empty_pipeline shutdown_between publish_once"

mkdir -p "$OUT"/{original,cleanup}

# 1. build test targets
for w in worktree-original worktree-cleanup; do
  (cd "$R/$w" && xmake build -y $SUITES > "$R/logs/eq-build-$w.log" 2>&1) || { echo "BUILD_FAIL $w"; exit 1; }
done
echo "builds ok"

# 2. in-repo suites
for st in original cleanup; do
  w=$R/worktree-$st
  for t in $SUITES; do
    bin=$w/build/linux/x86_64/release/$t
    if [ -x "$bin" ]; then
      "$bin" > "$OUT/$st/$t.out" 2>&1
      echo "$? $st $t" >> "$OUT/suite-rc.txt"
    else
      echo "MISSING $st $t" >> "$OUT/suite-rc.txt"
    fi
  done
done

# 3. oracle: compile per tree (cleanup uses the archived source + the frozen
# C2 mechanical-rename adaptation diff, same as R5; the diff is ed-format)
EQ=/tmp/r5rm-eq; mkdir -p $EQ/original $EQ/cleanup
cp "$ORACLE_SRC" $EQ/original/oracle.cpp
python3 - "$ORACLE_SRC" "$C2DIFF" "$EQ/cleanup/oracle.cpp" <<'PYEOF'
import sys
from pathlib import Path
src = Path(sys.argv[1]).read_text().splitlines(keepends=True)
diff = Path(sys.argv[2]).read_text().splitlines()
def rng(s):
    if ',' in s: x, y = s.split(','); return int(x), int(y)
    v = int(s); return v, v
i = 0; hunks = 0
while i < len(diff):
    line = diff[i]
    if line and line[0].isdigit() and 'c' in line:
        a_s, b_s = line.split('c')
        a0, a1 = rng(a_s); b0, b1 = rng(b_s)
        i += 1
        old = []
        while diff[i].startswith('< '): old.append(diff[i][2:]); i += 1
        assert diff[i] == '---'; i += 1
        new = []
        while i < len(diff) and diff[i].startswith('> '): new.append(diff[i][2:]); i += 1
        assert len(old) == a1 - a0 + 1 and len(new) == b1 - b0 + 1
        for k, o in enumerate(old):
            assert src[a0-1+k] == o + '\n', f"mismatch at line {a0+k}"
        src[a0-1:a0-1+len(old)] = [n + '\n' for n in new]
        hunks += 1
    else:
        i += 1
Path(sys.argv[3]).write_text(''.join(src))
assert not any(x in ''.join(src) for x in ('pressure_known', 'pressure_read', 'reads_pressure'))
print(f"adapted {hunks} hunks")
PYEOF
[ $? -eq 0 ] || { echo "C2_ADAPT_FAIL"; exit 1; }
printf '#define SLICE_ASYNC_INTERNAL_TESTING 1\n#define SLUICE_HAS_LIBURING 1\n#define NDEBUG 1\n' > /tmp/r5rm-eq/defs.h
for st in original cleanup; do
  w=$R/worktree-$st
  cat > $EQ/$st/oracle_main.cpp <<'WEOF'
#define SLUICE_HAS_LIBURING 1
#define SLUICE_ASYNC_INTERNAL_TESTING 1
#define NDEBUG 1
#include "oracle.cpp"
WEOF
  mkdir -p $EQ/$st/tu
  for tu in async_io_context uring_transport uring_completion uring_backend fail_fast; do
    printf '#define SLUICE_HAS_LIBURING 1\n#define SLUICE_ASYNC_INTERNAL_TESTING 1\n#define NDEBUG 1\n#include "%s/src/async/%s.cpp"\n' "$w" "$tu" > $EQ/$st/tu/$tu.cpp
  done
  mkdir -p $EQ/$st/tu/detail
  for tu in context_identity request_core; do
    printf '#define SLUICE_HAS_LIBURING 1\n#define SLUICE_ASYNC_INTERNAL_TESTING 1\n#define NDEBUG 1\n#include "%s/src/async/detail/%s.cpp"\n' "$w" "$tu" > $EQ/$st/tu/detail/$tu.cpp
  done
  g++ -std=c++20 -Wall -Werror -O3 \
    -I "$w/include" -I "$w/src/async" -I "$w/bench" \
    -o $EQ/$st/oracle $EQ/$st/oracle_main.cpp \
    $EQ/$st/tu/async_io_context.cpp \
    $EQ/$st/tu/uring_transport.cpp \
    $EQ/$st/tu/uring_completion.cpp \
    $EQ/$st/tu/uring_backend.cpp \
    $EQ/$st/tu/fail_fast.cpp \
    $EQ/$st/tu/detail/context_identity.cpp \
    $EQ/$st/tu/detail/request_core.cpp \
    "$w/build/linux/x86_64/release/libsluice_core.a" -luring -lpthread \
    > "$OUT/$st/oracle-build.log" 2>&1 || { echo "ORACLE_BUILD_FAIL $st"; head -20 "$OUT/$st/oracle-build.log"; exit 1; }
  $EQ/$st/oracle > "$OUT/$st/ready_reuse_oracle.out" 2>&1
  echo "$? full" >> "$OUT/$st/ready_reuse_oracle_percscenario.rc"
  : > "$OUT/$st/ready_reuse_oracle_percscenario.rc"
  for s in $SCENARIOS; do
    $EQ/$st/oracle --only=$s > "$OUT/$st/scenario-$s.out" 2>&1
    echo "$s rc=$?" >> "$OUT/$st/ready_reuse_oracle_percscenario.rc"
  done
done

# 4. W03 real-kernel smoke per arm (A0 and R on both trees; C == cleanup's R)
for st in original cleanup; do
  w=$R/worktree-$st
  for arm in A0 R; do
    rm -f /tmp/r5rm-eq-smoke.jsonl
    SLUICE_595_R3_ARM=$arm SLUICE_595_R3_CASES=1 SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 \
      SLUICE_BENCH_CASE_FILTER="=W03-uring-4k-d16-r3d16cap" SLUICE_BENCH_STATS=0 \
      SLUICE_BENCH_JSONL=/tmp/r5rm-eq-smoke.jsonl \
      taskset -c 2 "$w/build/linux/x86_64/release/bench_w03_external_loop" \
      > "$OUT/$st/w03_r3d16cap_$arm.out" 2>&1
    echo "$? $st $arm" >> "$OUT/smoke-rc.txt"
  done
done

echo BATTERY_DONE
