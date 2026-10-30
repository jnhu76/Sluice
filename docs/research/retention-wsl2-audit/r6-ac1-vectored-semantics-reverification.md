# Re-verification: retention decision for `research/r6-ac1-vectored-semantics`

- Date: 2026-10-29 (session re-audit)
- Ask label: `sluice/origin/research/r6-ac1-vectored-semantics`; the ask's
  `origin/origin/...` spelling noted in the original audit still does not
  resolve (`git rev-parse --verify origin/origin/research/r6-ac1-vectored-semantics`
  → `fatal: Needed a single revision`); the corrected form
  `refs/remotes/origin/research/r6-ac1-vectored-semantics` resolves to
  `999b5305ae7d413a2a71acde926f8c6cfef6876a` (receipt r6ac1-rv-01).
- Verdict: **DELETE_CANDIDATE stands (复核通过).** All three re-verified
  fields hold. Nothing was deleted or modified by this re-audit; it created
  one own verification worktree `/tmp/sluice-val-r6ac1-reverify` (left for
  the script's cleanup, per instructions).

## Basis

Local `origin/master` = `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` and
GitHub `refs/heads/master` = the same SHA (`git ls-remote`, receipt
r6ac1-rv-01), so no fetch of master was needed; all comparisons are against
current master. GitHub still serves
`refs/heads/research/r6-ac1-vectored-semantics` and `refs/pull/611/head`,
both at `999b5305ae7d413a2a71acde926f8c6cfef6876a`.

## Field 1 — uniqueEvidence: VERIFIED, none unarchived

1. Exclusion form re-run (ask-named):
   `git rev-list --objects refs/remotes/origin/research/r6-ac1-vectored-semantics ^origin/master`
   → 34 entries. Classified entry-by-entry against the FULL master reachable
   object set: 29 entries are actually reachable from master (over-reporting
   of shared objects confirmed), leaving only the tip commit and 3 unique
   subtrees as genuinely not on master (receipt r6ac1-rv-02). Note: the
   original audit reported "26 blobs" for this command; this re-run counted
   34 total entries without type filtering — a count-basis difference, not a
   conclusion difference.
2. Authoritative object-level set difference: `comm -13` of the full
   reachable sets (`git rev-list --objects <ref>` for branch = 34,680
   objects, master = 35,416 objects) → exactly **5 unique objects = 1 commit
   (the tip) + 4 trees; zero blobs** (receipt r6ac1-rv-02). No file content
   in the branch is unreachable from master.
3. Per-file blob comparison — run over the ENTIRE branch tree, not just the
   34 branch-introduced files: `git ls-tree -r` = 3,060 files; for each,
   `git rev-parse <branch>:<path>` vs `git rev-parse origin/master:<path>` →
   3,052 blob-identical at identical paths, **0 missing on master**, 8 paths
   differ (receipt r6ac1-rv-03). Those 8 DIFF blobs are the 8 `M` entries of
   the tip-vs-tip diff (master-side forward evolution) and each branch-side
   blob was verified IN master's reachable history (receipt r6ac1-rv-03b).
4. Tip-vs-tip: `git diff --name-status origin/master <branch>` → 0 A-entries,
   8 M + 790 D, all master-side (receipt r6ac1-rv-03). Three-dot
   (merge-base) diff = 34 files; all 34 blob-identical at identical paths on
   master tip (receipt r6ac1-rv-03b).
5. Master manifest cross-check: sha256 of each of the 34 branch-introduced
   blobs vs `research/direct-beyond-parity/EVIDENCE_MANIFEST.json` on master
   → 34 matched / 0 mismatched / 0 not-in-manifest (receipt r6ac1-rv-06).

## Field 2 — activeDependency: VERIFIED, none

- PR #611: `state=CLOSED`, `mergedAt=null`, `headRefName=research/r6-ac1-vectored-semantics`,
  `headRefOid=999b5305...`, base `master` (receipt r6ac1-rv-04). Unmerged.
- Open PRs: only #594 (`research/593-key-path`) — no OPEN PR uses this
  headRef (receipt r6ac1-rv-04).
- `git worktree list --porcelain`: 15 worktrees (main on master +
  14 detached `/tmp/sluice-val-*`/`/tmp/sluice-perf-*` at other SHAs) — none
  checks out the branch (receipt r6ac1-rv-04). More verification worktrees
  exist than at the original audit's snapshot; conclusion unchanged.
- No local branch matches r6-ac1; `git branch -a --contains 999b5305` → only
  the remote-tracking ref itself; `git rev-list --count origin/master..<branch>`
  = 1 (receipt r6ac1-rv-04).
- Master references: exactly one, prose provenance at
  `research/direct-beyond-parity/closure/r9-archive-increment.md:96`
  (mentions branch name and SHA `999b5305`); no script/CI consumer on master
  (receipt r6ac1-rv-04).
- dududu: `git grep -i dududu <branch-tree>` → no matches; no clone needed
  (receipt r6ac1-rv-04).
- Limitation (same as original audit): live enumeration of other agent
  sessions is not possible; the no-dependency conclusion rests on the worktree
  list, CLOSED/unmerged PR, and the master reference scan.

## Field 3 — recovery: VERIFIED

- Tip commit `999b5305` has exactly one parent `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`
  (`git cat-file -p`); merge-base with origin/master = that same commit;
  `git merge-base --is-ancestor bdfd0d8d origin/master` exit 0 (receipt
  r6ac1-rv-05). Deleting the branch therefore loses no master history; only
  the tip commit object is not on master, and it is reconstructible.
- Real recovery fetch executed this session in own worktree
  `/tmp/sluice-val-r6ac1-reverify`: `git fetch origin refs/pull/611/head`
  → exit 0, `FETCH_HEAD = 999b5305ae7d413a2a71acde926f8c6cfef6876a`
  (receipt r6ac1-rv-05). Post-deletion recovery:
  `git fetch origin refs/pull/611/head && git branch research/r6-ac1-vectored-semantics FETCH_HEAD`.
- Fallbacks: GitHub branch ref itself still present at the exact SHA
  (ls-remote, r6ac1-rv-01); SHA recorded on master at
  `research/direct-beyond-parity/closure/r9-archive-increment.md:96`
  (receipt r6ac1-rv-04).

## Receipts (this re-audit, prefix r6ac1-rv)

- `receipts/r6ac1-rv-01-ref-and-head.txt` — ref spelling, exact SHA, ls-remote
- `receipts/r6ac1-rv-02-object-uniqueness.txt` — rev-list both forms, comm set difference, classification
- `receipts/r6ac1-rv-03-blob-per-path.txt` — full-tree 3,060-file per-path comparison, tip-vs-tip and three-dot diffs
- `receipts/r6ac1-rv-03b-diff-blob-membership.txt` — 8 DIFF blobs in master history; 34/34 three-dot files MATCH
- `receipts/r6ac1-rv-04-dependency.txt` — PR 611, open PRs, worktrees, contains, master refs, dududu
- `receipts/r6ac1-rv-05-recovery.txt` — tip parentage, ancestry, real pull-ref fetch test
- `receipts/r6ac1-rv-06-manifest-sha256.txt` — 34/34 sha256 MATCHED vs master manifest

Original audit report and receipts (r6ac1-01…12) are unchanged alongside:
`r6-ac1-vectored-semantics-retention-value.md`, `receipts/r6ac1-*`.
