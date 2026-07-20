# E13 Select Formal Model Safety — Review Request

```text
E13-SELECT-FORMAL-MODEL-SAFETY-1:
PASS — AUTHOR SELF-ASSESSMENT

LAYERED SAFETY INVARIANTS:
IMPLEMENTED (ContractSafetyInv 32 laws / CentralSafetyInv 14 / AdapterSafetyInv 42)

REFINEMENT SAFETY:
IMPLEMENTED (PR #17 mappings unchanged; widened-domain check added)

FOCUSED NEGATIVE MODELS:
IMPLEMENTED (29 single-fault mutations + 3 restoration configs)

NON-VACUITY EVIDENCE:
IMPLEMENTED (20 inverse-reachability witnesses)

BOUNDED MULTI-GROUP NON-INTERFERENCE:
IMPLEMENTED (two-group shared-Event bounded model)

REPEATABLE VERIFICATION TOOLING:
IMPLEMENTED (tools/formal/verify-e13-select-safety.sh, source-safe)

PRODUCTION IMPLEMENTATION:
DENIED

PR #19 (production C++):
NOT AUTHORIZED — pending independent review of PR #18
```

## A. Baseline

```text
REPOSITORY:
jnhu76/Sluice

TASK:
E13-SELECT-FORMAL-MODEL-SAFETY-1

BASE_BRANCH:
master

BASE_COMMIT (PR #17 merge):
57435a913ad6d679df19a17f17f1c36e711dfc60

BRANCH:
feat/e13-select-formal-safety

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
CI policy unrelated to formal verification

PRE-EXISTING UNTRACKED FILES (must remain untracked / unchanged):
tests/test_t3_simple.cpp
tla2tools.jar
```

## B. Layered safety invariants

Three named aggregates are added alongside the PR #17 canonical `*Inv`
aggregates (which are preserved unchanged):

- `ContractSafetyInv` (32 laws, H1-H5 + I) in `E13SelectContract.tla`.
- `CentralSafetyInv` (14 laws, J) in `E13SelectCentralClaim.tla`.
- `AdapterSafetyInv` (K + L + M + N, 42 laws) in `E13SelectEventTimer.tla`.

Each law is named, scoped, and commented in the spec.  See
`docs/spec/e13_select/INVARIANTS.md` for the full table.

