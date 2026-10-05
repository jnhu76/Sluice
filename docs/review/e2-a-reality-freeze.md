# E2-A Reality / Reference Census Freeze (#452 execution, parent #401)

- **Status**: FROZEN (`E2_A_REALITY_FROZEN`)
- **Baseline**: `MASTER_SHA=ac613925612f1b17a0a9c41c754e397ffe06ec88` (branch
  `fix/401-e2-shutdown-replacement`, start HEAD identical; no rebase needed —
  live master equals the #452 recorded baseline)
- **Root**: `docs/explicit-io-v1-final-decision.md`, revision `v1-r3`
- **Issue states at freeze**: #400 CLOSED (adopted via PR #450), #401 OPEN,
  #402 OPEN (F2–F5 blocked), #452 OPEN
- **Ledger row at freeze**: `Shutdown and destruction | SHUT | NOT_ASSESSED`
- **Role**: this document freezes source reality and the reference census that
  E2-B..E2-G derive from. It records what the current implementation *is*,
  with executable counterexample evidence; it is not a contract. Contract
  derivation happens in E2-B from SHUT-01..04 only.

## 1. Live reality verified (per #452 §1, re-proven at HEAD)

### 1.1 AsyncIoContext

- No public/context-level `close_admission()`, `request_stop(policy)`,
  `shutdown(policy)` exists
  (`include/sluice/async/async_io_context.hpp:248-424` public surface).
  Admission closes today only through: progress-token exhaustion
  (`async_io_context.cpp:554-558`), backend poison
  (`request_core.cpp:194-198`), or the backend-level `close_admission()`
  whose in-repo callers are tests only.
- The destructor (`async_io_context.cpp:66-77`) is fail-fast checks only:
  (a) live progress binding (active drive / claimed owner / live external
  notification interest) → `async_context_progress_binding_fail_fast()`;
  (b) `core_->occupancy().public_bindings != 0` → fail-fast;
  (c) `backend_->outstanding() != 0` → fail-fast.
  There is no Settling driver, no ExecutionClosed representation, and no
  shutdown result. After the checks, members retire in declaration order
  (backend_, progress_, core_; `async_io_context.hpp:426-433`).

### 1.2 RequestCore derivations re-proven at HEAD

Per-slot facts present exactly as #452 records: `phase`, `binding_live`,
`terminal_chosen`, `published`, `publication_inflight`, `execution_refs`,
`control_refs`, `observer_phase`; aggregates `accepted_live`, `outstanding`,
`public_bindings`, `admission_open`, `health_failed`
(`request_core.hpp:156-276`, occupancy at `request_core.cpp:34-50`).

**Derivation D-A (accepted ⇒ binding_live).** `accept()` sets
`binding_live=true` atomically with `phase=accepted`
(`request_core.cpp:152-154`). `binding_live` transitions to false only in:
`release_public_binding` (requires `terminal_chosen && execution_refs==0 &&
(publication_inflight || published)`, `request_core.cpp:232-246`),
`discard_public_result` / `consume_public_result` (require `published`),
and `release_slot_` (full reclaim). All are publication-visible-or-later
paths. The `Request<T>` public surface fail-fasts any pre-publication
release (`request.hpp:143-167` → `request_nonterminal_release_fail_fast`).
**Holds.**

**Derivation D-B (outstanding>0 ⇒ public_bindings>0 at the destructor
observation point).** `occupancy().outstanding` counts `accepted && !published`
slots. The one pre-`published` binding-release window is
`publication_inflight` via the caller-`Completion` surface
(`completion.hpp:160-171`): `release_public_binding` accepts
`publication_inflight` as visible. That window can only be open while an
owner pass is between `begin_publication` and `complete_publication`
(backend `publish_one`). The destructor's serialized precondition excludes a
live drive (`drive_active_`), and a completed pass always closes the window
(`complete_publication` sets `published`), so no slot can be observed
`accepted && !published && !binding_live` at the destructor point.
**Holds at the destructor point** (not claimed for arbitrary racing
observers mid-pass).

Consequently, at the destructor point `backend_->outstanding()!=0` is, in
the external-violation direction, a redundant restatement of
`public_bindings!=0`; and `outstanding()==0` is neither ExecutionClosed
(published-but-unconsumed slots keep bindings and internal pins alive) nor
Destroyable (internal pins can outlive the last public binding — §3).

### 1.3 ThreadPool backend

Current teardown (`threadpool_backend.cpp:125-150`): pre-join
`core_->occupancy().accepted_live != 0` → `threadpool_non_quiescent_destruction_fail_fast()`;
then stop+drain worker lane, join workers, post-join quiescence re-check
(`dispatch_`, `active_workers_`, `publication_pending_`, `accepted_live`).
Claim-failed/stale dispatch entries are drained by the worker lane (the
pre-join check runs before the join for genuinely unsettled accepted work
only). Owner-side publication work lives in `poll_progress()`
(`threadpool_backend.cpp:599-649`): publication_pending_ drain
(`publish_one`) plus the `event_owed` delivery loop.

### 1.4 io_uring backend

#400-frozen invariants re-verified in source: CQ absence ≠ physical
retirement (`uring_transport.cpp:404-408`); negative submit requires
kernel-head reconciliation before any Class-A retirement
(`uring_transport.cpp:229-302`); cancel CQE ≠ original retirement
(`uring_completion.cpp:101-143`); post-poison CQ reap/overflow flush stays
live (`uring_completion.cpp:225-271`). Teardown
(`uring_backend.cpp:355-395`) is quiescence fail-fast (`dispatch_`,
`publication_pending_`, `live_cookies_`, `live_control_sqes_`, transport
ledger, `accepted_live`) then eventfd unregister + `io_uring_queue_exit`.

### 1.5 ProgressSource / notification

Reversed-ack detach (`detach_progress_host` acknowledges the external
interest is already gone), epoch/exhaustion saturation, eventfd
notification owned/drained solely by the source
(`progress_source.hpp:30-450`). E2 increment is ordering these facts into
the SHUT-02 step-7 teardown sequence; the pieces exist.

## 2. Reference census

EXTERNAL-BINDING TABLE (references whose lifetime is a caller obligation;
each blocks context destruction until released):

| REFERENCE | CREATOR / OWNER | TOUCHES CALLER MEMORY | TOUCHES FILE | CAN CHANGE RESULT | BLOCKS EXEC-CLOSE | BLOCKS DESTROY | RETIREMENT EVENT |
|---|---|---|---|---|---|---|---|
| `Request<T>` public binding (`slot.binding_live`) | submit caller / RAII handle | result read at consume | no | no (immutable post-publication) | no | **yes** | consume / discard / handle destruction |
| caller `Completion<T>` binding (compat surface) | submit caller / RAII | result write at publish, read at result() | no | no post-publication | no | **yes** | completion destruction/reset (releases core binding) |
| observer registration (`observer_phase` armed/queued/delivering) | attach caller | hook reads observer state during delivery | no | no | delivery may pin slot past exec-close | **yes** (external registration) | delivery retirement / explicit cancel |
| external progress/host registration (`notification_interest_live_`) | external loop host | callback may enter context | no | no | no (must detach before step 7) | **yes** | `detach_progress_host()` after loop unregister |
| progress owner / active drive (`owner_claimed_`, `drive_active_`) | driver thread | drive runs hooks/publish | indirectly | no | it *is* the driver | **yes** | ProgressOwner release / drive exit |

INTERNAL-REFERENCE TABLE (obligations the context itself must retire;
never caller violations):

| REFERENCE | WHO CREATES | BORROW-TOUCHING | TOUCHES FILE | CAN CHANGE RESULT | BLOCKS RECLAIM | BLOCKS EXEC-CLOSE | RETIREMENT EVENT / DRIVER |
|---|---|---|---|---|---|---|---|
| core execution ref (`execution_refs`, accept=1) | accept() | until physical outcome + terminal | yes (pre-physical) | offers terminal (first winner only) | yes | **yes** | worker/CQE `release_execution` |
| ThreadPool worker claim (`claim_execution`, running syscall) | worker dequeue | yes (pread/pwrite buffers) | yes | produces physical outcome | (transient) | **yes** | syscall return + terminal handoff |
| ThreadPool `publication_pending_` entry | worker outcome / won-cancel / poison drain | no | no | no (terminal already chosen) | no (slot pinned by binding) | **yes** (owner publication owed) | owner pass `publish_one` |
| ThreadPool `delivery_[slot]` `{completion*, publish, event_owed}` | submit | completion ptr until publish | no | no | no | event_owed: **yes** | publish + event delivery loop |
| core control ref (`control_refs`) | zero-op event pin; uring control SQE | **no** (post-publication only survivable form) | no | no | **yes** | zero-op pin: no (event delivery is core-local); uring control: **yes** until CQE | release_control (delivery loop / control CQE / poison Class-A) |
| uring prepared op in `dispatch_` (kernel-invisible SQE) | submit dispatch | on submit | yes | future physical | no (slot pinned) | **yes** | submit consumption or rollback |
| uring router entry + `live_cookies_` (kernel-visible original op) | dispatch_one_locked | until delivered CQE | yes | produces physical outcome | yes | **yes** | original CQE → terminal → publish |
| uring control/cancel SQE (`control_state`, `live_control_sqes_`, ledger `cancel_control`) | cancel on running op | **no** (cancel touches no buffers) | no | no | **yes** (pins slot after binding release) | **yes** | control CQE reap / poison Class-A |
| uring sticky cancel intent (`control_pending`) | SQE-exhausted cancel | no | no | no | no (no ref) | no | later service pass or original retirement (discarded) |
| transport ledger entry (operation/cancel_control) | dispatch/control prep | indirectly (tracked op) | indirectly | no | no | **yes** (until consumed/reconciled/Class-A) | submit consumption / reconciliation / poison mark |
| observer delivery episode (`queued→delivering`) | publication with armed observer | hook reads observer state | no | no | **yes** | delivery must retire before step 7 | driver `claim→hook→retire` |
| ProgressSource wait state / parked owner | owner wait | no | no | no | n/a | **yes** (final wait must end) | wake + wait return; control ack |
| ready-but-unconsumed published slot | publication | no | no | no | yes (REQ-05) | **no** — ExecutionClosed preserves it; destruction blocked by the *public binding*, not this | consume/discard |

### Frozen candidate dimensions

- `external_precondition_clear` ≡ `public_bindings == 0 ∧ live external
  progress/host registration == 0 ∧ live external observer registration == 0
  ∧ no live public Completion binding`.
- `internal_retirement_complete` ≡ no execution refs; no worker claims /
  kernel-visible borrow-touching ops; no owner-side publication/delivery work
  owed; no control refs / control SQEs / transport-ledger entries; observer
  deliveries retired; backend workers/ring resources retired.

## 3. C2/D4 witnesses — real, reachable, frozen

#452's correction is honored: `ThreadPool publication_pending_ != 0` is NOT
a witness (pre-publication accepted work keeps its public binding live).

**WITNESS-TP (ThreadPool): zero-op deferred-event control pin.**
`submit_read(len==0)` publishes inline on the submitter thread
(`threadpool_backend.cpp:365-384`): publication completes, but the owed
`ReadyEvent` delivery is deferred to a later progress pass, pinned by
`control_refs==1` + `delivery_[slot].event_owed==true`. After the caller
consumes the published result, the state is exactly
`public_bindings==0 ∧ external registrations==0 ∧ internal non-borrow
retirement obligation==1`. Reachable via the public API today.
Executable evidence at HEAD: `/tmp` probe (zero-op read, `take_result()`,
destroy context without a poll pass) → child dies `SIGABRT` in
`~ThreadPoolBackend` pre-join `accepted_live != 0` — **the internal pin is
misclassified as a caller violation (current FAIL, the D4/M2 bug).**

**WITNESS-UR (io_uring): late control SQE outliving the released binding.**
Cancel a running op (control SQE prepared/submitted, `acquire_control`),
the original op's CQE retires first (router entry intentionally kept,
`uring_completion.cpp:94-96`), publication runs, the caller consumes the
result; the control CQE has not arrived. State: `router_[i].in_use`,
`control_state=prepared|submitted`, `live_control_sqes_>=1`, core
`control_refs==1`, slot published+binding-gone+non-reclaimable
(`request_core.cpp:383-394` refuses only *new* control refs after binding
release; one acquired earlier survives). The control SQE itself touches no
caller memory (IORING_OP_CANCEL64 with a cookie target).
Executable evidence at HEAD: deterministic submit-fiction probe (liburing
2.10 built at `/tmp/liburing-prefix`, ring 4×8, inject only the original
CQE, consume, destroy) → child dies `SIGABRT` in `~UringAsyncBackend`
quiescence check — **same misclassification (D4/M2) on the uring side.**

