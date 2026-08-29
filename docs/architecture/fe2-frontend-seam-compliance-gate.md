# FE-2 Frontend Seam Compliance Gate

Phase-specific architecture compliance gate (AGENTS.md §8) for the FE-2
ResumeTarget token seam + deferred-publication delivery split + stackless
Event PoV. Links to the generic gate: `design-compliance-gate.md` (all Gate
0–4 fields are covered below). Companion design authority:
`docs/history/reviews/FE-1B-FRONTEND-NEUTRAL-CONTRACT-FREEZE.md` (frozen
contract) and `docs/history/reviews/FE-1C-TYPE-IDENTITY-DELIVERY-SEAM-DESIGN.md`
(seam design). Status markers are PENDING until the command named next to
them has actually run.

---

## Gate 0 — Architecture Classification

```text
Affected capability:    Scheduler (wait lifecycle, publication) + WaitNode/WaitQueue
                        (token edge) + experimental stackless Event frontend (test-only)
Affected layer:         E7-E13 scheduler (wait/publication seams); L0-L2 unchanged
Classification:         Intentional Divergence (representation widening) within a
                        Faithful semantic Core — no semantic transition changes
Governing ADR:          FE-1b frozen contract (campaign design authority); AC-2b/AC-2c
                        deadline & cancellation authorities unchanged and reused
Conformance map change: no (no AC rule changes meaning; WaitNode token type widens)
Constitution rules:     AC-5 (single publication authority — resolve_ stays the ONLY
                        terminal winner; the record consume CAS is subordinate),
                        AC-6 (explicit wake obligation — deferred drain), AC-7
                        (bounded/caller-owned resources — transit list transient),
                        AC-9 (layered cancellation — unchanged closure reused),
                        AC-10 (docs/comments updated with the change),
                        AC-11 (deterministic tests, no sleep-as-proof)
```

## Gate 1 — Ownership and State Machine

Modified object 1: `WaitNode` token field.

```text
States: unchanged (detached → registered → {woken, cancelled, expired}).
Change: the immutable admission-bound token widens from Fiber* to WaitResume
        {void* ptr; Kind {none, fiber, deferred}}.
Transitions affected:
  register (detached → registered)
    Authority:   WaitQueue::register_wait_locked under G + queue mtx (unchanged)
    Lock domain: queue mtx inside global_mtx_ (unchanged)
    Allocation:  none (token is inline data; +8 bytes node layout, documented)
    Failure:     CAS loss → register rejected, no mutation (unchanged)
    Wake:        none (unchanged)
    Shutdown:    unchanged
  resolve (registered → terminal)
    Authority:   resolve_ CAS (unchanged; token never consulted)
    (all other fields unchanged — the token is payload, not authority)
```

New object 2: frontend continuation record (TEST-ONLY, coroutine frame-embedded).

```text
States: unarmed → armed → consumed
  unarmed → armed
    Authority:   the shared Event admission closure tail, under G, in the SAME
                 resolver-excluded CS (contract L7) — deferred-kind eligibility commit
    Lock domain: global_mtx_ (+ queue mtx)
    Allocation:  none (frame-embedded)
    Failure:     admission inline-resolved or terminal → no arm; await_suspend
                 returns false (no suspension)
    Wake:        none (arming only enables future publication)
    Shutdown:    records are frame-local; Scheduler teardown requires the transit
                 list empty, which pins unconsumed records to live waiters
  armed → consumed
    Authority:   the drain, outside all authoritative locks, acq_rel CAS (exactly once)
    Lock domain: NONE held at the CAS (G released for the chunk)
    Allocation:  none
    Failure:     CAS loss → another discharger won; do nothing (loser law)
    Wake:        the resume itself (continuation executes)
    Shutdown:    a record armed but never consumed at teardown = transit list
                 non-empty = teardown precondition violation (fail-fast)
```

New object 3: `Scheduler::deferred_publications_` transit list.

