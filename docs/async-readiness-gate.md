# Async I/O readiness gate

**Status: sluice-CORE-016E.** A gate, not an implementation. It lists the
preconditions that **must** be satisfied before async implementation (jobs 017+,
see `docs/async-next-jobs.md`) may start, and how each is verified. It is the
async analogue of `docs/io-uring-readiness-gate.md` (012D), but for the async
*runtime* rather than the io_uring *spike*.

The gate is **not** "all green today." Several items are satisfied by this design
job (016A–016D) itself; the rest must be satisfied by the first implementation
jobs (017–019) before any *real backend* (020) is attempted. Items marked
**[DESIGN]** are closed by the documents; items marked **[IMPL]** must be proven
by code/tests in the named job.

## Decision

**CONDITIONALLY READY (design-side closed).** The async design (016A–016D) closes
every design-side precondition. Concretely: implementation of the **L1
foundation** (jobs 017 → 019 → 018 → 018B, per the revised 016F ordering) may
start — these jobs need no real backend and no io_uring. Implementation of a
**real backend** is staged: the thread-pool fallback (020A) may start once the
foundation is in place; the io_uring backend (020B) is **blocked** behind gate
item 7 (backend-agnostic core, which holds by design) AND its own validation
(014C); it also lands after the cancellation spike (021). No async code lands in
this job (016).

## 1. Minimum required gate items

Each item below is required by the task spec and maps to a specific ADR
section (`docs/adr/ADR-async-io-model.md`).

| # | Gate item | Status | Verified by | ADR § |
|---|---|---|---|---|
| 1 | Buffer lifetime + Completion lifecycle rules are testable | **[DESIGN]** rules stated (L1–L11); **[IMPL]** testable in job 019 | ADR §5 (L1–L3c, L7–L11); fake-backend contract test (016D T4) | §5 |
| 2 | Cancellation semantics are defined | **[DESIGN]** minimal semantics stated | ADR §7 (X1–X6) | §7 |
| 3 | Completion ordering is defined | **[DESIGN]** ordering stated (no per-fd FIFO on uring) | ADR §6 (O1–O5, P1) | §6 |
| 4 | Error propagation is defined | **[DESIGN]** stated, reuses Result<T>/IoError; submit_* returns Result<void> | ADR §8 (E1–E5) | §8 |
| 5 | Blocking backend compatibility is preserved | **[IMPL]** tests green, unchanged | `xmake test` after each job | §9a |
| 6 | No global scheduler required unless explicitly accepted | **[DESIGN]** AsyncIoContext is an owned object | ADR §3 (A6), §4; this gate §2 | §3/§4 |
| 7 | io_uring validation done OR async is explicitly backend-agnostic | **[DESIGN]** core is backend-agnostic; uring is one gated backend (020B) | ADR §4, §11 D1; validated separately via 014C | §4/§11 |
| 8 | Bench methodology exists for async workloads | **[DESIGN]** outlined; **[IMPL]** harness in job 022 | ADR §13 Phase 8; `docs/bench-methodology.md` extends to async | §13 |
| 9 | Existing blocking tests remain green | **[IMPL]** run now (this job) and after each future job | `xmake build sluice_core && xmake build -g test && xmake test` | §9a |

## 2. Per-item detail

### Item 1 — Buffer lifetime + Completion lifecycle rules are testable  **[DESIGN→IMPL]**

Rules L1–L3c (buffer: read-dst vs write-src) and L7–L11 (Completion lifecycle:
no move/destroy/reuse while outstanding; submit-into-non-ready = invalid_state;
result-before-ready = invalid_state; handle must outlive outstanding ops; no
implicit cancel on context destroy) are stated as testable propositions (ADR §5).
**Testability** is delivered by the FakeAsyncBackend (job 019): it can hold an op
outstanding across `poll()` calls, so a test can assert "buffer/Completion marked
in-use while outstanding" and "reusable after completion." Closed-as-design now;
closed-as-test in 019.

### Item 2 — Cancellation semantics are defined  **[DESIGN]**

ADR §7 defines the **minimal** model: `IoError::canceled` (already present,
016A §1) is reused; `cancel(completion)` is best-effort and asynchronous; the op
is marked ready **exactly once** with one of {success, error, canceled}; buffers
stay safe until ready. Structured cancel protection, group cancel, and
`IORING_OP_ASYNC_CANCEL` race handling are deferred to job 021. The minimal model
is sufficient to start implementation.

### Item 3 — Completion ordering is defined  **[DESIGN]**

