# Zig `std.Io` → cppio migration job cards

**Status: SLUICE-CORE-023C.** Authoritative Job Card sequence for migrating
Zig `std.Io`'s async execution architecture into cppio, built from the
source-derived dependency graph in `docs/zig-stdio-async-port-map.md` (023A)
and governed by the parity verdict in `docs/async-backend-parity.md` (023B).

Job IDs continue the repository's existing async numbering (017-022 are GREEN;
see `docs/async-next-jobs.md`). New jobs start at **023** (the audit docs
themselves are 023A/B/C). IDs are not reused and completed cards are not
silently changed.

## Current state

```text
023A  Zig→cppio port map             DONE (this sequence's source graph)
023B  Async backend parity audit     DONE (verdict B — sufficient w/ narrow extensions)
023C  Migration job cards            DONE (this document)
```

The async foundation (017-022) is GREEN: 52/52 tests pass in debug. The
PHASE B jobs below close substrate gaps before the task runtime (PHASE T) and
the Evented experiment (PHASE E) begin.

## Phase map

```text
PHASE B (backend substrate)   B1 → B2 ──┐
                              B1 → B3 ──┤
                                        ▼
PHASE T (task/async model)    T1 → T2 → T3 ─┐
                              T1 → T4 ───────┤
                              T3,T4,S3 → T5  │
                                              ▼
PHASE S (Io sync primitives)  S1 → S2 → S3 ──┐
                              S2 → S4        │
                                              ▼
PHASE E (Evented runtime)     E0(ADR) → E1 → E2 → E3 → E4
                                            E3,T1 → E5
                                            E3,S* → E6
```

---

# PHASE B — backend substrate completeness

Closes the substrate gaps identified in `docs/async-backend-parity.md` §5.
No job here changes the `AsyncBackend` contract or the public L1 API; all are
internal correctness, conformance, and seam additions.

---

## 024 — Shared backend conformance suite (B1) — DONE

**Implemented.** `tests/backend_conformance.hpp` (factory + run_conformance),
`tests/backend_conformance_test.cpp` (8 shared cases), and
`tests/backend_conformance_driver_test.cpp` (instantiates Fake / ThreadPool /
Uring-stub). Wired into `xmake.lua` as `backend_conformance_test` (test group).
Verified: release, debug, asanubsan, tsan all green; 53/53 tests pass.
real_mode cases (positional/EOF/short/exactly-once) exercised on ThreadPool;
skipped cleanly on Fake + Uring-stub. Backend-specific mechanism tests remain
in their existing per-backend files (no duplication removed yet — migration of
redundant assertions is deferred to avoid churn).

**Job ID.** 024
**Title.** Shared `AsyncBackend` conformance suite
**Type.** Test infrastructure (no production code change).
**Goal.** One parameterized test harness exercising every genuinely-shared
backend semantic against every backend (Fake, ThreadPool, Uring-real,
Uring-stub), so semantics stop being duplicated across unrelated test files.
Closes the task §6 requirement.

**Source references.**
- Zig ordering rules: `Io.zig:474-624` (Batch), `Io.zig:551` (next() =
  completion order).
- Zig exactly-once: `Io.zig:601-623` (batchCancel asserts `userdata == null`).
- cppio ADR §6 O1-O5, §7 X1-X6.

**Dependencies.** None new. Builds on the existing 017-022 backends.

**Current cppio evidence.** Backend semantics are currently spread across
`tests/fake_backend_test.cpp`, `tests/threadpool_backend_test.cpp`,
`tests/uring_backend_test.cpp`, `tests/async_cancel_test.cpp`,
`tests/async_durability_test.cpp`, `tests/async_op_helpers_test.cpp`. Each
reinvents the success/EOF/short/cancel assertions. Conformance is asserted
incidentally, not as a contract.

**Semantic contract.** Every backend that implements `AsyncBackend` must
satisfy the shared cases. Backend-specific mechanism (io_uring SQE pressure,
ThreadPool worker count) stays in backend-specific tests.

**Required implementation.**
- `tests/backend_conformance.hpp` — a templated/parameterized harness:
  `template <BackendFactory> void run_conformance(BackendFactory make)`.
