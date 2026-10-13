# F5-A — Live package closure and disposition table (2026-10-13)

Base master: `14a27a83821a94e6fc5a9684b48fcd296d7de891` (F2 final receipt).
Machine form: `closure-manifest.json` (same directory).

Live measurements at base: 49 installed headers, 2 core + 30 async archive
members, 32 library TUs compiled by the two library targets; the tree holds 34
`src/` TUs, the two orphan `src/experimental/*.cpp` compiled by no target. The
#402 CURRENT overlay's "56 headers / 33 async" line is the
older F3/F4-adoption snapshot; the F2 exit inventory 49/30 is current. The
overlay ambiguity is corrected by the F5 documentation slice.

## Method

Enumerated from the install globs (`xmake/libraries.lua`), the resolved archive
member lists of freshly built `libsluice_core.a`/`libsluice_async.a` in both
liburing profiles, per-member `nm -C --defined-only/--undefined-only`, and the
complete reverse-include graph over `include/`, `src/`, `tests/`, `apps/`,
`xmake/`, `scripts/`. Classification is by dependency role, not filename.

## Header disposition (49)

| # | Header | Class | Disposition basis |
|---|---|---|---|
| 21 | canonical core/async closure (see JSON list) | CANONICAL_REQUIRED | W-01/W-02/W-03 public surface; every one reachable from the canonical consumer closure; retained |
| 1 | `async/detail/context_identity.hpp` | INTERNAL (private to library TUs) | included by no installed header, only `async_io_context.cpp`, `detail/context_identity.cpp`, and the seam-build ownership test; de-install from package, keep in tree |
| 4 | `cancel.hpp`, `stackful_io_host.hpp`, `fiber.hpp`, `fiber_ctx.hpp` | OPTIONAL_CANDIDATE (W-04 host closure) | host uses Fiber/FiberCtx and `CancelToken`, not Scheduler/select/sync; `cancel.hpp` is included only by the host trio, matching F1's H-30/H-31 classification; stays installed with unsupported labeling (W-04 conditional N/A) |
| 3 | `condition.hpp`, `semaphore.hpp`, `async_queue.hpp` | COMPAT noncanonical — #475 Family A | zero in-tree consumers (reverse-include empty); outside v1 (HOST-01, MIG-02 dormant row); BOUNDED_HOLD triggers: T2/T3 fired; T1 assessed — no external-use signal observed (UNKNOWN, not zero); removal prepared, gated on T4/E1 owner decision |
| 2 | `future.hpp`, `wait_policy.hpp` | COMPAT noncanonical — #475 Family B | Group/EventedWaitPolicy dependency exit condition met (headers absent from tree since F3 part-C); `wait_policy.cpp` defines only `default_wait_policy()`; no remaining dependency/consumer beyond F4's historical retained probe; removal prepared, gated on owner adjudication |
| 10 | #475 Family C (`select`, `select_fwd`, `event`, `wait_queue`, `timer_registration`, `mutex`, `lock_guard`, `detail/queue_item`, `detail/queue_port`, `detail/select_registration`) | COMPAT noncanonical — #475 Family C | sole role was the physical closure of the retained legacy Scheduler; scheduler.hpp's installed-includer set is fully dispositioned inside the same batch (A3 + async_mutex + async_rwlock + event + select + semaphore + async_queue + select_port); wait_queue consumers (async_mutex/async_rwlock/condition/semaphore) all inside the batch; A and C exit together |
| 8 | `scheduler.hpp`, `async_mutex.hpp`, `async_rwlock.hpp`, `wait_node.hpp`, `thread_annotations.hpp`, `detail/select_port.hpp`, `detail/mutex_test_seam.hpp`, `detail/queue_test_seam.hpp` | COMPAT/INTERNAL legacy substrate | every in-tree consumer is inside the removal batch except the F4 retirement probe text (recorded as residual F5-F4PROBE); canonical archive objects reference none of them (verified by undefined-symbol audit); seams carry zero defined symbols in both profiles (D-13) |

