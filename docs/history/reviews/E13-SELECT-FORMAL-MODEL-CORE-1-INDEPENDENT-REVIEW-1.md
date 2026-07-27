# E13 Select Formal Model Core — Independent Layered Review 1

## A. Verdict

```text
E13-SELECT-FORMAL-MODEL-CORE-1-INDEPENDENT-REVIEW-1:
REQUEST-CHANGES

ABSTRACT SELECT CONTRACT:
REJECTED

CENTRAL CLAIM REFINEMENT SKELETON:
ACCEPTED

EVENT/TIMER ADAPTER INSTANCE:
ACCEPTED

REACHABILITY R1-R12:
PASS

ALTERNATIVE-STRATEGY REPLACEABILITY:
PASS

ADDITIONAL-ARM EXTENSIBILITY:
PASS

PR #18:
DENIED

PRODUCTION IMPLEMENTATION:
DENIED
```

The three-layer split is real, both supplied refinement checks pass, the
Event/Timer adapter skeleton closes concrete authority before completion, and
R1-R12 have genuine causal traces. The review nevertheless cannot accept the
abstract Contract: it admits a trace that suspends the caller, performs
registration rollback, and destroys the operation while the caller remains
`Waiting` with no runnable publication. This contradicts the controlling
preparation lifecycle and makes the claimed stable external Contract too weak.

This review is bound to GitHub PR #17 at head
`e61a0b4971758a819d8ba3b94f3e3147b34c4bc1`. Authorization remains denied
because of the Contract finding and the formal-helper source-tree deletion
risk, not because of missing PR identity.

## B. Baseline

Review time: 2026-07-20, Asia/Shanghai.

```text
REPOSITORY:
jnhu76/Sluice

PR:
https://github.com/jnhu76/Sluice/pull/17 — OPEN

REVIEW_TARGET:
PR17_LAYERED_CORE

BASE_COMMIT:
246505a1de2ffa2840ab404adc76a2839d4ae069

MERGE_BASE:
246505a1de2ffa2840ab404adc76a2839d4ae069

HEAD:
e61a0b4971758a819d8ba3b94f3e3147b34c4bc1

EXPECTED_HEAD:
e61a0b4971758a819d8ba3b94f3e3147b34c4bc1

BRANCH:
feat/e13-select-formal-model-core

COMMITS (origin/master..HEAD):
e61a0b4 docs(e13): add formal specs, verification tools, and design docs for select-formal-model-core

COMMITTED CHANGED FILES (origin/master...HEAD):
30 added files; 3,381 insertions, 0 deletions

WORKTREE STATUS:
PR HEAD TRACKED FILES CLEAN BEFORE REVIEW;
pre-existing tests/test_t3_simple.cpp and tla2tools.jar remain untracked
```

The required baseline commands were run independently against the immutable PR
head. `gh pr view 17` reports base `master`, head branch
`feat/e13-select-formal-model-core`, one commit, 30 changed files, and a
mergeable open PR. `git merge-base HEAD origin/master` equals `BASE_COMMIT`.
`git diff --check origin/master...HEAD` reports seven blank-line-at-EOF
warnings, recorded as P2-1.

Committed PR task files:

- `docs/formal/e13-select-formal-core-design.md`
- `docs/formal/e13-select-formal-core-plan.md`
- `docs/reviews/E13-SELECT-FORMAL-MODEL-CORE-1-INDEPENDENT-REVIEW-1.md`
- `docs/reviews/E13-SELECT-FORMAL-MODEL-CORE-1-REVIEW-REQUEST.md`
- `docs/spec/e13_select/*.tla`
- `docs/spec/e13_select/*.cfg`
- `docs/spec/e13_select/README.md`
- `tools/formal/verify-e13-select-core.sh`

Pre-existing local files observed separately:

- `tests/test_t3_simple.cpp`
- `tla2tools.jar`

## C. Scope audit

