# Retention audit: `research/r6-ac1-vectored-semantics` (Sluice, remote `origin`)

Date: 2026-10-29. Scope: non-destructive verification only; nothing deleted or modified.
Receipts: `receipts/r6ac1-01..12-*.txt` (R1–R12 referenced below).

## Ref and HEAD verification

- The ask's literal form `git rev-parse origin/origin/research/r6-ac1-vectored-semantics`
  **fails** (`unknown revision`, exit 128). The audit label `sluice/origin/research/r6-ac1-vectored-semantics`
  resolves as repo `sluice` + remote `origin` + branch `research/r6-ac1-vectored-semantics`
  (fetch refspec `+refs/heads/*:refs/remotes/origin/*`). All checks below use the resolvable
  ref `origin/research/r6-ac1-vectored-semantics` (R1).
- Actual HEAD = `999b5305ae7d413a2a71acde926f8c6cfef6876a` — **matches** the expected SHA.
  Single commit: `research(ac1): E5 semantic closure for positional vectored I/O (#605)`,
  2026-10-29 08:02:26 +0800; message states "Design evidence only: no public API, no product
  code, no v2-r1 change" (R1, R3).

## Associated PR: None (by headRef match); live repo shows CLOSED PR #611

No PR in the supplied list has `headRef` = `research/r6-ac1-vectored-semantics` → "None" per
the ask's matching rule. Live verification (beyond the supplied list): `gh pr view 611` →
PR **#611, state CLOSED (unmerged)**, `headRefName research/r6-ac1-vectored-semantics`,
`headRefOid 999b5305...` (exact SHA), base `master` — "research(ac1): E5 semantic closure for
positional vectored I/O (#605)". Consistent with the earlier audit snapshot
(`REPOSITORY_REF_SNAPSHOT.md:64,105,496`; `state-freeze-report.md:22`) (R8).

## Findings

1. **Commit reachability** — `git branch -a --contains 999b5305` → only
   `refs/remotes/origin/research/r6-ac1-vectored-semantics` itself; no tag/local/other ref
   contains it. `git rev-list --count origin/master..<branch>` = **1** (the tip only).
   Merge-base with `origin/master` = `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`, and the
   tip's single parent **is** `bdfd0d8d` (ancestor of master) (R2, R12).
2. **Blob/content reachability** — the ask's three-dot diff (`origin/master...branch`,
   34 files) measures against merge-base and overstates uniqueness; the authoritative
   tip-vs-tip comparison shows the branch has **zero unique content**: per-path
   `git rev-parse <ref>:<path>` for all 34 diff files returns **blob-identical SHAs at
   identical paths on `origin/master`** (R4); two-dot `git diff --name-status origin/master
   <branch>` has **0 `A` entries** (no file exists on the branch that is absent from master),
   798 master-side M/D entries (master evolved after the branch point), and **no entry
   touching any `r6-ac1` path** (R6). Note: `git rev-list --objects <branch> ^origin/master`
   lists 26 blobs (R3) — this is git's known tree-walk over-reporting (uninteresting trees
   are not descended, so blobs shared with master are relisted when tip trees differ); the
   per-path blob comparison is authoritative.
3. **Unique evidence — none unarchived.** All 34 files (`r6-ac1.md` + `r6-ac1/{API_DRAFT,
   E5_ORACLE,ERROR_MATRIX,OWNER_DECISION_PACKET,README,SEMANTICS}.md`, `probes/r6ac1_probe.c`,
   `probes/run_all.sh`, 24 `raw/*.txt`) are on master, added verbatim by
   `ac995641e7394bfd82f7edb1caafca6ebea21c13` = PR **#610 (MERGED)**, "archive #603 P0-P6
   evidence with sha256 manifest". Master's `research/direct-beyond-parity/EVIDENCE_MANIFEST.json`
   carries 34 r6-ac1 entries; **all 34 sha256 values match the branch's blob content
   byte-for-byte** (34 matched / 0 mismatched) (R7, R9, R11). The overlap is intentional and
   documented on master: `closure/r9-archive-increment.md:96` — "closure/r6-ac1* is
   additionally carried by the AC-1 design PR #611 … the two research branches intentionally
   overlap on that subtree" (R10). Even the open decision state is preserved: the archived
   `OWNER_DECISION_PACKET.md` on master records the recommendation with the choice left open.
