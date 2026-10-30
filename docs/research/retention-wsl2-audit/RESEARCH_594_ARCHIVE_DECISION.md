# Research #594 Archive Decision

- **Date:** 2026-10-30
- **Adjudicator role:** Research Maintainer archive adjudicator (retention-WSL2 audit)
- **Subject:** PR #594 `research(593): E5 native key-path discovery & mechanism-elision
  campaign` — OPEN, MERGEABLE/CLEAN, head `2714aa65fe4837d30f5fa3fcdb27db38c23b7136`,
  branch `research/593-key-path`, base `master` at merge-base
  `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`
- **Verdict:** `KEEP_RESEARCH_PR_UNMERGED`

## 1. Verdict

Keep PR #594 OPEN and keep branch `research/593-key-path` in place. Do not merge, do
not close, do not delete the branch in this round. The PR archives the complete,
branch-unique record of the #593-adjacent E5 K1–K4 mechanism-elision campaign and is
the only in-repository copy of that record and of the reproduction entry for its
measurements. An archive merge (pure research archive, on the precedent of #610)
remains the correct eventual disposition, but three of the eight archive-merge gates
are unmet today, and the PR carries its own explicit do-not-merge instruction that
only the Owner may override.

Every check cited below was re-executed in the deciding session; the command record
is in `receipts/research-594-archive-decision-verification.txt`. Evidence produced by
earlier audit rounds and consumed through the ask is cited as such (receipts
`diff-594.md`, `issue-593-pr-594-mapping-receipts.md`, `key-path-593-retention-value.md`).

## 2. Gate assessment for an archive-merge recommendation

| # | Gate | Status | Evidence |
|---|------|--------|----------|
| 1 | No product code | MET | `git diff --name-status origin/master...2714aa65`: 56 entries, all `A`, only under `docs/research/key-path-593/` (15) and `bench/research/key-path-593/` (41); 0 files under `include/`, `src/`, `apps/`, `tests/`, `xmake*`, `scripts/`, `.github/`; PR body line 3: `PRODUCT_CHANGE: NONE`. |
| 2 | Long-term value for #595 | MET | #595 (OPEN) body: `Evidence: #593 / research PR #594`, blob links on `research/593-key-path`, W0 "inspect #594's research patches and reports". The two K4-b counterfactual oracle sources #595 §4 must repair exist only in `bench/research/key-path-593/patches/k4b-final.diff` (14 grep hits); master `tests/` contains only `uring_owed_residue_test.cpp`. |
| 3 | Evidence links accurate | MET | Cited numbers located in the PR tree: 0.50017 (5 docs), 0.12577 (6), 3.098 (7), 4.79 (12); `evidence-index.md:77` anchors brief.md sha256 `c750a314807934b696d9e109f7545da7f62115c6ff58647dba59b587303bc198`; archived `harness/SHA256SUMS` 24/24 OK and `patches/SHA256SUMS` 14/14 OK via `git archive` extraction. |
| 4 | Raw evidence verified + independent preservation plan | **NOT MET** | E5 raw evidence (≈1.35 GiB / 6,870 files at `/home/jnhu/sluice-593-research/evidence`) is single-copy and not hash-anchored (`evidence-index.md` §7, `EVIDENCE_PRESERVATION: PARTIAL`). The path is unreachable from the audit host (`ls`: No such file or directory); the full-field manifest and both-direction verification in `E5_RAW_EVIDENCE_PRESERVATION_PLAN.md` are specified but **NOT RUN** — status `E5_RAW_ARCHIVE_PENDING`. |
| 5 | Documents honestly disclose limitations | MET | Verified disclosure instances: `correctness-review.md` §5 unmeasured-load rows; `measurement-review.md` §9.13 K4-discrimination reducer missing, §4 retraction of the "pre-registered" 4.79 % band label; k4 doc §9/§10 (D=2/D=3, mixed-driver, cancel, partial-submit/overflow/shutdown unmeasured). Prior-round inventory (`key-path-593-retention-value.md`) found no gap claimed as covered. |
| 6 | Harness does not pollute default Benchmark/Build/CI | MET | `git grep -ri 'key-path'` at PR head over `xmake xmake.lua .github scripts tests include src apps`: no hits; patches are inert text under `bench/research/`; nothing wired into xmake targets or CI. |
| 7 | Independent research review completed | **NOT MET** | `correctness-review.md:186-190`: "no separate independent reviewer signed off as an approver … the independent review … was a *review input* to this revision, not an approval" — the campaign brief §14.6 independent-review step is disclosed as not satisfied as an approval. Two review records exist with 42 dispositioned findings, but no independent sign-off. |
| 8 | Owner explicitly overrides the PR's do-not-merge instruction | **NOT MET (Owner decision only)** | `decision.md:205`: "opened and remains OPEN, unmerged per brief §14"; PR body line 97: "**This PR must remain open and unmerged.**" No Owner instruction overriding this exists; this gate cannot be self-granted by an audit round. |

