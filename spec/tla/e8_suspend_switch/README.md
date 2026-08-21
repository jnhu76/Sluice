# E8 Suspend-Switch Steal-Exclusion — Focused TLA+ Model (MODEL-007a / I47-F2)

Narrow, as-built, **C++-first** safety model of the window that
`E8OwnershipTransfer.tla` deliberately fuses away: its atomic `SuspendFiber`
linearizes register + `make_waiting` + the PHYSICAL `context_switch` into one
step, hiding exactly the transient this suite exists to examine.

The load-bearing question:

```text
A Fiber is logically Waiting — or already woken Runnable with a runnable
ticket published on the victim's local_runnable — while the victim worker's
CPU is still executing the Fiber and fiber->ctx rsp/rbp/rip have NOT yet
been saved by the in-flight context_switch.

What stops another worker from stealing that ticket and resuming a context
that was never saved?
```

As-built answer (production C++ is the implementation-level fact source; the
model conforms to the C++, never vice versa): **`WorkerState::suspend_switch_pending`**
— raised UNDER `global_mtx_` by `Scheduler::commit_suspend_locked` BEFORE
`make_waiting()`; cleared on the scheduler continuation in `run_next_on` AFTER
the physical save; read by `try_steal` under `global_mtx_`, refusing the whole
victim while true.

Tracker: issue #172 (parent umbrella #171 MODEL-007(a)).

## Exact C++ binding (master `c1e93f9`)

| Model construct | Production anchor |
| --------------- | ----------------- |
| `CommitSuspend` | `src/async/scheduler.cpp` `commit_suspend_locked` (:1313) — one `global_mtx_` CS: `store(true, release)` then `make_waiting()` |
| `RaiseAuthorityLate` (mutant-only) | the OLD P1-1 corrective shape (raise after G release) that I47-F2 replaced — `src/async/select.cpp` :1168-1169 records the history |
| `Resolve` | resolver publication under G: `make_runnable()` + `route_runnable_locked()` (`src/async/scheduler.cpp` :1388/:1513) — **no pending read** |
| `StealRefused` | `try_steal` pending check (`src/async/scheduler.cpp` :1931-1933): skip the WHOLE victim |
| `SaveContext` | `fiber_ctx::context_switch` three stores of rsp/rbp/rip into `fiber->ctx` (`src/async/fiber_ctx.cpp`) |
| `ClearPending` | `run_next_on` scheduler continuation (`src/async/scheduler.cpp` :1308) — the SINGLE clear point, no lock held |
| `StealTicket` | `try_steal` commit under G: queue move + `fiber_owner_` transfer (:1941-1958) |
| `PopResumeOnThief` | `run_next_on(W1, F)`: `make_running()` (I47-F3 fail-fast encoded structurally as the guard) + switch into `fiber->ctx` |

## State mapping

