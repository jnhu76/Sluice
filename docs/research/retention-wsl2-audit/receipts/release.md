# receipts/release.md — Release 构建 + 全量测试 + Binary Identity

Unit: `release` (WSL2 retention audit)
Date: 2026-10-29 (all times local)
Worktree: `/tmp/sluice-val-release` (detached), commit `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`
`git -C /tmp/sluice-val-release status --porcelain` → empty (pristine tree, no local modifications)

Environment: Ubuntu 26.04.1 LTS, kernel `6.18.54.1-microsoft-standard-WSL2`, gcc 15.2.0
(Ubuntu 15.2.0-16ubuntu1), xmake v3.1.2+HEAD.5d21e9d, 8 CPUs.
Configure warning: `Ascend SDK not found` (expected on this host; no Ascend target configured).

## cmd-01 buildAndTest — clean release build + full test matrix

```
cd /tmp/sluice-val-release
xmake clean -a && rm -rf build        # clean pre-existing partial artifacts first
xmake f -c -m release --liburing=n -y
xmake -y
xmake test -y
```

Configure output: `checking for platform ... linux (x86_64)`, `checking for
sluice_liburing_capability ... ok`, `warning: Ascend SDK not found`.

Build (`xmake -y`): builds default targets only — `sluice_core` (2 TUs:
`src/file_resource.cpp`, `src/blocking_file.cpp`; `src/*.cpp` contains exactly
these two non-async TUs). `sluice_async`, apps and test binaries are
non-default and are built on demand by `xmake test`. Result: `build ok,
spent 1.667s`, exit 0. Build wall time (`time xmake -y`): 2.077s total.

Full test run (`xmake test -y`): exit 0. First run also compiled all test/app
binaries (on-demand); wall time `time xmake test -y` = 1:09.23 total
(276.36s user, 451% cpu, dominated by parallel compilation of the 58 test
binaries).

Exact summary line (verbatim from output, ANSI stripped):

```
100% tests passed, 0 test(s) failed out of 58, spent 4.159s
```

Result: **58/58 passed, 0 failed** — matches the scout expectation of 58
`add_tests` targets at master 84e1b7d5 and the frozen E5 reference 58/58.
Per-test lines: 58 `passed` lines in the log (57 dot-leader format +
`backend_contract_conformance_threadpool_test` whose long name has no dot
leader), zero `failed` lines. Full 58-name enumeration (from the log, all
`passed`):

app_copy_consumption_test, app_copy_dir_fsync_test, app_copy_pipeline_oracle_test,
app_grep_consumption_test, app_hash_consumption_test, app_tail_cli_oracle_test,
app_tail_consumption_test, app_tail_lifecycle_oracle_test, async_sync_admission_test,
backend_contract_conformance_threadpool_test, blocking_file_read_test,
blocking_file_sequential_test, blocking_file_state_test, blocking_file_sync_all_test,
blocking_file_sync_data_test, blocking_file_write_test, direct_composition_fault_test,
direct_composition_test, direct_cursor_position_test, direct_w01_consumer_probe,
explicit_file_ref_test, external_loop_stop_order_test, file_access_precedence_test,
file_close_semantics_test, file_info_identity_test, file_move_borrow_test,
file_open_contract_test, file_read_test, file_resource_test, file_sync_all_test,
file_sync_data_test, file_write_test, progress_notification_suppression_test,
progress_source_ownership_test, public_request_release_violation_test,
public_request_test, request_core_consumer_probe, request_core_init_closure_test,
request_core_ownership_test, request_core_protocol_test, request_core_publication_test,
request_error_domain_threadpool_test, request_scope_test,
semantic_durability_reference_test, semantic_effect_outcome_test,
semantic_errno_mapping_test, semantic_open_test, semantic_range_test,
semantic_reference_case_test, semantic_short_io_reference_test,
semantic_validation_precedence_test, shutdown_lifecycle_test, submit_concurrency_test,
threadpool_external_loop_test, threadpool_progress_pass_bound_test,
threadpool_progress_race_test, threadpool_request_core_integration_test,
uring_submit_boundary_test.

