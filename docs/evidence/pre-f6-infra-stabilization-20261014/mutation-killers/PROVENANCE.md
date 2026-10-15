# Provenance of the mutation-killer records

Head `cdb2ca0f`, host toolchain Clang 22.1.8, build dir
`/tmp/pre-f6-killers-build`, configured with
`xmake f -y -m asanubsan --liburing=n --ccache=n --toolchain=clang
--ldflags=--target=x86_64-redhat-linux-gnu` (`killers-configure.log`).

- `killers-m1-build.log` / `killers-m2-build.log` —
  `xmake build -y request_scope_mut_destructor_skips_settle` and
  `xmake build -y request_scope_mut_finish_cancels_cleanup` (one target
  per invocation); both end `build ok`.
- `killers-m1-run.log` / `killers-m2-run.log` — the binaries run with
  `ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1`;
  `killers-exit-codes.log` records the exit codes (134 / 1).
- `killers-m1-bt.log` — `gdb -batch -ex run -ex "bt -6"` on the M4 binary;
  the aborting frame names
  `destructor_cleanup_preserves_the_original_exception_and_still_releases`
  (the first case that destroys a scope with unsettled accepted work).
- `killers-m1-iso-build.log` / `killers-m1-iso-bt.log` — isolation run:
  `tests/request_scope_test.cpp` temporarily reordered so
  `destructor_settles_on_early_return_with_drain_policy` (the conformance
  ledger's recorded M4 killer) runs first; rebuilt, run under the same
  gdb recipe; the aborting frame names that scenario. The test file was
  restored immediately afterwards (`git diff` clean); the binary in the
  throwaway build dir is stale and not evidence.
