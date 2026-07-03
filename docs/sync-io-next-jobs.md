# Sync I/O — next job cards (sync-first phase)

**Status: sluice-CORE-016G (sync-first planning patch).** Splits the **blocking**
I/O model completion phase into small, independently abortable jobs. **No code is
written in this patch.** These jobs run *before* any async implementation job
(017+); the async implementation jobs in `docs/async-next-jobs.md` (016F) are
**blocked** behind the sync-first readiness gate
(`docs/sync-before-async-readiness-gate.md`).

The async **design** (016A–016F) stays accepted. This phase only engineers the
blocking baseline so async is later compared against a fair, multi-stream
blocking baseline — not against sequential-only blocking.

> **Reconciliation note (SYNC-IO-COMPLETE).** This doc is the *planning layer*
> (actionable job cards). The *architecture/contract/policy layer* lives in:
> `docs/sync-io-architecture.md` (architecture + taxonomy), `docs/sync-io-model.md`
> (primitive contract), `docs/sync-durability-model.md`, `docs/sync-bench-
> methodology.md`, `docs/sync-bench-matrix.md`, `docs/sync-optimization-notes.md`.
> The job numbers below (017S–023S) **stay** as the actionable units; the
> SYNC-IO-COMPLETE phases are a doc-structure overlay. Terminology used below is
> aligned to the architecture doc: backends (`BlockingIoContext`/`MemoryIoContext`/
> Fault|Observed layers) vs execution models (`blocking_sequential`/
> `blocking_bounded_pool`/`blocking_thread_per_stream`); the pool is
> `BlockingIoPool`, an **execution model not a backend**.

## Phase ↔ job mapping

| SYNC-IO-COMPLETE phase | Job(s) | Primary doc output |
|---|---|---|
| Phase 1 — repository audit | 017S | (informs) `sync-io-architecture.md`, `sync-io-model.md` |
| Phase 2 — sync architecture doc | 017S | `docs/sync-io-architecture.md` ✓ (written) |
| Phase 3 — primitive contract | 017S | `docs/sync-io-model.md` ✓ (written) |
| Phase 4 — positional blocking I/O | **018S** | (contract in `sync-io-model.md`) |
| Phase 5 — derived helper closeout | **019S** | (contract in `sync-io-model.md`) |
| Phase 6 — durability model | **020S** | `docs/sync-durability-model.md` ✓ (written) |
| Phase 7 — bounded pool | **021S** | `BlockingIoPool` (code; bench/support) |
| Phase 8 — W1–W4 bench matrix | **022S** | bench targets + CSV |
| Phase 9 — bench methodology docs | 022S | `docs/sync-bench-methodology.md`, `docs/sync-bench-matrix.md` ✓ (written) |
| Phase 10 — optimization notes | **023S** | `docs/sync-optimization-notes.md` ✓ (template written) |
| Phase 11 — async deferral note | — | `docs/async-deferred-until-sync-baseline.md` ✓ (written) |
| Phase 12 — tests & validation | each | per-job tests |

Docs marked ✓ already exist (written in the reconciliation patch). Code phases
(018S/019S/021S/022S/023S) remain to be implemented in follow-up TDD sessions.

## Why a sync-first phase

```text
Async's value (016B W1-W4) = many outstanding ops on few threads.
Blocking today             = one op, one thread, sequential (sync-io-model-gap-audit.md).

Comparing async-W1..W4 against sequential-blocking is the wrong comparison.
The sync-first phase closes gaps G1-G7 so the blocking baseline is concurrent,
positional, durability-defined, and measured across W1-W4. THEN async starts.
```

## Ordering

```text
016 (design, accepted/frozen) ──> 017S Sync I/O model audit
                                       │
                                       v
                                 018S Positional blocking I/O  (pread/pwrite/preadv/pwritev)
                                       │
                                       v
                                 019S Blocking derived helper closeout
                                       │
                                       v
                                 020S Blocking durability model (policy + baseline)
                                       │
                                       v
                                 021S Blocking bounded pool baseline (std::thread)
                                       │
                                       v
                                 022S Blocking W1-W4 workload benchmark matrix
                                       │
                                       v
                                 023S Blocking optimization pass
                                       │
                                       v
                            [sync-first readiness gate: GREEN]
                                       │
                                       v
                            async implementation jobs (016F: 017 -> 019 -> 018 ...)
```

Conventions for every card:

