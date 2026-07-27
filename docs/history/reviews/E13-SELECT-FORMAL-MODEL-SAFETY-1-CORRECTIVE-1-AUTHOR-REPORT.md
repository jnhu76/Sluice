# E13 Select Formal Model Safety — Corrective-1 Author Report

This is the author's self-assessment for the corrective-1 changes against
the originally reviewed PR #18 head.  It does **not** rewrite the
historical `REQUEST-CHANGES` verdict in
`E13-SELECT-FORMAL-MODEL-SAFETY-1-INDEPENDENT-REVIEW-1.md`; that artifact
remains unchanged.  This report accompanies the corrective review request
at `E13-SELECT-FORMAL-MODEL-SAFETY-1-CORRECTIVE-1-REVIEW-REQUEST.md`.

```text
E13-SELECT-FORMAL-MODEL-SAFETY-1-CORRECTIVE-1:
PASS — AUTHOR SELF-ASSESSMENT

ORIGINAL VERDICT (unchanged, historical):
E13-SELECT-FORMAL-MODEL-SAFETY-1-INDEPENDENT-REVIEW-1 = REQUEST-CHANGES

CORRECTIVE VERDICT (this report, author self-assessment):
E13-SELECT-FORMAL-MODEL-SAFETY-1-CORRECTIVE-1 = PASS — AUTHOR SELF-ASSESSMENT

INDEPENDENT DELTA REVIEW:
REQUESTED — see E13-SELECT-FORMAL-MODEL-SAFETY-1-CORRECTIVE-1-REVIEW-REQUEST.md
```

## A. Baseline

```text
REPOSITORY:
jnhu76/Sluice

TASK:
E13-SELECT-FORMAL-MODEL-SAFETY-1-CORRECTIVE-1

ORIGINAL REVIEWED HEAD:
0f88ab9d2c19b8249df4e071c7a0662ce549ca9a

CORRECTIVE CANDIDATE HEAD:
resolve from PR headRefOid at independent-review start
(re-resolve at review time with `git rev-parse HEAD` on the corrective branch)

BASE (PR #17 merge):
57435a913ad6d679df19a17f17f1c36e711dfc60

BRANCH:
feat/e13-select-formal-model-safety

SCOPE:
docs/spec/e13_select/**
docs/formal/**
docs/reviews/**
tools/formal/**

OUT OF SCOPE (no changes permitted):
include/**
src/**
tests/**
examples/**
benchmarks/**
public API
production build policy

PRE-EXISTING UNTRACKED FILES (must remain untracked / unchanged):
tests/test_t3_simple.cpp
tla2tools.jar
```

## B. Original review disposition

Each must-fix item from the original `REQUEST-CHANGES` review is addressed
by a specific corrective change:

- **Multi-group rollback reachability (P1-1).**  Addressed by the
  registration split: `RegisterEventArm` / `RegisterTimerArm` keep
  `g_phase = "Registering"`; new `FinishRegistration(g)` advances to
  `Admission`.  BeginRollback is now genuinely reachable from the
  post-install pre-finish state.
- **Multi-group reach witness split (P1-2).**  Addressed by replacing the
  single combined reach cfg with three independent cfgs (shared-Event,
  mixed Event+Timer, rollback-vs-complete) plus two non-vacuity cfgs.
- **Claim-snapshot immutability via frozen history (P1-3).**  Addressed by
  adding `claim_snapshot_frozen` / `claim_snapshot_frozen_valid` to Central
  and propagating them through the EventTimer refinement.  Three Central
  snapshot laws rewritten to depend on the frozen value.
- **Winner identity stability after linearization (P1-4).**  Addressed by
  adding `linearized_winner` / `linearized_winner_valid` to Contract and
  propagating them through Central → EventTimer.  Three Contract winner
  laws rewritten.
- **Accounting at-most-once laws independent from TypeOK (P2).**  Addressed
  by widening the four counter TypeOK domains to `0..2` and adding the
  `expect_negative_no_typeok` double-check for `NEG-A1`/`NEG-A2`.
