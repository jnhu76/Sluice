#!/usr/bin/env bash
# #580 R6 arm builder (WSL2 research-only). Builds the production-shaped A/B
# runner arms from the same sources with arm-fixed compile-time selection.
#   baseline: unmodified production lookup code paths
#   we2:      find_live_router_index_ scan direction reversed (EXPLORATORY_ONLY)
# The runner labels its output with the arm string; binaries are hashed and the
# disassembly of the touched function is archived for provenance.
set -euo pipefail
cd "$(dirname "$0")/../../.."

require_gate() {
    grep -q "defined($1)" "$2" || {
        echo "FAIL-CLOSED: research gate $1 absent from $2." >&2
        echo "Run bench/research/uring-router-580/prepare-research-worktree.sh for a patched isolated tree." >&2
        exit 2
    }
}
require_gate SLUICE_580_WE2_REVERSE_CANCEL_SCAN src/async/uring_transport.cpp
mkdir -p build-research

CXXFLAGS=(-O3 -std=c++20 -DNDEBUG -DSLUICE_HAS_LIBURING -Wall -Iinclude)
SRCS=(bench/research/uring-router-580/router_ab.cpp
      src/async/async_io_context.cpp src/async/threadpool_backend.cpp
      src/async/fail_fast.cpp src/async/request_scope.cpp
      src/async/detail/context_identity.cpp src/async/detail/request_core.cpp
      src/async/uring_backend.cpp src/async/uring_completion.cpp
      src/async/uring_transport.cpp src/blocking_file.cpp src/file_resource.cpp)

g++ "${CXXFLAGS[@]}" "${SRCS[@]}" -luring \
    -D SLUICE_580_ARM=baseline \
    -o build-research/router_ab_baseline

g++ "${CXXFLAGS[@]}" "${SRCS[@]}" -luring \
    -D SLUICE_580_ARM=we2 -D SLUICE_580_WE2_REVERSE_CANCEL_SCAN \
    -o build-research/router_ab_we2

sha256sum build-research/router_ab_baseline build-research/router_ab_we2
mkdir -p docs/research/uring-router-580/results
for arm in baseline we2; do
  objdump -d -C --no-show-raw-insn "build-research/router_ab_$arm" \
    | sed -n '/UringAsyncBackend::find_live_router_index_/,/^$/p' \
    > "docs/research/uring-router-580/results/disasm_find_live_router_index_$arm.txt"
done
wc -l docs/research/uring-router-580/results/disasm_find_live_router_index_*.txt
