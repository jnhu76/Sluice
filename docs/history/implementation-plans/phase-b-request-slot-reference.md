# Design of Phase B — Bounded RequestKey / RequestSlot Reference Lifecycle

> **Archived 2026-08-25 (issue #167 Step 5).** Moved from `docs/design/`;
> classification at move: CLOSED-HISTORY (implemented design; all four
> backends on the RequestArena lifecycle). Body preserved as-written; see
> `docs/history/README.md`.

**Author:** jnhu
**Date:** 2026-08-02
**Status:** Accepted (governs the `feat/bounded-request-slot-reference` implementation)
**Governing ADR:** [ADR-explicit-io-request-contract](../../adr/ADR-explicit-io-request-contract.md) (Accepted)
**Constitution rules touched:** AC-2, AC-3, AC-4, AC-5, AC-6, AC-7, AC-10, AC-11, AC-12,
AC-13, AC-14, AC-15

This design fills the design-compliance gate (Gate 0–4) for the Phase B reference lifecycle
PR. It is binding for the implementation; divergence requires a superseding ADR or closeout
note (AGENTS.md §2). The companion evidence/status document is
`docs/history/closeout/phase-b-compliance-gate.md`.

## Phase B closeout (PR #63 review findings)

The PR #63 review found five issues plus two ADR-completeness gaps in the first
implementation. The authoritative closeout text is in the governing ADR's
"Phase B closeout" section; this design note records the resulting as-built
state-machine / resource-model changes so §5–§14 below stay accurate:

- **Queue linkage + reap order live in the arena, not a side-band ring.** The
  FakeAsyncBackend `HandleRing` FIFO and per-kind staging deques are deleted.
  `RequestSlot` gains `ready_next_` (ready-ring link) and `submit_seq_`
  (submission order); the arena owns a construction-time bounded ready-ring
  (`ready_head_`/`ready_tail_`/`ready_count_`). A terminal-winner transition
  pushes the slot onto the ring's tail; reap pops from the head, delivering in
  backend-known (terminal-winner) order (Decision 9) for ALL backends. This is
  the §9 state-machine authority for `backend_ready -> completion_ready` and
  resolves review findings #1 (no stale side-band handle can strand a later op)
  and #3 (reap order is not physical slot order).
- **Terminal binds to identity immediately.** `complete_oldest_*` calls
  `record_terminal` directly (no staging deque); a second completion against an
  already-terminal op is a no-op (terminal-winner rule). Review finding #2
  (cross-generation terminal pollution) is closed, and the manual-completion
  path is genuinely allocation-free (Decision 14).
- **Running-state cancel records intent (best-effort, Decision 11).**
  `RequestSlot` gains `cancel_intent_`; `cancel()` on a `running` slot sets
  intent + returns `intent_recorded` WITHOUT storing a terminal;
  `record_terminal` records the REAL result VERBATIM (an ordinary success is
  NOT rewritten to canceled — cancel is best-effort). A backend that CONFIRMS
  the cancellation actually took effect records
  `TerminalResult::err(canceled)` explicitly via `record_canceled`, and THAT
  call wins the terminal. pending/enqueued cancel returns `terminal_won` and
  stores the canceled terminal directly (Scheme B). Dormant at the reference
  layer (Fake/Sync never enter `running`); exercised by
  `request_arena_cancel_intent_test.cpp` driving the `mark_running()` seam.
- **Stale enqueue fail-fasts.** `enqueue()` on a stale handle is an I19
  reuse-before-ack invariant violation, not a successful no-op; it calls
  `request_arena_enqueue_stale_fail_fast` (review finding #4).
- **64-bit generation.** `Generation` is `uint64_t`; release fail-fasts at
  `UINT64_MAX` (`request_arena_generation_exhausted_fail_fast`) so I6 holds in
  perpetuity (review finding #5).

---

## 1. Problem

The async I/O subsystem has no stable request identity, no bounded admission, and no
identity-bearing reap. Every backend keys outstanding operations by raw `Completion<T>*`
pointer (FakeAsyncBackend deques, SyncBackend vectors, ThreadPoolBackend ready deques,
UringAsyncBackend maps). Open findings this design closes at the reference layer:

- **P0-01 / P2-03** — no pre-reserved terminal result/ready linkage; worker/container OOM
  can strand an accepted op.
- **P0-02** — `register_op` container allocation after claim/SQE is non-transactional.
- **P1-02 / P1-07** — reap discards identity; no distinct `backend_ready` vs
  `completion_ready`; no synchronous pointer-free `ReadyEvent`/`ReadySink`.
- **P1-05** — `invalid_state`, `would_block`, `no_space` are not distinct; capacity rejections
  have no dedicated metric.
- **P1-06 / P1-10** — no `(context, slot, generation)` identity; ABA on slot reuse; no
  private Completion `binding` transient; no RequestSlot-owned stable waiter token/routing
  lease.
- **P1-08 / P1-09** — cancellation targets a pointer, not a key; no explicit disposition.

Phase B delivers the reference lifecycle for `FakeAsyncBackend` and `SyncBackend` only. It
explicitly does **not** migrate Uring, ThreadPool, Scheduler, Batch, Runtime, or introduce a
public `RequestHandle`.

## 2. Current Authority

- **Claim/publish authority** — `include/sluice/async/completion.hpp` (private mutators,
  `friend AsyncBackend`) and `include/sluice/async/async_io_context.hpp:99-126` (protected
  `try_claim` / `publish` / `rollback_claim_before_accept`). ADR-explicit-io-completion-
  authority (Accepted) is the governing contract for the access boundary.
- **Submit routing** — `src/async/async_io_context.cpp:96-123` serializes every `submit_*`
  through `access_mtx_` and tallies `AsyncStats`.
- **Reference backends** — `include/sluice/async/fake_backend.hpp`,
  `include/sluice/async/sync_backend.hpp` (pointer-keyed tracking today).
- **Error model** — `include/sluice/error.hpp` (no `invalid_argument`/`not_found`/
  `not_supported` today).
- **Fail-fast plumbing** — `include/sluice/async/detail/fail_fast.hpp` +
  `src/async/fail_fast.cpp` (named `[[noreturn]] noexcept` entries → `std::terminate`).
- **Test harness** — `tests/harness.hpp` (`SLUICE_TEST_CASE` / `SLUICE_CHECK` /
  `SLUICE_MAIN`); POSIX death harness `tests/death_test_runner_posix.hpp`; negative-compile
  pattern `scripts/verify-completion-authority-negative-compile.sh`.

## 3. Affected Capabilities

```text
Capability:   AsyncIoContext + AsyncBackend (reference backends FakeAsyncBackend, SyncBackend)
Layer:        L0 backend contract; L1 AsyncIoContext contract
Holder:       caller holds AsyncIoContext; context owns the backend; backend owns the arena
```

Scheduler/Batch/Runtime are NOT modified. The synchronous core (`sluice_core`) is NOT
modified (only `include/sluice/error.hpp`, a shared header, gains three enum enumerators).
The `sluice-copy` app gains one required consumer-side fix (reset of the sync Completion
before context destruction) — mandated by the accepted release-at-reset contract (ADR
Decision 15), not new feature work.

## 4. As-Built Path (today)

```text
AsyncIoContext::submit_read(op, c)
  lock access_mtx_
  backend->submit_read(op, c)
    try_claim(c)                 // idle -> outstanding CAS (BACKEND authority)
    on claim failure: return invalid_state
    record op in pointer-keyed container (deque/vector/map)
  tally_submit(stats, r)
  unlock
```

Reap: `poll()` / `wait_one()` walks the pointer-keyed container and calls `publish(c, result)`
(outstanding → publishing → ready) for each settled op. Cancel records intent on the matched
pointer; the reap path publishes `canceled`. Identity is the `Completion*` pointer. Capacity
is unbounded (or backend-specific). No `binding` transient; no enqueue pin; no generation.

## 5. Proposed Path (Phase B)

Five-stage admission, transactional, allocation-free after commit (ADR Decision 5):

```text
AsyncIoContext::submit_read(op, c)
  lock access_mtx_
  backend->submit_read(op, c)
    [NEW] reserve   : acquire a free RequestSlot + ALL post-accept resources
                      (terminal Result storage, ready/pending linkage, scratch, waiter
                      storage, the type-erased Completion publication binding) from the
                      bounded RequestArena. Full -> would_block.
    [NEW] prepare   : write RequestKey, op kind/params, Completion candidate, fd/buffer
                      borrow metadata. Malformed -> invalid_argument;
                      lifecycle/provenance -> invalid_state; capability -> not_supported.
    [NEW] binding   : install the slot's Completion publication binding (opaque
                      Completion*, requested_bytes, publish thunk) BEFORE the CAS —
                      the slot IS the identity carrier; there is NO parallel map.
    [NEW] commit    : CAS Completion idle -> binding (elects ONE submitting context).
                      Winner writes private ContextIdentity/release-capability,
                      slot prepared -> pending, sets enqueue-in-flight pin, accepted_out++,
                      stages borrow, release-stores Completion binding -> outstanding.
                      [LP: submit-success] Loser: rollback own slot only, return invalid_state.
                      Pre-commit bookkeeping is transactional (review C1): the fake's
                      submission-order FIFO is a construction-time bounded ring and every
                      failure path rolls the reservation back with zero side effects.
    [NEW] enqueue   : noexcept, allocation-free. pending -> enqueued (link pending queue,
                      dispatch) OR observes backend_ready (terminal winner first) ->
                      successful no-op (no link, no dispatch, no fail-fast, no overwrite, no
                      second ready linkage). Either way: release-ack enqueue pin as final
                      slot access.
    dispatch        : Fake/Sync deterministically transition to backend_ready but do NOT
                      make Completion ready inline.
  tally_submit(stats, r)
  unlock
```

Reap (ADR Decision 9; allocation-free SINGLE-DOMAIN protocol — review C3: the
Completion-ready release-store is the leaf domain's own linearization point):

```text
poll() / wait_one()
  per slot (ONE lock acquisition per eligible slot):
    acquire-check enqueue pin. If still live: leave backend_ready linkage
      unconsumed, publish nothing, no accepted_out--, no borrow end
      (reap-ineligible). Continue.
    validate RequestKey + the slot's Completion publication binding
      (missing binding -> request_arena_missing_binding_fail_fast — NEVER a
      silent skip; a silent drop would lose an accepted request, AC-4)
    close waiter registration; take any token/lease exactly-once into the
      by-value ReadyEvent on the stack
    slot -> completion_ready; accepted_out--; backend_ready_count--; borrow ends
    publish the Completion THROUGH the slot-bound thunk (ready release-store =
      completion-ready LP; an acquire observer of ready sees every effect — I18)
  release the lock
  invoke SynchronousReadySink::on_ready(ReadyEvent{key, kind, waiter}) noexcept
```

No ready-record vector and no per-slot publish flag are allocated (I9 / Decision
14): the state transition itself (backend_ready -> completion_ready under the
leaf domain) is the exactly-once authority — a concurrent reap observes
completion_ready and skips. The slot is never touched after the lock is
released (a caller may reset/reuse it while the sink runs — I16).

Reset / ready destruction (`Completion::reset()` / `~Completion()` at `ready`):

```text
  CAS ready -> resetting
  use the installed release capability (arena + slot handle; installed by the
  binding CAS winner at commit) to enter the leaf slot-lifecycle domain
    release_completed_binding: verify enqueue pin acknowledged + registration
    closed(no_waiter) + slot is completion_ready; ANY failure is an internal
    protocol violation and fails fast (review I1 — a silently-failed release
    would leave the old slot permanently slot_in_use while the Completion
    becomes reusable; a later context destruction fail-fast). Pre-commit
    rollback is a SEPARATE authority (rollback_reserved_or_prepared) with
    ordinary recoverable errors.
    clear Completion binding; generation++; slot_in_use--; publish slot free
  leave domain
  release-store idle
```

The slot therefore remains bound (slot_in_use == 1) from commit until the
caller's reset/ready-destruction — capacity accounting covers the caller's
unreset ready Completions, as the ADR requires. The arena destructor fails fast
(debug AND release) if any slot is still bound, so the Completion-bound release
capability can never dangle.

## 6. Zig Source-Derived Comparison

```text
Zig concept:       Operation.Storage (caller-owned request lifecycle + backend scratch)
Zig file:          zig/lib/std/Io.zig (Operation.Storage, Completion)
Direction:         away (deliberate transitional divergence)
Conformance class: I (Intentional Divergence, registered as DIV-02)
```

Phase B keeps caller-owned `Completion<T>` and adds a backend-owned bounded `RequestSlot`
arena rather than moving storage into the caller. DIV-02 is activated for Phase B (reference
backends). Revisit trigger: caller-owned storage shows material per-request overhead
reduction at controlled public-API migration cost.

## 7. Classification

- [x] **Intentional Divergence** — registered (DIV-02). The bounded backend-owned arena is a
  deliberate C++ adaptation that preserves public submit signatures while restoring stable
  identity, bounded admission, and an allocation-independent terminal path. Corrective with
  respect to the open findings (P0-01/02, P1-02/05/06/07/10) at the reference layer.

## 8. Ownership

```text
Object:         RequestKey (ContextIdentity, SlotIndex, Generation)
Owner:          RequestArena constructs; RequestSlot stores the current key
Lifetime:       reserve -> release (one per accepted request; generation increments on release)
Borrowers:      submit path (local copy after enqueue ack); reap (validated under leaf domain);
                ReadyEvent carries it BY VALUE (no pointer)
Stability:      trivial value; copyable; the (slot,generation) pair is unique per accepted request

Object:         RequestSlot
Owner:          RequestArena (one arena per context/backend pair)
Lifetime:       arena construction -> arena destruction; individual slots free<->in-use
Borrowers:      backend submit/enqueue/dispatch/reap paths; NEVER the ReadySink
Stability:      address-stable (arena is a fixed array; slots do not move)

Object:         the slot's Completion publication binding (opaque Completion*, requested_bytes,
                publish thunk)
Owner:          RequestSlot (private field; installed by RequestArena::install_publication_binding
                before commit; cleared at release)
Lifetime:       install (prepared) -> commit -> completion_ready -> release
Borrowers:      reap (validates + publishes through it INSIDE the leaf domain); cancel resolution
                (bounded slot scan compares the binding's Completion pointer)
Stability:      construction-time storage in the fixed slot record — the slot IS the single
                identity carrier; there is NO parallel map (review C2)

Object:         Completion<T> binding payload (arena + slot handle — the slot-release capability)
Owner:          Completion<T> (private); only the binding-CAS winner writes it
Lifetime:       binding -> outstanding -> ready -> resetting
Borrowers:      none outside the slot-lifecycle domain; reset/ready-destruction use it
Stability:      private to Completion; forge-resistant (negative-compile gate)
```

**Generation wrap policy (I6):** `Generation` is a 64-bit counter incremented on every
release (including pre-commit rollback releases). Round-3 finding #5 widened it from
32-bit and added a fail-fast at `UINT64_MAX`
(`request_arena_generation_exhausted_fail_fast`) rather than silently wrapping, so
I6's absolute wording ("a stale key can never mutate the new occupant") holds in
perpetuity. The failure condition is generation EXHAUSTION, not an actual wrap: the
arena refuses to increment a generation that has reached `UINT64_MAX`, so a wrap can
never occur. Exhaustion would take 2^64 releases — approximately 585 years at one
release per nanosecond — and is unreachable in practice; the fail-fast makes the
guarantee absolute instead of probabilistic.

## 9. State Machine

```text
States (RequestSlot unified state word; single arbitration domain):
  free, reserved, prepared, pending, enqueued, running, backend_ready, completion_ready
  + enqueue_in_flight_pin : 1 bit
  + terminal_result_stored : 1 bit

Completion<T> state: idle, binding, outstanding, publishing, ready, resetting
  (binding is NEW; reports idle()==false, ready()==false, outstanding()==false)

Transitions (RequestSlot):
  free -> reserved
    Authority:      RequestArena::reserve (backend admission authority)
    Lock domain:    leaf slot-lifecycle mutex
    Allocation:     none (terminal Result/ready/pending linkage/scratch/waiter storage was
                    pre-reserved at arena construction; reserve only claims a free slot)
    Failure:        arena full -> synchronous would_block (Completion stays idle, no borrow);
                    genuine init failure at arena construction -> no_space (never a submit error)
    Wake:           none
    Shutdown:       admission closed -> reserve rejected (invalid_state)

  reserved -> prepared
    Authority:      backend prepare step
    Lock domain:    leaf slot-lifecycle mutex
    Allocation:     none
    Failure:        malformed op -> invalid_argument; provenance -> invalid_state;
                    capability -> not_supported; slot rolled back to free, Completion idle
    Wake:           none
    Shutdown:       admission closed -> rejected

  prepared -> pending   [COMMIT / submit-success linearization point]
    Authority:      backend commit (binding CAS winner)
    Lock domain:    leaf slot-lifecycle mutex; release-store Completion outstanding
    Allocation:     none
    Failure:        binding CAS loses -> loser rolls back own slot to free, returns
                    invalid_state; winner cannot lose after the CAS
    Wake:           none (the request is not yet dispatched)
    Shutdown:       admission closed -> commit rejected before CAS

  pending -> enqueued   [enqueue outcome 1]
    Authority:      backend enqueue (allocation-free, noexcept)
    Lock domain:    leaf slot-lifecycle mutex
    Allocation:     NONE (noexcept; post-commit obligation)
    Failure:        cannot fail legitimately (ADR Decision 5); only unknown/illegal state
                    is an invariant violation (fail-fast)
    Wake:           dispatch proceeds (backend-defined: Fake/Sync transition to backend_ready
                    deterministically)
    Shutdown:       enqueued ops continue to terminal under shutdown; not abandoned

  pending -> backend_ready   [enqueue outcome 2 / terminal winner first]
    Authority:      terminal winner (pending cancel, dispatch error, ordinary result)
    Lock domain:    SAME leaf slot-lifecycle mutex/domain as pending->enqueued (I17)
    Allocation:     none
    Failure:        first winner stores exactly one terminal Result + ready linkage; losers
                    (including a late enqueue) do nothing
    Wake:           backend_ready progress signal (level-triggered for Fake/Sync; edge
                    implementations re-arm on enqueue pin ack — NOT a second result/linkage)
    Shutdown:       designed shutdown terminal is one valid winner

  enqueued -> running / kernel-owned
    Authority:      dispatch
    Lock domain:    backend-defined (Fake/Sync: deterministic; no kernel)
    Allocation:     none
    Failure:        ownership-safe irrevocable dispatch failure -> terminal backend_error;
                    transient/partial -> stays enqueued for retry
    Wake:           worker/CQE-equivalent
    Shutdown:       designed shutdown terminal if ownership proven absent

  running -> backend_ready
    Authority:      first terminal winner
    Lock domain:    leaf slot-lifecycle mutex
    Allocation:     none
    Failure:        exactly-once terminal Result stored
    Wake:           backend_ready progress signal
    Shutdown:       as above

  backend_ready -> completion_ready   [REAP / completion-ready linearization point]
    Authority:      designated reap authority (poll/wait_one)
    Lock domain:    leaf slot-lifecycle mutex (acquire-observed enqueue pin ack first)
    Allocation:     none
    Failure:        cannot fail (result already stored); a still-pinned slot stays
                    backend_ready and reap-ineligible WITHOUT consuming linkage
    Wake:           synchronous ReadySink invocation AFTER leaving the domain
    Shutdown:       reap remains legal after admission close

  completion_ready -> free   [slot release / generation increment]
    Authority:      caller reset() / ready-Completion destruction
    Lock domain:    leaf slot-lifecycle mutex (after reap has left it)
    Allocation:     none
    Failure:        live pin OR open registration OR stored token/lease ->
                    request_slot_release_invariant_fail_fast (Debug AND Release)
    Wake:           none (slot published free under the domain)
    Shutdown:       quiescent destroy requires every slot free
```

## 10. Linearization Points

```text
Operation:          submit accepted
Linearization:      the release-store Completion binding -> outstanding (commit step 5)
Observable before:  Completion idle; slot free/reserved/prepared; no accepted_out++
Observable after:   Completion outstanding; slot pending; enqueue pin live; accepted_out++;
                    borrow staged; submit returns success

Operation:          pending cancel winner
Linearization:      the CAS/lock acquiring pending -> backend_ready(canceled) under the leaf
                    domain (stores exactly one terminal Result + ready linkage)
Observable before:  slot pending (or enqueued); no terminal Result
Observable after:   slot backend_ready(canceled); exactly one terminal Result; enqueue pin
                    STILL live (reap cannot publish yet)

Operation:          enqueue acknowledgement
Linearization:      the release-clear of the enqueue_in_flight_pin bit (submit's final slot
                    access), after either linking pending queue or confirming backend_ready
                    no-op
Observable before:  pin live; reap cannot publish completion_ready
Observable after:   pin cleared; eligible reap may proceed; level readiness preserved or
                    edge notification re-armed (NOT a second result)

Operation:          backend_ready
Linearization:      the terminal-winner transition storing Result + ready linkage
Observable before:  no terminal Result stored
Observable after:   exactly one terminal Result; ready linkage present (reap-ineligible while
                    pin live)

Operation:          completion_ready
Linearization:      the release-store Completion ready inside the leaf domain (after result
                    install, registration close, token/lease take, accepted_out--, borrow end)
Observable before:  Completion outstanding; accepted_out includes this slot
Observable after:   Completion ready; result available; registration closed; accepted_out-- ;
                    borrow ended; ReadySink delivered exactly once (outside the domain)

Operation:          slot release / generation increment
Linearization:      the leaf-domain publication of the slot as free with generation++
Observable before:  slot completion_ready (or resetting); old key live
Observable after:   slot free; generation incremented; old RequestKey stale (cancel/reap/
                    register/release on old key return not_found / no-op)
```

## 11. Wake/Progress Model

```text
Blocking/suspension:
  - Who may block?   poll() is non-blocking; wait_one() blocks the caller thread (no Fiber
                     in Phase B; Fiber routing is Phase F).
  - Who may suspend? no Fiber suspension in Phase B.
  - What makes them  the caller drives poll()/wait_one(); backend_ready is observable by the
    continue?        next reap. No backend->Scheduler wake in Phase B (Phase G).

Backend -> Scheduler progress:
  - How does backend-  observation via poll()/wait_one() only (as-built; unchanged).
    ready reach?
  - Signal or observation? observation.
  - Worst-case latency? caller-defined (no internal polling interval introduced).

External wake coexistence:
  - External wake +    Phase B has no external wake (no Scheduler integration). The enqueue
    backend progress?  pin protocol guarantees no lost progress: a backend_ready slot whose
                      pin is still live stays ready and is re-reaped on the next poll() after
                      ack; level-triggered by construction for Fake/Sync.
  - Commit-to-sleep     the pin bit under the leaf domain (acquire-observed by reap).
    race?

Polling dependency:
  - Periodic timeout?   NO. No polling interval is introduced. Progress is caller-driven.

Single-worker liveness:
  - N/A for Phase B (no Scheduler worker). Fake/Sync are single-threaded deterministic.
```

## 12. Resource Bounds

```text
Construction-time resources:
  - RequestSlot[n]:   capacity=request_capacity (construction-time fixed), preallocated,
                      failure=no_space at arena construction (never a submit error)
  - per-slot terminal Result storage, ready linkage, pending linkage, scratch, waiter
    storage:          preallocated at arena construction

Submit-time resources:
  - RequestSlot:      capacity=request_capacity, allocation=none (reserve claims free slot),
                      failure=would_block (full) / invalid_argument (malformed) /
                      invalid_state (lifecycle) / not_supported (capability)
  - submit allocation-free after acceptance? YES (enqueue is noexcept; I9)

Completion-time resources:
  - terminal Result:  capacity=1 per slot, preallocated, cannot be lost (I4/I9)

Capacity and backpressure:
  - Maximum outstanding: request_capacity (bounded; AC-7)
  - Queue-full behavior: synchronous would_block, Completion idle, no borrow, no accepted_out++
  - OOM at each stage:   construction -> no_space; submit (post-reserve) -> none (noexcept)

Reclamation:
  - Shrink under load?    NO (fixed arena; slots reuse via free list)
  - Bounded by outstanding or historical? outstanding (slot_in_use tracks reserve->release)
```

Metrics (ADR Decision 13 observability minimums): `capacity`, `slot_in_use`,
`accepted_outstanding`, `high_water_mark`, `capacity_rejections`. `slot_in_use` and
`accepted_outstanding` are DISTINCT counters (reserve vs commit; release vs reap).

## 13. OOM/Failure Semantics

```text
Allocation:     RequestArena slot array + per-slot storage
When:           construction
OOM behavior:   no_space (synchronous at construction; never a submit error)
Rollback:       arena construction fails -> context construction fails
Invariant:      no submit can depend on post-construction allocation for the terminal path

Allocation:     none on submit after reserve (enqueue is noexcept)
When:           submit (post-commit)
OOM behavior:   N/A (no allocation)
Rollback:       pre-commit failure rolls back slot to free + Completion to idle
Invariant:      successful submit -> exactly one terminal Result eventually (I4)

Allocation:     none on reap / reset / release
When:           completion
OOM behavior:   N/A
Invariant:      completion-ready publication never allocates; reset/release never allocate
```

## 14. Cancellation

```text
Layer:          operation (Phase B); admission-layer cancel is a separate future concern
Authority:      backend cancel(RequestKey) resolves the key to a slot+generation and targets it
Terminal:       canceled (one possible terminal Result); already_terminal; not_found
Exactly-once:   YES — pending cancel and ordinary terminal share ONE winner transition under
                the leaf domain; a cancel that finds backend_ready returns already_terminal
                and does NOT overwrite
Syscall effect: N/A (Fake/Sync have no syscalls; blocking-syscall interruption is Phase E)

CancelDisposition { terminal_won, intent_recorded, already_terminal, not_found, not_supported }:
  - pending      -> terminal_won: cancel wins pending -> backend_ready(canceled) directly (Scheme B / ADR I17)
  - enqueued     -> terminal_won: cancel wins enqueued -> backend_ready(canceled) (Scheme B)
  - running      -> intent_recorded: cancel records INTENT only (best-effort, Decision 11);
                    record_terminal later records the REAL result VERBATIM; a confirmed
                    interruption records TerminalResult::err(canceled) explicitly
  - backend_ready-> already_terminal (never overwrite)
  - completion_ready (generation-bound) -> already_terminal; after reset -> not_found
  - unknown/stale generation -> not_found (never acts on a reused slot; I6)
  - backend cannot cancel -> not_supported without changing terminality
```

Wait-cancel (Decision 10) removes ONLY the waiter; it does NOT cancel the I/O, does NOT end
the borrow, and races reap exactly-once for the token/lease.

## 15. Shutdown

```text
Admission closure:  close_admission atomically prevents new acceptance; existing accepted
                    requests continue to ordinary terminal
Drain:              callers continue poll()/wait_one() to reap accepted requests; cancel and
                    waiter-cancel remain legal
Join:               no threads in Phase B (Fake/Sync); join is Phase E
Destruction order:  (1) close admission (2) reap all accepted (3) callers reset/destroy ready
                    Completions releasing all slots (4) accepted_outstanding==0 AND
                    slot_in_use==0 (5) destroy
Fail-fast:          destroy context with slot_in_use != 0 -> async_context_outstanding_fail_fast
                    (existing entry); reset/destroy Completion in binding/outstanding ->
                    completion_binding_*_fail_fast; release with live pin / open registration /
                    stored token-lease -> request_slot_release_invariant_fail_fast
```

## 16. API Compatibility

```text
Public API change:  minimal
Breaking:           NO (public submit_* signatures unchanged; ADR Decision 7)
ABI:                Completion<T> gains a binding transient state (internal enum); no public
                    layout change visible to callers; IoError::Code gains three enumerators
                    (enum value addition; existing codes unchanged)
Documentation:      docs/reference/api.md (IoError table updated; Completion binding note);
                    this design doc; phase-b-compliance-gate.md
New public surface: NONE (no RequestHandle; no public RequestKey; the ReadySink is an
                    internal detail of the reference backends for Phase B — a future ADR will
                    decide whether/when reap_identity becomes public)
```

## 17. Alternatives Rejected

| Alternative | Reason rejected |
|-------------|-----------------|
| Scheme A (pending cancel records intent; enqueue completes; then terminalize) | Rejected by ADR Decision 4/I17. Lets the request be dispatched after cancel was requested, complicates exactly-once, and defers terminalization into the enqueue path. Phase B implements Scheme B (cancel wins directly, enqueue no-ops). |
| Caller-owned `Operation.Storage` now (Alternative B in ADR) | Big-bang migration of Runtime/Batch/copy-pipeline; rejected as too large for the reference-core PR. Registered as DIV-02 revisit trigger. |
| Per-backend arena (Fake its own, Sync its own) | Violates ADR Decision 2 ("no two independently oversubscribable request stores"); duplicates the slot-lifecycle domain logic. Shared `detail::RequestArena` selected. |
| Arena on AsyncIoContext | Cleanest single-capacity story but changes AsyncIoContext layout and touches context move/lifecycle death-tests; larger blast radius than Phase B needs. Arena owned by the backend (one per context/backend pair). |
| Map `invalid_argument`/`not_found`/`not_supported` onto existing codes | Blurs the ADR's distinct error semantics (Decision 6); weakens the cancel-disposition and negative-compile contract. Enum extended instead. |
| Defer ReadySink delivery as a queued ReadyEvent | ADR Decision 9 forbids this without a new ADR introducing an explicit event lease/ack. Synchronous by-value delivery selected. |
| Remove the legacy `try_claim_for_backend` / `try_claim` single-step claim in Phase B | The not-yet-migrated backends (UringAsyncBackend, ThreadPoolBackend) legitimately still use it — they are explicitly out of Phase B's scope (Phases D/E). The binding protocol is added as a NEW path for migrated backends; the legacy single-step claim is retained (not deprecated) until its last consumer migrates. The two share the same private-access boundary and never race because a given Completion is driven by exactly one backend. |

## 18. Required Tests

(Mapped to test files; Gate 4 evidence/status lives in `phase-b-compliance-gate.md`.)

- `tests/request_arena_test.cpp` — capacity bounded (I8); generation advances on release
  (I6); stale key rejected (I6); distinct slot_in_use vs accepted_outstanding (I3/I8);
  no post-accept allocation (I9).
- `tests/async_completion_test.cpp` (extended) — `binding_cas_elects_one_context` (I2);
  `binding_unobservable_to_cancel_await_reset` (I15).
- `tests/completion_authority_death_test.cpp` (extended) — destroy-in-binding, reset-in-binding,
  release-with-live-pin, release-with-open-waiter.
- `tests/request_lifecycle_scheme_b_test.cpp` (NEW, primary) — the task's 19-step
  pending-cancel-wins-before-enqueue trace; exactly-one-terminal-winner (I10);
  ready_sink_event_survives_reset_during_callback (I16); waiter_registration_cardinality (I13).
- `tests/backend_conformance_test.cpp` (extended) — backend-agnostic lifecycle cases.
- Death tests (commit 5) — destroy-outstanding, reset-outstanding, destroy-with-slot-in-use,
  destroy-in-binding.
- Negative-compile probe — ordinary caller cannot forge a binding, mutate RequestKey binding,
  clear the enqueue pin, publish backend_ready, release a RequestSlot, or increment generation.
- Gate matrix: Clang Debug, Clang Release (§6.1), ASan/UBSan (§6.2), TSan (§6.3),
  negative-compile, doc-check.

## 19. Architecture Constitution Checklist

| Rule | Compliant? | Evidence |
|------|-----------|----------|
| AC-1 | yes | request lifecycle enters through AsyncIoContext (caller-held capability); no global runtime |
| AC-2 | yes | RequestKey = (context, slot, generation); stable from admission to reap |
| AC-3 | yes | five-stage admission; commit is submit-success LP; pre-commit failure leaves Completion idle |
| AC-4 | yes | accepted terminal path allocation-independent (I9); reserved storage cannot strand the op |
| AC-5 | yes | only designated reap makes Completion ready; single-winner CAS |
| AC-6 | yes | no polling interval introduced; progress is caller-driven; pin protocol closes the commit-to-reap race |
| AC-7 | yes | bounded request_capacity at construction; no unbounded growth |
| AC-10 | yes | one authority per transition (documented in §9) |
| AC-11 | yes | tests prove semantic properties (exactly-once, bounded, transactional, no-lost-progress) not implementation preference |
| AC-12 | yes | DIV-02 registered and activated for Phase B |
| AC-13 | yes | publication mutators remain private; reset/destroy state-checked; binding unobservable; fail-fast in Debug AND Release |
| AC-14 | yes | RequestKey carries provenance + generation; cancel targets a generation; stale key rejected |
| AC-15 | yes | ReadyEvent carries RequestKey + OperationKind by value; no Completion*/RequestSlot* |

## 20. Revisit Triggers

```text
Trigger:            benchmarks or backend ABI evidence show caller-owned Operation.Storage
                    materially reduces per-request overhead at controlled public-API migration
                    cost (Runtime/Batch/copy-pipeline)
Revisit action:     re-evaluate DIV-02; consider migrating to caller-owned storage and a
                    public RequestHandle
Owner:              async subsystem maintainer

Trigger:            a real backend (Uring/ThreadPool) migration in Phase D/E exposes a
                    constraint the reference lifecycle cannot satisfy
Revisit action:     add a superseding ADR/closeout note; do not silently diverge
Owner:              async subsystem maintainer
```

## Scope fence (binding)

Phase B modifies ONLY: `include/sluice/error.hpp` (enum); new
`include/sluice/async/detail/{request_key,request_slot,request_arena,ready_sink}.hpp`;
`include/sluice/async/completion.hpp` (binding transient + release handshake);
`include/sluice/async/async_io_context.hpp` (protected helper rename);
`include/sluice/async/{fake_backend,sync_backend}.hpp`;
`src/async/{async_io_context,fail_fast}.cpp`; the new + extended tests; the docs listed in
the commit plan.

Phase B does NOT modify: `UringAsyncBackend`, `ThreadPoolBackend`, `Scheduler`, `Batch`,
`ApplicationRuntime`, the backend→Scheduler wake bridge, public `RequestHandle`, public
`submit_*` return types, the persistent blocking worker pool, real Fiber routing integration,
or the full backend conformance framework. No coroutine/P2300/Asio/networking/timers/new-
cancel-model is added as an incidental (AGENTS.md §7).