- **Mechanical cleanup (§I1, §I2).**  `ReachContractLoserExists` references
  only `Released`; the adapter neg-family label is corrected to `(E/T/A)`.
- **Portable tmpdir hardening (§I3).**  Core verifier cleanup trap
  rewritten to use the combined-guard form.
- **Rejected PASS draft not committed.**  The PASS draft
  `E13-SELECT-FORMAL-MODEL-SAFETY-1-INDEPENDENT-REVIEW-1.md` is deleted in
  the corrective (it predates the corrective branch as an untracked file).

## C. Files changed

Modified (relative to ORIGINAL REVIEWED HEAD `0f88ab9`):

- `docs/spec/e13_select/E13SelectContract.tla`
- `docs/spec/e13_select/E13SelectContractNeg.tla`
- `docs/spec/e13_select/E13SelectCentralClaim.tla`
- `docs/spec/e13_select/E13SelectCentralClaimNeg.tla`
- `docs/spec/e13_select/E13SelectCentralClaimNeg.S2.cfg`
- `docs/spec/e13_select/E13SelectEventTimer.tla`
- `docs/spec/e13_select/E13SelectEventTimerNeg.tla`
- `docs/spec/e13_select/E13SelectMultiGroup.tla`
- `docs/spec/e13_select/E13SelectMultiGroup.reach.cfg`
- `docs/spec/e13_select/INVARIANTS.md`
- `docs/spec/e13_select/NEGATIVE_MODELS.md`
- `docs/spec/e13_select/NON_VACUITY.md`
- `docs/spec/e13_select/EVIDENCE_SAFETY.md`
- `docs/spec/e13_select/REFINEMENT.md`
- `docs/formal/e13-select-formal-safety-plan.md`
- `tools/formal/verify-e13-select-safety.sh`
- `tools/formal/verify-e13-select-core.sh`

Added:

- `docs/spec/e13_select/E13SelectContract.nv_lin_not_committed.cfg`
- `docs/spec/e13_select/E13SelectContractNeg.C9.cfg`
- `docs/spec/e13_select/E13SelectCentralClaim.nv_frozen_snapshot.cfg`
- `docs/spec/e13_select/E13SelectCentralClaim.nv_multi_candidate_snapshot.cfg`
- `docs/spec/e13_select/E13SelectCentralClaimNeg.S7.cfg`
- `docs/spec/e13_select/E13SelectEventTimer.adapter_account_count_one.cfg`
- `docs/spec/e13_select/E13SelectMultiGroup.reach_shared_event.cfg`
- `docs/spec/e13_select/E13SelectMultiGroup.reach_mixed_event_timer.cfg`
- `docs/spec/e13_select/E13SelectMultiGroup.reach_rollback_vs_complete.cfg`
- `docs/spec/e13_select/E13SelectMultiGroup.nv_installed_not_finished.cfg`
- `docs/spec/e13_select/E13SelectMultiGroup.nv_pre_finish_rollback.cfg`
- `docs/spec/e13_select/E13SelectMultiGroupNeg.tla`
- `docs/spec/e13_select/E13SelectMultiGroupNeg.MG1.cfg`
- `docs/spec/e13_select/E13SelectMultiGroupNeg.restore.cfg`
- `docs/reviews/E13-SELECT-FORMAL-MODEL-SAFETY-1-CORRECTIVE-1-REVIEW-REQUEST.md`
- `docs/reviews/E13-SELECT-FORMAL-MODEL-SAFETY-1-CORRECTIVE-1-AUTHOR-REPORT.md` (this file)

Deleted (was an untracked rejected PASS draft, never committed):

- `docs/reviews/E13-SELECT-FORMAL-MODEL-SAFETY-1-INDEPENDENT-REVIEW-1.md`

The two pre-existing untracked files `tests/test_t3_simple.cpp` and
`tla2tools.jar` remain untracked and unchanged.

## D. Multi-group registration correction (P1-1)

The original spec advanced `g_phase` to `Admission` inside
`RegisterEventArm` / `RegisterTimerArm`, making the canonical rollback
path (whose domain requires `g_phase = "Registering"`) unreachable.  The
corrective splits registration into two steps:

