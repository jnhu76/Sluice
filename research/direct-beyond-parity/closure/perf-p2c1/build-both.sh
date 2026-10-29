#!/usr/bin/env bash
# Build the two reverify ladders with the kit's exact compile recipe (kit
# build.sh discipline, CCACHE_DISABLE=1, public headers only), each linked
# against its own fresh libsluice_core.a:
#   ladder-base @ 77e5399a45998dea396699fa2e063c94a665945a (PR #602 head)
#   ladder-cand @ 70d48f5eb55a664544b5160cd4675d32150bb21e (PR #612 head)
set -euo pipefail
cd "$(dirname "$0")"

BASE_WT=/home/jnhu/Source/Sluice-perf-p2c1-base
HEAD_WT=/home/jnhu/Source/Sluice-perf-p2c1-head

CCACHE_DISABLE=1 g++ -std=c++20 -O3 -DNDEBUG -fvisibility=hidden -Wall -Werror \
    -I"$BASE_WT/include" ladder.cpp "$BASE_WT/build/linux/x86_64/release/libsluice_core.a" \
    -o ./ladder-base
CCACHE_DISABLE=1 g++ -std=c++20 -O3 -DNDEBUG -fvisibility=hidden -Wall -Werror \
    -I"$HEAD_WT/include" ladder.cpp "$HEAD_WT/build/linux/x86_64/release/libsluice_core.a" \
    -o ./ladder-cand

CCACHE_DISABLE=1 g++ -std=c++20 -O2 -DNDEBUG ./mkfixture.cpp -o ./mkfixture

g++ --version | head -1
sha256sum ./ladder-base ./ladder-cand ./mkfixture ./ladder.cpp