Sticky-intent variant (`control_pending`, no SQE/ref) is explicitly NOT a
witness (no kernel state, no ref); it is discarded or serviced without
blocking anything.

## 4. Counterexample matrix status (current implementation)

| Case | Scenario | Current result at HEAD | Evidence |
|---|---|---|---|
| C1 | terminal published, `Request<T>` retained, drive shutdown to ExecutionClosed | **UNREACHABLE** — no context-level shutdown path exists; the only exits are "keep driving polls" or destructor | §1.1; existing half: `destroy_context_with_live_public_binding` death test (`tests/public_request_release_violation_test.cpp:145-167`) proves only the destructor half (part of V21), nothing about V20 |
| C2 | public_bindings==0 ∧ external==0 ∧ internal pin>0 → destructor | **FAIL (dies)** on both backends — internal pins misclassified as caller violations | WITNESS-TP / WITNESS-UR probes above (exit-6 child runs, recorded 2026-10-06, gcc 15 / liburing 2.10 local prefix / WSL2 6.18.33.2) |
| C3 | reserve→pause→close→accept vs accept→close | Core linearization implemented + tested (`request_core.cpp:107-198`, `tests/request_core_protocol_test.cpp:119-136,485-510,1092-1113`; submit-seam winners in `tests/threadpool_core_cutover_test.cpp`); **gap**: no context-level `close_admission()` wiring to exercise the race through the public context API | §1.1 |
| C4 | poison × unresolved physical access | #400 frozen behaviors present and mutant-guarded (absence-settles / cancel-settles-original / batch-invisible mutants all compiled out of default builds); poison closes admission, keeps borrows, flush stays live | §1.4; `xmake/tests.lua` E1 mutant targets |
| C5 | progress fd detach/reuse | reversed-ack detach + epoch discipline exist (`progress_source.hpp`, external-loop tests `tests/*_external_loop_test.cpp`); **gap**: not yet sequenced inside a full shutdown ordering (SHUT-02 step 7) | §1.5 |