No candidate file was found under `include/**`, `src/**`, the public API, or
production build policy. No candidate negative model was added. The formal
candidate is confined to `docs/formal/`, `docs/spec/e13_select/`,
`docs/reviews/`, and `tools/formal/`.

`tests/test_t3_simple.cpp` is an untracked AsyncCondition smoke test, not E13
Select evidence. Its filesystem birth/modify time is 2026-07-15, before the
2026-07-20 formal candidate, and multiple already-tracked historical documents
record it and `tla2tools.jar` as pre-existing untracked files. It is therefore
not classified as a PR17 `tests/**` change. This conclusion is evidence-based,
not taken from the author's self-assessment.

The committed PR file list independently confirms that neither pre-existing
local file is included in PR #17.

Ignored `docs/spec/e13_select/states/**` files are TLC metadir remnants covered
by the repository's `states/` ignore rule; they are not treated as review
evidence or proposed PR files.

`bash -n` found no shell syntax error. ShellCheck reported only SC2329 for the
cleanup function invoked indirectly by `trap`. `git diff --check` found the
seven P2 blank-line-at-EOF warnings listed below.

## D. Three-layer architecture

| Layer | Result | Independent observation |
|---|---|---|
| C — `E13SelectContract` | Structurally separated; semantically rejected | Contains only abstract registration, offered/reserved evidence, resolution, authority, publication, consumption, destruction, and rollback state. No concrete primitive or Central Claim symbol leaks in. Its rollback domain is invalid; see P0-1. |
| S — `E13SelectCentralClaim` | Accepted | Owns `candidate_ready`, `claim_candidates`, lowest-index selection, claim mode, and Winner/Loser classification. It does not own WaitNode, Event queue, or TimerRegistration mutation. |
| A — `E13SelectEventTimer` | Accepted | Owns Event identity/mapping, Event scan coordination, WaitNode state, TimerRegistration state, accounting, and concrete finalization. Winner authority is obtained by invoking the Central Claim action, not independently selected by an adapter. |

Contract contamination searches found the words CandidateReady/Event/Timer only
in explanatory comments that state their exclusion; there is no corresponding
Contract variable, action, guard, or invariant. Central Claim likewise contains
no Event/Timer/WaitNode/queue storage.

## E. Contract review

| Required Contract concept | Formal expression | Result |
|---|---|---|
| At most one winner linearization | single `winner`, `ContractLinearizeWinner`, `ExactlyOneLinearizedWinner` | PASS |
| No irreversible commit before linearization | `ContractCommitWinner` guard and `NoIrreversibleEffectBeforeLinearization` | PASS |
| Winner commit | `arm_resolution = "WinnerCommitted"` | PASS |
| Loser abort/release | `ContractReleaseLoser`, `arm_resolution = "Released"` | PASS |
| Result publication | `result_publication_count`, winner arm publication | PASS |
| Runnable publication | inline = 0, suspended = 1 | PASS |
| All authority closed before completion | `SuccessfulClosureReady`, `AllAuthorityClosedBeforeCompletion` | PASS |
| Consumption/destruction eligibility | `Completed -> Consumed -> Destroyed`; aborted teardown may destroy | PASS except rollback interaction |
| Optional reversible reservation | `Reserved/Held -> Committed|Released`, close count | PASS; 3-arm witness reached |
| Registration rollback | `BeginRollback`, per-arm release/close, `Aborted` | **FAIL**: enabled after caller suspension |

The failing trace is not a deferred PR18 negative-model concern. It is legal in
the positive `ContractSpec`, satisfies `ContractInv`, and contradicts the
stable external lifecycle that PR17 claims to establish.

## F. Strategy review

The Central Claim skeleton is accepted.

- `CentralObserveCandidate` maps CandidateReady to Contract `Offered`.
- `CentralClaimWinner` snapshots the complete current `ReadySet`, invokes
  Contract winner linearization, and assigns exactly one Winner class.
