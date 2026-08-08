# Changelog

## Unreleased — Phase C2e close / drain / reset / destruction (Issue #68 rows 15–16)

### Changed

- **`sluice::async::FakeAsyncBackend::close_admission()` added** — the
  reference backend now exposes the same production admission close as
  `ThreadPoolBackend` (ADR Decision 15 reference semantics:
  `arena_.close_admission()`). New `submit_*` returns `invalid_state`
  (Completion idle, no borrow) while existing accepted requests continue;
  cancel/poll/wait_one/reap remain legal. Idempotent. Additive — no existing
  behavior changed. Fake has no split wait capability (its `wait_one` is
  non-blocking by contract), so there is no parked participant to wake; the
  arena admission flag alone is the full reference semantics.
  (`include/sluice/async/fake_backend.hpp`)

- **Backend admission transaction domain (ADR §"Commit / accept" :453-462)**
  — `ThreadPoolBackend` and `FakeAsyncBackend` now hold `admission_mtx_`
  across the whole submit acceptance protocol (reserve → prepare →
  begin_binding → arena commit → install_binding → commit_binding), and
  `close_admission()` takes the same lock before `arena_.close_admission()`
  (the ThreadPool then interrupts parked waiters after the lock is released).
  The commit/accept linearization point is the Step 5 `binding -> outstanding`
  release-store; the winning submit "retains its own context/admission lock"
  through it, so after `close_admission()` returns NO new acceptance LP can
  occur (Decision 15): an in-flight submit either completes its LP before
  close returns (submit wins) or observes admission closed at reserve and
  rejects synchronously with `invalid_state` (close wins). The lock is
  released before enqueue (no-fail) and is never nested with `work_mtx_` or
  the ready-wait mutex. `RequestArena::commit()` itself carries no admission
  check (a reserved/prepared slot is an in-flight submission and completes
  its protocol; close gates NEW acceptance at reserve only) — the previous
  draft's `commit()`-level gate was removed because the arena's `commit()` is
  only the "submit-success linearization point's slot half", not the LP.
  Pinned by `close_admission_gates_reserve_not_inflight_prepared_slot` +
  `tp_c2e_close_waits_for_inflight_acceptance_lp` +
  `tp_c2e_close_wins_submit_started_before_close_rejected` +
  `fake_c2e_close_waits_for_inflight_acceptance_lp` +
  `tp_c2e_submit_races_close_linearization`.
  (`include/sluice/async/threadpool_backend.hpp`,
  `src/async/threadpool_backend.cpp`,
  `include/sluice/async/fake_backend.hpp`,
  `include/sluice/async/detail/request_arena.hpp`)

### Tests

- `tests/backend_conformance_test.cpp` — shared close/drain suite
  (`run_close_drain_cases`, driven per backend by
  `conformance_close_drain_fake` / `conformance_close_drain_threadpool`):
  close rejects future submit with `invalid_state` (idle Completion, zero
  residue), accepted-before-close reaches exactly one defined terminal with
  cancel/poll/reap legal, drained != releasable (`slot_in_use == 1` until the
  caller resets the ready Completion), slot-release vs admission-close
  orthogonality.
- `tests/threadpool_backend_c2e_close_drain_test.cpp` — deterministic window
  evidence on the real backend (15 cases): close while pending/enqueued/
  running (real result verbatim, size + void), close then pending cancel
  winner, close then running cancel intent only, one-shot parked-waiter wake
  with no busy-spin, close ‖ final `record_terminal` in both orderings plus
  the interrupt-vs-final-ready window closed by `wait_one`'s final reap,
  invariant race drain, submit ‖ close linearization, and the admission
  transaction arbitration: `tp_c2e_close_waits_for_inflight_acceptance_lp`
  (submit paused between the slot commit and the Step 5 release-store, INSIDE
  the transaction; the closer observes the Completion at the close return —
  the LP must have happened before close returned) and
  `tp_c2e_close_wins_submit_started_before_close_rejected` (submit paused
  before the admission lock; close wins; the resumed submit rejects at
  reserve). The linearization case asserts BOTH legal reject codes
  (`invalid_state`, `would_block`) leave the Completion
  idle/not-outstanding/not-ready (half-accepted detector), and computes its
  drain deadline fresh after the submitter joins.