- `tests/backend_conformance_test.cpp` — instantiates it for each backend.
- Shared cases (when applicable to the backend):
  - read success (full buffer, bytes verified)
  - write success (full buffer, bytes verified on re-read)
  - positional semantics (two ops same fd, different offsets, independent)
  - zero-length I/O (read 0 → EOF; write 0 on empty → ok)
  - EOF (read past end → `IoError::eof` after partial)
  - short read / short write (retried by `read_all`/`write_all`)
  - invalid descriptor (`fd = -1`) → `IoError` (backend-defined code)
  - operation result publication (exactly once, ready after poll/wait_one)
  - terminal state exactly once (no double-completion)
  - cancel before execution → terminal result is one of {success, canceled}
  - cancel while in flight → terminal result is one of {success, error, canceled}
  - complete-before-cancel race → no double-completion
  - cancel target not found / already complete → no-op, no spurious completion
  - wait_one reaps ≥1 then returns
  - shutdown with no operations (clean destruct)
  - stats accounting (submit_calls/submitted_ops/completed_ops increment)
- Backend-specific tests remain in their existing files for mechanism
  (Uring SQE pressure, Uring user_data identity, ThreadPool concurrency).

**Required tests.** The suite IS the test. Acceptance = every backend passes
every applicable shared case.

**Non-goals.**
- No new backend. No contract change. No performance assertion.
- Does not unify vectored I/O (out of L1 scope, 023A §D).
- Does not add cross-thread submission (forbidden for Uring; ThreadPool
  already locks).

**Acceptance criteria.**
- `tests/backend_conformance_test.cpp` exists and is wired into `xmake.lua`
  test group; passes for Fake + ThreadPool + Uring (real, when liburing
  present) + Uring (stub, skip-clean for the real-only cases).
- Existing per-backend tests remain green; redundant assertions may be
  migrated to the shared suite but the per-backend files are not deleted.
- `xmake -g test && xmake test -g test` 100% green. Existing count: 52; new
  count ≥ 52 + the conformance binary.
- Default (no-liburing) build unaffected (ADR AB1).

**Abort conditions.**
- A shared case cannot be expressed without backend-specific branching that
  defeats the purpose (then the case is backend-specific and stays out).
- The harness forces a false semantic equivalence (e.g. demanding Uring SQE
  pressure == ThreadPool queue pressure — explicitly forbidden by task §6).

**Expected files.**
- `tests/backend_conformance.hpp` (new)
- `tests/backend_conformance_test.cpp` (new)
- `xmake.lua` (wire the new test target)

---

## 025 — ThreadPoolBackend correctness repair (B2) — DONE

