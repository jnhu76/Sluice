# Retention audit: `backup/pr-601-original-head-e4bb8833` (Sluice, remote `origin`)

Date: 2026-10-29. Scope: non-destructive verification only; nothing deleted or modified.
Receipts: pass 2 (this audit) `receipts/backup-pr-601-original-head-e4bb8833-pass2.txt`;
pass 1 (same day, earlier) `receipts/backup-pr-601-original-head-e4bb8833.txt` (R1–R8).

## Decision: KEEP (revises pass-1 DELETE_CANDIDATE)

Pass 1 (23:07) concluded DELETE_CANDIDATE from the r2 closeout-deferral note. Pass 2 re-ran every
check independently and found a documented dependency pass 1 missed: master's archived closure
record designates this branch as the **sole remote keep-alive for `ea2fc4fc` — the `CANDIDATE_SHA`
cited by master's own evidence** — and instructs that cleanup must not delete it
(`P0_BASELINE_AND_OWNERSHIP.md` §2.3.1/§2.3.7/§2.4.1, landed via #610). The P0-planned second,
*local* keep-alive branch no longer exists (verified empty `git branch --contains`), so this ref is
the only thing anywhere keeping the CANDIDATE_SHA object resolvable. Under the audit's special
rule for backup branches — original commits must remain recoverable via immutable SHA, historical
PR **and** actual object storage before the ref may be dropped; 恢复性存疑 → KEEP — recovery after
deletion is not guaranteed: both PR refs point at rewritten heads, so the two objects would become
remote-unreachable and subject to gc. Deletion authority was also reserved to Owner/R9 closeout
(`r2-pr-boundary.md` §6), and the standing recommendation there and in P0 §2.3.1 is long-term /
permanent retention.

## Ref and HEAD verification

- Ask label `sluice/backup/pr-601-original-head-e4bb8833` = repo `sluice` + remote branch
  `backup/pr-601-original-head-e4bb8833` (no `sluice/` prefix on the remote); local tracking ref is
  `origin/backup/pr-601-original-head-e4bb8833`. Both resolve to the same SHA.
- HEAD = `e4bb8833699d0ca5dee191883f3fec6d41b221b2` (matches the branch-name suffix; R1).
- Remote branch **exists**: `ls-remote` →
  `e4bb8833699d0ca5dee191883f3fec6d41b221b2 refs/heads/backup/pr-601-original-head-e4bb8833` (R2).
- The P0-planned backup name `backup/600-research-pre-split-e4bb8833` is absent from the remote;
  the pushed `backup/pr-601-...` branch fulfills that Stage-0 role (r2-pr-boundary.md §2 records
  the actual push with this name) (R2, R6).

## Associated PR: None (by headRef match)

No PR in the supplied list has a matching `headRef`. The name references PR #601 (MERGED; listed
headSha `2fcc7964...` = current `refs/pull/601/head`): this branch is #601's **pre-split,
pre-rewrite original head**, set aside when the work was split into #601 (research evidence) +
#602 (C1 product change). It carries 2 commits on old base `bdfd0d8d`: `ea2fc4fc` (#600 C1 perf)
and `e4bb8833` (research evidence) (R1, R3, R9).

## Findings

1. **Commit reachability** — only
   `refs/remotes/origin/backup/pr-601-original-head-e4bb8833` contains the tip or its parent
   `ea2fc4fc`; no local branch and no other ref does. 2 commits unique vs `origin/master`
   (merge-base `bdfd0d8d`). #601 squash `84a0cc1d` and #602 squash `43cb8029` are single-parent
   squash merges on master, so the two original **commit objects are not in master's history**.
   Neither is an ancestor of `refs/pull/601/head` (`2fcc7964`, rewritten) nor of PR #602 head
   `77e5399a` nor of OPEN PR #594 head `2714aa65` (R3, R4).
