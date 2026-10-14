# F6 Final Acceptance Decision and Closeout Receipt (2026-10-15)

Owner decision packet for `F6_FINAL_CLOSEOUT`. Every value below is a measured
result, not a target copied from the task brief; the frozen content baseline is
`821c489fb081308d5ef95b13a14cc6435ac78d41` and the closeout is documentation-
only relative to it.

## Decision packet

```ini
F6_TECHNICAL_AUDIT = VERIFIED              (99d2d4b1, P0=0 P1=0, 16/16 gates)
F6_EVIDENCE_PROVENANCE = PASS              (local-only 99d2d4b1 recovered verbatim, pushed, API-verified)
F6_REQUIREMENT_COVERAGE = 113/113          (recounted from root headings; set-identical)
F6_FORMAL_ASSET_CLASSIFICATION = 26/26     (3 axes)
F6_FINDINGS_DISPOSITION = 18/18            (findings-disposition.md)
F6_REQUIRED_CLAIMS_INVALID = 0             (closeout-gates.json reverse enumeration)
F6_P0 = 0
F6_P1 = 0
F6_HARD_GATES = PASS_WITH_CONDITIONAL_W04_NA

SUFFICIENT_CONTRACTED_SURFACE_ESTABLISHED = YES
SCOPED_LOCALLY_IRREDUCIBLE = NOT_CLAIMED
GLOBALLY_MINIMAL_SURFACE = NOT_CLAIMED

V1_RELEASE_READY = NO
RELEASE_AUTHORIZATION = NOT_GRANTED
```

## Evidence basis (what an outside reviewer re-checks)

1. **Provenance** — original audit commit `99d2d4b1` was local-only (child of
   `origin/master` `fdb4b743`, never pushed); carried verbatim on
   `codex/f6-final-closeout-523`; GitHub API resolves it and raw evidence is
   readable at the exact commit URL. Nine deliverables + `raw/` complete;
   113 unique identifiers; 26 formal assets; 18 findings; 16 gates verbatim
   against #402's list.
2. **Findings** — all 18 dispositioned with the mandated field set and allowed
   vocabulary; deferred maintenance grouped into #524/#525/#526/#527/#528.
3. **F6-F03** — model superset is conservative for safety *and* liveness
   (ReleaseBind carries no fairness conjunct — independently re-derived by the
   closeout review); reuse premises byte-verified at the frozen tree; debt
   tracked in #524 with reopen triggers.
4. **F6-F04** — the fresh `RequestCoreLive.cfg` run **completed clean** at the
   frozen tree on 2026-10-15 01:33 +0800: COMPLETE_PASS, 153,347,909 states
   generated / 26,662,372 distinct / 0 on queue / depth 53 / 3h10m / "Model
   checking completed. No error has been found.", all 12 temporal-property
   branches clean on the complete space; the complete graph is exactly the
   E2-dated receipt's 26,662,372 states (`rcore-live-final-status.md`). The
   REQ-06/L1 closure authority remains the E2 receipt under F2's
   historical-reuse ruling; the fresh run is supplementary recency evidence
   that independently confirms the reuse premises, not a backdated
   replacement.
