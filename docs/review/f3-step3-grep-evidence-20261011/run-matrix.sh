#!/bin/bash
set -euo pipefail
repo=$1
app=$2
only=${3:-all}
sha=$(git -C "$repo" rev-parse HEAD)
root=$(mktemp -d "/home/jnhu/Source/sluice-step3-$app-$sha-XXXXXXXX")
logs=$(mktemp -d "/home/jnhu/Source/sluice-evidence/f3-step3-20261011/$app-$sha-XXXXXXXX")
printf 'HEAD=%s\nWORKTREES=%s\nLOGS=%s\n' "$sha" "$root" "$logs"
for mode in debug release; do
 for ring in n y; do
  if [ "$only" != all ] && [ "$only" != "$mode-$ring" ]; then continue; fi
  wt="$root/$mode-$ring"
  git -C "$repo" worktree add --detach "$wt" "$sha"
  (
   set -eux
   cd "$wt"
   export XMAKE_CONFIGDIR="$wt/.xmake-profile"
   xmake f -c -m "$mode" --liburing="$ring" -y
   xmake build -j4 "sluice-$app"
   test ! -e "build/linux/x86_64/$mode/libsluice_async.a"
   xmake build -j4 "app_${app}_consumption_test"
   test ! -e "build/linux/x86_64/$mode/libsluice_async.a"
   mapfile -t app_objects < <(rg --files --no-ignore -g '*.o' "build/.objs/sluice-$app")
   test "${#app_objects[@]}" -gt 0
   nm -C "${app_objects[@]}" > "$logs/$mode-$ring-objects-symbols.txt"
   if rg 'sluice::async::' "$logs/$mode-$ring-objects-symbols.txt"; then exit 1; fi
   xmake run "app_${app}_consumption_test"
   binary="build/linux/x86_64/$mode/sluice-$app"
   cc -shared -fPIC -Wall -Wextra -Werror tests/support/direct_pread_shim.c -o "$logs/$mode-$ring-shim.so"
   python3 tests/app_direct_cli_oracle.py "$app" "$binary"
   python3 tests/app_direct_cli_oracle.py "$app" "$binary" "$logs/$mode-$ring-shim.so"
   if [ "$app" = hash ]; then python3 tests/hash_cli_oracle.py "$binary" --early-bounds; else python3 tests/grep_cli_oracle.py "$binary" "$logs/$mode-$ring-traces"; fi
   if [ "$app" = hash ]; then trace_args=(AGENTS.md); else trace_args=(Sluice AGENTS.md); fi
   strace -f -e trace=clone,clone3 -o "$logs/$mode-$ring-threads.trace" "$binary" "${trace_args[@]}" > "$logs/$mode-$ring-output.txt" 2> "$logs/$mode-$ring-stderr.txt"
   if rg 'clone(3)?\(' "$logs/$mode-$ring-threads.trace"; then exit 1; fi
   printf '#include "%s_task.hpp"\nauto removed = &sluice_%s::%s_files_with_backend;\n' "$app" "$app" "$app" > "$logs/$mode-$ring-header-negative.cpp"
   if g++ -std=c++20 -Iinclude -I"apps/sluice-$app" -c "$logs/$mode-$ring-header-negative.cpp" -o "$logs/probe.o" > "$logs/$mode-$ring-header-negative.log" 2>&1; then exit 1; fi
   rg 'files_with_backend.*not a member' "$logs/$mode-$ring-header-negative.log"
   mkdir "$logs/$mode-$ring-old-header"
   git show c2331d70:"apps/sluice-$app/${app}_task.hpp" > "$logs/$mode-$ring-old-header/${app}_task.hpp"
   printf '#include "%s_task.hpp"\nauto removed = &sluice_%s::%s_files_with_backend;\nint main() { return removed == nullptr; }\n' "$app" "$app" "$app" > "$logs/$mode-$ring-link-negative.cpp"
   extra=sha256; if [ "$app" = grep ]; then extra=matcher; fi
   if g++ -std=c++20 -O0 -Iinclude -I"$logs/$mode-$ring-old-header" "$logs/$mode-$ring-link-negative.cpp" "apps/sluice-$app/${app}_task.cpp" "apps/sluice-$app/$extra.cpp" "build/linux/x86_64/$mode/libsluice_core.a" -o "$logs/link-probe" > "$logs/$mode-$ring-link-negative.log" 2>&1; then exit 1; fi
   rg 'undefined reference.*files_with_backend' "$logs/$mode-$ring-link-negative.log"
   for target in sluice-hash sluice-grep sluice-copy sluice-tail; do xmake build -j4 "$target"; done
   for target in app_hash_consumption_test app_grep_consumption_test app_copy_consumption_test app_copy_pipeline_oracle_test app_copy_dir_fsync_test app_tail_consumption_test app_tail_lifecycle_oracle_test app_tail_cli_oracle_test blocking_file_read_test blocking_file_state_test direct_composition_fault_test direct_w01_consumer_probe backend_conformance_e1_threadpool_test async_sync_admission_test request_scope_test runtime_waiter_observer_test threadpool_core_cutover_test; do
    xmake build -j4 "$target"
    timeout 180 xmake run "$target"
   done
   if [ "$ring" = y ]; then
    for target in semantic_backend_conformance_test uring_public_consumer_probe backend_conformance_e1_uring_test uring_backend_smoke_test uring_core_cutover_test request_scope_uring_test; do xmake build -j4 "$target"; timeout 180 xmake run "$target"; done
   fi
   if rg 'sluice/async|ApplicationRuntime|RuntimeBuilder|RuntimeTaskContext|TaskResultSlot|run_task_to_result|Completion|await_read_at|ThreadPoolBackend|files_with_backend' "apps/sluice-$app" --glob '*.cpp' --glob '*.hpp'; then exit 1; fi
   git diff --exit-code
   printf 'PROFILE_PASS=%s-%s\n' "$mode" "$ring"
  ) > "$logs/$mode-$ring.log" 2>&1
  tail -5 "$logs/$mode-$ring.log"
 done
done
printf 'MATRIX_PASS=%s\n' "$sha"