2. **Blob reachability** — 21 unique objects (2 commits + trees + 9 file blobs). Versus
   `origin/master` today: all 5 `research/600-direct-excess-cost/*.md` and
   `tests/semantic_range_test.cpp` are **blob-identical**; the 3 remaining files differ only
   because master evolved after the merges (#612 et al.). All 4 production/test file blobs of
   `ea2fc4fc` are byte-identical to PR #602 head `77e5399a` **and** to the merged squash
   `43cb8029` (`e39760ae` / `6660a423` / `0d3eb7e0` / `febe6515`). **All file content is merged;
   the only unique data left is the two commit objects themselves** (R5).
3. **Unique evidence** — no unarchived *file* content: research evidence landed verbatim via #601
   (`84a0cc1d`), C1 code via #602 (`43cb8029`). But the **measurement-identity anchor is not
   merged**: master's `research/600-direct-excess-cost/PHASE3_VALIDATION.md:4` (part of merged
   #601) cites `CANDIDATE_SHA: ea2fc4fca1a6`, and `P0_BASELINE_AND_OWNERSHIP.md` §2.3.1 states
   "This branch is the remote keep-alive for both `ea2fc4fc` (CANDIDATE_SHA) and `e4bb8833` …
   GitHub's `refs/pull/601/head` will not preserve the old chain after a force push. Keep it
   long-term — at minimum past the planned WSL2 portability task; **recommended permanently**."
   §2.3.7: "branch cleanup must not delete" the backup; §2.4.1: "remote reachability of
   CANDIDATE_SHA ea2fc4fca1a6 depends on a single branch, the backup." The second, *local*
   keep-alive named there no longer exists (empty `git branch --contains`), making this ref the
   sole holder (R3, R6). Note: #610's archive dir (`research/direct-beyond-parity/`) documents
   the branch but does not contain the objects; object preservation is the ref itself.
4. **Active dependency** — no script/CI/worktree consumer: `git grep` on `origin/master` finds the
   branch name/SHA only as prose and recovery instructions in the #610 closure docs (6 + ~20
   lines); no `.github`/xmake/script hit; no worktree checks it out (worktree list: `master` + 14
   detached validation worktrees at other SHAs). dududu (`/tmp/dududu-audit`, main @ `12a8712`,
   merged #7) contains no reference to `e4bb8833`/`ea2fc4fc`/`backup/pr-601`/
   `sluice-600-direct-research`. The dependency that matters is the **documented evidence
   keep-alive role** above, recorded on master by merged #610 (R6, R7, R8).
5. **Working tree** — not applicable: the branch is not checked out in any worktree, so no dirty
   check applies (R7).
6. **Recovery** — today: fully recoverable (remote ref at exact SHA, fetch verified in
   r2-pr-boundary.md §2 and re-confirmed by `ls-remote`). Full SHA + exact re-push command are
   archived on master at `r2-pr-boundary.md:64-65`. **After deletion**: the original commits are
   *not* recoverable via historical PR — `refs/pull/601/head` = `2fcc7964` (rewritten head),
   `refs/pull/602/head` = `77e5399a`, neither contains `ea2fc4fc`/`e4bb8833` (P0 §2.3.1 predicted
   exactly this) — and unreachable objects on the remote become subject to gc, so fetch-by-SHA is
   not guaranteed. Only local clones would retain the objects, until their gc. File content
   recovery would be unaffected (all merged); what would be permanently lost is the
   CANDIDATE_SHA/原 HEAD commit objects that master's evidence cites as measurement identity (R4,
   R6).

## Decision basis vs the DELETE_CANDIDATE conditions

| Condition | Result | Evidence |
|---|---|---|
| 非 OPEN PR head | ✓ | No headRef match; #601/#602 MERGED (R9, R3) |
| 无独有未归档数据 | ✓ (files) | All 9 file blobs merged on master (R5) — but the 2 commit objects are ref-unique and one is evidence-cited |
| 无未推送产品变更 | ✓ | C1 landed as `43cb8029` (#602), blobs identical (R5) |
| 原始数据已有可靠备份 | ✓ | Evidence on master; branch identity archived in `r2-pr-boundary.md:56-84` (R5, R6) |
| 无运行中 Agent/Worktree 依赖 | ✓ | No worktree/CI/script consumer; dududu clean (R7, R8) |
| HEAD SHA 与恢复步骤已记录 | ✓ | `r2-pr-boundary.md:64-65` + this receipt (R6) |
| **Special check: 原始提交可经 SHA/历史 PR/对象存储完整恢复（删 ref 后）** | **✗** | PR refs rewritten, remote objects unreachable post-deletion → 恢复性存疑 → KEEP (R4, R6) |

The special check governs: this is a backup branch whose documented duty (P0 §2.3.1, merged
authority on master) is exactly to keep `ea2fc4fc`/`e4bb8833` fetchable, with permanent retention
recommended, deletion reserved to Owner/R9 closeout, and no surviving alternative keep-alive.
**No deletion executed in this phase.** If the Owner later decides to drop it, first supersede the
P0 keep-alive designation on master (or relocate the objects to a new ref/tag), so
`PHASE3_VALIDATION.md:4`'s CANDIDATE_SHA stays resolvable.

Not run / limitations: dududu grep covered only the checked-out main tree of `/tmp/dududu-audit`
(other dududu branches not searched — no dududu PR matches this branch, so impact is limited);
`refs/pull/601/head` containment of the old chain was verified by ancestry of the listed headSha
plus the archived isolated-fetch record, not by a fresh fetch of the PR ref.
