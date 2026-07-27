# E13 Select Formal Model Core — Corrective-1 Review Request

This is an author-facing **corrective** review request. It does **not** rewrite
the historical verdict in
`E13-SELECT-FORMAL-MODEL-CORE-1-INDEPENDENT-REVIEW-1.md`, which remains a
`REQUEST-CHANGES` artifact. It asks the independent reviewer for a **narrow
delta re-audit** of the corrective-1 changes against the originally reviewed
PR #17 head.

The independent reviewer should produce a new artifact, for example
`E13-SELECT-FORMAL-MODEL-CORE-1-CORRECTIVE-1-DELTA-REVIEW-1.md`.

## A. Binding

```text
REPOSITORY:
jnhu76/Sluice

PR:
https://github.com/jnhu76/Sluice/pull/17

TASK:
E13-SELECT-FORMAL-MODEL-CORE-1-CORRECTIVE-1

ORIGINAL REVIEWED HEAD:
e61a0b4971758a819d8ba3b94f3e3147b34c4bc1

CORRECTIVE HEAD:
the tip of branch feat/e13-select-formal-model-core after the four corrective
commits (see `git log e61a0b4..HEAD`). The exact SHA is recorded in the
author's final self-assessment; re-resolve it at review time with
`git rev-parse origin/feat/e13-select-formal-model-core`.

BASE:
246505a1de2ffa2840ab404adc76a2839d4ae069

BRANCH:
feat/e13-select-formal-model-core

ORIGINAL VERDICT (unchanged, historical):
E13-SELECT-FORMAL-MODEL-CORE-1-INDEPENDENT-REVIEW-1 = REQUEST-CHANGES
```

The corrective diff is `e61a0b4..HEAD` (four commits). The base diff for the
whole PR remains `246505a..HEAD`.

## B. Scope of this corrective

This corrective only fixes the issues found in the independent review. It does
**not**:

- redesign the accepted three-layer architecture (Contract / Central Claim /
  Event-Timer);
- start PR #18 safety/negative models;
- implement any production C++, public API, or build-policy change;
- model post-suspension cancellation, shutdown cancellation, user-requested
  Select cancellation, or whole-Select timeout;
- add Semaphore/Mutex/Queue/Condition adapters or alternative selection
  strategies.

The pre-existing untracked `tests/test_t3_simple.cpp` and `tla2tools.jar` are
user assets and are **not** part of this corrective; they remain untracked and
untouched.

## C. Requested narrow delta re-audit

Please re-audit only the following seven deltas. Each maps to a corrective
change set; nothing outside these is requested for re-audit.

1. **Rollback domain.** `ContractBeginRollback` is now enabled only on
   `ContractRollbackEnabledDomain`
   (`contract_phase = "Building"` /\ `winner = NoArm` /\
   `caller_state = "Running"`). `"Selecting"` is no longer an allowed rollback
   phase. Confirm rollback cannot begin after `FinishRegistration`,
   `ContractSuspendCaller`, winner claim, or winner commit.

2. **Refinement propagation.** `CentralBeginRollback` requires
   `central_phase = "Registering"` (`"Admission"` removed); the concrete
   `BeginRollback` in `E13SelectEventTimer` requires
   `central_phase = "Registering"` /\ `contract_phase = "Building"` /\
   `caller_state = "Running"` /\ `winner = NoArm` and additionally forbids any
   arm still mid-registration. Confirm a partially-registered subset remains
   rollable while a post-suspension state does not.

3. **Terminal caller invariant.** New named invariants
   `TerminalCallerStateWellFormed`, `NoBadTerminalWaiting` (forbidding
   `caller_state = "Waiting"` in `Aborted`/`Destroyed`), and three-layer
   `*RegistrationRollbackDisabledAfterSuspension` invariants are added to
   `ContractInv` / `CentralInv` / `EventTimerInv`. Confirm no transition
   silently moves `Waiting -> Running` during rollback.

