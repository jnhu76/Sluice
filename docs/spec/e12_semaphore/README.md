# E12-B Semaphore — TLA+ Formal Model (safety)

> sluice::async::Semaphore (first-scope acquire-one / release-one),
> **E12-B-SEMAPHORE-PREPARATION-CORRECTIVE-1**.
>
> Status: **E12-B-PREPARATION-CORRECTIVE-1: COMPLETE — REAUDIT-REQUIRED —
> IMPLEMENTATION BLOCKED.** This model is the formal half of the corrective;
> the authority document is
> [`docs/e12-semaphore.md`](../../e12-semaphore.md).

This directory contains the **safety-only** TLA+ / TLC formal model for the
E12-B Semaphore preparation corrective. It proves the permit-accounting,
FIFO-grant, overflow-non-mutation, admission-closure, and deadline-precedence
properties of the first-scope design, and provides seven negative models that
each fail a single named invariant for the intended reason.

## The load-bearing E12-B question

> Can the first-scope Semaphore's permit supply / queued demand / release
> disposition / FIFO grant / deadline precedence be modelled so that every
> property is a TLC-checkable state invariant (no primed variables in
> invariants), with no `available_--` / refund model and no spurious liveness?

**Answer: YES** — by (a) modelling release as three atomic dispositions
(transfer / store / overflow-reject), (b) carrying minimal ghost/history
evidence so transition properties become state predicates, (c) latching
admission evidence (`admissionSawPermit` / `admissionSawDue`) atomically with
the resolution so `InvPermitFirstDeadline` is prime-free, (d) carrying an
explicit FIFO `queue : Seq(Node)` so `InvFIFOGrant` has a real ordering
authority, and (e) making the model safety-only (no release liveness).

## Files

| File | Purpose |
|------|---------|
| `E12Semaphore.tla` | Correct safety model (atomic release, FIFO queue, latched admission evidence, history-backed transition invariants) |
| `E12Semaphore.cfg` | Safety config (SPECIFICATION Spec, INVARIANT Inv) |
| `E12SemNeg1AdmissionClosure.tla/.cfg` | NEG-SEM-1: admission closure failure → `InvAdmissionClosure` |
| `E12SemNeg2ReleaseLoss.tla/.cfg` | NEG-SEM-2: release permit lost → `InvPermitConservation` |
| `E12SemNeg3DoubleStore.tla/.cfg` | NEG-SEM-3: one accepted release stores two permits → `InvPermitConservation` |
| `E12SemNeg4NonFIFOGrant.tla/.cfg` | NEG-SEM-4: grant a non-FIFO-head eligible node → `InvFIFOGrant` |
| `E12SemNeg5OverflowMutation.tla/.cfg` | NEG-SEM-5: overflow mutates `available` → `InvOverflowNonMutation` |
| `E12SemNeg6IdlePermitEligibleWaiter.tla/.cfg` | NEG-SEM-6: store a permit while an eligible waiter is queued → `InvNoIdlePermitWithEligibleWaiter` |
| `E12SemNeg7DeadlinePrecedence.tla/.cfg` | NEG-SEM-7: admissible permit + due deadline resolves Expired → `InvPermitFirstDeadline` |
| `_gen_neg.py` | Build aid that generates NEG-2..7 from `E12Semaphore.tla` by a single-rule substitution (NEG-1 is hand-written as the validated template). Not part of the formal gate. |

## Model domain

`Nodes = {N0, N1, N2}`, exhaustive. `MaxPermits = 2`. Three epochs suffice to
express the non-FIFO grant topology (two eligible waiters W1 before W2) and
the deadline-precedence case, while keeping the state space checkable.

## State dimensions

| Variable | Type | Meaning |
|----------|------|---------|
| `available` | `0..MaxPermits` | stored, unassigned permits (the atomic counter) |
| `queue` | `Seq(Node)` | the FIFO wait queue (explicit ordering authority) |
| `nodeState` | `[Node -> NodeState]` | per-epoch lifecycle (Detached/Registered/Woken/Cancelled/Expired) |
| `resolvedCount` | `[Node -> 0..1]` | terminal resolution count (single-resolution) |
| `wakeDispatched` | `Int` | total runnable publications (single-publication) |
| `admissionPhase` | `[Node -> AdmissionPhase]` | NoAdmission/AdmissionOpen/Suspended |
| `deadlineDue` | `[Node -> BOOLEAN]` | was the deadline already due at admission? |
| `admissionSawPermit` | `[Node -> BOOLEAN]` | latched: admission observed an admissible permit |
| `admissionSawDue` | `[Node -> BOOLEAN]` | latched: admission observed a due deadline |
| `acceptedReleaseCount` | `Nat` | total release() calls that contributed a permit |
| `acquiredCount` | `Nat` | total permits acquired (immediate acquire OR queued grant) |
| `lastAction` | `ActionKind` | HISTORY: the action that produced the current state |
| `lastGrantedNode` | `Node ∪ {NoNode}` | HISTORY: node granted by the last ReleaseTransfer |
| `expectedFIFOHead` | `Node ∪ {NoNode}` | HISTORY: eligible FIFO head latched BEFORE queue mutation |
| `preAvailable` | `0..MaxPermits` | HISTORY: `available` before the last action |
| `preAcceptedReleaseCount` | `Nat` | HISTORY |
| `preAcquiredCount` | `Nat` | HISTORY |

