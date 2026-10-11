#!/usr/bin/env bash
set -euo pipefail
repo=/home/jnhu/Source/Sluice-f3-tail-owned-driver
sha=f717e170331df8e098634fe60462b7dd256f8d7b
matrix=/home/jnhu/f3-combined-f717e170-20261011
mkdir -p "$matrix"
mkdir -p "$matrix/evidence"
printf "%s\n" "$sha" > "$matrix/evidence/source-sha.txt"
uname -a > "$matrix/evidence/host.txt"
for mode in debug release; do
 for ring in n y; do
  profile="$mode-$ring"; root="$matrix/$profile"; log="$matrix/evidence/$profile"
  mkdir -p "$root" "$log"
  if [[ ! -f "$log/config.log" ]]; then
  git -C "$repo" archive "$sha" | tar -x -C "$root"
  cd "$root"
  printf "%s\n" "cwd=$root" "xmake f -P $root -c -m $mode --liburing=$ring -o $root/build -y" > "$log/commands.log"
  ~/.local/bin/xmake f -P "$root" -c -m "$mode" --liburing="$ring" -o "$root/build" -y > "$log/config.log" 2>&1
  fi
  cd "$root"
  targets=(app_copy_consumption_test app_copy_pipeline_oracle_test app_copy_dir_fsync_test app_tail_consumption_test app_tail_lifecycle_oracle_test app_tail_cli_oracle_test request_scope_test public_request_test public_request_release_violation_test backend_conformance_e1_threadpool_test shutdown_lifecycle_test direct_w01_consumer_probe semantic_short_io_reference_test file_close_semantics_test)
  if [[ "$ring" == y ]]; then targets+=(request_scope_uring_test public_request_uring_test public_request_release_violation_uring_test backend_conformance_e1_uring_test shutdown_lifecycle_uring_test uring_public_consumer_probe uring_backend_smoke_test semantic_backend_conformance_test); fi
  for target in "${targets[@]}"; do
   if [[ "$profile" == debug-n ]] && rg -q "^debug-n $target PASS$" "$matrix/evidence/summary.log"; then continue; fi
   printf "%s\n" "xmake build -P $root -j 8 $target" "timeout 180 xmake run -P $root $target" >> "$log/commands.log"
   ~/.local/bin/xmake build -P "$root" -j 8 "$target" > "$log/$target-build.log" 2>&1
   timeout 180 ~/.local/bin/xmake run -P "$root" "$target" > "$log/$target.log" 2>&1
   printf "%s %s PASS\n" "$profile" "$target" | tee -a "$matrix/evidence/summary.log"
  done
  for app in sluice-copy sluice-tail; do
   printf "%s\n" "xmake build -P $root -j 8 $app" >> "$log/commands.log"
   ~/.local/bin/xmake build -P "$root" -j 8 "$app" > "$log/$app-build.log" 2>&1
  done
  printf "%s\n" "python3 docs/evidence/f3-copy-request-scope/cli_smoke.py $root/build/linux/x86_64/$mode/sluice-copy" >> "$log/commands.log"
  python3 docs/evidence/f3-copy-request-scope/cli_smoke.py "$root/build/linux/x86_64/$mode/sluice-copy" > "$log/copy-cli.log" 2>&1
  printf "%s COPY_CLI PASS\n" "$profile" | tee -a "$matrix/evidence/summary.log"
  cp "$root/.xmake/linux/x86_64/xmake.conf" "$log/xmake.conf"
 done
done
printf "COMBINED_MATRIX PASS sha=%s\n" "$sha" | tee -a "$matrix/evidence/summary.log"
