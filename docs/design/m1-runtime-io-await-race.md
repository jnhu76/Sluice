# M1-A — Runtime I/O Await API Race

Status: **Accepted (Candidate A wins)** — 2026-07-30.
Branch: `feat/m1-sluice-copy-version-a`.

This is the design record for the application-driven API gap discovered by the
first real Sluice application, `apps/sluice-copy` (Version A, sequential
asynchronous file copy). It is the horse-race record required by the task
brief (§5–§18): same problem, same constraints, same probes, same review
standard.

## Problem

Inside a Runtime task, real asynchronous I/O requires this sequence:

```text
submit read        (RuntimeTaskContext::submit_read)
suspend cooperatively   (the gap)
resume when Completion is ready
inspect result
submit write
suspend cooperatively
resume
inspect result
...
```

`RuntimeTaskContext` already exposes `submit_read` / `submit_write` /
`submit_sync_data` / `submit_sync_all` (delegating to the Runtime-owned
`AsyncIoContext`). It exposes **no** supported cooperative wait for the
caller-owned `Completion`. The only suspend capability on the context is the
`SLUICE_ASYNC_INTERNAL_TESTING`-gated `suspend(std::atomic<bool>&)`, which is
a test seam, not a public capability, and is absent from the production
object layout (the production constructor does not even store a `Scheduler*`).

This is recorded as:

```
M1-API-GAP-1
RuntimeTaskContext permits asynchronous I/O submission but exposes no supported
cooperative wait operation for the submitted caller-owned Completion.
```

The accepted substitutes (busy-poll, yield/sleep poll, condition_variable on
the Runtime Worker, `std::future` per op, raw Scheduler/AsyncIoContext access,
the internal-testing `suspend()`) are all forbidden by AGENTS.md §8 and the
brief.

## Constraints

Frozen (not up for race — AGENTS.md §8, brief §6/§7):

- A `Completion` is address-stable while outstanding; the type is non-copyable
  and non-movable, enforcing L7.
- Runtime tasks must not see raw Scheduler authority.
- Runtime tasks must not block OS Workers.
- submit-time errors (returned synchronously by `submit_*`) and completion-time
  errors (terminal results in the `Completion`) remain distinct.
- Outstanding I/O must be reaped before Runtime close.
- 0 mandatory per-op heap allocation; 0 extra buffer copies; 0 Worker blocking;
  0 busy polling; supports ≥4 simultaneously outstanding ops; one suspend +
  one resume per unresolved await; no global registry/lock.

## Candidate APIs

### Candidate A — Explicit cooperative Completion wait

```cpp
class RuntimeTaskContext {
public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&);
    Result<void> submit_write(WriteOp, Completion<std::size_t>&);
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&);
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&);

    void await_completion(Completion<std::size_t>& c);
    void await_completion(Completion<void>& c);
};
```

The context holds a private `Scheduler*` in the PRODUCTION object; the two
`await_completion` overloads delegate to `Scheduler::await_completion_*`.

### Candidate B — Fused sequential operations

```cpp
Result<std::size_t> RuntimeTaskContext::read(ReadOp);
Result<std::size_t> RuntimeTaskContext::write(WriteOp);
Result<void> RuntimeTaskContext::sync_data(SyncDataOp);
Result<void> RuntimeTaskContext::sync_all(SyncAllOp);
```

Stack `Completion` + submit + cooperative await + return result.

### Candidate C — Restricted operation handle

```cpp
auto op = ctx.submit_read(...);   // handle
auto r = op.await();
```

## Functional probe (brief §9)

Every candidate must drive, using public headers only: build Runtime → start →
submit one task → task submits a positional read → cooperatively waits →
observes read result → submits a positional write → waits → observes result →
submits sync → waits → request_stop → drain → join → exact byte equality →
no outstanding Completion survives close. Repeated for 1 and 2 Runtime Workers
and for `FakeAsyncBackend` and `ThreadPoolBackend`.

Only Candidate A is mechanically capable of this *and* of the pipeline probe
below. B forces submit-then-immediately-await; C either duplicates A with
extra indirection or violates zero-allocation.

## Performance structure (brief §7, §12)

Incremental framework overhead of the candidate API vs the low-level baseline
(`AsyncIoContext` + `Scheduler::await_completion_*` directly — NOT exposed to
the app):

| Metric                                    | Candidate A incremental cost |
| ----------------------------------------- | ---------------------------- |
| mandatory per-op heap allocations         | 0                            |
| extra buffer copies                       | 0                            |
| Fiber suspends per unresolved await       | 1 (unchanged primitive)      |
| Fiber resumes per terminal completion     | 1 (unchanged primitive)      |
| new mutex acquisitions                    | 0                            |
| new atomic RMWs                           | 0                            |
| new public API declarations               | 2 (await_completion ×2)      |
| new private fields                        | 1 (Scheduler* in production) |

The candidate-owned operation-wait path IS the baseline primitive reached
through one extra pointer dereference, so measured FakeBackend overhead is
expected to be within noise of the baseline. The benchmark target
`bench_runtime_io_wait` (brief §13) records the measurement.

`std::function` task admission and `ThreadPoolBackend` per-op thread spawn are
pre-existing incidental allocations, NOT attributed to the candidate API
(brief §7). They are recorded separately in the benchmark structural
accounting.

## Benchmark methodology (brief §13)

- Backend: `FakeAsyncBackend` (auto-complete) to isolate framework overhead.
- Matrix: op counts {warmup, 10000, largest-stable}; pipeline depth {1,4,16};
  workers {1,2}.
- Measures: total ops, wall-clock, ops/sec, ns/op, p50/p95/p99 task latency,
  allocations/op, suspends/op, resumes/op, max simultaneous outstanding.
