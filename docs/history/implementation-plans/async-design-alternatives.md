# Async I/O design alternatives

**Status: sluice-CORE-016C.** Compares the candidate async models for sluice
against a fixed set of criteria. The **recommendation** is made in
`docs/adr/ADR-async-io-model.md` (016D); this document is the evaluation that
justifies it. The comparison is about file I/O only (see 016B scope); it makes no
universal performance claim and introduces no dependency without explicit
evaluation.

## 1. Evaluation criteria

Every option is scored against the same axes, derived from the problem statement
constraints (016B §6) and the inventory gaps (016A §4):

```text
1. API complexity          How hard for callers to use correctly? Footguns?
2. Buffer lifetime safety  Is "buffer outlives the outstanding op" enforceable/auditable?
3. Cancellation model      Can an outstanding op be cancelled? Are semantics defined?
4. Error propagation       Does it reuse Result<T>/IoError? Partial-progress semantics?
5. Testability             Can it be driven by a deterministic fake (workload W5)?
6. Compatibility w/        Can the existing Reader/Writer and blocking path stay unchanged?
   Reader/Writer
7. Compatibility w/        Can a real io_uring backend sit behind it later?
   future io_uring
8. Incremental adoption    Can it be introduced in small jobs (017+) without a big bang?
```

Each axis is rated High / Medium / Low (good / mixed / weak) with a one-line
reason. The ratings are qualitative, not benchmarks.

## 2. The options

### Option 1 — Callback-based completion API

Caller submits an op with a buffer and a callback; the runtime invokes the
callback (on some thread) when the op completes.

```cpp
// sketch only — not a commitment
runtime.write(fd, buf, off, [](Result<size_t> r){ /* completion */ });
```

| Axis | Rating | Reason |
|---|---|---|
| API complexity | **Low** | Easy to define; easy to misuse (callback lifetime, thread of execution, re-entrancy, "what if I need the result later?"). Callback hell scales poorly. |
| Buffer lifetime safety | **Low** | Buffer must outlive the async op, but nothing ties the callback to the buffer's scope — easy to dangle; purely a documentation contract. |
| Cancellation model | **Low** | No natural place to express "this op is cancelled"; callbacks fire regardless; cancelling is bolted on. |
| Error propagation | **Medium** | Result<T>/IoError fits, but partial progress and ordering are caller-defined and ad hoc. |
| Testability | **High** | A fake can invoke callbacks synchronously and deterministically. |
| Reader/Writer compat | **Medium** | Orthogonal — coexists, but offers no shared abstraction with Reader/Writer; risks two parallel APIs. |
| io_uring compat | **Medium** | Maps to a CQE→callback dispatch, but the thread/queueing model is underspecified. |
| Incremental adoption | **Medium** | Small to start, but every caller learns a new callback style; refactor pressure accumulates. |

**Verdict:** viable as a lowest-level primitive, but a poor *public* API — weak
safety, weak cancellation, parallel to Reader/Writer. Rejected as the public
shape (see ADR).

### Option 2 — Pollable completion queue

Caller submits an op and gets back a handle/`OperationId`; it (or a driver)
polls a queue to reap completions. Closest to raw io_uring CQE reaping.

```cpp
auto id = runtime.submit_write(fd, buf, off);
// later:
while (auto c = runtime.poll()) { /* c.id, c.result */ }
```

| Axis | Rating | Reason |
|---|---|---|
| API complexity | **Medium** | Explicit submit/poll is clear but pushes the driver loop onto every caller. |
| Buffer lifetime safety | **Medium** | Buffer lifetime still caller-owned, but the queue gives an explicit "op still outstanding" set to audit against. |
| Cancellation model | **Medium** | A cancel(id) can be expressed, but ordering vs an in-flight CQE must be defined. |
| Error propagation | **High** | Completions carry Result<T>/IoError directly; partial progress is explicit per completion. |
| Testability | **High** | A fake queue can produce completions on demand (ideal for W5). |
| Reader/Writer compat | **Low** | Fundamentally a different shape from the pull-based Reader/Writer; not a drop-in. |
| io_uring compat | **High** | This *is* the io_uring model; maps 1:1. |
| Incremental adoption | **Medium** | The queue is a real subsystem to build before it is useful. |

**Verdict:** excellent as an *internal* backend boundary (and as the io_uring
mapping), but too low-level as the *public* API — every caller becomes a driver.

### Option 3 — C++20 coroutine `Task<T>`

Caller `co_await`s an async op; the compiler turns it into state-machine
resumption on completion. `Result<T>` returned through the coroutine.

```cpp
Task<Result<size_t>> copy_one(AsyncWriter& w) {
    auto n = co_await w.write_some(buf);   // suspends, resumes on completion
}
```

