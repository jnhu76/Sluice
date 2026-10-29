# Retention audit R3 (re-verification): `research/r6-ac1-vectored-semantics` (Sluice, remote `origin`)

Date: 2026-10-30 (this session). Scope: read-only re-verification of the
three most consequential fields of the R2 audit
(`r6-ac1-vectored-semantics-retention-audit-r2.md`) — `uniqueEvidence`,
`activeDependency`, `recovery` — at the scale the re-verification ask names:
full `git rev-list --objects` arbitration and per-file blob comparison were
re-run in full, not sampled. Nothing was deleted, merged, or rewritten; the
only writes are this report and receipts `receipts/r6ac3-01..06-*.txt`, plus
throwaway scratch under `/tmp/sluice-val-r6ac3*` (no `git worktree` objects
created; no fetch performed into the main repository).

## Name resolution and HEAD

- HEAD = `999b5305ae7d413a2a71acde926f8c6cfef6876a` (commit, single parent
  `bdfd0d8d`), "research(ac1): E5 semantic closure for positional vectored
  I/O (#605)", 2026-10-29 08:02:26 +0800 (R1). The ask's label
  `sluice/research/r6-ac1-vectored-semantics` still has no verbatim ref; the
  audit target is `research/r6-ac1-vectored-semantics` (local tracking
  `origin/…` = remote `refs/heads/…`).
- Still tip-only: `git rev-list --count origin/master..<branch>` = **1**;
  merge-base `bdfd0d8d` is an ancestor of master (exit 0); master is 5
  commits ahead of the fork point; no tag and no other ref contains the tip
  (`branch -a --contains` → only the branch itself); repo is not shallow and
  has no grafts/replace refs (R1).

## Field 1 — uniqueEvidence: 复核通过，无独有未归档内容

**(a) `git rev-list --objects` re-run and fully arbitrated (R2).** Both
exclusion forms (`--not origin/master` and `^origin/master`) list the same
34 objects. Each was classified with `git cat-file -t` and checked for
membership in the full master reachable-object set
(`git rev-list --objects origin/master`, 35,416 objects). Result:

- **UNIQUE objects: exactly 5, and none is content.** The tip commit
  `999b5305` itself plus 4 container trees (the tip's root tree, `research/`,
  `research/direct-beyond-parity/`, `closure/` — their hashes differ from
  master's only because master's siblings gained the later r9 increment).
- **UNIQUE blobs: 0.** All 29 remaining listed objects — including every
  r6-ac1 file blob — are present in master's reachable set. R2 had sampled
  only 5 SHAs for this arbitration; this pass closes the R2 caveat
  exhaustively: the branch-only listing of r6-ac1 blobs is rev-list's known
  over-approximation (objects emitted from the interesting side before
  uninteresting marks propagate), now disproven per-object rather than by
  sample.

**(b) Per-file blob comparison re-run at full scale (R3).** The three-dot
diff enumerates all 34 branch-side changes (34 `A` paths, all under
`research/direct-beyond-parity/closure/r6-ac1*`); `git rev-parse
<ref>:<path>` on both sides: **34 SAME / 0 DIFF / 0 MISSING**. Tip-vs-tip
two-dot diff restricted to the r6-ac1 paths: **0 lines**.

**(c) Completeness cross-checks.** The full two-dot diff master→branch has
status distribution `790 D + 8 M` and **no `A` entries** — no file exists on
the branch tip that does not exist on master tip. The 139 inserted lines come
from the 8 `M` files (`docs/roadmap/v2-conformance.md`,
`include/sluice/detail/file_semantics.hpp`,
`scripts/check_cpp_comment_authority.py`,
`scripts/test_check_cpp_comment_authority.py`, `src/blocking_file.cpp`,
`tests/direct_composition_fault_test.cpp`,
`tests/semantic_range_test.cpp`, `xmake/tests.lua`): master modified these
after the fork; the branch merely retains their fork-point versions — shared
history, not branch-authored content (R3 tail).

**(d) Archive integrity re-verified.** Master's
`research/direct-beyond-parity/EVIDENCE_MANIFEST.json` carries 34 `r6-ac1`
path entries; hashing the branch's blob bytes (`git cat-file blob`) gives
**34 matched / 0 mismatched** against the manifest sha256 values, and the
same verify against master's own blobs also gives 34/34 (R4). Provenance on
master unchanged: `closure/r9-archive-increment.md:94-96` names the branch,
commit `999b5305`, PR #611, and states the two research branches
intentionally overlap on `closure/r6-ac1*`.

## Field 2 — activeDependency: 复核通过，无可观察依赖

- Live PR state re-checked (R5): the only OPEN PR in Sluice is #594 (head
  `research/593-key-path`). PR #611: `state=CLOSED`, `mergeCommit=null`
  (unmerged), `headRefOid = 999b5305…` exactly. Issue #605 is OPEN (owner
  decision tracker); it consumes the decision packet archived on master, not
  the branch.
