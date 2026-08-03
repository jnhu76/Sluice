# ADR: Unified Explicit I/O Request Contract

**Status:** Accepted
**Date:** 2026-08-02
**Scope:** `sluice_async` request identity, admission, completion, cancellation, capacity,
and lifecycle contracts
**Baseline:** `b20bcc7` (`master`, including PR #60 and PR #61)
**Accepted at:** Phase B implementation (`feat(async): add bounded RequestKey / RequestSlot
reference lifecycle`), branch `feat/bounded-request-slot-reference`, working-tree change
awaiting the user's review/commit.

## Acceptance

This ADR was merged in its complete, normative form via PR #62 (commits `818b8d6`,
`3e80535`, `88f2d03`; merge `503e3fb` on `master`). The full Decision 1–16 text, the I1–I19
formal invariants, the linearization-points table, the architecture compliance gate
(Gate 0–4), and the rejected-alternatives analysis (Alternatives A–G) are all part of the
accepted record. With Phase B implementation beginning against it, the ADR is promoted from
`Proposed` to `Accepted` and becomes binding authority (AGENTS.md §2 tier 2).

On acceptance, the supersessions listed below take effect exactly as written — and only
those listed portions are superseded. In particular:

- The `idle -> binding -> outstanding` two-stage backend claim (Decision 5) now supersedes
  the direct `idle -> outstanding` claim described in ADR-explicit-io-completion-authority
  §§2.2/5/9/10. The access-control boundary, backend-only authority, reap-only publication,
  fail-fast invalid transitions, and idle/ready destruction permission of that ADR remain
  authoritative.
- The reference lifecycle's pending-cancellation-wins + enqueue-no-op + enqueue-in-flight-pin
  protocol (Decision 4, I17, I19) is the selected arbitration scheme. The rejected
  "record-intent-and-terminalize-after-enqueue" alternative (one of Alternatives A–G) is NOT
  implemented.

Acceptance does not assert that the C++ implementation already conforms — it does not. It
asserts that this document is the contract the implementation must now satisfy, and that
divergence from it requires a superseding ADR or closeout note rather than a silent choice
(AGENTS.md §2). Phase B's scope fence (Decision "Next PR" / roadmap Phase B) is binding: it
must not jump to Uring migration, blocking-offload, Scheduler integration, Batch migration,
Runtime wake integration, or a public `RequestHandle`.

## Authority and relationship to existing decisions

This ADR is the target contract for later implementation PRs. It does not describe the current
C++ implementation as already conforming.

On acceptance, this ADR supersedes only the following portions of
[ADR-async-io-model](ADR-async-io-model.md):

- request identity and backend-internal operation-record assumptions;
- the submit admission boundary and post-accept failure classification;
- count-only/global-sequence reap as an ordering authority;
- cancellation targeting and the point at which cancellation releases borrowed resources; and
- lifecycle wording that treats `Completion` readiness alone as the complete request-storage
  lifecycle.

On acceptance, it also supersedes only these details of
[ADR-explicit-io-completion-authority](ADR-explicit-io-completion-authority.md):

- the direct backend claim transition `idle -> outstanding`, replacing it with the private
  `idle -> binding -> outstanding` transition; and
- the protected `try_claim` / `rollback_claim_before_accept` state details needed to implement
  that two-stage claim. The access-control boundary and backend-only authority remain unchanged.

It does **not** supersede the remaining decisions of:

- [ADR-explicit-io-completion-authority](ADR-explicit-io-completion-authority.md), including private
  publication mutators, backend claim authority, reap-only publication, and fail-fast invalid
  transitions, as well as permission to destroy an idle or ready Completion;
- [ADR-execution-model](ADR-execution-model.md), including Scheduler ownership of runnable routing;
- [ADR-application-runtime](ADR-application-runtime.md), including Runtime ownership and explicit
  stop/drain/join ordering; or
- the file-only operation scope, positional I/O semantics, explicit durability operations, or
  `Result<T>`/`IoError` error transport selected by ADR-async-io-model.

The architecture rules most directly governing this decision are AC-2 through AC-7 and AC-10
through AC-15 in
[the Architecture Constitution](../architecture/architecture-constitution.md).

## Executive decision

Sluice will represent every accepted asynchronous I/O request with a stable logical key:

```cpp
struct RequestKey {
    ContextIdentity context;
    SlotIndex slot;
    Generation generation;
};
```

The operation descriptors (`ReadOp`, `WriteOp`, `SyncDataOp`, and `SyncAllOp`) continue to say
*what I/O to perform*. They are not request identities.

The caller continues to own `Completion<T>`. The context/backend owns a construction-time,
bounded arena of reusable `RequestSlot` objects. A successful submit atomically binds one
`Completion` to one `RequestKey` through a private `idle -> binding -> outstanding` protocol. The
`idle -> binding` CAS is the cross-context winner election; only its winner may initialize the
opaque binding, and a release-store to `outstanding` is the commit/accept linearization point. The
slot carries all per-request state needed to reach exactly one terminal result without a new
unbounded allocation after acceptance.

Submission is one five-stage transaction:

```text
reserve -> prepare -> commit/accept -> enqueue -> dispatch
```

`commit` (`accept`) is the successful-submit linearization point. Any failure before it is a
synchronous rejection and leaves the `Completion` idle. Any failure after it is a terminal result
of an accepted request. Backend completion first produces an identity-bearing `backend-ready`
record. Only the designated reap path may publish that result and make the caller's `Completion`
ready.

This restores the important source-derived Zig `std.Io` semantics—explicit operations, reusable
stable operation identity, bounded storage, identity-preserving completion, and separation of
admission from execution result—without copying Zig's ABI. The backend-owned arena is an
**intentional transitional C++ adaptation**, not a claim that caller-owned `Operation.Storage` is
permanently unsuitable.

## Scope and non-goals

This ADR decides the semantic contract needed by future backends, `AsyncIoContext`, Scheduler,
Batch, cancellation, and lifecycle work. It intentionally does not:

- implement `RequestKey`, `RequestSlot`, an arena, or `ReadySink`;
- change any public submit signature in this phase;
- change Scheduler, Runtime, Batch, io_uring, or blocking-offload behavior;
- add a public `RequestHandle`;
- select concrete integer widths, slot layout, capacity defaults, or a wake callback ABI;
- introduce networking, timers, coroutines, P2300, or a new cancellation model; or
- claim that any future conformance test already passes.

## Evidence vocabulary and as-built backend facts

The following tables are the pre-decision fact audit. Evidence labels mean:

- **[Code]** directly observed in current production or test-support source;
- **[Docs]** declared by an accepted ADR, public header, or architecture document;
- **[Test]** exercised by a current test or verification script; and
- **[Inference]** a consequence reasoned from the preceding evidence, not an as-built guarantee.

Paths are repository-relative. These facts describe the `b20bcc7` baseline, not the target model.

### FakeAsyncBackend

| Item | Current fact and evidence |
|---|---|
| submit authority | The backend claims and records an operation while `AsyncIoContext` serializes the call. **[Code]** `include/sluice/async/fake_backend.hpp`, `src/async/async_io_context.cpp` |
| Completion claim point | `try_claim()` occurs before appending backend records. **[Code]** `include/sluice/async/fake_backend.hpp` **[Test]** `tests/async_completion_test.cpp` |
| operation identity | Logical identity is the `Completion*` plus membership in size/void pending and ready deque families; multiple operation kinds share those families. **[Code]** `include/sluice/async/fake_backend.hpp` |
| pending identity | Pending deques retain `Completion*`. **[Code]** `include/sluice/async/fake_backend.hpp` |
| running/kernel identity | None; the fake has no worker or kernel-owned phase. **[Code]** `include/sluice/async/fake_backend.hpp` |
| ready identity | Ready/cancel queues retain `Completion*`; no provenance or generation. **[Code]** `include/sluice/async/fake_backend.hpp` |
| result storage | Staging deques hold result/error input until reap. **[Code]** `include/sluice/async/fake_backend.hpp` |
| per-op allocation | Standard-library deque growth may allocate after claim, including cancel staging. **[Code]** `include/sluice/async/fake_backend.hpp` **[Inference]** allocation failure can strand the claim or cancel path because no unified reserve phase exists. |
| capacity | No configured request capacity. **[Code]** `include/sluice/async/fake_backend.hpp` |
| full behavior | No semantic capacity-full result; allocator failure is not modeled as `would_block`. **[Code]** same source **[Inference]** no bounded-admission proof exists. |
| cancel target | Public cancellation supplies `Completion&`; the backend matches its address. **[Code]** `include/sluice/async/fake_backend.hpp` |
| cancel publication | Cancellation is staged and published by `poll()`/`wait_one()`. **[Code]** fake backend header **[Test]** `tests/backend_conformance_test.cpp` |
| reap API | `poll()`/`wait_one()` return counts and publish directly; identity is not returned upward. **[Code]** `include/sluice/async/async_io_context.hpp` |
| shutdown | Context/backend destruction requires no outstanding completion; there is no admission-close operation. **[Docs]** ADR-async-io-model L11 **[Test]** `tests/completion_authority_death_test.cpp`, `tests/async_io_context_death_test.cpp` |
| accepted-op terminal guarantee | Normal scripted paths terminate, but post-claim allocation failure is not covered. **[Test]** backend conformance tests **[Inference]** AC-4 is not proven under OOM. |
| fd/buffer borrow interval | Existing docs require stability until `Completion` ready; the fake does not encode the borrow. **[Docs]** ADR-async-io-model L1–L10 **[Code]** no slot metadata exists. |

### SyncBackend