| Axis | Rating | Reason |
|---|---|---|
| API complexity | **High** (good) | Linear, blocking-looking code; the compiler manages suspension/resume. Lowest cognitive load for callers *if* the runtime is hidden. |
| Buffer lifetime safety | **Low** | The danger zone: a coroutine can capture references/`std::span` by reference and suspend, while the caller's buffer goes out of scope. No compiler enforcement; discipline-only. This is the classic coroutine footgun. |
| Cancellation model | **Medium** | Expressible (await-cancel), but C++ has no language-level cancel; needs an explicit token/object and defined semantics. |
| Error propagation | **High** | `co_await` propagates `Result<T>`/IoError naturally; partial progress modeled in the awaiter. |
| Testability | **Medium** | A fake can resume coroutines synchronously, but coroutine debugging/stack traces are notoriously hard. |
| Reader/Writer compat | **Medium** | Async ops can *mirror* Reader/Writer method names, but they are new types (`AsyncReader`/`AsyncWriter`), not the existing ones. |
| io_uring compat | **High** | A driver resumes the coroutine on CQE — standard pattern. |
| Incremental adoption | **Low** | Coroutines demand a runtime (allocator, awaitable machinery, executor) up front; hard to grow incrementally. Compiler/ABI/tooling support must be assumed. |

**Verdict:** the most ergonomic *caller* API, but the worst *default* buffer
lifetime and the largest up-front runtime. Strong candidate **later**, layered on
top of a completion-based core — **not** as the first foundation. See ADR
"coroutines: later, not now".

### Option 4 — Sender/receiver style API

A value-based composition pipeline (`write(fd, buf, off) | then(...)`) where
completion is a push to a receiver. Std::execution (P2300) style; no coroutines.

| Axis | Rating | Reason |
|---|---|---|
| API complexity | **Low** | Powerful but notoriously hard to learn/teach; the abstraction depth is high. |
| Buffer lifetime safety | **Medium** | Better than callbacks (the chain expresses ownership), but still discipline-driven for raw buffers. |
| Cancellation model | **High** | P2300 has first-class cancellation receivers; the model is designed for it. |
| Error propagation | **High** | Errors are channelled through the sender contract; composable. |
| Testability | **Medium** | Testable, but the fake must speak the sender protocol. |
| Reader/Writer compat | **Low** | A pipeline style that does not resemble pull-based Reader/Writer. |
| io_uring compat | **High** | Maps cleanly to submit/complete. |
| Incremental adoption | **Low** | Either you adopt a heavy standard proposal dependency or you reimplement it — both are large. |

**Verdict:** the most principled model, but the heaviest to adopt and the least
like the existing API. It would also imply a dependency (P2300) the ADR must
explicitly evaluate — and it is not yet widely available. Rejected as the
foundation; ideas (composability, cancellation receivers) noted for the future.

### Option 5 — Thread-pool-backed async facade

Async ops are implemented by submitting blocking work to a thread pool; the
facade returns a future/handle. "Async" without a true event loop.

```cpp
auto f = pool.write(fd, buf, off);   // runs blocking write_some on a worker
auto r = f.get();                    // or a callback on completion
```

| Axis | Rating | Reason |
|---|---|---|
| API complexity | **High** (good) | Trivial to implement on top of the *existing* `write_some`/`read_some`. |
| Buffer lifetime safety | **Medium** | Buffer must outlive the worker's blocking call; expressible but discipline-driven. |
| Cancellation model | **Low** | Cannot cancel a blocking syscall in flight portably; only "don't start" cancellation. |
| Error propagation | **High** | Reuses Result<T>/IoError verbatim (it *is* the blocking path on another thread). |
| Testability | **High** | Easy: run-to-completion semantics; a single-threaded pool is deterministic-ish. |
| Reader/Writer compat | **High** | Wraps the existing Reader/Writer directly; semantics identical. |
| io_uring compat | **Low** | Does not use io_uring at all; cannot become a real async uring backend. It is a *fallback*, not a path forward. |
| Incremental adoption | **High** | Smallest possible first step. |

**Verdict:** the lowest-risk first step and the natural *fallback* backend for
machines without io_uring. But it does not deliver true concurrency on few
threads (it spends a thread per outstanding op) and does not lead to io_uring.
Keep as a fallback / test backend, not the recommendation.

### Option 6 — io_uring-native completion backend

The async model *is* io_uring: SQE submit + CQE reap, with a driver loop, as the
public shape.

| Axis | Rating | Reason |
|---|---|---|
| API complexity | **Low** | Pushes the raw uring model onto callers (see Option 2). |
| Buffer lifetime safety | **Medium** | io_uring's own lifetime rules (registered buffers, in-flight SQEs) apply; must be surfaced. |
| Cancellation model | **Medium** | `IORING_OP_ASYNC_CANCEL` exists, but cancel-vs-in-flight-CQE races must be defined. |
| Error propagation | **High** | CQE `res` maps to IoError via the existing `from_errno_value`. |
| Testability | **Low** | A real-kernel path; a fake must emulate the CQE stream — possible but heavy. |
| Reader/Writer compat | **Low** | Not Reader/Writer-shaped; a different surface. |
| io_uring compat | **High** (it *is*) | Maximally compatible — but welded to one backend and one OS. |
| Incremental adoption | **Low** | Ties the whole async API to Linux + liburing; not portable, not backend-agnostic. |

