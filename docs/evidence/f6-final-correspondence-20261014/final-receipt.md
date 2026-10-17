# F6 Final Receipt

```text
BEGIN_F6_FINAL_AUDIT_RECEIPT
TASK            = F6_FINAL_EVIDENCE_CORRESPONDENCE_AUDIT
MODE            = READ_ONLY_CODE_AND_FORMAL / EVIDENCE_OUTPUT_ONLY
EXECUTED_AT     = 2026-10-14 (UTC+8)
FROZEN_CONTENT  = 821c489fb081308d5ef95b13a14cc6435ac78d41 (master content freeze)
F6_ENTRY        = fdb4b743c8f24bdcc4ec446c4bc7efbd386e61c3
ROOT            = docs/explicit-io-v1-final-decision.md (v1-r4, sole normative)
LEDGER          = docs/roadmap/v1-conformance.md
FORMAL_MAP      = docs/roadmap/v1-formal-evidence-map.md
UMBRELLA        = issue #402 (OPEN)

CONSTRAINTS_OBSERVED
  PRODUCTION_CHANGES            = FORBIDDEN  -> none made
  BUILD_CHANGES                 = FORBIDDEN  -> none made
  TEST_CHANGES                  = FORBIDDEN  -> none made
  FORMAL_MODEL_CHANGES          = FORBIDDEN  -> none made
  HISTORICAL_EVIDENCE_REWRITE   = FORBIDDEN  -> none made
  OUTPUT                        = new files under docs/evidence/f6-final-correspondence-20261014/ only

ROUNDS
  R0 freeze & provenance        = COMPLETE   (INDEX.md)
  R1 requirement correspondence = COMPLETE   113/113 root identifiers (71 clause IDs + S1–S10 + L1–L5 + V01–V27), forward + reverse
                                             (requirements-matrix.tsv)
  R2 authority & lifetime DAG   = COMPLETE   14 authority nodes, 9/9 adversarial risks
                                             cleared, retirement inventory re-verified
                                             (authority-dag.md)
  R3 formal asset audit         = COMPLETE   3-axis classification of 26 assets;
                                             safety subset re-run clean at frozen tree;
                                             RequestCore.cfg complete: 152,240,323+
                                             states generated, 26,566,666+ distinct,
                                             depth 53, 8m09s, no errors;
                                             RequestCoreLive re-run in flight at close
                                             (supplementary; E2 receipt stands)
                                             (formal-assets.tsv, raw/tla-results.txt)
  R4 package & consumers        = COMPLETE   25 headers / 13 async members / 15 objects
                                             re-derived; W-01/02/03 clean-room PASS;
                                             W-04 N/A_WITH_NEGATIVE_PACKAGE_PROOF
                                             (package-and-claims.md, raw/)
  R5 claim integrity            = COMPLETE   receipts vs actual state; overlay staleness
                                             isolated as projection-only
  R6 adversarial findings       = COMPLETE   18 findings, P0=0 P1=0 P2=4 P3=14
                                             (findings.md)

FINAL HARD GATES (#402 verbatim list)
  E_REQUIRED_CLOSURE_ADOPTED ................................ PASS
  EACH_FINAL_DISPOSITION_HAS_ROOT_ROLE_POLICY_AND_EVIDENCE .. PASS
  RETAINED_CONSUMERS_MIGRATED_OR_EXPLICITLY_ISOLATED ........ PASS
  CLEAN_ROOM_NO_SOURCE_OR_BUILD_TREE_DEPENDENCY ............. PASS
  PACKAGE_USAGE_REQUIREMENTS_AND_DECLARATIONS_MATCH_ARTIFACTS  PASS
  NO_UNQUALIFIED_CANONICAL_DOC_CLAIMS_FOR_RETIRED_AUTHORITY . PASS
  DIRECT_AND_REQUEST_CORE_HOST_FIREWALL ..................... PASS
  CURRENT_IMPLEMENTATION_CLAIMS_WITH_INVALID_OR_MISSING_CORRESPONDENCE = 0
  SINGLE_CANONICAL_NATIVE_RESOURCE_AUTHORITY ................ PASS
  CANONICAL_ACCEPTED_REQUEST_HAS_SINGLE_MOVE_ONLY_SETTLEMENT_BINDING = PASS
  BACKEND_CORE_HOST_SPECIFIC_IDENTITY_CONTINUATION_ROUTING_AUTHORITY = 0
  RETIRED_ROLE_CODE_IN_CANONICAL_TARGET ..................... 0 (inert residue disclosed, F6-F08)
  W01_EXTERNAL .............................................. PASS
  W02_EXTERNAL .............................................. PASS
  W03_EXTERNAL .............................................. PASS
  W04_EXTERNAL .............................................. N/A_WITH_NEGATIVE_PACKAGE_PROOF
                                               (conditional gate; profile not shipped/supported)

DECISION
  F6 .......................................... VERIFIED_READY_FOR_FINAL_REVIEW
  F6_RETIREMENT_READY_FOR_FINAL_REVIEW ....... YES
  SUFFICIENT_CONTRACTED_SURFACE_ESTABLISHED .. YES
  SCOPED_LOCALLY_IRREDUCIBLE ................. NOT CLAIMED (no preregistered ablation required)
  GLOBALLY_MINIMAL_SURFACE ................... NOT CLAIMED (forbidden by #402)
  V1_RELEASE_READY ........................... NO
  ISSUE_402 ................................. REMAINS OPEN (final owner acceptance pending)

STOP CONDITION
  F6 stops at the final technical verdict above. No merge, no #402 closure,
  no v1 release action, and no production/build/test/model repair is taken
  by F6; every finding routes to its owning phase/owner per findings.md.
END_F6_FINAL_AUDIT_RECEIPT
```

## Deliverables index

| File | Content |
|---|---|
| `INDEX.md` | freeze/provenance, authority order, deliverables map, re-verification log |
| `requirements-matrix.tsv` | R1: 113 identifier rows, 14 columns, GOV-02 verdicts, honest scopes |
| `authority-dag.md` | R2: authority/lifetime DAG + 9 adversarial risk probes + retirement inventory |
| `formal-assets.tsv` | R3: retention / eligibility / proof-scope axes for all 26 formal assets |
| `package-and-claims.md` | R4+R5: package re-derivation, clean-room results, claim-integrity table |
| `findings.md` | R6: 18 findings with mandated field set (P0=0, P1=0) |
| `gates.json` | all #402 final gates with per-gate basis |
| `independent-review.md` | adversarial pass over this audit (13 attacks, 3 landed as disclosures) |
| `raw/` | clean-room consumer sources + results, TLA run results |

## Remaining limitations (honest scope of this verdict)

1. The verdict is *ready for final review*, not final acceptance: #402 stays
   OPEN and the owner makes the acceptance decision.
2. The fresh RequestCoreLive liveness run had not finished at receipt time;
   the E2-dated receipt plus F2's historical-reuse ruling carry REQ-06/L1
   (F6-F04). The complete safety graph over the same constants re-derived
   clean at the frozen tree.
3. F6 clean-room consumers ran debug + liburing=n; both full test matrices
   re-ran at the frozen tree (66/66 liburing=y, 52/52 liburing=n).
4. #475 external use of retired surfaces remains UNKNOWN (F6-F18).
5. Documentation projection staleness (F6-F01/F02) is disclosed, not fixed;
   dated receipts govern.
