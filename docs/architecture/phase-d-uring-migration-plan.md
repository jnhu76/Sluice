# Phase D — UringAsyncBackend RequestArena Migration: SOTA-Aligned Plan

**Status:** PLAN REVISED FOR HUMAN REVIEW — Phase D is NOT implemented; D1 is BLOCKED on the
narrow ADR amendment in §3.
**Date:** 2026-08-08
**Author:** jnhu
**Governing authority:** [ADR-explicit-io-request-contract](../adr/ADR-explicit-io-request-contract.md)
(Accepted), [Architecture Constitution](architecture-constitution.md),
[remediation roadmap](remediation-roadmap.md), and the Phase B/C2/Phase E compliance ledgers.

This document supersedes the earlier Phase D0 draft on this branch. The earlier draft was built
around an exact `io_uring_submit()` accepted-prefix → `RequestArena::mark_running()` transition.
That model is internally provable, but it couples Sluice request lifetime to one liburing transport
mode and forces partial-submit, prepared-suffix cancel, NOP neutralization, and physical-SQ order
bookkeeping into the semantic state machine.

A review against current liburing plus production io_uring runtimes changes the recommendation.
The target is now deliberately closer to the mature lifetime model used by Tokio/tokio-uring and
Monoio, and to the private-ring topology used by Seastar/Glommio:

> **RequestArena owns logical request lifetime. The Uring backend owns a request from the moment
> its SQE is installed into that backend's private ring until the original operation CQE retires
> the execution reference. `io_uring_submit()` is transport progress, not request-lifecycle
> authority.**

The resulting model is smaller, makes cancellation easier to reason about, does not need a
neutral-NOP request path, and does not make future SQPOLL support require another lifecycle rewrite.

---

## 1. Baseline and current gaps

Phase D still starts from master baseline `1349a6fdf63f760d73cec9d567bb3fecd46fa695`.
The current Uring implementation is the legacy model:

```text
Completion*
  + monotonic operation id
  + ops unordered_map
  + Completion* -> id unordered_map
  + cancel-id -> op-id unordered_map
  + pending SQE deque
  + direct CQE -> Completion publication
```

The migration still has to close the same structural gaps:

- RequestArena is not the sole request identity/lifecycle/terminal authority.
- per-request maps/deques can allocate after acceptance and are not bounded by request capacity;
- ring queue depth is incorrectly doing double duty as the only practical capacity control;
- CQE handling publishes Completion directly instead of stopping at backend-ready and letting reap
  publish;
- cancellation is keyed through legacy Completion/id side maps;
- Uring has no Phase-D close/drain/destruction proof;
- real-liburing evidence is not yet available for the whole conformance suite.

Two infrastructure findings from the first audit remain valid:

1. **P-D0-INF-01:** real-liburing `uring_submit_failure_test` has a pre-existing link break and must
   be repaired in D1 before it can be evidence.
2. **P-D0-INF-02:** the WSL2 development host has not yet demonstrated the complete real-path test
   matrix. Stub green never substitutes for real-liburing evidence.

The current PR is documentation/planning only. No production migration is implied by merging D0.

---

## 2. Reference hierarchy — what Phase D should learn from

The reference order for Phase D is intentionally:

```text
liburing contract
    ↓
Tokio / tokio-uring / Monoio lifetime model
    ↓
Seastar / Glommio ring topology
    ↓
Zig std.Io API abstraction
```

This order separates four questions that the earlier draft mixed together:

1. **What does the kernel/liburing actually guarantee?**
2. **How should an operation record live until completion?**
3. **How should rings and CPUs be laid out at scale?**
4. **What public abstraction should hide those backend details?**

### 2.1 liburing contract

The load-bearing facts are:

- `user_data` is carried from SQE to CQE and is the natural completion-correlation field;
- one submitted ordinary SQE produces one matching CQE;
- without SQPOLL, `io_uring_submit()` returns the number of SQEs submitted by that call;
- **with SQPOLL, that return value may be larger than the number actually submitted**;
- `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN` is a first-class event-loop shape;
- SQPOLL moves submission consumption away from a precise application-side `submit()` boundary.

References:

- [io_uring_submit(3)](https://www.man7.org/linux/man-pages/man3/io_uring_submit.3.html)
- [io_uring(7)](https://www.man7.org/linux/man-pages/man7/io_uring.7.html)
- [io_uring_submit_and_wait(3)](https://www.man7.org/linux/man-pages/man3/io_uring_submit_and_wait.3.html)

**Consequence for Sluice:** a request lifecycle whose `running` transition requires an exact
`io_uring_submit()` prefix is transport-specific. It can work in the current non-SQPOLL setup, but
it is the wrong long-lived abstraction boundary.

### 2.2 Tokio / tokio-uring / Monoio: retain operation state until completion

The common pattern is not identical code, but the lifetime rule is remarkably consistent:

```text
stable operation record
      ↓
SQ/ring takes the operation
      ↓
submit / retry / batching are transport progress
      ↓
best-effort cancel may be issued
      ↓
operation record stays alive
      ↓
original completion retires the operation
```

Current Tokio's Uring driver inserts a stable in-flight operation record, assigns `user_data`, and
keeps cancellation state alive until the operation completes. tokio-uring documents a
`Submitted/Waiting/Ignored/Completed` lifecycle and explicitly keeps the operation state because it
owns data the kernel may still reference. Monoio similarly pushes the operation into the submission
queue and treats later `io_uring_enter` progress as transport progress; its source notes that an
EAGAIN after the SQ tail is updated does not invalidate the operation — a later enter can submit it.
Monoio also uses reserved control `user_data` values for cancel/timeout/eventfd rather than making
control CQEs full request identities.

References:

- [Tokio Uring driver source](https://docs.rs/tokio/latest/src/tokio/runtime/io/driver/uring.rs.html)
- [tokio-uring design](https://docs.rs/crate/tokio-uring/latest/source/DESIGN.md)
- [Monoio Uring driver source](https://docs.rs/monoio/latest/src/monoio/driver/uring/mod.rs.html)

**Consequence for Sluice:** once an SQE has crossed into Uring-owned execution state, keep the
RequestSlot and its backend scratch alive until the original CQE. Do not try to turn a still-staged
SQE back into an immediately releasable canceled request.

### 2.3 Seastar / Glommio: private rings, not a shared hot ring

The topology lesson is equally important.

Glommio is thread-per-core and uses thread-local io_uring reactors; current documentation describes
multiple rings per CPU for different classes of work. Seastar's current asymmetric io_uring backend
keeps a distinct io_uring instance per shard. Proxy shards may attach their rings to a master's
kernel io-wq via `IORING_SETUP_ATTACH_WQ`, so **kernel worker resources can be shared without
turning the userspace submission ring into a multi-producer shared hot structure**. Seastar also
assigns worker cores using SMT/NUMA topology and pins SQ polling/worker execution to selected CPUs.

References:

- [Seastar asymmetric io_uring backend](https://github.com/scylladb/seastar/blob/master/doc/reactor_backend_asymmetric_uring.md)
- [Glommio](https://glommio.github.io/)

**Consequence for Sluice:** one Uring backend/context owns one private ring and one issuer domain.
Future multi-shard support should instantiate one ring per shard/executor, with optional shared
kernel worker pools and topology-aware affinity. Do **not** evolve the current backend toward
"many application threads share one ring behind a mutex" as the default architecture.

### 2.4 Zig: API abstraction, not current Uring correctness oracle

Zig remains useful for the public `std.Io` abstraction idea: code should depend on explicit I/O
semantics rather than backend implementation details. Phase D should not, however, use Zig's
current Uring implementation as the primary correctness reference when liburing and mature
io_uring runtimes give stronger evidence for request lifetime and ring topology.

---

## 3. Required narrow ADR amendment before D1

### 3.1 Why an amendment is required

The Accepted request-contract ADR currently freezes these Uring-relevant semantics:

```text
enqueued
  | dispatch
  v
running / kernel-owned
```

and its transition table says the `enqueued -> running/kernel-owned` authority is blocking-worker
dequeue or **kernel submission accounting**. Decision 5 further says a positive partial submit
leaves the unsubmitted suffix `enqueued` / submission-pending.

That wording is binding. The SOTA-aligned model below therefore **cannot** be implemented silently.
D1 is blocked until a superseding/amending ADR is accepted.

### 3.2 Proposed amendment

The amendment should be deliberately narrow and backend-neutral:

**Current semantic label**

```text
running / kernel-owned
```

**Proposed semantic label**

```text
running / backend-execution-owned
```

The transition means:

> The request has left the cancelable local dispatch queue and an execution engine now owns an
> execution reference that must be retired before the RequestSlot can be released.

Backend mapping:

| Backend | `enqueued -> running` means |
|---|---|
| ThreadPool | worker dequeue + `mark_running`; the worker now owns the syscall execution reference |
| Uring | the SQE has been successfully installed into this backend's private io_uring submission ring, with stable `user_data` and backend scratch; the ring now owns the execution reference |
| synthetic/reference backend | may skip `running` as already permitted |

For Uring specifically:

- `io_uring_submit()` / `io_uring_enter()` **does not change RequestArena lifecycle state**;
- a positive partial submit is transport progress only;
- SQPOLL consumption is transport progress only;
- a request in `running` remains bound until its original operation CQE, or until a separately
  proven transport-failure recovery path demonstrates that no SQE/kernel/CQE reference can exist;
- cancellation of a running request records intent and may enqueue `IORING_OP_ASYNC_CANCEL`, but
  does not locally release/reuse the slot.

This amendment does **not** weaken any of the following existing rules:

- commit/accept remains the successful public submit LP;
- pending/enqueued cancellation may win only before execution ownership transfers;
- RequestArena remains sole logical identity/lifecycle/terminal authority;
- Completion publication remains reap-only;
- borrow lifetime still ends at completion-ready, not at CQE arrival;
- exactly one terminal result wins;
- slot release/reuse still requires completion-ready and full-generation validation;
- ThreadPool behavior and its existing proofs remain valid.

### 3.3 Why this is worth changing the ADR

Keeping the old boundary solely to avoid an ADR edit would force Sluice to carry semantic machinery
that mature io_uring runtimes do not need:

```text
submit-prefix lifecycle authority
+ prepared-but-enqueued suffix state
+ enqueued prepared-suffix cancellation
+ request-token NOP neutralization
+ future NOP CQE after slot release
+ exact physical-SQ ledger as RequestArena state authority
```

The amendment removes those accidental complexities and also avoids making SQPOLL a future
lifecycle redesign.

---

## 4. Target ownership model after the amendment

### 4.1 Request lifecycle

```text
free
  ↓ reserve
reserved
  ↓ prepare
prepared
  ↓ commit / accept LP
pending
  ↓ enqueue arbitration
enqueued                     local dispatch queue owns request
  ↓ dispatch ownership transfer
running                      Uring private ring owns execution reference
  │
  ├── submit / retry / batching / partial submit / SQPOLL progress
  ├── best-effort AsyncCancel
  │
  ↓ original operation CQE
backend_ready
  ↓ RequestArena::reap
completion_ready
  ↓ reset / ready destruction
free (generation++)
```

The important sentence is:

> **There is no RequestArena transition at `io_uring_submit()` return.**

### 4.2 Authority table

| Concern | Authority |
|---|---|
| logical identity `(context, slot, generation)` | RequestArena |
| admission / capacity / generation / terminal winner | RequestArena |
| local not-yet-execution-owned ordering | bounded dispatch ring |
| execution ownership after dispatch | one private Uring ring |
| kernel-visible completion correlation | non-authoritative bounded CQE router + opaque op cookie |
| operation terminal result | original operation CQE → `arena.record_terminal` |
| cancel intent | RequestArena; backend scratch only remembers whether a cancel SQE still needs to be issued |
| Completion-ready publication | `RequestArena::reap` only |
| transport submit/retry state | Uring backend; never a second RequestArena state machine |

---

## 5. Dispatch: one clean ownership transfer

The dispatch path runs under one backend dispatch/driver serialization domain.

```text
dispatch_one(h):
    require arena state == enqueued

    sqe = io_uring_get_sqe(private_ring)
    if no SQE:
        flush transport progress
        retry later if still full
        # request remains enqueued; local cancel may still win

    allocate op_cookie from bounded/no-wrap routing domain
    fill SQE from RequestSlot descriptor/borrow
    SQE.user_data = op_cookie
    install fixed backend scratch / CQE routing entry containing FULL SlotHandle

    arena.mark_running(h)
    remove_exact(h) from local dispatch ring

    # from here until original CQE, the request cannot be locally released
```

`mark_running` and local dispatch removal are coordinated exactly like the ThreadPool ownership
transfer. No application-visible success/failure occurs here; public submit already crossed commit.

### 5.1 Why no prepared-but-enqueued state remains

Under this model, an SQE is not prepared while the request remains semantically `enqueued` across
driver calls. Either SQ capacity is unavailable and the request stays entirely local/enqueued, or
the backend installs the SQE and transfers the request to `running` / ring-owned in one dispatch
critical section.

Therefore Phase D no longer needs:

- request-carrying neutral NOP rewrites;
- "prepared suffix but still enqueued" as a production request state;
- cancel-after-prepare-before-running terminal wins;
- slot release while a request-derived NOP CQE is still possible.

### 5.2 `io_uring_submit()` becomes transport progress

A flush may return full, partial, zero, transient error, or (in a future SQPOLL mode) a count that
is not an exact kernel-acceptance measure. None of those outcomes changes the request's
`RequestState`.

For the initial non-SQPOLL D1 implementation, a construction-time-bounded **transport ledger** may
track physical SQ order if required for submit-failure diagnostics/recovery. Its scope is strictly:

```text
physical SQ transport evidence
```

not:

```text
request identity
request state
terminal authority
Completion publication
```

The old unbounded `pending_sqes` container still disappears. If a bounded transport ledger is kept,
its mutation tests must prove that deleting/corrupting it can break transport recovery diagnostics
but cannot fabricate an `enqueued -> running` lifecycle transition: that transition already
happened at ring ownership transfer.

---

## 6. CQE identity: stable routing back to the full RequestKey

### 6.1 Do not use Completion* or raw SlotIndex as kernel identity

`Completion*`, raw slot index, or `RequestSlot*` alone all permit ABA after slot reuse. The kernel
boundary needs a stable correlation value that resolves back to the authoritative full
`SlotHandle`.

### 6.2 Target shape

```text
SQE.user_data = opaque op_cookie

CQE.user_data
   ↓
construction-time bounded CqeRouter
   ↓
full SlotHandle(slot + full uint64 generation)
   ↓
RequestArena full validation
   ↓
record_terminal
```

`CqeRouter` is **not** a second request store. It is bounded transport routing metadata, analogous
to the backend scratch already sanctioned by Decision 3. It stores no independent lifecycle or
terminal state; RequestArena re-validates the full handle before any mutation.

The implementation should prefer bounded O(1) lookup (for example a fixed-capacity open-addressed
router) over an O(request_capacity) scan if measurements show the scan is material at high queue
depths. The D1 frozen design must choose the concrete representation and benchmark it; correctness
does not depend on exposing that representation publicly.

Opaque operation cookies are never reused within backend lifetime; exhaustion fail-fasts before
reuse. Reserve a small control range for non-request CQEs.

### 6.3 Control CQEs are not request identities

Following the simpler mature-runtime pattern, cancel/wake/timeout-style control operations should
use reserved control `user_data` values where possible. An AsyncCancel CQE is informational; it
must not own a RequestKey or publish a terminal.

---

## 7. Cancellation becomes simpler

### 7.1 pending/enqueued: local cancel may win

Before Uring owns an execution reference:

```text
lock dispatch domain
    resolve full handle
    remove_exact(h) from local dispatch queue if present
    arena.cancel(h) -> terminal_won
unlock
signal backend-ready progress
```

The ordering is deliberate:

```text
DISARM LOCAL EXECUTION FIRST
TERMINAL WIN SECOND
```

A canceled `pending` / `enqueued` request therefore has a strong guarantee: its operation SQE was never
installed into the ring and cannot execute.

### 7.2 running/ring-owned: intent only

Once the private ring owns the request:

```text
arena.cancel(h) -> intent_recorded
scratch.cancel_requested = true
```

The driver opportunistically appends:

```text
IORING_OP_ASYNC_CANCEL(target = op_cookie)
user_data = CONTROL_CANCEL
```

No ad-hoc request-state transition happens when that cancel SQE is submitted. If SQ capacity is
unavailable, the cancel request is retried later while the original operation remains running.

The original operation CQE decides the terminal:

- ordinary success stays success;
- ordinary error stays that error;
- original CQE `-ECANCELED` may record the canceled terminal;
- cancel CQE success/`-ENOENT`/other informational results never overwrite the operation result.

The RequestSlot and op cookie remain live until the original CQE (or a separately proven
transport-failure retirement path). Repeated cancel calls must not enqueue an unbounded number of
cancel SQEs; one fixed per-slot cancel_requested / cancel_queued bookkeeping bitset is sufficient.

### 7.3 What disappears

The SOTA-aligned lifetime removes the entire earlier mechanism:

```text
prepared enqueued request
  → local cancel wins
  → rewrite SQE to NOP
  → replace request cookie with neutral cookie
  → allow slot release before NOP CQE
```

There is no need for this because `enqueued` no longer has an SQE and `running` no longer permits
local terminal cancellation.

---

## 8. Submit failure and ring poison

This is the one area where transport evidence still matters, but it must not leak into normal
request lifecycle.

### 8.1 transient progress failure

`EINTR`, `EAGAIN`, `EBUSY`, zero progress, or partial progress leave ring-owned requests alive and
retryable. No terminal is fabricated and no RequestArena state changes.

### 8.2 permanent transport failure

A permanent `io_uring_submit` failure may poison the ring for new admissions, but accepted requests
must still obey the terminal guarantee. D1 must freeze one explicit recovery policy before code:

1. identify which ring-owned SQEs are **provably not kernel-consumed** using only bounded transport
   metadata supported by the chosen non-SQPOLL mode;
2. only those proven-unconsumed requests may be locally retired with `backend_error`;
3. requests that may be kernel-owned remain bound for their CQE;
4. new public submissions reject synchronously with the stored backend error once the ring is
   poisoned;
5. no slot is released merely because a transport syscall returned an error.

The initial D1 may deliberately keep SQPOLL disabled until this recovery proof is complete. That is
acceptable because the new lifecycle no longer depends on the non-SQPOLL prefix count; enabling
SQPOLL later becomes a transport/recovery enhancement rather than a RequestArena redesign.

**D1 implementation is forbidden until this permanent-failure proof is present in its frozen
design.** The frozen design and the Class-A proof policy live in
`docs/architecture/phase-d1-uring-frozen-design.md` §6.

---

## 9. Capacity and allocation model

Keep request capacity independent from ring depth:

| Resource | Bound | Full behavior |
|---|---|---|
| RequestArena | `request_capacity` | synchronous `would_block`; Completion remains idle |
| local dispatch ring | `request_capacity` | reserved before commit; no post-accept allocation |
| CqeRouter / per-slot Uring scratch | construction-time bounded by request capacity | no post-accept allocation |
| io_uring SQ/CQ | `queue_depth` | transport pressure; accepted request remains alive |
| future number of shard rings | number of configured shards/executors | each ring has its own capacity; never one unbounded global ring |

`request_capacity > queue_depth` remains legal. Excess accepted work simply remains in the local
`enqueued` dispatch ring until an SQE is available.

After acceptance, the normal request path must require zero unbounded allocation:

```text
commit
 → enqueue
 → dispatch/ring-owned
 → submit/retry
 → CQE
 → record_terminal
 → reap
 → reset/release
```

All storage used by that path is construction-time fixed/bounded.

---

## 10. One ring per shard/executor is the architectural default

Phase D1 itself still implements one Uring backend/context/ring, because Sluice does not yet need a
new public sharding runtime. But the design must leave the correct seam:

```text
Shard / Executor 0 ── UringBackend 0 ── private ring 0
Shard / Executor 1 ── UringBackend 1 ── private ring 1
Shard / Executor 2 ── UringBackend 2 ── private ring 2
```

Rules:

1. one ring has one issuer/driver domain by default;
2. do not add a shared-ring mutex as the scaling strategy;
3. cross-shard work moves at the task/routing layer, not by making every thread a producer to one
   io_uring SQ;
4. a future topology-aware mode may use `IORING_SETUP_ATTACH_WQ` so private rings share kernel
   io-wq resources;
5. a future SQPOLL mode may pin polling/worker CPUs and use NUMA/SMT topology, following the same
   broad direction as Seastar's asymmetric backend;
6. topology policy belongs above/beside the backend instance, not in RequestArena identity.

This is intentionally compatible with a later Sluice runtime or M:N scheduler without forcing that
runtime into Phase D.

---

## 11. Wait / close / drain / destruction

The earlier D4 correction stands: Uring should integrate with the existing `BackendWaitSource`
protocol rather than invent another blocking wait under `AsyncIoContext::access_mtx_`.

Target shape:

```text
snapshot readiness epoch
   ↓
nonblocking poll/reap under context serialization
   ↓
park without access_mtx_ on ring progress + control wake
   ↓
final nonblocking poll on interruption
```

The ring fd is pollable. If an eventfd is used, registration is through
`io_uring_register_eventfd`; there is no `IORING_SETUP_EVENTFD` flag.

D4 must freeze and test:

- progress-vs-control wake distinction;
- eventfd counter drain and spurious notifications;
- no-lost-wake snapshot/park ordering;
- close racing the public submit LP;
- drained vs releasable distinction;
- quiescent destruction only;
- non-quiescent destruction fail-fast in Debug and Release.

The per-shard/private-ring direction strengthens this design: every backend wait source corresponds
to one ring/issuer domain instead of a global shared ring.

---

## 12. Real/stub conformance and evidence discipline

Stub mode remains useful only as build/API evidence. It never substitutes for KernelIo behavior.

Phase D completion requires a real-liburing host to prove:

- shared backend semantic suite;
- capacity and accounting;
- RequestKey/CQE identity and stale-event rejection;
- pending/enqueued/running cancellation windows;
- borrow/waiter/lease behavior;
- failure injection and zero-post-accept-allocation;
- close/drain/destruction;
- exact reap-only publication;
- no legacy identity maps/deques remain;
- current stub still classifies honestly as non-conforming/incomplete where kernel evidence is
  impossible.

Manifest records flip only after command-backed evidence exists on the final implementation head.
A D1 merge on a host without real io_uring evidence may leave the corresponding Uring record
`not_implemented`; stub-only green never flips it.

---

## 13. Mutation / detector priorities

The new model changes what the killing tests should target.

High-value mutants:

| Defect | Required RED detector |
|---|---|
| SQE prepared before public commit | pre-commit rejection leaves ring/SQ unchanged |
| request left `enqueued` after SQE becomes ring-owned | cancel incorrectly wins terminal despite ring execution reference |
| `io_uring_submit()` prefix drives `mark_running` | partial-submit hook changes RequestState — forbidden |
| running cancel locally releases slot | delayed original CQE hits reused generation / lifecycle death |
| cancel CQE publishes terminal | ordinary result vs cancel-CQE race overwrites winner |
| CQE router skips full-generation validation | stale cookie mutates new slot occupant |
| post-accept allocation returns | always-throw allocator strands accepted request |
| permanent submit failure terminalizes possibly kernel-owned work | later CQE double-terminal / invariant violation |
| shared-ring multi-producer path appears | structural/configuration test rejects unapproved multi-issuer mode |
| CQE path publishes Completion directly | Completion becomes ready without arena reap |
| close returns before acceptance LP arbitration | a new request becomes accepted after close returns |
| destructor drains/cancels implicitly | non-quiescent destruction does not fail fast |

The old neutral-NOP/request-cookie detector is deleted because that mechanism is no longer in the
target architecture.

---

## 14. Revised PR decomposition

### D0 — this PR

Planning only:

- audit legacy Uring;
- record SOTA reference hierarchy;
- identify the Accepted-ADR conflict;
- freeze the SOTA-aligned target at review level;
- **do not start production D1 until the narrow ADR amendment is accepted.**

### D0.5 — narrow ADR amendment

A documentation/ADR PR (or an explicit extension of this planning PR if governance permits) changes
only the execution-ownership meaning described in §3:

```text
running/kernel-owned
        ↓
running/backend-execution-owned
```

and the Uring dispatch note from exact kernel-submit accounting to private-ring ownership.
ThreadPool proofs remain valid and should be called out explicitly as unaffected.

### D1 — Uring RequestArena + private-ring lifetime cut

One atomic production cut:

- bounded RequestArena admission and Uring config (`request_capacity`, `queue_depth`);
- bounded local dispatch ring;
- private one-issuer io_uring instance;
- `enqueued -> running` on private-ring execution ownership transfer;
- bounded CqeRouter + full SlotHandle validation;
- `io_uring_submit` as transport progress only;
- CQE → `record_terminal` → reap-only publication;
- pending/enqueued cancel winner + running best-effort cancel;
- control cancel cookie, no request-carrying cancel identity map;
- permanent-submit failure proof frozen before implementation;
- P-D0-INF-01 real-test link fix;
- capacity and core real-path evidence.

D1 must not add shared-ring multi-threading, SQPOLL, registered buffers/files, topology-aware worker
placement, Scheduler routing, or a user-space runtime.

### D2 — failure injection / no-allocation

Add deterministic seams and prove:

- pre-commit rollback leaves zero ring residue;
- post-commit/ring-owned paths allocate no unbounded storage;
- transient submit pressure preserves operations;
- permanent submit poison never terminalizes work that may still execute;
- cancel/failure has exactly one terminal winner.

### D3 — identity / cancel / borrow / waiter race matrix

Close the full Uring C2b/C2c matrix on the real backend, including stale CQE routing, generation
reuse, Scheme-B pending cancel, enqueued cancel, running cancel, original-vs-cancel CQE orders, and
borrow/waiter/lease rules.

### D4 — wait / close / destruction + KernelIo gate lift

Implement BackendWaitSource, close/drain/destruction proof, real-liburing CI evidence, manifest
closure, and finally remove the special KernelIo NOT-CONFORMING hard-code only when the evidence is
actually complete.

### Future topology phase — deliberately after Phase D

Only after the single-ring backend is proven usable:

```text
private ring per shard/executor
       + optional ATTACH_WQ
       + optional SQPOLL
       + worker CPU affinity
       + NUMA / SMT placement
```

This is where the Seastar/Arachne-style topology question belongs. It must be justified by
measurements rather than pulled into the basic I/O correctness migration.

---

## 15. D1 frozen-design checklist

D1 production code is authorized only after reviewers can answer **yes** to all of these:

1. Has the narrow ADR amendment in §3 been accepted?
2. Is one backend instance unambiguously one private ring / one issuer domain?
3. Does `running` mean ring-owned execution reference rather than submit-prefix accepted?
4. Can `io_uring_submit()` return any partial count without changing RequestArena state?
5. Can an enqueued cancel prove no SQE was installed before it records the canceled terminal?
6. Does every running cancel retain the RequestSlot until the original CQE/recovery retirement?
7. Is cancel CQE control-only and incapable of publishing a request terminal?
8. Does CQE routing recover a full generation-validated SlotHandle without an unbounded map?
9. Is request capacity independent from ring depth?
10. Is the accepted path allocation-independent after commit?
11. Is permanent submit failure safe for both definitely-unconsumed and possibly-kernel-owned work?
12. Does the design avoid a shared-ring mutex/multi-producer architecture?
13. Does the wait design plug into BackendWaitSource rather than blocking under access_mtx_?
14. Are real-liburing evidence and stub-mode honesty explicitly separated?

Any "no" keeps D1 at DESIGN PENDING.

---

## 16. Advantages and costs of the revised model

### Advantages

- far smaller cancel state machine;
- no request-derived neutral NOP lifecycle;
- partial submit becomes transport progress rather than logical identity movement;
- future SQPOLL no longer requires redefining RequestArena states;
- operation memory naturally remains stable for the whole ring/kernel lifetime;
- private-ring topology scales toward shard-per-core without a shared SQ hot lock;
- RequestArena remains the portable semantic layer shared with ThreadPool;
- kernel-specific complexity stays inside Uring transport bookkeeping.

### Costs / tradeoffs

- the Accepted ADR needs a narrow amendment before implementation;
- a running cancel can no longer claim the strong "never executes" guarantee even if the SQE has
  not yet reached the kernel — once ring-owned, cancellation is best-effort by design;
- permanent transport failure needs an explicit retirement proof instead of being hidden behind
  normal request state transitions;
- a bounded CQE router adds a small indirection versus raw slab index/pointer designs; benchmark
  evidence should decide its concrete representation;
- one ring per shard means aggregate ring memory grows with shard count, which is an intentional
  trade for locality and contention avoidance and must be measured before a topology phase.

These are clearer costs than the earlier design's semantic dependence on exact submit-prefix
accounting.

---

## 17. D0 verdict

**Recommended decision:** adopt the revised SOTA-aligned model and do not implement the previous
prefix-driven D1 design.

The key architectural choices are now:

```text
RequestArena = logical lifetime authority
private io_uring instance = execution-ownership domain
SQE installed in private ring = enqueued -> running
io_uring_submit / SQPOLL = transport progress only
original operation CQE = execution retirement / terminal candidate
reap = sole Completion publication
one ring per future shard/executor
```

The next step is therefore **not more Uring production code**. It is the narrow §3 ADR amendment,
followed by a much smaller D1 frozen design built around ring-owned lifetime.

That ordering keeps the repository honest: architecture authority changes first, implementation
second, benchmarks/topology third.