- `CentralCommitWinner` is separate from claim and performs no primitive
  Event/Timer mutation.
- `CentralReleaseLoser` and `CentralCloseAuthority` are separate actions.
- `claim_candidates` is assigned only at claim and is immutable afterwards.
- Lowest index is applied to the claim snapshot, not asserted as a global
  post-arming fairness guarantee.
- The 3-arm reviewer reach probe reached `claim_candidates = Arms` before
  selecting the minimum arm.

The identity projection to Contract is total for all Contract variables and
each Central action is a Contract action or Contract stutter.

## G. Adapter review

### Event

- `event_state` is indexed by real `Events`; `arm_event` maps arms to identity.
- `StartEventBroadcast` records one held Event and snapshots only registered
  arms mapped to it.
- `ScanEventArm` only offers candidates and leaves WaitNodes pending/linked.
- `FinishEventScan` releases `held_event` before `ClaimEventWinner`.
- The same-Event trace scans two arms from one `scan_remaining` snapshot and
  then claims from `{0, 1}`.
- Event winner and loser finalization terminalize/detach WaitNodes, clear
  context, and close waiting accounting before adapter authority closes.
- Select never consumes persistent Event `Set` state.

### Timer

- Every Timer arm has `Active/Consumed/Retired` registration state.
- `TimerPumpEntry` requires `Active` before changing `timer_node_deref` and
  offering the candidate.
- Winner order is claim, `Active -> Consumed`, then expire/detach and commit.
- Loser order is cancel/detach, then `Active -> Retired` and Contract release.
- `TimerPumpSkip` requires a due terminal registration, records an actual skip,
  and leaves `timer_node_deref` unchanged.
- Two Timer arms were independently checked to completion with one Consumed
  winner and one Retired loser.

### Completion gate

`CompleteInline` and `CompleteSuspended` require every adapter arm `Retired`.
Central publication additionally requires winner commit, every loser release,
and every abstract authority closed. Adapter invariants make `Retired` imply
terminal/detached WaitNode, cleared context, and closed waiting/timer accounts.

A reviewer-only `CompletionAccountingClosed` invariant was checked over the
complete 2-arm adapter graph: 53,617 generated / 17,472 distinct / queue 0,
depth 30, PASS.

## H. Reachability

All R1-R12 probes violated only their expected named inverse invariant. Key
action sequences inspected from TLC traces:

| Reach | Trace evidence | Result |
|---|---|---|
| R1 | admission Event offer -> claim -> Event winner -> Event loser -> all close -> inline publish | PASS |
| R2 | Timer due at admission -> claim -> consume Timer -> expire/detach -> Event loser -> inline publish | PASS |
| R3 | suspend -> one Event broadcast/scan -> claim -> finalize/close -> suspended publish | PASS |
| R4 | suspend -> Tick -> `TimerPumpEntry` -> claim -> consume/finalize -> suspended publish | PASS |
| R5 | Event winner plus `CancelTimerLoser -> RetireTimerLoser` | PASS |
| R6 | Timer consume/winner plus Event loser | PASS |
| R7 | one admission claim; snapshot `{0,1,2,3}`; winner `0`; all four CandidateReady | PASS |
| R8 | Event `0`; `scan_remaining={0,1}`; two scan actions; snapshot `{0,1}` | PASS |
| R9 | partial one-arm registration -> rollback -> release -> authority close -> Aborted | PASS |
| R10 | Event winner -> Timer loser Retired -> Tick due -> actual `TimerPumpSkip`; deref count `0` | PASS |
| R11 | full inline completion -> `ConsumeResult` with runnable count 0 | PASS |
| R12 | full suspended completion -> `ResumeCaller -> ConsumeResult` with runnable count 1 | PASS |

R7 is not merely a terminal winner/loser state. R8 cannot be witnessed by two
independent Event sets. R10 cannot be witnessed by retirement alone.

## I. Refinement checks

### Central Claim -> Contract

