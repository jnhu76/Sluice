# Seam-gating cluster evidence (AUD-016, AUD-017, AUD-020, AUD-023) — 2026-11-21

Slice: `audit/643-seam-gating-cluster` commit `1c97dcc2`, cut from origin/master
`5fc8e803`. The validation runs below executed on a working tree identical to
`1c97dcc2` (committed immediately after the runs). Independent review re-ran the
builds and suites on a separate copy of `1c97dcc2` (`/tmp/review-c2b`) against a
master copy (`/tmp/review-c2b-master`) and reproduced every result cited here.

Host: Fedora 44, Linux 7.3.3-200.fc44.x86_64, gcc 16.2.1, xmake v3.1.2+HEAD.5d21e9d.
Configurations: xmake debug and release, `--liburing=y` and `--liburing=n`.

## Dispositions recorded

- AUD-016 (REMOVE): `classify_uring_submit`, `UringSubmitProgress`,
  `retry_uring_wait_on_eintr` deleted from `include/sluice/detail/uring_submit.hpp`
  (26 lines) plus the `<cerrno>`/`<cstdint>` includes that only served them.
  `uring_chunk_length` retained (consumers `src/async/uring_backend.cpp:487`,
  `tests/uring_submit_boundary_test.cpp`).
- AUD-017 (REMOVE): the single-argument inline overloads of
  `observe_public_result`/`consume_public_result` deleted; the three-argument
  declarations remain (`include/sluice/async/detail/request_core.hpp:202,204`),
  and the only call route is the three-argument RequestBridge path
  (`src/async/detail/request_core.cpp:721,740`).
- AUD-020 (SIMPLIFY): `dispatch_occupancy()`/`active_workers()` moved under
  `SLUICE_ASYNC_INTERNAL_TESTING` (declarations `threadpool_backend.hpp:62,64,66`;
  definitions inside the gate at `threadpool_backend.cpp:827`). The `available()`
  leg is NOT implemented — deviation recorded below.
- AUD-023 (SIMPLIFY): `syscall_count_` moved inside the gate
  (`threadpool_backend.hpp:259`) and its worker-loop `fetch_add` gated
  (`threadpool_backend.cpp:436`), between the adjacent already-gated seam calls.

## Release-behavior differentials (first-hand, commands verbatim)

Baseline tree extracted read-only: `git archive 5fc8e803 | tar -x -C
/tmp/aud-c2b-evidence/master`; slice tree `git archive HEAD | tar -x -C
/tmp/aud-c2b-evidence/slice`.

Object differential (production shape — no `SLUICE_ASYNC_INTERNAL_TESTING`),
per tree (`object-differential.txt` holds the full transcript):

```
g++ -std=c++20 -c -O2 -I <tree>/include -I <tree>/src/async \
    -D SLUICE_HAS_LIBURING -D _GNU_SOURCE <tree>/src/async/threadpool_backend.cpp -o tp_<tree>.o
nm -C tp_<tree>.o | grep -iE 'syscall_count|dispatch_occupancy|active_workers'
```

- master `5fc8e803`: exports `T ThreadPoolBackend::active_workers() const` and
  `T ThreadPoolBackend::dispatch_occupancy() const` (plus `.cold` clones);
  no `syscall_count` symbol (the counter is inline — preprocess is its criterion).
- slice `1c97dcc2`: zero matches for all three names.

Preprocess differential (same defines, `g++ -E ... | grep -c <sym>`):

| symbol | master | slice |
|---|---|---|
| `syscall_count` | 2 (member declaration + `fetch_add`) | 0 |
| `dispatch_occupancy` | 2 (declaration + definition) | 0 |
| `active_workers` | 8 | 5 (the member `active_workers_` and its production readers remain; the query is gone) |

Archive-level check on the slice release libraries
(`release-nm-liburing-y.txt`, `release-nm-liburing-n.txt`):

```
xmake f -m release --liburing=y -y && xmake -y sluice_async
nm -C build/linux/x86_64/release/libsluice_async.a | grep -iE 'syscall_count|dispatch_occupancy|active_workers'
```

Zero matches in both profiles. The liburing=y transcript also records the
deliberate deviation control: `T UringAsyncBackend::available() const` is still
exported (ungated capability probe, see below).

## Suite results

