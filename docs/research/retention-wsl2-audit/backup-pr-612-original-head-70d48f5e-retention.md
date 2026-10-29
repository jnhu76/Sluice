# Retention audit: `backup/pr-612-original-head-70d48f5e` (Sluice, remote `origin`)

Date: 2026-10-29. Scope: non-destructive verification only; nothing deleted or modified.
Receipts: `receipts/backup-pr-612-original-head-70d48f5e.txt` (R1–R9 referenced below).

## Ref and HEAD verification

- The audit label `sluice/backup/pr-612-original-head-70d48f5e` resolves as repo `sluice` +
  remote `origin` + branch `backup/pr-612-original-head-70d48f5e`. All checks use
  `origin/backup/pr-612-original-head-70d48f5e` (R1).
- Actual HEAD = `70d48f5eb55a664544b5160cd4675d32150bb21e` — **matches** the short SHA
  embedded in the branch name. Commit: `docs(ledger): record #608 P2C1 composition evidence
  and THREAD-01 boundary`, 2026-10-29 08:47:06 +0800 (R1).

## Associated PR: None (by headRef match)

No PR in the supplied list has `headRef` = `backup/pr-612-original-head-70d48f5e`. The name
references PR #612 (MERGED, `headRef sluice-608-p2c1-read-compose`, headSha
`e4c9588ae8754ea44cf52194294103a7795aa1a0`; `ls-remote` confirms
`refs/pull/612/head` = `e4c9588...`). The backup tip is **not** an ancestor of `e4c9588`
(`git merge-base --is-ancestor` exit 1): after #602 landed as squash `43cb8029`, the PR
branch was rebased from its original base `77e5399a` onto master (`e4c9588` is a merge of
`4b962abb` + rebased chain `3bf551bd`→`64508282`). The backup ref preserves the
**pre-rebase original head** of MERGED PR #612 (R2, R7).

## Findings

1. **Commit reachability** — only
   `refs/remotes/origin/backup/pr-612-original-head-70d48f5e` contains the tip
   (`git branch -a --contains`). 6 commits unique vs `origin/master` (`84e1b7d5`),
   merge-base `bdfd0d8d`: base pair `4c3e698d` + `77e5399a` (the pre-squash #602-era chain;
   `77e5399a` = PR #602 headSha) plus the 4 original P2C1 commits `42f5eef9` (test: seams +
   killers) → `cc8ad2ab` (perf: prevalidated steps) → `b12fb551` (refactor: dead-store
   shrink) → `70d48f5e` (docs: ledger) (R3).
2. **Blob/content reachability** — 42 unique objects (6 commits + 24 trees + 12 blobs). All
   12 unique blobs are intermediate review-iteration versions of the same 6 files; no other
   content exists on the branch. Per-file blob compare vs `origin/master`: **all 6 files at
   the tip are blob-identical** — `docs/roadmap/v2-conformance.md` (`cfd8e7db`),
   `include/sluice/detail/file_semantics.hpp` (`d0090897`), `src/blocking_file.cpp`
   (`8a27525d`), `tests/direct_composition_fault_test.cpp` (`1945fc79`),
   `tests/semantic_range_test.cpp` (`0d3eb7e0`), `xmake/tests.lua` (`c55e97be`) (R4).
3. **Unique evidence** — none unarchived. The branch carries no research documents, scripts
   or receipts (changed-file list vs merge-base is ledger + product code + tests + xmake
   only). The original chain's identity and provenance are **archived on master** by MERGED
   PR #610: `research/direct-beyond-parity/EVIDENCE_MANIFEST.json:247,555`
   (`"source_sha": "70d48f5e..."` inside the sha256-manifested archive),
   `WSL2_VALIDATION_HANDOFF.md:11-29` (chain composition and stacking over `77e5399a`;
   notes the chain commits are cherry-picks of dududu-side `d79244b0`/`31d18b6a`),
   `closure/perf-p2c1/SUMMARY.md:10-13,161` (candidate SHA + detached-worktree recreation
   command), `closure/r4-p2c1.md:15,70-71`. The 6 PR files are byte-identical between the
   original tip `70d48f5e` and the rebased tip `64508282` (empty diff), and the rebased
   review chain is preserved server-side via `refs/pull/612/head` (R5).
