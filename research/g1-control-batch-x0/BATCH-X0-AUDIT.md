# BATCH-X0-AUDIT — as-built Batch / AsyncIoContext / backend topology

- Campaign: BATCH-X0 (G1-Control Candidate 3) under #227 / #221 / #259
- Baseline: master `4a57cc4f` (merge of PR #281 COPY-X0). Branch
  `research/g1-control-batch-x0`.
- Method: every fact below was recovered from current master sources, tests,
  and docs at the baseline SHA. Comments were verified against code; where a
  comment differs from code the CODE is recorded and the comment noted.
- Classification vocabulary: `PUBLIC/SEMANTIC CONTRACT`,
  `CURRENT IMPLEMENTATION`, `ACCIDENTAL CURRENT BEHAVIOR`,
  `BACKEND-SPECIFIC FACT`, `UNKNOWN`.

Sources read in full or in the cited regions:

```text
include/sluice/async/batch.hpp            (whole file, 168 lines)
src/async/batch.cpp                       (whole file, 169 lines)
include/sluice/async/async_io_context.hpp (whole file)
src/async/async_io_context.cpp            (whole file)
include/sluice/async/detail/submit_transaction.hpp (whole file)
include/sluice/async/uring_backend.hpp    (whole file)
src/async/uring_backend.cpp               (submit/enqueue/dispatch/transport/poll/wait_one/cancel regions)
include/sluice/async/detail/request_arena.hpp (lock structure + reap signature)
include/sluice/async/completion.hpp       (reap_seq / publish_from_reap)
tests/batch_test.cpp, tests/batch_reap_order_test.cpp, tests/batch_result_origin_test.cpp
docs/reference/api.md §Batch (lines 1515-1560)
include/sluice/experimental/uring_write_batch.hpp (prior-art disambiguation)
xmake/benchmarks.lua (bench target precedents)
```

---

## 1. The public object under study

### 1.1 Public API shape — PUBLIC/SEMANTIC CONTRACT

`sluice::async::Batch` (batch.hpp:92):

```text
add(BatchOp) -> index                     — append one op descriptor
await_one(AsyncIoContext&) -> Result<size_t> — submit all un-submitted ops;
                                             drive ctx.wait_one() until >=1
                                             UNPOPPED batch slot is ready
next() -> optional<BatchResult>           — pop in backend reap order
pending_count()                           — added − popped
```

`BatchOp::Kind` ∈ {read, write, sync_data, sync_all} — the mixed-kinds batch
is part of the declared contract (S5 scope). `BatchResult{index, origin,
is_void, size_res | void_res}`.

Non-copyable, non-moveable. Default-constructible; the context is passed to
`await_one`, not to the constructor.

### 1.2 BatchResultOrigin — PUBLIC/SEMANTIC CONTRACT

(batch.hpp:58-78) `rejected` vs `accepted_and_completed`, ORTHOGONAL to
success/error. `rejected` = submit failed BEFORE commit/accept (invalid
descriptor, capacity full, non-idle Completion). `accepted_and_completed` =
crossed the accept LP and later got a terminal via reap — including accepted
I/O errors and accepted cancellation winners. "Do NOT infer origin from
whether the Result is success/error." (ADR Decision 9 is cited in the header.)

### 1.3 Reap-order iteration — PUBLIC/SEMANTIC CONTRACT

(batch.hpp:121-135; batch.cpp:124-167) `next()` pops the ready-unpopped slot
with the SMALLEST `reap_seq`; submit-time rejections carry reap_seq 0 and
surface BEFORE any backend-reaped completion, in submission order among
themselves (tie-break by index). Each slot pops exactly once. `reap_seq` is a
process-wide monotonic counter stamped by `publish_from_reap()`
(completion.hpp:116, 423-425).

### 1.4 await_one error propagation — PUBLIC/SEMANTIC CONTRACT

(batch.hpp:106-119; batch.cpp:70-121) A backend `wait_one()` error is
returned from `await_one`, but slots made ready in the same call (submit
rejections, earlier reaps) REMAIN poppable via `next()`. A success return
carries the count of unpopped ready slots (can be > 1).

### 1.5 Cancellation — PUBLIC/SEMANTIC CONTRACT (a NEGATIVE grant)

`Batch` has NO cancel API. There is no group-cancel operation anywhere in
batch.hpp/batch.cpp. Cancellation of batch members is the caller issuing
per-Completion `AsyncIoContext::cancel()` — i.e. exactly the per-request
cancel contract (ADR Decision 11). "cancel Batch" does not exist; any
campaign mutant that introduces group-cancel semantics is introducing a NEW
operation, not optimizing an existing one (M5 / §11 of the campaign goal).

### 1.6 Group admission — PUBLIC/SEMANTIC CONTRACT: NOT GRANTED

The header states Batch is a DRIVER over the existing per-op `submit_*`
(batch.hpp:9-13: "it submits N ops via the existing per-op submit_*"; the
class comment names the integration seam "the existing L1 surface"). Nothing
in batch.hpp, batch.cpp, or docs/reference/api.md:1515-1560 grants
contiguous/group admission, batch-level atomicity, or any admission priority.
The accept linearization point of each member remains that member's own
`binding -> outstanding` release-store inside its own per-op submit call.

**Central §8 question (does Batch permit non-interleaving?) — audited
answer: the contract preserves interleavability.** Each member submit is an
independent serialized context operation (async_io_context.cpp:147-174 —
every `submit_*` individually acquires `access_mtx_`, runs its own admission
transaction, releases); a concurrent submitter holding the lock between two
member submits is a legal linearization of the CURRENT implementation, and no
contract text excludes it. Corollary: an amortized path that holds one
admission critical section across all N members would produce an admission
order the current implementation can produce only accidentally, never
guarantees, and — under capacity pressure — a DIFFERENT accepted/rejected
membership in a concrete interleaved run. Verdict input for
BATCH-X0-SEMANTIC-GRANT; to be demonstrated (not merely argued) by fixture
S9 / M4.

### 1.7 Zero members / one member — ACCIDENTAL CURRENT BEHAVIOR (S6/S7)

From batch.cpp:22-122:

- N=0 with an idle context: Phase 1 submits nothing; `any_ready` false;
  `ctx.outstanding() == 0` skips the wait loop; returns 0; `next()` returns
  nullopt. No error.
- N=0 on a context with UNRELATED outstanding work: the loop condition
  `ctx.outstanding() > 0` is CONTEXT-GLOBAL, not batch-scoped — `await_one`
  keeps calling `wait_one()` until the unrelated work drains, then returns 0.
  The batch's readiness predicate counts only batch slots, but the WAIT
  predicate counts the whole context. This asymmetry is as-built, undocumented
  in api.md, and part of any honest semantic floor.
- N=1: no special case; behaves as one op (S7 negative control).

### 1.8 Existing semantic test coverage — evidence inventory

```text
tests/batch_test.cpp                      — basic add/await/next; mixed kinds
tests/batch_reap_order_test.cpp           — ScriptedBackend (SequenceBackend)
                                            through the PUBLIC submit path:
                                            reap orders (single/reverse/
                                            forward/mixed), multi-reap per
                                            wait, one-per-wait, next-before-
                                            ready = nullopt, exactly-once,
                                            mixed-kind order, submit-failure
                                            surfaces first, wait-error
                                            propagation + ready-slots-stay-
                                            poppable, success-vs-error
                                            distinction
tests/batch_result_origin_test.cpp        — origin rejected on submit
                                            failure; accepted on success /
                                            terminal error / canceled
                                            terminal; mixed ordering
tests/uring_write_batch_test.cpp          — the EXPERIMENTAL synchronous
                                            writer, NOT sluice::async::Batch
```

Coverage already includes most of S1/S3/S4/S10 semantics at the
contract level with a deterministic scripted backend. NOT covered anywhere:
S2 (capacity pressure with a real arena backend — which member is rejected),
S8 (N = C-1/C/C+1), S9 (concurrent external submitter interleaving), and any
transport-counter evidence. BATCH-X0 fixtures must add exactly these and must
NOT re-litigate what the regression suite already proves.

### 1.9 `sluice::experimental::UringWriteBatch` — not this object

A synchronous, own-ring experimental write pipeline
(include/sluice/experimental/uring_write_batch.hpp). Shares nothing with
`sluice::async::Batch` semantics; excluded from this campaign.

---

## 2. The per-op submission path (the control plane BATCH-X0 asks about)

### 2.1 Context serialization — CURRENT IMPLEMENTATION

(async_io_context.cpp:147-174) EVERY `submit_*` acquires the context-level
`access_mtx_`, calls the backend submit, tallies stats, releases. N ops via
Batch = N separate `access_mtx_` critical sections. This is the FIRST
per-op control cost.

### 2.2 Shared admission ladder — CURRENT IMPLEMENTATION

(submit_transaction.hpp:94-205) Each op runs the full pre-accept ladder:

```text
stage0_precheck -> reserve -> validate -> prepare -> write_scratch
-> install_publication_binding -> begin_binding CAS -> commit
-> install_binding + commit_binding   (the accept LP)
```

The arena operations (reserve, prepare, install_publication_binding, commit,
rollback) EACH acquire the RequestArena leaf mutex (request_arena.hpp lock
structure: one `mutex_` leaf domain; ~6 acquisitions per successful op
counting enqueue/mark_running below). The Completion CAS + release-store are
per-op atomics. This is the SECOND per-op control cost, and the one whose
fusion is a RequestArena-shape question (out of BATCH-X0 scope by the goal's
non-authorization list).

### 2.3 Uring backend enqueue/dispatch — CURRENT IMPLEMENTATION

(uring_backend.cpp:640-708) After the admission transaction (under
`dispatch_mtx_`), `enqueue_after_commit(h)` runs a SECOND `dispatch_mtx_`
critical section: `arena_.enqueue(h)` (arena leaf lock), dispatch ring
push_back, then a FIFO-front drain of `dispatch_one_locked` — router
free-list pop, `io_uring_get_sqe`, SQE prep, cookie allocation,
transport-ledger append, `arena_.mark_running` (arena leaf lock),
`remove_exact`. This is the THIRD per-op control cost.

### 2.4 Transport flush policy — CURRENT IMPLEMENTATION (load-bearing for Q1)

`dispatch_one_locked` calls `submit_transport_locked()` ONLY on the
NULL-SQE retry path (SQ full; uring_backend.cpp:830-849). The normal path
installs the SQE and returns WITHOUT any syscall (comment at :920-922:
"io_uring_submit() may happen now or later — it is transport progress").

The flush points are:

```text
poll()      : drain dispatch queue -> submit_transport_locked() (one
              io_uring_submit) -> reap_cqes() -> arena_.reap()
wait_one()  : same drain+flush, then either a final reap or
              io_uring_submit_and_wait(min_complete=1)
```

(uring_backend.cpp:1526-1602).

**Consequence (BATCH-T1):** N consecutive primitive `submit_*` calls install
N SQEs into the SQ with ZERO enters between them (as long as the SQ has
room); the single kernel enter happens at the first poll/wait_one. The
kernel ALREADY sees a batch of N SQEs from unbatched primitive submits when
`queue_depth >= N`. "Multiple SQEs reaching one io_uring_submit()" is
therefore NOT evidence of Batch adding anything — the prompt's §4 suspicion
is structurally confirmed by the as-built code and must be verified
empirically (Q1) before any transport claim.

### 2.5 Reap/publication — CURRENT IMPLEMENTATION

(uring_backend.cpp:1445-1507, 1558-1568) `reap_cqes()` reads ALL currently
ready CQEs in batched `io_uring_peek_batch_cqe` loops;
`arena_.reap(sink)` publishes ALL backend-ready slots in one call, each
through its slot's publication binding, stamping per-op reap_seq.
Completion-side driving (BATCH-C2) is ALREADY episode-shared: one
wait_one/poll reaps every ready completion, regardless of Batch.

### 2.6 Stats tally — CURRENT IMPLEMENTATION

(async_io_context.cpp:101-121, 147-152) Per submit: `submit_calls`++,
outcome counter, and (when a stats sink is attached) a
`backend_->outstanding()` virtual call + arena leaf lock round-trip for
`max_outstanding`. With NO stats sink, max-outstanding evaluation is skipped
(#261). BATCH-X0 perf runs attach NO stats sink (production-faithful) and
count at bench level instead.

### 2.7 ThreadPoolBackend — BACKEND-SPECIFIC FACT (control arm only)

ThreadPool admission mirrors the same submit_transaction ladder under its
own admission lock, with a bounded dispatch queue and persistent workers;
backend progress is thread-offload, not kernel transport. Included only as
the optional backend-generic control (wrapper cost vs transport value), not
as a primary arm.

---

## 3. Batch wrapper costs (Q2 candidates — measured, not presumed)

```text
W1  one unique_ptr<Slot> heap allocation per add()          (batch.cpp:15)
W2  every Slot carries BOTH Completion<size_t> and
    Completion<void> — one is always dead weight            (batch.hpp:145-147)
W3  await_one: >=3 full-slot linear scans per call
    (submit scan; any_ready scan; ready_count scan) + one
    harvest scan PER wait-loop iteration                    (batch.cpp:28-114)
W4  next(): linear scan over all slots per pop, reap_seq
    compare per candidate                                   (batch.cpp:132-149)
W5  worst-case O(N^2) harvest across one episode (scan all
    slots per wake)                                         (W3 consequence)
```

These are CURRENT IMPLEMENTATION facts. Whether they are material is a
measurement question (Q2), NOT a defect claim.

---

## 4. Amortization candidate inventory (Q3 raw material)

Per successful uring op, outside the arena leaf and outside SQE prep
(per-op identity work that NO legal fusion may remove):

```text
A1  access_mtx_ acquire/release                    (context)
A2  dispatch_mtx_ acquire/release x2               (admission txn; enqueue)
A3  RequestArena leaf mutex x ~6                   (reserve/prepare/install/
                                                    commit/enqueue/mark_running)
A4  Completion begin_binding CAS + commit
    release-store                                  (accept LP — per-op semantic)
A5  stats tally + optional outstanding() call      (context)
A6  dispatch drain bookkeeping per submit call
```

A1/A2/A5/A6 are context/backend-outer control work; A3 is arena-shaped; A4
is the per-op accept LP itself (never fusable). Whether ANY of A1/A2/A5/A6
can be legally amortized is decided by the §8 admission-order question and
the S9 fixture — NOT by this inventory.

---

## 5. Frozen fact classification table

| # | Fact | Class |
|---|---|---|
| F1 | Batch = driver over per-op submit_*; mixed 4 kinds; reap-order next(); origin orthogonal to error; exactly-once; wait-error keeps ready slots poppable | PUBLIC/SEMANTIC CONTRACT |
| F2 | No group admission grant anywhere in header/docs; per-op accept LP preserved | PUBLIC/SEMANTIC CONTRACT |
| F3 | No Batch cancel API; member cancel == per-request cancel | PUBLIC/SEMANTIC CONTRACT |
| F4 | Each member submit = own access_mtx_ section + own admission ladder + own enqueue episode | CURRENT IMPLEMENTATION |
| F5 | Uring flush policy: no enter per op; one flush at next poll/wait_one (SQ-room case) | CURRENT IMPLEMENTATION |
| F6 | One poll/wait_one already reaps+publishes ALL ready completions | CURRENT IMPLEMENTATION |
| F7 | Wrapper costs W1-W5 (allocs, dual Completion, O(N) scans, O(N^2) harvest worst case) | CURRENT IMPLEMENTATION |
| F8 | N=0 batch waits on context-global outstanding; N=1 has no special path | ACCIDENTAL CURRENT BEHAVIOR |
| F9 | Uring request_capacity independent of queue_depth; SQ-full -> flush+retry -> stay enqueued; FIFO dispatch | BACKEND-SPECIFIC FACT |
| F10 | ThreadPool = thread-offload backend; same ladder, different progress mechanism | BACKEND-SPECIFIC FACT |
| F11 | B1-vs-B2 io_uring_enter counts on the real kernel | UNKNOWN — measure (Q1) |
| F12 | Wrapper cost materiality vs per-op control cost | UNKNOWN — measure (Q2) |
| F13 | Whether fused admission changes accepted membership under concurrent capacity pressure | UNKNOWN — fixture S9/M4 |
| F14 | Whether a standalone thin implementation gets the same topology without Sluice semantics | UNKNOWN — measure (Q4/B0) |

---

## 6. Governance mapping (AGENTS.md §8)

Applicable architecture-constitution articles:

```text
AC-1  Explicit Capability Boundary      — a batch submit entry would BE the
                                          capability question (not granted
                                          here)
AC-2  Explicit Operation Identity       — per-op RequestKey/generation must
                                          survive any fusion (M7)
AC-3  Transactional Submission          — per-op ladder/rollback shape
AC-4  Accepted Operation Must Terminate — batch members each need a terminal
                                          path (M1 mutant)
AC-5  Single Completion Publication     — reap-only publication (M1/M7)
AC-6  Explicit Wake Obligation          — wait_one driving unchanged by arms
AC-7  Bounded or Caller-Owned Resources — batch slots caller-owned; arena
                                          capacity pressure -> would_block
                                          (S2/S8)
AC-9  Layered Cancellation              — member cancel vs batch (F3)
AC-10 Documentation-Interface-          — api.md Batch section is the public
       Implementation Authority            contract mirror (F1/F2)
AC-11 Tests Prove Semantics             — fixtures prove contract, not
                                          implementation preference
AC-13 Unforgeable Publication Authority — scripted backends publish through
                                          the protected helper only
AC-15 Completion Identity Preservation  — per-member Completion identity in
                                          every arm
```

Governing Accepted ADRs: `docs/adr/ADR-explicit-io-request-contract.md`
(request lifecycle, Decisions 5/6/9/11/13), ADR-explicit-io-completion-
authority (two-stage binding, publication authority). BATCH-X0 is
research-only: no production target changes, so the §8 compliance gate is
satisfied at the DESIGN level (this audit + preregistration); PENDING/PASS
markers apply to the research harness, not production.

---

## 7. Audit verdicts feeding the preregistration

```text
V1  The §8 central question has a documented-code answer: Batch does NOT
    grant group admission (F2). The fixture suite must still DEMONSTRATE the
    observable consequence (S9 + M4) before the SEMANTIC-GRANT verdict may
    cite it.
V2  BATCH-T1 (kernel transport batching) is structurally already available
    to primitive consecutive submits (F5); Q1 must quantify B1 vs B2 vs B0
    io_uring_enter counts to convert this into evidence.
V3  BATCH-C2 (completion-side amortization) is already episode-shared by
    poll/wait_one (F6) for BOTH primitive loops and Batch — the marginal
    completion value of Batch is expected zero; verify via drive-episode
    counters.
V4  BATCH-C1 (control-plane amortization) raw material exists (F4, §4) but
    its legality is bounded by V1: fusing A1/A2 across members would create
    contiguous group admission — a new semantic unless S9 proves otherwise.
V5  Q2 wrapper costs are real but unquantified (F7/F12) — measure before
    judging.
V6  Existing regression tests already pin most of the declared Batch
    contract (§1.8); BATCH-X0 semantic fixtures only add S2/S5-arena/S8/S9
    and the scripted-reap permutations needed for mutants M1-M6.
```
