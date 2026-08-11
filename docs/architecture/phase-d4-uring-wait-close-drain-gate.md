# Phase D4 — Uring Wait / Close / Drain / Destruction Gate

Status: COMPLETE (2026-08-11, branch feat/phase-d4-uring-wait-close-drain;
D3 is MERGED into master, so this branch's base is current master
`259f0bd2dc5d027fd463132b65db1bef9c33f08f`, the D3 merge commit — no D3-branch
stacking remains). Round-4 (PR #84 review) closed the two remaining P0
control-wake gaps (commit-to-park handshake, durable broadcast gate), the P1
stub-gate false-green, the poison lock-order violation, the death-mode guard
drift, the two-waiter test self-deadlock, and the P2 count/cleanup drift.

Governing authority chain (AGENTS.md §3):

```text
Accepted ADR (ADR-explicit-io-request-contract.md, Decisions 4, 5, 8, 9, 11, 12, 15, 18, 19)
>
Phase D frozen design (phase-d1-uring-frozen-design.md §11 wait/close/drain,
phase-d-uring-migration-plan.md §11, §13 mutation priorities)
>
shared C2e contract gate (phase-c2e-compliance-gate.md rows 15-16)
>
production implementation (include/sluice/async/uring_backend.hpp,
include/sluice/async/detail/uring_wait_source.hpp, src/async/uring_backend.cpp)
>
tests (uring_backend_c2e_close_drain_test, uring_backend_c2e_death_test,
backend_conformance_driver_test.cpp conformance_close_drain_uring)
>
docs / PR prose
```

This gate is the D4 slice: `BackendWaitSource` integration (ring-fd poll +
control eventfd), `close_admission()` with accept-LP serialization, drained vs
releasable destruction semantics, the shared C2e suite for Uring, the death
matrix, manifest closure (`uring_c2e_close_drain` + Uring in the shared
close/drain suite), and — only after all mandatory real evidence is complete —
the removal of the KernelIo fail-closed hard-code (the final lift mutation
D4-L1, phase-d-uring-migration-plan.md §14 D4).

---

## Gate 0 — Architecture Classification

```text
Affected capability:    Backend (Uring), AsyncIoContext wait path, admission close
Affected layer:         L0 backend, L1 context (observe-only wait source)
Classification:         Faithful (frozen design §11; ADR Decision 15 reference semantics)
Governing ADR:          ADR-explicit-io-request-contract.md Decisions 4 (re-arm),
                        5 (stage order), 11 (best-effort cancel), 12 (terminal winner),
                        15 (close admission), 18/19 (Phase D scope)
Conformance map change: no (no new public SEMANTIC; close_admission() was already
                        in the public surface for Fake/ThreadPool; Uring now
                        implements the same ADR Decision 15 seam. Round-4
                        (D4-RM14/P0-1) adds two INTERNAL-use surface members:
                        AsyncIoContext::arm_backend_wait_commit() (the
                        commit-to-park registration, mirror of
                        interrupt_backend_waiters) and the defaulted virtuals
                        BackendWaitSource::arm_committed_wait() /
                        consume_committed_wait() (source-compatible; only
                        ReadyWaitSource / UringWaitSource override them))
Constitution rules:     AC-1 (explicit ownership), AC-2 (identity), AC-3 (bounded
                        resources), AC-4 (no post-accept allocation dependency),
                        AC-6 (wake/progress), AC-7 (implementation-resource tests),
                        AC-8 (shutdown), AC-9 (fail-fast), AC-10 (no destructor
                        drain/cancel/wait)
```

## Gate 1 — Ownership and State Machine

### Close / admission lifecycle (UringAsyncBackend)

```text
States:
  open → closed(admission) → (accepted work continues) → drained → destroyed

Transitions:
  open → closed
    Authority:      caller via public close_admission() (ADR Decision 15)
    Lock domain:    dispatch_mtx_ (serializes against the whole Stage 1-5
                    admission transaction; the `binding -> outstanding`
                    release-store is the accept LP — a winning submit finishes
                    its LP before close returns; a losing submit observes
                    closed at Stage 0 inside the lock and rejects
                    synchronously with invalid_state)
    Allocation:     none
    Failure:        n/a (no-fail; have_ring_ false -> no-op)
    Wake:           interrupt_all() — one-shot control generation advance +
                    eventfd write AFTER the epoch is published (lost-wake
                    three-window theorem); parked wait_one participants
                    re-evaluate and return 0 (nothing fabricated)
    Shutdown:       admission close is NOT destruction; cancel/poll/wait_one/
                    reap remain legal; accepted work reaches its real terminal

  closed → drained
    Authority:      reap path only (arena_.reap — sole Completion-ready
                    publication authority)
    Lock domain:    AsyncIoContext::access_mtx_ (the context's serialized
                    poll/reap domain — reap_cqes()/arena_.reap run WITHOUT
                    dispatch_mtx_) + arena leaf
    Allocation:     none
    Failure:        terminal winner / record_terminal invariants fail-fast
    Wake:           signal_ready_progress() after a non-empty reap
    Shutdown:       drain is caller-driven (poll/wait_one loop); a
                    completion-ready-but-unreset Completion keeps the slot
                    bound (drained != releasable)

  drained → destroyed
    Authority:      caller (Completion::reset() releases each slot), then
                    destructor preflight
    Lock domain:    destructor: dispatch_mtx_ -> arena leaf (frozen order)
    Allocation:     none
    Failure:        non-quiescent destroy fail-fasts (exit 86) BEFORE
                    io_uring_queue_exit — no implicit drain/cancel/wait
    Wake:           none
    Shutdown:       quiescent teardown only (AGENTS.md §14)
```

### Wait-source lifecycle (UringWaitSource, observe-only)

```text
States:
  created → ring_fd_installed → (snapshot/poll/park loop) → destroyed

  snapshot        authority: context (AsyncIoContext::wait_one); lock: mtx_
                  (leaf); reads {progress_epoch, control_epoch}
  park            authority: wait_for_change(token); locks mtx_ for epoch
                  check + durable-broadcast gate + eventfd drain + park
                  registration, then poll(2) WITHOUT any lock; wakes: ring
                  fd POLLIN (kernel CQE), control eventfd (interrupt_all/
                  signal_progress writes), EINTR (re-loop), POLLNVAL
                  (fail-fast — parked waiter with a torn-down fd is a caller
                  contract violation)
  report          progress (ring readable) or interrupted (control epoch
                  advanced); the CONTEXT owns the final poll that closes the
                  interrupt-vs-final-ready race
  commit-to-park  arm_committed_wait() / consume_committed_wait() — one-shot
                  mandatory control baseline for the NEXT wait_one()
                  invocation (D4-RM14/P0-1); a control wake published after
                  the arm is observed by that invocation even if it lands
                  before wait_one() captured its own snapshot
  durable gate    parked_count_ (registered parkers) / pending_wake_count_
                  (parked-at-publish waiters not yet acknowledged) — a
                  future waiter's drain is gated on zero pending
                  acknowledgements (D4-RM15/P0-2)
```

The wait source is observe-only (frozen design §11 / issue #67): it NEVER
reaps, records terminals, publishes Completions, mutates RequestArena state,
cancels operations, or changes outstanding. `AsyncIoContext` continues to own
serialized poll/reap under `access_mtx_`.

## Gate 2 — Resource and Failure Model

```text
Construction-time resources:
  - control eventfd: one fd, created in UringWaitSource ctor; failure ->
    std::runtime_error THROWN from backend construction (the wait source is
    created inside the ring-init try block; the catch tears down the ring and
    rethrows). There is NO silent capability downgrade and NO "wait_source()
    returns nullptr" fallback for eventfd failure — backend construction
    fails truthfully. wait_source() returns nullptr only when there is no
    ring at all (stub / ring-init failure), where the legacy serialized
    wait_one contract applies.
  - wait source object: one unique_ptr member, created after
    io_uring_queue_init succeeds; ring fd installed once before any park

Submit-time resources:
  - unchanged from D1/D2 (bounded RequestArena, bounded dispatch ring, bounded
    router/ledger); close adds no allocation
  - post-accept allocation: NONE (I9) — close cannot make an accepted request
    lost; admission_closed_ is a plain bool read/written ONLY under
    dispatch_mtx_ (P0-B: no unlocked fast-path read; the in-lock Stage-0
    check is the single authority)

Completion-time resources:
  - reap publishes through the slot-bound publication capability; no
    allocation; a completed result can never be lost to allocation failure

Capacity and backpressure:
  - maximum outstanding: request_capacity (bounded)
  - queue-full behavior: would_block (unchanged)
  - close + full arena: reserve rejects invalid_state (admission beats
    capacity, Stage 0 precedes Stage 1)

Reclamation:
  - dispatch ring / router / ledger bounded; no growth by historical
    submissions (P0-A/P0-B/P0-D)
  - eventfd counter: drained only when the durable-broadcast gate is open
    (every parked-at-publish waiter acknowledged — D4-RM15/P0-2); epoch not
    persistent state — no shutdown busy-spin
```

## Gate 3 — Progress and Wake Model

```text
Blocking/suspension:
  - who may block: a wait_one() caller (context participant)
  - what makes them continue: (a) ring fd POLLIN — kernel CQE delivery;
    (b) control eventfd — progress epoch bump (signal_ready_progress after a
    non-empty reap / terminal_noop re-arm) or control epoch bump
    (close_admission / interrupt_backend_waiters)

Progress mechanism: signal-based (eventfd writes) PLUS observation-based
(kernel ring fd poll). The ring fd is poll(2)-able; POLLIN holds exactly while
CQEs are pending (empirically verified on the D4 proof kernel 6.18; empty ring
-> poll returns 0; POLLIN delivered exactly with the CQE; after reap the ring
is not readable again).

Lost-wake closure (three-window theorem, AGENTS.md §13.2):
  - signal before poll            -> epoch check sees it; poll/reap observe
                                     ring readability directly
  - signal between poll and park  -> eventfd write lands after the pre-park
                                     drain; poll(2) returns immediately; epoch
                                     check sees the bump
  - signal after park             -> eventfd write wakes the parked poll(2)
  - the pre-park drain (EAGAIN-tolerant) empties the counter so a consumed
    wake can never busy-spin a future park; both signal_progress() and
    interrupt_all() publish the epoch under mtx_ BEFORE writing (publish-
    epoch-then-write order)

Multi-waiter / durable broadcast (D4-RM15, P0-2): an eventfd write DOES wake
every poller parked at that moment (Linux wakes the poll waitqueue), but the
counter is a single CONSUMABLE token, not a notify_all — after the wake,
do_poll() re-runs each fd's readiness check, and a poller whose recheck finds
an empty counter can go back to sleep. A FUTURE-generation waiter draining
the counter can therefore steal the wake of an OLD-generation waiter that was
woken but has not finished its recheck. The wait source closes this with a
generation-scoped register/acknowledge gate: every waiter registers
(parked_count_++) atomically with its pre-park drain; every publish sets
pending_wake_count_ = parked_count_; a parked waiter that observes the epoch
delta after its poll acknowledges exactly once, releasing the gate; a
future-generation waiter's drain is GATED on pending_wake_count_ == 0 (a
persistent predicate + CV notify — no lost wake, no busy-spin) and it
re-checks the epochs after the gate so a wake that belongs to ITS invocation
is reported (D4-RM13). The token therefore stays in the level-triggered
counter until every waiter it was published for has rechecked, after which
the next park drains it. Re-arm: a pinned/backend-ready request that is
temporarily reap-ineligible re-arms readiness so no wake is lost.

Commit-to-park handshake (D4-RM14, P0-1): the Scheduler's MW-S2 participant
registers its mandatory control-observation baseline with the backend wait
source (`arm_committed_wait`) under global_mtx_ at the Phase-B commit —
BEFORE the backend-park commitment is exposed and the admission authority is
released. `AsyncIoContext::wait_one()` consumes that baseline
(`consume_committed_wait`) at invocation start, so a runtime stop
(request_stop -> interrupt_backend_waiters) landing between the commit and
the wait_one() entry is observed by that invocation instead of being
rebaselined as a past event (D4-RM13 invocation-begin semantics). Without
the registration the participant parks in the BACKEND domain (which the
Scheduler wake domain cannot interrupt) and, with backend I/O that never
completes, the run can never reach its stop-predicate boundary —
drain_complete_ unreachable. The registration is one-shot: a FUTURE
wait_one() captures a fresh baseline, so the interrupt stays one-shot.

Polling dependency: NONE — poll(2) parks indefinitely (-1 timeout); a periodic
timeout is never the progress authority. Deadlines exist only as hang
watchdogs in tests.

Worst-case latency: kernel CQE -> ring POLLIN -> poll(2) return -> context
re-poll: direct, no periodic interval.
```

## Gate 4 — Evidence Plan

Deterministic causal tests (`uring_backend_c2e_close_drain_test`, real mode):

```text
- uring_c2e_close_waits_for_inflight_acceptance_lp:
    submit-vs-close LP (honest evidence split). A runtime "closer blocked on
    the admission mutex" observation CANNOT be made without scheduler timing,
    so this case makes NO deterministic mutex-blocking claim and uses NO
    sleep/time window as an ordering proof. What it DOES prove (structurally):
    a submit paused inside its acceptance transaction (BeforeCommitBinding
    PauseGate) reaches a genuine in-flight LP state; after a genuine concurrent
    close the LP-winning request is outstanding and is still driven to exactly
    one terminal (no lost acceptance); post-close no new acceptance LP can
    occur (Stage-0 reject, idle Completion, zero residue). The DETERMINISTIC
    authority that submit Stage 0..commit_binding and close_admission's
    admission-close write share the same dispatch_mtx_ lives in a source-drift
    self-test (D4DriftDetectorTest.test_close_admission_uses_dispatch_mtx);
    focused TSan on submit||close and the concurrent linearization case
    (uring_c2e_submit_races_close_linearization) complete the evidence.
- uring_c2e_close_wins_submit_started_before_close_rejected:
    close-wins — a submit paused before the admission lock observes closed at
    Stage 0 and rejects synchronously (idle Completion, zero residue)
- uring_c2e_close_while_pending_preserves_accepted /
  uring_c2e_close_while_enqueued_preserves_dispatch /
  uring_c2e_close_while_running_result_verbatim /
  uring_c2e_close_while_backend_ready:
    close at every execution state — no retroactive reject/cancel/discard;
    real terminal verbatim; reap legal after close
- uring_c2e_void_submit_after_close_rejected /
  uring_c2e_malformed_submit_after_close_rejected:
    post-close precedence — invalid_state; malformed descriptor loses to
    admission-closed
- uring_c2e_close_then_pending_cancel_wins /
  uring_c2e_close_then_running_cancel_intent_only:
    cancel remains legal after close (Decision 15): Scheme-B pending cancel
    wins with no SQE; running cancel is intent only, original CQE verbatim
- uring_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin:
    close wakes a parked wait_one as a ONE-SHOT control wake (0, nothing
    fabricated); a future wait parks again (no busy-spin) and wakes on real
    progress
- uring_c2e_multiple_parked_waiters_all_wake:
    one interrupt_all() wakes ALL N=3 parked participants; nothing fabricated.
    Deterministic proof: a guarded per-participant pre-poll barrier
    (BeforePhysicalPollPauseGate) records ONE arrival per distinct participant
    at the physical-poll boundary (the same waiter cannot double-count by
    retrying on EINTR — the barrier blocks it at the boundary until release,
    so arrivals == N proves N distinct parked participants); the plain
    prepark counter remains as an additional reach observation but is no
    longer the uniqueness authority. The test waits for arrivals == N with a
    bounded deadline ONLY as a hang watchdog — no sleep is the ordering proof
    (AGENTS.md §13.3)
- uring_c2e_interrupt_final_reap_closes_ready_race:
    a terminal recorded in the interrupt-vs-final-poll window is reaped by the
    final poll — wait_one returns the actual count, never 0
- uring_c2e_two_waiter_consumer_strand:
    post-poll control reclassification (D4-RM10): a control wake co-ready
    with ring POLLIN MUST report interrupted (the caller's final poll reaps
    the co-ready CQE); the controller consumes A with a BOUNDED NONBLOCKING
    poll() loop — never a second wait_one() (round-4/P1-1: a wait_one() in
    the controller would enter the SAME pre-poll barrier it owns the release
    of, deadlocking the test on itself; the historical "WSL2 flake"
    attribution was wrong)
- uring_c2e_future_waiter_cannot_steal_old_wake (round-4, D4-RM15):
    a FUTURE-generation waiter cannot consume the eventfd token that wakes an
    OLD-generation waiter (durable broadcast). Old waiter parked at the
    pre-poll barrier; interrupt; future waiter must NOT drain the token — its
    drain is gated on the old waiter's acknowledgement; the old waiter returns
    interrupted (0) and the future waiter wakes only on REAL progress (1)
- uring_c2e_drained_not_releasable:
    accepted_outstanding == 0 with a ready-but-unreset Completion leaves
    slot_in_use == 1; caller reset releases the slot; release does not
    re-open admission
- uring_c2e_poison_close_keeps_class_c:
    permanent transport poison + close — poison error precedence, quarantined
    Class-A ledger entry retained as teardown evidence
```

Death matrix (`uring_backend_c2e_death_test`): 7 non-quiescent destroy states
(pending / enqueued / running / ledger residue / backend-ready /
completion-ready / live control) fail-fast exit 86; the quiescent control
(close -> drain -> reset -> destroy) exits 0. P0-C: the death target is a
MANDATORY real-mode evidence record (`uring_c2e_quiescent_destruction`) with
an exact pinned case-set registered in BOTH builds (the stub build compiles
the same case names as empty bodies + an evidence-mode case emitting
mode=stub) — a missing/failing death target, lost Release fail-fast, or
broken quiescent control now fails the real KernelIo aggregate mechanically.

Shared C2e suite: `conformance_close_drain_uring` drives the UNCHANGED shared
cases through the public `close_admission()` / `arena_slot_in_use()` seams —
`close_rejects_future_submit`, `close_preserves_accepted_terminal`,
`drain_then_reset_releases_slot`, `slot_released_but_admission_stays_closed`.

Mutations: D4-M1..D4-M13 RED→GREEN (see
`docs/verification/phase-d4-uring-wait-close-drain-mutation-evidence.md`) plus
the lift mutation D4-L1 (KernelIo hard-code removal) and the PR #84 repair
mutations D4-RM1..D4-RM9 (P0-A aggregate fail-closed, P0-B unlocked
admission_closed_ read under TSan, P0-C destruction evidence INCOMPLETE,
P1-B pinned backend-contract case deletion, P1-A C2e evidence-mode compile-out,
P1-D unconditional wait-source include, P1-C multi-waiter under-count /
single-wake and close-lock-removal detectors, P0-C death target
fail/disappear), plus round-2 repair mutations D4-RM10 (post-poll ring-ready
bypasses control-epoch reclassification), D4-RM11 (destructor preflight
bypassed before io_uring_queue_exit), D4-RM12 (non-EINTR poll failure treated
as retryable), the pre-poll participant-uniqueness barrier mutant, and the
close-LP shared-lock mutant revisited (now caught by a source-drift self-test,
not a timing-based mutex-blocking claim), plus the round-3 repair mutation
D4-RM13 (inter-iteration control wake rebaselined per internal progress
iteration — the P0 control-wake-theorem gap; authority moved to the context:
control baseline is per external wait_one invocation), plus the round-4
repair mutations D4-RM14 (commit-to-park handshake removed — the P0-1
runtime-shutdown race; detector
`stop_between_mw_s2_commit_and_backend_wait_registration`), D4-RM15
(durable-broadcast gate removed — the P0-2 future-waiter eventfd steal;
detector `uring_c2e_future_waiter_cannot_steal_old_wake`, the 21st pinned C2e
case), D4-RM16 (poison wake inside dispatch_mtx_ — the P1-3 lock-order
violation; the wake is deferred past the dispatch lock, state first then
wake), and the round-5 repair mutation D4-RM17 (the cancel-path
newly-poisoned early return in `issue_running_cancel()` skipped the deferred
wake entirely — state published, wake obligation missing, AC-6/AGENTS §13.2;
detector `uring_c2e_running_cancel_poison_deferred_wake`, the 22nd pinned C2e
case; the poisoned flush order is scripted so the wake under test provably
comes from the cancel path).

## Lock / atomic authority table

| Domain | Guards | Held while | MUST NOT |
| ------ | ------ | ---------- | -------- |
| `dispatch_mtx_` (backend) | admission transaction Stage 1-5, close_admission, poll re-dispatch, cancel remove_exact, destructor preflight | submit LP; close; front peek/transfer | call the arena leaf OUTSIDE the frozen order `dispatch_mtx_ -> arena leaf` (production calls arena_.reserve/prepare/commit/record_terminal/quiescence_snapshot in exactly that order); wait for worker progress; join threads; ACQUIRE the wait-source mutex (D4-RM16/P1-3: signal_ready_progress is called only after dispatch_mtx_ is released — state first, then wake) |
| wait-source `mtx_` (LEAF) | progress/control epochs, eventfd drain, committed-wait registration | snapshot; epoch check + drain; arm_committed_wait | call Scheduler; publish Completions; call user code; touch request state; ACQUIRE the Scheduler `global_mtx_` (the one edge is the reverse: `global_mtx_ -> wait-source mtx_` for `arm_committed_wait()`, D4-RM14); block on asynchronous progress; call Scheduler/user/sink/request-lifecycle code |
| arena leaf mutex | slot lifecycle, terminal winner, ready-ring | record_terminal / reap / release | call Scheduler; call ReadySink (sink runs outside); publish outside the leaf (the ready release-store IS the linearization point) |
| `access_mtx_` (context) | serialized poll/reap + stats accounting | poll; accounting | the PARK (park is outside the lock — I1) |
| Scheduler `global_mtx_` | scheduler state, MW-S2 Phase-B admission authority | MW-S2 commit (incl. `ctx_.arm_backend_wait_commit()`) | hold while parking a backend wait |

Order: `access_mtx_ -> dispatch_mtx_ -> arena leaf -> wait-source mtx_` (leaf).
The ONE exception to "wait-source `mtx_` is a leaf acquired only with no other
lock held" is the D4-RM14 commit-to-park registration edge:

```text
Scheduler global_mtx_ -> wait-source mtx_   (arm_committed_wait() only)
```

`arm_committed_wait()` is a BOUNDED registration: it reads/writes only the
armed epoch/state under the wait-source mutex, never waits on a condition
variable, never calls the Scheduler, user code, a sink, or the request
lifecycle, and never takes `access_mtx_` / `dispatch_mtx_` — so the edge is
acyclic (the wait-source leaf MUST NOT acquire Scheduler `global_mtx_` or any
other lock, and nothing acquires wait-source `mtx_` then `global_mtx_`).
Do NOT move `arm_backend_wait_commit()` out of the Scheduler's `global_mtx_`
scope: that would reopen the D4-RM14 commit-to-park window (a stop published
between the commit and the wait registration would be rebaselined away).

All other wait-source entry points (`signal_ready_progress()` /
`interrupt_all()`) are called with NO other lock held: the backend calls
`signal_ready_progress()` only OUTSIDE `dispatch_mtx_` — D4-RM16 (P1-3)
removed the poison path's in-lock signal (`poison_and_recover_locked` no
longer wakes; `enqueue_after_commit()` and `issue_running_cancel()` defer the
wake past their own dispatch_mtx_ scope — state first, then wake; the
D4-RM17 round-5 repair additionally closes the cancel-path early return that
skipped that deferred wake), and the reap paths signal via their n>0 path
already outside the lock. Reap (reap_cqes + arena_.reap) runs under
`access_mtx_` WITHOUT `dispatch_mtx_` (the D1 single-driver call domain);
`dispatch_mtx_` is released before reap in poll()/wait_one(). No bidirectional
cycles.

## Wake / progress model (AGENTS.md §13.2)

```text
Persistent state:  progress_epoch_ / control_epoch_ (published under mtx_)
Signal producer:   signal_ready_progress() (reap n>0, terminal_noop re-arm,
                   deferred poison wake — all OUTSIDE dispatch_mtx_, D4-RM16);
                   interrupt_all() (close_admission, ctx.interrupt_backend_waiters)
Sleeping consumer: wait_for_change() park (poll on ring fd + control fd)
Predicate:         epoch mismatch OR ring fd POLLIN
Commit-to-sleep:   epoch check + durable-broadcast gate (pending_wake_count_
                   == 0) + eventfd drain + park registration under mtx_, then
                   poll (any write after the drain lands in the level-triggered
                   counter and wakes the park — the three-window theorem; the
                   gate keeps the token until every parked-at-publish waiter
                   rechecked — D4-RM15/P0-2)
Worst case:        kernel CQE -> POLLIN (direct); control write -> poll return
Shutdown:          interrupt_all is one-shot; no persistent "never park" state

Commit-to-park (D4-RM14/P0-1): the Scheduler MW-S2 participant arms its
control baseline under global_mtx_ BEFORE the backend-park commitment is
exposed; wait_one() consumes it at invocation start, so a stop published
between the commit and the wait entry is observed (register -> publish ->
release -> wait with the registered baseline).

Control-wake scope (D4-RM13): the CONTROL baseline is captured ONCE at the
start of an external wait_one() invocation and held fixed for its whole
duration; the PROGRESS baseline may refresh per internal loop. A control-plane
wake landing in the inter-iteration window (between wait_for_change() returning
`progress` and the next internal snapshot) is observed as interrupted by THIS
invocation — it is NOT absorbed into the fresh snapshot. A FUTURE wait_one()
captures a fresh control baseline, so the interrupt stays one-shot. The
control-wake theorem's scope is ONE EXTERNAL wait_one invocation, not one
internal wait_for_change loop.
```

## Shutdown / destruction semantics (AGENTS.md §14)

```text
close admission -> continue progress -> reap accepted requests -> callers reset
ready Completions -> accepted_outstanding == 0 -> slot_in_use == 0 -> destroy
```

The destructor preflight fail-fasts BEFORE `io_uring_queue_exit` on any
non-quiescent state (dispatch non-empty, live cookies/control SQEs, ledger
residue without recovery retirement, slot_in_use / accepted_outstanding /
backend_ready != 0). It does NOT cancel, drain, wait, reap, or publish. A
parked waiter implies outstanding > 0, so quiescent destruction can never
tear down an fd under a parked poll (POLLNVAL fail-fast remains a caller
contract violation detector).

## Zig conformance

`zig/` remains a source-derived design reference only (AGENTS.md §1). The D4
wait source / close seam have no Zig analogue to classify; no new divergence
is introduced. The existing divergence registry entries are unchanged.

## Evidence (actual commands)

```text
real-mode aggregate gate : PASS — Uring shared=PASS (shared_suite,
                            shared_capacity_suite, c2e_shared_close_drain_suite
                            all mode=real), lifecycle=PASS (c2b/c2c/c2d/c2e
                            integration records), backend_specific=PASS
                            (uring_backend_contract mode=real),
                            overall ELIGIBLE (RESULT: PASS)
stub aggregate gate       : PASS — Uring shared=INCOMPLETE (all three suites
                            downgraded by the KernelIo real-mode attribution),
                            lifecycle=INCOMPLETE, backend_specific=INCOMPLETE,
                            overall INCOMPLETE (spec §41 — stub never
                            satisfies real obligations)
manifest self-tests       : PASS 380/380 (python3 -m unittest discover -v
                            scripts/tests — incl. KernelIo lift semantics:
                            stub->INCOMPLETE, complete real set->ELIGIBLE;
                            close-drain real-mode downgrade; D4 drift and
                            evidence-mode drive tests; round-2 adds the
                            close_admission dispatch_mtx_ source-drift
                            detector; round-4 adds the P1-2 stub
                            expected-vs-unexpected gate cases GATE-L8/L9/L10;
                            round-5 adds GATE-L11: stub_expected requires
                            mode==\"stub\" — a deterministic-mode run of a
                            real-only target fails the stub aggregate)
focused C2e real target   : PASS 22/22 (uring_backend_c2e_close_drain_test —
                            the manifest's pinned cases tuple is the
                            authority; round-2 adds control-wins-over-co-ready-
                            ring, two-waiter-consumer-strand, non-eintr-poll-
                            failure-failfast; round-4 adds
                            future-waiter-cannot-steal-old-wake and rewrites
                            the two-waiter controller to a bounded nonblocking
                            poll loop — the rewritten
                            close-waits-for-inflight-acceptance-lp makes no
                            mutex-blocking claim; round-5 adds
                            running-cancel-poison-deferred-wake — the D4-RM17
                            detector)
death matrix              : PASS 10/10 (uring_backend_c2e_death_test — 8 semantic
                            + both-builds evidence-mode case + round-2
                            preflight-before-queue-exit-order; pending/enqueued
                            children now destroy in the GENUINE state via a
                            leaked thread; round-4 aligns the evidence-mode
                            guard with the semantic-body guard — __unix__ &&
                            SLUICE_HAS_LIBURING && SLUICE_ASYNC_INTERNAL_TESTING
                            — so an empty-body build can never emit mode=real)
shared C2e driver (Uring) : PASS (SLUICE_TEST_FILTER=conformance_close_drain_uring;
                            [conformance-meta] backend=Uring profile=KernelIoProfile
                            mode=real)
mutations                 : PASS 13/13 RED->GREEN (D4-M1..D4-M13) + D4-L1 lift
                            + 9/9 RED->GREEN (D4-RM1..D4-RM9) + G2 drift closure
                            + round-2 RED->GREEN: D4-RM10 (post-poll ring-ready
                            bypasses control reclassification), D4-RM11
                            (destructor preflight bypassed before queue_exit),
                            D4-RM12 (non-EINTR poll treated as retryable),
                            pre-poll barrier uniqueness, close-LP shared-lock
                            (D4-RM8 revisited via source-drift detector)
                            + round-3 RED->GREEN: D4-RM13 (inter-iteration
                            control wake rebaselined)
                            + round-4 RED->GREEN: D4-RM14 (commit-to-park
                            handshake removed — P0-1), D4-RM15
                            (durable-broadcast gate removed — P0-2), D4-RM16
                            (poison wake inside dispatch_mtx_ — P1-3; the
                            lock-order repair restores the frozen
                            state-first-then-wake structure)
                            + round-5 RED->GREEN: D4-RM17
                            (issue_running_cancel newly-poisoned early return
                            skipped the deferred wake — P0; the wake is
                            deferred past the lock scope on every poison
                            path)
                            (see phase-d4 mutation evidence doc)
real liburing Debug full  : PASS 158/158 (xmake test -v)
real liburing Release full: PASS 158/158 (xmake f -m release --toolchain=clang
                            --with-liburing=true -y; xmake test -v)
stub/off suite            : PASS 156/156 (xmake f --with-liburing=false; xmake test -v;
                            round-4 registers the D2 pinned corpus in stub
                            (empty bodies) so a stub run is INCOMPLETE for the
                            RIGHT reason — "mode=stub not allowed by
                            required_modes" — never a case-set mismatch)
ASan+UBSan                : PASS (xmake f -m asanubsan --toolchain=clang -y;
                            xmake run -g test; zero sanitizer reports)
TSan                      : PASS (xmake f -m tsan --toolchain=clang -y;
                            xmake run -g test; zero ThreadSanitizer reports)
negative compile          : PASS 5/5 probes (completion-authority,
                            request-arena, async-api, async-identity,
                            external-backend-authority)
formal models             : PASS 18/18 (python3 scripts/formal/verify.py all;
                            45 positive, 93 negative, 55 reachability;
                            executed in full on the round-3 head 2026-08-10 —
                            e13-select-core 1605s, e13-select-safety 331s,
                            the earlier local timeouts were model size, not
                            failures)
                            FORMAL COVERAGE GAP (round-4/round-5): the
                            D4-RM14/RM15/RM17 wait-source protocol — the
                            commit-to-park registration, the eventfd
                            durable-broadcast gate, and the poison-path
                            deferred wake (RM16/RM17) — is NOT modeled by any
                            TLA+ suite (the e16 model covers the Runtime
                            lifecycle at the epoch level, not the wait
                            source's transport). The C++ detectors
                            (stop_between_mw_s2_commit_and_backend_wait_
                            registration,
                            uring_c2e_future_waiter_cannot_steal_old_wake,
                            uring_c2e_running_cancel_poison_deferred_wake)
                            carry the regression evidence. REVISIT TRIGGER: a
                            future wait-source/control-wake protocol change
                            must either add a focused model (the smallest
                            protocol capturing the register->publish->
                            acknowledge race) or re-record this gap with the
                            new change.
pre-push gate             : PASS (bash scripts/gates/pre-push.sh; and the
                            Lefthook pre-push hook on the branch push)
```