4. **Active dependency — none.** Not an OPEN PR head (#611 CLOSED; no supplied-list match).
   `git grep 'research/r6-ac1-vectored-semantics' origin/master` → exactly one hit,
   `r9-archive-increment.md:96` (prose provenance record, not a consumer); the same holds for
   a working-tree grep excluding this audit directory; no script, CI config or build file
   consumes the branch. No worktree checks it out (`git worktree list`: `master` @ `84e1b7d5`
   + 3 detached `/tmp/sluice-val-*` validation worktrees at other SHAs). No dududu coupling:
   `git grep -i dududu` on the branch tree → no matches; the supplied dududu PRs (#7, #8)
   have unrelated headRefs, so the `/tmp/dududu-audit` clone was not needed (R2, R8, R10).
5. **Working tree — N/A.** Branch is not checked out in any worktree, so no dirty check
   applies (R2).
6. **Recovery — fully verified.** The remote branch still exists on GitHub at the exact SHA
   (`git ls-remote origin refs/heads/research/r6-ac1-vectored-semantics` →
   `999b5305ae7d413a2a71acde926f8c6cfef6876a`), so before deletion it is fetchable by name.
   After deletion, `refs/pull/611/head` retains it server-side: `git fetch origin
   refs/pull/611/head` succeeded and `FETCH_HEAD` = the exact SHA (R8, R12). The tip's single
   parent `bdfd0d8d` is an ancestor of master, so the PR ref + master history reconstruct the
   entire commit. Recovery command: `git fetch origin refs/pull/611/head && git branch
   research/r6-ac1-vectored-semantics FETCH_HEAD`. The SHA is also recorded on master
   (`r9-archive-increment.md:96`) and in this audit (`REPOSITORY_REF_SNAPSHOT.md:64,105`,
   receipts R1/R8/R12). Unlike the `backup/pr-601-original-head` case, no history is lost by
   deletion here.

## Decision: DELETE_CANDIDATE

All required conditions verified in this session:

| Condition | Result | Evidence |
|---|---|---|
| 非 OPEN PR head | ✓ | Supplied list: no headRef match; live PR #611 CLOSED unmerged (R8) |
| 无独有未归档数据 | ✓ | All 34 files blob-identical on master at same paths; 0 A-only paths tip-vs-tip (R4, R6) |
| 无未推送产品变更 | ✓ | 1 research-only commit; already pushed (ls-remote = local ref); "no public API, no product code" (R1, R3, R8) |
| 原始数据已有可靠备份 | ✓ | #610 archive on master; 34/34 manifest sha256 match branch bytes (R9, R11) |
| 无运行中 Agent/Worktree 依赖 | ✓ | No worktree checkout; only prose provenance mention on master; no script/CI consumer (R2, R10) |
| HEAD SHA 与恢复步骤已记录 | ✓ | This report + receipts + master `r9-archive-increment.md:96`; `refs/pull/611/head` fetch-verified (R8, R12) |

Content-wise the branch is a strict subset of `origin/master`; its only distinct object is the
commit itself, preserved indefinitely by GitHub's `refs/pull/611/head`. **No deletion executed
in this phase.**

Not run / not applicable: no dududu clone (no dududu artifact relates to this branch); no
build or test run (audit is evidence-reachability only); enumeration of other live agent
sessions is not directly possible — the dependency conclusion rests on the worktree list, PR
state, and the master/working-tree reference scans above.