- `tests/request_lifecycle_scheme_b_test.cpp` — added
  `close_admission_gates_reserve_not_inflight_prepared_slot` (arena
  boundary: close gates NEW acceptance at reserve; an in-flight
  reserved/prepared slot completes its protocol — the arena is not the
  admission authority, the backend admission transaction is).
- `tests/fake_backend_c2e_close_drain_test.cpp` (new) — the Fake driver's
  admission transaction: close blocks on an in-flight acceptance protocol
  paused between the slot commit and the Step 5 release-store (the fake's
  SubmitPauseGate), the resumed submit wins its LP, close returns after, and
  a new submit after close rejects `invalid_state`.
- `tests/async_io_context_split_wait_c2e_test.cpp` (new) — context-level
  detector for `AsyncIoContext::wait_one`'s interrupted-branch final poll: a
  test-only split-wait backend/wait source (public `AsyncBackend` /
  `BackendWaitSource` interfaces; no production context seam) pauses
  `wait_for_change()` after observing a control interrupt, the test records
  backend-ready in that window, and the context's final poll is the only path
  that can reap it (deleting it → wait_one returns 0).
- `tests/threadpool_backend_death_test.cpp` — added the `pending`-state
  non-quiescent destruction death case.
- `tests/fake_backend_death_test.cpp` (new) — Fake-type non-quiescent
  destruction fail-fast (unreaped bound request; ready-but-unreset
  Completion) + quiescent control, Debug AND Release.
- `scripts/backend_conformance_manifest.py` / `scripts/verify-backend-conformance.py`
  — C2e evidence records (`c2e_shared_close_drain_suite`,
  `c2e_threadpool_close_drain_race`, `c2e_fake_close_drain_death`,
  `uring_c2e_close_drain_not_implemented`) + per-backend close/drain driver
  driving; Uring's C2e gap enters its verdict via
  `applicable_evidence_for_backend()` (never skip-as-pass).
- `scripts/tests/test_backend_conformance_manifest.py` — 16 new C2e self-test
  cases (records exist/applicable; missing close/drain run → INCOMPLETE;
  per-backend attribution isolation; Uring gap never PASS; drive exclusion).
- `include/sluice/async/threadpool_backend.hpp` /
  `src/async/threadpool_backend.cpp` — `ControlWakeFinalReapPauseGate`,
  `BeforeCommitBindingPauseGate`, and `BeforeAdmissionLockPauseGate`
  (guarded, compiled out of production; 0 symbols in `libsluice_async.a`)
  making the interrupt-vs-final-ready window and the admission-transaction
  windows deterministic.

## Unreleased — Group Evented admission exception safety (P2-01)

### Changed

- **`sluice::async::Group::async_evented` admission is now transactional.** All
  three bookkeeping vectors (`evented_fibers_`, `evented_stacks_`, `futures_`) are
  reserved to `size() + 1` BEFORE the first `push_back`, inside one `mtx_`
  critical section (`include/sluice/async/group.hpp`). Previously the three
  `push_back`s could fail independently, leaving a partial Fiber/stack/Future
  record on allocation failure. Now a reserve failure propagates
  `std::bad_alloc` with no partial task record, no `Scheduler::spawn`, and the
  user task never runs. The three moved types are noexcept-movable (pinned by
  `static_assert`), so the post-reserve commit block is non-throwing. This
  closes the E16 foundation prerequisite P2-01 (Group transactional admission
  seam); it does NOT implement E16 Application Runtime. The Threaded
  (`async_threaded`) public behavior is unchanged.

### Tests