5. **E1** — submit path shares the direct path's semantic oracle (no divergent
   errno table); bad_alloc/slot-capacity → no_space conflation confined to the
   unsupported W-04 candidate host (mandatory precondition in #526).
6. **Independent closeout review** — APPROVE_AFTER_CORRECTION at `938b63aa`;
   all seven corrections applied and scoped re-review recorded below.

## Owner acceptance

`ACCEPT_F6_FINAL_CLOSEOUT` — recorded 2026-10-15. The owner's standing
directive for this closeout (task F6_FINAL_CLOSEOUT, TARGET
`F6_CLOSED_ACCEPTED`) authorizes the final phase closure — merge of this
documentation-only closeout PR and the #523/#402 receipts — conditional on the
gates above holding, the independent closeout review passing, and the
post-merge verification succeeding. It grants no release authorization:
`V1_RELEASE_READY=NO`, `RELEASE_AUTHORIZATION=NOT_GRANTED`, #390 remains open
under its own gates. All three conditions were met (gates above; review below;
post-merge receipt below).

## Independent closeout review (R4)

First pass: **APPROVE_AFTER_CORRECTION** (7 corrections, none altering a gate
value; verbatim record in `independent-closeout-review.md`). All corrections
applied; scoped re-review of the corrected record returned
**APPROVE_CLOSEOUT** — re-review record appended below.

## Re-review of corrections (same independent reviewer, 2026-10-15)

Re-review scope: the seven corrections only (rcore-live-final-status.md +
F6-F04 row; independent-closeout-review.md existence; invariance wording;
#523 typo edit + reworded typo_note; matrix parentheticals W-04/HOST-01/
HOST-03; overlay application note; 18/18 re-verification).

RE_REVIEW_VERDICT = APPROVE_CLOSEOUT

## Final receipt

```text
BEGIN_F6_FINAL_CLOSEOUT

FROZEN_CONTENT_SHA = 821c489fb081308d5ef95b13a14cc6435ac78d41
ORIGINAL_AUDIT_ISSUE = 523
ORIGINAL_AUDIT_EVIDENCE_SHA = 99d2d4b14498f9b71e4df620f97c1a2306e036ff
PUBLISHED_EVIDENCE_SHA = 99d2d4b14498f9b71e4df620f97c1a2306e036ff (verbatim; branch codex/f6-final-closeout-523)
F6_CLOSEOUT_PR = NOT_SET
REVIEWED_HEAD_SHA = NOT_SET
MERGE_SHA = NOT_SET
POSTMERGE_VERIFIED_SHA = NOT_SET

NORMATIVE_ROOT = v1-r4
REQUIREMENTS = 113/113
FORMAL_ASSETS = 26/26
FINDINGS_DISPOSITION = 18/18

F6_F01 = CLOSED_DOC_CORRECTED (#402 overlay refreshed at acceptance)
F6_F02 = CLOSED_DOC_CORRECTED (new dated LIVE STATUS PIN; old pin kept as dated history)
F6_F03 = TRANSFERRED_TO_TRACKED_ISSUE (#524; conservative superset, byte-verified reuse premises)
F6_F04 = SUPERSEDED_BY_NEW_EVIDENCE (fresh run COMPLETE_PASS at frozen tree; E2 receipt remains the authority)
E1_ERRNO_DISPOSITION = DOC_TABLE_DRIFT (shared oracle on both paths; re-diff test tracked in #526)
E1_BAD_ALLOC_DISPOSITION = LATENT_DIVERGENCE_CONFINED_TO_UNSUPPORTED_W04_CANDIDATE (precondition in #526; no shipped-surface violation)
RCORE_LIVE_RESULT = COMPLETE_PASS (153,347,909 generated / 26,662,372 distinct = exactly the E2 graph / depth 53 / 3h10m / no errors)

EVIDENCE_PROVENANCE = PASS
AUTHORITY_CORRESPONDENCE = PASS (File sole fd authority; single settlement binding; backend/core/host boundaries)
FORMAL_CORRESPONDENCE = PASS (safety subset re-run complete clean; liveness per E2 receipt + reuse, fresh-run status recorded)
PACKAGE_CORRESPONDENCE = PASS (25/13/15 re-derived; W-01/02/03 clean-room; W-04 N/A_WITH_NEGATIVE_PACKAGE_PROOF)
CURRENT_STATUS_PROJECTION = PASS (ledger pin + #402 overlay refreshed; historical records preserved)
INDEPENDENT_CLOSEOUT_REVIEW = APPROVE_CLOSEOUT (after corrections + scoped re-review)

P0 = 0
P1 = 0
DEFERRED_ISSUES = #524 #525 #526 #527 #528
OWNER_ACCEPTANCE = ACCEPT_F6_FINAL_CLOSEOUT (2026-10-15, task-mandate provenance recorded above)

F6 = CLOSED_ACCEPTED (effective at the merge of this record; post-merge receipt below completes the NOT_SET fields)
SUFFICIENT_CONTRACTED_SURFACE_ESTABLISHED = YES
SCOPED_LOCALLY_IRREDUCIBLE = NOT_CLAIMED
GLOBALLY_MINIMAL_SURFACE = NOT_CLAIMED

ISSUE_523 = NOT_SET
ISSUE_402 = NOT_SET
V1_RELEASE_READY = NO
RELEASE_AUTHORIZATION = NOT_GRANTED

END_F6_FINAL_CLOSEOUT
```

Fields marked `NOT_SET` above are completed on actual master in the post-merge
receipt commit, exactly as the previous phases did (e.g. `df2ed3ec`); the
issue-state fields are set only after the corresponding issue receipts are
published.