- Branch-name references: the full name `r6-ac1-vectored-semantics` appears
  in the tracked master tree exactly once — the prose provenance at
  `closure/r9-archive-increment.md` (line 94-96) — and nowhere else in the
  working tree outside this audit dir (the `.zcode/` hits seen mid-session
  belong to this cleanup workflow's own transient, git-ignored runtime state
  and are not consumers). No script/CI/build consumer: `git grep` over
  `xmake.lua`, `xmake/`, `scripts/`, `.github/` on origin/master → no match.
- No worktree checks the branch out: `git worktree list` shows master plus
  15 detached `/tmp` validation worktrees, none at `999b5305` (R1).
- dududu re-checked live (R5): remote refs are `main`,
  `r1-request-geometry`, `r5-async-host-probe`, `sluice-r0-closure`,
  `sluice-r0-e5-verdict` (+ pull refs 1,2,3,7,8) — no r6-ac1/vectored ref;
  `git grep -i -e r6-ac1 -e r6ac1 -e vectored-semantics origin/main` → no
  match; the only open dududu PR is #8 (`r1-request-geometry`).
- Standing limitation (unchanged from R2): other live agent sessions cannot
  be enumerated from this sandbox; the conclusion rests on the observable
  absence of any checkout, consumer, or open PR.
- Minor doc staleness noted, non-blocking: `r9-archive-increment.md:96`
  describes PR #611 as "OPEN"; #611 has since closed unmerged. The
  provenance (branch + commit + PR) it records remains accurate. No fix
  applied in this audit-only phase.

## Field 3 — recovery: 复核通过，SHA 端到端可恢复

- Remote presence re-verified (R1, R6): `git ls-remote origin` shows both
  `refs/heads/research/r6-ac1-vectored-semantics` and `refs/pull/611/head`
  at exactly `999b5305ae7d413a2a71acde926f8c6cfef6876a`.
- **End-to-end fetch proof, new this pass (R6):** in a throwaway empty repo
  under `/tmp/sluice-val-r6ac3-recovery` (main repo untouched),
  `git fetch --depth=1 origin refs/pull/611/head` returns FETCH_HEAD =
  `999b5305…`, `cat-file -t` = commit, and the fetched tip tree contains all
  34 r6-ac1 paths. The branch ref fetches to the same SHA. This upgrades R2's
  "pull ref present per ls-remote" to a demonstrated fetch of the exact SHA.
- Post-deletion recovery remains recorded, not executed:
  `git fetch origin refs/pull/611/head && git branch
  research/r6-ac1-vectored-semantics FETCH_HEAD`. Provenance additionally
  recorded on master (`r9-archive-increment.md:94-96`) and in this report.
  One caveat stated honestly: that GitHub retains `refs/pull/N/head` for
  closed PRs indefinitely is platform behavior, not something verifiable
  from this sandbox; what is verified today is that the ref is present and
  fetchable at the exact SHA.

## Decision: DELETE_CANDIDATE（维持）

| Re-verified field | Result | Evidence |
|---|---|---|
| uniqueEvidence | ✓ 复核通过 | rev-list 全量仲裁 0 独有 blob（仅 tip commit + 4 容器 tree 不在 master 集）；34/34 per-file blob SAME；受限 two-dot diff 0 行；two-dot 无 `A` 状态；manifest sha256 34/34 双侧匹配（R2, R3, R4） |
| activeDependency | ✓ 复核通过 | 唯一 open PR #594（不同 head）；#611 CLOSED unmerged；#605 为 OPEN issue；master 树分支名仅 1 处 prose 引用；无 script/CI 引用；无 worktree checkout；dududu 无任何 r6-ac1/vectored 引用（R5, R1）；live-session 枚举不可达，记为限制 |
| recovery | ✓ 复核通过 | 远端双 ref = 999b5305；一次性仓库 shallow fetch `refs/pull/611/head` 端到端取回该 SHA 与 34 文件 tip tree；恢复命令已记录未执行（R6） |

**No deletion executed in this phase.** Not run: builds/tests (audit is
evidence-reachability only); enumeration of other live agent sessions
(outside sandbox reach — recorded as limitation).