- `tests/group_evented_admission_exception_safety_test.cpp` — deterministic
  failure injection at each of the three reserve boundaries, proving no partial
  task record, no Scheduler publication, safe destruction without `await()`, and
  Group reusability. Links `sluice_async_internal_testing`; the test seam is
  absent from production.

## Unreleased — documentation corrective (post-v0.1.0)

Documentation and build metadata changes since the v0.1.0 tag. No production
library behavior or unit-test behavior changed.

### Changed

- **API reference accuracy** — `docs/api-reference.md` Async Runtime section
  rewritten to match installed public headers exactly. Test-only seams
  (`spawn_on`, `advance_clock`, `shutting_down_for_test`) marked as NOT user API.
- **Async quickstart** — new `examples/async_foundation_quickstart.cpp` (public
  headers only). README examples match.
- **Async runtime architecture** — `docs/architecture/async-runtime.md`
  separates Scheduler-integrated primitives from policy-based task waiting.
- **Link checker integrity** — `scripts/check-doc-links.py` now resolves
  markdown links strictly doc-relative (matches GitHub rendering). Self-test
  mode added. 69+ stale doc links repaired.
- **Changelog** — corrected version identity and E13/E14/E15 status.
- **Build metadata** — `xmake/examples.lua` adds `async_foundation_quickstart`
  target; `.github/workflows/ci.yml` restores the documentation verification
  gate (self-test + full scan). The link checker now scans only git-tracked
  Markdown via `git ls-files`, so generated/gitignored files under `docs/`
  cannot leak into the CI scan set.

## v0.1.0 — Runtime Foundation (E10–E15)

The first tagged release. Synchronous core (`sluice_core`) and asynchronous
runtime (`sluice_async`) both production-ready.

### Added

**Synchronous core (`sluice_core`):**

- `Result<T>`/`IoError` error model, `Reader`/`Writer` semantics, `copy_all`
  with `CopyStrategy` (Scratch/BufferedFirst/Auto), `FileReader`/`FileWriter`
  (POSIX, positional I/O, vector I/O), `BlockingIoContext`/`MemoryIoContext`,
  `SyncableWriter` (`sync_data`/`sync_all`), WAL record format, `BlockingIoPool`.

**Asynchronous runtime (`sluice_async`):**

- E10 `WaitNode`/`WaitQueue`, E11 `TimerRegistration`/deadline.
- E12-A `Event`, E12-B `Semaphore`, E12-C `AsyncMutex`, E12-D `AsyncCondition`,
  E12-E `AsyncQueue<T>`, E12-F `AsyncRwLock`.
- E13 `Select` (multi-arm Event/Timer select).
- E14 Threaded/Evented parity (`ThreadedWaitPolicy`/`EventedWaitPolicy`).
- `Scheduler`, `Fiber`, `Completion<T>`, `AsyncIoContext`, `Future<T>`,
  `Group`, `Batch`, `ThreadPoolBackend`.

**Experimental:**

- `UringAsyncBackend` — Linux io_uring (build-gated behind `--with-liburing`,
  stub without liburing). **Not the default backend.**

### Tests

109 tests, all green in debug and release. Coverage spans the synchronous
core, all async synchronization primitives, multi-worker scheduling, and both
execution strategies.

### Known limitations

```text
io_uring remains experimental unless real liburing validation supports promotion.
No production io_uring backend yet.
Public cooperative cancellation primitives exist (CancelToken, CancelState,
CancelGuard, check_cancel()), but E16 has not yet introduced
application-runtime-wide cancellation ownership and shutdown policy.
No networking.
No default backend switch (BlockingIoContext stays the default).
No universal performance conclusion.
liburing/kernel support required for the uring path; without it the path is a clean stub.
FileWriter::flush() does not imply durability (by design).
Zig stdlib remains design reference only, not a dependency.
```

### Non-goals for this release

- io_uring as the default backend.
- Networking, mmap.
- Universal performance claims.