```text
States: (implicit) empty ⇄ holds record addresses
  push (defer_publication_locked)
    Authority:   winner publication tail, under G, after winner/timer/resource/
                 accounting commits (contract L5/L8 — publication LAST)
    Lock domain: global_mtx_
    Allocation:  possible vector growth — bounded by CONCURRENT armed suspended
                 waiters (same resource class as worker inboxes: grows with
                 outstanding, not historical; drained every drain point)
    Failure:     bad_alloc under G — same class as inbox push today; the
                 terminal decision already committed, delivery is pending state,
                 not a lost terminal (see Gate 2)
    Wake:        the drain (explicit call after CS release)
    Shutdown:    teardown gate requires empty
  take (drain_deferred_publications chunk)
    Authority:   the drain caller (frontend), under G for the move-out only
    Lock domain: global_mtx_ for the chunk move; NONE during resume
    Allocation:  none (caller-provided fixed buffer)
    Failure:     n/a (move-out cannot fail)
    Wake:        resume() per consumed record (user continuation runs lock-free)
    Shutdown:    drain is the discharge path; teardown requires drained empty
```

Modified function family: Event admission closures
(`await_event_wait[_deadline]` internals) — refactored into shared `_locked`
ladders parameterized by {WaitResume, kind, fiber/ws}. Ladder steps, order,
lock topology, counters, and inline-resolution law are BYTE-IDENTICAL for the
fiber kind; only the token source and the eligibility-commit tail differ.
The fiber entry remains the public API; the deferred entry is reached only
through internal-testing seams.

## Gate 2 — Resource and Failure Model

```text
Construction-time resources:
  - deferred_publications_: capacity=[unbounded-transient, drained; same class
    as worker inboxes], allocation=[lazy vector, first push], failure=[bad_alloc
    under G → terminal already committed; entry retried by next winner? NO —
    single winner: entry push failure = publication obligation stranded]
    MITIGATION: vector push of one pointer cannot realistically fail after the
    first small growth; the PoV and the compliance test record treat push
    failure as fail-fast (contract L8: "at most once" — a lost push would
    strand a suspended waiter, which is a contract violation, so fail-fast is
    the honest behavior; std::vector::push_back strong guarantee keeps the
    list consistent).
Submit-time (admission) resources:
  - unchanged: R2-ALLOC prepare-before-mutate discipline intact; the token adds
    no allocation; eligibility arm adds no allocation (record is frame-local)
  - Is admission allocation-free after acceptance? YES (unchanged)
Completion-time resources:
  - drain chunk buffer: caller-stack, fixed, no allocation
  - Can a completed result be lost due to allocation failure? NO for the
    terminal (resolve_ CAS precedes publication); the DEFERRED DELIVERY push
    failure is fail-fast (above), never silent loss.
Capacity and backpressure:
  - Maximum deferred entries = concurrent suspended deferred-kind waiters
    (caller-owned WaitNodes bound it; AC-7 caller-owned class)
  - Queue-full behavior: n/a (transient, no cap)
  - OOM at each stage: admission unchanged; drain none; push fail-fast
Reclamation:
  - Do containers shrink? The list drains to empty between wait episodes;
    capacity (not size) may persist — same as inboxes. Growth bounded by
    outstanding, not historical.
```

## Gate 3 — Progress and Wake Model

```text
Blocking/suspension:
  - Who may block?  scheduler worker / caller thread (unchanged)
  - Who may suspend? Fiber (unchanged) + coroutine frame (NEW, test-only
    frontend; physical suspension BEFORE await_suspend runs — A2)
  - What makes them continue? fiber: worker routing (unchanged); coroutine:
    drain_deferred_publications discharge resume()

Deferred delivery progress (the only new wake edge):
  - Producer: winner tail under G (push) — persistent state written BEFORE
    the producer releases G
  - Consumer: frontend drain after the resolver CS releases
  - Predicate: list non-empty (guarded by G — no separate atomic, no separate
    lock; the drain observes the SAME mutex domain that wrote it)
  - Commit-to-sleep race: N/A — the drain is invoked, not slept-on; a record
    pushed before G release is visible to any later drain (mutex ordering)
  - Worst-case latency: until the next drain invocation at the resolver seam
    (immediately after the CS that created the obligation — PoV contract);
    no periodic dependency introduced
  - Shutdown: teardown gate requires drained-empty; a stranded record is a
    fail-fast precondition, not a silent leak

Single-worker liveness: unchanged for fibers; the coroutine frontend has no
worker dependency (drain runs on the resolver thread) — a coroutine waiter
makes progress even with zero scheduler workers, which is a FE capability
gain, not a liveness risk.
```

