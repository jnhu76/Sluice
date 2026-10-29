# Recheck: `backup/pr-601-original-head-e4bb8833` — DELETE_CANDIDATE re-verification

Date: 2026-10-29. Read-only recheck of the three most critical fields of the
retention audit for remote branch `backup/pr-601-original-head-e4bb8833`
(head `e4bb8833699d0ca5dee191883f3fec6d41b221b2`, 2 commits over merge-base
`bdfd0d8d`). Full command log: `receipts/backup-pr-601-original-head-e4bb8833-recheck.txt`
(R1–R11). No branch, PR, worktree or evidence directory was touched; the only
ref deletion happened inside a disposable `/tmp` bare clone (R9).

**Verdict: 复核通过 — all three fields hold; decision stays DELETE_CANDIDATE.**

## 1. uniqueEvidence — confirmed, with one methodological correction

- The named check `git rev-list --objects <branch> ^origin/master` reproduces
  the original audit's **21 objects** (also 21 against `^84a0cc1d` and
  `^bdfd0d8d`).
- **Correction:** 21 is *not* a unique-object count. A controlled minimal-repo
  experiment (`/tmp/sluice-val-pr601-t/excl-test`) showed listing-mode
  `rev-list --objects A ^B` lists blobs that are byte-identical to and
  reachable from the excluded side (identical `docs.md` blob `fe1dce6a` on both
  branches was still listed). The 21 include 9 blobs and 8 subtree trees that
  all exist on master.
- Authoritative measure — full object-set difference: branch 34,667 objects
  minus master 35,416 objects = **exactly 4 unique objects: the 2 commits
  (`e4bb8833`, `ea2fc4fc`) and their 2 root trees (`20485468`, `006c31d1`).
  Zero unique blobs, zero unique subtrees.**
- Per-file blob compare over the 9 files of
  `diff --name-only origin/master...<branch>`: the 5
  `research/600-direct-excess-cost/*.md` docs (`b9b0b233`, `1741c098`,
  `a5bbf1cf`, `89141091`, `cfad9902`), `tests/semantic_range_test.cpp`
  (`0d3eb7e0`) and even the subtree tree `45d388ea` are identical to
  `origin/master`; the 3 code files (`e39760ae` file_semantics.hpp,
  `6660a423` blocking_file.cpp, `febe6515` xmake/tests.lua) differ only from
  *current* master and are blob-identical to `43cb8029` (the #602 squash, an
  ancestor of master — `merge-base --is-ancestor` exit 0);
  `git diff ea2fc4fc 43cb8029 -- <4 code paths>` is empty.
- Identity archive verified: `research/direct-beyond-parity/closure/r2-pr-boundary.md:78-90`
  (merged via #610) records both commit SHAs and patch-ids; recomputed
  `git show <c> | git patch-id --stable` = `8ed5d3c0ae157859c44548fe838824aecbfbf520`
  and `70a0899cf8d428c3bbee01163d8c34129df7b25c` — exact matches. Diffstat
  `9 files changed, 463 insertions(+), 98 deletions(-)` matches `:84`.

No unarchived unique content exists.

## 2. activeDependency — confirmed: None

- `git grep -I -nE 'backup/pr-601|pr-601-original-head' origin/master`: exactly
  6 prose lines in `research/direct-beyond-parity/closure/r2-pr-boundary.md`
  (`:64,:65,:70,:73,:145,:168`) — recovery instructions plus the deletion
  deferral. No script, CI or build consumer.
- Working-tree grep: same 6 lines plus hits only inside
  `docs/research/retention-wsl2-audit/` (earlier audit reports/receipts —
  audit artifacts, not consumers).
- `git worktree list --porcelain`: 16 worktrees, none on this branch.
- `gh pr list --repo jnhu76/Sluice --state open` → only **#594
  `research/593-key-path`**. The branch name references PR #601, which is
  MERGED (squash `84a0cc1d`), as is #602 (squash `43cb8029`) — both verified
  ancestors of `origin/master`. dududu is irrelevant: the branch lives in the
  Sluice remote.

## 3. recovery — confirmed and strengthened

- Now: `git ls-remote origin refs/heads/backup/pr-601-original-head-e4bb8833`
  → `e4bb8833699d0ca5dee191883f3fec6d41b221b2`; the branch is still fetchable
  from GitHub at the exact SHA.
- After deletion: a disposable bare clone of *all* remote refs (all heads +
  tags, plus all **376** `refs/pull/*/head` refs fetched via
  `+refs/pull/*/head:refs/pull/*`) with the audited ref removed shows
  `git for-each-ref --contains=e4bb8833` and `--contains=ea2fc4fc` both **empty**
  — no remaining remote ref retains either commit. `refs/pull/601`
  (`2fcc7964`, 2324 commits) keeps old base `bdfd0d8d`
  (`merge-base --is-ancestor` exit 0) but neither tip. (The original audit's
  "1345 commits" count differs from this session's 2324; the substantive
  containment conclusion is identical.)
- Content remains fully recoverable from master (all 9 blobs + the research
  subtree tree shared with master; code blobs at ancestor `43cb8029`); commit
  identity remains recoverable from the archived SHAs/patch-ids. Post-deletion
  the 2 commit objects survive only in local clones (e.g. the main repo's
  `origin/backup/...` remote-tracking ref) until gc, plus any out-of-band
  GitHub retention that cannot be verified from here.
- Deletion precondition (`r2-pr-boundary.md:166-170`: after #602 AND #601
  merge): **satisfied** — both merged.
