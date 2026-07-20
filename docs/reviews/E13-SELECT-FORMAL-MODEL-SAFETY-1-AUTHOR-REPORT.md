# E13 Select Formal Model Safety — Author Report (PR #18)

**Task:** `E13-SELECT-FORMAL-MODEL-SAFETY-1`
**Branch:** `feat/e13-select-formal-model-safety`
**Base:** `57435a913ad6d679df19a17f17f1c36e711dfc60` (PR #17 merge)
**Head:** `f433bcd` (this PR's tip)

## Outcome

**=== PASS: E13 Select layered formal SAFETY suite (PR #18) ===**

PR #18 closes the safety foundation of the E13 Select formal core.  All 65
gated TLC runs are green on tip.  No production code, public API, build
policy, or CI policy was touched.

## Commits (10)

```
f433bcd formal(e13): add PR #18 safety docs, review request, verifier target fixes
6fe064c formal(e13): add widened refinement check + PR #18 safety verifier (X, Y)
1bf2b33 formal(e13): add per-law non-vacuity witness matrix (W)
cb836d2 formal(e13): add Event/Timer/Accounting focused negative models
ad6040e formal(e13): add Central Claim focused negative models
33acca1 formal(e13): add layered focused negative models
87a6110 formal(e13): add two-group non-interference model
799cf22 formal(e13): add adapter safety, exactly-once accounting, history
7a56e31 formal(e13): add Central Claim and adapter safety history
aaa87d7 formal(e13): add abstract Contract safety invariants
```

## Diff scope

```
79 files changed, 4831 insertions(+), 37 deletions(-)
```

Every touched path is under `docs/` or `tools/`:

```
docs/formal/
docs/reviews/
docs/spec/
tools/formal/
```

No path under `include/`, `src/`, `tests/`, `examples/`, `benchmarks/`,
public API, production build policy, or CI policy was modified.

Pre-existing untracked files preserved:
- `tests/test_t3_simple.cpp` (still `??`)
- `tla2tools.jar` (still `??`)

## Deliverables matrix

| Deliverable | Letter | Status | Count |
|-------------|--------|--------|-------|
| Layered safety invariants (H/I/J/K/L/M/N) | B-F | done | 88 named laws |
| Two-group bounded non-interference | O | done | MGSafetyInv (11 laws) + 3 reach witnesses |
| Contract negative models | R | done | NEG-C1..C8 + restore |
| Central negative models | S | done | NEG-S1..S6 + restore |
| Adapter negative models | T/U/V | done | NEG-E1..E6 + NEG-T1..T5 + NEG-A1..A4 + restore |
| Positive configurations | P | done | 7 layered safety aggregates |
| Non-vacuity matrix | W | done | 20 inverse-reachability witnesses |
| Refinement checks | X | done | 1 widened-domain cfg + mapping docs |
| Reproducible verifier | Y | done | `verify-e13-select-safety.sh` (4 gates) |
| Safety docs | (docs) | done | INVARIANTS / NEGATIVE_MODELS / NON_VACUITY / REFINEMENT / EVIDENCE_SAFETY + design + plan |
| README status | AA | done | PR #18 section appended |
| Review request | Z/AB | done | `E13-SELECT-FORMAL-MODEL-SAFETY-1-REVIEW-REQUEST.md` |
| Final validation | AD/AF/AG | done | scope audit clean, untracked preserved |

## Toolchain

```
TLC2 Version 2.19 of 08 August 2024 (rev: 5a47802)
OpenJDK 25.0.3+9-2-26.04.2-Ubuntu
TLC_WORKERS=1 (deterministic)
```

## Verification commands

```bash
# PR #17 regression (must still PASS):
TLC_WORKERS=1 ./tools/formal/verify-e13-select-core.sh

# PR #18 safety suite (must PASS):
TLC_WORKERS=1 ./tools/formal/verify-e13-select-safety.sh
```

## Known limitations (carried into review request)

- **Bounded, not unbounded.**  Multi-group non-interference is two-group /
  one-Event-identity only.
- **Safety, not liveness.**  No fairness / termination claim.
- **Single-fault negative models.**  Multi-fault interactions not
  exhaustively explored.
- **Adapter refinement on wider domains.**  The temporal
  `RefinesCentralClaim` PROPERTY is checked only on the 2-arm domain;
  wider adapter refinement PROPERTY blows up past the 5-minute TLC budget.
  The wider adapter domain is covered by `AdapterSafetyInv` in safety3mix.

## Request

Independent review of invariant coverage, fault reachability, target
match, source safety, and scope adherence.  See
`docs/reviews/E13-SELECT-FORMAL-MODEL-SAFETY-1-REVIEW-REQUEST.md`.
