# #524 closeout

The independent closeout review of the pushed head `0070c1da` returned
`APPROVE_CLOSEOUT` with no new blocking item, on the basis of the receipts and
diffs recorded in this directory. The owner's own close-out record follows.

```text
CLOSEOUT_VERDICT          = PASS
MODEL_STATUS              = FULL_TLC_PASS
ISSUE_524_CLOSE_READY     = YES
RESOLUTION                = ReleaseBind removed; mutation gates migrated;
                            formal safety/liveness receipts and evidence
                            classification aligned.
```

## What the closeout verified

| Closeout condition | Result |
|---|---|
| `ReleaseBind` removal and the mutation migration | Passed in the earlier rounds; no model or cfg change in this round |
| Complete safety | PASS — 5,359,732 distinct states, queue drained, depth 53 |
| Complete L1/L5 liveness | PASS — the formal log contains the temporal check of the **complete** graph and then a normal completion |
| Provenance of the formal run | Log, pid `65752`, timestamps, exit code 0, frozen hashes and run metadata all agree |
| Separation of the stopped attempt from the result | The old partial log is retained; the formal receipt is separate and unambiguous |
| Classification, ledger and disposition records | Consistent at `FULL_TLC_PASS` |

The decisive lines in `raw/tlc/liveness.log` are `0 states left on queue`,
`Checking 12 branches of temporal properties for the complete state space`,
`Finished checking temporal properties`, and
`Model checking completed. No error has been found.`; the run metadata records
exit code 0, and the diff from the frozen model head to the reviewed head is
`docs/` only, so the verified object did not drift. The unsupported liveness
PASS that the implementer found in `safety-preflight.md` during this phase was
corrected with a provenance note before this review.

## Scope of `FULL_TLC_PASS` (not widened by the closeout)

`FULL_TLC_PASS` is a statement about the **RequestCore slice only**: the
converged `RequestCore.tla` under the established finite bounds (`Slots` = 2,
`GenMax` = 2, `MaxExec` = 2, `MaxCtl` = 1) and the declared fairness assumptions
(the six `WF_vars` conjuncts of `FairSpec`), checked by the two complete gates
recorded in `resource-and-execution-manifest.md`. It is **not** a verdict on the
repository's whole formal matrix: the end-to-end `scripts/verify_tla.sh` run
(ten models, ~170 configurations) was not executed, and the closeout review
confirmed that neither it nor a C++ build is required for this issue, because
C++ production is byte-identical to BASE and the other models are untouched.

## What the closeout does not claim

- No refinement proof and no claim beyond the two disclosed, pre-existing
  abstractions (`Reclaim` carries no observer-phase pin, which ObserverCore
  models; the model has no zero-op accept action).
- No C++ behaviour claim, no root-spec change, and no change to the frozen
  F2/E2/F6 evidence — the closeout adds only this record, the index row and the
  ledger's closeout line.
- No authorization to repeat or extend the run: the receipts are the ones
  committed here, and any future liveness violation in any run of this
  configuration would reopen the corresponding obligation.