| Item | Current fact and evidence |
|---|---|
| submit authority | The backend claims and appends a synthetic pending record. **[Code]** `include/sluice/async/sync_backend.hpp` |
| Completion claim point | `try_claim()` precedes vector insertion. **[Code]** same header |
| operation identity | `Completion*` and vector position; no context/generation key. **[Code]** same header |
| pending identity | A vector entry retains the descriptor, `Completion*`, and cancel bit; no terminal result is stored yet. **[Code]** same header |
| running/kernel identity | None; this backend performs no real syscall. **[Code]** same header |
| ready identity | The pending entry is visited during reap by `Completion*`. **[Code]** same header |
| result storage | The vector retains the descriptor/cancel bit; poll derives the synthetic terminal result at reap time. **[Code]** same header |
| per-op allocation | Vector growth can allocate after claim. **[Code]** same header **[Inference]** admission is not transactional under allocation failure. |
| capacity | No configured request capacity. **[Code]** same header |
| full behavior | No capacity-full outcome. **[Code]** same header |
| cancel target | `Completion&`/pointer lookup. **[Code]** same header |
| cancel publication | `cancel()` marks the entry; reap publishes `canceled`. **[Code]** same header **[Test]** `tests/async_completion_test.cpp` |
| reap API | Count-returning `poll()`/`wait_one()`, no identity event. **[Code]** same header |
| shutdown | Quiescent destruction only; no explicit close/drain API. **[Docs]** ADR-async-io-model L11 |
| accepted-op terminal guarantee | Deterministic normal path exists; vector allocation after claim remains a gap. **[Test]** backend conformance tests **[Inference]** OOM terminality is unproven. |
| fd/buffer borrow interval | Docs require the interval; the synthetic backend neither performs I/O nor records borrow metadata. **[Docs]** ADR-async-io-model **[Code]** same header |

The target classification is therefore **synthetic/reference poll backend**, not a production
synchronous I/O adapter. It may remain installed temporarily for API compatibility, but it must
not be presented as performing production file I/O. A future production synchronous adapter would
need to execute real syscalls and independently pass the backend conformance suite.

### ThreadPoolBackend

| Item | Current fact and evidence |
|---|---|
| submit authority | The backend claims, creates a task record, and starts one OS thread per operation. **[Code]** `src/async/threadpool_backend.cpp` |
| Completion claim point | `try_claim()` occurs before task/thread setup. **[Code]** same source |
| operation identity | `Completion*`, lambda/`std::function` capture, and a historical worker entry. **[Code]** threadpool header/source |
| pending identity | No bounded explicit pending slot; identity is distributed across closures and containers. **[Code]** same source |
| running/kernel identity | A per-operation `std::thread` and captured `Completion*`. **[Code]** same source |
| ready identity | A ready deque record retains `Completion*` and result. **[Code]** same source |
| result storage | Worker-local result is moved into the ready deque. **[Code]** same source |
| per-op allocation | `std::function`, thread construction, vector/deque growth, and ready publication may allocate. **[Code]** header/source |
| capacity | Unbounded accepted request/thread count. **[Code]** `include/sluice/async/threadpool_backend.hpp` **[Docs]** DIV-12 |
| full behavior | No semantic full result; spawn failure is currently converted into an accepted asynchronous terminal error. **[Code]** threadpool source **[Test]** threadpool tests |
| cancel target | `Completion&`, but running operations are not interrupted. **[Code]** threadpool source **[Docs]** DIV-10 |
| cancel publication | No cancel terminal winner for a running syscall; its ordinary result proceeds to reap. **[Code]** same source |
| reap API | Count-returning poll/wait, direct Completion publication, no identity event. **[Code]** same source |
| shutdown | Reap joins finished per-op threads; destruction requires quiescence. **[Code]** same source **[Test]** `tests/threadpool_backend_reap_test.cpp` |
| accepted-op terminal guarantee | Normal syscall return terminates; ready-deque OOM can escape a worker and terminate the process. **[Code]** same source **[Docs]** P0-01 |
| fd/buffer borrow interval | Captures raw fd/buffer references; existing caller contract lasts until Completion ready. **[Docs]** ADR-async-io-model **[Code]** operation captures |

### UringAsyncBackend

| Item | Current fact and evidence |
|---|---|
| submit authority | The backend claims, obtains/prepares an SQE, registers maps, and later submits pending SQEs. **[Code]** `src/async/uring_backend.cpp` |
| Completion claim point | `try_claim()` precedes SQE pressure acquisition; null-SQE failure rolls the claim back. **[Code]** same source **[Test]** completion authority tests |
| operation identity | Backend-generated numeric id plus parallel maps and `Completion*`; it is not `(context, slot, generation)`. **[Code]** same source |
| pending identity | Numeric id in `pending_sqes` and operation maps. **[Code]** same source |
| running/kernel identity | SQE/CQE `user_data` carries the numeric id. **[Code]** same source |
| ready identity | CQE numeric id is reverse-mapped to an operation and Completion pointer. **[Code]** same source |
| result storage | The operation record retains descriptor/result-kind bookkeeping; `reap_ready()` consumes `cqe->res` and publishes it immediately rather than retaining a separate ready-result record. **[Code]** same source |
| per-op allocation | Map/deque insertion can allocate after SQE preparation. **[Code]** same source **[Docs]** P0-02 |
| capacity | Ring queue depth exists, but there is no unified request capacity. **[Code]** uring header/source |
| full behavior | SQE pressure may submit/reap to obtain space; it is not a synchronous bounded-arena `would_block` contract. **[Code]** same source |
| cancel target | `Completion&` is reverse-mapped to numeric operation id; a cancel SQE targets the original `user_data`. **[Code]** same source |
| cancel publication | Cancel CQE is informational; original CQE currently determines the published result. **[Code]** same source |
| reap API | CQEs are drained and Completions published inside count-returning poll/wait. **[Code]** same source |
| shutdown | Real and stub paths require quiescent context destruction; no explicit admission close. **[Code]** uring header/source **[Docs]** ADR-async-io-model L11 |
| accepted-op terminal guarantee | Partial-submit accounting exists, but post-claim registration allocation remains non-transactional. **[Code]** same source **[Docs]** P0-02 |
| fd/buffer borrow interval | SQEs retain raw fd/buffer parameters until CQE/reap; current docs require stability until Completion ready. **[Code]** same source **[Docs]** ADR-async-io-model |

### Scripted/test backends

| Item | Current fact and evidence |
|---|---|
| submit authority | Test backend subclasses use protected claim/publish/rollback capabilities. **[Code]** `tests/support/scripted_async_backend.hpp`, `tests/support/probe_backend.hpp` |
| Completion claim point | Claim occurs before inserting scripted operation records. **[Code]** scripted backend |
| operation identity | Monotonic test id plus Completion-pointer maps; ids are not reusable slot generations. **[Code]** scripted backend |
| pending identity | Test id/map entry. **[Code]** scripted backend |
| running/kernel identity | Scripted phase/id only; no kernel. **[Code]** scripted backend |
| ready identity | Test id and Completion pointer inside staged records. **[Code]** scripted backend |
| result storage | Scripted records hold terminal results. **[Code]** scripted backend |
| per-op allocation | Standard-library maps/queues may allocate after claim. **[Code]** scripted backend |
| capacity | No common configured capacity. **[Code]** scripted backend |
| full behavior | Test scripts model selected failures, not bounded arena pressure. **[Code]** scripted backend |
| cancel target | Completion pointer/test id lookup. **[Code]** scripted backend |
| cancel publication | Scripted staging followed by reap publication. **[Code]** scripted backend |
| reap API | Current `AsyncBackend` count-returning API. **[Code]** scripted backend |
| shutdown | Test fixtures drain or assert quiescence; no lifecycle protocol of their own. **[Test]** backend conformance/death tests |
| accepted-op terminal guarantee | Scripted normal and cancel cases are deterministic; OOM/generation/capacity are not covered. **[Test]** `tests/backend_conformance_test.cpp`, `tests/scripted_backend_test.cpp` |
| fd/buffer borrow interval | Tests exercise existing Completion lifetime, not a slot-encoded borrow contract. **[Test]** async tests **[Inference]** target coverage is still required. |

## Decision 1: descriptors and request identity are separate

`ReadOp`, `WriteOp`, `SyncDataOp`, and `SyncAllOp` remain the public descriptors. A descriptor can
be copied or normalized as needed, but it is never sufficient to identify an accepted instance.

Every accepted request has exactly one `RequestKey = (context, slot, generation)`:

- `context` proves provenance and prevents cross-context use;
- `slot` locates one position in the bounded arena; and
- `generation` distinguishes successive uses of that slot and prevents ABA after reset/reuse.

Exact integer widths, packing, and the representation of `ContextIdentity` are implementation
decisions. `Completion*` may be a validated fast-path index, but it is not logical identity and
cannot replace generation. A key is valid for one accepted request only. Slot reuse increments the
generation before the next key can become visible. A stale key can never cancel, complete, attach a
waiter to, or otherwise mutate the new occupant.

## Decision 2: ownership and the transitional storage adaptation

The first implementation uses:

```text
caller                         context/backend
------                         ---------------
owns Completion<T>             owns bounded RequestSlot arena
owns fd and borrowed buffer    owns request lifecycle and backend scratch
```

This ADR **proposes a transitional divergence** from Zig's caller-owned `Operation.Storage`:

- it preserves current public submit signatures;
- it avoids forcing Runtime, Batch, and copy-pipeline migration into the reference-core PR;
- stable identity and generation do not require public storage layout;
- construction-time arena allocation provides bounded admission;
- reserved result/ready storage supports an allocation-independent accepted terminal path; and
- the implementation can gather realistic layout and performance evidence before exposing storage.

Revisit caller-owned operation storage when benchmarks or a backend ABI demonstrate a material
per-request overhead reduction and the public API migration cost for Runtime, Batch, and copy
pipelines is controlled. Reconsideration requires a separate ADR/API proposal; it is not an
incidental optimization.

The arena may be physically owned by the concrete backend or by `AsyncIoContext`, but there is one
logical capacity and one context provenance domain per context/backend pair. The implementation
must not create two independently oversubscribable request stores.

## Decision 3: logical RequestSlot contents

This ADR freezes logical information, not C++ layout. A `RequestSlot` contains enough state to
complete and audit its request:

- `RequestKey` and `RequestState`;
- operation kind and either the public descriptor or normalized native parameters;
- an unforgeable `CompletionBase*` binding plus the result type/kind needed for safe publication;
- terminal `Result` storage reserved before acceptance;
- fixed or pre-reserved backend scratch;
- an identity-bound enqueue-in-flight pin acquired at commit and acknowledged by the submit path;
- single-waiter registration state, a stable opaque token, and its routing lease; the Scheduler
  owns the referenced Fiber/runnable routing record, not the slot;
- cancellation intent/state;
- ready/pending queue linkage or an identity into a pre-reserved bounded queue;
- fd identity; and
- buffer address, length, direction, and offset.

