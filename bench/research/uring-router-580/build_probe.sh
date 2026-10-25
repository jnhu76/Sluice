#!/usr/bin/env bash
# #580 R2 count-probe builder. Instrumented build for lookup frequencies and
# probe distributions ONLY - never used for a timing claim. Pass WE2=1 to also
# build the reversed-cancel-scan variant used for mechanism verification.
set -euo pipefail
cd "$(dirname "$0")/../../.."
CXXFLAGS=(-O3 -std=c++20 -DNDEBUG -DSLUICE_HAS_LIBURING -DSLUICE_ASYNC_INTERNAL_TESTING
          -DSLUICE_URING_LOOKUP_INSTRUMENT -Wall -Iinclude -Isrc/async)
SRCS=(bench/research/uring-router-580/lookup_probe.cpp
      src/async/async_io_context.cpp src/async/threadpool_backend.cpp src/async/fail_fast.cpp
      src/async/request_scope.cpp src/async/detail/context_identity.cpp
      src/async/detail/request_core.cpp src/async/uring_backend.cpp
      src/async/uring_completion.cpp src/async/uring_transport.cpp
      src/blocking_file.cpp src/file_resource.cpp)
g++ "${CXXFLAGS[@]}" "${SRCS[@]}" -luring -o build-research/lookup_probe
if [ "${WE2:-0}" = "1" ]; then
  g++ "${CXXFLAGS[@]}" -D SLUICE_580_WE2_REVERSE_CANCEL_SCAN "${SRCS[@]}" -luring \
      -o build-research/lookup_probe_we2
fi
sha256sum build-research/lookup_probe*