4. **Active dependency** — none. Not an OPEN PR head (no headRef match; #612 MERGED).
   Working-tree grep finds the branch name only in (a) this audit documentation directory
   `docs/research/retention-wsl2-audit/` and (b) the audit tooling itself
   (`.zcode/workflow-drafts/…dwf.ts:276`, `.zcode/workflow-runs/…mjs` — untracked, this
   audit run's own driver listing the branch as its audit subject). `.github` CI, `xmake/`,
   `apps/`, `src/`, `include/`, `tests/` contain no reference (R6).
5. **Working tree** — not applicable: the branch is not checked out in any of the 15
   worktrees (main repo at `master`; all `/tmp/sluice-val-*` and `/tmp/sluice-perf-*`
   worktrees detached at `84e1b7d5`, `77e5399a` or `2714aa65`), so no dirty check applies
   (R6).
6. **Recovery** — the remote branch still exists on GitHub at the exact SHA (`ls-remote` →
   `70d48f5eb55a664544b5160cd4675d32150bb21e`), so it is fetchable by name today
   (`git fetch origin backup/pr-612-original-head-70d48f5e`). Full ancestry matrix: the tip
   is contained in **no other ref** — not master, not the 4 research branches or
   `backup/pr-601…` (`branch -a --contains`), and none of the 8 supplied Sluice PR heads
   (`0592e5cb`, `5593b186`, `e9baafb9`, `2714aa65`, `2fcc7964`, `77e5399a`, `d40a811d`,
   `e4c9588` all "not-contained"; #601/#610 verified after isolated fetches of their pull
   refs). The base pair additionally remains via `refs/pull/602/head` = `77e5399a`. After
   deleting the backup ref, the 4 original P2C1 commits would have **no guaranteed recovery
   channel** (no PR ref contains them; fetching unreachable objects by bare SHA is not
   guaranteed server-side) — the same residual the pr-601 sibling audit accepted, because
   their full content is merged and their identity is archived on master (R7).

## Decision: DELETE_CANDIDATE

All required conditions verified in this session:

| Condition | Result | Evidence |
|---|---|---|
| 非 OPEN PR head | ✓ | No headRef match in supplied list; #612 MERGED; `refs/pull/612/head` = rebased head (R2) |
| 无独有未归档数据 | ✓ | All 6 files blob-identical on master; branch holds no research docs/scripts/receipts (R4) |
| 无未推送产品变更 | ✓ | Content landed via #612 squash `84e1b7d5`; tip blobs byte-identical; remote branch exists (R4, R7) |
| 原始数据已有可靠备份 | ✓ | Content on master; chain identity + provenance in #610's checksummed `EVIDENCE_MANIFEST.json`, `SUMMARY.md`, `WSL2_VALIDATION_HANDOFF.md`, `r4-p2c1.md` (R5) |
| 无运行中 Agent/Worktree 依赖 | ✓ | No worktree checks it out; no script/CI reference outside audit docs and the audit tooling itself (R6) |
| HEAD SHA 与恢复步骤已记录 | ✓ | Master archive records `source_sha` twice + recreation command (`SUMMARY.md:161`); `REPOSITORY_REF_SNAPSHOT.md:60`, `state-freeze-report.md:18` and this audit record the SHA; remote branch fetchable today (R5, R7) |

Special backup-branch check: satisfied in the same sense as the pr-601 sibling audit
(`backup-pr-601-original-head-e4bb8833-retention.md`) — today the original submission is
fully recoverable: immutable SHA recorded (master archive + audit docs), historical PR #612
with its review chain (`refs/pull/612/head`), and object storage holding everything while
the ref exists. Residual, same as pr-601: after remote deletion the 4 original commit
objects survive only in local clones until gc — acceptable because their full content is
merged (stronger here: 6/6 files blob-identical vs 3/6 differing in the pr-601 case) and
their identity is archived on master. One difference from pr-601: no archived document
explicitly designates `backup/pr-612…` for deletion (`r2-pr-boundary.md:167-169` designated
pr-601's), so the deletion remains an owner decision. **No deletion executed in this
phase.**

Not run / not applicable: no dududu check (branch lives in Sluice; the supplied dududu PR
heads `r1-request-geometry`/`sluice-r0-e5-verdict` are different-repo refs unrelated to this
branch, so `/tmp/dududu-audit` was not cloned).

Audit-trail note: to close the ancestry matrix, `refs/pull/{7,8,601,610}/head` were
temporarily fetched into `refs/remotes/audit-pull-{7,8,601,610}` and deleted afterwards with
`git update-ref -d`; fetched objects remain unreachable in `.git/objects`; no gc was run. No
branch, worktree, PR, or product file was modified (R8).
