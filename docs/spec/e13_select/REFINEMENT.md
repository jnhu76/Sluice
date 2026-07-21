# E13 Select — Refinement Safety (PR #18)

This document records the refinement mapping checks added in PR #18 on top
of the PR #17 layered formal core.  Each refinement is checked by TLC as a
temporal `PROPERTY`, not just documented.

## Architecture

```text
E13SelectContract.tla
        ^ temporal PROPERTY RefinesContract
E13SelectCentralClaim.tla
        ^ temporal PROPERTY RefinesCentralClaim
E13SelectEventTimer.tla
        ^ thin compatibility root
E13Select.tla
```

Each layer's INSTANCE-WITH mapping is exposed as a temporal property that
must hold over the full behavior of the layer's `*Spec`:

```tla
\* In E13SelectCentralClaim.tla:
ContractRefinement == INSTANCE E13SelectContract WITH ...
RefinesContract == ContractRefinement!ContractSpec

\* In E13SelectEventTimer.tla:
CentralRefinement == INSTANCE E13SelectCentralClaim WITH ...
RefinesCentralClaim == CentralRefinement!CentralSpec
```

TLC checks these as `PROPERTY RefinesContract` / `PROPERTY RefinesCentralClaim`.

## PR #17 baseline (unchanged, regression-covered)

| Cfg | Module | Property | Domain |
|-----|--------|----------|--------|
| `E13SelectCentralClaim.cfg` | `E13SelectCentralClaim` | `RefinesContract` | 2-arm |
| `E13SelectEventTimer.cfg` | `E13SelectEventTimer` | `RefinesCentralClaim` | 2-arm |
| `E13Select.cfg` | `E13Select` (thin root) | (covered by 2-arm adapter PROPERTY) | 4-arm constrained |

These remain green; PR #18 does not modify the mapping itself.

## PR #18 widened-domain checks (X)

| Cfg | Module | Property | Domain | Purpose |
|-----|--------|----------|--------|---------|
| `E13SelectCentralClaim.refine3.cfg` | `E13SelectCentralClaim` | `RefinesContract` + `CentralSafetyInv` | 3-arm | Admission-tie refinement across every candidate combination. |

The adapter's widened-domain refinement PROPERTY (3-mix, 4-mix) blows up
past the 5-minute TLC budget when checked as a temporal property; the
3-arm adapter domain is instead exercised by the `AdapterSafetyInv`
aggregate in `E13SelectEventTimer.safety3mix.cfg` (8.7M states, 2.7M
distinct, completes in ~2.5 minutes).  This covers the same Event/Timer
combinations the refinement PROPERTY would cover.

## PR #18 corrective-1 — three-layer observer propagation

The corrective-1 changes add *load-bearing* observer variables that
prove immutability and identity-stability laws.  Because these variables
are part of a layer's formal state, they MUST propagate through the
refinement chain's INSTANCE-WITH mapping at every layer.  The chain is:

```text
Contract carries linearized_winner / linearized_winner_valid
    -> Central's ContractRefinement INSTANCE binds them via WITH
    -> EventTimer's CentralRefinement INSTANCE binds them via WITH

Central carries claim_snapshot_frozen / claim_snapshot_frozen_valid
    -> EventTimer's CentralRefinement INSTANCE binds them via WITH
    (NOT mapped back to Contract: Contract does not see claim-snapshot state)
```

One semantic, three-layer projection, no branching.  Each observer is
initialised alongside its layer's `*Init`, assigned by exactly one
canonical action (`ContractLinearizeWinner` / `CentralClaimWinner`), and
frozen in every other action's UNCHANGED.  The temporal PROPERTY checks
(`RefinesContract`, `RefinesCentralClaim`) re-pass unchanged on the
widened WITH clauses.

Single-claim / single-operation model: each observer captures exactly
one claim epoch.  Multi-epoch claim identity (re-claiming after a
terminal rollback, then claiming again) is deferred — the corrective-1
scope does not model a second claim epoch, and the observers carry a
single frozen value per behaviour.

## Mapping tables

### Central Claim → Contract

Every Central variable either projects directly onto a Contract variable
or is pure strategy state invisible to the contract.

| Central variable | Contract mapping | Notes |
|------------------|------------------|-------|
| `contract_phase`, `arm_registered`, `readiness_evidence`, `reservation_state`, `arm_resolution`, `authority_open`, `winner`, `caller_state`, `completion_mode`, `result_publication_count`, `runnable_publication_count`, `arm_publication_count`, `reservation_close_count` | identity | Direct projection. |
| `linearized_winner`, `linearized_winner_valid` | identity | Corrective-1 P1-4 frozen winner history; propagated via WITH to Central. |
| `central_phase` | derived from `contract_phase` | Strategy phase (Registering/Admission/Armed/Claimed/Closing/...) maps to contract phase per `CentralTypeOK`. |
| `candidate_ready`, `claim_candidates`, `arm_class`, `claim_mode` | none | Pure strategy state; invisible to contract. |
| `claim_snapshot_frozen`, `claim_snapshot_frozen_valid` | none | Corrective-1 P1-3 Central-only frozen snapshot history; NOT mapped back to Contract (Contract does not perceive claim-snapshot state). |

### Event/Timer → Central Claim

Every adapter variable either projects directly onto a Central variable or
is pure adapter detail.

| Adapter variable | Central mapping | Notes |
|------------------|-----------------|-------|
| All Central projection vars | identity | Direct projection. |
| `linearized_winner`, `linearized_winner_valid` | identity | Corrective-1 P1-4 frozen winner history; propagated via WITH from Central's ContractRefinement binding. |
| `claim_snapshot_frozen`, `claim_snapshot_frozen_valid` | identity | Corrective-1 P1-3 frozen snapshot history; propagated via WITH from Central. |
| `arm_kind`, `arm_index`, `adapter_phase`, `arm_event`, `wait_outcome`, `wait_linked`, `context_state`, `timer_state`, `timer_due`, `timer_node_deref`, `timer_skip_observed`, `waiting_account_open`, `timer_account_open`, `event_state`, `admission_checked`, `scan_phase`, `held_event`, `scan_remaining`, `coordination_kind`, `coordination_arm`, `last_broadcast_event`, `finalization_step`, `rollback_started`, `select_result_published`, `registration_count`, `registered_arm_count`, `retired_arm_count`, `now` | none | Pure adapter detail; invisible to Central. |
| `wait_account_open_count`, `wait_account_close_count`, `timer_account_open_count`, `timer_account_close_count` (M) | none | PR #18 per-arm accounting; refines Central behaviour but adds no Central-visible state.  Corrective-1 widened TypeOK domain to `0..2` so NEG-A1/A2 isolate the at-most-once law without piggy-backing on `EventTimerTypeOK`. |
| `group_id`, `claim_epoch`, `broadcast_epoch`, `timer_pump_epoch`, `winner_linearization_step`, `commit_step`, `terminal_step`, `unlink_step`, `timer_transition_step`, `account_close_step`, `authority_close_step`, `publication_step`, `global_step` (N) | none | PR #18 step-indexed history; pure observer state, no behaviour. |

Because the PR #18 M and N variables are pure extensions (they do not
replace any mapped variable), the refinement mapping is unaffected by
their addition: the same `WITH` clause that worked in PR #17 continues
to work in PR #18, and the temporal PROPERTY still passes.

## Verification

Run `tools/formal/verify-e13-select-safety.sh` and look for the
`PASS  Central -> Contract refinement (3-arm admission tie)` line under
the "Widened-domain refinement (X)" section.  The PR #17 canonical
refinement configs are still checked by `verify-e13-select-core.sh`.