4. **R9 preservation.** `scene_rollback.cfg` still reaches `Reach_R9` through a
   genuine strict-subset partial registration
   (`0 < registration_count < MaxArms`), with every registered arm
   Retired/Released/authority-closed and every unregistered arm Detached/None.
   Confirm the witness is not fabricated and the caller stays `Running`.

5. **Rejected post-suspension rollback trace.** A dedicated
   `E13SelectContract.rollback_regression.cfg` positive gate (added to the
   verifier) exercises `TerminalCallerStateWellFormed`,
   `NoBadTerminalWaiting`, and `ContractRegistrationRollbackDisabledAfterSuspension`.
   Confirm the previously rejected `Aborted + Waiting` trace is unreachable and
   that the gate is a real check, not a state-constraint suppression.

6. **Source-safe verifier.** `tools/formal/verify-e13-select-core.sh` now copies
   the spec into a `mktemp` work directory, runs every TLC with a private
   `-metadir` inside that root, and `cleanup` only removes the temporary root
   after a defensive path-shape check. Confirm no `rm` targets `$spec` and that
   a source-preservation sentinel survives a full run.

7. **Config/document drift and file scope.** The stale
   `E13SelectContract.reach.cfg` reference is corrected; scene cfg descriptions
   match their actual named witnesses (R1/R2, R3/R4, R5/R6, R9, R10 corrected);
   README/design/plan document the registration-rollback vs post-suspension
   boundary; seven EOF blank-line warnings are removed; `git diff --check` is
   clean. Confirm no `include/**`, `src/**`, `tests/**`, or build-policy file is
   touched and no PR #18 negative model is introduced.

## D. Files changed by this corrective (delta vs original reviewed head)

Model (rollback domain + terminal invariants):

- `docs/spec/e13_select/E13SelectContract.tla`
- `docs/spec/e13_select/E13SelectCentralClaim.tla`
- `docs/spec/e13_select/E13SelectEventTimer.tla`

Configuration and evidence:

- `docs/spec/e13_select/E13SelectContract.rollback_regression.cfg` (new)
- `docs/spec/e13_select/E13Select.scene_rollback.cfg`
- `docs/spec/e13_select/E13Select.scene_staletimer.cfg`
- `docs/spec/e13_select/E13Select.scene_inline.cfg`
- `docs/spec/e13_select/E13Select.scene_suspended.cfg`
- `docs/spec/e13_select/E13Select.scene_2mix.cfg`
- EOF-only: `E13SelectCentralClaim.cfg`, `E13SelectCentralClaim.reach.cfg`,
  `E13SelectContract.cfg`, `E13SelectContract.reach_inline.cfg`,
  `E13SelectContract.reach_suspended.cfg`, `E13SelectEventTimer.cfg`

Tooling (source isolation):

- `tools/formal/verify-e13-select-core.sh`

Documentation:

- `docs/spec/e13_select/README.md`
- `docs/formal/e13-select-formal-core-design.md`
- `docs/formal/e13-select-formal-core-plan.md`

Review artifacts:

- this file
- `docs/reviews/E13-SELECT-FORMAL-MODEL-CORE-1-INDEPENDENT-REVIEW-1.md`
  (baseline-binding corrections only; the `REQUEST-CHANGES` verdict is preserved)

No file under `include/**`, `src/**`, `tests/**`, or any build-policy path is
modified.

## E. Validation evidence (corrective-1 run)

```text
TLC2 Version 2.19 of 08 August 2024 (rev: 5a47802)
OpenJDK 25.0.3
TLC_WORKERS=1
```

Reproduced with `tools/formal/verify-e13-select-core.sh` (21 gates, all PASS /
expected named witnesses). Source-preservation sentinel
(`docs/spec/e13_select/E13SelectUserTTrace.keep`) survived the run and was
removed before commit; no `TTrace`/metadir artifact was written into
`docs/spec/e13_select/`.