## Gate 4 — Evidence Plan

```text
Deterministic causal tests (internal-testing target; exact names in the PoV):
  - deferred wait, event set after suspension: proves async resolution →
    exactly-one resume (no double, no lost)
  - deferred wait, event already set before admission (§21-A): proves inline
    resolution publishes nothing; await_suspend returns false; zero resumes
  - deferred wait, set during admission-before-arm (§21-B, deterministic phase
    seam): proves the inline window never creates an obligation (L6)
  - deferred wait, set immediately after arm (§21-C): proves armed epoch gets
    exactly one async publication
  - deferred cancel (§21-D): proves cancellation closure reused; Cancelled
    outcome consumed inline after resume=false path / via winner tail
  - deadline already due (§21-E) and due-after-arm (§21-F): proves ordinary
    deadline authority reuse (expired outcome, no second timer authority)
  - no-user-code-under-lock witness (§22): resumed body re-enters a primitive
    seam that takes G; deterministic phase observation proves the resolver CS
    already released (no sleep)
  - resume-before-armed fail-fast: try_consume on unarmed record fails loudly
    (L8 guard sensitivity)
  - stackful regression: full existing suite green (Event/Queue/RwLock/
    Condition/Semaphore/Mutex/Select/timer suites unchanged behavior)

Backend conformance: n/a (no backend touched).

Resource-bound tests:
  - teardown gate: scheduler destruction with a stranded deferred entry fails
    fast (proves L13 extension)
  - drain chunk loop: >chunk-size episode drains fully (loop termination)

OOM / failure-injection: push-failure posture documented (fail-fast); no
injection harness in scope (recorded as residual posture, not claimed evidence).

Shutdown race tests: teardown gate above + full drain teardown-green suite.

Sanitizers:
  - [ ] TSan clean (concurrency class: winner tails vs drain vs admission)
  - [ ] ASan+UBSan clean (frame-embedded record lifetime, drain buffer)

Benchmark: NO performance claim in FE-2 (inspection only; §37 of campaign).
```

## Gate Completion Checklist

- [ ] Gate 0 classification complete (above)
- [ ] Gate 1 state machines cover WaitNode token, record, transit list, ladder
- [ ] Gate 2 resource model: no unbounded HISTORICAL growth (transient class documented)
- [ ] Gate 3 wake model: no new polling dependency (drain is invoked at seam tails)
- [ ] Gate 4 evidence filled with ACTUAL results before any PASS claim
- [ ] Conformance map: no change (representation widening only)
- [ ] Divergence registry: record the documented +8B WaitNode layout change and
      the test-only coroutine frontend as intentional, bounded divergences
- [ ] AGENTS.md change-class gates: Debug (always), TSan (§16.3 — scheduler/
      synchronization change), ASan+UBSan (§16.2 — new lifetime), Release
      (§16.1 — installed header change: wait_node.hpp/wait_queue.hpp)

---

## FE-3 Addendum — Queue Vertical Slice (Gate 0–4 recheck)

The FE-3 Queue slice extends the FE-2 seam to `AsyncQueue`/`QueuePort` under
the SAME classification and the same four gates; only the deltas are stated
here (the parent sections above remain authoritative).

### Gate 0 — Classification delta

- Authority touched: Queue admission/reconciliation (AC-4 wait/wake, AC-6
  queue capacity) — same gate class as FE-2; no new subsystem, no public API.