`E13SelectCentralClaim.cfg` checks `RefinesContract` as a temporal property and
completed with 332 generated / 144 distinct / queue 0 / depth 16. The mapping
is type-correct and total for the single modeled operation/epoch and preserves
arm identity by using the same `Arms` domain.

### Event/Timer -> Central Claim

`E13SelectEventTimer.cfg` checks `RefinesCentralClaim` and completed with 53,617
generated / 17,472 distinct / queue 0 / depth 30. Concrete winner, candidate,
classification, commit, release, closure, and publication state all project to
the same arm of the same single modeled operation.

No cross-operation or cross-epoch mapping exists because PR17 models one
operation and one claim epoch. That bounded scope must not be restated as a
proof for multiple concurrent SelectGroups.

## J. History/order checks

PR17 does not yet contain the full step-indexed history suite required from
PR18. Within the PR17 skeleton, independently inspected traces establish:

- winner linearization precedes Event/Timer commit;
- Timer consumption precedes Timer winner expiration/finalization;
- Timer loser cancellation/detach precedes registration retirement;
- all concrete finalization and accounting closure precede publication;
- same-Event scan finishes before group claim;
- stale Timer skip occurs after retirement and without dereference.

The model uses the same arm index throughout each ordering. No order predicate
compares different arms or default step-zero values. Full same-operation,
same-arm, same-epoch numeric history invariants remain PR18 work and are not
claimed closed here.

## K. Invariant review

The PR17 positive invariants are reachable and non-vacuous for their claimed
scope. The supplied Contract, Central, adapter, and constrained 4-arm positive
graphs all exhausted with queue 0. The invariant set is explicitly a coherence
foundation, not full E13 safety closure.

One load-bearing omission is blocking: no Contract invariant forbids an
aborted/destroyed operation from retaining a `Waiting` caller. As a result the
positive invariant suite accepts P0-1.

No two-winner, double-publication, stale Timer dereference, completion with
open adapter accounting, or pre-claim primitive commit was observed in the
accepted Central/Event/Timer instance.

## L. Negative models

No `NEG-C*`, `NEG-S*`, `NEG-E*`, or `NEG-T*` model is supplied. PR17 does not
claim negative-model closure, so their absence is not itself a PR17 finding.
They remain mandatory for PR18. The reviewer-only rollback reach predicate is
not accepted as a substitute for the required PR18 fault models; it is evidence
that the current positive Contract itself admits an invalid behavior.

## M. Non-vacuity

| Trigger/state | Evidence | Result |
|---|---|---|
| Winner linearized/committed | R1-R8 and positive coverage | REACHED |
| Loser aborted | R1/R3/R5/R6 and coverage | REACHED |
| Authority closed | R1-R6/R11/R12; completion invariant | REACHED |
| Reservation Held/Committed/Released | Contract 2-arm and reviewer 3-arm traces | REACHED |
| Central reservation None-instance | `CentralStateWellFormed`; exhaustive Central graphs | REACHED |
| Inline/suspended complete | R1-R4 | REACHED |
| Multiple CandidateReady | Central 3-arm and R7 | REACHED |
| Same Event multi-arm | R8 | REACHED |
| Timer Active/Consumed/Retired | R2/R4/R5/R6/R10/two-Timer | REACHED |
| Stale check/skip | R10 actual `TimerPumpSkip` | REACHED |
| Rollback | R9 | REACHED |
| Consumed/Destroyed | R11/R12 plus positive action coverage | REACHED |

The exhaustive 2-arm coverage run gave non-zero coverage to every non-stutter
adapter action. `EventTimerStutter` produced zero distinct transitions, as
expected.

## N. Replaceability

Answers to the PR17 replacement questions:

1. A non-Central-Claim strategy can instantiate the Contract without changing
   its primitive-neutral state: **yes**.
2. Reservation state can be added/used without changing winner semantics:
   **yes**; the Contract already supports reversible `Held` evidence.
