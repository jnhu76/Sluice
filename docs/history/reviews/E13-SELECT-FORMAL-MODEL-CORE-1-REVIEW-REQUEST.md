# E13 Select Formal Model Core — Review Request

```text
E13-SELECT-FORMAL-MODEL-CORE-1:
PASS — AUTHOR SELF-ASSESSMENT

FORMAL CORE MODEL:
IMPLEMENTED

EVENT ARM:
MODELED

INDEPENDENT TIMER ARM:
MODELED

INLINE COMPLETION:
REACHABLE

SUSPENDED COMPLETION:
REACHABLE

EVENT WINNER:
REACHABLE

TIMER WINNER:
REACHABLE

REGISTRATION ROLLBACK:
REACHABLE

STALE TIMER SKIP:
REACHABLE

SAFETY INVARIANT CLOSURE:
NOT CLAIMED — PR #18 REQUIRED

NEGATIVE MODELS:
NOT IMPLEMENTED — PR #18 REQUIRED

PRODUCTION IMPLEMENTATION:
DENIED

PR #18:
AUTHORIZED PENDING INDEPENDENT REVIEW OF PR #17
```

## A. Baseline

```text
REPOSITORY:
jnhu76/Sluice

TASK:
E13-SELECT-FORMAL-MODEL-CORE-1

BASE_BRANCH:
master

BASE_COMMIT:
246505a1de2ffa2840ab404adc76a2839d4ae069

START_HEAD:
246505a1de2ffa2840ab404adc76a2839d4ae069

WORKING_BRANCH:
feat/e13-select-formal-model-core
```

`START_HEAD` equals `origin/master`; it contains merged PR #16 commit
`246505a E13: Select multi-wait preparation design (Event + Timer first scope)
(#16)`.

The starting worktree contained the author's untracked partial
`docs/spec/e13_select/` model plus unrelated untracked
`tests/test_t3_simple.cpp` and `tla2tools.jar`. The unrelated files were not
modified, deleted, staged, or committed.

## B. Files changed

Formal model:

- `docs/spec/e13_select/E13SelectContract.tla`
- `docs/spec/e13_select/E13SelectCentralClaim.tla`
- `docs/spec/e13_select/E13SelectEventTimer.tla`
- `docs/spec/e13_select/E13Select.tla`
- contract, Central Claim, adapter, bounded-root, and R1–R12 `.cfg` files in
  `docs/spec/e13_select/`
- `docs/spec/e13_select/README.md`

Supporting material:

- `docs/formal/e13-select-formal-core-design.md`
- `docs/formal/e13-select-formal-core-plan.md`
- `tools/formal/verify-e13-select-core.sh`
- this review request

`E13Select.tla` is only a compatibility root. No C++, production test, public
API, build policy, negative model, or unrelated formal model was added or
changed.

## C. Model structure

The revised model has three semantic layers:

```text
E13SelectContract
        ^ explicit TLC-checked refinement
E13SelectCentralClaim
        ^ explicit TLC-checked refinement
E13SelectEventTimer
        ^ thin compatibility instance
E13Select
```

`E13SelectContract` defines stable Select laws. It has no CandidateReady,
Event, Timer, WaitQueue, timer heap, or coordination-mutex state.

`E13SelectCentralClaim` is the currently selected Candidate A strategy, not the
definition of every possible Select implementation. It alone owns
CandidateReady, the claim-time snapshot, lowest-index admission, group claim,
and Winner/Loser classification.

`E13SelectEventTimer` contains only the authorized first-scope adapters and
their concrete causal obligations.

Review of the supplied partial monolith found and corrected these material
issues:

- completion could occur while loser authority remained registered and linked;
- the Timer pump action attempted incompatible phase assignments and could not
  witness its intended path;
- R7 accepted a terminal loser instead of a multi-candidate claim snapshot;
- R8 accepted unrelated per-arm Event sets rather than one real Event broadcast;
- R10 accepted rollback-created retirement without an actual stale pump skip;
- Event scanning could bypass queue-identity/authority causality;
- timer dereference and per-arm authority accounting were incomplete.

## D. Variable mapping

| Preparation concept | Contract | Central Claim | Event/Timer adapter |
|---|---|---|---|
| operation lifecycle | `contract_phase` | `central_phase` projection | adapter terminal guards |
| offered readiness evidence | `readiness_evidence` | `candidate_ready` | Event admission/scan or Timer pump |
| reversible reservation | `reservation_state` | deliberately unused by Candidate A | deliberately unused by first-scope adapters |
| winner authority | `winner` | `winner`, `claim_candidates` | projection only |
| arm resolution | `arm_resolution` | `arm_class` | `finalization_step` |
| final authority closure | `authority_open` | close action | `adapter_phase = "Retired"` |
| publication | result/runnable/per-arm counts | claim-mode projection | `select_result_published` mirror |
| Event identity | absent | absent | `Events`, `arm_event`, `held_event` |
| WaitNode | absent | absent | `wait_outcome`, `wait_linked` |
| typed context | absent | absent | `context_state` |
| timer authority | absent | absent | `timer_state`, `timer_node_deref` |
| per-arm accounting | abstract authority only | abstract authority only | waiting/timer account maps |