The slot borrows the user's buffer; it does not copy buffer contents. Operation-specific inline
storage may be a union. Backend scratch must have a statically bounded or construction-time-bounded
representation sufficient for the accepted terminal path.

Each context/backend pair has one logical **leaf slot-lifecycle synchronization domain**. In the
reference implementation it is the same mutex/domain used for slot admission. It serializes waiter
registration, reap's registration-close/token-take/Completion-ready publication, reset/destructor
release, generation increment, and publication of the slot back to the free arena. In particular,
release/reuse cannot enter this domain until the old generation's reap critical section has left
it. A later implementation may replace the mutex with provably equivalent atomics, but it may not
split these transitions into independently racing domains. No code holding this domain calls
ReadySink, Scheduler, user code, or backend progress.

The enqueue-in-flight bit belongs to the same atomic state word or slot-state arbitration domain as
`pending`, `enqueued`, and terminal-winner transitions. It is not a separately allocated object or
a new upward lock edge. Reap acquire-checks that bit before entering Completion-ready publication;
slot release and generation reuse therefore cannot overtake an old submit path that still owns its
enqueue acknowledgement.

## Decision 4: unified state machine

```text
free
  | reserve
  v
reserved
  | prepare
  v
prepared
  | commit / accept
  v
pending
  | enqueue arbitration wins           | pending cancellation wins
  v                                    |
enqueued                               |
  | dispatch                           |
  v                                    |
running / kernel-owned                 |
  | first terminal winner stores result |
  +------------------------------------+
  v
backend-ready
  | reap publication
  v
completion-ready
  | caller reset or ready-Completion destruction releases slot and increments generation
  v
free
```

Backends may merge physical representations—for example a synthetic backend may move directly from
`enqueued` to `backend-ready`, and a dispatch failure may move from `enqueued` to
`backend-ready`—but the semantic boundaries `accepted`, `dispatched`, `backend-ready`, and
`completion-ready` remain observable and testable.

`pending -> enqueued` and `pending -> backend-ready(canceled)` are competing transitions under one
slot-state arbitration domain. If enqueue wins, cancellation continues from `enqueued` under the
ordinary terminal-winner rules. If pending cancellation wins, it stores the canceled result,
publishes the pre-reserved backend-ready linkage, and performs the backend-ready progress signal.
The submit thread then observes that legitimate terminal state and completes enqueue as an
allocation-free no-op: it does not add pending linkage, fail fast, execute the operation, or publish
a second terminal result. The same no-op rule applies if another explicitly permitted terminal
winner moves `pending` to `backend-ready` first. Submit still returns success because commit already
accepted the request.

Commit sets an identity-bound **enqueue-in-flight pin** before publishing Completion
`outstanding`. A pending terminal winner does not clear that pin. Whether enqueue wins or observes
a legitimate `backend-ready` state, its final RequestSlot access release-clears the pin and then it
returns using only local state. Reap may observe the stable backend-ready result/linkage while the
pin is set, but it must leave that linkage unconsumed and must not publish Completion-ready. The
backend-ready condition is level-triggered until eligible reap consumes it; an implementation that
uses edge notifications must re-arm the same logical readiness when enqueue acknowledges the pin.
That re-arm is not a second terminal result or a second ready linkage. Consequently Completion
reset/destruction and generation reuse cannot occur before the original submit has stopped touching
the slot. This is an internal backend-to-reap eligibility rule, not the deferred Phase G
backend-to-Scheduler wake ABI.

### Transition table

| Transition | Authority and synchronization domain | Allocation allowed | Failure semantics | Wake obligation |
|---|---|---|---|---|
| `free -> reserved` | Backend/context admission authority under its arena lock or equivalent atomic free-list operation | No request-time allocation; arena/startup allocation completed before submission | Failure is synchronous `would_block` for capacity; construction/startup failure is separately `no_space`; Completion remains idle | None |
| `reserved -> prepared` | Admission authority; slot remains invisible to progress engines | No resource needed after commit may be newly unbounded; bounded preparation may use already reserved scratch | Validation/preparation failure rolls back to `free`, increments no accepted count, and returns synchronously | None |
| `prepared -> pending` | Backend claim authority; the Completion `idle -> binding` CAS elects one context, which initializes both bindings and the enqueue-in-flight pin under its context/admission domain and release-stores `outstanding` | No | The final release-store is commit. Failure before it rolls back all pre-accept state and leaves Completion idle | None |
| `pending -> enqueued` | Backend queue authority arbitrates with pending terminal winners under the same slot-state CAS/lock and publishes pre-reserved linkage with release ordering only if enqueue wins; either valid outcome ends with the submit path acknowledging its pin | No; operation is `noexcept` | Cannot reject or lose the accepted request. Observing `backend-ready` after a legitimate terminal winner, including pending cancellation, is a successful no-op; enqueue must not link, fail fast, or republish. Only an unknown/illegal state is an invariant failure | If enqueue wins, the progress engine may be notified without Scheduler/Fiber routing. If a terminal path already won, that winner owns the logical readiness; pin acknowledgement preserves or re-arms it when required |
| `enqueued -> running/kernel-owned` | Blocking worker dequeue or kernel submission accounting | No terminal-path dependency on allocation | Transient pressure or partial kernel submission leaves the unsubmitted request enqueued. Only an irrevocable dispatch failure with proof that no userspace/kernel execution reference remains may become a terminal error. | Backend progress notification only if the failure makes a request backend-ready |
| accepted state `-> backend-ready` | First successful terminal-winner transition under slot CAS/lock | No | Winner stores exactly one terminal result; losers do nothing | Publish backend-ready progress through the backend's domain; Scheduler wake ABI is deferred |
| `backend-ready -> completion-ready` | After acquire-observing an acknowledged enqueue pin, designated `poll`/`wait_one`/reap authority validates key/binding and, under the shared leaf slot-lifecycle domain, closes registration, takes any token/lease, installs the terminal result, marks the slot completion-ready, decrements accepted-outstanding, and stages borrow end before the final release-store of Completion ready | No; the synchronous sink uses pre-reserved storage | A still-pinned request remains backend-ready and reap-ineligible without consuming its linkage. Publication or sink-delivery failure after eligibility is an invariant violation; it cannot be converted to a second result | After leaving the domain, deliver one by-value identity event synchronously before reap returns; the sink consumes the extracted delivery lease |
| `completion-ready -> free` | Caller `reset()` or destruction of a ready Completion acquires that same slot-lifecycle domain after reap has left it; enqueue pin must be acknowledged and waiter registration must be closed with no stored token/lease | No | Invalid/stale/double release, a live enqueue pin, or an open/registered waiter state fails fast and cannot affect a later generation | Generation increments and release publishes the slot to the free arena before reuse becomes visible |

`completion-ready` ends fd/buffer borrowing, but the slot remains bound until caller reset or
destruction of the ready Completion completes the slot-release handshake. `result()` may be read
without releasing the slot. Ready Completion destruction remains allowed, as selected by the
Completion Authority ADR, and discards the result while releasing the slot without canceling,
draining, allocating, or waiting for I/O or Scheduler progress. Quiescent backend/context
destruction therefore requires all slots to be free, not merely an absence of kernel work.

## Decision 5: five-stage admission transaction

### Reserve

Reserve one slot and every userspace resource needed after acceptance: terminal result storage,
queue/ready linkage, backend bookkeeping, and required backend scratch. An io_uring backend
reserves bounded userspace dispatch-queue capacity here, but does not acquire or fill an SQE before
commit; actual SQE acquisition/preparation belongs to dispatch so a pre-commit rollback never
leaves a live submission entry behind.

Failure is synchronous. Capacity pressure returns `would_block`; true construction/startup memory
failure returns `no_space`. The `Completion` stays idle, no borrow begins, and no executor can see
the request.

### Prepare

Populate the key, descriptor/native parameters, Completion candidate binding, borrow metadata, and
backend preparation while the slot is invisible. Invalid descriptors return `invalid_argument`;
lifecycle/provenance misuse returns `invalid_state`; unsupported operation capability returns
`not_supported`. Failure releases all reservations and leaves the Completion idle.

### Commit / accept

Commit is the successful `submit_*` linearization point. Different contexts have different
admission locks, so a context lock alone cannot establish the Completion side of the binding. The
Completion therefore has a private `binding` transient in addition to the lifecycle states selected
by the Completion Authority ADR:

```text
idle
  | backend-only CAS elects one submitting context
  v
binding
  | binding payload and slot commit are complete; release-store
  v
outstanding
```

The winning submit performs this protocol while retaining its own context/admission lock:

1. reserve and prepare its candidate `RequestSlot`;
2. CAS the Completion from `idle` to `binding`; a competing context that loses this CAS cannot
   write any Completion binding field and rolls back only its own candidate slot;
3. initialize the Completion's private `RequestKey`, `ContextIdentity`, and slot-release
   capability while `binding` remains exclusively owned by the winner;
4. change the winning slot from `prepared` to `pending`, establish its enqueue-in-flight pin,
   increment accepted-outstanding accounting, and stage the fd/buffer borrow; and
5. release-store the Completion from `binding` to `outstanding` before releasing the context lock.

Step 5 is the commit/accept linearization point. Its acquire observers see the fully initialized
binding and committed slot. Acceptance accounting is not independently published outside the
context lock before Step 5; the fd/buffer borrow also begins logically at this linearization point,
not while its metadata is merely staged. Together the steps make the following logically
indivisible to observers:

```text
Completion <-> RequestKey binding
Completion idle -> outstanding
RequestSlot prepared -> pending
accepted-outstanding accounting increment
fd/buffer borrow begins
enqueue-in-flight pin begins
```

Steps 2–5 must not contain a recoverable operation after the winner has begun installing the
binding. If any pre-linearization invariant failure is represented as rollback, it clears the
winner's private binding fields, reverses slot/accounting/borrow/pin staging, returns the slot to
`free`, and release-stores `binding -> idle` before releasing the context lock. No observer may
read binding payload while the Completion is `binding`: cancel and waiter registration/await
return synchronous `invalid_state`, while reset and destruction fail fast in Debug and Release.
`idle()` and `ready()` both report false, and `result()` remains invalid. This is the only protocol
that authorizes writing the Completion-side `RequestKey`; losing contexts cannot overwrite it.

### Enqueue