All runs `xmake run -y <target>`, exit 0 required for PASS.

liburing=y (debug): `uring_owed_residue_test`, `backend_contract_conformance_threadpool_test`,
`backend_contract_conformance_uring_test`, `uring_backend_smoke_test`,
`request_core_protocol_test`, `request_core_publication_test`,
`request_core_ownership_test`, `request_core_ownership_uring_test`,
`public_request_test`, `public_request_uring_test`, `request_scope_test`,
`request_scope_uring_test`, `threadpool_request_core_integration_test`,
`semantic_backend_conformance_test`, `semantic_validation_precedence_test`,
`file_move_borrow_test`, `file_move_borrow_uring_test`,
`request_error_domain_threadpool_test`, `request_error_domain_uring_test` — all PASS.

liburing=n (debug): `backend_contract_conformance_threadpool_test`,
`public_request_test`, `request_scope_test`,
`threadpool_request_core_integration_test`, `request_core_protocol_test`,
`request_core_publication_test`, `request_core_ownership_test`,
`uring_submit_boundary_test`, `file_move_borrow_test`,
`request_error_domain_threadpool_test` — all PASS.

The 30 affected test targets build clean in both profiles (build exit 0),
covering all 16 files / 20 call sites that consume `available()`.

Independent review re-runs at the same commit added per-suite counts:
`uring_owed_residue_test` 3/3, `backend_contract_conformance_uring_test` 20/20
(liburing=y), `uring_submit_boundary_test` 2/2 (liburing=n),
`uring_backend_smoke_test` 8/8, `public_request_test` 13/13 (+uring 13/13),
`request_scope_test` 25/25 (+uring 16/16),
`threadpool_request_core_integration_test` 17/17,
`backend_contract_conformance_threadpool_test` 19/19 + uring 20/20.

Note for re-runners: `request_scope_test` prints
`progress health failed...failing fast` / `terminate called without an active
exception` on stderr — that is the designed negative scenario
`health_failure_during_cleanup_fails_fast_instead_of_returning`
(`tests/request_scope_test.cpp:1273`); exit 0 with 25/25 is the pass condition.

## AUD-020 available() deviation (open, needs matrix-owner revision)

The row resolution ("三者移入 SLUICE_ASYNC_INTERNAL_TESTING 门，available()
保持 SLUICE_HAS_LIBURING 组合语义") is infeasible against the line-level
consumer surface (16 files / 20 call sites). Five call sites sit in targets
that never define the seam macro under liburing=y:

- `tests/file_move_borrow_test.cpp:57` — target compiles the async TUs in
  production shape (`xmake/tests.lua:996-1050`, comment at `:996-999`);
- `tests/request_error_domain_test.cpp:49` — same production-shape claim
  (`xmake/tests.lua:940-994`, comment at `:943-944`);
- `tests/semantic_backend_conformance_test.cpp:30` — links the shipped
  `sluice_async` library (`xmake/tests.lua:131-138`, no defines);
- `tests/uring_backend_smoke_test.cpp:54` — links the shipped library
  (`xmake/tests.lua:149-156`, no defines);
- `tests/semantic_validation_precedence_test.cpp:90` — links the shipped
  library via `sluice_one_file_target` (`xmake/tests.lua:43-44` +
  `xmake/helpers.lua:6-17`, no defines).

Any `SLUICE_ASYNC_INTERNAL_TESTING` gate on the declaration breaks these five
call sites; for the three library-linking targets, adding the define to the
test TU cannot repair the link because the symbol is produced by the production
library TU. The remaining fifteen call sites are seam-built targets (and the
liburing=n probe sites are already compiled out by their own
`#if defined(SLUICE_HAS_LIBURING)` guards). The five exposed probes carry
host-capability semantics (`NOT RUN: io_uring unavailable`), which is a
different class from the `*_for_test` state-snapshot family — the audit row's
premise ("全部是 seam 测试") does not hold for them.

Recorded control: `T UringAsyncBackend::available() const` remains exported in
the release liburing=y archive (`release-nm-liburing-y.txt`). Closing this leg
requires a matrix-owner revision — either record `available()` as a BACKEND-04
capability-probe exception left ungated, or charter a slice that converts the
five production-shape probe targets. The threadpool legs of AUD-020 are
implemented and verified (see the ledger rows).