| Variable | Domain | Production representation |
| -------- | ------ | ------------------------- |
| `fiberState` | `Running`(on W0) / `Waiting` / `Runnable` / `Resumed`(on W1) | `Fiber::state_` CAS; executor encoded (Running = `WorkerState::current` on W0, Resumed = on W1) |
| `switchPhase` | `NoSuspend` / `InFlight` / `Saved` | physical save state of `fiber->ctx` |
| `suspendPending` | BOOLEAN | `WorkerState::suspend_switch_pending` (victim W0's) |
| `ticketLocation` | `None` / `VictimLocal` / `ThiefLocal` | the one runnable ticket on `W0.local_runnable` / `W1.local_runnable` |
| `sawStealRefusal` | BOOLEAN (HISTORY ghost) | records that a thief observed (ticket@victim ∧ pending) and refused |

Deliberately NOT modeled (no contribution to any guard/property in this
scenario): `fiber_owner_` (redundant with ticket movement for a single fiber),
`execWorker` (encoded in `fiberState`), `waitOwner`/WaitReg routing (constant
W0 here — both production routing disciplines agree at suspend time).

## Fused vs split

**Split (the point of this suite — do not re-fuse):**

- `CommitSuspend` (logical commit, one G CS) /
  `SaveContext` (physical save, thread-local) /
  `ClearPending` (authority withdrawal, thread-local, post-save).
  E8's atomic `SuspendFiber` fuses all three; the wake-before-save transient
  lives exactly in the kept-open gap.
- The `SaveContext`→`ClearPending` gap (`Saved ∧ pending`) is also real and
  observable (a thief refuses then too — harmless over-protection); preserved,
  not optimized away.

**Fused (with C++ justification):**

- `CommitSuspend` fuses raise+`make_waiting`: they are one `global_mtx_`
  critical section in C++, and every resolver/thief needs the same mutex, so
  no interleaving exists inside it. The H2 negative models the OLD protocol
  where they were NOT fused.
- `Resolve` fuses `make_runnable` + ticket publication (one G CS).
- `StealTicket` fuses queue move + owner transfer (one G CS).

## Properties

| Invariant | Statement | Meaning |
| --------- | --------- | ------- |
| `InvNoResumeBeforeContextSaved` (core) | `Resumed ⇒ Saved` | ONLY unsafe resume is forbidden — pre-save ticket MOVEMENT is not claimed impossible (deliberately not over-strengthened) |
| `InvUnsavedSuspensionProtected` | `(InFlight ∧ fiberState ∈ {Waiting,Runnable}) ⇒ pending` | protocol authority: the protection flag covers the whole unsaved window |
| `InvTicketImpliesRunnable` | `ticket ≠ None ⇒ Runnable` | E7-T2 one-ticket structure survives steal/consume |
| `InvPendingImpliesCommitted` | `pending ⇒ switchPhase ≠ NoSuspend` | the flag only exists inside a real suspension cycle |

## Negative controls (cfg-boolean flips; one defect per cfg; d1_uring_poison precedent)

| cfg | flip | defect | expected NAMED violation | specificity (must PASS) |
| --- | ---- | ------ | ------------------------ | ---------------------- |
| `…NegIgnorePendingSteal` | `GuardStealWithPending = FALSE` | H1: `try_steal` ignores the flag | `InvNoResumeBeforeContextSaved` | the other 3 laws |
| `…NegRaiseTooLate` | `RaiseBeforeVisibility = FALSE` | H2: old P1-1 late raise | `InvUnsavedSuspensionProtected` | ticket/pending-binding laws |
| `…NegRaiseTooLateChain` | same | same, chain completeness | `InvNoResumeBeforeContextSaved` | — |
| `…NegClearTooEarly` | `ClearOnlyAfterSave = FALSE` | H3: clear before save | `InvNoResumeBeforeContextSaved` | ticket/pending-binding laws |

Co-victim exclusions (by design, NOT collateral): `InvNoResumeBeforeContextSaved`
is an expected co-victim of the NEG-SS2 defect chain (proven separately by the
chain cfg), and `InvUnsavedSuspensionProtected` is an expected co-victim of
NEG-SS3 (clearing the protection while unsaved trivially breaks the authority
law too). Each specificity cfg checks only the laws the one-rule defect must
NOT break — this is the PR #168 Round-1 anti-collateral discipline.

Every negative produces its violation through the NORMAL action chain
(commit/wake → steal → pop/resume); no magic `unsafe := TRUE` action exists.

## Reachability (witness cfgs; each expects its NoReach* invariant VIOLATED)

- **R1** `…ReachWakeBeforeSave` → `NoReachWakeBeforeSave` violated:
  the wake-before-save transient (Runnable ∧ ticket@victim ∧ InFlight ∧
  pending) — the reason this suite exists.
- **R2** `…ReachStealRefusal` → `NoReachStealRefusal` violated:
  a thief actually attempted the steal during the window and was refused.
- **R3** `…ReachSafeMigration` → `NoReachSafePostSaveMigration` violated:
  save done → authority cleared → ticket stolen → thief resumed (the
  legitimate path the guard protects).

Ghost independence: `sawStealRefusal` only snapshots the independent pre-state
fact (ticket@victim ∧ pending); under a broken protocol it still faithfully
records what a thief would have seen. No action labels itself "safe".

## Results (TLC 2.19 / tla2tools 1.7.4)

- positive: 33 states generated, 14 distinct, depth 8, exhaustive — all 4
  invariants PASS.
- all 3 negatives: exact named CEX; all specificity cfgs PASS; chain cfg CEX.
- all 3 reachability witnesses: named NoReach* CEX.
- adversarial probes (task §19): guard-removal / early-clear / late-raise
  sabotages each produce the exact violation; a collateral-damage sabotage is
  caught by the specificity cfg; removing the R1 path makes the reach gate
  fail closed.

## C++ bridge (existing, no new tests added)

- `tests/select_multi_worker_test.cpp` `suspend_lw_mw_steal_before_switch_excluded`
  (P6-LW-MW): deterministic seam (`select_suspend_before_switch`) parks the
  fiber in the window; resolver publishes; a second worker's steal attempts
  are refused; mechanical counts prove single-entry/single-resume.
- `tests/event_primitive_test.cpp` I47-F1 case: snapshot asserts the R1
  transient itself (`fiber_state == runnable` ∧ ticket on W0's queue ∧
  `owner_suspend_switch_pending == true`), deterministically.

## Weak-memory boundary

This is a **sequentially-consistent protocol abstraction** of the as-built
program order and `global_mtx_` serialization boundaries. TLC does **NOT**
prove the C++ release/acquire implementation. The C++ memory-model argument —
raise `store(true, release)` (:1331), clear `store(false, release)` (:1308),
thief `load(acquire)` (:1931); the save stores and the clear are same-thread
program-ordered so the release/acquire pair carries save-visibility to the
thief — is a separate implementation-level obligation, not a TLC verdict.

## Non-goals

- No fairness/liveness (safety-only; if WF/SF were needed for the core
  conclusion the boundary would be wrong).
- No second fiber, no WaitQueue capacity, no timers/backends/select/cancel
  semantics; the resolver is abstracted as legitimate resolve authority.
- No owner-side pop/resume path: W0 reaches its worker loop only AFTER the
  clear (control-flow safe, not the flag's job); including it would blur the
  `Resumed` = on-thief encoding.
- Not an implementation verification of the C++ (abstract protocol model).
- MODEL-007(b)–(e) remain outside (umbrella #171).

## Claim vocabulary

Allowed: AS-BUILT MODELED (focused protocol) · FORMAL PROPERTY PASS ·
NEGATIVE-CONTROL SENSITIVE · REACHABILITY WITNESSED.

NOT allowed: "C++ formally verified" · "race impossible in all
implementations" · "memory-model verified".
