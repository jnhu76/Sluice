#!/usr/bin/env bash
# Final-HEAD reverify ladders, same recipe as perf-p2c1 build-both.sh:
#   ladder-base @ 43cb8029 (master after C1 #602)
#   ladder-cand @ 64508282 (PR #612 re-anchored head)
set -euo pipefail
cd "$(dirname "$0")"
BASE_WT=/home/jnhu/Source/Sluice-perf-final-base
HEAD_WT=/home/jnhu/Source/Sluice-perf-final-head
CCACHE_DISABLE=1 g++ -std=c++20 -O3 -DNDEBUG -fvisibility=hidden -Wall -Werror \
    -I"$BASE_WT/include" ladder.cpp "$BASE_WT/build/linux/x86_64/release/libsluice_core.a" \
    -o ./ladder-base
CCACHE_DISABLE=1 g++ -std=c++20 -O3 -DNDEBUG -fvisibility=hidden -Wall -Werror \
    -I"$HEAD_WT/include" ladder.cpp "$HEAD_WT/build/linux/x86_64/release/libsluice_core.a" \
    -o ./ladder-cand
CCACHE_DISABLE=1 g++ -std=c++20 -O2 -DNDEBUG ./mkfixture.cpp -o ./mkfixture
g++ --version | head -1
sha256sum ./ladder-base ./ladder-cand ./mkfixture ./ladder.cpp