The contract lifecycle distinguishes offered evidence, optional reversible
reservation evidence, winner linearization, irreversible winner commit,
loser/rollback release, and final authority closure. CandidateReady is not an
abstract primitive state.

## E. Action mapping

| Observable contract step | Central Claim realization | Adapter realization |
|---|---|---|
| register arm | `CentralRegisterArm` | begin plus Event/Timer registration |
| offer evidence | `CentralObserveCandidate` | admission observation, Event scan, Timer pump entry |
| linearize winner | `CentralClaimWinner` | admission, Event phase-2, or Timer claim |
| irreversible winner commit | `CentralCommitWinner` | Event winner or Timer winner finalization |
| release loser | `CentralReleaseLoser` | Event cancellation or Timer cancel-then-retire |
| close authority | `CentralCloseAuthority` | `CloseAdapterAuthority` |
| publish result/runnable | inline/suspended publish | only after every adapter arm is retired |
| rollback | begin/release/finish | Event rollback or Timer cancel-then-retire rollback |
| consume/destroy | central projection actions | adapter stutter |

Every Central Claim transition is a Contract transition or Contract stutter.
Every Event/Timer transition is a Central Claim transition or Central stutter.
The two mappings are top-level instantiated temporal properties, not
parameterized instance operators that TLC would silently omit.

## F. Event model

Events have real identities in `Events`; each Event arm has an `arm_event`
mapping. Event state belongs to an Event, not to an arm, and remains persistent
when a Select arm wins.

Post-suspension broadcast is explicitly two-phase:

1. acquire one Event coordination identity, set that Event, snapshot and scan
   every registered arm mapped to it, and offer CandidateReady evidence while
   WaitNodes remain linked;
2. finish the scan, release the held Event identity, then process the affected
   Central Claim group and finalize winner/losers.

`Reach_R8` requires at least two claim-snapshot arms mapped to the same recorded
`last_broadcast_event`; independent per-arm Set states cannot satisfy it.

## G. Timer model

Each independent Timer arm owns a TimerRegistration state. `TimerPumpEntry`
requires `Active` before it increments `timer_node_deref`, and retains a
modeled Timer coordination authority through group claim. Winner ordering is
claim, `Active -> Consumed`, then WaitNode expire/detach and context/accounting
closure. Loser ordering is WaitNode cancel/detach first, followed by
`Active -> Retired` and context/accounting closure.

`TimerPumpSkip` applies only to a due, terminal registration on a retired arm
and does not dereference the WaitNode. `Reach_R10` additionally requires the
actual skip-history bit and `timer_node_deref = 0`, so rollback retirement alone
is not a witness.

## H. Inline/suspended paths

Inline completion remains on a running caller, publishes one result and zero
runnable transitions, permits result consumption, and becomes destruction
eligible only after all arm authority closes.

Suspended completion requires the caller to enter Waiting before the winning
post-suspension Event/Timer action. After every winner and loser adapter is
retired, it publishes one result and exactly one Runnable transition; the
caller may then resume, consume, and destroy the operation.

R1/R2 and R11 witness the inline paths. R3/R4 and R12 witness the suspended
paths.

## I. Rollback

Rollback is allowed after partial completed registration and before winner
linearization. Event arms cancel and detach their WaitNode, clear context,
close waiting accounting, release abstract authority, and retire. Timer arms
first cancel/detach, then retire TimerRegistration, clear context, close both
accounts, release abstract authority, and retire.

Rollback completion requires every affected adapter to be Detached or Retired
and every abstract authority to be closed. It publishes neither result nor
runnable state. R9 binds the witness to `rollback_started`, a genuinely retired
registered arm, and zero publications.

## J. Reachability evidence

All reachability configs intentionally check an inverse invariant. Success is
the named `NotReach_*` violation, not an arbitrary invariant error.