- **Default backend unchanged.** No job changes `BlockingIoContext` as default or
  touches `Reader`/`Writer` *semantics* (positional ops are *additions*, not
  changes to existing methods).
- **Tests-green floor.** Every job keeps the existing blocking tests green.
- **No new dependency** unless explicitly evaluated. The pool (021S) uses
  `std::thread` only — no external thread-pool library.
- **No universal performance claim.** Bench jobs produce workload-specific
  evidence only.

---

## 017S — Sync I/O model audit

**Goal.** Turn `docs/sync-io-model-gap-audit.md` into a reviewed, evidence-locked
audit of the blocking model, with a go/no-go decision on positional I/O (G1/G2)
and an explicit list of what each subsequent job must and must not touch. This is
the planning anchor for 018S–023S.

**Non-goals.**
- No code. No header changes.
- No decision reversal on async (016 stays accepted).

**Required artifacts.**
- Confirmed gap inventory (G1–G7) with file:line evidence (already in the audit
  doc; this job reviews/signs it off).
- A positional-I/O decision record: add `pread`/`pwrite` (+`preadv`/`pwritev`) to
  the blocking backend as new opt-in methods, OR document why blocking stays
  cursor-only and how async comparison accounts for it.
- A per-job "do not touch" list (existing semantics frozen).

**Acceptance criteria.**
- `docs/sync-io-model-gap-audit.md` reviewed; positional decision recorded inline.
- Each of 018S–023S has an unambiguous scope derived from this audit.
- Build/tests green (docs-only job; verification is a no-op build check).

**Abort conditions.**
- The positional-I/O decision cannot be made without changing Reader/Writer
  semantics (would violate the hard boundary).

---

## 018S — Positional blocking I/O

**Goal.** Close gap G1: add **positional** blocking read/write to the POSIX file
backend, mirroring the async P1 rule (ADR 016D §6) on the blocking side. Adds
opt-in `pread`/`pwrite` (and `preadv`/`pwritev` vector forms per G2) as **new**
methods — does not alter existing `read_some`/`write_some`/`read_vec`/`write_vec`.

**Non-goals.**
- No change to existing cursor-based methods or their semantics.
- No `lseek` wrapper unless the audit (017S) explicitly requires it.
- No async. No io_uring.

**Required tests.**
- Positional read/write at explicit offsets do not move the implicit cursor
  (verify a subsequent cursor-based read sees the pre-op position).
- Two positional ops on the same fd at different offsets are independent
  (no cursor coupling) — the blocking analogue of async W1/W2 position independence.
- Positional vector (`preadv`/`pwritev`) correctness across `IOV_MAX` chunking,
  mirroring existing `readv`/`writev` tests.
- Short-read/short-write retry semantics match the cursor-based derived helpers.
- EINTR retry and errno preservation match existing `read_some`/`write_some`.
- Existing cursor-based tests unchanged (regression).

**Acceptance criteria.**
- New methods compile, link, and pass in debug + release.
- The blocking backend can express "many offsets on one fd" without `lseek`.
- No existing test changes.
- `xmake build sluice_core`, `xmake build -g test`, `xmake test` all green.

**Abort conditions.**
- Positional ops cannot be added without modifying existing method signatures
  (would break Reader/Writer semantics — hard boundary).
- `pread`/`pwrite`/`preadv`/`pwritev` are unavailable on a target platform and no
  clean fallback exists — then the decision reverts to "cursor-only, documented"
  and the gap is recorded rather than closed.

---

## 019S — Blocking derived helper closeout

**Goal.** Close gap G3: ensure the derived-helper surface (`read_exact`,
`write_all`, `read_vec_all`, `write_all_vec`) is complete and consistent,
including positional variants if 018S added them (e.g. `read_exact_at`,
`write_all_at`). Audit for untested edge cases (empty slices, EOF mid-vec,
zero-progress). This is a closeout job, not new architecture.

**Non-goals.**
- No new architecture. No async.
- No change to existing helper semantics.

**Required tests.**
- Each derived helper has a short-input/EOF/zero-progress test (fill gaps found
  in the audit).
- Positional derived helpers (if added) match the cursor-based ones byte-for-byte
  in semantics, differing only in the explicit offset.
- Regression: existing derived-helper tests unchanged.

**Acceptance criteria.**
- Audit checklist closed: every derived helper has edge-case coverage.
- No existing test changes.
- Build/tests green.