Liburing-gated exclusions (expected, not failures): `xmake/tests.lua:126`
`if has_config("liburing")` gates `semantic_backend_conformance_test`,
`uring_public_consumer_probe`, `uring_backend_smoke_test`
(`xmake/tests.lua:138,147,156`); with `--liburing=n` they are not registered,
hence 58 (not 61).

Raw logs (worktree-local, transient): `/tmp/sluice-val-release-build.log`,
`/tmp/sluice-val-release-test-full.log`.

## cmd-02 direct group — Direct Primitive suites

Commanded matrix used bare target names; adaptation required (see ADAPTED note).

```
cd /tmp/sluice-val-release
xmake f -c -m release --liburing=n -y     # already configured, no-op re-check
xmake -y                                   # already up to date
xmake test -y '*/blocking_file_read_test' '*/blocking_file_write_test' \
  '*/blocking_file_state_test' '*/blocking_file_sequential_test' \
  '*/direct_cursor_position_test' '*/file_access_precedence_test' \
  '*/file_close_semantics_test' '*/semantic_range_test' \
  '*/semantic_validation_precedence_test'
```

Output (exit 0):

```
100% tests passed, 0 test(s) failed out of 9, spent 0.096s
```

9/9 passed: blocking_file_read_test, blocking_file_sequential_test,
blocking_file_write_test, blocking_file_state_test, file_close_semantics_test,
direct_cursor_position_test, semantic_range_test,
semantic_validation_precedence_test, file_access_precedence_test — all the
commanded suites, including the C1 killer suites (blocking_file_{read,sequential,
state}_test) and the SLUICE_FILE_INTERNAL_TESTING seam build
file_close_semantics_test (seam declaration `xmake/tests.lua:61-72`:
`direct_seam_target` adds `add_defines("SLUICE_FILE_INTERNAL_TESTING")` at
line 67; the target is registered at line 72).

## cmd-03 composition group — Composition / SEM-05 stop-reason oracle suites

```
cd /tmp/sluice-val-release
xmake test -y '*/direct_composition_test' '*/direct_composition_fault_test' \
  '*/semantic_short_io_reference_test' '*/semantic_effect_outcome_test'
```

Output (exit 0):

```
100% tests passed, 0 test(s) failed out of 4, spent 0.005s
```

4/4 passed: direct_composition_fault_test (composition stop-reason oracle +
write-side impossible-count script), semantic_short_io_reference_test (SEM-05
rules-table oracle), semantic_effect_outcome_test (Effect Certainty
accounting), direct_composition_test.

## cmd-04 Binary Identity — sha256 of key artifacts and SDK headers

Artifacts under `/tmp/sluice-val-release/build/linux/x86_64/release/`
(release, gcc 15.2.0, `--liburing=n`, no `--hardened`):

```
d41685d2ab7be2ea6cc1e8e9870537e2b67992f7b7a2396dd15ba2e25570d338  libsluice_core.a
e456008ee510b7c9901b0bf9dc360ce9650b4fee668587699e7018d277f073b4  libsluice_async.a
b274ade7e60fa65c9e85755b6512eb5953ab5f1aaafce9f1b34a511e687c7870  sluice-tail
815b73c4105c436fcd687a3262ef6f63fbd22647fa503fb3562c095745ad7874  libtail_pread_shim.so
52f14446198a27456fad20b615f85dc5f2ff1586c884fb20faf39060eb417be6  blocking_file_read_test
d4616145e74d9ead4982fd63ec7d04834c5f4ec212695f0042aa65a6293c4d9f  blocking_file_write_test
3ed89051d0fa4a2ba48b2e7f2f0cde61c9386c136eaf85e2db8fc59732655894  blocking_file_state_test
8892c713af3131e1853be818f49643ebfec987fa0a65b1ba5b8b74dde86742cd  blocking_file_sequential_test
0acdf182fc34f2e436a796befc6705126ce8aa0bb8e807afc595fa8cf4e7ef09  direct_cursor_position_test
c2560c778ea553170ac2284d0d60b8ac859d226bc67532653bfac11343d28f1b  file_access_precedence_test
f7b57d874cd55ab4f95a688ee96f6d49db808e7e08fe0be9b8e58d3ec31fbe09  file_close_semantics_test
fbaff1b3e2968f89df619282fee09b416107803f4f93adee0c0aac94d649631f  semantic_range_test
989e69c17458058afe3269929e98d4c2d904c19ce5867d3eb585189b656581b9  semantic_validation_precedence_test
fbc240e85eead5d95b647395dfd5e34d978c6ae8371196c16e29cf06477bcf21  direct_composition_test
2c5e0bc44198cdc289aa796e4c9df74694ce25b86fb5f594c140a5d25ab4ba5a  direct_composition_fault_test
89b09215af5bb4dc4ab9902fe80d7f5e1df29269ac82a309f71ea13a182ec633  semantic_short_io_reference_test
3ed22fe63fb74917e3367f1c497d1725e4325079a093c0c9025a20006f0bc5f3  semantic_effect_outcome_test
b619750612fe1e38828e47d1e851302534f94f5d19f0a52109cfd3e248e12b7d  direct_w01_consumer_probe
```