| Gate | Generated | Distinct | Queue at stop | Depth | Result |
|------|----------:|---------:|--------------:|------:|--------|
| Contract semantics | 809 | 346 | 0 | 16 | PASS |
| Contract registration-rollback regression | 809 | 346 | 0 | 16 | PASS |
| Central + Contract refinement | 253 | 111 | 0 | 16 | PASS |
| Event/Timer + Central refinement | 24,761 | 8,432 | 0 | 30 | PASS |
| 4-arm bounded mixed root | 71,868 | 17,108 | 0 | 40 | PASS |
| Contract inline reach | 530 | 240 | 43 | 11 | expected witness |
| Contract suspended reach | 691 | 299 | 23 | 13 | expected witness |
| Central tie-break reach | 55 | 31 | 9 | 7 | expected witness |
| R1 Event admission inline | 12,358 | 4,430 | 520 | 15 | expected witness |
| R2 Timer admission inline | 14,867 | 5,305 | 493 | 17 | expected witness |
| R3 Event post-suspension | 16,167 | 5,763 | 482 | 18 | expected witness |
| R4 Timer post-suspension | 17,508 | 6,231 | 448 | 19 | expected witness |
| R5 Event winner + Timer loser | 9,274 | 3,389 | 587 | 13 | expected witness |
| R6 Timer winner + Event loser | 10,565 | 3,808 | 554 | 14 | expected witness |
| 3-arm mixed R5 variant | 87,437 | 27,074 | 4,130 | 16 | expected witness |
| R7 snapshot tie-break | 453,829 | 134,998 | 35,771 | 16 | expected witness |
| R8 same-Event multi-arm | 10,498 | 3,781 | 558 | 14 | expected witness |
| R9 partial-registration rollback | 865 | 380 | 186 | 7 | expected witness |
| R10 stale Timer skip | 17,819 | 6,346 | 442 | 19 | expected witness |
| R11 inline consume | 13,797 | 4,932 | 501 | 16 | expected witness |
| R12 suspended consume | 18,655 | 6,618 | 403 | 20 | expected witness |

The positive Contract, Central, Event/Timer, and rollback-regression configs
were also run independently outside the runner with explicit `-metadir`; all
reproduced the same metrics and verdicts.

## F. Self-assessment verdict

```text
E13-SELECT-FORMAL-MODEL-CORE-1-CORRECTIVE-1:
PASS — AUTHOR SELF-ASSESSMENT

P0 CONTRACT ROLLBACK:
CORRECTED

REGISTRATION ROLLBACK DOMAIN:
BUILDING / REGISTERING ONLY

POST-SUSPENSION ROLLBACK:
UNREACHABLE

TERMINAL WAITING CALLER:
UNREACHABLE

R9 PARTIAL REGISTRATION ROLLBACK:
REACHABLE

CONTRACT REFINEMENT:
PASS

CENTRAL CLAIM REFINEMENT:
PASS

EVENT/TIMER ADAPTER REFINEMENT:
PASS

R1–R12:
PASS

SOURCE-SAFE VERIFIER:
PASS

SOURCE SENTINEL PRESERVATION:
PASS

CONFIG/DOCUMENT DRIFT:
CLOSED

PR #18:
DENIED PENDING INDEPENDENT DELTA REVIEW

PRODUCTION IMPLEMENTATION:
DENIED
```

This self-assessment does not authorize PR #18 or production implementation.
Both remain denied pending the independent delta review requested above.

## G. Residual risks

- TLC evidence is bounded to the supplied finite domains (2/3/4 arms, 2
  Events); it does not prove unbounded arm counts.
- Post-suspension cancellation, shutdown cancellation, user-requested Select
  cancellation, and whole-Select timeout remain explicitly unmodelled and
  deferred to PR #18; this corrective makes them *provably unreachable* through
  the registration-rollback path but does not implement them.
- The constrained four-arm topology is representative, not exhaustive over
  every arm-kind/Event-identity permutation.
- Concrete pointer safety, memory ordering, timer-heap implementation, and
  runtime scheduling remain production/refinement obligations outside PR #17.