Enqueue makes the accepted request visible to the progress engine. It must be allocation-free,
`noexcept`, and incapable of dropping the request. Typical mechanisms are intrusive linkage,
publishing a slot index into a preallocated ring, or changing a persistent queue state.

Enqueue attempts `pending -> enqueued` in the same slot-state arbitration domain used by pending
terminal winners. It has exactly two valid outcomes: it wins and publishes the pre-reserved pending
linkage, or it observes `backend-ready` from a legitimate prior terminal winner and completes as a
successful no-op. Pending cancellation is the required race case. In the second outcome enqueue
must not link, fail fast, or publish another terminal result/linkage; the terminal winner already
owns the logical readiness. After either outcome, enqueue release-acknowledges its identity-bound
pin as its last slot access. If terminal readiness was observed, that acknowledgement preserves or
re-arms progress so an earlier ineligible reap attempt cannot lose the wake. Any other state is an
invariant violation. The `submit_*` call returns success in both valid outcomes because the request
crossed commit before this race.

The post-commit segment through pin acknowledgement is a bounded, allocation-free, `noexcept`
obligation of the submit call. It invokes no user code and does not wait for worker, kernel,
cancellation, reap, Scheduler, or other asynchronous progress. If an edge notification needs
re-arming, enqueue records that decision while acknowledging the pin, leaves the slot-state domain,
and notifies using only stable context/backend state; the acknowledgement remains its final slot
access.

### Dispatch

Dispatch is worker dequeue, SQE acquisition/preparation plus `io_uring_submit`, or equivalent
execution handoff. Transient SQE pressure and a positive partial submit are not terminal failures:
the unsubmitted suffix stays bound and enqueued/submission-pending for allocation-free retry. A
post-commit failure becomes terminal `backend_error` only after the backend proves that no worker,
userspace SQE, kernel request, or future CQE can still refer to or execute that request. The result
is stored in the same slot and later reaped; it is never retroactively reported as submit rejection.

## Decision 6: error vocabulary

| Term | Meaning |
|---|---|
| `invalid_state` | Caller lifecycle/provenance misuse: non-idle Completion, duplicate waiter, disallowed operation after admission close, or direct use of an invalid/stale key. It is never queue-full. |
| `invalid_argument` | Malformed operation: invalid length/buffer contract, impossible offset conversion, or invalid fd parameter form. |
| `would_block` | Temporary configured-capacity pressure: request arena or bounded admission queue full. Completion remains idle and retry is permitted. |
| `no_space` | Genuine memory/startup/resource initialization failure, distinct from a configured full arena. |
| `backend_error` | Platform error or backend failure represented as the rejection cause before commit or, after commit, as the accepted request's terminal error. |
| `canceled` | One possible terminal result of an accepted request after the cancel path wins. |
| `not_supported` | The selected backend/platform does not provide the requested operation or cancellation capability. |

The C++ mapping may reuse existing `IoError` codes or add deliberate codes in an implementation
PR, but it must preserve these distinctions. Submit rejection is not a terminal completion. A
cancel lookup may return `CancelDisposition::not_found` for an absent or stale generation rather
than exposing `invalid_state`; that lookup result still must not be conflated with capacity.

## Decision 7: Completion binding and API compatibility

The initial implementation preserves the current public signatures:

```cpp
Result<void> submit_read(ReadOp, Completion<std::size_t>&);
Result<void> submit_write(WriteOp, Completion<std::size_t>&);
Result<void> submit_sync_data(SyncDataOp, Completion<void>&);
Result<void> submit_sync_all(SyncAllOp, Completion<void>&);
```

On success, the Completion privately binds the opaque `RequestKey`, `ContextIdentity`, and the
allocation-free slot-release capability needed by `reset()` or ready destruction. Only the winner
of the `idle -> binding` CAS may initialize those fields, and acquire observation of `outstanding`
is required before any backend, cancel, or waiter path reads them. Ordinary callers cannot forge,
replace, or inspect fields needed to authorize publication or release. A compatibility cancellation
API may continue to accept `Completion&`; the context must resolve and validate its private key
before asking the backend to cancel.

No public `RequestHandle` is introduced. If callers later need independent request identity after
Completion reset, a public handle requires a separate ADR/API PR.

## Decision 8: fd and buffer borrow lifetime

Borrowing begins at commit/accept and ends only after the terminal result is published through reap
and the Completion is `completion-ready`.

- A `WriteOp` source remains alive, address-stable, and byte-stable; the caller must not mutate it.
- A `ReadOp` destination remains alive, address-stable, and exclusively owned by the operation;
  the caller must neither read nor write it.
- The fd remains the same resource. The caller must not close it, overwrite its numeric identity
  with `dup2`, or allow that number to be reused for another resource.

None of these events ends the borrow: cancel requested, waiter canceled, Fiber canceled,
backend-ready transition, CQE arrival, blocking syscall return, dispatch failure discovery, or
shutdown requested. Dispatch failure must first become a reaped terminal completion. This wording
supersedes any earlier implication that cancel intent itself releases the resources.

## Decision 9: identity-bearing reap

Completion is two-stage:

```text
backend-ready:
    backend has a stable RequestKey and exactly one stored terminal result

completion-ready:
    designated reap path validated the key/binding and published the result
```

Workers, CQE handlers, cancel callbacks, and shutdown helpers may only win/store
`backend-ready`; they cannot make the caller's Completion ready. The internal integration will use
an identity-preserving semantic equivalent of:

```cpp
struct ReadyEvent {
    RequestKey key;
    OperationKind kind;
    OptionalWaiterDelivery waiter; // stable value token + Scheduler routing lease
};

void reap_ready(SynchronousReadySink& sink);
```

This ADR selects the **synchronous ReadySink** ownership model. For each backend-ready request,
reap:

1. acquire-checks that the identity-bound enqueue-in-flight pin is acknowledged; if not, it leaves
   the backend-ready linkage and level-triggered progress condition intact and publishes nothing;
2. validates the `RequestKey` and Completion binding;
3. enters the shared slot-lifecycle domain, closes further registration, and atomically takes an
   optional registered waiter delivery (token plus routing lease);
4. installs the terminal result while it is still private, marks the RequestSlot
   `completion-ready`, decrements accepted-outstanding, and stages the fd/buffer borrow end;
5. as the final publication step in that domain, release-stores the Completion as ready; and
6. releases every slot/admission/backend-progress lock, then invokes the sink with a by-value event
   before the reap call returns.

The release-store in Step 5 is the sole Completion-ready linearization point. Every acquire
observer of ready therefore also observes the immutable terminal result, closed registration with
no stored waiter delivery, completion-ready slot state, accepted-outstanding decrement, and ended
borrow. Those bookkeeping changes are made under the same domain and are not independently
observable as a completed reap transition before the ready publication. Public statistics and
drain/quiescence decisions that read accepted-outstanding must also synchronize through this
domain; an unsynchronized counter sample is not lifecycle authority. Reset/destruction may contend
briefly for the domain after observing ready, but it cannot overtake the reap critical section.

The callback-scoped `ReadyEvent` does not contain a `Completion*` and must not carry a
`RequestSlot*`. Reap holds no backend or slot lock while calling the sink. The sink handler is
`noexcept`, allocation-independent, and invoked exactly once for each Completion-ready publication;
it must not retain the event reference, call user code, wait for I/O, or use the key to reacquire
mutable slot storage. It may copy the by-value key/kind and consume the move-only waiter delivery
through pre-reserved routing state. Consequently a caller that observes ready may reset or destroy
the Completion and release/reuse the slot while the synchronous sink finishes: the sink has no
pointer that can dangle, and the extracted waiter delivery pins only its Scheduler routing record,
not the slot generation. Sink delivery is part of reap and cannot be deferred as a queued
`ReadyEvent` without a new ADR that introduces an explicit event lease/ack protocol. `RoutingLease`
pins only the Scheduler routing record; it is not a lease on ReadyEvent, Completion, RequestSlot,
or context.

This freezes semantics, not syntax. Reap must preserve identity and backend-known order. A
count-returning `poll()` may remain as a compatibility wrapper, but Scheduler must eventually
consume identities instead of scanning all registered Completions. Batch must eventually consume
outcome origin and ready identity rather than reconstructing order with the process-global
`reap_seq`. `reap_seq` may survive only as non-authoritative diagnostics.

## Decision 10: waiter cardinality

`RequestSlot` owns the waiter registration state and, while registered, one opaque
`WaiterDelivery`. Scheduler owns the Fiber/runnable routing record referenced by that delivery and
remains the only authority that routes it. The slot never stores, invokes, or routes a Fiber
directly.

Conceptually, a waiter delivery contains:

```text
WaiterToken = (SchedulerIdentity, RegistrationSlot, RegistrationGeneration)
RoutingLease = move-only authority that pins that Scheduler routing record
```

The token is a stable value handle, never a raw Fiber/runnable pointer. Before registration, the
Scheduler reserves the routing record and creates the token/lease without making terminal delivery
depend on a later allocation. Successful slot registration transfers the lease into RequestSlot.
Duplicate, invalid, or already-ready registration does not transfer the candidate lease; Scheduler
reclaims it or completes inline as appropriate.
Reap and waiter cancellation race to move the token/lease out exactly once; the loser neither
routes nor retires the record. The winning path must consume/acknowledge the lease after routing.
Scheduler cancellation, drain, and shutdown cannot destroy or reuse the routing record while a
slot or synchronous ReadySink owns its lease.

The registration state machine is:

```text
registration_open(no_waiter)
  | register(token, RequestKey)
  v
registration_open(registered(token, lease))
  | reap closes registration,        | waiter cancellation takes delivery
  | takes delivery, publishes ready   v
  v                                  registration_open(no_waiter)
registration_closed(no_waiter)
  + completion route

registration_open(no_waiter)
  | reap closes registration and publishes ready
  v
registration_closed(no_waiter)
```

Here `no_waiter` means no registration or lease remains stored in RequestSlot; an extracted
Scheduler delivery lease may still be in the synchronous sink or cancellation winner and is
independent of slot release/reuse.