Installed public SDK headers (the `add_headerfiles` allowlist in
`xmake/libraries.lua`; source paths relative to
`/tmp/sluice-val-release/include/`):

```
51c28afb5a5cebf2f3cb4f4ed3a1e2a5cf46f132cf32ffe3f9cd6ee8d18dd505  sluice/effect.hpp
387eeec1cb39de9ea14edbaee24bd16e494375e81482133627ac7532532a5539  sluice/error.hpp
3cd9fc33b891c2221da18fef75ae74bb101c3eec106b42f50e387265006d4e7c  sluice/file_resource.hpp
dce4ed9c7c1dcfbd3518d6a07f2c349e90b689c99e1b9bf647475268b440dc7c  sluice/measurement.hpp
7b331f5250f9d0bc9acf6bf9366dfaef23cb96b975b9cd3065ed7108cfb557cf  sluice/result.hpp
930902c07e51feeff7800b1460e0ffb9abd045e77fbdb3e3d1299cc4abd8b67f  sluice/blocking/file.hpp
beb52389a9ed37d94f17c7e181900a225467024813223f31d0c1458f22486f4e  sluice/async/async_io_context.hpp
2e369190edaf81d055f0619c486df9dd9496d349822c888e1447e9c1a888ea1a  sluice/async/request.hpp
e329db10e70845736b3f749b2181f0029190317d4095cae75623742f9c2ccfc8  sluice/async/request_scope.hpp
a20a201eac5fbb8f12db644437c6d4bf177c7ecf9e403b7c04cd7291065432d0  sluice/async/threadpool_backend.hpp
a614d4ca53402d372270c3c90756735ae96767a8f388a0371796d09f17b34677  sluice/async/uring_backend.hpp
```

Note: `libsluice_core.a` / `libsluice_async.a` archives are built with GNU ar
`-D` (deterministic mode, forced in `xmake/libraries.lua`), so archive hashes
are reproducible for identical objects. ELF binaries embed no timestamps by
default; hashes should be stable across rebuilds with the same toolchain and
flags, but cross-machine reproducibility was not tested in this audit (same
host only).

## ADAPTED — command deviations from the scout matrix

1. Test selector syntax: the commanded `xmake test -y blocking_file_read_test
   ...` (bare names) produced `nothing to test` on xmake v3.1.2+HEAD —
   verified twice (`xmake test -y blocking_file_read_test` → `nothing to
   test`). `xmake test -h` documents pattern forms (`foo`, `*/foo`,
   `targetname/*`); the working form on this version is `*/name`. Both group
   runs above use `*/name`; all 9 direct + 4 composition commanded suites
   matched and executed, so coverage of the commanded matrix is complete.
2. Clean-build preamble: the worktree contained leftover partial artifacts
   from an earlier interrupted session (only `sluice_core` objects, archive
   absent). `xmake clean -a && rm -rf build` was inserted before the
   commanded configure/build so cmd-01 reflects a from-clean release build.
   No source or config file was modified (`git status --porcelain` empty).
3. Group runs were executed after the full-matrix run with binaries already
   built, so their wall times (≈0.4–1.5s) reflect no compilation.

## Result

- Full matrix: 58/58 passed, 0 failed (exact xmake summary line above).
- Direct Primitive group: 9/9 passed.
- Composition group: 4/4 passed.
- status = PASS (no adaptation changed what was measured; both deviations
  are recorded above and neither weakens an assertion).
