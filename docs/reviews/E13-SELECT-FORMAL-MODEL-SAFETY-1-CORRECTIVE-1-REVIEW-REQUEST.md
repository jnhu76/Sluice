# E13 Select Formal Model Safety — Corrective-1 Review Request

This is an author-facing **corrective** review request.  It does **not**
rewrite the historical verdict in
`E13-SELECT-FORMAL-MODEL-SAFETY-1-INDEPENDENT-REVIEW-1.md`, which remains a
`REQUEST-CHANGES` artifact.  It asks the independent reviewer for a **narrow
delta re-audit** of the corrective-1 changes against the originally reviewed
PR #18 head.

The independent reviewer should produce a new artifact, for example
`E13-SELECT-FORMAL-MODEL-SAFETY-1-CORRECTIVE-1-DELTA-REVIEW-1.md`.

## A. Binding

```text
REPOSITORY:
jnhu76/Sluice

PR:
https://github.com/jnhu76/Sluice/pull/18

TASK:
E13-SELECT-FORMAL-MODEL-SAFETY-1-CORRECTIVE-1

ORIGINAL REVIEWED HEAD:
0f88ab9d2c19b8249df4e071c7a0662ce549ca9a

CORRECTIVE HEAD:
the tip of branch feat/e13-select-formal-model-safety after the corrective
commits (see `git log 0f88ab9..HEAD`).  The exact SHA is recorded in the
author's final self-assessment; re-resolve it at review time with
`git rev-parse origin/feat/e13-select-formal-model-safety` (or whichever
branch the corrective is published to).

BASE:
57435a913ad6d679df19a17f17f1c36e711dfc60

BRANCH:
feat/e13-select-formal-model-safety

ORIGINAL VERDICT (unchanged, historical):
E13-SELECT-FORMAL-MODEL-SAFETY-1-INDEPENDENT-REVIEW-1 = REQUEST-CHANGES
```

The corrective diff is `0f88ab9..HEAD`.  The base diff for the whole PR
remains `57435a9..HEAD`.

## B. Scope of this corrective

This corrective only fixes the issues found in the independent review.  It
does **not**:

- implement PR #19 (production C++ Event/Timer adapters);
- replace the Central Claim with an alternative Select strategy;
- introduce new arm types, new completion modes, post-suspension
  cancellation, or any new liveness/starvation claim;
- modify any path under `include/`, `src/`, `tests/`, `examples/`,
   `benchmarks/`, public API, build policy, or unrelated CI;
- relax the PR #17 canonical rollback semantics (rollback is still
   registration-only: `Registering`/`Building` only, `Running` caller only,
   `winner = NoArm` only, post-suspension cancellation NOT modelled).

The corrective only edits files under:

- `docs/spec/e13_select/**`
- `docs/formal/**`
- `docs/reviews/**`
- `tools/formal/**`

## C. Original review disposition

The original review returned `REQUEST-CHANGES`.  Each must-fix item from
that review is addressed by a specific corrective change; the mapping is
recorded in the author's self-assessment
(`E13-SELECT-FORMAL-MODEL-SAFETY-1-CORRECTIVE-1-AUTHOR-REPORT.md`, section B).
The corrective is a *fix-up* of the original PR, not a replacement: the
original positive aggregates, refinement PROPERTY checks, focused negative
models, and non-vacuity witnesses all remain, with the corrective adding
the load-bearing observers, three new focused negative gates (C9, S7, MG1),
widened accounting TypeOK + no-TypeOK double-check, and the multi-group
registration-split that makes rollback genuinely reachable.

## D. What the corrective changes

The high-level shape of the corrective:

1. **Multi-group registration split (P1-1, P1-2).**  `RegisterEventArm` /
   `RegisterTimerArm` no longer advance `g_phase` to `Admission`; they
   install the arm kind/identity and open wait/authority/account while
   keeping `g_phase = "Registering"`.  A new `FinishRegistration(g)` advances
   to `Admission`.  This makes the canonical rollback path genuinely
   reachable from a post-install pre-finish state, exercises the 9 new
   rollback invariants, and is witnessed by 5 independent cfgs (3 reach + 2
   non-vacuity).

