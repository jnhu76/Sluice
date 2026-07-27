# Async I/O — next job cards

**Status: sluice-CORE-016F. ALL JOBS COMPLETE on `feat/async-runtime`.** Splits
the async implementation (designed in 016A–016E, decided in the ADR
`docs/adr/ADR-async-io-model.md`) into small, independently abortable jobs. All
jobs below have been implemented and are green (52/52 tests pass).

## GREEN: all jobs complete

```text
017  Completion<T> + AsyncIoContext + AsyncStats          DONE
019  FakeAsyncBackend (deterministic test vehicle)        DONE
018  Read/Write op model + "all" helpers                  DONE
018B SyncDataOp/SyncAllOp (overlapped durability)         DONE
020A ThreadPoolBackend (portable fallback)                DONE
021  Cancellation spike                                   DONE
020B UringAsyncBackend (gated, validated separately)      DONE
022  Async bench harness                                  DONE
```

> **The async DESIGN (016A–016F, ADR 016D) remains ACCEPTED and is not changed
> by this notice.** All async implementation jobs are now complete on
> `feat/async-runtime`.

The sync-first gate (`docs/sync-before-async-readiness-gate.md`) is **GREEN**.
The async readiness gate (`docs/async-readiness-gate.md`) is **GREEN**.

Once the gate is green, the async jobs proceed in the order below, and async
bench (job 022) MUST compare against the engineered concurrent W1–W4 blocking
rows from 022S/023S — not the sequential rows (sync-first gate item 7).

## Ordering (revised after the 016 review patch)

```text
016 (design, accepted/frozen) ──> [sync-first gate: 017S..023S GREEN]
                                       │
                                       v (unblocked)
016 (this job, design) ─┬─> 017  Completion core (L0/L1 skeleton + AsyncStats)
                        │           │
                        │           v
                        │      019  FakeAsyncBackend  (deterministic test vehicle)
                        │           │
                        │           v
                        │      018  Read/Write op model (positional, P1) + "all" helpers
                        │           │
                        │           v
                        │      018B SyncDataOp/SyncAllOp (overlapped durability, W4)
                        │           │
                        │           v
                        │      020A ThreadPoolBackend (portable fallback)
                        │           │
                        │           v
                        │      021  Cancellation spike
                        │           │
                        │           v
                        │      020B UringAsyncBackend (gated, validated separately)
                        │           │
                        │           v
                        └─────> 022 Async bench harness (vs engineered blocking W1-W4)
```

Why this order:

- **019 before 018** — the op model's derived `read_all`/`write_all` need a
  backend that injects short completions, EOF, errors, and ordering. 016F itself
  calls 019 "the primary unit-test vehicle for all later async work"; landing it
  first makes 018 testable.
- **018B before any real backend** — durability (W4) is a first-class workload
  in 016B; without `SyncDataOp`/`SyncAllOp` the op set is incomplete and W4 is
  unexpressible.
- **021 before 020B** — the io_uring backend's abort conditions mention cancel
  races, but 020B's non-goals exclude building cancellation. Landing cancellation
  (021) first avoids a doc/self-contradiction: cancel semantics exist before the
  backend that needs them.
- **020A (thread pool) before 020B (uring)** — gives a portable, always-buildable
  real backend to compare against before the gated uring one.

A job may only start when its predecessors' acceptance criteria are met and the
readiness gate (`docs/async-readiness-gate.md`, 016E) items it depends on are
closed.

Conventions for every card:

- **Default backend unchanged.** No job touches `BlockingIoContext` or
  `Reader`/`Writer` semantics.
- **Tests-green floor.** Every job keeps the existing blocking tests green (count
  per 016E §3).
- **No new dependency** unless the ADR (§11) already evaluated it (only liburing,
  job 020B, optional).
- **No universal performance claim.** Bench jobs produce workload-specific
  evidence only.

---

## 017 — Minimal completion queue abstraction

