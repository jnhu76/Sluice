# Receipts — 验证单元「sdk」: Installed SDK Package Verifier + Independent Consumer

- Date: 2026-10-29 (WSL2, kernel 6.18.54.1-microsoft-standard-WSL2 x64)
- Verification worktree: `/tmp/sluice-val-sdk` @ `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`
  (detached; created before this run; clean at start — `git status --porcelain` empty)
- Toolchain: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0; xmake v3.1.2+HEAD.5d21e9d; python3; ar; nm
- All builds/tests ran inside `/tmp/sluice-val-sdk`; the main workspace
  `/home/hoo/Projects/Sluice` was never built in.

## Command-matrix adjustments (recorded honestly)

1. Scout matrix said `cd /tmp/sluice-val-release`. This unit's dedicated
   worktree is `/tmp/sluice-val-sdk`, and `scripts/verify_v2_package.py:52`
   resolves `REPO` from the script's own location (parent.parent), so running
   the in-worktree script builds/installs inside that worktree by
   construction. Ran in `/tmp/sluice-val-sdk`; did not touch
   `/tmp/sluice-val-release` or any other unit's worktree.
2. Verifier stdout/stderr logs were kept OUTSIDE the worktree
   (`/tmp/sluice-val-sdk-consumer/verifier-runN.log`). Runs 1–2 show why: the
   provenance gate (`V2_B_PROVENANCE_RECORDED`,
   scripts/verify_v2_package.py:466-478,536-538) excludes only the two
   declared outputs (`--manifest-out` and its derived
   `*-archive-baseline.json`); any other file inside the worktree — including
   my own run log — counts as dirt.
3. Consumer probe source is self-written per the ask (the R3/R4 probe
   sources `/tmp/r3c1_sdk_consumer_probe.cpp` and
   `/tmp/r4p2c1_sdk_consumer_probe.cpp` were NOT present on this machine and
   were never archived — known gap noted in the matrix). My fresh 16-case
   probe implements the named case families.

## Part 1 — Installed SDK Package Verifier

Command (per unit ask; executed from the dedicated worktree):

```
cd /tmp/sluice-val-sdk && time python3 scripts/verify_v2_package.py \
    --mode release --manifest-out /tmp/sluice-val-sdk/v2-manifest.json --keep-prefix
```

### Run 1 — FAIL (self-inflicted harness dirt), 23/24 gates PASS

- Redirected the log INTO the worktree (`> /tmp/sluice-val-sdk/verifier-run.log`).
- Wall time: 1:21.45 total (51.73s user) — cold-ish xmake build + archive baseline hashing.
- Exit 1. Only failing gate:
  `[FAIL] V2_B_PROVENANCE_RECORDED (head 84e1b7d578c2, worktree_clean=False, non-output dirt=['?? verifier-run.log'])`
  Cause: my own log file inside the worktree, not a product defect. Log preserved at
  `/tmp/sluice-val-sdk-consumer/verifier-run1.log` (moved, not deleted).

### Run 2 — FAIL (stale snapshot), same single gate

- Re-ran with the log outside the worktree but BEFORE moving run 1's log;
  the provenance snapshot at script start still saw
  `?? verifier-run.log` → identical single FAIL. Exit 1. Wall: 54.8s.
- Log: `/tmp/sluice-val-sdk-consumer/verifier-run2.log`.

### Run 3 — clean, EXIT=0, 24/24 gates PASS (authoritative run)

- Worktree contained only the two declared outputs before start
  (`git status --porcelain` → `?? v2-manifest-archive-baseline.json`,
  `?? v2-manifest.json`; both exempted by the script).
- Wall time: 29.935s total (23.55s user; warm xmake cache).
- Exit code 0; final line: `v2 package verification: all gates PASS`.
- Kept prefix (`--keep-prefix`, scripts/verify_v2_package.py:766-767):
  `prefix=/tmp/sluice-v2-prefix-0vjpa23t consumer_build=/tmp/sluice-v2-consumers-41i_zfhi`

Gate summary (run 3, verbatim):