Registration and either token-taking transition serialize in the shared slot-lifecycle domain.
Reap holds that domain from closing registration through the Completion-ready release-store,
so no waiter can install a token in between token extraction and ready publication. If registration
wins, reap takes and synchronously delivers that token. If reap wins, registration acquires the
domain only after ready is visible and returns immediately without installing a token. A second
registration while `registration_open(registered(token, lease))` returns synchronous `invalid_state`
without overwriting or consuming the first token. Batch/select are higher-level compositions and do
not turn one request into a multi-waiter request.

Wait cancellation removes or disables only that waiter registration. It does not cancel the I/O,
does not choose a terminal result, and does not end the fd/buffer borrow. Operation cancellation is
an explicit separate action. A waiter-cancellation winner owns the moved delivery until it routes
the cancellation outcome and acknowledges the lease. A reap winner moves the delivery into the
callback-scoped ReadyEvent; ReadySink routes or otherwise resolves it and acknowledges the lease
before returning. Both winning paths leave the slot-lifecycle domain before calling Scheduler.
Phase B proves the abstract transfer and exactly-once rules with fake stable tokens/leases and no
Scheduler modification. Phase F implements and proves actual Scheduler record lifetime,
cancellation, drain, and shutdown integration; Phase B alone is not that evidence.

Slot release requires an acknowledged enqueue pin and `registration_closed(no_waiter)`. Reset or
destruction that encounters a live pin, open registration, or a stored token/lease is a contract
violation and fails fast in Debug and Release; it never silently discards the waiter or increments
generation. Once reap or waiter cancellation has taken the delivery, routing no longer depends on
the slot. The owner of an actual wait must still keep the Completion alive and stable until that
wait has resumed or otherwise stopped using it.

## Decision 11: cancellation target and disposition

Cancellation logically targets `RequestKey`. The phase-1 compatibility API resolves the key from
the Completion's private binding and validates context and generation.

```cpp
enum class CancelDisposition {
    requested,
    already_terminal,
    not_found,
    not_supported,
};
```

Required behavior:

- **pending:** cancellation may directly win `pending -> backend-ready(canceled)` in the same
  slot-state arbitration domain as enqueue. The winner stores the result, publishes the
  pre-reserved ready linkage, and establishes the level-triggered backend-ready progress condition.
  The enqueue pin remains live, so reap cannot publish Completion-ready until a later enqueue
  observation performs its successful no-op and acknowledges the pin;
- **enqueued:** the backend may remove or neutralize the request using its pre-reserved linkage and
  make `canceled` the terminal result before dispatch wins;
- **running blocking syscall:** cancellation records intent and attempts only supported interruption;
  if the syscall later succeeds or fails, the common terminal-winner rule decides the result;
- **kernel-owned io_uring:** the backend may submit a cancel SQE; original CQE, effective cancel,
  and cancel-not-found races are resolved by slot state and generation;
- **backend-ready:** return `already_terminal` and never overwrite the result;
- **completion-ready:** return `already_terminal` while the just-finished generation remains bound;
  after caller reset releases the slot and clears the binding, the same lookup returns `not_found`;
- **unknown/stale generation:** return `not_found` and never act on a reused slot; and
- **backend cannot cancel this operation:** return `not_supported` without changing terminality.

`requested` means the cancellation authority accepted the request: for `pending`, cancellation may
already have won and stored `canceled`; for later states, intent may only have been recorded or an
attempt initiated, so the disposition does not promise that `canceled` will win.

## Decision 12: terminal winner

The first event that successfully transitions the `RequestSlot` into `backend-ready` wins. Winner
candidates are ordinary success, ordinary error, effective cancel, a post-commit dispatch failure
after execution ownership is proven absent, and an explicitly designed shutdown terminal event.
The transition uses one slot lock/CAS domain.

The pending enqueue/cancel race uses that same state arbitration: a cancel winner owns terminal
publication and makes enqueue a no-op, while an enqueue winner transfers the race to the enqueued
cancellation rules. Neither outcome can leave both pending linkage and backend-ready linkage live.
Terminal victory does not clear the enqueue pin or authorize reap to overtake its acknowledgement.

Losers do not publish, overwrite result storage, unlink on their own, decrement accounting twice,
or mutate generation. A cancel SQE result is evidence about the cancellation attempt, not an
independent permission to overwrite the original request.

## Decision 13: bounded capacity and observability

Each context/backend pair has a bounded `request_capacity`. Its arena and all per-slot terminal
resources are allocated at construction. Capacity is explicitly configurable or supplied by a
bounded default. This ADR deliberately defers the numeric default, supported maximum, and exact
memory layout to an implementation design informed by benchmarks and platform limits.

Arena full returns synchronous `would_block`, leaves Completion idle, creates no borrow, and does
not enter backend execution. True arena construction/allocation failure is `no_space`.

At minimum, observability exposes:

- configured request capacity;
- current accepted-outstanding count (commit through Completion-ready);
- current slot-in-use count (reserve through reset/ready-destruction release);
- high-water mark; and
- capacity rejection count, named independently from lifecycle violations.

The following are different controls:

| Control | Meaning and relationship |
|---|---|
| `pipeline_depth` | Application-owned concurrent buffer/window count; it must not exceed usable request capacity for a no-retry pipeline. |
| Runtime worker count | Number of scheduler execution workers; not request storage. |
| blocking-I/O worker count | Number of persistent blocking syscall executors; may be less than request capacity. |
| `request_capacity` | Upper bound on context/backend-owned request slots and accepted/bound requests. |
| `uring_queue_depth` | Kernel submission/completion ring depth; may be less than or equal to request capacity. |

If ring depth or worker count is smaller than request capacity, excess accepted requests may remain
`enqueued` only because their queue position/linkage was reserved before commit. Capacity must be
chosen to cover the largest legitimate outstanding application window; oversubscription is handled
by explicit `would_block`, not hidden growth.

## Decision 14: accepted terminal path cannot depend on new unbounded allocation

After commit, the request must eventually produce exactly one terminal result without requiring a
new unbounded allocation. Reserve must obtain result storage, ready linkage, queue linkage, and
backend bookkeeping. Enqueue, backend-ready publication, dispatch-failure recording, and reap use
that storage. OOM after commit cannot leave a request permanently outstanding or terminate a
worker because it attempted to allocate a ready record.

This does not ban all allocation elsewhere in a backend. It bans allocation as a correctness or
liveness precondition for an already accepted request's terminal path.

## Decision 15: admission close, drain, and destruction

Backend/context destruction remains quiescent and fail-fast:

- backend/context destructors do not implicitly close admission, cancel, drain, or block;
- destroying with a bound/non-free request slot is a contract violation in Debug and Release; and
- there is no implicit mass-cancellation mode in this ADR.

Completion destruction preserves the accepted Completion Authority contract: destroying an idle
or ready Completion is allowed, while destroying an outstanding/publishing/resetting Completion
fails fast. The proposed `binding` transient is also fail-fast for reset and destruction. A ready
Completion destructor must perform the same allocation-free slot release as `reset()` before its
address becomes invalid. This is terminal-storage cleanup, not an implicit I/O cancel or drain.

Here, “does not block” is not a lock-free guarantee. Slot release:

- is allocation-free and must not wait for I/O progress, worker completion, cancellation, drain,
  backend progress, or Scheduler activity;
- may perform one bounded internal synchronization operation under the same leaf slot-lifecycle
  domain used by waiter registration and reap-ready publication;
- does not call user code, invoke a ReadySink, enter Scheduler, or acquire a backend progress lock;
- verifies waiter registration is closed with no stored token/lease, clears the Completion binding,
  increments generation, decrements slot-in-use, and makes the slot reusable in that domain; and
- relies on the lifecycle rule that the context/backend remains alive until every bound slot is
  released; the release capability does not make context destruction race-safe.

The slot-lifecycle domain is a leaf in the lock order: code holding it cannot call upward into
Scheduler or backend progress. Mutex contention for this bounded critical section is permitted;
waiting for an asynchronous actor or condition is not.

The required explicit lifecycle is:

```text
close admission
-> continue progress and reap all accepted requests
-> callers reset or destroy ready Completions and release all slots
-> accepted-outstanding == 0 and slot-in-use == 0
-> destroy
```

The semantic `close_admission` operation atomically prevents new acceptance. New submits then
return synchronous `invalid_state`, with idle Completion and no borrow. Existing pending,
enqueued, running, or kernel-owned operations continue toward their ordinary terminal result;
poll/reap remains legal; operation cancellation remains legal; and waiter cancellation remains
legal. The default drain is graceful and does not silently turn queued requests into `canceled`.

Concrete names and whether the implementation offers `close_admission()`, `drain()`, or a composed
graceful shutdown helper are deferred. Any future abort/cancel shutdown mode requires a separate
design that defines its terminal-winner and platform semantics.

## Decision 16: public AsyncBackend author contract

`AsyncBackend` remains a public extension point. A backend author is part of the trusted computing
base; an ordinary caller is not. Completion publication mutators remain private and backend access
remains the protected capability chosen by the Completion Authority ADR.

Every backend author must implement and pass a common conformance suite covering:

- transactional five-stage admission and rollback;
- RequestKey provenance and generation;
- bounded capacity and distinct full behavior;
- identity-bearing reap and reap-only Completion publication;
- exactly-once terminal/cancel winner semantics;
- explicit close/drain and quiescent destruction;
- no exception escaping a worker thread;
- no loss of an accepted operation; and
- no new unbounded-allocation dependency on the accepted terminal path.

The suite becomes part of the public extension contract. Protected access is not permission to
invent different lifecycle semantics.

## Backend mappings

### FakeAsyncBackend: reference authority

Fake is the first backend migrated. It receives a bounded slot arena and deterministic controls for
completion order, capacity full, pre-commit rejection, post-commit dispatch error, cancel winner,
slot reuse, and stale-generation attempts. It requires no threads or kernel and is the reference
state-machine implementation.

### SyncBackend: synthetic/reference poll backend

Sync is explicitly a synthetic/reference backend, not a production synchronous file-I/O adapter.
It may compute a synthetic result without a syscall, but it still traverses accepted ->
backend-ready -> reap -> completion-ready. Submit never makes Completion ready inline. If it remains
publicly installed during migration, documentation and naming must not imply real production I/O.

### UringAsyncBackend

The target mapping is:

```text
RequestKey -> encoded/indirect SQE user_data -> CQE
           -> validate context/slot/generation -> same RequestSlot backend-ready
```

