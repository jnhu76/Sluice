# ADR: Unified Explicit I/O Request Contract

**Status:** Proposed
**Date:** 2026-08-02
**Scope:** `sluice_async` request identity, admission, completion, cancellation, capacity,
and lifecycle contracts
**Baseline:** `b20bcc7` (`master`, including PR #60 and PR #61)

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

It does **not** supersede:

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
`Completion` to one `RequestKey`. The binding remains opaque and unforgeable to ordinary callers.
The slot carries all per-request state needed to reach exactly one terminal result without a new
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

This is an **Accepted transitional divergence** from Zig's caller-owned `Operation.Storage`:

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
- single-waiter registration metadata or a Scheduler-owned waiter key;
- cancellation intent/state;
- ready/pending queue linkage or an identity into a pre-reserved bounded queue;
- fd identity; and
- buffer address, length, direction, and offset.

The slot borrows the user's buffer; it does not copy buffer contents. Operation-specific inline
storage may be a union. Backend scratch must have a statically bounded or construction-time-bounded
representation sufficient for the accepted terminal path.

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
  | enqueue
  v
enqueued
  | dispatch
  v
running / kernel-owned
  | first terminal winner stores result
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

### Transition table

| Transition | Authority and synchronization domain | Allocation allowed | Failure semantics | Wake obligation |
|---|---|---|---|---|
| `free -> reserved` | Backend/context admission authority under its arena lock or equivalent atomic free-list operation | No request-time allocation; arena/startup allocation completed before submission | Failure is synchronous `would_block` for capacity; construction/startup failure is separately `no_space`; Completion remains idle | None |
| `reserved -> prepared` | Admission authority; slot remains invisible to progress engines | No resource needed after commit may be newly unbounded; bounded preparation may use already reserved scratch | Validation/preparation failure rolls back to `free`, increments no accepted count, and returns synchronously | None |
| `prepared -> pending` | Backend claim authority; one coordinated context/admission lock domain plus Completion CAS establishes both bindings | No | This is commit. Failure rolls back all pre-accept state and leaves Completion idle | None |
| `pending -> enqueued` | Backend queue authority publishes pre-reserved linkage with release ordering | No; operation is `noexcept` | Cannot reject or lose the accepted request; unexpected inability is an invariant failure, not an ordinary retry/rejection path | Progress engine may be notified, but no Scheduler/Fiber routing occurs |
| `enqueued -> running/kernel-owned` | Blocking worker dequeue or kernel submission accounting | No terminal-path dependency on allocation | Transient pressure or partial kernel submission leaves the unsubmitted request enqueued. Only an irrevocable dispatch failure with proof that no userspace/kernel execution reference remains may become a terminal error. | Backend progress notification only if the failure makes a request backend-ready |
| accepted state `-> backend-ready` | First successful terminal-winner transition under slot CAS/lock | No | Winner stores exactly one terminal result; losers do nothing | Publish backend-ready progress through the backend's domain; Scheduler wake ABI is deferred |
| `backend-ready -> completion-ready` | Designated `poll`/`wait_one`/reap authority validates key/binding, publishes through `AsyncBackend::publish` semantics, and decrements accepted-outstanding | No; the sink uses pre-reserved storage | Publication failure is an invariant violation; it cannot be converted to a second result | Emit identity-bearing ready event; Scheduler later routes the unique waiter |
| `completion-ready -> free` | Caller `reset()` or destruction of a ready Completion performs a non-blocking context/backend slot-release handshake; representation is an implementation detail | No | Invalid/stale/double release fails safely and cannot affect a later generation | None; generation increments before reuse becomes visible |

`completion-ready` ends fd/buffer borrowing, but the slot remains bound until caller reset or
destruction of the ready Completion completes the slot-release handshake. `result()` may be read
without releasing the slot. Ready Completion destruction remains allowed, as selected by the
Completion Authority ADR, and discards the result while releasing the slot without canceling,
draining, allocating, or blocking. Quiescent backend/context destruction therefore requires all
slots to be free, not merely an absence of kernel work.

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

Commit is the successful `submit_*` linearization point. A single coordinated lock/atomic domain
must make the following logically indivisible to observers:

```text
Completion <-> RequestKey binding
Completion idle -> outstanding
RequestSlot prepared -> pending
accepted-outstanding accounting increment
fd/buffer borrow begins
```

The Completion CAS remains the publication-authority mechanism selected by PR #61. Implementations
may sequence private writes under the admission lock, but no partial binding may escape: if the
claim fails, every prepared slot field is rolled back before the lock is released.

### Enqueue

Enqueue makes the accepted request visible to the progress engine. It must be allocation-free,
`noexcept`, and incapable of dropping the request. Typical mechanisms are intrusive linkage,
publishing a slot index into a preallocated ring, or changing a persistent queue state.

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
non-blocking release capability needed by `reset()` or ready destruction. Ordinary callers cannot
forge, replace, or inspect fields needed to authorize publication or release. A compatibility
cancellation API may continue to accept `Completion&`; the context must resolve and validate its
private key before asking the backend to cancel.

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
    CompletionBase* completion;
    OperationKind kind;
};

