# F6 Independent Adversarial Review Pass (over this audit's own conclusions)

Scope: attack the audit before anyone else can. Each row is an attempt to
invalidate a conclusion in this directory; the result column is what the
attack actually found when executed against the frozen tree and the audit
artifacts. Attacks that succeeded are already folded into findings.md.

| # | Attack | Result |
|---|---|---|
| 1 | "The matrix misses requirement IDs" — recount root identifiers | Root defines 71 clause IDs + S1–S10 + L1–L5 + V01–V27 = 113; matrix has 113 data rows, one per identifier, no merges. (An earlier working count of 104 had folded the scenario families; the committed matrix is the exhaustive superset.) Attack failed |
| 2 | "GOV/MIG/ADR rows are self-graded process clauses — no evidence" | Correct by construction: they are DOC_ONLY correspondence rows whose evidence is the governance chain itself (root v1-r4 → dated receipts). Marked DOC_ONLY, never counted as implementation evidence. Attack absorbed by honest scoping |
| 3 | "fiber.hpp:64 violates RETIRED_ROLE_CODE_IN_CANONICAL_TARGET=0" | Strongest attack. Verified: no `class Scheduler` definition exists anywhere (include/src/apps), so the friendship grants nothing and the gated setters are unreachable dead code. Classification: inert cosmetic residue (F6-F08, P3), disclosed inside the gate basis rather than hidden. Gate stands as 0 with disclosure; reviewer may re-adjudicate at final acceptance |
| 4 | "W04 N/A is gate-laundering — #402 says W04_EXTERNAL = PASS conditional" | #402's own text: "conditional on shipped/supported profile". Profile is not shipped (candidate-labeled, honest construction failure, no support claim, F5 install keeps candidate labeling). The conditional resolves to N/A with the negative proof #402's ledger pin itself demands ("if unsupported, W-04 is a conditional N/A requiring negative package/support-claim proof"). Attack failed — the ledger mandates exactly this outcome |
| 5 | "ReleaseBind means CURRENT_…_CORRESPONDENCE ≠ 0" | The gate counts *implementation claims* with invalid/missing correspondence. No current claim asserts exact correspondence over the reclaim sub-graph; every claim citing RequestCore.tla cites safety/liveness over a superset with the flavor documented. Model-side maintenance gap = F6-F03 (P2). Attack narrowed but did not flip the gate |
| 6 | "The clean-room is weak: single profile, single build type" | Partially lands: F6 clean-room ran debug + liburing=n only. Mitigation recorded: both full test matrices re-ran at the frozen tree (66/66 liburing=y, 52/52 liburing=n clean-config), and the in-repo package consumers (w03 uring variant included) cover the installed surface in both profiles. Residual is disclosed here, not claimed away |
| 7 | "The 66/66 run was accidental (config not applied)" | True and disclosed: the first run intended liburing=n but the config flag was invalid, so it exercised the liburing=y matrix; the subsequent clean `xmake f -m debug --liburing=n -c -y` run produced 52/52. Both matrices therefore executed at the frozen tree, one by accident. Evidence value unaffected; sequence recorded for honesty |
| 8 | "F6-F04 hides an incomplete liveness re-run behind a P2" | The finding states the basis outright: E2-dated receipt + F2 historical-reuse ruling are the closure authority; the fresh run is supplementary and its in-flight status is published, not buried. The same state space's complete safety graph was re-derived clean (152M generated / 26.57M distinct / depth 53). Attack failed |
| 9 | "Layer-A N/A rows (V04/V05/V13/V15/V17/V21/V22/V24) are coverage evasion" | Each N/A row names why external manufacture is impossible without seams/sleeps (F1 §5 discipline, re-checked) and keeps its layer B/C/D evidence cited in the matrix. None was converted from a failure to an N/A. Attack failed |
| 10 | "Stale docs could hide retired-authority guidance (NO_UNQUALIFIED_…_gate)" | Every stale item found (F6-F05/06/07/15) is non-instructional history or index/writing staleness; none directs a user toward a retired canonical authority. Gate PASS with the P3s disclosed |
| 11 | "Counts could be release-profile artifacts" | Header/archive/object counts re-derived from fresh install + debug archives; a stale pre-F5 release archive in build/ was explicitly identified as local residue and excluded. Attack failed |
| 12 | "Verdict vocabulary violation: SCOPED_LOCALLY_IRREDUCIBLE or GLOBALLY_MINIMAL" | Neither is claimed. gates.json records SCOPED_LOCALLY_IRREDUCIBLE as NOT_CLAIMED (no preregistered ablation was required by any gate) and GLOBALLY_MINIMAL_SURFACE as forbidden/not claimed. Attack failed |
| 13 | "F6 exceeded READ_ONLY scope" | Production/build/test/formal-model trees untouched (git status clean except new docs/evidence/f6-* files); models ran in /tmp with the pinned checksummed jar; no historical evidence file edited; only new audit files added. Attack failed |

Reviewer independence note: rounds R1/R2/R5 deep-dives were executed by
separate agent passes over the frozen tree; this pass re-attacked their
merged conclusions. The three attacks that landed (6, 7, and the residue
disclosure in 3) are recorded as disclosures above and in findings.md; none
changes a gate value.

Verdict of this pass: **F6 = VERIFIED_READY_FOR_FINAL_REVIEW** stands;
no P0/P1; final acceptance remains the owner's decision, with #402 OPEN.