The `lastAction` / `lastGrantedNode` / `expectedFIFOHead` / `pre*` variables
are **ghost/history evidence**: they exist solely so that transition
properties (release disposition, FIFO grant, overflow non-mutation) are
expressible as pure state invariants. They are NOT production fields.

## Actions

| Action | Production refinement |
|--------|----------------------|
| `AcquireRegister(n)` | admission epoch opener (register into the Semaphore wait queue; environment chooses the deadline-due bit) |
| `AcquireImmediate(n)` | admission: `available>0` + no eligible waiter → resolve Woken inline, consume a permit (no barging) |
| `AcquireUntilImmediate(n)` | permit-first deadline admission (Woken; latches `admissionSawPermit`+`admissionSawDue`) |
| `AcquireUntilDueNoPermit(n)` | no-permit + deadline-due admission (Expired; the other half of precedence) |
| `AcquireSuspend(n)` | register/recheck/suspend-commit window; rechecks `available` and consumes if a permit appeared |
| `ReleaseTransfer` | release() with an eligible FIFO head → transfer the pending permit (no `available` mutation) |
| `ReleaseStore` | release() with no eligible waiter, `available<max` → store the pending permit |
| `ReleaseOverflow` | release() at `available==max`, no eligible waiter → reject, no mutation |
| `ResolveCancel(n)` | `Scheduler::cancel_wait` (resolve_(Cancelled) + unlink) |

## Correct properties

| Property | Formal name | Meaning |
|----------|-------------|---------|
| P1 | `InvPermitConservation` | `available + acquiredCount == acceptedReleaseCount` (initialPermits=0 in the model) |
| - | `InvPermitBounds` | `0 ≤ available ≤ MaxPermits`, `MaxPermits > 0` |
| P8 | `InvQueueWellFormed` | queue is duplicate-free; only Registered admissible nodes are queued; no terminal/Detached node is queued |
| P9 | `InvSingleResolution` | each wait epoch resolves at most once (inherited E10) |
| P10 | `InvSinglePublication` | one winning epoch → at most one runnable publication |
| - | `InvGrantCommitCoupling` | `acquiredCount == Cardinality({n : nodeState[n]="Woken"})` (holds because each Node is one wait epoch) |
| P3 | `InvFIFOGrant` | the last release transfer granted the FIFO head (history-backed) |
| P6 | `InvAdmissionClosure` | a wait that observed an admissible permit at admission resolved Woken |
| P4 | `InvOverflowNonMutation` | the last overflow release changed no authoritative counter (history-backed) |
| P5 | `InvNoIdlePermitWithEligibleWaiter` | `available > 0 ⇒ EligibleSet = {}` (the stable-state invariant) |
| P2 | `InvReleaseDisposition` | every accepted release did exactly one of transfer / store (history-backed) |
| P7 | `InvPermitFirstDeadline` | a wait that observed BOTH an admissible permit AND a due deadline resolved Woken (latched-evidence-backed; prime-free) |

## Negative models

| NEG | Single broken rule | Counterexample | Expected property | TLC result |
|-----|-------------------|----------------|-------------------|------------|
| NEG-1 | `AcquireSuspend` commits Suspended despite `available>0` (drops recheck) and latches `admissionSawPermit` | Registered+Suspended+`admissionSawPermit`=TRUE | `InvAdmissionClosure` | CEX (violated) |
| NEG-2 | `ReleaseTransfer` drops `acquiredCount++` (grant permit lost) | `acceptedReleaseCount` grew but `acquiredCount` did not | `InvPermitConservation` | CEX (violated) |
| NEG-3 | `ReleaseStore` increments `available` by 2 for one accepted release | one `ReleaseStore` (`acceptedReleaseCount`: 0→1) with `available`: 0→2 | `InvPermitConservation` | CEX (violated) |
| NEG-4 | `ReleaseTransfer` grants an eligible node that is NOT the FIFO head | `lastGrantedNode # expectedFIFOHead` | `InvFIFOGrant` | CEX (violated) |
| NEG-5 | `ReleaseOverflow` mutates `available++` | `available = preAvailable+1` under `ReleaseOverflow` | `InvOverflowNonMutation` | CEX (violated) |
| NEG-6 | `ReleaseStore` drops the `EligibleSet={}` guard | `available>0` while an eligible waiter is queued | `InvNoIdlePermitWithEligibleWaiter` | CEX (violated) |
| NEG-7 | `AcquireUntilImmediate` latches evidence but resolves Expired | `admissionSawPermit ∧ admissionSawDue` with `nodeState=Expired` | `InvPermitFirstDeadline` | CEX (violated) |