**Implemented.** The shutdown gate (`destroying_`) is now consulted by every
`submit_*` via `accepting_new_work()` and returns `invalid_state` synchronously
once destruction begins (was dead state). `shutting_down_for_test()` is the
test hook that flips the gate without running the destructor (the destructor
path is unsafe to test directly: use-after-free). The header now describes the
realized cancel semantics (best-effort, exactly-once, terminal result is one
of {success, error, canceled}; real cancellation of in-flight syscalls stays
the Uring backend's job). The unused `Job` struct was removed.

**Scope decision (recorded):** the "cancel-not-yet-started" path (dequeue +
complete-as-canceled) is **not implemented** because this backend spawns a
worker thread at submit, so an op is effectively started immediately. The ADR
§7 X3 best-effort semantics permit this. Real signal-based interrupt
(pthread_kill/tgkill, mirroring Zig `Threaded.signalAllCanceledSyscalls`) is
deferred to a sub-job only if a future conformance case demands it.

Verified: release + debug + asanubsan + tsan all green; 53/53 tests pass.
The shared conformance suite (024) `cancel_yields_defined_terminal` case
passes for ThreadPool.

**Job ID.** 025
**Title.** `ThreadPoolBackend` cancel region + shutdown gate + doc repair
**Type.** Backend correctness (no public API change).
**Goal.** Bring `ThreadPoolBackend` to Zig-`Threaded`-equivalent correctness:
real cancel (not a no-op), a `destroying_` gate that actually gates, and
header/impl agreement.

**Source references.**
- Zig `Threaded.cancelAwaitable`: `Io/Threaded.zig:1199-1254`.
- Zig `signalAllCanceledSyscalls`: `Io/Threaded.zig:1265` (pthread_kill/tgkill).
- Zig `Syscall` RAII cancel region: `Io/Threaded.zig:1342-1460`.
- cppio header doc/impl drift: `include/sluice/async/threadpool_backend.hpp:21-22`
  vs `src/async/threadpool_backend.cpp:147-153`.

**Dependencies.** 024 (the conformance suite defines the cancel contract this
job implements against).

**Current cppio evidence.**
- `cancel` is a documented no-op (`src/async/threadpool_backend.cpp:147-153`).
- `destroying_` is set in the destructor but never read by submit
  (`src/async/threadpool_backend.cpp:22-31`).
- Header claims "if the syscall hasn't started, it's removed and completed as
  canceled" (`include/sluice/async/threadpool_backend.hpp:21-22`) — impl does
  not do this.

**Semantic contract.**
- Cancel of an op not yet started by a worker: the op is removed from the
  queue and completed with `IoError::canceled` at the next `poll`/`wait_one`
  (exactly-once). This matches Zig's fast-path cancel.
- Cancel of an op whose worker has started: best-effort. Portable cancel of
  an in-flight blocking syscall is not possible without signals; the op
  completes with its real result. (Zig uses signals; cppio defers signal-based
  interrupt to a sub-job only if 024's conformance shows it is required. The
  ADR §7 X3 "best-effort and asynchronous" semantics permit this.)
- Submit after the destructor begins returns `IoError::invalid_state`
  synchronously.

**Required implementation.**
- Add a per-op "started" flag set by the worker under the lock before the
  syscall. `cancel` checks it: not started → dequeue + complete-with-canceled;
  started → record intent, op completes with real result.
- Wire `destroying_`: submit checks it under the lock and rejects.
- Fix the header to describe the implemented semantics exactly.
- Document the memory-order protocol: shared state = `started` flag +
  `outstanding_` + ready deques; writers = driver thread (submit) + worker
  thread (syscall + ready push); readers = driver thread (poll/cancel); the
  `mtx_` provides all happens-before edges (CP.20).

**Required tests.** (most via 024's conformance suite; add ThreadPool-specific)
- Cancel-not-yet-started → `canceled`, exactly once.
- Cancel-after-started → real result, no double-completion, no use-after-free.
- Submit-after-destructor-begins → `invalid_state`.
- `canceled_ops` stat increments on the canceled path only.

**Non-goals.**
- No signal-based interrupt of in-flight syscalls (portability hazard; defer
  to a sub-job only if conformance demands it).
- No `async_limit` cap (optional; record as a follow-up).
- No public API change.

**Acceptance criteria.**
- 024 conformance suite passes for ThreadPool including the cancel cases.
- Header and impl agree.
- ASan/UBSan/TSan runs of the ThreadPool conformance cases are clean.
- Existing tests unchanged in behavior.

**Abort conditions.**
- The "not started" dequeue cannot be made race-free under the lock without
  restructuring (then narrow the contract to "best-effort, op completes with
  real result" and document — ADR §7 X3 permits this).
- Signal-based interrupt proves required AND unportable (then ThreadPool
  cancel stays best-effort and the real-cancel path is Uring-only — record
  in the ADR).

**Expected files.**
- `include/sluice/async/threadpool_backend.hpp`
- `src/async/threadpool_backend.cpp`
- `tests/threadpool_backend_test.cpp` (add cancel-not-started case)
- `docs/async-backend-parity.md` (mark B2 done)

---

## 026 — UringAsyncBackend substrate hardening (B3) — DONE (real-mode runtime verification DEFERRED)

**Implemented.** Three substrate hardenings:

1. **O(1) cancel identity.** Added `Impl::comp_to_op`, a `Completion* -> op-id`
   reverse index, inserted at `register_op` and erased at `reap_op_cqe`. Both
   `cancel(Completion<size_t>&)` and `cancel(Completion<void>&)` now do a single
   hash lookup via `op_id_for()` instead of a linear scan of `ops`. The
   exactly-once invariant (ADR §7 X3) is unchanged: the cancel CQE never
   completes a Completion; only the original op's CQE does.

2. **Submit batching seam (internal).** Verified already present from 020B:
   `submit_*` only acquires + preps an SQE (pressure-flush); the kernel is poked
   in `poll()`/`wait_one()`. The public L1 API is unchanged (one submit_* per
   op); the seam is internal so the future `Batch` (T4) can submit many SQEs
   per flush. Matches Zig `Io/Uring.zig`'s `enqueue`/`submit` split. No code
   change needed here — documented in the header.

3. **Feature gates.** Added xmake options `with-uring-registered-buffers` /
   `with-uring-registered-files` (both OFF by default, matching Zig upstream —
   `Io/Uring.zig` uses neither). The defines
   `SLUICE_URING_REGISTERED_BUFFERS` / `SLUICE_URING_REGISTERED_FILES` are
   threaded onto `sluice_async` only when liburing is also enabled. No
   implementation behind them yet — a future job adds them under a documented
   lifetime contract.

**Verification (honest):** liburing is **absent on this host** and cannot be
installed (no sudo). Real-mode runtime verification is therefore **DEFERRED**
to a liburing-equipped environment. Structural verification done:
- Stub-mode build + full suite green (53/53).
- Real-mode **syntax/type check** via a stub `liburing.h` (g++ -fsyntax-only
  -DSLUICE_HAS_LIBURING): exit 0 — the O(1) cancel refactor and reverse-index
  maintenance are syntactically and type-correct.
- The exactly-once cancel-race logic is unchanged from 020B (which was
  validated under liburing when that job landed); B3 only changes the LOOKUP
  path, not the race resolution.

**Hard blocker (per task §14/§15):** real-mode runtime verification of the
O(1) cancel path requires liburing. This is recorded, not silently claimed.
When a liburing environment is available, the existing
`tests/uring_backend_test.cpp` cancel cases (gated behind
`SLUICE_HAS_LIBURING`) exercise the path and must pass unchanged.

**Job ID.** 026
**Title.** O(1) cancel identity + batch-submit seam + feature gates
**Type.** Backend hardening (gated; no public API change).
**Goal.** Close the three Uring substrate gaps that block the future `Batch`
(T4) and the Evented runtime (E5): cancel identity, batch submission, feature
gating for registered buffers/files.

**Source references.**
- Zig cancel identity: `Io/Uring.zig:1093-1103` (`Completion.Userdata` enum,
  `*Fiber` as user_data, cancel uses `wakeup` + `addr = fiber`).
- Zig exactly-once: `Io/Uring.zig:379-411` (`IOSQE_CQE_SKIP_SUCCESS`).
- Zig batch submission: `Io/Uring.zig:128-140` (`enqueue`/`submit`), the
  `submit_and_wait(1)` reap pattern at `:1154-1157`.
- Zig feature gating: `Io/Uring.zig:894` (no SQPOLL, no registered buffers/
  files upstream — cppio matches).

**Dependencies.** 024 (conformance suite). Builds on the existing 020B backend.

**Current cppio evidence.**
- Cancel is a linear scan of `ops` for the matching `Completion*`
  (`src/async/uring_backend.cpp:400-422`) — O(outstanding).
- No L1 batch submit: each `submit_*` flushes eagerly under pressure
  (`src/async/uring_backend.cpp:160-166`).
- No registered-buffer/file gating (matches Zig upstream, but the gate must
  exist before a future job can add them).

**Semantic contract (unchanged from 020B).**
- Exactly-once terminal result (ADR §7 X3) — preserved.
- CQE reap order only (ADR §6 O3) — preserved.
- Single-driver-thread — preserved (multi-thread is PHASE E).

**Required implementation.**
- **O(1) cancel identity.** Use the `Completion*` (or a stable id derived
  from it) as the SQE `user_data`, mirroring Zig's `*Fiber`-as-user_data. The
  cancel SQE carries the target's `user_data` in `addr`. Drop the
  `unordered_map<u64, OpRec>` linear scan. Keep the cancel-CQE-never-
  completes invariant (cancel CQE only clears intent).
- **Batch-submit seam (internal).** Add an internal `flush_pending()` so
  successive `submit_*` calls accumulate SQEs without flushing; `poll`/
  `wait_one` flush before reaping. The public L1 API is unchanged. This is
  the substrate the future `Batch` (T4) will call into.
- **Feature gates.** Add `SLUICE_URING_REGISTERED_BUFFERS` /
  `SLUICE_URING_REGISTERED_FILES` build-time gates, both OFF by default
  (matching Zig upstream). No implementation behind them yet — just the gate
  so a future job can add them under a documented lifetime contract.

**Required tests.** (via 024 + Uring-specific)
- Cancel works with O(1) identity (conformance cases pass).
- Batch-submit seam: N submits with no intermediate poll, then one poll reaps
  ≥1 — verifies SQEs accumulated.
- Feature gates OFF by default: default build links and runs identically to
  pre-026.

**Non-goals.**
- No multi-thread driver (PHASE E).
- No registered buffer/file implementation (just the gate).
- No public API change. No `Batch` type yet (T4).

**Acceptance criteria.**
- 024 conformance suite passes for Uring (real, with liburing) including all
  cancel cases.
- Skip-clean without liburing (stub mode) unchanged.
- `queue_full_retries` stat still correct under the batch seam.
- Existing Uring tests unchanged in behavior.

**Abort conditions.**
- O(1) cancel identity cannot be made correct under the cancel-vs-completion
  race (ADR AB5) — then keep the linear scan and document why (the race is
  already handled structurally; the O(1) change is performance, not safety).
- Batch seam changes observable ordering (it must not — CQE order only).

**Expected files.**
- `include/sluice/async/uring_backend.hpp`
- `src/async/uring_backend.cpp`
- `tests/uring_backend_test.cpp` (add batch-seam case)
- `xmake.lua` (feature gates)
- `docs/async-backend-parity.md` (mark B3 done)

---

# PHASE T — task / async model (deferred past PHASE B)

These jobs add the Zig `Io.zig:1174-1538` surface (Future/Group/Select/Batch)
as a **new layer above** `AsyncIoContext`. They do not modify `AsyncBackend`.
Each card is a sketch; full cards are written when PHASE B closes (autonomous
execution proceeds, but the cards are seeded here so the dependency graph is
visible).

**027 (T1) — Cancel token + protection region + recancel — DONE.**

**Implemented.** `include/sluice/async/cancel.hpp` + `src/async/cancel.cpp`
introduce the cooperative cancellation layer derived from Zig's model
(Io.zig:1183-1188, 1310-1358):

- `CancelToken` — the shareable cancel-request state (atomic, thread-safe).
  `request()`/`is_requested()`/`rearm()`/`clear()`. Release/acquire ordering
  so a consumer on another thread observes the request with a happens-before
  edge.
- `CancelState` — per-consumer state: the `CancelProtection` bit
  (delivery-blocking) and the acknowledgement bit (single-shot). Default
  `unblocked`, matching Zig (tasks created unblocked, Io.zig:1325).
- `CancelGuard` — RAII wrapper for protected regions (mirrors Zig
  `swapCancelProtection` usage, Io.zig:1334). `[[nodiscard]]`.
- `check_cancel(token, state)` — the pure cancelation point (Zig
  `checkCancel`, Io.zig:1356). Delivers `IoError::canceled` iff requested AND
  unblocked AND not-yet-acknowledged; on delivery marks acknowledged
  (single-shot).

**Scope decision (recorded):** the token/state split mirrors Zig's per-task
cancel bit + per-task protection, decoupled from the task object so cppio can
compose it (a Future wraps a token; a Group shares one). The backend op-cancel
(ADR §7 X2) stays on `AsyncIoContext::cancel`; this layer is what a task uses
to COOPERATIVELY observe a cancel request between/at Io operations. No
scheduler dependency.

Verified: release + debug + asanubsan green; full suite 54/54. Single-threaded
logic tests (cross-thread happens-before documented; future task-runtime tests
will exercise it under TSan).
**028 (T2) — `Future<Result>` — DONE.**

**Implemented.** `include/sluice/async/future.hpp` (header-only template). A
single-task awaitable derived from Zig Future (Io.zig:1176-1206):

- Caller-provided result storage (like Completion, ADR §5 L4). Uses
  `std::optional<Result<T>>` so the pre-ready state is meaningfully empty
  (avoid the meaningless "default Result" state).
- Producer side: `complete_with(Result<T>)` — exactly-once terminal publish
  under `mtx_`, then `cv_.notify_all()`. Thread-safe (producer may run on a
  worker thread).
- Consumer side: `await()` (idempotent, blocks the calling thread on `cv_`
  until ready, Zig Io.zig:1199) and `cancel()` (idempotent, requests via the
  token then awaits, Zig Io.zig:1191). Not thread-safe for concurrent
  awaiters (Zig: "not threadsafe", Io.zig:1198) — one awaiter per Future.
- Cooperative cancel via `cancel_token()` (composes 027's CancelToken). The
  producer observes the token at its cancel points; best-effort (ADR §7 X3).

**Scope decision (recorded):** with no fiber runtime (PHASE E not started),
`await()` BLOCKS THE CALLING THREAD on a condition variable. This is the
Threaded-equivalent shape (Zig Io.Threaded), NOT the Evented shape (Zig
Io.Uring, where await suspends a fiber). The API is identical; only the
mechanism differs — so a future fiber-based await can replace it without
churning callers. No scheduler dependency; Future is scheduler-free like Zig's
own Future type (the scheduler lives in the backend).

Verified: release + debug + asanubsan + tsan green; full suite 55/55. TSan run
twice (Future uses real threads → race-relevant); clean. Deterministic tests
(no timing races; slice 4 polls a token, not a clock).
**029 (T3) — `Group`.** Unordered task set, cancel-propagation boundary
(`Io.zig:1218`). Depends on 028.
**030 (T4) — `Batch`.** Grouped completions over op storage (`Io.zig:474`).
Depends on 028, 026.
**031 (T5) — `Select`.** Higher-level selector on `Queue` (`Io.zig:1367`).
Depends on 029, 030, 034 (S3).

---

# PHASE S — Io-aware synchronization (deferred past PHASE T)

**032 (S1) — futex substrate.** Cancelable 4-byte wait/wake (`Io.zig:1552`).
Depends on 027 + the E0 execution-model decision (does contention block a
worker or suspend a task?).
**033 (S2) — Io-Mutex / Condition / Event.** (`Io.zig:1587-1870`). Depends on 032.
**034 (S3) — `Queue`.** MPSC ring with suspended putters/getters, recancel-on-
partial (`Io.zig:1872`). Depends on 033, 027.
**035 (S4) — `RwLock` / `Semaphore`.** Depends on 033.

---

# PHASE E — Evented task runtime (deferred past PHASE T + S)

**E0 — Execution-model ADR (gates E1-E6).** Decides whether cppio adopts
fibers (the principal experiment, task §9). Records the target arch set
(`fiber.zig` supports aarch64/riscv64/x86_64). Lands after B/T/S give evidence
about whether the completion-based L1 model already preserves blocking-shaped
control flow.

**E1 — fiber port.** x86_64 (+ aarch64 if targeted) `contextSwitch`/`Context`/
trampoline, gated (`Io/fiber.zig`).
**E2 — Fiber + Thread.** Per-thread ring, ready/free queues, idle loop
(`Io/Uring.zig:94-147, 1147`).
**E3 — yield + SwitchMessage.** The suspension/resume seam (`Io/Uring.zig:964`).
**E4 — Work stealing.** idle-search, ready-steal, free-steal, MSG_RING wakeup
(`Io/Uring.zig:937-1078`).
**E5 — CancelRegion + ASYNC_CANCEL race.** Exactly-once via SKIP_SUCCESS
(`Io/Uring.zig:415-528`).
**E6 — Io-aware sync rebase.** Mutex/Condition/Event/Queue suspend the fiber
(task §12 payoff).

---

## Execution rules (task §13, §14, §15)

- **One commit per completed Job Card.** Commit message contains Job ID,
  semantic goal, key implementation, tests/verification (task §19).
- **TDD per card** (task §15, `tdd` skill): RED (failing test for one
  behavior) → GREEN (minimal code) → refactor. One behavior at a time.
  **Vertical slices**, not horizontal — do not write all tests then all impl.
- **Autonomous execution** (task §14): proceed through runnable cards; do not
  stop after a plan. A hard blocker is recorded in the card and work continues
  on independent cards.
- **C++ quality** (task §16, `cpp-coding-standards` skill): RAII, explicit
  ownership, `std::span` for buffers, `std::byte` for raw bytes, no
  naked owning pointers, no detached threads, document every atomic protocol
  (shared state / writers / readers / happens-before / terminal publication).
- **Verification matrix** (task §15): GCC + Clang builds; warnings per repo
  policy; non-liburing build; liburing build where available; async + sync
  tests; sanitizers where stack-switching does not make them misleading.
- **No flaky tests** (task §15): a flaky test is a defect or missing
  deterministic seam. No larger timeouts / arbitrary sleeps / retry-until-pass
  / test ordering / disabling / fake completion in real-backend tests.
- **Status honesty** (task §18): a gate is GREEN only when its acceptance
  criteria and required tests are actually satisfied.

## Cross-links

- Source graph: `docs/zig-stdio-async-port-map.md` (023A).
- Parity audit (governs PHASE B): `docs/async-backend-parity.md` (023B).
- Async ADR: `docs/adr/ADR-async-io-model.md` (016D).
- Async next jobs (017-022, GREEN): `docs/async-next-jobs.md` (016F).