The implementation should eliminate Completion reverse maps, parallel identity maps, and identity
reconstruction where the slot itself suffices. Reserve/prepare obtain RequestSlot and bounded
userspace dispatch-queue capacity, not an SQE. Dispatch acquires and fills an SQE after commit. If
SQ space is temporarily unavailable or `io_uring_submit` accepts only a prefix, the unsubmitted
suffix remains bound and enqueued/submission-pending for allocation-free retry. A slot may become
terminal or reusable only after the backend proves that no prepared SQE, kernel request, or future
CQE can still reference it. Cancel SQEs target the original key; stale CQEs fail generation
validation; and ring depth remains distinct from request capacity.

### Portable blocking-offload backend

The final backend is a portable blocking-I/O offload mechanism, not a translation of Zig
`Io.Threaded`. It uses a fixed set of persistent workers, a bounded pre-reserved pending queue,
explicit operation kinds, reusable RequestSlots, and no arbitrary `std::function` as the core
request. A worker performs the syscall and publishes only backend-ready. Reap publishes
Completion-ready. Queued and running cancellation follow Decision 11. This ADR does not implement
that replacement.

## Scheduler and Batch migration target

Scheduler will supply a stable value token and routing lease at registration and consume the
delivery taken by reap or waiter cancellation. RequestSlot validates the unique registration;
Scheduler retains sole authority to map the token/epoch to and route the corresponding Fiber, and
the lease prevents early record retirement or reuse. Backends never route Fibers. The
synchronous ReadySink receives no Completion or slot pointer, is invoked without backend/slot
locks held, and invokes no user code. The
mechanism that signals backend-ready progress into Scheduler wake belongs to the later
wake-integration PR; this ADR adds no new lock edge or polling assumption.

Batch must represent whether an outcome originated from synchronous admission rejection or from an
accepted terminal completion. It must not encode this distinction as `reap_seq == 0`. Once
identity-bearing reap is integrated, the process-global reap sequence is removed or demoted to
non-authoritative diagnostics.

## Formal invariants

1. **I1 — Stable identity.** Every accepted request has exactly one
   `(ContextIdentity, SlotIndex, Generation)` key.
2. **I2 — Single binding.** The `idle -> binding` CAS elects exactly one context. Only its winner
   installs one current RequestKey; acquire observation of `outstanding` sees that complete
   binding, and one in-use slot is bound to exactly one Completion.
3. **I3 — Transactional rejection.** A failed submit leaves Completion idle, no accepted slot, no
   borrow, no outstanding-count increment, and no background execution.
4. **I4 — Accepted terminality.** A successful submit eventually yields exactly one terminal
   result, assuming the documented progress/drain obligations are met.
5. **I5 — No identity loss.** RequestKey is preserved from commit through backend-ready and reap.
6. **I6 — Generation safety.** A stale key cannot observe or mutate a later use of the same slot.
7. **I7 — Borrow safety.** fd/buffer borrowing begins only at commit and ends only at
   completion-ready publication.
8. **I8 — Bounded admission.** In-use slots never exceed configured request capacity.
9. **I9 — No post-accept allocation dependency.** Terminal progress for an accepted request does
   not depend on a new unbounded allocation.
10. **I10 — Exactly-once terminal winner.** Success, error, cancel, dispatch failure, and any
    designed shutdown terminal event share one winner transition.
11. **I11 — Reap authority.** Only designated reap code makes a Completion ready.
12. **I12 — Quiescent destruction.** Backend/context destruction is legal only when every slot is
    free and no outstanding callback or kernel ownership remains.
13. **I13 — Single waiter.** At most one waiter is registered for an accepted request; a second is
    rejected synchronously without changing the first.
14. **I14 — Admission/result separation.** A rejection is never fabricated as an accepted async
    completion, and a post-commit error is never retroactively reported as rejection.
15. **I15 — No half-binding observation.** Cancel, await/registration, reset, destruction, and
    publication cannot read or act on binding payload while Completion state is `binding`.
16. **I16 — Non-escaping ready delivery.** Reap closes waiter registration, takes any stable
    token/routing lease, and publishes ready in the shared slot-lifecycle domain. It then
    synchronously delivers a by-value event with no Completion/slot pointer; slot release or
    generation reuse cannot dangle the event, lose a token installed in the publication window, or
    retire the Scheduler routing record before sink acknowledgement.
17. **I17 — Enqueue/cancel arbitration.** Pending enqueue and cancellation share one slot-state
    winner domain. A cancellation winner makes enqueue an allocation-free no-op, so a request can
    never have both pending linkage and backend-ready linkage or execute after pending cancellation.
18. **I18 — Final ready publication.** The Completion-ready release-store occurs after terminal
    result installation, registration closure/token extraction, slot-state update,
    accepted-outstanding decrement, and borrow-end staging. An acquire observer of ready sees all
    of those effects.
19. **I19 — Submit pin before reuse.** Commit establishes an identity-bound enqueue-in-flight pin.
    Reap cannot publish Completion-ready, and reset/destruction cannot release or reuse the slot,
    until enqueue/no-op acknowledges that pin as the old submit path's final slot access.

## Linearization points

| Event | Authority and atomic/lock domain | Observable consequence | Wake obligation |
|---|---|---|---|
| submit accepted | Backend admission authority; Completion `idle -> binding` CAS elects one context, which commits its slot/accounting/enqueue pin and release-stores `binding -> outstanding` under its slot/admission lock | The release-store is the linearization point; acquire observers see the complete binding, pending slot, borrow, accepted-outstanding accounting, and live enqueue pin; submit may return success | None until progress engine/backend-ready |
| submit rejected | Admission authority before commit; same lock domain completes rollback | Completion is idle, slot free, no borrow/execution; submit returns an error | None |
| cancel requested | Backend cancellation authority validates RequestKey, then atomically records intent, queues pre-reserved cancel work, or wins the pending terminal transition | Returns `requested` unless terminal/absent/unsupported; pending cancellation may already have won, while other states make no promise that canceled wins | A pending terminal winner establishes one logical level-ready condition; enqueue acknowledgement re-arms an edge implementation if required |
| terminal winner selected | Slot state CAS/lock changing an accepted state to backend-ready | Exactly one terminal result becomes immutable | Backend-ready progress notification; no direct Fiber routing |
| backend-ready publication | Same terminal transition plus release publication of ready linkage | Reap can observe stable key/result, but cannot consume or publish it while the enqueue pin remains live | Backend level-ready condition remains asserted through pin acknowledgement; Scheduler bridge deferred |
| enqueue acknowledged | Submit path release-clears its identity-bound pin after publishing pending linkage or confirming a terminal no-op; this is its last slot access | The old submit no longer blocks eligible reap, Completion-ready publication, or eventual generation reuse | Preserve level readiness or re-arm an edge notification if a terminal winner preceded acknowledgement |
| Completion-ready publication | After acquire-observing the enqueue acknowledgement, designated reap authority validates key/binding and, under the shared leaf slot-lifecycle domain, closes registration, extracts waiter delivery, installs result, marks the slot completion-ready, decrements accepted-outstanding, and stages borrow end; the final Completion-ready release-store is the linearization point, followed outside the domain by synchronous ReadySink invocation | An acquire observer sees the result and all prior bookkeeping; fd/buffer borrow is ended, the request is absent from accepted-outstanding, registration is closed, and the callback-scoped event has no Completion/slot pointer | Sink consumes the extracted lease before reap returns; no later slot lookup |
| waiter registration | RequestSlot registration authority validates the acquire-observed bound key and stores one Scheduler-supplied stable token/routing lease only while registration is open | Slot state is `registration_open(registered(token, lease))`; a second registration is `invalid_state`; closed registration observes ready without storing | If already completion-ready, return ready without registering |
| waiter cancellation | RequestSlot registration authority moves out the matching token/lease under the same domain; Scheduler owns routing and lease acknowledgement | Wait ends/cancels; I/O and borrow remain active; the losing path cannot retire the routing record | Route only the canceled waiter and acknowledge its lease as required by Scheduler semantics |
| slot release/generation increment | Caller reset or ready-Completion destructor acquires the same leaf slot-lifecycle domain after reap leaves it; requires acknowledged enqueue pin and closed waiter registration with no stored token/lease | Completion binding clears, generation increments, slot-in-use decrements, and only then is the slot published free; the old key is stale before reuse | Capacity observer may be notified within the bounded domain; no user code, Scheduler, backend-progress lock, or Fiber routing |
| admission closure | Context lifecycle authority atomically closes the admission flag under lifecycle/admission domain | Later submit is synchronous `invalid_state`; accepted requests remain drainable | Wake any admission waiters if such an API is later introduced |

## Zig conformance and divergence classification

The local files `zig/lib/std/Io.zig`, `Io/Threaded.zig`, `Io/Uring.zig`,
`Io/Kqueue.zig`, `Io/Dispatch.zig`, and `Io/fiber.zig` were used as
source-derived reference, not copied as an ABI.

| Topic | Classification | Decision |
|---|---|---|
| Explicit typed operation descriptor | Faithful semantic preservation | Separate C++ structs preserve the tagged-operation purpose. |
| Stable reusable request storage and identity-bearing completion | Faithful semantic restoration | Bounded slot/generation identity restores the core property, with different ownership. |
| Caller-owned `Operation.Storage` | Proposed transitional C++ adaptation | If accepted, backend/context owns the first arena to preserve API and stage migration; DIV-02 records the pending decision and revisit trigger. |
| `Pending.Userdata` inline scratch ABI | Proposed C++ adaptation | If accepted, logical bounded scratch is required; exact fixed-word Zig layout is not. |
| Threaded task model | Permanent structural divergence at this layer | Sluice blocking offload is an I/O mechanism, not Zig's task execution strategy. |
| Integrated Evented scheduler/backend wake | Accepted divergence plus deferred capability | Sluice keeps Scheduler routing separate; identity-bearing progress comes first, wake bridge later. |
| Structured cancel-protection regions | Deferred capability | This request contract does not add them. |
| File-only I/O scope and explicit durability ops | Permanent intentional divergence | Existing ADR decisions remain authoritative. |

## Rejected or deferred alternatives

### A. Continue using `Completion*` as the only identity — rejected

It cannot prevent reuse ABA, prove context provenance, reject stale cancellation, or provide one
stable identity across backend records and Scheduler waiter registration. It also preserves the
current identity fragmentation and silent waiter-overwrite risk.