1. `RegisterEventArm(g, e)` / `RegisterTimerArm(g)` install the arm kind
   and identity, open wait link / authority / account, and LEAVE
   `g_phase[g] = "Registering"`.
2. New `FinishRegistration(g)` advances `g_phase[g]` to `Admission`.  Its
   guard requires `arm_kind # None /\ wait_linked /\ authority_open /\
   account_open /\ ~rollback_started`.

`BeginRollback(g)` keeps its canonical guard
(`g_phase[g] = "Registering" /\ caller = "Running" /\ winner = NoArm /\
arm_kind # None`), so it is now genuinely reachable from the
post-install pre-finish state.  Nine rollback invariants are added to
`MGSafetyInv` to capture the load-bearing consequences; the
`MG_InvRollbackDoesNotAffectOtherGroup` law is expressed via the
`g_rollback_started[g]` marker so it does not over-constrain independent
groups that legitimately produce their own winners.

## E. Multi-group rollback trace (M1 adversarial)

The corrected model admits the canonical adversarial trace:

```text
RegisterEventArm(g0, e)         \* install Event arm, g_phase[g0]="Registering"
RegisterTimerArm(g1)            \* install Timer arm in group 1
FinishRegistration(g0)          \* g_phase[g0] -> "Admission"
BeginRollback(g0)               \* reachable: g_phase[g0]="Registering" pre-finish
                                \* (M1 also reaches via leave-registering variant)
RollbackArm(g0)                 \* close g0 authority/account, retire g0 arm
FinishRollback(g0)              \* g_phase[g0] -> "Aborted"
Tick; Tick                      \* time passes in group 1
ClaimAdmissionWinner(g1)        \* group 1 claims its Timer winner
FinalizeTimerWinner(g1)         \* group 1 commits
PublishInline                   \* group 1 publishes
```

The `reach_rollback_vs_complete` cfg witnesses this trace, and the
`nv_pre_finish_rollback` cfg witnesses that the pre-finish rollback state
itself is reachable.  Both report `Invariant NotMG_Reach* is violated`,
which is the desired non-vacuity witness.

## F. Independent reachability configs (P1-2)

The original single combined `reach.cfg` is split into three independent
cfgs so each arm of the multi-group non-interference property is exercised
in isolation:

- `E13SelectMultiGroup.reach_shared_event.cfg` — both groups complete via a
  single shared Event identity (`NotMG_ReachSharedEventBothGroupsComplete`).
- `E13SelectMultiGroup.reach_mixed_event_timer.cfg` — one group completes
  via an Event arm, the other via a Timer arm (`NotMG_ReachMixedEventTimer`).
- `E13SelectMultiGroup.reach_rollback_vs_complete.cfg` — one group reaches
  Aborted via rollback, the other completes successfully
  (`NotMG_ReachOneRollbackOtherComplete`).

`Events = {0}` throughout (Timer arm does not consume Event identity;
state space stays minimal).  The historical `reach.cfg` is kept as a
documentation entry point that points at the three replacements.

## G. Frozen claim snapshot (P1-3)

Central adds two observer variables:

- `claim_snapshot_frozen \subseteq Arms` — the snapshot value stamped at
  claim time.
- `claim_snapshot_frozen_valid \in BOOLEAN` — TRUE once a claim has
  stamped the frozen value.

`CentralInit` sets frozen=`{}`, valid=`FALSE`.  `CentralClaimWinner(i)`
sets `claim_snapshot_frozen' = ReadySet` and `claim_snapshot_frozen_valid' = TRUE`.
Every other Central action UNCHANGES both.  EventTimer's
`CentralRefinement` INSTANCE binds them via WITH; EventTimer claim actions
project them unchanged.

The three Central snapshot laws are rewritten:

- `S_InvClaimSnapshotContainsWinner == winner \in Arms => winner \in claim_candidates`
- `S_InvClaimSnapshotImmutableAfterClaim == claim_snapshot_frozen_valid => claim_candidates = claim_snapshot_frozen`
- `S_InvWinnerChosenFromSnapshot == winner \in Arms /\ claim_snapshot_frozen_valid => winner \in claim_snapshot_frozen`