Gates 4, 7 and 8 are unmet, so the threshold for `ARCHIVE_MERGE_RECOMMENDED` is not
reached. Per the adjudication rule that unmet conditions fall back to keeping the
research PR unmerged, the verdict is `KEEP_RESEARCH_PR_UNMERGED`.

## 3. What would be lost if the branch were deleted

Re-verified this session against current `origin/master` (`84e1b7d5`):

- 0 of the 56 PR paths exist on master; `git ls-tree -r origin/master` contains no
  path matching `key-path` or `593`.
- Content-level sha256 intersection: 0 of 56 PR file content hashes match any of the
  665 distinct sha256 values in the merged #610 archive manifest
  (`research/direct-beyond-parity/EVIDENCE_MANIFEST.json`, 0 `key-path` entries).
  Earlier rounds additionally verified 0/56 blob-SHA membership in master's tree and
  `git cherry` patch-uniqueness of both PR commits (receipts `diff-594.md`).
- Consequence: closing or deleting PR #594 / `research/593-key-path` without merging
  destroys the only in-repo copy of the K1–K4 campaign record, the frozen measured-arm
  patches (apply to base `bdfd0d8d`), the two K4-b counterfactual oracle sources, the
  SHA256SUMS-pinned harness, and #595's working citation links (its blob URLs point at
  the branch). Deletion before a verified archive merge is prohibited under this
  verdict.

## 4. Attribution corrections (delivery mapping)

These corrections are part of the ruling and should accompany any future merge or
Owner triage; none of them was applied to GitHub content in this round (write
boundary: report directory only).

1. **PR #594 does not satisfy issue #593.** #593 defines a PocketJS-inspired
   execution-path slicing and deterministic replay Pilot with R0–R4 gates and the
   closeout vocabulary `METHOD_EARNED|PARTIAL_OBSERVATION_ONLY|REPLAY_NOT_ADMITTED|NO_EXTRA_DIAGNOSTIC_VALUE|BLOCKED_CORRECTNESS`. The #594 tree contains zero hits for
   that vocabulary, the R0 feasibility matrix terms (`REQUEST_REPLAY`,
   `COMPLETION_REPLAY`), or any replay/mutation-tape/trace-corpus machinery (grep at
   `2714aa65`, exit 1). #593 itself remains OPEN (verified via `gh issue view 593`).
   Any closure of #593 citing #594 as completion is a mis-attribution and is rejected.
2. **#594 is its own campaign.** Its title and body self-identify as the "E5 native
   key-path discovery & mechanism-elision campaign"; the body says "This implements
   the #593 K1–K4 campaign", i.e. a distinct benchmark-counterfactual campaign that
   borrowed #593's path segmentation, not the #593 Pilot. Its protocol authority is
   the un-archived `brief.md` (sha256 `c750a314…`, anchored at
   `evidence-index.md:77`; original text not readable from this host — forensic
   limitation, NOT RUN).
3. **#593 stays OPEN** as the authority for the still-unexecuted PocketJS Pilot
   (R0–R4). If the Owner still wants that Pilot, it requires a new research
   Issue/PR; nothing in #594 provides R0–R4 content. Follow-on issues #596–#599
   derive from #594's findings, consistent with #594 being an independent campaign.
4. **#595's evidence line should be corrected.** `Evidence: #593 / research PR #594`
   attributes the K2/K4 numbers to #593; they were produced by #594's campaign and
   only borrowed #593's path segmentation. PR #594 should be the evidence authority
   for those numbers, with #593 as nominal parent task only — or the Owner must
   explicitly redefine #593's scope.