### B. Adopt complete caller-owned `Operation.Storage` immediately — deferred

It would force a simultaneous public API, Runtime, Batch, and copy-pipeline migration. The bounded
backend-owned arena restores the required identity, capacity, and terminal-path properties while
allowing staged evidence. The revisit trigger is explicit in Decision 2 and DIV-02.

### C. Use a generic `std::function` task queue — rejected

It hides allocation, erases operation identity and resource contracts, makes cancel targeting
opaque, and turns an I/O backend into a generic executor contrary to AC-8.

### D. Let workers publish Completion directly — rejected

It bypasses sole reap authority, loses a common ordering/identity boundary, and breaks Scheduler and
Batch integration. Workers and CQE handlers stop at backend-ready.

### E. Use unbounded dynamically allocated request records — rejected

It violates AC-7, makes capacity unobservable, and allows OOM to strand or terminate an accepted
request. Bounded construction-time storage is mandatory.

### F. Fabricate submit rejection as an asynchronous completion — rejected

It conflates admission with execution outcome and causes incorrect retry/idempotency decisions.
Pre-commit failure is synchronous; post-commit failure is terminal.

### G. Make destructors implicitly drain — rejected

It introduces hidden blocking and ambiguous lifecycle authority and contradicts the existing
Release fail-fast contract. Close/drain is explicit; destructor requires quiescence.

## Required implementation sequence

The sequence has no reverse dependency on Scheduler, Runtime, or a persistent worker pool.

1. **Phase A — this ADR:** `docs(adr): define unified Explicit I/O Request Contract`.
2. **Phase B — reference core:**
   `feat(async): add bounded RequestKey / RequestSlot reference lifecycle`; implement only
   RequestKey, the Completion `binding` transient, the enqueue-in-flight pin/ack, arena/slot and
   waiter-token state machines, Fake, Sync/Synthetic, a synchronous non-escaping ReadySink, and the
   initial conformance tests. Use fake stable waiter tokens/leases; do not modify Scheduler.
3. **Phase C — conformance framework:**
   `test(async): enforce explicit request lifecycle across backends`; cover capacity, rejection,
   generation, cancel, dispatch failure, exactly-once, shutdown, and identity-bearing reap.
4. **Phase D — Uring migration:**
   `refactor(async): migrate UringBackend to RequestSlot identity`.
5. **Phase E — blocking-offload migration:**
   `refactor(async): replace per-op threads with bounded blocking-I/O workers`.
6. **Phase F — Scheduler/Batch integration:**
   `refactor(runtime): consume identity-bearing reap events`; remove O(N) Completion scanning,
   global reap ordering authority, and waiter overwrite; implement the real Scheduler routing-record
   token/lease lifecycle across completion, waiter cancel, drain, and shutdown.
7. **Phase G — wake integration:**
   `feat(runtime): bridge backend-ready progress to Scheduler wake` without giving the backend
   Fiber-routing authority.

## Architecture compliance gate

### Gate 0 — classification

| Field | Decision |
|---|---|
| Affected capability | Explicit I/O request lifecycle |
| Affected layers | L0 backend contract; L1 AsyncIoContext contract; future Scheduler/Batch integration |
| Classification | Corrective plus intentional transitional C++ adaptation |
| Zig alignment | Restores explicit operation-storage semantics through a bounded backend-owned RequestSlot arena rather than immediate caller-owned storage |
| Constitution rules | AC-2, AC-3, AC-4, AC-5, AC-6, AC-7, AC-10, AC-11, AC-12, AC-13, AC-14, AC-15 |
| Public API effect in this ADR | None; submit signatures remain directionally unchanged |

### Gate 1 — state and transitions

The complete state machine, transition authorities, allocation rules, failure semantics, and wake
obligations are specified in Decision 4. Commit is the accepted linearization point; backend-ready
and completion-ready are distinct; slot release invalidates the generation before reuse.

### Gate 2 — resources and failures

- Request capacity and terminal-path storage are bounded and allocated at construction.
- Capacity full is synchronous `would_block`; genuine initialization failure is `no_space`.
- Pre-commit failure rolls back with idle Completion and no borrow.
- Post-commit dispatch failure becomes terminal only after execution ownership is proven absent;
  transient/partial dispatch remains enqueued for retry.
- An accepted terminal path does not depend on new unbounded allocation.
- Quiescence requires all slots free; destructor performs no hidden drain.

### Gate 3 — wake and progress

- Backend-ready publishes a stable key/result into pre-reserved ready linkage.
- A live enqueue pin makes that linkage temporarily reap-ineligible without consuming its
  level-triggered progress condition; acknowledgement makes it eligible and re-arms edge-based
  notification when necessary.
- Reap alone makes Completion ready, takes any waiter token/routing lease, and synchronously emits
  a by-value identity event with no Completion/slot pointer.
- Scheduler remains the only Fiber-routing authority.
- This ADR introduces no backend-to-Scheduler lock edge and promises no new polling interval.
- The final backend-ready progress signal/wake interface is Phase G work.

### Gate 4 — future verification obligations (not yet passed)

The implementation/conformance PRs must add evidence for:

- every state transition and invalid transition;
- capacity-full rejection with unchanged Completion;
- injected failure at reserve, prepare, commit boundary, and dispatch, plus proof that enqueue is
  allocation-free and cannot produce an ordinary recoverable failure;
- a deterministic commit/enqueue pause in which pending cancellation wins, an intervening reap
  publishes nothing, Completion remains non-ready, and slot release/reuse remains unavailable;
  resumed enqueue then acknowledges the pin as a no-op without linking or fail-fast, and subsequent
  reap proves submit remains successful, the operation is never dispatched, no old submit access
  reaches a new generation, progress is not lost, and exactly one backend-ready result/linkage and
  Completion publication occur;
- two-context contention for one Completion, proving that only the `idle -> binding` CAS winner
  installs binding payload and that no operation observe a half binding;
- OOM/failure after commit without loss, hang, or worker exception escape;
- identity-bearing completion order and exact RequestKey preservation;
- generation reuse and stale submit/cancel/CQE/waiter attempts;
- cancel races for pending, enqueued, blocking-running, kernel-owned, and backend-ready states;
- io_uring SQE pressure, partial-submit suffix retry, and proof that no stale SQE/CQE can outlive
  terminal publication or slot reuse;
- exactly one terminal winner and one Completion publication;
- duplicate waiter synchronous `invalid_state` and wait-cancel/I/O independence;
- synchronous ReadySink delivery with reset/destruction and generation reuse during the callback,
  proving the event carries no dangling Completion/slot pointer and the extracted token/lease is
  not lost;
- waiter cancel/reap/shutdown races proving the delivery lease pins the Scheduler routing record
  through exactly one route and acknowledgement (Phase F for the real Scheduler);
- exactly one `noexcept`, allocation-independent sink callback per Completion-ready publication;
- acquire observation of Completion ready proving that terminal result installation, registration
  closure/token extraction, completion-ready slot state, accepted-outstanding decrement, and
  borrow end are already visible;
- fail-fast reset/destruction if slot waiter state is still `registered`;
- borrow lifetime through backend-ready, cancel, dispatch failure, and reap;
- graceful close/drain, rejection after admission close, quiescent destroy, and death on non-free
  destroy;
- allocation-free slot release on both ready `reset()` and allowed ready Completion destruction,
  with no wait for I/O/Scheduler/backend progress, no upward lock acquisition, and proof that the
  shared slot-lifecycle domain prevents release/reuse from overtaking old-generation reap;
- backend-agnostic conformance for Fake, Sync/Synthetic, Uring, and blocking offload;
- negative compile proof that ordinary callers cannot claim/publish/rollback or forge a binding;
- Release coverage for fail-fast contracts; and
- TSan coverage for submit/cancel/reap/slot-reuse concurrency when implementation changes land.

No Gate 4 item is marked passed by this documentation-only ADR.

#### Phase B reference-layer evidence (working tree, branch `feat/bounded-request-slot-reference`)

The Phase B implementation closes the reference-layer portion of Gate 4 (Fake/Sync backends +
the bounded `RequestArena` + the `Completion` binding transient + the enqueue-in-flight pin +
the `SynchronousReadySink`). The production-backend rows (io_uring SQE pressure, blocking offload)
remain open for Phase D/E. Evidence map:

| Obligation | Evidence |
|---|---|
| state transitions / capacity-full / generation-on-release / stale-key rejection / distinct counters / allocation-independent terminal | `tests/request_arena_test.cpp` (5 cases); `tests/request_lifecycle_scheme_b_test.cpp` |
| `idle -> binding -> outstanding` two-stage claim; rollback; concurrent exactly-one-winner | `tests/completion_binding_test.cpp` (3 cases) |
| destroy/reset-in-binding fail-fast (Debug AND Release) | `tests/completion_authority_death_test.cpp` |
| the 19-step Scheme-B trace (pending cancel wins; enqueue no-op; reap-ineligible while pinned; exactly-one terminal; generation++; stale not_found) | `tests/request_lifecycle_scheme_b_test.cpp :: pending_cancel_wins_before_enqueue_then_enqueue_noop` |
| exactly-one terminal winner among pending-cancel / dispatch-error / ordinary | `:: exactly_one_terminal_winner` |
| ReadyEvent survives reset/reuse during the sink callback (no dangling pointer) | `:: ready_sink_event_survives_reset_during_callback` |
| duplicate-waiter `invalid_state`; wait-cancel/I/O independence; lease exactly-once | `:: waiter_registration_cardinality`, `:: reap_wins_lease_over_wait_cancel` |
| cancel disposition per state (pending/enqueued/backend_ready/free/reserved/prepared) | `:: cancel_races_per_state` |
| generation reuse rejects stale submit/cancel/reap/register/release | `:: generation_reuse_stale_attempts` |
| acquire observer sees all effects (I18) | `:: acquire_observer_of_ready_sees_all_effects` |
| close_admission rejects new reserve; existing reapable still reaps | `:: close_admission_rejects_new_but_existing_reapable` |
| allocation-free slot release (1000-cycle convergence; generation advanced 1000×) | `:: allocation_free_slot_release_proof` |
| genuine concurrent submit ‖ cancel/reap (TSan, 0 data races); the test asserts the pin is already acknowledged after join (no maskable extra-ack) | `:: concurrent_submit_cancel_enqueue` |
| **backend-level Scheme-B race** (real FakeAsyncBackend submit thread + Completion binding + deterministic commit/enqueue barrier pause; cancel wins pending; resumed enqueue no-ops + acks; poll reaps canceled) | `tests/backend_scheme_b_race_test.cpp` (links `sluice_async_internal_testing` for the `SubmitPauseGate` seam) |
| **zero-allocation accepted path + transactional rejection**: counting + always-throw operator new drives submit → poll → reset (and the would_block / binding-CAS-loss rejection paths) — zero allocations, zero side effects, no result contamination | `tests/reference_backend_no_alloc_test.cpp` (3 cases) |
| release-with-live-pin / release-with-registered-waiter / **reap-without-binding / record-terminal-on-prepared** fail-fast (Debug AND Release) + valid-release control | `tests/request_arena_death_test.cpp` (7 death/control cases) |
| Fake/Sync migrate onto the arena; observable slot lifecycle + capacity rejections (`would_block`, never `invalid_state`) + exactly-once deliveries + caller-handshake slot release | `tests/reference_backend_arena_lifecycle_test.cpp` (6 cases); `tests/completion_binding_test.cpp` (release-capability cases) |
| ordinary caller cannot forge a binding / claim / publish / reap_seq / install or clear the slot-release capability | `scripts/verify-completion-authority-negative-compile.sh` (12/12) |
| ordinary caller cannot mutate slot state/generation/pin/terminal/registration/publication-binding | `scripts/verify-request-arena-negative-compile.sh` (6/6) |
| metric vocabulary (P1-05): `would_block` → `queue_full_retries`; `invalid_state` → `AsyncStats::invalid_state_rejections` (never conflated) | `tests/async_completion_test.cpp :: async_stats_increment_on_submit_poll_wait`; `tests/reference_backend_no_alloc_test.cpp` |