`E_InvEventPersistentSetNotConsumed` was strengthened from a placeholder
`TRUE` to a real state law (`e = last_broadcast_event => event_state[e]="Set"`)
so NEG-E4 can target it without vacuity.  Canonical safety and safety3mix
still PASS unchanged on the strengthened law (65171 / 8741747 states,
matching PR #17 baselines).

## C. Refinement safety

The PR #17 INSTANCE-WITH mappings are unchanged.  The new M accounting and
N history variables are pure extensions: they shadow canonical variables
but never replace them, so the WITH refinement clause is unaffected and
the temporal `RefinesContract` / `RefinesCentralClaim` properties still
hold.  See `docs/spec/e13_select/REFINEMENT.md` for the full mapping
tables.

PR #18 adds one widened-domain refinement check
(`E13SelectCentralClaim.refine3.cfg` — 3-arm admission-tie domain).
Wider adapter refinement PROPERTY checks (3-mix, 4-mix) blow up past the
5-minute TLC budget; the 3-arm adapter domain is instead exercised by
`AdapterSafetyInv` in `E13SelectEventTimer.safety3mix.cfg`.

## D. Focused negative models

29 single-fault mutations, one per named law, plus three FAULT="None"
restoration configs.  Each negative module wraps the canonical spec by
INSTANCE-WITH; the canonical `*Next` is reused unchanged via
`BaseNextFrozen`, and the wrapper's Next disjoins it with ONE focused
fault selected by the constant `FAULT`.  See
`docs/spec/e13_select/NEGATIVE_MODELS.md` for the full matrix.

- Contract: NEG-C1..C8 in `E13SelectContractNeg.tla`.
- Central: NEG-S1..S6 in `E13SelectCentralClaimNeg.tla`.
- Adapter: NEG-E1..E6 + NEG-T1..T5 + NEG-A1..A4 in
  `E13SelectEventTimerNeg.tla`.

Each fault is reachable from a legal state, mutates exactly the variables
its target law constrains, and is reported by TLC as
`Invariant <LawName> is violated`.  Each restoration cfg (FAULT="None")
reaches "completed, no error".

## E. Non-vacuity evidence

20 inverse-reachability predicates proving each named law's premise is
reachable in the canonical model.  See
`docs/spec/e13_select/NON_VACUITY.md`.  All 20 witnesses report
`Invariant NotReach_<X> is violated`.

## F. Bounded multi-group non-interference

`E13SelectMultiGroup.tla` models two 1-arm SelectGroups sharing a single
Event identity space.  It proves each group's winner, publication,
classification, authority closure, and TimerRegistration are isolated.  It
is a bounded non-interference proof, NOT an unbounded concurrency proof.
See `MGSafetyInv` (11 laws) + 3 reach witnesses.

## G. Repeatable verification tooling

`tools/formal/verify-e13-select-safety.sh` runs the full PR #18 suite
(65 distinct TLC runs) in an isolated `mktemp` workspace with a defensive
cleanup trap that verifies the path matches the expected pattern before
`rm -rf`.  See `docs/spec/e13_select/EVIDENCE_SAFETY.md`.

The verifier defines four expectation gates:

- `expect_pass` — positive safety aggregate (must reach "completed, no error").
- `expect_reach` — inverse-reachability / non-vacuity witness (must report
  the named Invariant violated).
- `expect_negative` — focused fault (must report the named Invariant violated).
- `expect_restored` — FAULT="None" restoration (must reach "completed, no error").

## H. Toolchain

```text
TLC2 Version 2.19 of 08 August 2024 (rev: 5a47802)
OpenJDK 25.0.3+9-2-26.04.2-Ubuntu
TLC_WORKERS=1 (deterministic; state order is reproducible)
```

## I. Evidence summary

| Category | Count | Status |
|----------|-------|--------|
| Layered safety aggregates (P) | 7 | all PASS |
| Multi-group non-interference (O) | 2 | PASS + REACH |
| Widened refinement (X) | 1 | PASS |
| Contract negative models (R) | 8 + 1 restore | all NEG + RESTORE |
| Central negative models (S) | 6 + 1 restore | all NEG + RESTORE |
| Adapter negative models (T/U/V) | 15 + 1 restore | all NEG + RESTORE |
| Per-law non-vacuity witnesses (W) | 20 | all REACH |
| PR #17 regression anchors | 5 | all REACH |

Total: 65 distinct TLC runs, all green.

## J. Residual risk

- **Bounded, not unbounded.**  Multi-group non-interference is proven for
  two groups sharing one Event identity only.  An unbounded concurrency
  proof (arbitrary groups, arbitrary Event identities, arbitrary Scheduler
  interleavings) is out of scope and would require a different proof
  strategy (e.g. TLA+ Proof System, or invariant decomposition).
- **Safety, not liveness.**  No fairness / liveness is claimed.  Termination
  and eventual completion are not proven.
- **Negative models are single-fault.**  Multi-fault interactions are not
  exhaustively explored.
- **Adapter refinement on wider domains.**  The temporal
  `RefinesCentralClaim` PROPERTY is checked only on the 2-arm domain; the
  3-arm / 4-arm adapter refinement PROPERTY blows up past the 5-minute TLC
  budget.  The wider adapter domain is covered by the `AdapterSafetyInv`
  aggregate in safety3mix, but not by the explicit PROPERTY check.

## K. Request

Independent review requested for:

1. **Invariant coverage.**  Are the 32 + 14 + 42 named laws the right
   coverage?  Any load-bearing law missing?
2. **Fault reachability.**  Are the 29 focused faults genuinely reachable
   from legal states (not vacuous)?
3. **Target match.**  Does each fault break exactly its target law (not a
   different invariant)?
4. **Source safety.**  Is the mktemp isolation + defensive cleanup trap
   sufficient?  Any path where the verifier could mutate the source tree?
5. **Scope adherence.**  Confirm no path under `include/`, `src/`,
   `tests/`, `examples/`, `benchmarks/`, public API, build policy, or CI
   policy was touched, and that `tests/test_t3_simple.cpp` and
   `tla2tools.jar` remain untracked and unchanged.

Run `tools/formal/verify-e13-select-safety.sh` to reproduce.