5. **#594 should stay OPEN until Owner disposition**, consistent with its own body.
   Two lawful end states exist: (a) pure research-archive merge after the gates below
   are met (Owner decision, #610 precedent); or (b) continued OPEN retention. Closing
   without either is barred by §3.

## 5. Research value (retained while OPEN)

- **Direct #595 dependency:** #595 §4 requires repairing the two defective K4-b
  oracles (`k4_residue_wake_test` with its 100 ms sleep, `k4_retry_residue_wake_test`
  without deferred multi-entry pass-start retry coverage); their only sources are in
  `k4b-final.diff` in this PR. Repair patches `k4b-final.diff` /
  `combo-k4b-k1ab.diff` with SHA256SUMS are preserved here (verified 14/14 OK).
- **Long-term knowledge** (prior-round extraction, spot-reverified): K1/K3 negative
  results with threshold dependence; K2-a as an ungated upper bound (D=1 p50 ×14.1,
  p99 regressions); K4-b form-gated support only at D=4/16/64 with the 64 KiB p99
  +7.46 % regression; K2-a/K4-b as one non-additive mechanism (`MECHANISM_ONLY_GAIN`
  combo); the 8-item productization precondition list that seeds #595 §6; the
  measurement-boundary and oracle-quality disclosures.
- **Reproduction entry:** every measured arm rebuilds from frozen base `bdfd0d8d` +
  archived patches + harness (internal SHA256SUMS 38/38 OK). Known limits are
  disclosed in-tree and do not block retention: one-shot harness provenance with
  hard-coded paths, missing K4-discrimination reducer, un-hash-anchored 1.35 GiB raw
  evidence owner-retained on the E5 host (which #595 W0 explicitly forbids moving).

## 6. Non-claims

This verdict does **not** adopt K4-b (or any K-lane mechanism) as a product
mechanism. `PRODUCT_CHANGE: NONE` stands; `ELISION_PERF_SUPPORTED` is form-gated
(D=4/16/64) with D=2/D=3 unmeasured; verdicts count once under K4; percentages are
never summed. No productization, merge to product code, or contract claim is
authorized by this document.

## 7. Conditions unmet (blocking `ARCHIVE_MERGE_RECOMMENDED`)

1. **Raw evidence verification and independent preservation not executed.** The E5
   evidence tree is unreachable from the audit host; the full-field manifest and
   both-direction sha256 verification defined in
   `E5_RAW_EVIDENCE_PRESERVATION_PLAN.md` are NOT RUN; status
   `E5_RAW_ARCHIVE_PENDING`. Required: Owner-side execution of Scheme A/B/C with
   destination-side verification receipts, plus the hash anchor committed in-tree.
2. **Independent research review sign-off absent** (`correctness-review.md:187`).
   Required: a genuine independent approver sign-off, or an explicit Owner acceptance
   of the disclosed absence for an archive-only merge.
3. **Owner consent to override the do-not-merge instruction absent** (`decision.md:205`,
   PR body line 97). Only the Owner may override; an audit round cannot.
4. **Protocol authority not archived in-repo.** `brief.md` (sha256 `c750a314…`) and
   the `mapping/` lane drafts exist only on the E5 host; until committed into the PR
   docs tree or archived as a PR/Issue comment, the K1–K4 verdict vocabulary is not
   reproducible from the repository alone.

## 8. Ordered path to a future archive merge

1. Owner executes E5 preservation per plan §4.3 (both-direction verification on the
   destination; 6,870 files; tar self-hash match) and commits the in-tree hash anchor.
2. Commit `brief.md` + mapping drafts into the PR tree (or archive as PR/Issue
   comment) so the protocol authority is reproducible.
3. Independent review sign-off, or explicit Owner acceptance of its disclosed absence
   for archive-only purposes.
4. Explicit Owner instruction overriding "must remain open and unmerged", framed as a
   pure research-archive merge per the #610 precedent — never as productization.
5. Apply the §4 attribution corrections (#595 evidence line; #593 remains the open
   Pilot authority; #594 owns the K1–K4 campaign) at merge time.
6. Only after the merge is verified on master may `research/593-key-path` be deleted.

## 9. Verification record and forensic limitations

All checks in §§2–3 were executed in the deciding session (2026-10-30) and are logged
with commands and outputs in
`receipts/research-594-archive-decision-verification.txt`. Not executed anywhere, and
reported as such rather than guessed:

- Reading the original `brief.md` text and the E5 raw evidence tree (path does not
  exist on this host; single-copy state taken from the in-tree
  `evidence-index.md` §7 disclosure).
- Generation of the E5 full-field evidence manifest and any preservation-scheme
  verification (specified in `E5_RAW_EVIDENCE_PRESERVATION_PLAN.md`, all NOT RUN).
- Any mutation of GitHub state (no comment posted, no merge, no close, no branch
  deletion) and any build. Writes this round: this decision document and the
  receipts file only.
