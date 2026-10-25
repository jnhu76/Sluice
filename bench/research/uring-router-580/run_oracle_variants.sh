#!/usr/bin/env bash
# #580 R5 oracle builder/runner. Four variants: baseline, we2 (direction flip),
# false_miss (M7 family mutant), slot_only (M3/M6 family mutant).
#
# Phases:  --phase build  compile the four variants, fail on any build error
#          --phase run    execute the semantic oracle with RED/GREEN enforcement
#          --phase all    both (default)
#
# Enforcement: a GREEN variant must exit 0 and print ALL GREEN; the mutant
# must be killed by a NAMED oracle assertion (FAILED assertion: <name>) with a
# nonzero exit — a timeout, a build failure or an unrelated crash is not a
# kill. The run phase finishes with a restored-GREEN baseline rerun. Output
# goes to --out-dir (default build-research/replay), never over the archived
# evidence in docs/research.
set -euo pipefail
cd "$(dirname "$0")/../../.."

PHASE=all
OUT_DIR=build-research/replay/oracle
while [ $# -gt 0 ]; do
    case "$1" in
        --phase) PHASE="${2:?}"; shift 2 ;;
        --out-dir) OUT_DIR="${2:?}"; shift 2 ;;
        *) echo "unknown flag: $1" >&2; exit 64 ;;
    esac
done
case "$PHASE" in build|run|all) ;; *) echo "bad --phase" >&2; exit 64 ;; esac

TRANSPORT=src/async/uring_transport.cpp
for gate in SLUICE_580_WE2_REVERSE_CANCEL_SCAN SLUICE_580_MUTANT_FALSE_MISS \
            SLUICE_580_MUTANT_SLOT_ONLY; do
    grep -q "defined($gate)" "$TRANSPORT" || {
        echo "FAIL-CLOSED: research gate $gate absent from $TRANSPORT." >&2
        echo "This tree is not research-patched. Run bench/research/uring-router-580/prepare-research-worktree.sh" >&2
        exit 2
    }
done

CXXFLAGS=(-O3 -std=c++20 -DNDEBUG -DSLUICE_HAS_LIBURING -DSLUICE_ASYNC_INTERNAL_TESTING -Wall -Iinclude -Isrc/async)
SRCS=(bench/research/uring-router-580/identity_oracle.cpp
      src/async/async_io_context.cpp src/async/threadpool_backend.cpp src/async/fail_fast.cpp
      src/async/request_scope.cpp src/async/detail/context_identity.cpp
      src/async/detail/request_core.cpp src/async/uring_backend.cpp
      src/async/uring_completion.cpp src/async/uring_transport.cpp
      src/blocking_file.cpp src/file_resource.cpp)

mkdir -p build-research "$OUT_DIR"
build() { local v="$1"; shift; g++ "${CXXFLAGS[@]}" "${SRCS[@]}" "$@" -luring -o "build-research/oracle_$v"; }

if [ "$PHASE" = build ] || [ "$PHASE" = all ]; then
    build baseline
    build we2 -D SLUICE_580_WE2_REVERSE_CANCEL_SCAN
    build false_miss -D SLUICE_580_MUTANT_FALSE_MISS
    build slot_only -D SLUICE_580_MUTANT_SLOT_ONLY
    sha256sum build-research/oracle_baseline build-research/oracle_we2 \
              build-research/oracle_false_miss build-research/oracle_slot_only
fi
[ "$PHASE" = build ] && exit 0

TIMEOUT_S=${ORACLE_TIMEOUT_S:-120}
verdicts=()
run_variant() { # variant expected(GREEN|RED)
    local v="$1" expected="$2" rc=0
    local log="$OUT_DIR/oracle_$v.log"
    timeout "$TIMEOUT_S" "./build-research/oracle_$v" >"$log" 2>&1 || rc=$?
    local verdict=UNDECIDED detail=""
    if [ "$expected" = GREEN ]; then
        if [ "$rc" -eq 0 ] && tail -1 "$log" | grep -q '^ALL GREEN'; then
            verdict=GREEN; detail="exit=0 $(tail -1 "$log")"
        else
            verdict=WRONG; detail="expected GREEN, exit=$rc, last: $(tail -1 "$log")"
        fi
    else
        local named
        named=$(grep -c '^FAILED assertion: ' "$log" || true)
        if [ "$rc" -eq 124 ]; then
            verdict=WRONG; detail="timeout after ${TIMEOUT_S}s is not a kill"
        elif [ "$rc" -ne 0 ] && [ "$named" -gt 0 ]; then
            verdict=RED; detail="exit=$rc, $named named oracle assertion(s): $(grep -m1 '^FAILED assertion: ' "$log")"
        else
            verdict=WRONG; detail="expected RED by named assertion, exit=$rc, named=$named (survival or unrelated crash is not a kill)"
        fi
    fi
    verdicts+=("$v:$expected:$verdict")
    printf '%-12s expected=%-5s verdict=%-8s %s\n' "$v" "$expected" "$verdict" "$detail"
}

run_variant baseline   GREEN
run_variant we2        GREEN
run_variant false_miss RED
run_variant slot_only  GREEN
run_variant baseline   GREEN   # restored green after the mutant exposure

FAILED=0
for v in "${verdicts[@]}"; do
    [ "${v##*:}" = GREEN ] || [ "${v##*:}" = RED ] || FAILED=1
done
if [ "$FAILED" -ne 0 ]; then
    echo "ORACLE ENFORCEMENT FAILED" >&2
    exit 1
fi
echo "oracle replay: all variants met their expected semantics"