```
[PASS] V2_B_PACKAGE_PREFIX_READY (11 headers vs contracted 11, 2 libs; extra=[], missing=[])
[PASS] V2_B_PROVENANCE_RECORDED (head 84e1b7d578c2, worktree_clean=True, non-output dirt=[])
[PASS] V2_E_STANDALONE_HEADER_COMPILES (11/11 sole-include compiles; failed=[])
[PASS] V2_E_ARCHIVE_BASELINE_RECORDED (2 archives, 11 objects, 422 defined symbols -> /tmp/sluice-val-sdk/v2-manifest-archive-baseline.json)
[PASS] V2_E_ARCHIVE_MATCHES_PROFILE (async archive io_uring refs=False, make_uring_context defined=True, profile liburing=False)
[PASS] CLEAN_ROOM_CONSUMER_INCLUDES_ANGLE_BRACKET_ONLY []
[PASS] P0_W01_CORE_ONLY_CLEAN_ROOM rc=0 (compile+link+run against installed prefix; -lsluice_core only)
[PASS] P1_w02_pipeline rc=0
[PASS] P1_contract_request_lifetime rc=0
[PASS] P1_contract_admission rc=0
[PASS] P1_contract_cancel_effect rc=0
[PASS] P1_contract_observer rc=0
[PASS] P1_contract_progress rc=0
[PASS] P1_contract_shutdown rc=0
[PASS] P1_contract_durability rc=0
[PASS] P1_contract_backend_availability rc=0
[PASS] P1_W03_EXTERNAL_LOOP_THREADPOOL (rc=0)
[PASS] V2_P1_URING_EXPLICIT_NOT_SUPPORTED (rc=2)
[PASS] V2_E_ODR_PUBLIC_LAYOUT_MACRO_INDEPENDENT (public layouts agree across opposite macro views; context_bytes=8)
[PASS] NEGATIVE_V2_REMOVED_HEADERS_UNAVAILABLE []
[PASS] NEGATIVE_V2_SEAM_UNAVAILABLE []
[PASS] NEGATIVE_V2_MUTANT_DEFINES_NOT_INSTALLED []
[PASS] NEGATIVE_ASYNC_SYMBOLS_NOT_IN_CORE link against core-only must fail with undefined async symbols
[PASS] CLEAN_ROOM_NO_SOURCE_OR_BUILD_TREE_DEPENDENCY []
```

(E5 expectation "24/24" matches: 24 gates, all PASS. This is the
no-liburing profile; `V2_P2_URING_RUNTIME` is the liburing counterpart and
was not in scope of the named release command.)

### Manifest validation

`python3` read-back of `/tmp/sluice-val-sdk/v2-manifest.json`:

```
FORMAT: v2-package-manifest-v1
PROFILE: v2-noliburing
VERIFICATION_HEAD: 84e1b7d578c2c58a3c46033487d9e1ac7c639b8b   (== git rev-parse HEAD)
VERIFICATION_HEAD_DIRTY: False
BUILD_MODE: release   LIBURING: none
HEADERS: 11 (exactly the contracted list: async/async_io_context, async/request,
  async/request_scope, async/threadpool_backend, async/uring_backend,
  blocking/file, effect, error, file_resource, measurement, result)
LIBRARIES: ['lib/libsluice_async.a', 'lib/libsluice_core.a']
GATES: 24 all ok: True
COMPILER: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0
GENERATED_UTC: 2026-10-29T15:58:24.726596+00:00
```

## Part 2 — Independent Consumer (self-built dir under /tmp, installed SDK only)

- Consumer workspace (self-built): `/tmp/sluice-val-sdk-consumer/`
- Probe: `sdk_consumer_probe.cpp`, self-written this session. Sole Sluice
  include: `#include <sluice/blocking/file.hpp>` (angle bracket); File /
  FileOpen / Result / IoError / CompositionOutcome all arrive through that
  header's transitive composition. System includes: cstddef/cstdint/cstdio/
  string/vector + fcntl.h/unistd.h (temp files, fd liveness check).
- Prefix used: `/tmp/sluice-v2-prefix-0vjpa23t` (run 3's kept install, 23:58:17 +0800,
  matching GENERATED_UTC 15:58:24 UTC).

16 cases (handoff §8.4 + r4 §8.2 families):

1. full read/write round-trip (positional `write_at` + sequential `read_exact`, content equality)
2. past-EOF: `read_exact_at` 31B on 23B file → `eof_before_full`, confirmed=23, remaining=accounted, `error` unset, bytes 0..22 match, 0xAA sentinel intact
3. tail cross-boundary: offset 20, 8B request → `eof_before_full`, confirmed=3, prefix=="UVW", sentinel intact
4. zero-length `read_exact` → complete/0/accounted, no error
5. zero-length `write_all` → complete/0/accounted, no error
6. `read_exact_at` offset UINT64_MAX → invalid_argument
7. `write_all_at` offset UINT64_MAX → invalid_argument
8. closed file `read_exact` (1B) → invalid_state
9. closed file `write_all` (1B) → invalid_state
10. closed file zero-length `read_exact` → invalid_state (closed-before-empty)
11. closed file zero-length `write_all` → invalid_state
12. write-only file `read_exact` (1B) → invalid_argument
13. write-only file zero-length `read_exact` → invalid_argument (access-before-empty)
14. move-then-close: moved-from `is_open()==false`, op on moved-from → invalid_state; closing shell is success no-op; moved-to file fully operational; double close success; `fcntl(F_GETFD)` < 0 after close
15. positional round-trip at offset 40 (`write_all_at`/`read_exact_at`), file_info size==43
16. Result<CompositionOutcome> accounting: pwrite past EOF hole ("POS"@40, "HOLE"@100), read [40,104) → complete, confirmed=64, remaining=accounted, no error, hole bytes zero, tail=="HOLE"