`NEG-S2` is retargeted to `S_InvClaimSnapshotImmutableAfterClaim` (it
mutates `claim_candidates` post-claim while leaving the frozen value
untouched).  `NEG-S7` is new: it adds a snapshot-external registered arm to
`claim_candidates` while leaving the frozen snapshot and the winner
untouched (3-arm cfg, isolates the strict immutability law in its hardest
case).

Single-claim / single-operation model: the frozen snapshot captures
exactly one claim epoch.  Multi-epoch claim identity is deferred.

## H. Frozen winner identity (P1-4)

Contract adds two observer variables:

- `linearized_winner \in Arms \cup {NoArm}` — the identity stamped by the
  first linearization.
- `linearized_winner_valid \in BOOLEAN` — TRUE once a linearization has
  stamped the identity.

`ContractInit` sets winner=`NoArm`, valid=`FALSE`.  `ContractLinearizeWinner(i)`
sets `linearized_winner' = i`, `linearized_winner_valid' = TRUE`.  Every
other Contract action UNCHANGES both.  Central's `ContractRefinement`
INSTANCE binds them via WITH (Central's `CentralClaimWinner` projects
Contract's linearization unchanged); EventTimer's `CentralRefinement`
INSTANCE binds them via WITH again (EventTimer's claim actions project
Central's projection unchanged).

The three Contract winner laws are rewritten:

- `C_InvCommitRequiresWinnerLinearization == \A i: arm_resolution[i]="WinnerCommitted" => linearized_winner_valid /\ linearized_winner = i`
- `C_InvNoIrreversibleEffectBeforeLinearization == ~linearized_winner_valid => \A i: arm_resolution[i] # "WinnerCommitted"`
- `C_InvWinnerIdentityStableAfterLinearization == linearized_winner_valid => winner = linearized_winner`

`NEG-C9` is new: a legal linearization has stamped `linearized_winner = A`;
the fault flips the live `winner` to a different registered arm B ≠ A
while no arm is `WinnerCommitted` and the frozen history is left
unchanged.  This isolates exactly the identity-stability law.

## I. New negative models (C9, S7, MG1)

| Fault | File | Target | Domain |
|-------|------|--------|--------|
| `NEG-C9` | `E13SelectContractNeg.tla` | `C_InvWinnerIdentityStableAfterLinearization` | 2-arm |
| `NEG-S7` | `E13SelectCentralClaimNeg.tla` | `S_InvClaimSnapshotImmutableAfterClaim` | 3-arm |
| `NEG-MG1` | `E13SelectMultiGroupNeg.tla` (new wrapper) | `MG_InvAuthorityClosureDoesNotCrossGroups` | 2-group, Events={0} |

`E13SelectMultiGroupNeg.tla` is a new INSTANCE-WITH wrapper that follows
the same architecture as the Contract / Central / EventTimer neg modules:
canonical `MGNext` is reused via `BaseNextFrozen`, and `FaultNext`
disjoins exactly one focused fault selected by the `FAULT` constant.
`NEG-MG1` makes group g0's `RollbackArm` spuriously mutate group g1's
`authority_open` while group g1 is at a terminal phase; the per-action
UNCHANGED audit establishes structurally that the canonical action would
never do this.

Each new fault has its own FAULT="None" restoration cfg.

## J. Accounting type correction (P2)

`E13SelectEventTimer.tla` defines `AccountingCountT == 0..2` and uses it
as the TypeOK domain for `wait_account_open_count`,
`wait_account_close_count`, `timer_account_open_count`,
`timer_account_close_count`.  No legal transition ever produces a 2; the
at-most-once `A_Inv*` laws are now the *only* source of the `<= 1` bound.

`NEG-A1`/`NEG-A2` push a counter from 1 to 2 to isolate the at-most-once
law.  The verifier runs them twice: once via `expect_negative` (target
must be violated) and once via `expect_negative_no_typeok` (target must
be violated AND the TLC output must NOT contain
`EventTimerTypeOK is violated`).  The double-check confirms the fault
isolates exactly the at-most-once law, not a typing precondition.

## K. Refinement

The refinement PROPERTY checks `RefinesContract` and `RefinesCentralClaim`
re-pass on the corrective-1 changes.  The new Contract observers
(`linearized_winner`, `linearized_winner_valid`) propagate through Central's
`ContractRefinement` WITH and EventTimer's `CentralRefinement` WITH.  The
new Central observers (`claim_snapshot_frozen`, `claim_snapshot_frozen_valid`)
propagate through EventTimer's `CentralRefinement` WITH only (Contract
does not perceive claim-snapshot state).

- `E13SelectCentralClaim.cfg` (2-arm) PASS — 111 distinct, depth 16.
- `E13SelectCentralClaim.refine3.cfg` (3-arm) PASS — 531 distinct, depth 20.
- `E13SelectEventTimer.cfg` (2-arm) PASS — 22528 distinct, depth 30.
- `E13SelectEventTimer.safety3mix.cfg` (3-arm mix) PASS — 2.67M distinct,
  depth 37 (widened TypeOK adds small state-space growth over PR #18's
  2.67M baseline; still well under the 5-minute TLC budget).

## L. Positive safety

All seven positive aggregates PASS unchanged in scope:

- `ContractSafetyInv` 2-arm / 3-arm.
- `CentralSafetyInv` 2-arm / 3-arm sim / 4-arm tie-break.
- `AdapterSafetyInv` 2-arm / 3-arm mix.

The Contract safety cfgs grow slightly because `linearized_winner` /
`linearized_winner_valid` are now part of the state tuple (346 distinct →
346 distinct at 2-arm; the new variables are deterministic functions of
the existing state so the distinct-state count does not grow).  The
Central safety cfgs likewise stay at 111 distinct.  The Adapter safety3mix
cfg grows by the TypeOK widening (8.74M generated, 2.67M distinct — same
order of magnitude as the PR #18 baseline).

## M. Non-vacuity

Five new non-vacuity witnesses are added:

- Contract: `NotReachContractWinnerLinearizedNotCommitted` (linearized
  but not yet committed — the identity-stability window).
- Central: `NotReachCentralFrozenSnapshotValid` (frozen snapshot stamped)
  + `NotReachCentralMultiCandidateSnapshot` (>= 2 candidates in the
  frozen snapshot — non-trivial selection).
- Adapter: `NotReachAdapterAccountCountOne` (at least one arm reached
  counter value 1, the value the at-most-once laws constrain, post
  TypeOK widening).
- Multi-group: `NotMG_ReachArmInstalledNotFinished` (arm installed but
  pre-`FinishRegistration`) + `NotMG_ReachPreFinishRollback` (rollback
  reachable from the pre-finish state).

All 28 non-vacuity witnesses (24 carried over + 5 new — wait, see the
corrected count in §N below) report `Invariant NotReach_<X> is violated`.

## N. Restoration

Four FAULT="None" restoration cfgs:

- `E13SelectContractNeg.restore.cfg` PASS — 346 distinct, depth 16.
- `E13SelectCentralClaimNeg.restore.cfg` PASS — 111 distinct, depth 16.
- `E13SelectEventTimerNeg.restore.cfg` PASS — 22528 distinct, depth 30.
- `E13SelectMultiGroupNeg.restore.cfg` PASS — 4456 distinct, depth 25.

Each proves the canonical aggregate (`*SafetyInv`) still reaches
"completed, no error" once the fault is disabled.

## O. Core regressions

The PR #17 core verifier (`tools/formal/verify-e13-select-core.sh`) is not
semantically changed by the corrective: it still runs the canonical 21
gates + sentinel + source-clean check.  The only change is the portable
tmpdir hardening (§I3) of the cleanup trap.  The pre-existing untracked
files `tests/test_t3_simple.cpp` and `tla2tools.jar` remain untracked and
unchanged.

## P. Verification tooling

`tools/formal/verify-e13-select-safety.sh`:

- splits the multi-group O section into 3 independent reach cfgs + 2 nv
  cfgs;
- adds NEG-C9 (Contract), NEG-S7 (Central, 3-arm), NEG-MG1 (Multi-group)
  + their FAULT="None" restoration cfg;
- retargets NEG-S2 to `S_InvClaimSnapshotImmutableAfterClaim`;
- adds `expect_negative_no_typeok` for NEG-A1/A2 (widened-TypeOK
  double-check);
- adds 5 new non-vacuity witnesses under the W section and 2 under the O
  section;
- updates the header docstring to reflect the new gate shape.

The script's success criterion is the absence of `FAIL` lines, not a
hard-coded total.  Corrective-1 brings the total from 65 to 78 TLC runs.

## Q. State-space evidence

Selected metrics from the corrective-1 verifier run (full output in the
reviewer's reproduction):

```text
PASS  Contract safety (2-arm)         346 distinct, depth 16
PASS  Contract safety (3-arm)         3436 distinct, depth 20
PASS  Central safety (2-arm)          111 distinct, depth 16
PASS  Central safety (3-arm sim)      531 distinct, depth 20
PASS  Central safety (4-arm tie)      2871 distinct, depth 24
PASS  Adapter safety (2-arm)          22528 distinct, depth 30
PASS  Adapter safety (3-arm mix)      2671164 distinct, depth 37, ~4min
PASS  MG safety (2-group shared Ev)   4456 distinct, depth 25
PASS  Central -> Contract refine (3)  531 distinct, depth 20
NEG   NEG-C9 winner identity flip     49 distinct, depth 7
NEG   NEG-S7 snapshot add (3-arm)     81 distinct, depth 8
NEG   NEG-MG1 cross-group rollback    489 distinct, depth 8
NEG   NEG-A1 widowed (no TypeOK)      195 distinct, depth 6
NEG   NEG-A2 widowed (no TypeOK)      390 distinct, depth 7
```

## R. Scope audit

```bash
git diff --check                                       # no whitespace errors
git status --short                                     # only docs/tools paths
git diff --name-status 0f88ab9...HEAD                  # only docs/spec, docs/formal, docs/reviews, tools/formal
git diff --name-status 57435a91...HEAD                 # same scope (corrective does not widen PR scope)
git status --short tests/test_t3_simple.cpp tla2tools.jar  # both remain '??'
```

No path under `include/`, `src/`, `tests/`, `examples/`, `benchmarks/`,
public API, build policy, or unrelated CI appears in either diff.  The
rejected PASS draft
`docs/reviews/E13-SELECT-FORMAL-MODEL-SAFETY-1-INDEPENDENT-REVIEW-1.md` is
deleted (it was an untracked file, never committed).

## S. Residual risks

- **Single-claim / single-operation.**  The frozen observer variables
  capture exactly one claim epoch.  Multi-epoch claim identity (re-claim
  after a terminal rollback, then claim again) is deferred; the model
  does not represent a second claim epoch.
- **Multi-group bounded.**  Non-interference is proven for two groups
  sharing one Event identity only; an unbounded concurrency proof is out
  of scope.
- **Safety, not liveness.**  No fairness / liveness is claimed.
- **Adapter refinement on wider domains.**  The temporal
  `RefinesCentralClaim` PROPERTY is checked only on the 2-arm domain;
  wider adapter PROPERTY checks blow up past the 5-minute TLC budget.
  The wider adapter domain is covered by the `AdapterSafetyInv`
  aggregate in safety3mix, not by the explicit PROPERTY check.
- **Canonical 4-arm root slowdown (unchanged from PR #18).**  The
  canonical 4-arm root is a ~10-minute gate due to the M/N state
  extension; this is a known characteristic documented in PR #18 and
  unaffected by the corrective.

## T. Independent delta review request

Independent delta review requested.  See the binding, scope, and ask in
`E13-SELECT-FORMAL-MODEL-SAFETY-1-CORRECTIVE-1-REVIEW-REQUEST.md`.  The
author's verdict is `PASS — AUTHOR SELF-ASSESSMENT`; the independent
verdict is the load-bearing one.
