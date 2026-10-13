# F2 `rcore-live` reconciliation — historical evidence inheritance

Recorded per the F2 closeout instruction. Status block:

```ini
CURRENT_RCORE_LIVE_EXECUTION = INCOMPLETE
HISTORICAL_RCORE_LIVE_PROOF = VERIFIED_REUSABLE
F2_FORMAL_ACCEPTANCE = ACCEPTED_BY_REUSE
```

The F2 capture's own run never finished `rcore-live` (`verdicts.json`: 170/171
PASS, none failing, `rcore-live` outstanding at a documented ≈18.5 h /
≈40–48 GB configuration); it is recorded as INCOMPLETE and never as a pass.

## Exact identity of the inherited proof

The historical completed result is the E2 full-gate attempt 2 at master
`eb7bf055` (PR #453; recorded in `docs/roadmap/v1-conformance.md`, SHUT row):
`VERIFY_TLA: PASS` with **all 171 jobs** as expected and 0 FAIL — including
`rcore-live` on the complete 26,662,372-state behavior graph, after the solo
probe (-Xmx40g, same worker count) had independently checked the same graph
cleanly in 2h50m.

Reuse conditions, verified at the F2 final corrective head:

| Input | Identity check |
|---|---|
| `formal/tla/RequestCore.tla` | `git diff eb7bf055..HEAD -- formal/tla/ scripts/verify_tla.sh` is empty; SHA-256 `ce867f9e…` unchanged since the proof |
| `formal/tla/RequestCoreLive.cfg` | same empty diff; constants (all 22 mutant switches FALSE), `SPECIFICATION FairSpec`, 21 invariants and `PROPERTY L1 L5` byte-identical |
| `scripts/verify_tla.sh` | same empty diff; pinned tla2tools v1.7.4, SHA-256 `936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88` |
| Checked properties | L1, L5 plus the RequestCore invariant set above |
| Historical completed result | E2 attempt 2, all 171 jobs including `rcore-live`, `VERIFY_TLA: PASS` |

## Correspondence to the current C++ semantic authority

- F2 changed public spellings (`Request<T>`/`RequestId`/`lookup`), no modeled
  action; `correspondence.md` already records this mapping.
- The F2 final corrective round changes no modeled transition either: the
  io_uring correction is transport-local (the backend dispatch queue and its
  post-poison drain; the models do not cover backend transports and the fix
  calls only the already-modeled core actions `claim_execution` /
  `offer_terminal` / `release_execution`), the E1 gate-lifetime correction is
  test-only, and the `retire_delivery` guard changes behavior only for a
  moved-from context object state no model represents.

No correspondence table entry changes and no newly required formal check
arises from the corrective diff.

## Independent review obligation

This inheritance is subject to the F2 final independent adversarial review
(`../independent-review.md` and the closeout receipt): the reviewer must
confirm the identity table above and that the corrective diff introduces no
modeled-action change.
