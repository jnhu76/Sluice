# E13 Select Formal Core Layered Design

**Task:** `E13-SELECT-FORMAL-MODEL-CORE-1` (PR #17)

## Decision

PR #17 uses three semantic layers. The abstract Select contract is stable and
contains no Event, Timer, WaitQueue, timer-heap, or global-coordination detail.
Central Claim is one strategy that refines that contract. Event + independent
Timer is the first adapter layer that refines Central Claim.

```text
E13SelectContract
        ^ explicit refinement mapping
E13SelectCentralClaim
        ^ explicit refinement mapping
E13SelectEventTimer
        ^ thin compatibility/root instance
E13Select
```

Central Claim is the selected Candidate A strategy for this PR, not the only
possible Select implementation strategy. `CandidateReady`, the claim-time
candidate snapshot, lowest-index tie-break, group claim, and Winner/Loser
classification are strategy state and therefore do not appear in the contract.

## Contract interface

The contract exposes only stable Select semantics:

- arms may present offered readiness or reversible reservation evidence;
- exactly one successful operation winner is linearized;
- irreversible external effects are forbidden before winner linearization;
- reversible pre-claim reservation is permitted only when loser abort fully
  restores primitive state and commit-or-release closes the reservation exactly
  once;
- the winner performs one irreversible commit;
- non-winners abort/release without publishing;
- every arm authority closes before successful completion or rollback teardown;
- result publication is exactly one for successful completion;
- runnable publication is zero for inline completion and one for suspended
  completion.

Registration rollback is an unsuccessful terminal path with no winner, result,
or runnable publication.

### Registration rollback domain (corrective boundary)

Rollback is scoped narrowly to **registration rollback**: registration or
allocation failing *before registration completes, before caller suspension,
and before winner linearization*. It is enabled only while
`contract_phase = "Building"`, `winner = NoArm`, and `caller_state = "Running"`
(Contract `ContractRollbackEnabledDomain`), and this restriction propagates
through both refinements (Central `central_phase = "Registering"`, adapter
additionally forbids any arm still mid-registration).

It must be impossible to begin rollback after `FinishRegistration`,
`SuspendCaller`, a winner claim, or a winner commit. The terminal caller state
is therefore pinned: `Rollback`/`Aborted` require `caller_state = "Running"`,
and `Aborted`/`Destroyed` forbid `caller_state = "Waiting"`.

Registration rollback does **not** model cancellation after caller suspension,
shutdown cancellation, user-requested Select cancellation, or timeout of the
whole Select operation. Those post-suspension cancellation paths require a real
wake/publication protocol that PR #17 deliberately does not contain; reusing
registration rollback to fake them is forbidden. They remain deferred to
PR #18.

## Central Claim strategy

Central Claim maps its concrete state to the contract with a top-level TLA+
`INSTANCE ... WITH` refinement mapping. Its actions refine contract actions or
stutter at the contract seam. It adds only strategy state:

- `CandidateReady` evidence;
- the candidate set latched at claim time;
- lowest-index selection from that snapshot;
- inline versus suspended claim origin;
- Winner/Loser classification.

The mapping is checked by TLC as a temporal `PROPERTY`, not asserted only in
documentation.

## Event/Timer adapters

The concrete adapter module adds:

- real Event identities and `arm_event` mapping;
- two-phase Event broadcast with one held Event queue identity, a finite scan,
  lock release, then group processing;
- independent TimerRegistration state and an explicit Active-before-dereference
  timer-pump entry;
- an observable stale-pump skip that does not increment `timer_node_deref`;
- WaitNode outcome/link state and typed context;
- per-arm waiting and timer accounting;
- ordered Event/Timer winner and loser finalization;
- concrete R1-R12 causal reachability predicates backed by action history.

The adapter module maps to Central Claim through another top-level
`INSTANCE ... WITH` and TLC temporal property. `E13Select.tla` remains a thin
root so existing scene configurations can keep their module name.

## Scope

This design adds no alternative strategy, additional arm type, negative model,
liveness/fairness gate, production C++, public API, production test, or build
policy change. Full E13 concrete safety/negative-model closure remains PR #18.