## Archive member disposition (32 objects)

* 9 canonical async members + 2 core members: retained (W-01/W-02/W-03).
* 4 host-closure members (`cancel`, `fiber`, `fiber_ctx`, `stackful_io_host`): retained;
  `cancel.cpp.o` is consumed only by the host closure (`CancelToken::request()` is
  referenced by `stackful_io_host.cpp`), matching the header reclassification.
* 14 legacy runtime members (9 `scheduler*` — the eight `scheduler_*` TUs plus
  `scheduler.cpp` — 3 `select*`, `queue_port`, `wait_policy`): removal
  candidates, one dependency family.
* 3 test-seam members (`mutex_test_seam`, `queue_test_seam`,
  `scheduler_fe2_test_seam`): TEST_ONLY; zero defined symbols in both profiles;
  in the production archive only via the `src/async/*.cpp` glob (D-13).

## Profile-dependent findings

* no-liburing: `uring_completion.cpp.o`/`uring_transport.cpp.o` are empty
  objects (all content `SLUICE_HAS_LIBURING`-guarded); `uring_backend.cpp.o`
  carries the honest-stub surface. liburing: 64/5/25 defined symbols
  respectively. No cross-profile ODR divergence beyond the documented public
  `SLUICE_HAS_LIBURING` usage requirement.

## #475 trigger assessment

* T1: did NOT fire — no external-use signal observed through 2026-10-13;
  UNKNOWN, not zero.
* T2: F5 is itself a package-mutation event; this manifest re-records the A/B/C
  dispositions instead of carrying them silently.
* T3: F2/F3/F4 final dispositions exist; four-app retirement complete.
* T4: unresolved by construction — the consolidated owner decision packet in
  this directory is the deliverable that closes it.
* Family B: the recorded necessary condition (Group + EventedWaitPolicy exit)
  is met in the current tree; no independent surviving requirement exists
  (`future.hpp`'s only consumer is F4's historical retained probe).
* Family C: retained Scheduler has no remaining consumer outside the removal
  batch; the W-04 candidate host does not depend on it.

## Non-installed tree surfaces

* `include/sluice/experimental/` (2 headers) + `src/experimental/` (2 orphan
  TUs): zero in-tree consumers, built by no target, never installed (D-14
  isolation, M-A4 "final tree disposition" owed to F5). Proposed final-tree
  disposition: remove from tree (superseded by canonical `uring_backend`);
  source-tree-only change, no installed-surface impact; negative install proof
  preserved.
* `src/async/scheduler_internal.hpp`, `src/async/scheduler_test_access.hpp`,
  `src/async/queue_detail.hpp`: internal headers of the removed runtime; retire
  with it.

## Residuals recorded by the F5 review round

* `F5-F4PROBE`: `tests/package/f4_retirement/probes/retained.cpp` (compiled by
  `scripts/verify_f4_retirement.py` against its own F4-era prefix) is the only
  in-tree text that still includes removed headers. F4 evidence is frozen; a
  supersession label or probe update is an F4-owner/F6 hygiene follow-up.
* `F5-W04FIBER`: the retained host header `fiber.hpp` still declares
  Scheduler-era surface with no retained user (`friend class Scheduler;`,
  `CompletionWaitOutcome`, `set_entry`, `cancel_token`, `cancel_state`,
  `execution_tag`, `completion_wait_outcome` and their setters/members).
  Removing it is a further per-surface decision on the pending W-04 host.
* `F5-F1VERIFIER`: the F1-era verifier text in `scripts/verify_f1_package.py`
  still lists `scheduler.hpp` under configuration-dependent headers; left
  unchanged to preserve F1 provenance.
* `F5-FORMAL`: Lean/TLA models naming retired headers remain
  HISTORICAL/ADAPT/RETIRE_FROM_V1_EVIDENCE per the formal evidence map; no model
  file changed in this slice.

Machine form: `RESIDUALS` in `closure-manifest.json`.