3. A new adapter can refine through its strategy without adding
   primitive-specific Contract state: **yes**.
4. `CandidateReady` exists only in Layer S/A projection, not Layer C: **yes**.
5. WaitNode state exists only in Layer A: **yes**.

Therefore mechanical alternative-strategy replaceability passes. P0-1 still
requires strengthening the stable Contract before it can be accepted; a future
strategy may refine a stricter subset today, but the Contract itself must not
advertise the bad rollback trace as legal external behavior.

## O. Extensibility

The Contract boundary can express the required future categories without
primitive-specific state:

- single-epoch Event/Timer adapters use `Offered` evidence;
- Semaphore/Mutex grant reservations can use reversible `Reserved/Held` state;
- Queue payload/transfer adapters can keep transfer authority internally and
  project only commit-or-release to the Contract;
- a multi-epoch coordinator can keep internal epochs in Layer A while holding
  abstract authority open until winner commit or loser release.

The preparation documents correctly keep Semaphore, Mutex, Queue, and
AsyncCondition out of the current instance. In particular, this review does
not infer that AsyncCondition's mandatory Mutex reacquire already fits the
single-epoch Event/Timer adapter. No additional arm type is implemented or
authorized by PR17.

## P. State-space evidence

Independent full gate (`TLC_WORKERS=1 tools/formal/verify-e13-select-core.sh`):

| Model | Generated | Distinct | Queue | Depth | Result |
|---|---:|---:|---:|---:|---|
| Contract 2-arm | 1,252 | 533 | 0 | 16 | PASS |
| Central 2-arm + Contract refinement | 332 | 144 | 0 | 16 | PASS |
| Event/Timer 2-arm + Central refinement | 53,617 | 17,472 | 0 | 30 | PASS |
| constrained 4-arm mixed root | 443,664 | 99,868 | 0 | 40 | PASS |

Reviewer-only bounded checks, stored outside the repository:

| Check | Generated | Distinct | Queue | Depth | Result |
|---|---:|---:|---:|---:|---|
| Contract 3-arm safety | 14,582 | 4,973 | 0 | 20 | PASS |
| Contract 3-arm all-reservation completion | 14,077 | 4,790 | 138 at witness | 17 | REACHED |
| Central 3-arm safety/refinement | 2,112 | 734 | 0 | 20 | PASS |
| Central 3 simultaneous candidates | 234 | 119 | 40 at witness | 9 | REACHED |
| Central 4-arm safety/refinement | 14,676 | 4,116 | 0 | 24 | PASS |
| pure two-Timer adapter safety/refinement | 7,694 | 2,412 | 0 | 29 | PASS |
| pure two-Timer complete winner/loser | 4,467 | 1,530 | 191 at witness | 18 | REACHED |
| successful completion accounting invariant | 53,617 | 17,472 | 0 | 30 | PASS |
| invalid rollback-after-suspend trace | 1,051 | 453 | 40 at witness | 12 | **REACHED** |

The supplied R1-R12 metrics also reproduced exactly. No symmetry set is used.
`FourArmMixedConstraint` fixes one topology (same-Event pair plus two Timers)
but does not remove lifecycle actions; unconstrained 2-arm and targeted scenes
cover the other first-scope topologies.

TLC version: `2.19 of 08 August 2024 (rev: 5a47802)`.

## Q. Findings

### P0-1 — CONTRACT INVALID: rollback can destroy a suspended caller

Locations:

- `docs/spec/e13_select/E13SelectContract.tla:205` — suspend leaves
  `contract_phase = "Selecting"` and sets caller `Waiting`.
- `docs/spec/e13_select/E13SelectContract.tla:339` — rollback is allowed from
  any `Selecting` state without requiring a running caller.
- `docs/spec/e13_select/E13SelectContract.tla:365` — rollback finishes as
  `Aborted` without repairing caller state or publication.
