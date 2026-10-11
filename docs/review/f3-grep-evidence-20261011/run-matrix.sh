#!/bin/bash
set -euo pipefail
repo=/home/jnhu/Source/Sluice-f3-grep-direct
evidence=/home/jnhu/Source/f3-grep-evidence
sha=$(git -C "$repo" rev-parse HEAD)
for mode in debug release; do
  for ring in n y; do
    profile="$mode-$ring"
    wt="/home/jnhu/Source/sluice-f3-grep-$sha-$profile"
    git -C "$repo" worktree add --detach "$wt" "$sha"
    (
      cd "$wt"
      export XMAKE_CONFIGDIR="$wt/.xmake-profile"
      xmake f -c -m "$mode" --liburing="$ring" -o build
      for target in sluice-grep app_grep_consumption_test direct_w01_consumer_probe blocking_file_read_test blocking_file_state_test runtime_waiter_observer_test; do
        xmake build "$target"
        if [ "$target" != sluice-grep ]; then
          timeout 120 xmake run "$target"
        fi
      done
      if [ "$ring" = y ]; then
        for target in uring_public_consumer_probe uring_backend_smoke_test; do
          xmake build "$target"
          timeout 120 xmake run "$target"
        done
      fi
      python3 tests/grep_cli_oracle.py "build/linux/x86_64/$mode/sluice-grep" "$evidence/$profile-traces"
      git rev-parse HEAD
      printf 'PROFILE_PASS=%s\n' "$profile"
    ) >"$evidence/$profile.log" 2>&1
    tail -n 8 "$evidence/$profile.log"
  done
done