Each negative model is a self-contained module identical to `E12Semaphore`
except for ONE rule in the named action. NEG-3 is scoped to a single defect
(double-store) and expects only `InvPermitConservation`; a separate
double-publication negative is not modelled.

## Design notes (refinement / scope)

- **Each modeled Node denotes ONE wait epoch**, not a reusable production
  `WaitNode` object across multiple epochs. This is what makes
  `InvGrantCommitCoupling` (`acquiredCount == |{Woken}|`) hold as a state
  invariant; epoch reuse would require a separate `wokenEpochCount` instead.
- **`Expired` is in the `nodeState` domain** so the model can prove BOTH
  halves of deadline precedence (permit+due → Woken via
  `AcquireUntilImmediate`; no-permit+due → Expired via
  `AcquireUntilDueNoPermit`). In production, Expired's authority is the **E11
  timer/expire seam** (`Scheduler::expire_wait` /
  `WaitQueue::expire_locked`); the E12-B model exercises it directly only to
  close the precedence proof. This is a refinement choice, not a claim that
  E12-B owns the expire resolver.
- **Transition properties as state invariants (ghost evidence).**
  `InvFIFOGrant`, `InvOverflowNonMutation`, and `InvReleaseDisposition` are
  properties of a single action's before/after state. They are expressed as
  state predicates over the `lastAction` / `lastGrantedNode` /
  `expectedFIFOHead` / `pre*` history variables, written by every action. No
  invariant uses a primed variable (actions do; invariants do not).
- **Overflow "no mutation" wording.** `ReleaseOverflow` updates only the
  ghost/history fields (`lastAction`, `pre*`). The accepted meaning is: no
  authoritative Semaphore state, permit counter, queue, node outcome, or
  accounting counter is mutated; formal ghost/history evidence may be updated
  solely to verify the non-mutation property.
- **Safety-only.** There is no liveness `.cfg`. The prior draft's
  "Registered+Suspended+available>0 eventually resolves" property was removed:
  its premise is unreachable under `InvNoIdlePermitWithEligibleWaiter`
  (vacuous), and release is modelled as an atomic external action (external
  future-release fairness would be unjustified). Genuine release liveness would
  require explicit multi-step pending-release state (`releaseRequested` /
  `wakePublicationPending` / `schedulerDispatchPending`); that is deferred.
- **Conclusion A (Scheduler seam).** No negative model assumes a linked
  eligible FIFO head can lose its `resolve_(Woken)` CAS — see
  `docs/e12-semaphore.md` §5 for the production lock-protocol proof that
  `wake_wait_one_locked` returns `nullptr` only when the queue is empty.

## Results (actual TLC execution)

TLC runtime version (exact runtime line, reported by TLC itself):
`TLC2 Version 2026.07.09.134028 (rev: 227f61b)`.

TLA+ tools release tag: `not associated with a verified release tag` (a 2026
development build; the runtime line above is the exact build identifier).

| Model | Result | States generated | Distinct states |
|-------|--------|-----------------|-----------------|
| E12Semaphore safety (all 12 invariants) | PASS | 58332 | 12214 |
| NEG-1 AdmissionClosure | CEX (`InvAdmissionClosure`) | 116 | 101 |
| NEG-2 ReleaseLoss | CEX (`InvPermitConservation`) | 108 | 95 |
| NEG-3 DoubleStore | CEX (`InvPermitConservation`) | 9 | 8 |
| NEG-4 NonFIFOGrant | CEX (`InvFIFOGrant`) | 2761 | 1284 |
| NEG-5 OverflowMutation | CEX (`InvOverflowNonMutation`) | 496 | 346 |
| NEG-6 IdlePermitEligibleWaiter | CEX (`InvNoIdlePermitWithEligibleWaiter`) | 109 | 96 |
| NEG-7 DeadlinePrecedence | CEX (`InvPermitFirstDeadline`) | 181 | 157 |

Depth is not reported here; TLC's complete-state-graph search depth for the
correct safety model is 13. The state counts above are the concrete
`states generated` / `distinct states found` figures TLC reports.

Reproduce: `scripts/verify-e12-semaphore-formal.sh`.

## What this model does NOT cover

- E11 timer/expiry state machinery (production authority is E11; the model
  reaches Expired directly via `AcquireUntilDueNoPermit` only to close the
  precedence proof).
- MW admission/steal (E7–E9), Fiber lifecycle/asm (E2/E4).
- Backends / io_uring / timerfd / networking.
- `acquire(N)` / `release(N)` (multi-permit; deferred).
- Liveness / starvation freedom (safety-only model).
- E12-C Mutex / E12-D Condition / E12-E Queue / E12-F RwLock / Select
  (separate phases).