- `docs/spec/e13_select/E13SelectContract.tla:330` — an aborted operation may
  be destroyed unconditionally.

Independent counterexample:

```text
RegisterArm(0)
RegisterArm(1)
FinishRegistration
SuspendCaller                 caller = Waiting
BeginRollback
RollbackRelease(0)
CloseAuthority(0)
RollbackRelease(1)
CloseAuthority(1)
FinishRollback               phase = Aborted, caller = Waiting
DestroyOperation             phase = Destroyed, caller = Waiting
```

Final state: no winner, result publication 0, runnable publication 0, all arm
authority closed, caller still `Waiting`, operation destroyed. The trace
satisfies the supplied `ContractInv`.

This contradicts the preparation's registration-failure rollback domain and
its rule that destroying an armed/waiting operation is invalid. It is a stable
Contract error, not a Central Claim detail. Suggested correction: make the
rollback domain explicitly pre-suspension (or model a separate cancellation
protocol that safely transitions/publishes a suspended caller), then add a
positive invariant forbidding `Waiting` in aborted/destroyed states and rerun
all Contract reach/refinement checks.

### P1-1 — FORMAL HELPER MAY DELETE SOURCE-TREE TRACE ARTIFACTS

`tools/formal/verify-e13-select-core.sh:22-30` runs TLC from the source spec
directory and its `cleanup` executes:

```bash
find "$spec" -maxdepth 1 -name 'E13Select*TTrace*' -delete
```

This deletes every matching file in the source tree, not only files created by
the current run. Running the required reviewer gate can therefore remove an
existing evidence or user file, violating the review's no-model-modification
discipline. Run TLC against a temporary copy/work directory, or record and
delete only artifacts created by that invocation; cleanup should be confined
to the fresh temporary root.

### P2-1 — configuration/documentation drift

- `docs/formal/e13-select-formal-core-plan.md:17` names the nonexistent
  `E13SelectContract.reach.cfg` instead of the actual inline/suspended files.
- `E13Select.scene_2mix.cfg` says it proves R5 and R6, but declares only
  `INVARIANT NotReach_R5`; R6 is checked by the separate Timer-winner cfg.
- `E13Select.scene_sameevent.cfg` still describes old per-arm Event-state
  shorthand although the model now has real Event identity and scan state.
- `E13Select.scene_staletimer.cfg` describes mere coexistence of a retired
  Timer and completed group, while the actual predicate correctly requires an
  executed `TimerPumpSkip`.

These do not invalidate the actual predicates/traces, but the comments should
match the evidence they claim.

`git diff --check origin/master...HEAD` also reports a new blank line at EOF in
the formal design document and six layer cfg files. These are formatting-only
warnings but should be removed so the required baseline command is clean.

## R. Authorization effect

```text
PR #17 MERGE:
DENIED

PR #18:
DENIED

FORMAL SAFETY CLOSURE:
OPEN

PRODUCTION IMPLEMENTATION:
DENIED

ALTERNATIVE STRATEGIES:
NOT IMPLEMENTED / NOT AUTHORIZED

ADDITIONAL ARM TYPES:
NOT IMPLEMENTED / NOT AUTHORIZED
```

PR18 may proceed only after P0-1 is corrected, the formal helper no longer
risks deleting source-tree files, and an independent re-review of the new PR17
head reproduces the corrected Contract and both refinements.

## S. Repository status

This review turn modified only:

```text
docs/reviews/E13-SELECT-FORMAL-MODEL-CORE-1-INDEPENDENT-REVIEW-1.md
```

No formal model, cfg, helper, production source, production test, public API,
or build policy file was modified by the reviewer. Reviewer-only TLA modules
and configs used for adversarial probes were kept under `/tmp/e13-pr17-review`
and are outside the repository.

The worktree now differs from PR head only in this review artifact plus the
pre-existing untracked test/JAR. No formal model, cfg, helper, production file,
or public API was modified; nothing was staged, committed, or pushed.
