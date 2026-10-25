#!/usr/bin/env bash
# #580 R9 cache-locality builder (research-only). Produces:
#   cache_ab          uninstrumented timing binary (production-shaped flags)
#   cache_ab_aa_*     byte-identical copies for the A/A path-name control
#   cache_ab_flush    + SLUICE_580_CLFLUSH_ROUTER (mechanism sensitivity only)
#   cache_probe       + SLUICE_URING_LOOKUP_INSTRUMENT (counts only, never timing)
set -euo pipefail

require_gate() {
    grep -q "defined($1)" "$2" || {
        echo "FAIL-CLOSED: research gate $1 absent from $2." >&2
        echo "Run bench/research/uring-router-580/prepare-research-worktree.sh for a patched isolated tree." >&2
        exit 2
    }
}
require_gate SLUICE_580_CLFLUSH_ROUTER src/async/uring_transport.cpp
require_gate SLUICE_URING_LOOKUP_INSTRUMENT src/async/uring_internal.hpp
require_gate SLUICE_URING_LOOKUP_INSTRUMENT src/async/uring_transport.cpp
require_gate SLUICE_URING_LOOKUP_INSTRUMENT src/async/uring_completion.cpp
mkdir -p build-research
cd "$(dirname "$0")/../../.."

TIMING_FLAGS=(-O3 -std=c++20 -DNDEBUG -DSLUICE_HAS_LIBURING -Wall -Iinclude)
SRCS=(bench/research/uring-router-580/cache_router.cpp
      src/async/async_io_context.cpp src/async/threadpool_backend.cpp
      src/async/fail_fast.cpp src/async/request_scope.cpp
      src/async/detail/context_identity.cpp src/async/detail/request_core.cpp
      src/async/uring_backend.cpp src/async/uring_completion.cpp
      src/async/uring_transport.cpp src/blocking_file.cpp src/file_resource.cpp)

g++ "${TIMING_FLAGS[@]}" "${SRCS[@]}" -luring -o build-research/cache_ab
g++ "${TIMING_FLAGS[@]}" -D SLUICE_580_CLFLUSH_ROUTER "${SRCS[@]}" -luring \
    -o build-research/cache_ab_flush
cp build-research/cache_ab build-research/cache_ab_aa_alpha
cp build-research/cache_ab build-research/cache_ab_aa_beta

g++ -O3 -std=c++20 -DNDEBUG -DSLUICE_HAS_LIBURING -DSLUICE_ASYNC_INTERNAL_TESTING \
    -DSLUICE_URING_LOOKUP_INSTRUMENT -Wall -Iinclude -Isrc/async \
    "${SRCS[@]}" -luring -o build-research/cache_probe

sha256sum build-research/cache_ab build-research/cache_ab_aa_alpha \
          build-research/cache_ab_aa_beta build-research/cache_ab_flush \
          build-research/cache_probe