### First probe run — 13/16, all three failures were probe-authoring errors (documented, then corrected)

```
[FAIL] 01_full_roundtrip_read_write      (my probe wrote sequentially then read sequentially)
[FAIL] 14_move_then_close_semantics      (same pattern inside case 14)
[FAIL] 16_composition_outcome_accounting_hole_read  (my arithmetic: 63B span excludes the 4th tail byte)
consumer_probe: 13/16 PASS  RUN_EXIT=1
```

Root causes verified against the implementation, not assumed:

- `write_all`/`read_exact` are sequential and share the fd offset
  (`src/blocking_file.cpp:368-392`: steps are `prevalidated_read_step` /
  `write`): a sequential write advances the offset to EOF, so a subsequent
  sequential read correctly returns 0 bytes → `eof_before_full`, confirmed=0.
  The correct round-trip composition is positional write + sequential read —
  exactly the pattern of the in-tree P0 consumer
  `tests/package/consumers/w01_direct.cpp:85-92` (which passed the verifier's
  P0 gate against the same prefix).
- Case 16: read of 63B at offset 40 covers file bytes [40,102]; "HOLE" lives
  at [100,104), so only "HOL" fits. Corrected to a 64B read covering
  [40,103] — a strictly stronger assertion (adds `complete()` +
  `confirmed_bytes==64`).

Assertions were corrected toward the true installed semantics; none weakened.

### Corrected probe — 16/16 PASS

```
cd /tmp/sluice-val-sdk-consumer
PFX=/tmp/sluice-v2-prefix-0vjpa23t
time g++ -std=c++20 -I"$PFX"/include sdk_consumer_probe.cpp -L"$PFX"/lib \
     -Wl,-rpath,"$PFX"/lib -lsluice_core -o consumer_probe   # 0.63s wall, BUILD_OK
./consumer_probe
```

```
[PASS] 01_full_roundtrip_read_write
[PASS] 02_past_eof_31_of_23_eof_before_full
[PASS] 03_tail_cross_boundary_offset20_prefix3
[PASS] 04_zero_length_read_complete_zero
[PASS] 05_zero_length_write_complete_zero
[PASS] 06_uint64_max_offset_read_invalid_argument
[PASS] 07_uint64_max_offset_write_invalid_argument
[PASS] 08_closed_read_exact_invalid_state
[PASS] 09_closed_write_all_invalid_state
[PASS] 10_closed_zero_length_read_invalid_state
[PASS] 11_closed_zero_length_write_invalid_state
[PASS] 12_write_only_read_invalid_argument
[PASS] 13_write_only_zero_length_read_invalid_argument
[PASS] 14_move_then_close_semantics
[PASS] 15_positional_write_read_at_offset
[PASS] 16_composition_outcome_accounting_hole_read
consumer_probe: 16/16 PASS
RUN_EXIT=0
```

### Independence controls

- `ldd consumer_probe` → only system libs (libstdc++, libgcc_s, libc, libm,
  vdso, loader). libsluice_core.a is linked statically; no dependency on the
  source/build tree at runtime.
- Link-stage control: compile with `-I$PFX/include` only, link without
  `-L/-lsluice_core` → exit 1 with 74 `undefined reference` errors
  (e.g. `sluice::File::open(std::string const&, sluice::FileOpen)`,
  `sluice::File::File(sluice::File&&)`) — the 16/16 run genuinely required
  the installed archive.
- Include-stage control: compile without `-I` →
  `fatal error: sluice/blocking/file.hpp: No such file or directory`.

## Leftovers (not removed, per no-delete discipline)

- Kept install prefixes from the three verifier runs (`--keep-prefix`):
  `/tmp/sluice-v2-prefix-0vjpa23t` (run 3, authoritative),
  `/tmp/sluice-v2-prefix-zsax2lot` (run 2), `/tmp/sluice-v2-prefix-s3w_r18u` (run 1).
- `/tmp/sluice-v2-consumers-*` scratch dirs created by the verifier.
- `/tmp/sluice-val-sdk` worktree left untouched for the orchestrator's
  unified cleanup; no `git worktree remove` executed by this unit.
- Artifacts referenced: `/tmp/sluice-val-sdk/v2-manifest.json`,
  `/tmp/sluice-val-sdk/v2-manifest-archive-baseline.json`,
  `/tmp/sluice-val-sdk-consumer/{sdk_consumer_probe.cpp,consumer_probe,verifier-run1.log,verifier-run2.log,verifier-run3.log,neg_link.log}`.

## UNKNOWN / not run

- liburing profile (`--liburing`, gate `V2_P2_URING_RUNTIME`): not part of
  the named release command; not run.
- R3 21-case and R4 16-case probe sources: not archived anywhere I could
  find (matrix notes the same gap); my probe is a fresh equivalent, not a
  reproduction of those sources.