Gate results (command + count): Clang Debug 135/135; Clang Release 135/135; ASan/UBSan 135/135
clean; TSan 135/135 with 0 data races (including the genuine two-thread concurrent case and the
backend-level Scheme-B race); negative-compile 12/12 + 6/6; doc-check PASS. Round-2 review fixes
closed the three Critical findings (transactional pre-commit admission with the publication
binding in the slot record + construction-time bounded ring — C1; the parallel `bindings_` map
removed, reap validates the slot's own binding and publishes Completion-ready through it INSIDE
the leaf domain — C2/C3) and the three Important findings (completed-binding release fails fast
on any failure — I1; `record_terminal` validates state before writing and the unconditional
pin-acknowledge escape hatch is gone — I2; `queue_full_retries` no longer counts lifecycle
violations — I3). Full evidence ledger: `docs/architecture/phase-b-compliance-gate.md`.

### Round-4 review closeout

The round-4 review identified three more findings on the Phase B reference
layer. Two are code fixes; the third is an ADR-deferred vocabulary item that
this closeout records as intentional divergence rather than implementing
behavior the ADR explicitly scopes out.

- **Round-4 finding 1 — running cancel must stay best-effort (Decision 11).**
  `record_terminal()` previously substituted `canceled` for an ordinary
  success when `cancel_intent_` was set, secretly turning best-effort cancel
  into cancel-wins. It now records the REAL result VERBATIM and consumes the
  intent on any winner. `CancelDisposition::requested` is split into
  `terminal_won` (pending/enqueued cancel stored the canceled terminal
  directly — Scheme B) and `intent_recorded` (running cancel recorded intent
  only); the reference backends now tally `canceled_ops` only on
  `terminal_won`, not at intent-request time. A confirmed cancellation
  (valid interruption / cancel CQE winner) records
  `TerminalResult::err(canceled)` explicitly via `record_canceled`. Regression:
  `tests/request_arena_cancel_intent_test.cpp` (6 cases, including a capturing
  test that fails on the pre-fix code with "ordinary success must NOT be
  rewritten to an error").
- **Round-4 finding 2 — terminal authority rejects unstored results.**
  `record_terminal()` now rejects a default-constructed `TerminalResult`
  (`stored == false`) up front via `request_arena_invalid_terminal_fail_fast`
  in BOTH Debug and Release. Recording it would publish a phantom 0-byte
  success and risk a double ready-ring push. `push_ready_locked_()` also
  asserts its invariants (slot is backend_ready with a stored terminal, not
  already linked, ring not at capacity) via
  `request_arena_ready_ring_invariant_fail_fast`. Regression: two new death
  cases in `tests/request_arena_death_test.cpp`
  (`record-terminal-unstored-result`; the ready-ring guard is inspection-
  verified, like the generation-exhaustion guard, since the only push sites
  guarantee the preconditions).
- **Round-4 finding 3 — descriptor validation (Decision 5/6).** This remains
  a DECLARED but DEFERRED vocabulary item for the Phase B reference backends,
  as the original closeout already stated. The reference backends perform no
  real I/O (the fd is a metadata carrier, not a syscall target — the test
  corpus deliberately uses `ReadOp{-1, ...}` as an "unused by fake" sentinel),
  and `BorrowMetadata` carries no offset, so the `invalid_argument` causes
  that ARE representable at this layer would reject reference-backend test
  traffic without backing a real safety property. They are enforced by the
  full-backend prepare paths (Phase D/E). This is recorded as intentional
  divergence in `docs/architecture/divergence-registry.md`.


## Phase B closeout (PR #63 review findings)

The PR #63 review identified five findings and two ADR-completeness gaps in the
first Phase B implementation. All are closed by the reference-layer redesign
that lands in this PR; the contract decisions themselves are unchanged. This
section records how each is realized so the Decision text below remains the
governing authority (per AGENTS.md §2, accepted decisions are not rewritten).

- **Decision 9 — backend-known reap order (finding #3).** Reap no longer scans
  the slot array by physical index. The arena owns a construction-time bounded
  ready-ring of backend_ready slot indices threaded through each slot's
  `ready_next_` link; every terminal-winner transition (`record_terminal`,
  pending/enqueued `cancel`) pushes the slot onto the ring's tail, and reap pops
  from the head. Delivery is therefore in terminal-winner (backend-known) order,
  not slot-index order, for ALL backends (not just Fake).
- **Queue linkage as a reserved slot resource (finding #1).** The side-band
  `HandleRing` FIFO and the per-kind staging deques are removed from
  FakeAsyncBackend. Queue linkage and submission order live IN the slot
  (`ready_next_`, `submit_seq_`); a cancelled or reused slot can never leave a
  stale side-band handle that strands a later accepted op. Terminal evidence
  binds to a stable `RequestKey` at `complete_*`/`cancel` call time.
- **Decision 12 / Decision 14 — terminal binds to identity immediately
  (finding #2).** `complete_oldest_*` calls `record_terminal` directly (no
  staging deque), so a second completion against an already-terminal op is a
  terminal-winner no-op and terminal evidence cannot leak across generations.
  The staging-deque allocation is gone, so the manual-completion path is
  genuinely allocation-free (Decision 14 now proven, not just the auto path).
- **Decision 5 / I19 — stale enqueue is an invariant violation (finding #4).**
  `enqueue()` on a stale handle is no longer masked as a successful no-op. A
  stale handle means the committed submit path's slot moved on while its
  enqueue pin was still live — an I19 reuse-before-ack disaster. It fails fast
  in BOTH Debug and Release (`request_arena_enqueue_stale_fail_fast`). A
  LEGITIMATE `terminal_noop` now means only "observed backend_ready from a
  prior terminal winner."
- **Decision 11 — running-state cancel records intent.** `cancel()` on a
  `running` blocking-syscall slot records `cancel_intent_` and returns
  `intent_recorded` WITHOUT storing a terminal; the syscall's ordinary
  result/error/valid-interruption later competes for the terminal winner via
  `record_terminal`, which records the REAL result VERBATIM (an ordinary
  success is NOT rewritten to canceled — cancel is best-effort, Decision 11).
  A backend that CONFIRMS the cancellation actually took effect (a valid
  interruption, a cancel CQE winner) records `TerminalResult::err(canceled)`
  explicitly and THAT call wins the terminal. pending/enqueued cancel still
  wins the terminal directly (returns `terminal_won` and stores the canceled
  terminal under Scheme B). The Phase B reference backends never enter
  `running`, so the intent path is exercised by `tests/request_arena_cancel_
  intent_test.cpp` driving the `mark_running()` dispatch seam directly; the
  shared arena is now correct for the later ThreadPool/Uring migration.
- **I6 — 64-bit generation with fail-fast (finding #5).** `Generation` is
  `uint64_t`; the arena fail-fasts at `UINT64_MAX` on release
  (`request_arena_generation_exhausted_fail_fast`) rather than silently
  wrapping, so a stale key can never collide with a future occupant (I6's
  absolute wording holds in perpetuity).

`prepare()` descriptor validation (Decision 6 `invalid_argument`) remains a
declared-but-deferred vocabulary item for the reference backends — see the
"Round-4 review closeout" section above and `docs/architecture/divergence-
registry.md` for the recorded divergence (the reference backends perform no
real I/O; enforcement lands in the full-backend prepare paths, Phase D/E).

## Open risks and deferred decisions

- Whether a future API should expose caller-owned `Operation.Storage` after measurement.
- The actual default and maximum `request_capacity` and the per-slot memory budget.
- Portable interruption of a running blocking syscall.
- The final backend-ready-to-Scheduler wake capability and lock-free/lock-order details.
- Whether a public `RequestHandle` is needed independently of Completion.
- The representation of the fixed Completion reset/ready-destruction release capability and any
  completion-ready tombstone, within Decision 15's allocation and synchronization constraints.
- How context identity is generated and retained without creating process-global lifetime hazards.

## Next PR

The next PR should be exactly:

```text
feat(async): add bounded RequestKey / RequestSlot reference lifecycle
```

Its scope is limited to `RequestKey`, the bounded `RequestSlot` arena, FakeAsyncBackend,
the Completion `binding` transient, Sync/Synthetic backend, a synchronous non-escaping
`ReadySink`, fake stable waiter tokens/leases, and conformance tests. It must not jump to the
persistent blocking worker pool, Scheduler integration, Batch migration, Runtime wake integration,
or public `RequestHandle` work.