ADR §6 (O1–O5 + P1): completions are reaped only in `poll()`/`wait_one()` (single
reaping family); no global FIFO is promised across independent ops/fds; per-backend
ordering is defined (Fake = submit order; ThreadPool = unspecified; **Uring = CQE
reap order ONLY — no per-fd write FIFO**); read/write ops are positional by
default (P1). No implicit cross-op ordering — callers compose it. Defined now so
cancellation and tests have a basis.

### Item 4 — Error propagation is defined  **[DESIGN]**

ADR §8 (E1–E5): no exceptions; `Result<T>`/`IoError` reused verbatim; CQE `res`
maps via the existing `from_errno_value`; **submit-time errors return
synchronously from `submit_*` as `Result<void>`** (incl. submit-into-non-ready
Completion, §5 L8), completion errors from `Completion::result()`. No new error
vocabulary.

### Item 5 — Blocking backend compatibility is preserved  **[IMPL]**

BlockingIoContext, FileReader/FileWriter, and Reader/Writer semantics are
declared frozen (ADR §9a). The async layer adds types in `sluice::async` and
edits no existing header's behavior. Verified by the blocking suite running
unchanged after every future job (item 9).

### Item 6 — No global scheduler unless explicitly accepted  **[DESIGN]**

The async context (`AsyncIoContext`) is an explicitly constructed, owned,
move-only object — paralleling `IoContext`. There is no thread-local/global
scheduler, in keeping with sluice's "stats never global, state never global"
rule (016A §1; `docs/design-io-context.md`). This is a hard constraint: any
future proposal to add a global scheduler requires a new ADR and explicit
acceptance. **Accepted posture: no global scheduler.**

### Item 7 — io_uring validation done OR async is backend-agnostic  **[DESIGN]**

The async **core** (017 → 019 → 018 → 018B) is **backend-agnostic**: it talks to
an `AsyncBackend` interface, and the foundation jobs ship with only the
FakeAsyncBackend. Therefore async implementation may start *without* a new
io_uring validation pass. The **UringAsyncBackend** (job 020B) is gated behind
`SLUICE_HAS_LIBURING` and validated under the existing runbook
(`docs/io-uring-liburing-validation.md`, 014C), with its own abort conditions
(ADR §15 AB7). This satisfies the disjunction: backend-agnostic core (now) +
separate validation (later).

### Item 8 — Bench methodology exists for async workloads  **[DESIGN→IMPL]**

The existing `docs/bench-methodology.md` and `docs/bench-decision-matrix.md`
define the no-universal-claims, workload-specific approach. Async benches (job
022) extend this: throughput/latency for W1–W4 (016B) against Fake / ThreadPool
(020A) / Uring (020B) backends, never claiming universal superiority.
Methodology exists now; the harness is built in job 022.

### Item 9 — Existing blocking tests remain green  **[IMPL]**

Run in this job (see §3) and re-run after every future job. This is the
non-negotiable regression floor. Any blocking-test change is abort condition AB2
(ADR §15). (The suite currently has 38 registered targets, all passing; the
"35/35" in the original spec is a lower bound, not a hard count.)

## 3. Verification run for this job (016)

This job adds **docs only**; no source changed. The build/test floor is verified:

```bash
xmake build sluice_core        # core lib still builds
xmake build -g test            # all test targets still build
xmake test                     # full suite runs (all green expected)
```

(Results recorded in the job's final report.)

## 4. Not yet satisfied (and who closes it)

```text
[IMPL] Item 1 testability        -> closed by job 019 (FakeAsyncBackend)
[IMPL] Item 5/9 per-job re-run   -> closed by every future job's CI step
[IMPL] Item 8 harness            -> closed by job 022 (async bench harness)
[IMPL] io_uring async validation -> closed by job 020B (separate, gated; after 021)
```

None of these block starting the foundation (017 → 019 → 018 → 018B). Job 020B
(io_uring async) is blocked until the core is backend-agnostic (it is, by
design), cancellation lands (021), AND its own validation passes.

## 5. Cross-links

- ADR: `docs/adr/ADR-async-io-model.md` (016D).
- Inventory: `docs/async-source-inventory.md` (016A).
- Problem statement: `docs/async-problem-statement.md` (016B).
- Alternatives: `docs/async-design-alternatives.md` (016C).
- Next jobs: `docs/async-next-jobs.md` (016F).
- Prior (spike) gate: `docs/io-uring-readiness-gate.md` (012D).
- liburing validation runbook: `docs/io-uring-liburing-validation.md` (014C).
- Bench methodology: `docs/bench-methodology.md`, `docs/bench-decision-matrix.md`.