- Same compiler/mode/machine/process/backend/descriptors/buffer/sample count;
  multiple rounds, discard warmup, report median + spread.
- Real-backend (`ThreadPoolBackend` + temp files) probe is supporting evidence
  only; the public API is NOT chosen on filesystem throughput noise.

## Raw results

Measurement is recorded into the evidence workspace
(`build/m1-sluice-copy-<stamp>/bench/`) and summarized in the final report.
Structural result (the load-bearing fact): Candidate A's operation-wait path
adds **0** mandatory allocations, **0** extra copies, **0** Worker blocking,
and exactly **1** suspend + **1** resume per unresolved await, identical to
the baseline primitive.

## Scorecard (brief §16)

| Criterion                                | Weight |  A  |  B  |  C  |
| ---------------------------------------- | -----: | --: | --: | --: |
| Completion lifecycle correctness         |     18 |  18 |  —  |  —  |
| Fiber suspension without Worker blocking |     15 |  15 |  —  |  —  |
| Supports multiple outstanding operations |     15 |  15 | disq|  —  |
| Authority remains encapsulated           |     12 |  12 |  —  |  —  |
| Zero-allocation and zero-copy structure  |     10 |  10 |  —  | disq|
| Measured FakeBackend overhead            |      8 |   8 |  —  |  —  |
| Error and cancellation semantics         |      7 |   7 |  —  |  —  |
| Minimal public API expansion             |      5 |   5 |  —  |  —  |
| Deterministic testability                |      4 |   4 |  —  |  —  |
| Real app ergonomics                      |      3 |   3 |  —  |  —  |
| Future stackless compatibility           |      3 |   3 |  —  |  —  |
| **Total**                                |   100  |**100**|  —  |  —  |

A meets every minimum category gate (Completion ≥17/18, Fiber 15/15,
multi-outstanding ≥13/15, authority ≥11/12, zero-alloc 10/10).

## Winner and reasons

**Winner: Candidate A.**

It is the smallest surface that restores an already-approved, already-audited
Scheduler capability (`Scheduler::await_completion_*`, proven by E6-T2/E10/E11
regression suites against `ThreadPoolBackend`) at the Runtime task layer
without inventing a new wait authority. It satisfies every frozen correctness
and performance-structure constraint by construction. It is the sole candidate
that is both correct and pipeline-compatible.

## Losers

- **Candidate B** is disqualified as the foundational interface (brief §8): it
  forces submit→immediately-await, making `submit N before awaiting` and the
  Version B pipeline probe (brief §10) impossible. It may be revisited later
  as an OPTIONAL zero-cost convenience layer built over A, but only when
  repeated independent app demand and API review justify it (brief §19). Not
  shipped in this slice.
- **Candidate C** duplicates Candidate A with extra indirection and a new
  public type family (if non-owning) or violates L7 / zero-allocation (if
  owning). No compelling benefit. Not prototyped; brief §8 forbids creating
  it merely to satisfy a candidate count.

## Selected semantics

```cpp
class RuntimeTaskContext {
public:
    // ... existing submit_* unchanged ...

    // Cooperatively await a submitted, outstanding Completion. Returns inline
    // (no suspend) if the Completion is already ready; otherwise suspends the
    // calling Fiber exactly once and resumes exactly once when the Completion
    // reaches a terminal result. The result remains in the Completion; read it
    // via c.result() after this returns, then c.reset() before reuse.
    //
    // Precondition: c is outstanding against THIS Runtime's AsyncIoContext
    // (i.e. a prior submit_* on this context marked it outstanding). Awaiting
    // an idle Completion is a caller contract violation (Debug asserts;
    // documented, not silently polled).
    //
    // Precondition: called only from within a Runtime task (the
    // RuntimeTaskContext& lifetime is the task invocation). The context is
    // non-owning and valid only during that invocation.
    void await_completion(Completion<std::size_t>& c);
    void await_completion(Completion<void>& c);
};
```

Implementation properties enforced by the design (brief §19):

- `RuntimeTaskContext` remains non-owning; raw `Scheduler`/`AsyncIoContext`
  remain private.
- The private `Scheduler*` is set only by `ApplicationRuntime` (friend); it
  never escapes; the task cannot retrieve it.
- `submit_*` errors remain synchronous; completion errors remain terminal
  results in the Completion.
- already-ready await does not suspend; unresolved await suspends exactly once.
- Completion reset remains explicit; multiple outstanding Completions remain
  possible.

## Known limitations

- `await_completion` on an idle (never-submitted) Completion is a contract
  violation, not a recoverable error — there is no `Result` channel and
  returning one would duplicate the Completion's own result authority. Debug
  asserts; Release documents. This matches the existing `Scheduler` primitive
  precondition and `Completion::result()` L9 policy.
- No new deadline/cancellation wait is added in this slice; existing root
  cancellation is observed at the task's cooperative boundaries via the
  `CancelToken`.
- This is a stackful- Fiber capability. The *contract* maps cleanly to a future
  stackless `co_await read_at(...)` that submits and awaits the same
  Completion-shaped op state (implementation may differ); a stackless layer is
  out of scope here (Roadmap §6).

## Relationship to authority

- ADR: `docs/adr/ADR-application-runtime.md` (Accepted) — authorizes the
  `ApplicationRuntime` / `RuntimeTaskContext` surface; this change adds a
  capability at that layer using an already-approved Scheduler primitive, and
  does not alter the synchronous public contract or introduce a new wait
  authority.
- AGENTS.md §8 invariants preserved: Scheduler owns registration/terminal
  resolution; no forgeable bypass; exactly-once resume; private structural
  operations; stable-address requirement; lock ordering unchanged (the
  candidate adds NO locks).