**Abort conditions.**
- A derived helper is found to have a latent semantic bug that requires changing
  its behavior (would break callers) — then it is documented as a known gap, not
  silently fixed in this phase.

---

## 020S — Blocking durability model

**Goal.** Close gap G4: document and baseline the **blocking durability model**
as a defined policy, so async W4 (overlapped durability) has a defined blocking
baseline to compare against. Does NOT add group commit (016B O5) — it records
what blocking durability *is* and measures it.

**Non-goals.**
- No group commit / multi-stream atomic commit.
- No async sync (no AIO/io_sync). That is async W4, gated behind this phase.
- No stronger durability guarantee than what fsync/fdatasync give.

**Required artifacts + tests.**
- A durability-policy doc (extends `docs/design-flush-sync-durability.md`):
  when a blocking writer should sync; cadence options (per-record / per-batch /
  caller-driven); accepted limits of the single-writer-barrier WAL.
- A blocking durability baseline bench row: cost of `sync_data`/`sync_all` under
  per-batch cadence (single-stream), recorded under `docs/results/`.
- Regression: existing `sync_data`/`sync_all`/WAL tests unchanged.

**Acceptance criteria.**
- The blocking durability baseline is *defined* (policy doc) and *measured*
  (bench row), workload-specific, no universal claim.
- W4 has a blocking baseline number to compare against once async W4 exists.
- Build/tests green.

**Abort conditions.**
- Defining a cadence policy would require changing `SyncableWriter` semantics or
  WAL behavior — then the policy stays "caller-driven" and is documented as such.

---

## 021S — Blocking bounded pool baseline (`BlockingIoPool`)

**Goal.** Close gap G5: add **`BlockingIoPool`** — a bounded blocking worker pool
(`std::thread` based, no external dependency) so W1–W4 are expressible on the
blocking side as *concurrent* workloads (`blocking_bounded_pool` mode), not only
sequential ones. This is the largest gap: today the blocking path has zero
concurrency primitives. **`BlockingIoPool` is an execution model for benchmarks,
NOT an I/O backend** — it does not implement `IoContext`, is not selectable as a
backend, and lives in `bench/support/` (see `docs/sync-io-architecture.md` §3).
It is opt-in and does NOT become the default backend.

**Non-goals.**
- Not a replacement for `BlockingIoContext` (the default stays cursor-based,
  single-threaded, unchanged).
- No async runtime. No futures/promises abstraction beyond what the pool needs.
- No cancellation of in-flight blocking syscalls (portable cancel is an async
  concern, deferred).
- No networking/timers.

**Required tests.**
- Pool runs N independent blocking tasks on K worker threads; results collected.
- Bounded: submitting past the bound blocks/throttles as documented (no unbounded
  queue growth).
- Exception/error propagation from worker tasks back to the submitter.
- Buffer/handle lifetime: a task's buffers outlive the task (caller-owned, same
  discipline as blocking today).
- Determinism-friendly: a single-worker pool is sequential (testable without
  races via the single-worker config).
- Regression: existing single-threaded tests unchanged.

**Acceptance criteria.**
- W1–W4 are expressible as concurrent blocking workloads on `BlockingIoPool`.
- `BlockingIoPool` lives in `bench/support/` and is NOT an `IoContext` backend.
- No new external dependency (std::thread/mutex/condition_variable only).
- ASan/TSan runs clean on pool tests.
- `BlockingIoContext` default path unchanged.
- Build/tests green.

**Abort conditions.**
- The pool cannot be made correct under TSan (data races) without a global
  scheduler or breaking the no-global-state rule.
- Buffer/handle lifetime cannot be kept caller-owned cleanly.
- The pool forces a default-backend change (hard boundary).

---

## 022S — Blocking W1–W4 workload benchmark matrix

**Goal.** Close gaps G6+G7: extend stats/bench for multi-stream cells and build
the **blocking W1–W4 benchmark matrix** — concurrent independent writes (W1),
reads (W2), copy (W3), and overlapped durability (W4) — against `BlockingIoPool`
(`blocking_bounded_pool` mode) from 021S, with `blocking_sequential` and
`blocking_thread_per_stream` baselines for reference. Workloads, parameters,
metrics, and decision questions are fixed in `docs/sync-bench-matrix.md`; the
methodology in `docs/sync-bench-methodology.md`. This is the blocking analogue of
async job 022; it must exist *first* so async is later compared against an
engineered blocking matrix.

