> **HISTORICAL / EVIDENCE — NOT CURRENT AUTHORITY.** Archived from `docs/architecture/` by S0-DOCS (#290, 2026-09-04). Point-in-time record; do not cite as authority for new decisions.

# Phase C2e Compliance Gate — Close / Drain / Reset / Destruction

**Roadmap:** [`remediation-roadmap.md`](remediation-roadmap.md) — Phase C (status PARTIAL; C2e COMPLETE)
**Governing ADR:** [`ADR-explicit-io-request-contract`](../../adr/ADR-explicit-io-request-contract.md) (Accepted) — Decisions 11, 13, 15; invariants I8, I9, I17, I19
**Issue #68:** https://github.com/jnhu76/Sluice/issues/68 — C2e scope (rows 15–16)
**Branch:** test/phase-c2e-close-drain-destruction
**Baseline:** `origin/master` @ `0b6c0b9126e6461d0317dee81e460f2abcc22f02` (PR #72, C2d merged)
**Scope:** Tests + the backend admission transaction domain (ADR §"Commit /
accept" :453-462) on `ThreadPoolBackend` and `FakeAsyncBackend` — close
serializes against an in-flight acceptance protocol and the acceptance
linearization point is the Step 5 `binding -> outstanding` release-store — +
`FakeAsyncBackend::close_admission()` (the C2e reference method) + a
context-level interrupt-window detector (test-only split-wait backend; no
production context seam) + manifest/gate records + docs. No new public request
handle, no Uring migration, no Scheduler/Batch/wake-phase work.

This is the PR-level evidence ledger for Phase C2e, the fifth C2 semantic-coverage
slice: **admission close / drain / reset / quiescent destruction** (rows 15–16).
C2e closes row 15 for the Fake reference path and the real ThreadPoolBackend,
strengthens row 16's already-FULL evidence with the `pending`-state death case
and a Fake-type death target, records Uring's Phase-D gap as a
`not_implemented` manifest record that enters Uring's verdict, and proves every
non-neutral defect injection is mutation-sensitive: all detector cases fail on
deliberately nonconforming code (mutants M1–M12; M6/M7 are documented
redundant authorities, not detector lines).

---

## 1. Scope

| Requirement (Issue #68 row) | Evidence |
|---|---|
| 15 — close/drain/reset sequence (close → new submit `invalid_state`; close → existing reap/cancel legal; reset releases slot) | Shared suite: `close_rejects_future_submit`, `close_preserves_accepted_terminal`, `drain_then_reset_releases_slot`, `slot_released_but_admission_stays_closed` (Fake + ThreadPool via `conformance_close_drain_fake` / `conformance_close_drain_threadpool`). Admission-LP arbitration: `close_admission_gates_reserve_not_inflight_prepared_slot` (arena boundary — close gates NEW acceptance at reserve; an in-flight prepared slot completes its protocol) + `tp_c2e_close_waits_for_inflight_acceptance_lp` / `tp_c2e_close_wins_submit_started_before_close_rejected` (ThreadPool admission transaction) + `fake_c2e_close_waits_for_inflight_acceptance_lp` (Fake admission transaction; ADR §"Commit / accept" Step 5 — the `binding -> outstanding` release-store is the acceptance LP; the winning submit retains its admission lock through Step 5). ThreadPool windows: `tp_c2e_close_while_pending_preserves_accepted_request`, `tp_c2e_close_while_enqueued_preserves_dispatch`, `tp_c2e_close_while_running_result_verbatim`, `tp_c2e_close_while_running_void_result_verbatim`, `tp_c2e_void_submit_after_close_rejected`, `tp_c2e_close_then_pending_cancel_wins`, `tp_c2e_close_then_running_cancel_intent_only`, `tp_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin`, `tp_c2e_close_before_final_terminal_no_lost_ready`, `tp_c2e_final_terminal_before_close_not_affected`, `tp_c2e_close_races_workers_invariant_drain`, `tp_c2e_submit_races_close_linearization`, `tp_c2e_interrupt_final_reap_closes_ready_race`, `ctx_wait_one_interrupt_final_poll_closes_ready_race` (context-level interrupted-branch final poll) |
| 16 — quiescent destruction (accepted_outstanding == 0 / slot_in_use == 0 before destroy) | ThreadPool death matrix (now incl. `pending`): `tp_death_destroy_with_pending` + existing `tp_death_destroy_with_{enqueued,running,backend_ready,completion_ready}` + `tp_death_control_quiescent_destroy`; Fake-type death target: `fake_death_destroy_with_unreaped_request`, `fake_death_destroy_with_ready_unreset`, `fake_death_control_quiescent_destroy`; arena death (`arena_death_destroy_with_slot_in_use`), Completion ready-destruction release (`binding_release_capability_ready_destruction_releases_slot`) |

**Out of scope (explicitly, unchanged from Issue #68):** rows 4b/12b/14b
(Phase F), Uring RequestArena migration (Phase D), Scheduler/Batch identity
consumption (Phase F), backend-ready wake bridge (Phase G), public
`RequestHandle`, any change to `AsyncIoContext`'s public surface.

## 2. Authority

- **Issue #68** — the C2e design authority: row 15 (close/drain/reset sequence;
  PARTIAL at C2e start: ThreadPool close cases only, no Fake evidence, no
  deterministic close-while-\* / close‖terminal windows, no shared suite) and
  row 16 (quiescent destruction; FULL at C2e start — re-audited and confirmed,
  then strengthened with the `pending` death case and the Fake-type death
  target).
- **ADR-explicit-io-request-contract (Accepted)** — Decision 15 (admission
  close, drain, and destruction: destructors do not implicitly close/cancel/
  drain/block; destroying with a bound/non-free slot is a contract violation
  in Debug and Release; the explicit lifecycle
  `close admission -> continue progress and reap -> callers reset/destroy
  ready Completions -> accepted-outstanding == 0 and slot-in-use == 0 ->
  destroy`; `close_admission` atomically prevents new acceptance, new submits
  return synchronous `invalid_state` with idle Completion and no borrow;
  existing pending/enqueued/running/kernel-owned operations continue toward
  their ordinary terminal result; poll/reap, operation cancellation, and
  waiter cancellation remain legal; the default drain is graceful and does not
  silently turn queued requests into `canceled`; a ready-Completion
  destructor performs the same allocation-free slot release as `reset()`),
  Decision 11 (cancellation target and disposition — running cancel records
  intent only; the real result wins verbatim), Decision 13 (bounded capacity
  and observability — `slot_in_use` counts reserve through reset/ready-
  destruction release), and §"Commit / accept" (the five-step submission
  protocol: Step 5 — the `binding -> outstanding` release-store — is the
  commit/accept linearization point, and "the winning submit performs this
  protocol while retaining its own context/admission lock" — the backend
  admission transaction domain that close serializes against).
- **AGENTS.md** §10.7 (slot release: clears binding, increments generation,
  decrements slot-in-use, allocates nothing, waits for no asynchronous
  progress), §14 (shutdown and destruction: destructors do not implicitly
  close admission, cancel accepted work, drain, wait for asynchronous I/O, or
  publish terminal results; the explicit lifecycle; a persistent worker
  backend may notify and join already-idle workers during quiescent
  destruction — that join is worker-pool teardown, not implicit I/O drain),
  §16.3 (TSan for the concurrency classes below), §18 (conformance
  philosophy), §22 (commit discipline).
- **Architecture Constitution** — AC-4 (accepted terminality), AC-7 (bounded
  resources), AC-12/AC-13 (identity / reap authority).
- **Issue #67** — the split-phase ready wait + control interrupt protocol that
  makes close's one-shot wake correct (no lost wake, no busy-spin, no
  fabricated completion).

## 3. The C2e semantics (close / drain / reap / release / destruction are distinct)

The five lifecycle controls are DISTINCT authorities and MUST NOT be conflated
(ADR Decision 15; AGENTS.md §14):

```text
close admission   — atomic admission refusal: new reserve -> invalid_state
                    (Completion idle, no borrow). Never fabricates a terminal,
                    never touches existing requests. Idempotent.
graceful drain    — existing accepted requests continue toward their ordinary
                    terminal; the terminal winner stores the real result;
                    reap (poll/wait_one) publishes Completion-ready. close
                    does NOT retroactively cancel or rewrite.
reap              — the SOLE Completion-ready publication authority (workers
                    only record backend-ready).
Completion reset / slot release — caller-owned: ready -> resetting -> idle;
                    returns the slot (generation++, slot_in_use--) under the
                    leaf domain; allocation-free, no waits. A ready
                    Completion's DESTRUCTOR performs the same release.
backend destruction — quiescent + fail-fast: verified accepted_outstanding == 0
                    AND slot_in_use == 0 AND backend_ready == 0 AND no active
                    worker AND empty dispatch ring; then stops + joins the
                    already-idle persistent workers. Non-quiescent destruction
                    terminates in BOTH Debug and Release.
```

The key accounting distinction C2e pins with tests:

- `accepted_outstanding == 0` (drained) does NOT imply the backend is
  releasable: a completion-ready-but-unreset Completion still holds its slot
  (`slot_in_use == 1`), so destruction must fail-fast until the caller resets
  or destroys the ready Completion (`drain_then_reset_releases_slot`,
  `fake_death_destroy_with_ready_unreset`).
- `slot_in_use == 0` (released) does NOT imply admission is open: a released
  slot does not re-open admission (`slot_released_but_admission_stays_closed`).

### 3.1 Close linearization

The acceptance linearization point is the `binding -> outstanding` release-store
(ADR §"Commit / accept" Step 5 — "Step 5 is the commit/accept linearization
point"; the arena's `commit()` is its SLOT HALF, Step 4: `prepared -> pending` +
pin + accepted++ + borrow). Decision-15 arbitration ("close_admission atomically
prevents new acceptance") lives in the BACKEND admission transaction domain
(ADR :453-462 — "the winning submit performs this protocol while retaining its
own context/admission lock"): the submit paths hold `admission_mtx_` across the
whole Step 1-5 protocol (reserve -> prepare -> begin_binding -> arena commit ->
install_binding -> commit_binding), and `close_admission()` takes the same lock
before `arena_.close_admission()` (the ThreadPool additionally interrupts
parked waiters AFTER the lock is released). Therefore after
`close_admission()` returns NO new acceptance LP can occur, and submit‖close
linearizes as either:

- **submit wins** — the submit completes its Step 5 release-store before
  releasing the lock; close blocks on the transaction until then and returns
  after the LP; the accepted request continues to exactly one terminal; or
- **close wins** — close sets the arena admission flag; a submit that acquires
  the lock afterwards observes admission closed at reserve and rejects
  synchronously with `invalid_state` (Completion idle, zero residue).

There is deliberately no admission re-check inside `arena_.commit()`: a slot
already reserved/prepared when close lands is an IN-FLIGHT submission and
completes its protocol (the backend lock makes the close-vs-protocol
interleaving unreachable inside a backend; the arena gates NEW acceptance at
reserve only). The arena boundary is pinned by
`close_admission_gates_reserve_not_inflight_prepared_slot`
(`request_lifecycle_scheme_b_test`). Deterministic arbitration evidence:

- `tp_c2e_close_waits_for_inflight_acceptance_lp` — the submit is paused
  INSIDE the admission transaction between the slot commit (Step 4) and the
  Step 5 release-store; the closer thread observes the Completion at the close
  return (`close_saw_outstanding`): close must NOT have returned before the
  in-flight LP (mutant M11 detector).
- `tp_c2e_close_wins_submit_started_before_close_rejected` — the submit is
  paused BEFORE the admission lock; close completes with no contention; the
  resumed submit rejects at reserve with `invalid_state`, idle Completion,
  zero residue.
- `fake_c2e_close_waits_for_inflight_acceptance_lp` — the same close-waits
  arbitration on the Fake driver (its SubmitPauseGate pauses in the same
  Step 4→Step 5 window, inside the transaction; mutant M11-fake detector).
- `tp_c2e_submit_races_close_linearization` (concurrent): every attempt is
  accepted-then-terminal or synchronously rejected-idle; both legal reject
  codes (`invalid_state`, `would_block`) are asserted to leave the Completion
  idle / not outstanding / not ready, so a half-accepted regression cannot be
  masked by cleanup.

### 3.2 Close never changes request terminal semantics

Per-state (deterministic pause-gate windows):

| Window at close | Semantics | Evidence |
|---|---|---|
| submit path between commit and enqueue (`pending`) | resumed enqueue + dispatch run the real syscall; result verbatim | `tp_c2e_close_while_pending_preserves_accepted_request` |
| `enqueued` on the dispatch ring | the ring is not discarded; the worker dequeues and runs the syscall; result verbatim | `tp_c2e_close_while_enqueued_preserves_dispatch` |
| worker `running` the syscall | the real result wins verbatim; close is not a cancel and never rewrites success into canceled (size + void paths) | `tp_c2e_close_while_running_result_verbatim`, `tp_c2e_close_while_running_void_result_verbatim` |
| pending/enqueued + cancel after close | cancel still WINS the canceled terminal (Scheme B): no dispatch linkage, no syscall, `canceled_ops == 1` | `tp_c2e_close_then_pending_cancel_wins` |
| running + cancel after close | intent only (Decision 11 best-effort); the real result wins verbatim | `tp_c2e_close_then_running_cancel_intent_only` |
| parked wait_one | one-shot control wake: returns 0, no fabricated completion, no accounting change; a FUTURE wait_one parks normally again and is woken by real progress (no busy-spin) | `tp_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin` |
| close ‖ final `record_terminal` (close first) | the interrupted wait_one's final reap returns 0; the NEXT wait_one reaps the final ready — the control interrupt never swallows it | `tp_c2e_close_before_final_terminal_no_lost_ready` |
| close ‖ final `record_terminal` (terminal first) | the stored terminal is unaffected by close; wait_one reaps it verbatim | `tp_c2e_final_terminal_before_close_not_affected` |
| close races running workers (invariant-only) | every accepted request reaches exactly one verbatim terminal; accounting zero; exactly N syscalls | `tp_c2e_close_races_workers_invariant_drain` |

### 3.3 Wake / progress model for close

The split-phase ready wait (`detail::ReadyWaitSource`, issue #67) uses TWO
epochs: `progress_generation` (advanced by `signal_progress()` AFTER real
readiness is published — state first, then notify) and `control_generation`
(advanced by `interrupt_all()` as a ONE-SHOT re-evaluation signal).
`close_admission()` bumps the control epoch and notifies ALL parked waiters;
`wait_for_change()` reports `interrupted`; `AsyncIoContext::wait_one` (and the
backend's raw `wait_one`) performs ONE final non-blocking reap to close the
interrupt-vs-final-ready race and returns that reap's reaped-completion count
— `0` only when the final poll finds nothing (I8: no fabricated completion;
the M4/M12 detectors assert the final reap can return 1).
The CONTEXT-level final poll is pinned deterministically by
`ctx_wait_one_interrupt_final_poll_closes_ready_race` (mutant M12 detector: a
test-only split-wait backend pauses `wait_for_change()` after observing the
interrupt; the test records backend-ready in that window; deleting the
context's final poll makes wait_one return 0). Future waits snapshot the
advanced control generation and park normally — the control wake is one-shot
by construction, so an admission-closed runtime with outstanding work never
busy-spins.

Lost-wake closure per race:

- signal between snapshot and reap → seen by the reap;
- signal between reap and park → the predicate wait does not park (epoch
  advanced);
- interrupt between reap and park → the predicate wait returns `interrupted`;
- interrupt before the final `record_terminal` → the next wait reaps it
  (G1 ordering);
- terminal before close → unaffected (G2 ordering).

### 3.4 Destruction fail-fast matrix (row 16)

| State at destruction | Authority | Death evidence |
|---|---|---|
| `pending` (committed, pin live, not enqueued) | `threadpool_non_quiescent_destruction_fail_fast` (slot_in_use != 0) | `tp_death_destroy_with_pending` |
| `enqueued` (non-empty dispatch ring) | same | `tp_death_destroy_with_enqueued` |
| worker `running` a syscall (active worker) | same (active_workers_ != 0) | `tp_death_destroy_with_running` |
| `backend_ready` unreaped | same (backend_ready != 0) | `tp_death_destroy_with_backend_ready` |
| `completion_ready` but unreset | same (slot_in_use != 0) | `tp_death_destroy_with_completion_ready`, `fake_death_destroy_with_ready_unreset` |
| bound unreaped request (Fake reference path) | `request_arena_destruction_fail_fast` | `fake_death_destroy_with_unreaped_request` |
| quiescent (close + drain + reset) | clean teardown of idle workers / arena | `tp_death_control_quiescent_destroy`, `fake_death_control_quiescent_destroy` |

All fail-fast entries route through `std::terminate()` unconditionally
(`src/async/fail_fast.cpp`) — NOT `assert()` — so the fail-fast line is active
in BOTH Debug and Release; the death suites run in both configurations.

### 3.5 Re-audit of row 16's pre-existing FULL claim

Re-audited against master `0b6c0b9` before extending: the row-16 FULL claim
was substantively correct — `~ThreadPoolBackend` verifies quiescence
(dispatch empty, active_workers == 0, slot_in_use == 0, accepted_outstanding
== 0, backend_ready == 0) under `work_mtx_` BEFORE setting `stopping_` and
joining the idle workers; `active_workers_` is incremented under the same lock,
so no worker can start a syscall after the check; the arena destructor
fail-fasts on any bound slot; Completion reset/ready-destruction release is
generation-safe; all fail-fast entries are `std::terminate()` in both
configurations. The gaps C2e closed were evidence-only: the `pending` death
case (the matrix listed it but no case drove it) and Fake-type death evidence
(the arena-level record existed; no death test exercised the concrete
`FakeAsyncBackend` type).

## 4. Test-only seams (all `SLUICE_ASYNC_INTERNAL_TESTING`-guarded)

C2e reuses the existing guarded pause gates (`BeforeEnqueueLockPauseGate` for
`pending`, `BeforeWorkerDequeuePauseGate` for `enqueued`,
`WorkerRunningPauseGate` for `running`, `TerminalPublicationPauseGate` for the
pre-`record_terminal` window, the wait-phase flag for the parked-waiter
observation) plus the existing method-only introspection
(`handle_for_completion_for_test`, `observe_for_test`, `dispatch_size_for_test`,
`active_workers_for_test`, `backend_ready_count_for_test`, `arena_slot_in_use`,
`syscall_count_for_test`).

C2e adds THREE new `SLUICE_ASYNC_INTERNAL_TESTING`-only pause gates on
`ThreadPoolBackend`:

- `ControlWakeFinalReapPauseGate` — pauses `wait_one()` between the interrupted
  control wake and its single final reap (the interrupt-vs-final-ready window;
  `tp_c2e_interrupt_final_reap_closes_ready_race`; mutant M4 detector);
- `BeforeCommitBindingPauseGate` — pauses the submit path INSIDE the admission
  transaction, between `arena_.commit()` (Step 4) and the Step 5 release-store
  (`tp_c2e_close_waits_for_inflight_acceptance_lp`; mutant M11 detector);
- `BeforeAdmissionLockPauseGate` — pauses the submit path BEFORE taking
  `admission_mtx_` (`tp_c2e_close_wins_submit_started_before_close_rejected`).

All are compiled out of production `sluice_async` (0 symbols in
`libsluice_async.a`; verified by `nm` scan) and add guarded `std::atomic`
members only to the internal-testing build.

The B3 context-level detector
(`ctx_wait_one_interrupt_final_poll_closes_ready_race`) uses NO production seam
at all: a test-only split-wait backend + wait source (public `AsyncBackend` /
`BackendWaitSource` interfaces) pauses `wait_for_change()` after observing a
control interrupt and before returning `interrupted`; the test records
backend-ready in that window, and the context's interrupted-branch final poll
(the real L1 `AsyncIoContext::wait_one` code) is the ONLY path that can reap
it. Mutant M12 detector.

The shared suite's `CloseDrainFixture` wires two INSTANCE-LEVEL closures
(`close`, `slot_in_use`) over the concrete backends' PUBLIC
`close_admission()` / `arena_slot_in_use()` — no new production member, no
public API change beyond the reference method below.

**Production changes:**

1. `FakeAsyncBackend::close_admission()` — an additive public method mirroring
   `ThreadPoolBackend::close_admission()` (ADR Decision 15 reference semantics:
   `arena_.close_admission()` under the admission transaction lock). The
   reference backend previously had NO admission-close surface at all, so no
   shared close evidence could exist; the C2e shared suite requires both
   backends to expose the same lifecycle control. It is documented on the
   method and in `docs/reference/api.md`; the close/drain semantics are
   unchanged from the C2e-start design.

2. **Backend admission transaction domain (B1; ADR §"Commit / accept"
   :453-462)** — both `ThreadPoolBackend` and `FakeAsyncBackend` now hold
   `admission_mtx_` across the whole submit acceptance protocol (reserve ->
   prepare -> begin_binding -> arena commit -> install_binding ->
   commit_binding — the Step 5 acceptance LP), and `close_admission()` takes
   the same lock before `arena_.close_admission()`. This replaces the previous
   draft's attempt to gate `RequestArena::commit()` on `admission_closed_`:
   per the ADR the acceptance LP is the `binding -> outstanding` release-store
   (the arena's own `commit()` comment calls it "the submit-success
   linearization point's slot half"), so Decision-15 arbitration belongs in the
   backend transaction domain the ADR mandates ("the winning submit performs
   this protocol while retaining its own context/admission lock"). The lock is
   released before enqueue (no-fail, needs no admission serialization). Lock
   order: `admission_mtx_` -> arena leaf only; never nested with `work_mtx_`
   or the ready-wait mutex (no path takes `admission_mtx_` while holding
   `work_mtx_`). `RequestArena::commit()` carries no admission check: a
   reserved/prepared slot is an in-flight submission and completes its
   protocol; close gates NEW acceptance at reserve only.

3. **Descriptor-validation precedence (review P1; ADR Decision 5/6/15)** —
   `ThreadPoolBackend`'s malformed-descriptor probe now runs INSIDE the
   admission transaction, AFTER reserve and BEFORE prepare (Stage 1.5). It
   previously ran before any admission serialization, so a post-close
   malformed submit returned `invalid_argument` instead of `invalid_state`
   and a capacity-full malformed submit returned `invalid_argument` instead
   of `would_block` — the Reserve-stage admission decisions (closed ->
   `invalid_state`, Decision 15; full -> `would_block`, Decision 13) must
   precede the Prepare-stage descriptor validation (`invalid_argument`,
   Decision 6). A rejected descriptor rolls back the reserved slot through
   the same `rollback_reserved_or_prepared` the prepare-failure path uses
   (zero residue, generation++, capacity recyclable). The precedence is
   pinned by `tp_c2e_close_then_malformed_read_rejected_invalid_state`,
   `tp_c2e_close_then_malformed_sync_rejected_invalid_state`, and
   `tp_c2e_capacity_full_malformed_rejected_would_block` (all new).

## 5. Test case ledger

| Case (SLUICE_TEST_CASE) | Target | Status |
|---|---|---|
| `conformance_close_drain_fake` (4 shared cases) | backend_conformance_test | PASS |
| `conformance_close_drain_threadpool` (4 shared cases) | backend_conformance_test | PASS |
| `close_admission_gates_reserve_not_inflight_prepared_slot` | request_lifecycle_scheme_b_test | PASS |
| `tp_c2e_close_while_pending_preserves_accepted_request` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_while_enqueued_preserves_dispatch` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_while_running_result_verbatim` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_while_running_void_result_verbatim` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_void_submit_after_close_rejected` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_then_malformed_read_rejected_invalid_state` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_then_malformed_sync_rejected_invalid_state` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_capacity_full_malformed_rejected_would_block` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_waits_for_inflight_acceptance_lp` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_wins_submit_started_before_close_rejected` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_then_pending_cancel_wins` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_then_running_cancel_intent_only` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_before_final_terminal_no_lost_ready` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_final_terminal_before_close_not_affected` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_races_workers_invariant_drain` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_submit_races_close_linearization` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_interrupt_final_reap_closes_ready_race` | threadpool_backend_c2e_close_drain_test | PASS |
| `fake_c2e_close_waits_for_inflight_acceptance_lp` | fake_backend_c2e_close_drain_test | PASS |
| `ctx_wait_one_interrupt_final_poll_closes_ready_race` | async_io_context_split_wait_c2e_test | PASS |
| `tp_death_destroy_with_pending` | threadpool_backend_death_test | PASS |
| `fake_death_destroy_with_unreaped_request` | fake_backend_death_test | PASS |
| `fake_death_destroy_with_ready_unreset` | fake_backend_death_test | PASS |
| `fake_death_control_quiescent_destroy` | fake_backend_death_test | PASS |
| Python: `test_backend_conformance_manifest.py` (152 cases, incl. 16 new C2e) | unittest | PASS |

## 6. Fake / ThreadPool eligibility, Uring known gap

- **Fake** — ELIGIBLE: `c2e_shared_close_drain_suite` (shared) +
  `c2e_fake_close_drain_death` (lifecycle) PASS; row 15 now FULL (shared suite
  + reference death path).
- **ThreadPool** — ELIGIBLE: `c2e_shared_close_drain_suite` (shared) +
  `c2e_threadpool_close_drain_race` (lifecycle) + the extended
  `threadpool_death` matrix (lifecycle) PASS; row 15 now FULL.
- **Uring** — NOT CONFORMING (unchanged, Phase D pending): the
  `uring_c2e_close_drain_not_implemented` MANDATORY `not_implemented` record
  enters Uring's OWN verdict via `applicable_evidence_for_backend()` and
  appears in its reasons — the C2e gap is never skip-as-pass, and Uring is
  never marked conforming.

## 7. Validity evidence (mutations)

M1–M12 in
[`docs/verification/phase-c2e-close-drain-destruction-mutation-evidence.md`](../../verification/phase-c2e-close-drain-destruction-mutation-evidence.md):
11 of 13 backend-specific mutation executions (10 of 12 defect classes;
M11/M11-fake are one defect class exercised on two backends) made the targeted
detector case(s) fail RED (M1–M5, M8, M9, M10, M11, M11-fake, M12 — M8 via the
death child's hang instead of fail-fast, bounded by the death runner's 60 s
watchdog); every mutation was restored and the case(s) re-ran GREEN. M6 (`slot_in_use` check)
and M7 (`backend_ready` check) are documented BEHAVIOR-NEUTRAL mutants:
defense-in-depth redundancy covered by other authorities — `~RequestArena`
fail-fasts on any `slot_in_use != 0` (M6), and `backend_ready != 0` implies
`accepted_outstanding != 0` since reap is the sole accepted-outstanding
decrementer (M7) — so the death matrix still fail-fasts under both mutants
(recorded as negative results with the covering-authority proof, not
fabricated REDs). M11/M11-fake (close drops the admission transaction; the
closer observes the in-flight submit's Completion still `binding` at the close
return) and M12 (`AsyncIoContext::wait_one` drops the interrupted-branch final
poll; the probe backend's ready stays unreaped and wait_one returns 0) are the
new B1/B3 detectors. Final scan confirmed 0 mutation markers remain.

## 8. Commands run (validation) — see section 9 for the full matrix

| Gate | Command | Result |
|---|---|---|
| Focused ThreadPool | `xmake build threadpool_backend_c2e_close_drain_test && xmake run threadpool_backend_c2e_close_drain_test` | PASS (18 cases) |
| Focused admission-LP (arena boundary) | `SLUICE_TEST_FILTER=close_admission_gates_reserve_not_inflight_prepared_slot xmake run request_lifecycle_scheme_b_test` | PASS |
| Focused admission-LP (ThreadPool transaction) | `xmake run threadpool_backend_c2e_close_drain_test` (cases `tp_c2e_close_waits_for_inflight_acceptance_lp`, `tp_c2e_close_wins_submit_started_before_close_rejected`) | PASS |
| Focused admission-LP (Fake transaction) | `xmake run fake_backend_c2e_close_drain_test` | PASS (1 case) |
| Focused context final poll | `xmake run async_io_context_split_wait_c2e_test` | PASS (1 case) |
| Focused shared | `SLUICE_TEST_FILTER=conformance_close_drain_fake xmake run backend_conformance_test` and `..._threadpool` | PASS (4 cases each) |
| Death | `xmake run fake_backend_death_test`, `xmake run threadpool_backend_death_test` | PASS (3 + 6 cases) |
| Stability | 5× repeated runs of the C2e race cases (`close_races_workers`, `submit_races_close`, `wakes_parked_waiter`) | PASS (5/5, no flake) |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (152 cases, incl. 16 new C2e) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | PASS (Fake/TP ELIGIBLE with C2e records PASS; Uring NOT CONFORMING with the C2e gap in its reasons) |
| RED validity | 13 mutation executions across 12 defect classes (M1–M12 incl. M11-fake), focused filtered runs | 11 RED executions (M1–M5, M8, M9, M10, M11, M11-fake, M12) = 10 RED defect classes (M11/M11-fake count as one); M6/M7 behavior-neutral (defense-in-depth redundancy, see §7 + mutation ledger); all mutations restored and re-run GREEN |

## 9. Validation matrix (full evidence)

| Configuration | Command | Result |
|---|---|---|
| Clang Debug full | `xmake f -m debug --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test && xmake test -v` | PASS (151/151 tests passed) |
| Clang Release full | `xmake f -m release --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test && xmake test -v` | PASS |
| ASan + UBSan | `xmake f -m asanubsan --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test && xmake run -g test` | PASS |
| TSan | `xmake f -m tsan --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test && xmake run -g test` | PASS (race classes: submit‖close incl. the in-flight acceptance-LP window, close‖enqueue/worker/terminal, close/control-wake‖wait-park, interrupt‖final-ready, context final-poll‖ready-record, reap‖reset) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | PASS |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (152) |
| Negative compile | `verify-completion-authority-negative-compile.sh`, `verify-request-arena-negative-compile.sh` | PASS (no public authority change) |
| Doc checks | `python3 scripts/check-doc-links.py`, `python3 scripts/verify-architecture-docs.py` | PASS |
| Diff hygiene | `git diff --check` | PASS |

### 9.1 Completion report (commits / working tree)

Final head for this gate: `f33dcfc` — 14 commits on top of the baseline
`0b6c0b9` (PR #73):

```text
f33dcfc docs(async): C2e evidence reconcile — case inventory, final-reap wording, completion report
65f8a2b test(async): C2e hygiene — gate lifetime ordering, probe-loop pacing, split-wait lock-hold comment
9ae7e84 fix(async): FakeAsyncBackend::close_admission() not noexcept (matches ThreadPool) + wait_one final-reap comment
2843058 docs(async): C2e mutation-evidence accounting (13 executions/11 RED/2 neutral) + P1 precedence record (review P1/P2)
e510169 test(async): C2e post-close malformed-descriptor precedence detectors (review P1)
c10d7d2 fix(async): ThreadPool descriptor validation inside the admission transaction (review P1)
153638f docs(async): C2e gate/ledger/mutation-evidence + residual-risk records (issue #74)
41e2840 test(async): C2e admission-transaction + context final-poll detectors (B1/B3) + B2/B4 hygiene
a612a20 fix(async): backend admission transaction domain — close serializes against the Step-5 acceptance LP (B1)
8de63bb docs(async): C2e compliance gate + mutation evidence + roadmap/registry updates
6c7a46e test(gate): C2e manifest records, close/drain driver, and self-tests
20bdefc test(gate): complete C2e destruction matrix — pending state + Fake death target
eeab3f5 test(async): C2e ThreadPool deterministic close/drain window evidence
b6a3bc8 test(async): add C2e shared close/drain suite + FakeAsyncBackend::close_admission
```

Working tree at the final gate run: clean — the C2e changes are exactly the
11 commits above and no unrelated tracked/untracked/ignored files were
modified (`git status --short` empty after the commit slice; `git diff
--check` clean).

## 10. Remaining gaps

- **Uring close/drain/destruction** — Phase D scope (RequestArena migration);
  recorded as `uring_c2e_close_drain_not_implemented` and surfaced in Uring's
  verdict.
- **ApplicationRuntime / Scheduler shutdown composition** — Phase F/G scope;
  the backend-level close/drain contract proven here is the authority the
  runtime-level shutdown composes on.
- **Abort/cancel-on-shutdown mode** — deliberately NOT defined by ADR Decision
  15 ("there is no implicit mass-cancellation mode in this ADR"); requires a
  separate approved design if ever wanted.
- **Recorded residual risks (issue
  [#74](https://github.com/jnhu76/Sluice/issues/74))** — two documented
  evidence caveats, deliberately not hidden (AGENTS.md §23):
  1. The C1b/Fake negative probes
     (`tp_c2e_close_waits_for_inflight_acceptance_lp` /
     `fake_c2e_close_waits_for_inflight_acceptance_lp`) observe "the closer's
     read must not complete while the submit is paused before its acceptance
     LP" through a bounded 2 s window (`kCloseProbeTimeout`; failure-protection
     only — the mutation's close returns in microseconds, >=1000x inside the
     window). The ordering proof is structural (the admission lock + the pause
     gate: the submitter cannot advance until the test resumes it, and under
     the fix the closer is blocked on the held lock) — the window is NOT a
     timing claim (§13.3). Follow-up trigger: if a future change alters the
     admission protocol, the pause-gate shape, or the lock scope, the probe's
     structural premise must be re-audited.
  2. The mutation-harness coverage scope: mutants M1–M10/M12 were verified on
     the final test code in the first full harness run, and M11/M11-fake were
     re-verified 4/4 on the final code after the detector fix — the remaining
     detector cases were byte-identical since the first run. Follow-up
     trigger: any future edit to a C2e detector case requires re-running the
     full harness (M1–M12) before claiming RED validity.

## 11. Phase status

- Row 15 (close/drain/reset sequence): **FULL** (was PARTIAL).
- Row 16 (quiescent destruction): **FULL** (re-audited; evidence strengthened
  with the `pending` death case and the Fake-type death target).
- C2e: **COMPLETE**. Phase C remains PARTIAL overall until the remaining
  out-of-scope slices (Phase D Uring migration, Phase F public request
  lifecycle, Phase G wake bridge) land; the C2 semantic-coverage slices
  C2a–C2e are all complete.
