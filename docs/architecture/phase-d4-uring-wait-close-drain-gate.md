# Phase D4 — Uring Wait / Close / Drain / Destruction Gate

Status: COMPLETE (2026-08-10, branch feat/phase-d4-uring-wait-close-drain
stacked on the D3 head `4cc4789`)

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
Conformance map change: no (no new public semantic; close_admission() was already
                        in the public surface for Fake/ThreadPool; Uring now
                        implements the same ADR Decision 15 seam)
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
    Lock domain:    dispatch_mtx_ (reap serialization) + arena leaf
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
                  check + eventfd drain, then poll(2) WITHOUT any lock;
                  wakes: ring fd POLLIN (kernel CQE), control eventfd
                  (interrupt_all/signal_progress writes), EINTR (re-loop),
                  POLLNVAL (fail-fast — parked waiter with a torn-down fd is
                  a caller contract violation)
  report          progress (ring readable) or interrupted (control epoch
                  advanced); the CONTEXT owns the final poll that closes the
                  interrupt-vs-final-ready race
```

The wait source is observe-only (frozen design §11 / issue #67): it NEVER
reaps, records terminals, publishes Completions, mutates RequestArena state,
cancels operations, or changes outstanding. `AsyncIoContext` continues to own
serialized poll/reap under `access_mtx_`.

## Gate 2 — Resource and Failure Model

```text
Construction-time resources:
  - control eventfd: one fd, created in UringWaitSource ctor; failure ->
    std::runtime_error, wait_source() then returns nullptr (legacy serialized
    wait_one contract applies; no partial wait capability)
  - wait source object: one unique_ptr member, created after
    io_uring_queue_init succeeds; ring fd installed once before any park

Submit-time resources:
  - unchanged from D1/D2 (bounded RequestArena, bounded dispatch ring, bounded
    router/ledger); close adds no allocation
  - post-accept allocation: NONE (I9) — close cannot make an accepted request
    lost; admission_closed_ is a plain bool under dispatch_mtx_

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
  - eventfd counter: one-shot drain before park; epoch not persistent state —
    no shutdown busy-spin
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

Multi-waiter: one 8-byte eventfd write wakes ALL parked pollers (level-triggered;
empirically verified with N=3). Re-arm: a pinned/backend-ready request that is
temporarily reap-ineligible re-arms readiness so no wake is lost.

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
    submit-vs-close LP — close blocks while the submit holds the transaction
    (pause between arena commit and binding->outstanding), returns only after
    the LP completed (submit wins)
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
    one interrupt_all() wakes ALL N=3 parked participants; nothing fabricated
- uring_c2e_interrupt_final_reap_closes_ready_race:
    a terminal recorded in the interrupt-vs-final-poll window is reaped by the
    final poll — wait_one returns the actual count, never 0
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
(close -> drain -> reset -> destroy) exits 0.

Shared C2e suite: `conformance_close_drain_uring` drives the UNCHANGED shared
cases through the public `close_admission()` / `arena_slot_in_use()` seams —
`close_rejects_future_submit`, `close_preserves_accepted_terminal`,
`drain_then_reset_releases_slot`, `slot_released_but_admission_stays_closed`.

Mutations: D4-M1..D4-M13 RED→GREEN (see
`docs/verification/phase-d4-uring-wait-close-drain-mutation-evidence.md`) plus
the final lift mutation D4-L1 (KernelIo hard-code removal, gated on the
complete real-mode evidence set).

## Lock / atomic authority table

| Domain | Guards | Held while | MUST NOT |
| ------ | ------ | ---------- | -------- |
| `dispatch_mtx_` (backend) | admission transaction Stage 1-5, close_admission, poll re-dispatch, cancel remove_exact, destructor preflight | submit LP; close; front peek/transfer | call arena leaf (it is acquired separately, frozen order dispatch -> arena); wait for worker progress; join threads |
| wait-source `mtx_` (LEAF) | progress/control epochs, eventfd drain | snapshot; epoch check + drain | call Scheduler; publish Completions; call user code; touch request state |
| arena leaf mutex | slot lifecycle, terminal winner, ready-ring | record_terminal / reap / release | call Scheduler; call ReadySink (sink runs outside); publish outside the leaf (the ready release-store IS the linearization point) |
| `access_mtx_` (context) | serialized poll/reap + stats accounting | poll; accounting | the PARK (park is outside the lock — I1) |

Order: `access_mtx_ -> dispatch_mtx_ -> arena leaf -> wait-source mtx_` (leaf).
Wait-source `mtx_` never acquired while holding any other lock (signal/
interrupt are called outside the backend lock). No bidirectional cycles.

## Wake / progress model (AGENTS.md §13.2)

```text
Persistent state:  progress_epoch_ / control_epoch_ (published under mtx_)
Signal producer:   signal_ready_progress() (reap n>0, terminal_noop re-arm);
                   interrupt_all() (close_admission, ctx.interrupt_backend_waiters)
Sleeping consumer: wait_for_change() park (poll on ring fd + control fd)
Predicate:         epoch mismatch OR ring fd POLLIN
Commit-to-sleep:   epoch check + eventfd drain under mtx_, then poll (any write
                   after the drain lands in the level-triggered counter and
                   wakes the park — the three-window theorem)
Worst case:        kernel CQE -> POLLIN (direct); control write -> poll return
Shutdown:          interrupt_all is one-shot; no persistent "never park" state
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
manifest self-tests       : PASS 181/181 (incl. KernelIo lift semantics:
                            stub->INCOMPLETE, complete real set->ELIGIBLE;
                            close-drain real-mode downgrade)
focused C2e real target   : PASS 16/16 (uring_backend_c2e_close_drain_test)
death matrix              : PASS 8/8 (uring_backend_c2e_death_test)
shared C2e driver (Uring) : PASS (SLUICE_TEST_FILTER=conformance_close_drain_uring;
                            [conformance-meta] backend=Uring profile=KernelIoProfile
                            mode=real)
mutations                 : PASS 13/13 RED->GREEN (D4-M1..D4-M13) + D4-L1 lift
                            (see phase-d4 mutation evidence doc)
real liburing Debug full  : PASS 158/158 (xmake test -v)
real liburing Release full: PASS 158/158 (xmake f -m release --toolchain=clang
                            --with-liburing=true -y; xmake test -v)
stub/off suite            : PASS 155/155 (xmake f --with-liburing=false; xmake test -v)
ASan+UBSan                : PASS (xmake f -m asanubsan --toolchain=clang -y;
                            xmake run -g test; zero sanitizer reports)
TSan                      : PASS (xmake f -m tsan --toolchain=clang -y;
                            xmake run -g test; zero ThreadSanitizer reports)
negative compile          : PASS 5/5 probes (completion-authority,
                            request-arena, async-api, async-identity,
                            external-backend-authority)
formal models             : PASS 18/18 (python3 scripts/formal/verify.py all;
                            45 positive, 93 negative, 55 reachability)
pre-push gate             : PASS (bash scripts/gates/pre-push.sh; and the
                            Lefthook pre-push hook on the branch push)
```