**Non-goals.**
- No universal performance claim.
- No async rows (async bench is its own job, gated behind this phase).
- No networking/timer benches.

**Required artifacts + tests.**
- Stats extension (G6): active-streams count, per-cell concurrency fields in the
  bench CSV (the required columns `streams`/`pool_threads`/`threads_used` per
  `docs/sync-bench-methodology.md` §4). Caller-owned, never global.
- Bench targets (new, under `bench/`, names adjusted to repo convention):
  W1 many-write, W2 many-read, W3 many-copy, W4 overlapped-durability — each
  parameterized by (streams, pool_threads, block/buffer size, file_layout,
  sync_policy) per `docs/sync-bench-matrix.md`. Modes:
  blocking_sequential / blocking_bounded_pool / blocking_thread_per_stream
  (+ sync_uring_spike as a build-gated comparison row only).
- CSV output per `docs/sync-bench-methodology.md` §4.
- A results note under `docs/results/` recording at least one run per workload,
  scoped per-workload/per-machine, never universal.

**Acceptance criteria.**
- The blocking baseline has W1, W2, W3, W4 coverage with a concurrency axis.
- Results are scoped, reproducible, and feed 023S.
- Existing single-stream bench numbers do not regress (regression check).
- Build/tests green.

**Abort conditions.**
- The matrix cannot produce reproducible numbers (flaky beyond documentation).
- Adding multi-stream benches changes single-stream baseline numbers.

---

## 023S — Blocking optimization pass

**Goal.** Using the 022S matrix evidence, tune the **blocking baseline** (buffer
sizes, vector chunking, copy-strategy selection, sync cadence) so that async is
later compared against an *engineered* blocking baseline, not a naive one. This
is the analogue of the existing 011 decision-matrix work, extended to W1–W4.

**Non-goals.**
- No universal claim. No async.
- No optimization that changes Reader/Writer semantics.
- No optimization that requires a new dependency.

**Required artifacts + tests.**
- An evidence-linked decision matrix (extends `docs/bench-decision-matrix.md`)
  recording, per W1–W4 cell, the chosen blocking tuning and why.
- Re-run 022S after tuning; record before/after, scoped per workload.
- Regression: single-stream tests and the 011 single-stream baseline do not
  regress beyond documented tuning bounds.

**Acceptance criteria.**
- The blocking baseline is engineered (tuned with evidence) across W1–W4.
- The sync-first readiness gate (`docs/sync-before-async-readiness-gate.md`) can
  go GREEN: the blocking baseline is positional, durability-defined, concurrent,
  and measured.
- Build/tests green.

**Abort conditions.**
- Tuning cannot be done without semantic changes or new deps — then the baseline
  stays at the 022S numbers and the gate records "engineered as far as blocking
  allows without semantic change."

---

## After 023S: the sync-first readiness gate

Once 017S–023S are accepted, `docs/sync-before-async-readiness-gate.md` goes
GREEN and the async implementation jobs in `docs/async-next-jobs.md` (016F:
017 → 019 → 018 → 018B → 020A → 021 → 020B → 022) are **unblocked**. Async then
measures itself against the engineered blocking W1–W4 matrix from 022S/023S, not
against sequential blocking.

## Cross-links

**Architecture / contract / policy layer (SYNC-IO-COMPLETE):**
- Sync architecture: `docs/sync-io-architecture.md`.
- Primitive contract: `docs/sync-io-model.md`.
- Durability model: `docs/sync-durability-model.md`.
- Bench methodology: `docs/sync-bench-methodology.md`, `docs/sync-bench-matrix.md`.
- Optimization notes: `docs/sync-optimization-notes.md`.
- Async deferral note: `docs/async-deferred-until-sync-baseline.md`.

**Planning layer (016G):**
- Gap audit: `docs/sync-io-model-gap-audit.md` (016G).
- Sync-first readiness gate: `docs/sync-before-async-readiness-gate.md`.

**Async (frozen / blocked):**
- Async next jobs (blocked until the gate is green): `docs/async-next-jobs.md` (016F).
- Async ADR (accepted, unchanged): `docs/adr/ADR-async-io-model.md` (016D).
- Async problem statement (W1–W5): `docs/async-problem-statement.md` (016B).

**Existing (extended, not replaced):**
- Durability design: `docs/design-flush-sync-durability.md`, `docs/design-wal-durability.md`.
- Bench methodology: `docs/bench-methodology.md`, `docs/bench-decision-matrix.md`.