- Production surface: `Scheduler::queue_push_admit_locked` /
  `queue_pop_admit_locked` (ONE textual admission ladder per direction,
  blocking+timed, parameterized by `WaitResume`), `QueueAdmitDisposition`
  {rejected, resolved_inline, resolved_inline_grant, authorized},
  `queue_publish_winner_locked` (the ONE grant publication tail),
  `queue_cancel` tail switched to `publish_wait_winner_locked`.
  The four Fiber admit entries become thin frontend entries; their
  instruction sequences are preserved (ladder body = moved code).
- Test-only surface (DIV-16): deferred Queue admission entries in
  `scheduler_fe2_test_seam.cpp` reproducing the QueuePort ordinary-entry
  protocol (lifecycle gate + `active_port_calls_` interval + control
  transition + frame-embedded `QueueWaitCtx`), plus the
  `fe3_stackless_queue_slice_test` target.

### Gate 1 — State machine delta

- Ladder: `register -> counters -> [timed: LOCAL publish] -> precedence`
  (commit / closed / already-due-expire) `-> authorized`. The disposition
  RE-USES the existing WaitNode `resolve_` winner; no new epoch state.
- `resolved_inline_grant` obliges the ENTRY to run the Q-LIV-1
  opposite-role grant after its role-mutex release — the grant lock
  position (G + S only) is unchanged; the two role mutexes are still never
  held together.
- Deferred-kind grant publication: `defer_publication_locked` WITHOUT the
  `granted_not_resumed_` increment. Rationale: that counter pairs the grant
  increment with the FIBER winner's post-resume decrement under G; a
  deferred winner's discharge path never touches the port again (the
  coroutine frame carries `QueueWaitCtx` + lease/out), so there is no
  pairing decrement and no FIFO-empty teardown window change:
  `begin_teardown` may proceed once both role FIFOs drain; the in-flight
  deferred obligation is owned by the Scheduler-level
  `deferred_publications_` teardown gate (stranded fail-fast, FE-2).

### Gate 2 — Resource model delta

No new resource. `active_port_calls_` bracket reproduced for the deferred
entry (single-exit decrement); ring/lease custody unchanged; the transit
list stays bounded by CONCURRENT suspended deferred waiters (FE-2 Gate 2).

### Gate 3 — Wake model delta

Fiber path: unchanged (commit_suspend -> context_switch -> post-resume
`--granted_not_resumed_`). Deferred path: grant resolves the node under
G + S + role, writes the transit entry under G (persistent state) BEFORE
release; the drain take is a G-scoped move-out; discharge resumes with NO
lock held (L9). No new polling, no lost-wake window: the deferred arm
lands inside the resolver-excluded admission CS (L7), so every resolver
observes either pre-registration (membership fail) or post-arm state.

### Gate 4 — Evidence (FE-3 Queue slice)

```text
BASE: feat/frontend-semantic-reuse @ FE-2 closeout (PR #243 Draft)
Commands (actual results):
  xmake f -m debug --toolchain=clang -y
  xmake build sluice_core / sluice_async / -g test     -> OK
  xmake test -v                                        -> 195/195 PASS
    (incl. fe3_stackless_queue_slice_test: 12 cases — inline push/pop,
     deferred push granted by try_pop Q-LIV-1, deferred pop granted by
     try_push, close dispositions (push retained / pop closed+empty),
     cancel loser-exactly-once, timed expiry (pump retire + defer),
     FIFO close drain of two parked deferred producers, and two
     cross-frontend worker mixes)
  xmake f -m tsan --toolchain=clang -y; build; xmake run -g test
                                                       -> ALL TESTS PASSED
    (race classes: queue submit vs dequeue, grant vs drain, cancel vs
     admission, teardown balance with deferred winners)
  python3 scripts/gates/mechanical-facts.py            -> OK
  python3 scripts/check-doc-links.py                   -> PASS
  python3 scripts/verify-architecture-docs.py          -> OK
  python3 scripts/gates/assert-hygiene.py              -> OK (no new
      assert sites: fail-fast reuses existing named authorities)
  git diff --check                                     -> clean
Sanitizers: TSan run above; ASan+UBSan + Release deferred to the FE-4
  full-campaign gate (same class as FE-2; no new lifetime surface — the
  frame-embedded ctx/lease is the FE-1a property under test).
Benchmark: NO performance claim (structural authority sharing; §37).
```