2. **Three-layer frozen observer propagation (P1-3, P1-4).**  Contract
   carries `linearized_winner` / `linearized_winner_valid`; Central carries
   `claim_snapshot_frozen` / `claim_snapshot_frozen_valid`.  Both pairs
   propagate through the refinement INSTANCE-WITH chain (Contract → Central
   → EventTimer for `linearized_*`; Central → EventTimer for
   `claim_snapshot_*`).  The Contract winner-commit laws and Central
   snapshot laws are rewritten to depend on these frozen values, so they
   prove *immutability* and *identity stability* directly, not just live
   winner/snapshot membership.

3. **Widened accounting TypeOK (P2).**  The four accounting counters' TypeOK
   domain widens from `0..1` to `AccountingCountT == 0..2`.  The at-most-once
   `A_Inv*` laws are now the *only* source of the `<= 1` bound.  This lets
   `NEG-A1`/`NEG-A2` push a counter to 2 and isolate the law WITHOUT
   tripping `EventTimerTypeOK`; the verifier's new
   `expect_negative_no_typeok` double-check confirms the isolation.

4. **Mechanical cleanup (§I1, §I2).**  `ReachContractLoserExists` now
   references only the type-correct resolution value `Released` (the
   historical `"Aborted"` alternative was never a `ResolutionT` member).
   The Event/Timer/Accounting negative-model family label is corrected from
   `(T/U/V)` to `(E/T/A)` everywhere it appears.

5. **New focused negative gates.**  `NEG-C9` (winner identity flip),
   `NEG-S7` (snapshot member addition, 3-arm cfg), `NEG-MG1` (cross-group
   authority mutation during rollback, new `E13SelectMultiGroupNeg.tla`
   wrapper).  Each has its own FAULT="None" restoration cfg.

6. **New non-vacuity witnesses.**  Contract winner-linearized-not-committed,
   Central frozen-snapshot-valid + multi-candidate-snapshot, Adapter
   account-close-count-1 (widened), and two Multi-group registration-split
   witnesses.

7. **Portable tmpdir hardening (§I3).**  `verify-e13-select-core.sh` cleanup
   trap is rewritten to combine the non-empty check, the prefix check, and
   the `rm` into a single guarded command so `set -e` cannot short-circuit
   between the check and the `rm`.  Same form already used by the safety
   verifier.

## E. Verification

Reproduce the full corrective-1 gate:

```bash
TLC_WORKERS=1 tools/formal/verify-e13-select-core.sh    # PR #17 regression (unchanged)
TLC_WORKERS=1 tools/formal/verify-e13-select-safety.sh  # PR #18 + corrective-1
```

Both must reach their final `=== PASS ===` line with no `FAIL` lines
printed.  The corrective-1 gate count grows from 65 to 78 TLC runs; the
script's success criterion is the absence of `FAIL` lines, not a hard-coded
total.

Toolchain pin:

```text
TLC2 Version 2.19 of 08 August 2024 (rev: 5a47802)
OpenJDK 25.0.3+9-2-26.04.2-Ubuntu
TLC_WORKERS=1 (deterministic; state order is reproducible)
```

## F. Request

Independent delta review requested for:

1. **Frozen observer propagation.**  Are the new `linearized_*` /
   `claim_snapshot_*` variables correctly bound at every layer of the
   refinement chain?  Is the single-claim / single-operation assumption
   (multi-epoch identity deferred) clearly documented and sound?
2. **Multi-group registration split.**  Is the new `FinishRegistration`
   action + `Registering`-state rollback genuinely reachable, and does the
   9-rollback-invariant set match the canonical PR #17 rollback semantics?
3. **Widened accounting + no-TypeOK double-check.**  Does widening
   TypeOK to `0..2` isolate the at-most-once laws without weakening any
   other invariant?  Is the `expect_negative_no_typeok` double-check
   correctly implemented?
4. **New focused negative gates.**  Are `NEG-C9`, `NEG-S7`, `NEG-MG1`
   genuinely reachable from legal states?  Do they break exactly their
   target laws?
5. **Scope adherence.**  Confirm no path under `include/`, `src/`,
   `tests/`, `examples/`, `benchmarks/`, public API, build policy, or CI
   policy was touched, and that `tests/test_t3_simple.cpp` and
   `tla2tools.jar` remain untracked and unchanged.
6. **Rejected PASS draft.**  Confirm the rejected
   `E13-SELECT-FORMAL-MODEL-SAFETY-1-INDEPENDENT-REVIEW-1.md` PASS draft
   was NOT committed (it is deleted in the corrective, not preserved).

The author's self-assessment lives at
`E13-SELECT-FORMAL-MODEL-SAFETY-1-CORRECTIVE-1-AUTHOR-REPORT.md`.
