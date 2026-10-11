#!/usr/bin/env bash
set -eu
repo=$(git rev-parse --show-toplevel)
matrix=${TAIL_MATRIX_ROOT:?set a new absolute isolated evidence directory}
sha=${TAIL_SOURCE_SHA:-143e8d4525877eb8dabdd49d8054ff8d5b239acc}
mkdir -p "$matrix/evidence"
test -z "$(ls -A "$matrix/evidence")"
echo "$sha" > "$matrix/evidence/source-sha.txt"
for mode in debug release; do
 for ring in n y; do
  profile="$mode-$ring"
  root="$matrix/$profile"
  mkdir -p "$root" "$matrix/evidence/$profile"
  git -C "$repo" archive "$sha" | tar -x -C "$root"
  cd "$root"
  log="$matrix/evidence/$profile"
  printf '%s\n' "xmake f -P $root -c -m $mode --liburing=$ring -o $root/build -y" > "$log/commands.log"
  ~/.local/bin/xmake f -P "$root" -c -m "$mode" --liburing="$ring" -o "$root/build" -y > "$log/config.log" 2>&1
  targets=(app_tail_consumption_test app_tail_lifecycle_oracle_test app_tail_cli_oracle_test direct_w01_consumer_probe blocking_file_read_test semantic_short_io_reference_test backend_conformance_e1_threadpool_test)
  if [[ "$ring" == y ]]; then targets+=(backend_conformance_e1_uring_test); fi
  for target in "${targets[@]}"; do
   printf '%s\n' "xmake build -P $root -j 4 $target" "timeout 120 xmake run -P $root $target" >> "$log/commands.log"
   ~/.local/bin/xmake build -P "$root" -j 4 "$target" > "$log/$target-build.log" 2>&1
   timeout 120 ~/.local/bin/xmake run -P "$root" "$target" > "$log/$target.log" 2>&1
   printf '%s %s PASS\n' "$profile" "$target" | tee -a "$matrix/evidence/summary.log"
  done
  cp "$root/.xmake/linux/x86_64/xmake.conf" "$log/xmake.conf"
  nm -C "$root"/build/.objs/sluice-tail/linux/x86_64/"$mode"/apps/sluice-tail/*.o > "$log/tail-symbols.log"
  ~/.local/bin/xmake build -P "$root" -rv -j 4 sluice-tail > "$log/tail-link-closure.log" 2>&1
  if rg 'ApplicationRuntime|RuntimeTaskContext|TaskResultSlot|sluice::Scheduler|sluice::Fiber|ThreadPoolBackend|UringAsyncBackend' "$log/tail-symbols.log"; then exit 1; fi
  printf '%s SYMBOL_CONTRACTION PASS\n' "$profile" | tee -a "$matrix/evidence/summary.log"
 done
done

for sanitizer in asan,ubsan tsan; do
 root="$matrix/clang-$sanitizer"
 log="$matrix/evidence/clang-$sanitizer"
 mkdir -p "$root" "$log"
 git -C "$repo" archive "$sha" | tar -x -C "$root"
 cd "$root"
 ~/.local/bin/xmake f -P "$root" -c -m debug --liburing=n --toolchain=clang --cc=clang-22 --cxx=clang++-22 --sanitizers="$sanitizer" -o "$root/build" -y > "$log/config.log" 2>&1
 for target in app_tail_consumption_test app_tail_lifecycle_oracle_test app_tail_cli_oracle_test; do
  ~/.local/bin/xmake build -P "$root" -v -j 4 "$target" > "$log/$target-build.log" 2>&1
  set +e
  ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 180 ~/.local/bin/xmake run -P "$root" "$target" > "$log/$target.log" 2>&1
  status=$?
  set -e
  echo "$sanitizer $target exit=$status" >> "$log/summary.log"
 done
 cp "$root/.xmake/linux/x86_64/xmake.conf" "$log/xmake.conf"
 ldd "$root/build/linux/x86_64/debug/app_tail_lifecycle_oracle_test" > "$log/ldd.log"
done
