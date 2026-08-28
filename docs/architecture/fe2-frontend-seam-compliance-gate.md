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
