# Draft replacement for #402's CURRENT overlay (applied at final acceptance)

This is the exact replacement text for the topmost `## CURRENT — …` block of
issue #402 (F6-F01 disposition). Historical paragraphs and dated status
records below the overlay are preserved untouched. The overlay is applied only
when the owner records final acceptance of the F6 closeout; it is not applied
before the closeout PR merges.

---

## CURRENT — F0–F6 CLOSED_ACCEPTED; v1 NOT release-ready (2026-10-15)

**F6 = CLOSED_ACCEPTED.** The final evidence-correspondence audit
(frozen master `821c489f`, artifacts
[`docs/evidence/f6-final-correspondence-20261014/`](https://github.com/jnhu76/Sluice/tree/master/docs/evidence/f6-final-correspondence-20261014),
originally commit `99d2d4b1`) verified all 113 root identifiers, 26 formal
assets, the package surface (25 installed headers / 13 async archive members /
15 library objects), clean-room W-01/W-02/W-03 and the 16 final hard gates
(`W04_EXTERNAL = N/A_WITH_NEGATIVE_PACKAGE_PROOF`; W-04 not shipped/supported).
The closeout (PR bound to this record,
[`docs/evidence/f6-final-closeout-20261015/`](https://github.com/jnhu76/Sluice/tree/master/docs/evidence/f6-final-closeout-20261015))
adopted the evidence, dispositioned all 18 findings (P0=0, P1=0; deferred
maintenance tracked in #524, #525, #526, #527, #528) and passed an independent
closeout review. Final receipt: `final-acceptance-decision.md`.

| Phase | Final disposition |
|---|---|
| F2 authority contraction | **CLOSED** (#516, `27a5041a`; installed-surface break scope approved, post-merge verified) |
| F3 app/host closeout | **CLOSED** (#513 merge `f4a97234`, closeout #515; four app/package configs, dual native-Clang sanitizers) |
| F4 legacy retirement | **ADOPTED_SCOPED** (#514 merge `de551ed0`; #505/#506 CLOSED) |
| F5 final package contraction | **CLOSED** (#518 merge `f99acbc3`; post-merge B-01 v7 verified on actual master: 25/13/15 both ring profiles, `removed_present` 0) |
| pre-F6 infra | #519/#520/#521 CLOSED as FIXED_VERIFIED (PR #522 merge `573d616c`, post-merge receipts) |
| F6 final evidence audit | **CLOSED_ACCEPTED** (this record) |

`OPTIONAL_W04_SUPPORTED=NO`; W-04 is a conditional N/A with negative
package/support-claim proof, not a PASS. External use of retired surfaces
remains **UNKNOWN** (not zero; #475 residual — owner decision at release
planning). `V1_RELEASE_READY=NO`; **no release action was taken** by F6 or its
closeout. #390 continues under its own release gates.

Earlier dated paragraphs below this overlay are historical and must not
override the dated receipts; where they conflict with a dated receipt, the
receipt governs.
