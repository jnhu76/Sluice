# Async I/O problem statement

**Status: sluice-CORE-016B.** Defines *why* sluice needs an async model, *which*
workloads it is for, and — just as importantly — what is explicitly out of scope.
This frames the alternatives comparison (016C) and constrains the ADR (016D). It
makes **no universal performance claim**.

## 1. Why async matters for sluice

sluice v0.1 MVP is **blocking-first and release-ready**: every primitive
(`read_some`, `write_some`, `read_vec`, `write_all_vec`, `sync_data`) blocks the
calling thread until the kernel returns. That is correct, simple, observable, and
is the deliberately chosen default. But blocking has a structural cost:

```text
A thread that blocks on one fd cannot make progress on another fd.
```

For a single-stream copy or a single WAL writer this is irrelevant — the work is
inherently sequential and blocking reads/writes are optimal. The cost appears
only when sluice is asked to **drive many independent I/O streams from few
threads**: many files written concurrently, many shards copied in parallel, a WAL
that overlaps fsync with the next batch. In that regime a blocking thread-per-fd
model spends threads (stack memory, scheduler contention, context-switch cost) on
nothing but waiting. An async model lets one or a few threads keep many
outstanding operations in flight and resume them on completion.

The point is not "async is faster in general" — it is not, for the sequential
paths sluice already serves well. The point is that there is a class of
**multi-stream, latency-dominated** workloads blocking handles poorly, and sluice
currently has no answer for them at all. This job designs that answer without
implementing it.

## 2. Which workloads async is meant to serve (in scope)

These are the workloads the async model must be able to express. They are all
*file* workloads — the same domain as today's blocking backend.

```text
W1. Concurrent independent file writes
    Many files (or many offsets) written at once from one driver thread,
    overlapping kernel completion with submit. Example: sharded WAL, parallel
    snapshot writes, multi-part output staging.

W2. Concurrent independent file reads
    Many files read at once (scatter-fan-in), feeding a downstream consumer as
    completions arrive rather than serializing on per-file blocking reads.

W3. Concurrent copy across multiple streams
    N independent reader→writer copies in flight on a small number of threads,
    where blocking one stream must not stall the others.

W4. Overlapped durability
    fsync/fdatasync overlapped with the *next* batch of writes (today sync is a
    blocking tail on each writer). Concurrency here reduces the wall-clock cost
    of durability under load. (Implemented by SyncDataOp/SyncAllOp — ADR §3;
    job 018B. Note: an async sync Completion means the kernel acknowledged the
    sync, NOT a stronger durability guarantee than blocking fsync/fdatasync.)

W5. Deterministic, fast async tests
    A fake async backend that completes operations on demand, so async code paths
    can be unit-tested without a real kernel scheduler or real threads.
    (Implemented by FakeAsyncBackend — job 019.)
```

The common property is **many outstanding ops, few threads**. Single-stream
throughput is **not** an async goal — the blocking backend already serves it.

## 3. Which workloads are explicitly OUT of scope

These are excluded by the job's hard boundaries and must not appear in the
recommendation. Listing them prevents scope creep in the ADR and future jobs.

```text
O1. Networking (sockets, accept, connect, recv/send)        — out of scope
O2. Timers / sleep / time-based completion                  — out of scope
O3. Process / spawn / wait APIs                             — out of scope
O4. Memory-mapped files (mmap)                              — out of scope
O5. Group commit / multi-stream atomic commit               — out of scope
O6. Universal / general-purpose async runtime               — out of scope (file I/O only)
O7. Replacing the blocking backend as the default           — FORBIDDEN (hard boundary)
O8. Plugging io_uring into BlockingIoContext                — FORBIDDEN (hard boundary)
O9. Production async implementation in this job             — this job is design only
O10. Any new dependency not evaluated in the ADR            — FORBIDDEN (hard boundary)
```

The async model is a **narrow, file-I/O-focused concurrency addition** behind a
seam. It is not a general concurrency framework and must not become one.

## 4. Why synchronous-over-uring is insufficient

The experimental `UringWriteBatch`/`UringIoContext` (sluice-CORE-013) proved the
io_uring kernel seam works: SQEs are submitted, CQEs arrive, bytes are written,
and the result is correct under short writes and queue pressure. That is real and
valuable. But the spike is **synchronous-over-uring**: it submits, then blocks on
completion (`io_uring_submit` + `io_uring_wait_cqe`) **inside the call**. That
choice was deliberate — it needed no async runtime and so could ship behind the
013 gate. It is also exactly what makes it insufficient as a production async
backend:

