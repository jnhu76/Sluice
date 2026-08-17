# Issue #115 — Runnable-Publication Wake Obligation: Compliance Gate

**Change class:** post-freeze evidence-derived correctness fix (Issue #115).
**Baseline master SHA:** `fbb3ea074db8c7c2aafc3c5e4599166e2381f458` (post #119).
**Generic gate:** `docs/architecture/design-compliance-gate.md` (this document
covers every Gate 0–4 field for the fix and links back).
**Investigation:** `docs/investigations/issue-115-runnable-publication-wake.md`

**One-line defect:** `Scheduler::spawn()` / `Scheduler::spawn_on()` published a
runnable ticket onto a participant's `local_runnable` and notified only the
target's `inbox_cv` — a condition variable no production path ever waits on —
without advancing the Scheduler wake epoch. Every worker already committed to
the unbounded wake-domain park slept through the publication, stranding a
stealable ticket whose owner was busy inside user Fiber code.

---

## Gate 0 — Architecture Classification

```text
Affected capability:    Scheduler runnable publication wake transport (spawn/spawn_on)
Affected layer:         src/async/scheduler.cpp spawn()/spawn_on() publication tail;
                        include/sluice/async/scheduler.hpp interface comments
Classification:         Corrective (evidence-derived; restores RP-1/RP-2 — the
                        same publication obligation route_runnable_locked
                        already carries under ADR-execution-model.md §9.4.5)
Governing ADR:          ADR-execution-model.md §9.3.5 (runnable publication),
                        §9.4.5 (park/wake epoch protocol) — UNCHANGED by this fix
Conformance map change: no (no state machine, authority, or identity change;
                        two publication sites gain the canonical wake signal)
Constitution rules:     AC-4 (progress must be observed / no stranded work),
                        AC-6 (explicit wake obligation: state first, then wake;
                        the notification is advisory, persistent state is
                        authority)
```

## Root cause (proven; see the investigation doc §3)

```text
W1: runs F1 (blocks in user fiber code)            [running observer, cannot drain]
W0: no local work -> classify mw_s1 -> park commit:
        G-recheck: running_fiber_count>0 -> observer exists -> pass
        baseline := wake_epoch_ (E) under wake_mtx_
        (G1 refusal protects only PRE-commit visibility with NO observer;
         it cannot see a publication that has not happened yet)
publisher: spawn(F2) / spawn_on(F2, W1):
        push F2 onto W1.local_runnable (state published)
        W1.inbox_cv.notify_one()                    [NO WAITER — inert]
        (no wake_epoch_ advance)                    [missing obligation]
W0: cv.wait predicate: epoch==E, !terminate, OWN inbox empty -> sleeps
F2: runnable, stealable, owner busy, all steal-capable peers asleep
    -> permanent strand (unbounded park: no deadline, no ready-flag wait)
```

The distinction the pre-fix code missed: the G1 park-commit refusal closes
"runnable already visible **before** the commit (with no observer)"; it does
nothing for "runnable published **after** the commit", whose only transport is
the wake-epoch advance the predicate checks (the E9-CORRECTIVE predicate
inspects epoch + terminate + the worker's OWN inbox only — by design, to avoid
a wake_mtx_→every-inbox lock-order expansion).

## Gate 1 — Ownership and State Machine

No state machine changes. `FiberState created->runnable` exactly-once
(`make_runnable` guard, unchanged); `fiber_owner_` recording (unchanged);
queue membership under `global_mtx_`→`inbox_mtx_` (unchanged order). The fix
appends the canonical wake publication (`signal_wake_locked()`) after the
inbox critical section releases, exactly mirroring `route_runnable_locked`
(`src/async/scheduler.cpp`). Authority table (unchanged domains):

| Domain | Authority | Fix interaction |
|---|---|---|
| run-domain queues/owners | `global_mtx_` (+ per-inbox `inbox_mtx_`) | publication unchanged, order preserved |
| wake domain | `wake_mtx_` / `wake_epoch_` / `wake_cv_` | two more publishers of the SAME advance (idempotent, coalescing-safe) |
| backend park bridge | `backend_wait_active_` gate + `interrupt_backend_waiters` | reached through the existing `signal_wake_locked` tail (see Gate 3) |

## Gate 2 — Resource and Failure Model

No new state, containers, threads, or allocation. Per-spawn added cost: one
`wake_mtx_` critical section + `wake_cv_.notify_all()` + one acquire load of
`backend_wait_active_` (the bridge stays a single load when no backend
participant is parked). No failure path: `signal_wake_locked` is
non-failing/noexcept-equivalent. Bounded by construction (wake cost ≤ worker
count per notify; spawn rate is fiber-admission rate — the same traffic class
`route_runnable_locked` already generates on every wait resolution).

## Gate 3 — Progress and Wake Model

RP-1 (publication reachability) / RP-2 (state first, then wake) restored:

```text
make_runnable -> queue membership + owner (under G->inbox) -> inbox released
    -> wake_epoch_++ (under wake_mtx_) -> wake_cv_.notify_all()
    -> [bridge] if backend_wait_active_: interrupt_backend_waiters
```

Lost-wake closure (`std::condition_variable::wait(lock, pred)` ≡
`while(!pred()) wait(lock)`; cppreference): the predicate reads
`wake_epoch_ != observed_epoch` under `wake_mtx_`. A publication that lands
after the baseline advances the epoch past it — the predicate fires at wait
entry or on notify; a publication that lands before the baseline is visible to
the G1 commit recheck when no observer exists (refusal path). The residual
pre-commit window — publication fully before the baseline while a running
fiber exists (observer exemption) — is the delegation model the canonical
route path already accepts: the running owner drains its queue when the Fiber
yields/completes (cooperative discipline); it is symmetric with
`route_runnable_locked`, documented as residual risk (investigation §13), not
a regression introduced here.

Lock order (no inversion; the forbidden edge is inbox→wake, held by the park
predicate's wake→inbox):

```text
spawn/spawn_on:   global_mtx_ -> inbox_mtx_ (release) -> wake_mtx_   [accepted G->wake]
route (existing): global_mtx_ -> inbox_mtx_ (release) -> wake_mtx_   [identical]
park predicate:   wake_mtx_ -> inbox_mtx (own only, read)            [unchanged]
```

Backend-wait bridge: a spawn while an MW-S2 participant is parked in
`ctx_.wait_one()` now interrupts that park (one-shot per `wait_one` by
D4-RM13; armed-baseline handshake D4-RM14 unchanged). The interrupt is a
re-evaluation signal, not fabricated readiness: the participant's Phase-D
drain re-classifies — runnable work exists (mw_s1) — and the loop runs it.
No interrupt loop: each interrupt corresponds to a real publication and the
re-park requires a fresh classify. Verified by the Phase-G suites (§Evidence).

## Gate 4 — Evidence (executed; commands and outputs in the investigation doc)

```text
Baseline (pre-fix master fbb3ea0):  Clang Debug xmake test -v ......... 167/167 PASS
Deterministic reproducer (pre-fix): issue115 A/B 3/3 FAIL (progressed)  [strand]
                                    issue115 C FAIL (same mechanism)   [strand]
Post-fix Clang Debug:               xmake test -v .................... 168/168 PASS (rc=0)
Post-fix Clang Release:             xmake test -v .................... 168/168 PASS (rc=0)
Post-fix reproducer:                5/5 full-binary runs PASS (all cases; 0.56 s wall)
runnable_steal_test standalone:     3/3 PASS; ASan+UBSan repeated samples 0/15 failures
TSan:                               full suite 168/168 rc=0; focused race binaries
                                    7/7 rc=0 with 0 ThreadSanitizer warnings
ASan+UBSan:                         full suite 168/168 rc=0
Stress (24-way oversubscription):   pre-fix probe 12/12 reproducer FAIL / post-fix
                                    24/24 rc=0 (investigation §12)
Mechanical gates:                   git diff --check, check-doc-links (+self-test),
                                    verify-architecture-docs, mechanical-facts
                                    (+self-test), negative-compile probes (12+6)
                                    — all PASS (investigation §11)
```

## Zig conformance / divergence

Unchanged. The `zig/` reference is not built and carries no scheduler
runnable-publication protocol binding for this fix; no divergence entry is
created. (Zig baseline audit: memory `zig-baseline-and-audit-2026-08`.)

## Foundation-freeze note

This is a post-freeze evidence-derived correctness fix: it changes no frozen
row's contract — it makes `spawn`/`spawn_on` satisfy the wakeup-semantics
obligation (`foundation-freeze.md` "Wakeup semantics" row, AC-6) that the
canonical publication path already carries. No new phase, no topology change,
no stealing-model change.