| Obligation | Generated | Distinct | Queue at witness | Depth | Runtime |
|---|---:|---:|---:|---:|---:|
| Contract inline | 838 | 380 | 71 | 11 | <1 s |
| Contract reservation + suspended | 1,103 | 478 | 38 | 13 | <1 s |
| Central snapshot tie-break | 58 | 33 | 11 | 7 | <1 s |
| R1 Event admission inline | 24,734 | 8,919 | 1,690 | 15 | 1 s |
| R2 Timer admission inline | 33,733 | 11,705 | 1,487 | 17 | 1 s |
| R3 Event post-suspension | 37,872 | 12,962 | 1,294 | 18 | 1 s |
| R4 Timer post-suspension | 41,495 | 14,068 | 1,085 | 19 | 1 s |
| R5 Event winner + Timer loser | 14,782 | 5,636 | 1,511 | 13 | 1 s |
| R6 Timer winner + Event loser | 18,406 | 6,887 | 1,680 | 14 | 1 s |
| 3-arm mixed R5 variant | 205,107 | 69,871 | 22,161 | 16 | 4 s |
| R7 multi-candidate lowest-index | 674,566 | 230,698 | 95,750 | 16 | 10 s |
| R8 same-Event multi-arm | 18,152 | 6,809 | 1,696 | 14 | 1 s |
| R9 partial rollback | 874 | 380 | 186 | 7 | 1 s |
| R10 stale timer skip | 42,166 | 14,277 | 1,042 | 19 | 2 s |
| R11 inline consume | 29,864 | 10,514 | 1,594 | 16 | 1 s |
| R12 suspended consume | 44,424 | 14,953 | 901 | 20 | 2 s |

The queue column is TLC's remaining distinct-state queue when the expected
witness stopped exploration; it is not presented as an exhausted graph.

## K. TLC execution evidence

Author command:

```bash
TLC_WORKERS=1 tools/formal/verify-e13-select-core.sh
```

Runtime:

```text
TLC2 Version 2.19 of 08 August 2024 (rev: 5a47802)
```

| Positive/exhaustive gate | Generated | Distinct | Final queue | Diameter | Verdict |
|---|---:|---:|---:|---:|---|
| Contract semantics/coherence | 1,252 | 533 | 0 | 16 | PASS |
| Central + Contract refinement | 332 | 144 | 0 | 16 | PASS |
| Event/Timer + Central refinement | 53,617 | 17,472 | 0 | 30 | PASS |
| constrained 4-arm mixed root | 443,664 | 99,868 | 0 | 40 | PASS |

The runner creates a fresh metadir, requires the positive models and both
temporal refinement properties to pass, rejects parse/config/deadlock errors,
and verifies every reach probe fails only its expected named inverse invariant.
An additional exhaustive two-arm `-coverage 1` run reported non-zero coverage
for every non-stutter adapter action, including both registration kinds,
admission ready/not-ready, both claim sources, Event scan/empty scan, Timer
pump/skip, all winner/loser finalizers, rollback, both completion modes,
resume/consume/destroy, and Tick. The explicit stutter action creates zero
distinct transitions, as expected.

The four-arm root fixes one useful bounded topology—two Event arms sharing one
Event plus two independent Timers—to avoid an unhelpful unconstrained topology
cross-product. It does not disable registration, readiness, claim,
finalization, rollback, publication, consumption, or destruction actions.

## L. Abstraction boundaries

The Contract deliberately excludes every adapter and coordination mechanism.
It can therefore be refined later by a different strategy without replacing
the public Select laws. Central Claim excludes Event/Timer storage and can be
instantiated by an additional authorized adapter later without changing the
Contract.

The Event/Timer layer abstracts concrete list pointers, timer-heap storage,
worker routing, atomic memory orders, and actual mutex objects. It retains the
semantic authority transitions and orderings that matter to this phase. One
SelectGroup is modeled; same-Event deduplication inside that group is covered,
while cross-group broadcast scaling is not.

No alternative strategy or additional arm type is implemented in PR #17.

## M. Deferred safety work

PR #17 establishes a reachable, internally coherent positive contract, two
checked refinements, and concrete causal witnesses. It does not claim complete
E13 safety closure.

PR #18 remains responsible for the independently reviewed full safety suite
and negative models, including stale-pump mutations and other deliberate
ordering/publication faults. No liveness/fairness gate is added here.

## N. Scope verification

```text
PRODUCTION CODE CHANGES:
NONE

PRODUCTION TEST CHANGES:
NONE

PUBLIC API CHANGES:
NONE

BUILD POLICY CHANGES:
NONE

NEGATIVE MODELS:
NONE

ADDITIONAL ARM TYPES:
NONE
```

All task changes are confined to `docs/spec/e13_select/`, `docs/formal/`,
`docs/reviews/`, and `tools/formal/`. The pre-existing untracked production
test remains outside the task and untouched.

## O. Residual risks

- TLC evidence is bounded to the supplied finite domains and does not prove an
  unbounded arm count.
- The constrained four-arm topology is representative, not exhaustive over
  every arm-kind/Event-identity permutation; the exhaustive two-arm adapter
  gate and targeted scenarios cover the semantic actions.
- Refinement mappings intentionally use explicit shared projection variables;
  future strategies with different storage must supply their own mapping.
- One SelectGroup is modeled, so cross-group Event batching/deduplication is
  deferred.
- Concrete pointer safety, memory ordering, timer-heap implementation, and
  runtime scheduling remain production/refinement obligations outside PR #17.
- Independent review is still required before PR #18 authorization is acted
  upon.
