# Provenance of the request-scope TSan case-count evidence

Head `cdb2ca0f`; the binaries are the six-profile matrix's own builds
(`/tmp/pre-f6-build-debug-n`, `/tmp/pre-f6-build-tsan-n`; the matrix run
that produced them is recorded under `../matrix/` with
`toolchain.log` binding the head).

- `head-dbg-rs.log` —
  `/tmp/pre-f6-build-debug-n/linux/x86_64/debug/request_scope_test`
  (no options); exit 0; `25 of 25 request scope tests passed`.
- `head-tsan-rs.log` —
  `/tmp/pre-f6-build-tsan-n/linux/x86_64/tsan/request_scope_test` with
  `TSAN_OPTIONS=halt_on_error=1`; exit 0; `24 of 24 request scope tests
  passed`.
- Symbol counts (recorded here; binaries remain re-derivable from the
  matrix build dirs, and the matrix logs are committed):
  `nm -C <debug binary> | grep -cE 'g_allocations|AllocationProbe'` → 3;
  the same grep on the TSan binary → 0. Strong allocation overrides:
  `nm -C` on the debug binary shows 6 `operator new/delete` replacements
  (2 new + 4 delete); on the TSan binary the only `operator new/delete`
  definitions are the TSan runtime's (20, identical count in
  `request_core_ownership_test`, which never had a seam).