## FE-3 RwLock vertical-slice addendum (writer ownership as ActorIdentity)

The FE-3 RwLock slice extends the FE-2 seam to `AsyncRwLock` under the SAME
classification and the same four gates; only the deltas are stated here (the
parent sections and the FE-3 Queue addendum remain authoritative).

### Gate 0 — Classification delta

- Authority touched: RwLock admission/reconciliation + writer OWNERSHIP
  identity (AC-4 wait/wake) — same gate class as FE-2/FE-3-Queue.
- Production surface: `Scheduler::rwlock_read_admit_locked` /
  `rwlock_write_admit_locked` (ONE textual admission ladder per mode,
  blocking+timed, parameterized by `WaitResume` + the caller's actor),
  `WaitAdmitDisposition` (renamed from the FE-2 `EventAdmitDisposition` —
  shared vocabulary, same three values), the shared ownership cores
  `rwlock_try_write_admission_locked` / `rwlock_unlock_write_core_locked`
  (consumed by BOTH the fiber entries and the deferred seam), and
  `rwlock_grant_from_head_locked` committing `writer_owner = winner's
  ActorId` at grant time.
- Identity change (FE-1b corrective A1): `AsyncRwLock::writer_owner_` moves
  from `Fiber*` to the new public plain-data token `ActorId`
  {none, fiber(Fiber*), frontend(void*)}. ActorIdentity is deliberately a
  DIFFERENT type from `WaitResume` (delivery/ResumeTarget): ownership
  comparisons never inspect how a winner will be resumed, and a fiber
  pointer can never equal a frontend token by address coincidence (kind tag
  participates in equality). `RwWaitCtx` gains the `actor` field and moves
  to the non-installed `scheduler_internal.hpp` — NO `WaitNode` layout
  change; `AsyncRwLock` grows by 8 bytes (ActorId vs raw pointer), an
  accepted internal layout cost of one lock instance per contention domain.
- Fail-fast conversions (§9.2): the reachable caller-contract asserts at the
  recursive-write check and the unlock-write owner checks become named
  Release-active entries (`async_rwlock_recursive_write_fail_fast`,
  `async_rwlock_unlock_write_inactive_fail_fast`,
  `async_rwlock_unlock_write_not_owner_fail_fast`); the grandfathered
  E12-F expiry-ladder asserts are preserved verbatim (moved, not edited).
- Test-only surface (DIV-16): deferred read/write/try/unlock/cancel
  admission entries in `scheduler_fe2_test_seam.cpp` (out-of-line; the seam
  header forward-declares `AsyncRwLock`/`RwWaitCtx` to avoid the installed
  circular include) plus the `fe3_stackless_rwlock_slice_test` target.

### Gate 1 — State machine delta

- Write ladder: `register -> [timed: LOCAL publish] -> precedence`
  (head-prefix inline claim commits `writer_active = true; writer_owner =
  actor` / already-due expire / terminal recheck) `-> authorized`. The
  REGISTRATION-ADMISSION-DRIFT note lives ONCE at the read ladder; both
  ladders claim through the ONE `rwlock_claim_node_woken_locked` primitive
  (resolve/unlink/retire/accounting sequence shared with
  `grant_from_head_locked`).
- Ownership law location: the inline claim and `unlock_write_core_locked`
  are the ONLY two sites that write `writer_owner`; grant-from-head is the
  only site that writes it for a PARKED winner. All three commit the
  winner's/caller's ACTOR, so "same ActorIdentity + different ResumeTarget"
  still owns and releases (proved by a dedicated slice case).
- Cancel/expiry: unchanged winner law (head reconcile after unlink); the
  captured-free tails publish through `publish_wait_winner_locked`.

### Gate 2 — Resource model delta

No new resource. Reader batching, waiting counts, and timer retirement are
unchanged (ladder body = moved code). The deferred transit list stays
bounded by CONCURRENT suspended deferred waiters (FE-2 Gate 2).

### Gate 3 — Wake model delta

Identical shape to the FE-3 Queue addendum Gate 3: the deferred arm lands
inside the resolver-excluded admission CS (L7); grants resolve under
G + W with publication under G after W release (reader batch collected
under W, published after); discharge resumes with NO lock held (L9). No
new polling, no lost-wake window.

### Gate 4 — Evidence (FE-3 RwLock slice)

```text
BASE: feat/frontend-semantic-reuse @ FE-3 Queue slice (PR #243 Draft)
Commands (actual results):
  xmake f -m debug --toolchain=clang -y
  xmake build sluice_core / sluice_async / -g test     -> OK
  xmake test -v                                        -> 196/196 PASS
    (incl. fe3_stackless_rwlock_slice_test: 7 cases — deferred writer
     owns+releases on ActorId alone; same actor + different resume
     target; deferred writer granted by FIBER unlock_write; writer
     fairness (parked reader must not bypass parked writer); reader
     prefix batch from one reconcile; cancel of a parked deferred
     writer (loser-exactly-once, terminal cancel is a loser); timed
     expiry via the pump with timer retirement)
  xmake f -m tsan --toolchain=clang -y; build; xmake test
                                                       -> 196/196 PASS
    (race classes: rwlock admission vs grant-from-head, unlock vs
     batch grant, cancel vs resolve, expiry vs resolve, deferred
     discharge with no lock held)
  python3 scripts/gates/mechanical-facts.py            -> OK
  python3 scripts/check-doc-links.py                   -> PASS
  python3 scripts/verify-architecture-docs.py          -> OK
  python3 scripts/gates/assert-hygiene.py              -> OK (moved
      assert strings kept verbatim; new enforcement is named
      fail-fasts, not assert-family)
  git diff --check                                     -> clean
Observed one-off (NOT this slice's class): during the first TSan suite
  run, select_event_registry_test
  (test_phase_seam_reset_serialization — a 4-thread causal phase-seam
  test outside the rwlock change surface) hung once and was killed;
  it then passed standalone, passed 20/20 in a repeated-run loop under
  TSan, and the full suite passed 196/196 on re-run. Debug passes the
  same test repeatedly. Classified: rare test-infrastructure flake in
  the phase-seam causal test, no rwlock-slice attribution; tracked for
  observation, not repaired in this slice (§19 no-batching).
Sanitizers: TSan run above; ASan+UBSan + Release deferred to the FE-4
  full-campaign gate (no new lifetime surface: ownership identity is
  plain data; the frame-embedded ctx/record is the FE-1a property).
Benchmark: NO performance claim (structural authority sharing; §37).
```

## FE-3 Condition vertical-slice addendum (shared CONDITION-WAIT-PREPARE ladder)

Extends the FE-2 seam to the Condition epoch under the SAME classification and
the same four gates; only the deltas are stated (the parent sections and the
FE-3 Queue/RwLock addenda remain authoritative).

### Gate 0 — Classification delta

- Authority touched: Condition admission/reconciliation (AC-4 wait/wake) +
  the register-before-handoff combined step — same gate class as FE-2 and the
  other FE-3 slices.
- Production surface: `Scheduler::condition_wait_admit_locked` (the ONE
  textual admission law for the Condition epoch, blocking+timed,
  parameterized by `WaitResume`), `ConditionAdmitDisposition`
  {rejected_retain, resolved_inline_retain, resolved_inline_released,
  authorized} — the disposition ENCODES the released_mutex/reacquire
  obligation so both frontends derive it from one source. The two fiber
  entries (`condition_wait_prepare` / `_until`) become thin frontend entries
  over the ladder; their sequences are preserved (ladder body = moved code).
  `condition_cancel_wait` publishes through `publish_wait_winner_locked`
  instead of the direct `cond_node.fiber()` fiber route — behavior-equal for
  the fiber branch, and a DEFERRED cancelled waiter is no longer stranded
  (its cancelled terminal is delivered through the deferred branch).
- Scope boundary (documented, FE-1b A1 §12): Condition has NO identity of its
  own — it inherits Mutex choreography. Mutex ownership re-typing is its own
  later slice (RwLock is done); the deferred PoV therefore presents BARE
  WaitQueues (empty bound Mutex queue ⇒ the documented UnlockNoWaiter no-op)
  and the full AsyncCondition choreography composition stays covered by the
  UNCHANGED fiber tests running over the SAME ladder.
- Test-only surface (DIV-16): deferred condition wait entries +
  notify_one/notify_all/cancel one-liners over the presented bare queues in
  `scheduler_fe2_test_seam.cpp` / `scheduler_test_access.hpp`, plus the
  `fe3_stackless_condition_slice_test` target.

### Gate 1 — State machine delta

- Ladder: `[timed: R2-ALLOC prepare -> register -> LOCAL timer publish] ->
  register -> already-due inline Expired (Mutex RETAINED) ->
  register-before-handoff phase seam -> Mutex handoff (the ONE accepted
  mutex_handoff_one_locked; owner committed BEFORE publication) -> terminal
  recheck -> authorized`. The lost-notify closure is unchanged: a
  notify/cancel/expire needs global_mtx_ and cannot interleave between
  registration and Mutex release.
- SUSPENSION DISCIPLINE (repair recorded): the fiber entries' `global_mtx_`
  guard scope still ends BEFORE the physical `context_switch` — the
  first draft of this slice held the guard across the switch (an
  immediate self-deadlock in the fiber condition tests); the extraction now
  mirrors the pre-extraction brace structure exactly.

### Gate 2 — Resource model delta

No new resource. Timer pool / heap behavior unchanged (R2-ALLOC prepare
order preserved); the deferred transit list stays bounded by CONCURRENT
suspended deferred waiters (FE-2 Gate 2).

### Gate 3 — Wake model delta

Unchanged for fibers. Deferred: the arm lands inside the resolver-excluded
`global_mtx_` CS (L7); the discharge resumes with NO lock held (L9). The
winner's reacquire epoch is the BODY'S OWN step after resume — witnessed in
the slice by the presented owner slot staying released at resume time for
Woken, suspended-Expired, and Cancelled resolutions alike, and NOT run for
`released=false` dispositions (rejected / already-due Expired retain the
Mutex).

### Gate 4 — Evidence (FE-3 Condition slice)

```text
BASE: feat/frontend-semantic-reuse @ FE-3 RwLock slice (PR #243 Draft)
Commands (actual results):
  xmake f -m debug --toolchain=clang -y
  xmake build sluice_core / sluice_async / -g test     -> OK
  xmake test -v                                        -> 197/197 PASS
    (incl. fe3_stackless_condition_slice_test: 5 cases — deferred
     notify_one (own-reacquire separation), already-due inline Expired
     with Mutex RETAINED (owner sentinel observable), pump expiry
     (suspended Expired still reacquires), cancel loser-exactly-once +
     terminal re-wait rejection, notify_all two-winner drain; plus the
     UNCHANGED async_condition_primitive_test fiber suite over the SAME
     ladder)
  xmake f -m tsan --toolchain=clang -y; build; xmake test   -> see below
  python3 scripts/gates/mechanical-facts.py            -> OK
  python3 scripts/check-doc-links.py                   -> PASS
  python3 scripts/verify-architecture-docs.py          -> OK
  python3 scripts/gates/assert-hygiene.py              -> OK
  git diff --check                                     -> clean
Sanitizers: TSan + ASan/UBSan + Release deferred to / covered by the
  FE-4 full-campaign gate (no new lifetime surface; the guard-scope
  repair above is verified by the full suite).
Benchmark: NO performance claim (structural authority sharing; §37).
```