## 5. Environment record (evidence runs)

- Machine: WSL2 `6.18.33.2-microsoft-standard-WSL2`, gcc 15 (system), xmake
  2.9.7.
- liburing: **not installed system-wide; no passwordless sudo.** Built from
  source `liburing-2.10` tag at `/tmp/liburing-prefix` (static + shared) for
  probe runs. Real-kernel uring conformance evidence for E2-G must record
  this environment explicitly; the #400 environment (WSL2 6.18.40.1,
  liburing 2.14) is NOT inherited as this ticket's evidence.
- Probe sources preserved in this PR's test suite (the E2-F death-test files
  reproduce WITNESS-TP/WITNESS-UR scenarios deterministically).

## 6. Gaps frozen for E2-B..E2-G

1. No five-state context lifecycle (Open→AdmissionClosed→Settling→
   ExecutionClosed→Destroyable) with derivable predicates (E2-B).
2. No context-level `close_admission()` / `request_stop(policy)` /
   `shutdown(policy)` wired to the canonical admission authority (E2-C);
   observer attachment is not yet gated on admission close (SHUT-02 step 3).
3. Destructor treats every non-idle state as a caller violation: it must
   first check external preconditions, then own internal retirement
   (drive owed publication/delivery/control retirement) — the exact
   inversion both probes demonstrate (E2-C/E2-D/E2-E).
4. `outstanding()==0` conflates nothing today (it is only a destructor
   check) but no ExecutionClosed/Destroyable predicates exist; ThreadPool
   `accepted_live` and uring quiescence facts are raw backend mechanism,
   not a unified semantic view (E2-B/E2-D/E2-E).
5. No V20/V26 oracles; V21 has only the ready-unconsumed half (pending
   half missing); V23 ordering inside a full shutdown untested (E2-F).
6. No shutdown/reference formal model (E2-G).

Gate: `E2_A_REALITY_FROZEN` — recorded. E2-B contract derivation may start.