**Goal.** Land the foundation types from ADR §3/§4 with **no backend** beyond a
purely abstract `AsyncBackend` interface and an in-process (non-threaded) default
that completes ops synchronously at `poll` time only. This is the
`Completion<T>` + `AsyncIoContext` + `AsyncBackend` skeleton, plus `AsyncStats`
(ADR §10b). Reaping is `poll()` / `wait_one()` — no `drain`/deadline (ADR §3).

**Non-goals.**
- No io_uring, no threads, no thread pool.
- No coroutines.
- No real file I/O yet (ops carry buffers but no fd is touched).
- No cancellation (deferred to 021 — `cancel` may be a no-op returning "not
  supported" here).
- No bench harness (022).

**Required tests.**
- `Completion<T>` lifecycle: ready/not-ready, move semantics, result access;
  **L7–L11**: outstanding-when-destroyed/moved is caught, `result()` before
  ready is `invalid_state` (ADR §5 L7–L11).
- `submit_*` returns `Result<void>`; records an op as outstanding without
  completing it; submit-into-non-ready Completion returns `invalid_state` (L8).
- `poll()` completes outstanding ops in the order the (trivial) backend reports
  them; `wait_one()` blocks until ≥1 ready (ADR O1).
- Submit-time errors return synchronously from `submit_*` (ADR E5).
- Completion errors surface via `Completion::result()` (ADR E2).
- `AsyncStats` counters increment correctly (submit/poll/wait/completed).
- Buffer-lifetime contract (L1–L3c): an outstanding op keeps a handle to its
  buffer; the test documents (and asserts where feasible) that misusing the
  buffer before completion is a contract violation (gate item 1).

**Acceptance criteria.**
- Public types compile and link in `sluice::async` with no new dependency.
- All required tests pass in debug + release.
- `xmake build sluice_core`, `xmake build -g test`, `xmake test` all green;
  existing blocking tests unchanged.
- No header under `include/sluice/` (blocking side) is modified in behavior.

**Abort conditions.**
- The abstraction cannot be expressed without a global scheduler (gate item 6).
- `Completion<T>` cannot be made caller-owned without forcing heap allocation.
- Buffer-lifetime / Completion-lifecycle rules cannot be stated/testably enforced
  (gate item 1; ADR §5 L7–L11).
- Any blocking test changes.

---

## 019 — Fake async backend for deterministic tests

**Goal.** A `FakeAsyncBackend` that produces completions on demand, enabling
fully deterministic async tests with no kernel and no threads (workload W5; ADR
T1). This is the primary unit-test vehicle for all later async work and the thing
that makes gate item 1 (buffer lifetime) genuinely testable. Lands BEFORE 018 so
the op model is testable.

**Non-goals.**
- No real I/O. No threads. No io_uring.
- No performance characterization (it is a test tool, not a benchmark path).
- No cancellation timing beyond what 021 needs to hook later.

**Required tests.**
- Ops submitted are held outstanding across `poll()` calls until the test
  explicitly completes them (deterministic timing).
- Completion order is controllable by the test (submit order by default;
  arbitrary order on demand) — exercises ADR O1–O3.
- "Buffer in-use while outstanding; reusable after completion" contract test
  (closes gate item 1's [IMPL] half).
- Error injection: a submitted op can be completed with any `IoError` (eof,
  no_space, backend_error, canceled) to exercise ADR E2/E3.
- Short-completion injection (complete with fewer bytes than requested) to
  exercise 018's retry logic.

**Acceptance criteria.**
- Every async unit test in 017 (and later 018/018B) can be (re)written against
  FakeAsyncBackend with zero real I/O and no flakiness.
- Gate item 1 [IMPL] closed: buffer-lifetime contract is asserted by a test.
- Blocking tests unchanged.

**Abort conditions.**
- The fake cannot express the ordering/error/timing hooks later jobs need.
- The fake requires threads or kernel state (defeating its purpose).

---

## 018 — Async read/write operation model

**Goal.** Define `ReadOp`/`WriteOp` descriptors (positional by default — ADR §6
P1) and the derived "all" helpers (loop-until-complete over completions),
mirroring the blocking `read_exact`/`write_all` factoring (ADR §6 O5). Builds on
017's completion core and is tested against 019's fake.

**Non-goals.**
- No real backend (uses the fake from 019).
- No io_uring, no threads.
- No cancellation semantics beyond 017's (full model is 021).
- No vector/gather async yet (single-buffer ops only).
- No durability ops (those are 018B).

**Required tests.**
- `ReadOp`/`WriteOp` construction, buffer binding, and EXPLICIT offset (P1).
- Positional independence: two ops on the same fd at different offsets complete
  independently (no implicit-cursor coupling).
- Derived "read all"/"write all" complete exactly when all bytes transfer or an
  error occurs; short completions are retried (mirrors `write_all`/`read_exact`).
- EOF surfaces as `IoError::eof` after partial progress (mirrors blocking E4).
- Zero-progress on non-empty input is `invalid_state` (mirrors `write_some`).
- Partial-progress error propagates immediately (ADR E4).

**Acceptance criteria.**
- Async read/write ops share error vocabulary and partial-progress semantics
  with blocking Reader/Writer (no drift); ops are positional (P1).
- All required tests pass in debug + release.
- Blocking tests unchanged.

**Abort conditions.**
- Async op semantics cannot match blocking partial-progress/EOF rules without
  duplicating (rather than reusing) the logic.
- Positional I/O (P1) cannot be enforced, leaving same-fd async ops racing on an
  implicit cursor (ADR AB10).
- The op model forces a backend coupling the ADR forbids (ADR §4).

---

## 018B — Async durability operation model

**Goal.** Define `SyncDataOp` (fdatasync) and `SyncAllOp` (fsync) and the
overlapped-durability semantics for W4 (016B): fsync/fdatasync overlapped with
the *next* batch of writes. Today sync is a blocking tail on each writer; this
job makes durability an async op that can run concurrently with other ops. Builds
on 017/019/018.

**Non-goals.**
- No group commit / multi-stream atomic commit (016B O5).
- No durability *guarantee* beyond what the kernel gives fsync/fdatasync (a
  completed sync Completion means the kernel acknowledged the sync, same
  semantics as blocking `SyncableWriter`).
- No real backend beyond the fake.

**Required tests (against FakeAsyncBackend).**
- `SyncDataOp`/`SyncAllOp` construction and submit; Completion<void> result.
- Overlap: a sync op can be outstanding concurrently with write ops on the same
  fd without forcing serialization at submit time.
- Ordering composition (ADR §6 P3): to durably persist writes A, the caller
  awaits A's Completions, THEN submits the sync — the fake verifies this sequence
  is expressible and that submitting sync before writes complete does NOT imply
  those writes are durable.
- Sync error injection (EIO / backend_error) surfaces via the Completion.
- Sync stats counted in `AsyncStats` (or a dedicated counter).

**Acceptance criteria.**
- W4 (overlapped durability) is expressible end-to-end on the fake.
- Durability ops share the IoError vocabulary with blocking `sync_data`/
  `sync_all` (no drift).
- Blocking tests unchanged.

**Abort conditions.**
- Overlapped durability cannot be expressed without implying a false durability
  guarantee (i.e. it would let callers believe unsynced writes are durable).
- Sync semantics diverge from blocking `SyncableWriter` (ADR §9a).

---

## 020A — ThreadPool async backend (portable fallback)

**Goal.** A `ThreadPoolBackend` that implements `AsyncBackend` by running the
existing blocking `read_some`/`write_some`/`sync_*` on worker threads. The
portable, always-buildable real backend — the fallback where io_uring is absent
(ADR §4; 016C Option 5). Reuses `Result<T>`/`IoError` verbatim.

**Non-goals.**
- Not a true event loop; it spends a worker thread per outstanding op. It is the
  *fallback*, not the high-concurrency path.
- No cancellation of an in-flight blocking syscall (portable cancel is deferred;
  this backend's cancel is "don't start" or "already running — best effort").
- No io_uring. No universal performance claim.

**Required tests.**
- Submit N independent writes/reads; reap N completions via `poll`/`wait_one`;
  verify bytes and results.
- Short writes retried via 018's derived helper.
- Sync ops (018B) complete and report errors correctly.
- Buffer lifetime (L1–L3c) holds under real concurrency (sanitizer-checked).
- `AsyncStats` increments correctly on the thread-pool path.

**Acceptance criteria.**
- Backend builds with no new dependency (std::thread only — ADR §11 D4).
- Provides the ThreadPool row that 022's bench compares.
- Blocking tests unchanged.

**Abort conditions.**
- Cannot keep buffer lifetime / Completion lifecycle (L7–L11) correct under real
  threads.
- The thread pool requires a global scheduler (gate item 6).

---

## 021 — Cancellation spike

**Goal.** Flesh out ADR §7 from the minimal "exactly-once terminal result" model
toward structured cancellation: a cancel token/object, cancel-during-outstanding
semantics, and (for io_uring) `IORING_OP_ASYNC_CANCEL` race handling. Lands
BEFORE 020B so cancel semantics exist before the backend that needs them.

**Non-goals.**
- No Zig-style cancel-protection regions yet (ADR §14 deferred) unless the spike
  shows they are cheap — record and defer otherwise.
- No group/batch cancel unless trivially derivable.
- Not a performance goal; correctness and defined semantics only.

**Required tests (against FakeAsyncBackend; io_uring variants gated, in 020B).**
- `cancel` before completion: op completes with `IoError::canceled` (or a real
  result if it had already finished — exactly-once, ADR X3).
- `cancel` after completion: no-op, no double-completion.
- `cancel` during a long op (fake-controlled timing): terminal result is one of
  {success, error, canceled}; buffer remains safe until ready (ADR X5).

**Acceptance criteria.**
- Cancellation semantics are defined, tested, and documented as an extension of
  ADR §7 (update the ADR or add a sub-ADR).
- Gate item 2 fully closed (beyond the design-level minimum).
- `canceled_ops` counter in `AsyncStats` increments correctly.
- Blocking tests unchanged.

**Abort conditions.**
- Exactly-once terminal result cannot be guaranteed (ADR AB5).
- io_uring cancel races cannot be made correct without registered-buffer
  machinery not yet built — then io_uring cancel is feature-gated off and the
  fake/thread-pool cancel model stands alone.

---

## 020B — io_uring async backend prototype

**Goal.** A `UringAsyncBackend` behind `SLUICE_HAS_LIBURING` that submits many
SQEs and reaps CQEs in `poll`/`wait_one` (ADR §4) — the high-concurrency real
backend, and the production path candidate. Measured against blocking and against
the 013 synchronous-over-uring spike. Lands AFTER 021 so cancel-race handling
exists first.

**Non-goals.**
- Not the default backend. Not plugged into `BlockingIoContext`.
- No registered buffers/files yet (plain SQE buffers; lifetime contract first —
  ADR §5, §14).
- No networking/timers/mmap.
- No universal performance claim (workload-specific evidence only — ADR R6).
- Does NOT promise per-fd write completion FIFO (ADR O3): CQE reap order only.

**Required tests (build-gated; skip clean without liburing, like 013).**
- Submit N independent writes; reap N completions via `poll`/`wait_one`; verify
  bytes. Ordering asserted only as "CQE reap order" (O3), NOT per-fd FIFO.
- Short writes are retried via 018's derived helper.
- CQE `res < 0` maps to `IoError` via `from_errno_value` (ADR E3).
- SQE-pressure (queue full) is handled (flush + retry; `queue_full_retries` stat).
- Cancellation (from 021): cancel-vs-in-flight-CQE race produces exactly one
  terminal result; no use-after-free of the buffer.
- Skip-clean: without `SLUICE_HAS_LIBURING`, the build and all tests pass with
  the backend absent.

**Acceptance criteria.**
- Backend compiles only with `SLUICE_HAS_LIBURING`; default build unaffected.
- Repeated-liburing measurement recorded under `docs/io-uring-liburing-
  validation.md` (014C) for at least the W1/W4 workloads; results are
  workload-specific, not universal.
- Gate item 7 [IMPL] closed for io_uring: validation pass recorded.
- Blocking tests unchanged.

**Abort conditions.**
- liburing cannot remain a clean optional dependency (ADR AB8).
- Buffer lifetime cannot be enforced for in-flight SQEs (ADR AB4 / gate item 1).
- io_uring async is not correct under short writes / queue pressure / cancel
  races (ADR AB5) — then async stays on fake + thread pool and io_uring
  promotion is abandoned (ADR AB7).
- The default (no-liburing) build breaks (ADR AB1).

---

## 022 — Async bench harness

**Goal.** A microbench harness for async workloads W1–W4 (016B) that compares
Fake / ThreadPool / Uring backends against the blocking baseline, producing
**workload-specific** evidence (ADR §13 Phase 8; gate item 8). Reuses the
existing bench framework (`bench/`, `docs/bench-methodology.md`,
`docs/bench-decision-matrix.md`).

**Non-goals.**
- No universal performance claim (ADR R6; 016B C9).
- No optimization decisions baked in — evidence feeds a future decision matrix,
  like 011 did for blocking.
- No networking/timer benchmarks.

**Required tests / artifacts.**
- Bench targets build under the existing `bench` group; gated uring rows skip
  clean without liburing (like `bench/uring_write_bench`).
- Workloads: concurrent independent writes (W1), concurrent independent reads
  (W2), concurrent copy (W3), overlapped durability (W4 — uses 018B sync ops).
- All three async backends have rows: Fake (sanity only), ThreadPool (020A),
  Uring (020B). This is why 020A exists as a job.
- CSV/summary output consumable by the existing summarizer; rows labeled by
  backend and workload.
- A results note under `docs/results/` (per `docs/io-uring-liburing-
  validation.md`'s convention) recording at least one run.

**Acceptance criteria.**
- Gate item 8 [IMPL] closed: async bench methodology + harness exist.
- Results are scoped (per-workload, per-backend, per-machine), never universal.
- Blocking baseline row is unchanged from the 011 matrix (regression check).
- Blocking tests unchanged.

**Abort conditions.**
- The harness cannot produce reproducible, scoped numbers (flaky / machine-bound
  in a way that can't be documented).
- Adding the harness changes blocking bench numbers (baseline drift).

---

## Cross-links

- ⛔ **Sync-first gate (BLOCKS all jobs below until GREEN):** `docs/sync-before-async-readiness-gate.md` (016G).
- Sync-first job cards (must complete first): `docs/sync-io-next-jobs.md` (017S–023S).
- Sync gap audit (why async is blocked): `docs/sync-io-model-gap-audit.md` (016G).
- ADR: `docs/adr/ADR-async-io-model.md` (016D).
- Inventory: `docs/async-source-inventory.md` (016A).
- Problem statement: `docs/async-problem-statement.md` (016B) — defines W1–W5.
- Alternatives: `docs/async-design-alternatives.md` (016C).
- Readiness gate (the async-side gate, after the sync-first gate): `docs/async-readiness-gate.md` (016E).
- io_uring spike (reference point for 020B): `docs/io-uring-spike.md` (013).
- liburing validation runbook (020B follows this): `docs/io-uring-liburing-validation.md` (014C).
- Blocking bench methodology (022 extends this; async bench compares against the
  engineered W1–W4 rows from sync-first 022S/023S): `docs/bench-methodology.md`,
  `docs/bench-decision-matrix.md`.