```text
S1. It blocks the calling thread per op.
    write_all(fd, bytes, off) does not return until the kernel completes the
    write (looping on short writes). So it has io_uring's machinery but
    blocking's concurrency model: one outstanding op per thread. None of W1–W4
    can be expressed.

S2. It cannot overlap submit with completion.
    The ring is used submit→wait→submit→wait, one op at a time. The whole point
    of io_uring — batching many SQEs and reaping many CQEs asynchronously — is
    unused. Throughput vs blocking is workload-dependent, not universally better.

S3. It has no cancellation.
    Because it blocks to completion, "cancel" is meaningless. A production async
    model needs defined cancellation semantics; the spike cannot provide them.

S4. Its buffer-lifetime contract is the blocking one.
    "Caller buffer must outlive the call" is only safe because the call blocks.
    The moment completion is deferred, the buffer must outlive the *outstanding
    op* — a different, stricter contract the spike neither states nor enforces.

S5. It cannot serve W5 (deterministic tests).
    It is a real-kernel path; there is no fake. Async testability needs a fake
    backend, which a synchronous-over-uring spike cannot be.
```

In short: synchronous-over-uring is the right *experiment* (it de-risked the
kernel seam) but the wrong *production model* (it preserves blocking's
concurrency limitation while paying io_uring's complexity). Promotion to a real
async backend requires the async runtime, cancellation model, and buffer-lifetime
contract that the spike deliberately omitted — which is precisely what 016D
designs.

## 5. Why the blocking backend must remain stable

`BlockingIoContext` is the default, the tested baseline (the tests-green gate of
016E), the backend every example and bench assumes, and the reference against
which any async path is compared. Its stability is both a product guarantee and a
measurement precondition:

```text
B1. Product guarantee   — callers that do not opt into async get today's behavior,
                          byte-for-byte. No silent semantics change.
B2. Measurement basis   — async-vs-blocking comparison is only meaningful if the
                          blocking baseline is unchanged while async is added.
B3. Safety floor        — if async proves wrong, the project falls back to an
                          unchanged blocking core. This is the abort condition.
B4. Reader/Writer       — the Reader/Writer *semantics* (read_some's 0==EOF rule,
   semantics             write_some's 0-on-non-empty==failure rule, error
                          propagation, vector stop-on-short) are the contract
                          async must not break.
```

Therefore: the async model is **additive and opt-in**. It introduces new types
behind a new seam; it does not modify `FileReader`/`FileWriter`/`BlockingIoContext`,
does not change `Reader`/`Writer` semantics, and does not touch default-backend
selection. This is enforced as a readiness-gate item (016E) and as the top abort
condition (016D).

## 6. Constraints carried forward into the design

These become hard requirements on the ADR (016D) and the alternatives comparison
(016C):

```text
C1. Must serve W1–W5 with few threads; concurrency is the point.
C2. Must NOT serve O1–O10 (networking, timers, mmap, group commit, …).
C3. Must be opt-in and additive; blocking stays default and unchanged.
C4. Must preserve Reader/Writer semantics on the blocking path.
C5. Must define buffer lifetime for the *outstanding op*, not just the call.
C6. Must define cancellation (even if minimal) — the spike's "none" is not enough.
C7. Must be testable without a real kernel scheduler (W5).
C8. Must be backend-agnostic enough that async is not welded to io_uring
    (io_uring is one candidate async backend, validated separately; see 016E).
C9. Must make NO universal performance claim; only workload-specific evidence.
C10. Must introduce no dependency unless the ADR explicitly evaluates it.
```

## 7. Cross-links

- What exists today (synchronous + spike + Zig reference): `docs/async-source-inventory.md` (016A).
- Option comparison: `docs/async-design-alternatives.md` (016C).
- Decision: `docs/adr/ADR-async-io-model.md` (016D).
- Preconditions to start: `docs/async-readiness-gate.md` (016E).
- Implementation split: `docs/async-next-jobs.md` (016F).
- io_uring spike (synchronous-over-uring origin): `docs/io-uring-spike.md` (013).