**Verdict:** this is the right *backend*, the wrong *public API*. The ADR keeps
io_uring as a backend *behind* a backend-agnostic completion boundary (mirrors
how sluice already keeps the POSIX backend behind `IoContext`).

## 3. Comparison matrix (qualitative)

H = strong/good, M = mixed, L = weak/problematic. From §2.

| Axis | 1 Callback | 2 Pollable CQ | 3 Coroutine | 4 Sender/Rcvr | 5 Thread pool | 6 uring-native |
|---|---|---|---|---|---|---|
| API complexity | L | M | H | L | H | L |
| Buffer lifetime | L | M | L | M | M | M |
| Cancellation | L | M | M | H | L | M |
| Error propagation | M | H | H | H | H | H |
| Testability | H | H | M | M | H | L |
| Reader/Writer compat | M | L | M | L | H | L |
| io_uring compat | M | H | H | H | L | H |
| Incremental | M | M | L | L | H | L |

## 4. Synthesis: separate *foundation* from *ergonomic public* API

The options separate into **three layers** (matching the ADR's L0/L1/L2 model),
and the right answer is to **combine** them rather than pick one. The key
distinction this section makes — and that the ADR (016D §2) rests on — is that
the *foundation* API and the *ergonomic public* API are not the same thing:

```text
L2  Ergonomic public API : must be ergonomic, error-clean, Reader/Writer-shaped.
                           -> Option 3 (coroutine Task<T> / AsyncReader/AsyncWriter)
                              is the best ERGONOMIC SHAPE — but only later. Its
                              buffer-lifetime footgun and up-front runtime cost
                              make it wrong as the FOUNDATION.
L1  Low-level foundation : must map cleanly to io_uring, be testable with a fake,
                           carry completions explicitly, and be backend-agnostic.
                           -> Option 2 (pollable completion queue) is the best
                              FOUNDATION. This is PUBLIC but it is a power-user
                              surface, not the end-user API — that resolves the
                              tension that "Option 2 is too low-level for the
                              public API": it is the right *foundation*, and L2
                              is the future *public* shape that hides it.
L0  Backend seam          : internal; backends plug in.
                           -> Option 5 (thread pool) = fallback backend.
                           -> Option 6 (io_uring)    = one real backend.
```

So the structure the ADR recommends is: **an explicit completion-queue /
completion-object low-level foundation (Option 2, borrowing Zig's
caller-provided Completion idea from 016A §3), with a thread-pool fallback
(Option 5) for portability and a deterministic fake for tests, and an ergonomic
coroutine/AsyncReader layer (Option 3) built on top LATER — not now.** io_uring
(Option 6) is one *backend* for that foundation, validated separately.

> Note on consistency: this document rates Option 2 "excellent as a foundation,
> too low-level as the public API." That is not a contradiction with the ADR
> accepting Option 2's shape — the ADR accepts it as the **L1 low-level
> foundation API**, and explicitly defers the **L2 ergonomic public API**. The
> "public" in "too low-level as the public API" means the *end-user* public API
> (L2), which is deferred. L1 is public in the sense of "linkable/visible," but
> it is a power-user surface.

This layered shape is also what makes incremental adoption (016F) possible. Per
the revised ordering: the foundation (017) and a fake backend (019) ship first,
with no coroutine and no io_uring dependency; the read/write op model (018) and
durability ops (018B) build on them; the thread-pool backend (020A) and
cancellation spike (021) follow; the io_uring async backend (020B) and bench
harness (022) come last and are independently abortable.

## 5. Rejected-as-foundation (kept-as-future / kept-as-backend)

```text
Callbacks (1)              : rejected as an API (weak safety/cancel); may be
                             used internally by the completion runtime.
Pollable CQ (2)            : ACCEPTED as the L1 low-level foundation (see ADR).
Coroutine Task<T> (3)      : deferred to L2 — the future ergonomic public layer
                             (ADR "coroutines: later, not now").
Sender/receiver (4)        : rejected now (heaviest, least like Reader/Writer,
                             implies unevaluated P2300 dependency); ideas noted.
Thread pool (5)            : accepted as fallback/test backend behind the seam.
io_uring-native (6)        : accepted as ONE backend behind the seam, validated
                             separately — not the public API.
```

## 6. Cross-links

- Inventory: `docs/async-source-inventory.md` (016A).
- Problem statement: `docs/async-problem-statement.md` (016B).
- Decision: `docs/adr/ADR-async-io-model.md` (016D).
- Preconditions: `docs/async-readiness-gate.md` (016E).
- Implementation split: `docs/async-next-jobs.md` (016F).
