#!/usr/bin/env bash
# #580 R5 oracle builder/runner. Four variants: baseline, we2 (direction flip),
# false_miss (M7 family mutant), slot_only (M3/M6 family mutant).
set -uo pipefail
cd "$(dirname "$0")/../../.."
CXXFLAGS=(-O3 -std=c++20 -DNDEBUG -DSLUICE_HAS_LIBURING -DSLUICE_ASYNC_INTERNAL_TESTING -Wall -Iinclude -Isrc/async)
SRCS=(bench/research/uring-router-580/identity_oracle.cpp
      src/async/async_io_context.cpp src/async/threadpool_backend.cpp src/async/fail_fast.cpp
      src/async/request_scope.cpp src/async/detail/context_identity.cpp
      src/async/detail/request_core.cpp src/async/uring_backend.cpp
      src/async/uring_completion.cpp src/async/uring_transport.cpp
      src/blocking_file.cpp src/file_resource.cpp)
build() { local v="$1"; shift; g++ "${CXXFLAGS[@]}" "${SRCS[@]}" "$@" -luring -o "build-research/oracle_$v"; }
build baseline
build we2 -D SLUICE_580_WE2_REVERSE_CANCEL_SCAN
build false_miss -D SLUICE_580_MUTANT_FALSE_MISS
build slot_only -D SLUICE_580_MUTANT_SLOT_ONLY
for v in baseline we2 false_miss slot_only; do
  echo "=== oracle_$v ==="
  ./build-research/oracle_$v > "docs/research/uring-router-580/results/oracle_$v.log" 2>&1
  echo "exit=$? $(tail -1 docs/research/uring-router-580/results/oracle_$v.log)"
done