void reap_ready(ReadySink& sink);
```

This freezes semantics, not syntax. Reap must preserve identity and backend-known order. A
count-returning `poll()` may remain as a compatibility wrapper, but Scheduler must eventually
consume identities instead of scanning all registered Completions. Batch must eventually consume
outcome origin and ready identity rather than reconstructing order with the process-global
`reap_seq`. `reap_seq` may survive only as non-authoritative diagnostics.

## Decision 10: waiter cardinality

An accepted request supports at most one registered waiter. A second registration returns a
synchronous `invalid_state`; it never silently overwrites the first registration and is not a
Release-only hang or fail-fast shortcut. Batch/select are higher-level compositions and do not
turn one request into a multi-waiter request.

Wait cancellation removes or disables only that waiter registration. It does not cancel the I/O,
does not choose a terminal result, and does not end the fd/buffer borrow. Operation cancellation is
an explicit separate action.

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

- **pending/enqueued:** the backend may remove the request using its pre-reserved linkage and make
  `canceled` the terminal result;
- **running blocking syscall:** cancellation records intent and attempts only supported interruption;
  if the syscall later succeeds or fails, the common terminal-winner rule decides the result;
- **kernel-owned io_uring:** the backend may submit a cancel SQE; original CQE, effective cancel,
  and cancel-not-found races are resolved by slot state and generation;
- **backend-ready:** return `already_terminal` and never overwrite the result;
- **completion-ready:** return `already_terminal` while the just-finished generation remains bound;
  after caller reset releases the slot and clears the binding, the same lookup returns `not_found`;
- **unknown/stale generation:** return `not_found` and never act on a reused slot; and
- **backend cannot cancel this operation:** return `not_supported` without changing terminality.

`requested` means cancel intent was recorded or a cancellation attempt was initiated; it does not
promise that `canceled` will win.

## Decision 12: terminal winner

The first event that successfully transitions the `RequestSlot` into `backend-ready` wins. Winner
candidates are ordinary success, ordinary error, effective cancel, a post-commit dispatch failure
after execution ownership is proven absent, and an explicitly designed shutdown terminal event.
The transition uses one slot lock/CAS domain.

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
fails fast. A ready Completion destructor must perform the same allocation-free, non-blocking slot
release as `reset()` before its address becomes invalid. This is terminal-storage cleanup, not an
implicit I/O cancel or drain.

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

Scheduler will consume `ReadyEvent`/`RequestKey`, validate the unique waiter registration, and
retain sole authority to route the corresponding Fiber. Backends never route Fibers. The mechanism
that signals backend-ready progress into Scheduler wake belongs to the later wake-integration PR;
this ADR adds no new lock edge or polling assumption.

Batch must represent whether an outcome originated from synchronous admission rejection or from an
accepted terminal completion. It must not encode this distinction as `reap_seq == 0`. Once
identity-bearing reap is integrated, the process-global reap sequence is removed or demoted to
non-authoritative diagnostics.

## Formal invariants

1. **I1 — Stable identity.** Every accepted request has exactly one
   `(ContextIdentity, SlotIndex, Generation)` key.
2. **I2 — Single binding.** One outstanding/non-idle Completion is bound to exactly one current
   RequestKey, and one in-use slot is bound to exactly one Completion.
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

## Linearization points

| Event | Authority and atomic/lock domain | Observable consequence | Wake obligation |
|---|---|---|---|
| submit accepted | Backend admission authority; coordinated slot/admission lock plus Completion CAS | Binding becomes current, slot is pending, borrow and accepted-outstanding accounting begin, submit may return success | None until progress engine/backend-ready |
| submit rejected | Admission authority before commit; same lock domain completes rollback | Completion is idle, slot free, no borrow/execution; submit returns an error | None |
| cancel requested | Backend cancellation authority validates RequestKey then atomically records intent or queues pre-reserved cancel work | Returns `requested` unless terminal/absent/unsupported; no promise that canceled wins | Notify backend progress engine only if needed |
| terminal winner selected | Slot state CAS/lock changing an accepted state to backend-ready | Exactly one terminal result becomes immutable | Backend-ready progress notification; no direct Fiber routing |
| backend-ready publication | Same terminal transition plus release publication of ready linkage | Reap can observe stable key/result | Backend domain signal; Scheduler bridge deferred |
| Completion-ready publication | Designated reap authority under backend/context reap domain plus Completion publish CAS | Caller may observe result; fd/buffer borrow ends; accepted-outstanding decrements; ReadyEvent is delivered | Scheduler may route the registered waiter using the event |
| waiter registration | Scheduler registration authority validates bound key under Scheduler global/registration lock | One waiter owns the current request wait registration | If already completion-ready, enqueue that waiter once |
| waiter cancellation | Scheduler registration authority removes/disarms the matching waiter token/key | Wait ends/cancels; I/O and borrow remain active | Wake only the canceled waiter as required by Scheduler semantics |
| slot release/generation increment | Caller reset or ready-Completion destructor invokes the non-blocking context release capability under the slot/admission domain | Completion binding clears, slot-in-use decrements, slot becomes free, old key becomes stale before reuse | Capacity waiter/observer may be notified; no Fiber routing implied |
| admission closure | Context lifecycle authority atomically closes the admission flag under lifecycle/admission domain | Later submit is synchronous `invalid_state`; accepted requests remain drainable | Wake any admission waiters if such an API is later introduced |

## Zig conformance and divergence classification

The local files `zig/lib/std/Io.zig`, `Io/Threaded.zig`, `Io/Uring.zig`,
`Io/Kqueue.zig`, `Io/Dispatch.zig`, and `Io/fiber.zig` were used as
source-derived reference, not copied as an ABI.

| Topic | Classification | Decision |
|---|---|---|
| Explicit typed operation descriptor | Faithful semantic preservation | Separate C++ structs preserve the tagged-operation purpose. |
| Stable reusable request storage and identity-bearing completion | Faithful semantic restoration | Bounded slot/generation identity restores the core property, with different ownership. |
| Caller-owned `Operation.Storage` | Intentional transitional C++ adaptation | Backend/context owns the first arena to preserve API and stage migration; DIV-02 records the revisit trigger. |
| `Pending.Userdata` inline scratch ABI | Intentional C++ adaptation | Logical bounded scratch is required; exact fixed-word Zig layout is not. |
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
   RequestKey, arena/slot state machine, Fake, Sync/Synthetic, an independent ReadySink, and the
   initial conformance tests. Do not modify Scheduler.
3. **Phase C — conformance framework:**
   `test(async): enforce explicit request lifecycle across backends`; cover capacity, rejection,
   generation, cancel, dispatch failure, exactly-once, shutdown, and identity-bearing reap.
4. **Phase D — Uring migration:**
   `refactor(async): migrate UringBackend to RequestSlot identity`.
5. **Phase E — blocking-offload migration:**
   `refactor(async): replace per-op threads with bounded blocking-I/O workers`.
6. **Phase F — Scheduler/Batch integration:**
   `refactor(runtime): consume identity-bearing reap events`; remove O(N) Completion scanning,
   global reap ordering authority, and waiter overwrite.
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
- Reap alone makes Completion ready and emits identity to its consumer.
- Scheduler remains the only Fiber-routing authority.
- This ADR introduces no backend-to-Scheduler lock edge and promises no new polling interval.
- The final backend-ready progress signal/wake interface is Phase G work.

### Gate 4 — future verification obligations (not yet passed)

The implementation/conformance PRs must add evidence for:

- every state transition and invalid transition;
- capacity-full rejection with unchanged Completion;
- injected failure at reserve, prepare, commit boundary, and dispatch, plus proof that enqueue is
  allocation-free and cannot produce an ordinary recoverable failure;
- OOM/failure after commit without loss, hang, or worker exception escape;
- identity-bearing completion order and exact RequestKey preservation;
- generation reuse and stale submit/cancel/CQE/waiter attempts;
- cancel races for pending, blocking-running, kernel-owned, and backend-ready states;
- io_uring SQE pressure, partial-submit suffix retry, and proof that no stale SQE/CQE can outlive
  terminal publication or slot reuse;
- exactly one terminal winner and one Completion publication;
- duplicate waiter synchronous `invalid_state` and wait-cancel/I/O independence;
- borrow lifetime through backend-ready, cancel, dispatch failure, and reap;
- graceful close/drain, rejection after admission close, quiescent destroy, and death on non-free
  destroy;
- allocation-free slot release on both ready `reset()` and allowed ready Completion destruction;
- backend-agnostic conformance for Fake, Sync/Synthetic, Uring, and blocking offload;
- negative compile proof that ordinary callers cannot claim/publish/rollback or forge a binding;
- Release coverage for fail-fast contracts; and
- TSan coverage for submit/cancel/reap/slot-reuse concurrency when implementation changes land.

No Gate 4 item is marked passed by this documentation-only ADR.

## Open risks and deferred decisions

- Whether a future API should expose caller-owned `Operation.Storage` after measurement.
- The actual default and maximum `request_capacity` and the per-slot memory budget.
- Portable interruption of a running blocking syscall.
- The final backend-ready-to-Scheduler wake capability and lock-free/lock-order details.
- Whether a public `RequestHandle` is needed independently of Completion.
- The representation of the fixed non-blocking Completion reset/ready-destruction release
  capability and any completion-ready tombstone.
- How context identity is generated and retained without creating process-global lifetime hazards.

## Next PR

The next PR should be exactly:

```text
feat(async): add bounded RequestKey / RequestSlot reference lifecycle
```

Its scope is limited to `RequestKey`, the bounded `RequestSlot` arena, FakeAsyncBackend,
Sync/Synthetic backend, an independent `ReadySink`, and conformance tests. It must not jump to the
persistent blocking worker pool, Scheduler integration, Batch migration, Runtime wake integration,
or public `RequestHandle` work.
