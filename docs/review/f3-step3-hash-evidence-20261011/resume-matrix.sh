#!/bin/bash
set -euo pipefail
app=$1
repo="/home/jnhu/Source/Sluice-f3-$app-legacy-exit"
sha=$(git -C "$repo" rev-parse HEAD)
if [ "$app" = hash ]; then dispatch=/home/jnhu/Source/sluice-evidence/f3-step3-20261011/hash-final-matrix-dispatch.log; else dispatch=/home/jnhu/Source/sluice-evidence/f3-step3-20261011/grep-matrix-dispatch.log; fi
root=$(sed -n 's/^WORKTREES=//p' "$dispatch")
logs=$(sed -n 's/^LOGS=//p' "$dispatch")
for profile in debug-n debug-y release-n; do
 wt="$root/$profile"
 (
  set -eux
  cd "$wt"
  test -z "$(git status --porcelain)"
  old=$(git rev-parse HEAD)
  test "$(git diff --name-only "$old" "$sha")" = tests/request_scope_test.cpp
  git switch --detach "$sha"
  mode=${profile%-*}; ring=${profile##*-}
  export XMAKE_CONFIGDIR="$wt/.xmake-profile"
  xmake f -m "$mode" --liburing="$ring" -y
  xmake build -j4 request_scope_test
  timeout 180 xmake run request_scope_test
  if [ "$ring" = y ]; then xmake build -j4 request_scope_uring_test; timeout 180 xmake run request_scope_uring_test; fi
  mapfile -t objects < <(rg --files --no-ignore -g '*.o' "build/.objs/sluice-$app")
  test "${#objects[@]}" -gt 0
  nm -C "${objects[@]}" > "$logs/$profile-objects-symbols.txt"
  if rg 'sluice::async::' "$logs/$profile-objects-symbols.txt"; then exit 1; fi
  if [ "$profile" = release-n ]; then
   for target in runtime_waiter_observer_test threadpool_core_cutover_test; do xmake build -j4 "$target"; timeout 180 xmake run "$target"; done
  fi
  test -z "$(git status --porcelain)"
  printf 'PROFILE_INCREMENTAL_PASS=%s SHA=%s\n' "$profile" "$sha"
 ) > "$logs/$profile-scope-resume.log" 2>&1
 tail -3 "$logs/$profile-scope-resume.log"
done
/home/jnhu/Source/sluice-evidence/f3-step3-20261011/run-matrix.sh "$repo" "$app" release-y
