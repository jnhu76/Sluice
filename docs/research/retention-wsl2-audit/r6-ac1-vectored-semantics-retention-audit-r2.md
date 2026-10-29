# Retention audit R2: `research/r6-ac1-vectored-semantics` (Sluice, remote `origin`)

Date: 2026-10-29 (this session). Non-destructive verification only; nothing
deleted, merged, or rewritten. Supersedes the prior pass
`r6-ac1-vectored-semantics-retention-value.md` (written 23:20 by an earlier
agent) — every load-bearing check below was re-run in THIS session with fresh
command output; receipts: `receipts/r6ac2-01..07-*.txt`.

## Name resolution and HEAD

- The ask's label `sluice/research/r6-ac1-vectored-semantics` has no verbatim
  ref (no local branch, no remote head, no `sluice/*` ref at all — R1, R6).
  The audit target is the unique matching branch: local tracking
  `origin/research/r6-ac1-vectored-semantics` = remote
  `refs/heads/research/r6-ac1-vectored-semantics`.
- HEAD = `999b5305ae7d413a2a71acde926f8c6cfef6876a`, single commit
  "research(ac1): E5 semantic closure for positional vectored I/O (#605)",
  2026-10-29 08:02:26 +0800, message states "Design evidence only: no public
  API, no product code, no v2-r1 change" (R1).

## Associated PR

None by headRef match in the supplied list. Live verification: the only OPEN
PR in the repo is #594 (head `research/593-key-path` — different branch, R6).
The branch's actual PR is **#611, CLOSED (unmerged)**, `headRefOid` =
`999b5305ae7d413a2a71acde926f8c6cfef6876a` exactly, base `master`
(https://github.com/jnhu76/Sluice/pull/611) (R6). The `(#605)` in the commit
subject is an **OPEN ISSUE** ("Design decision: positional vectored
read/write (AC-1, M1 Case A) — Awaiting Owner"), not a PR (R6).

## Checks (all run this session)

1. **Commit reachability** — `git branch -a --contains 999b5305` → only the
   branch itself; no tag/local/other ref contains it (R2).
   `git rev-list --count origin/master..<branch>` = **1**. Merge-base
   `bdfd0d8d` (2026-10-25) is an ancestor of master; master is 5 commits
   ahead of the fork point; the tip's content landed on master afterwards via
   `ac995641` = PR #610 (MERGED, 2026-10-29 13:30 +0800) (R2, R5).
2. **Blob reachability** — the ask's three-dot diff lists 34 added files
   (docs + probe + 24 raw outputs, all under
   `research/direct-beyond-parity/closure/r6-ac1*`). Per-file blob comparison
   against `origin/master` at identical paths: **34/34 SAME** (R3); the
   tip-vs-tip two-dot diff restricted to the r6-ac1 paths is **empty (0
   lines)** (R3). The branch has zero unique content.
   - Caveat recorded: `git rev-list --objects <branch> --not origin/master`
     lists the r6-ac1 blobs/trees as branch-only (R4). This is contradicted
     by three symmetric checks run this session: `git rev-list --objects
     origin/master` contains all sampled SHAs (a49e118c, f7b54ba6, 2bca64d7,
     c185c7c1, d4b8d1a1); `git rev-parse origin/master:<path>` equals the
     branch blob for every file; the restricted two-dot diff is empty. No
     shallow/grafts/replace mechanisms exist (R4). The per-file blob
     comparison (the ask's named arbitration) is authoritative.
3. **Unique evidence — none unarchived.** All 34 files exist verbatim on
   master (added by #610). Master's
   `research/direct-beyond-parity/EVIDENCE_MANIFEST.json` carries 34 r6-ac1
   entries; **all 34 sha256 values match the branch's blob bytes (34 matched,
   0 mismatched, verified this session)** (R5). The overlap is intentional and
   documented on master: `closure/r9-archive-increment.md:96` — "`closure/
   r6-ac1*` is additionally carried by the AC-1 design PR #611 … the two
   research branches intentionally overlap on that subtree" (R5). Issue #605
   remains OPEN, but the decision packet that serves it is archived on master.
4. **Active dependency — none found.** Not an OPEN PR head (#611 CLOSED; live
   open-PR list has only #594) (R6). Branch-name scan: exactly one master hit,
   `r9-archive-increment.md:96` (prose provenance, not a consumer); the
   working-tree grep outside this audit dir finds only the same tracked doc
   (R6). No worktree checks the branch out (`git worktree list`: `master` +
   14 detached `/tmp/sluice-val-*`/`-perf-*` validation worktrees at other
   SHAs) (R2). dududu `origin/main` has no r6-ac1/r6ac1/vectored-semantics
   reference (R6); the supplied dududu PRs #7/#8 have unrelated headRefs.
   Limitation: other live agent sessions cannot be enumerated from this
   sandbox; the conclusion rests on the observable absence of any checkout,
   consumer, or open PR.
5. **Working tree — N/A.** Not checked out anywhere, so no dirty check
   applies (R2).
6. **Recovery — verified.** `git ls-remote origin` shows BOTH
   `refs/heads/research/r6-ac1-vectored-semantics` AND
   `refs/pull/611/head` at the exact SHA `999b5305ae7d413a2a71acde926f8c6cfef
   6876a`; the pull ref persists server-side after branch deletion (R1).
   Post-deletion recovery (not executed):
   `git fetch origin refs/pull/611/head && git branch
   research/r6-ac1-vectored-semantics FETCH_HEAD`. The SHA is also recorded on
   master (`r9-archive-increment.md:96`) and in this report (R5, R7).

## Decision: DELETE_CANDIDATE

| Condition | Result | Evidence |
|---|---|---|
| 非 OPEN PR head | ✓ | Supplied list: no match; live: only #594 open, #611 CLOSED (R6) |
| 无独有未归档数据 | ✓ | 34/34 blob-identical at same paths on master; tip-vs-tip diff empty (R3) |
| 无未推送产品变更 | ✓ | Single research-only commit, already pushed (remote = same SHA); "no public API, no product code" (R1, R2) |
| 原始数据已有可靠备份 | ✓ | #610 archive on master; EVIDENCE_MANIFEST.json 34/34 sha256 match (R5) |
| 无运行中 Agent/Worktree 依赖 | ✓ (observable) | No worktree checkout; no script/CI consumer; no dududu coupling (R2, R6); live-agent enumeration out of sandbox reach — recorded as limitation |
| HEAD SHA 与恢复步骤已记录 | ✓ | This report + receipts + master `r9-archive-increment.md:96`; `refs/pull/611/head` verified present (R1, R7) |

**No deletion executed in this phase.** Not run: no build/test (audit is
evidence-reachability only); no dududu clone was needed beyond the existing
`/tmp/dududu-audit` grep (no dududu artifact references this branch).
