# Remote Branch Delete Readiness Plan — retention-wsl2-audit

**Status: PLAN ONLY — DELETE READINESS ASSESSMENT.** This document deletes
nothing and pushes nothing. During its generation only the following
state-touching commands ran: `git fetch -p origin` in both repositories (a
local remote-tracking refresh), plus one local commit of this file on
`research/retention-wsl2-audit` in the worktree `/tmp/sluice-closeout-614`.
No branch (local or remote) was deleted, no `git push` was executed, no PR
was merged or closed. Delete eligibility below is an audit conclusion, not
an authorization: **every row requires explicit per-branch Owner
authorization before any deletion.** 满足删除条件不等于可以删除。

Generated: 2026-10-30T07:57:11+08:00 (decision-time identity = the
`for-each-ref` snapshots in §9, taken after `git fetch -p origin`).

Repositories:

- `jnhu76/Sluice` — clone `/home/hoo/Projects/Sluice`, origin
  `git@github.com:jnhu76/Sluice.git`, default branch `master`
  (`84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`).
- `jnhu76/dududu` — clone `/home/hoo/Source/dududu`, origin
  `git@github.com:jnhu76/dududu.git`, default branch `main`
  (`12a8712eb5ff5f48e570b49b1e01ad5821837dca`).
  **dududu has no `origin/master`** (`git symbolic-ref refs/remotes/origin/HEAD`
  → `refs/remotes/origin/main`; `for-each-ref` lists no `refs/remotes/origin/master`).
  All `origin/master`-based command templates are therefore applied against
  `origin/main` in dududu, same structure, and this substitution is recorded
  wherever it occurs.

## 1. Method and a measurement caveat that changes reading of the counts

Every per-branch field below was produced by the exact prescribed commands
(listed per row). One caveat materially affects interpretation and is
recorded here once:

`git rev-list --objects origin/<b> ^origin/master | wc -l` (the prescribed
"unique objects" metric) excludes objects via the **commit-graph boundary**:
it walks the negative side's trees only down the shared ancestry, so objects
that `master` acquired **after** the branch's fork point — including
byte-identical copies of the branch's own blobs (e.g. landed via a later
archive/squash commit) — do **not** participate in exclusion. This was
demonstrated concretely on `research/r6-ac1-vectored-semantics`: the
prescribed command reports 34, yet all 34 branch files are blob-identical on
the `master` tip (§2) and a full reachable-set difference reports only 5
unique objects (1 commit + 4 trees, 0 blobs).

Therefore each Sluice row reports BOTH:

- **boundary count** — the prescribed command's number (this is the value in
  the DeletePlan `uniqueObjects` field); and
- **true residual** — full set difference of reachable object IDs
  (`comm -23` of `git rev-list --objects <side> | awk '{print $1}' | sort -u`
  against the union of the negative sides' full object sets), which is the
  number that answers "what actually becomes unreachable if this branch is
  deleted". For ARCHIVE_FIRST rows the true residual is computed against
  `master ∪ archive-branch`.

Squash-merged branches are judged on the two identities separately, per the
audit rule: (a) commit-chain identity — are the branch's commits reachable
from the default branch; (b) file-content identity — are the branch's blobs
present on the default branch (tip or history). `git branch --merged` alone
is never sufficient (both backups and r6-ac1 are absent from
`--merged` output yet have fully merged content).

## 2. Sluice — `research/r6-ac1-vectored-semantics`

| Field | Value |
|---|---|
| HEAD SHA | `999b5305ae7d413a2a71acde926f8c6cfef6876a` (`git rev-parse origin/research/r6-ac1-vectored-semantics`) |
| OPEN PR head | No. `gh pr list --head research/r6-ac1-vectored-semantics --state open --repo jnhu76/Sluice` → empty. History: PR **#611 CLOSED, not merged** (`mergeCommit: null`), closed 2026-10-29T05:39:33Z. |
| Unique objects (boundary, prescribed) | **34** (`git rev-list --objects origin/research/r6-ac1-vectored-semantics ^origin/master \| wc -l`) |
| True residual | **5** = 1 commit (`999b5305`) + 4 trees (root, `research`, `research/direct-beyond-parity`, `.../closure`); **0 blobs** |
| Unique commits | **1** (`git log --oneline origin/master..origin/research/r6-ac1-vectored-semantics \| wc -l`): `999b5305 research(ac1): E5 semantic closure for positional vectored I/O (#605)` |
| Worktree dependency | None. `git worktree list \| grep research/r6-ac1-vectored-semantics` → no match. (`/tmp/sluice-val-r6ac1-reverify` is detached at `84e1b7d5`, not on this branch.) |
| Recovery | `git push origin 999b5305ae7d413a2a71acde926f8c6cfef6876a:refs/heads/research/r6-ac1-vectored-semantics` |

Content identity: all 34 files changed since merge-base `bdfd0d8d`
(`docs…closure/r6-ac1.md`, 6 docs, 2 probes, 25 raw evidence files) are
**blob-identical on the master tip** — each compared by
`git rev-parse origin/master:<path>` equal to `origin/<branch>:<path>`;
landed via `ac995641 research(direct-beyond-parity): archive #603 P0-P6
evidence … (#610)` (`git log --oneline origin/master -- research/direct-beyond-parity/closure/r6-ac1`
→ only `ac995641`). Chain identity: 1 commit not on master (PR #611 was
closed unmerged; the content reached master through the #610 archive commit).

**Delete eligibility: DELETE-READY on content** (0 unique blobs; everything
the branch's tree carries is byte-identical on master). Loss on deletion is
chain-archaeology only (1 commit + 4 tree objects). **Owner per-branch
authorization required.**

## 3. Sluice — `backup/pr-612-original-head-70d48f5e`

| Field | Value |
|---|---|
| HEAD SHA | `70d48f5eb55a664544b5160cd4675d32150bb21e` |
| OPEN PR head | No (`gh pr list --head backup/pr-612-original-head-70d48f5e --state open` → empty; `--state all` → no PR ever used this head). The preserved work reached master via squash-merge **#612** = `84e1b7d5 … (#608) (#612)`. |
| Unique objects (boundary, prescribed) | **42** |
| True residual | **16** = 6 commits + 8 trees + **2 blobs** (both intermediate versions of `src/blocking_file.cpp`: `6448f1a1`, `eb9b35be`; the tip version is on master) |
| Unique commits | **6**: `70d48f5e`, `b12fb551`, `cc8ad2ab`, `42f5eef9`, `77e5399a`, `4c3e698d` (chain includes the pre-squash originals of both the #602-line and #612-line work) |
| Worktree dependency | None (`git worktree list \| grep backup/pr-612-original-head-70d48f5e` → no match) |
| Recovery | `git push origin 70d48f5eb55a664544b5160cd4675d32150bb21e:refs/heads/backup/pr-612-original-head-70d48f5e` |

Squash identity, judged separately: **chain NOT merged** (6 unique commits;
`git branch -r --merged origin/master` does not list it). **Content
merged**: all 6 files changed since merge-base `bdfd0d8d`
(`docs/roadmap/v2-conformance.md`, `include/sluice/detail/file_semantics.hpp`,
`src/blocking_file.cpp`, `tests/direct_composition_fault_test.cpp`,
`tests/semantic_range_test.cpp`, `xmake/tests.lua`) are blob-identical on
the master tip (per-path blob compare, all SAME).

**Delete eligibility: DELETE-READY on content** (tip content fully on
master). Residual loss = the 6-commit original chain plus 2 mid-chain
`src/blocking_file.cpp` versions. **Owner per-branch authorization
required.**

## 4. Sluice — `backup/pr-601-original-head-e4bb8833` (KEEP-reason recheck)

| Field | Value |
|---|---|
| HEAD SHA | `e4bb8833699d0ca5dee191883f3fec6d41b221b2` |
| OPEN PR head | No (`--state open` empty; no PR ever used this head). Preserved work reached master via squash-merge **#601** = `84a0cc1d … (#601)`. |
| Unique objects (boundary, prescribed) | **21** |
| True residual | **4** = 2 commits + 2 trees; **0 blobs** |
| Unique commits | **2**: `e4bb8833 research(direct): #600 E5 excess-cost attribution and C1 validation evidence`, `ea2fc4c perf(direct): dispatch admission verdicts without Precheck materialization (#600 C1)` |
| Worktree dependency | None |
| Recovery | `git push origin e4bb8833699d0ca5dee191883f3fec6d41b221b2:refs/heads/backup/pr-601-original-head-e4bb8833` |

KEEP-reason recheck, the two identities judged separately as required:

- **File-content identity — fully preserved by master.** Of the 9 files
  changed since merge-base `bdfd0d8d`: the 5 research evidence docs
  (`research/600-direct-excess-cost/{DIRECT_COST_DAG,DIRECT_MACHINE_COST_LEDGER,EVIDENCE_INDEX,PHASE3_VALIDATION,PHASE_I_COST_ATTRIBUTION}.md`)
  and `tests/semantic_range_test.cpp` are blob-identical on the master tip
  (docs landed via squash `84a0cc1d`, confirmed by
  `git show --stat 84a0cc1d` listing exactly those 5 files); the 3 remaining
  (`include/sluice/detail/file_semantics.hpp`, `src/blocking_file.cpp`,
  `xmake/tests.lua`) differ at tip because master evolved them via later
  merged work, and the branch's exact blob versions
  (`e39760ae`, `6660a423`, `febe6515`) are each **present in master
  history** (membership in the full `git rev-list --objects origin/master`
  ID set: IN-MASTER-HISTORY ×3). True residual blobs: 0.
- **Commit-chain identity — NOT preserved anywhere else.** The 2 original
  commits exist only on this ref (the #601 squash replaced them; the
  sibling backup `pr-612-original-head` carries a *different* commit
  `4c3e698d` with an equivalent message, not these objects —
  `git rev-list --objects origin/backup/pr-601-original-head-e4bb8833
  ^origin/master ^origin/backup/pr-612-original-head-70d48f5e | wc -l` = 21,
  i.e. no reduction).

**Delete eligibility: DELETE-READY on content; the KEEP reason reduces to
commit-chain archaeology only.** Whether chain identity alone justifies
remote retention is an explicit Owner decision (KEEP or DELETE), recorded
per branch. **Owner per-branch authorization required either way.**

## 5. Sluice — `research/575-native-e5-atomic-rmw` (ARCHIVE_FIRST)

| Field | Value |
|---|---|
| HEAD SHA | `e9baafb9d117cbaf387dde9f4433ebbacb4a73b3` |
| OPEN PR head | No (`--state open` empty). History: PR **#584 CLOSED, not merged** ("…NO_CHANGE_JUSTIFIED", `mergeCommit: null`). |
| Unique objects (boundary, prescribed, `^origin/master`) | **196** |
| Unique objects (boundary, prescribed anchored form, `^origin/master ^origin/research/archive-575-evidence`) | **196** (unchanged — the archive branch forked after the source and is not its descendant, so the boundary walk cannot use its objects; see §1) |
| True residual vs `master ∪ archive-575-evidence` | **91** = 9 commits + 51 trees + **31 blobs** |
| Unique commits | **9** (`45a34296`…`e9baafb9`, R0→R9 campaign chain) |
| Worktree dependency | None for this branch. (Note: `/tmp/sluice-closeout-575` is a worktree on the **archive** branch `research/archive-575-evidence`, not on the source branch.) |
| Recovery | `git push origin e9baafb9d117cbaf387dde9f4433ebbacb4a73b3:refs/heads/research/575-native-e5-atomic-rmw` |

**ARCHIVE_FIRST condition:** deletion eligibility is preconditioned on
archive PR **#615** (head `research/archive-575-evidence`) being **merged**
and the branch's unique data landing on `master`. At generation time #615 is
**OPEN** (`gh pr view 615`: state OPEN, base `master`, no mergeCommit).
Material finding for that precondition: even after #615 merges, the true
residual is **91 objects including 31 blobs** not carried by the curated
archive — sampled residual paths: `docs/research/mw-hw-575-native-e5/R5-CALIBRATION.md`,
`R6-THREADPOOL.md`, `R8-CAUSAL-ANALYSIS.md`, `scripts/run_575_ab.sh`,
`raw/ab/ab-ur-d8/out.tmp`, `patches/…`. The archive branch does capture 103
of the source's 194 master-unique objects byte-identically, but not all.

**Delete eligibility: NOT YET — ARCHIVE_FIRST.** Requires #615 merged AND an
explicit Owner disposition of the 31 residual blobs (extend #615 to carry
them, or accept the curated archive as sufficient and accept the loss).
**Owner per-branch authorization required.**

## 6. Sluice — `research/product-integration-records` (ARCHIVE_FIRST)

| Field | Value |
|---|---|
| HEAD SHA | `4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35` |
| OPEN PR head | No (`--state open` empty; no PR ever used this head). |
| Unique objects (boundary, prescribed, `^origin/master`) | **43** |
| Unique objects (boundary, anchored `^origin/master ^origin/research/archive-product-integration-records`) | **43** (unchanged; same boundary-semantics reason as §5) |
| True residual vs `master ∪ archive-product-integration-records` | **8** = 2 commits + 5 trees + **1 blob** |
| Unique commits | **2**: `8c3adf6c research(integration): archive E5 product integration records and Phase 3.3 final-head perf receipts`, `4d621d91 research(integration): record archive push SHA in Phase 8 log` |
| Worktree dependency | None for this branch. (`/tmp/sluice-closeout-integration` is on the **archive** branch.) |
| Recovery | `git push origin 4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35:refs/heads/research/product-integration-records` |

**ARCHIVE_FIRST condition:** archive PR **#616** (head
`research/archive-product-integration-records`) must merge and the unique
data must land on master. At generation time #616 is **OPEN** (base
`master`, no mergeCommit). The archive branch carries 35 of the source's 43
master-unique objects byte-identically (incl. root `INTEGRATION_PLAN.md`
`82879bb0` and the first `INTEGRATION_LOG.md` version). Post-#616 residual
= **8 objects**, the only blob being the **final root `INTEGRATION_LOG.md`
`e87db785`** — the version whose Phase 8 entry records the archive push SHA,
written after the archive was pushed, hence not inside #616.

**Delete eligibility: NOT YET — ARCHIVE_FIRST.** Requires #616 merged AND
Owner disposition of the final `INTEGRATION_LOG.md` version (fold it into
#616, or accept its loss). **Owner per-branch authorization required.**

## 7. dududu — `r5-async-host-probe`, `sluice-r0-closure`, `sluice-r0-e5-verdict`

Base-branch note: dududu has no `origin/master`; default branch is
`origin/main` (§ header). Commands below are the prescribed forms with
`origin/main` substituted.

| Field | `r5-async-host-probe` | `sluice-r0-closure` | `sluice-r0-e5-verdict` |
|---|---|---|---|
| HEAD SHA | `4eafbc06d6a2e476055fe45fc75656e7e28a2183` | `cc053cf9258b98546b16f9d2f08c5559fa73bc77` | `cc321a4994ee638493fecda0c437481528f5cea9` |
| OPEN PR head | No (`gh pr list --head <b> --state open --repo jnhu76/dududu` empty; no PR ever) | No (same, empty) | No (`--state open` empty; PR **#7 MERGED** into `main`, mergeCommit `12a8712e` = current `main` tip) |
| Unique objects (`^origin/main`) | **0** | **0** | **0** |
| True residual | 0 | 0 | 0 |
| Unique commits (`origin/main..<b>`) | **0** | **0** | **0** |
| Ancestor of `origin/main` | YES (`git merge-base --is-ancestor`) | YES | YES |
| Worktree dependency | None (`git worktree list` in `/home/hoo/Source/dududu` shows only the main checkout on local `r1-parquet-sluice-bench`) | None | None |
| Recovery | `git push origin 4eafbc06d6a2e476055fe45fc75656e7e28a2183:refs/heads/r5-async-host-probe` | `git push origin cc053cf9258b98546b16f9d2f08c5559fa73bc77:refs/heads/sluice-r0-closure` | `git push origin cc321a4994ee638493fecda0c437481528f5cea9:refs/heads/sluice-r0-e5-verdict` |

All three are plain ancestry-merged refs (also listed by
`git branch -r --merged origin/main`); no squash subtlety arises — both
chain and content identities are fully on `main`. Sibling-anchored counts
(`^` the other two) are 0 for each, trivially consistent.

**Delete eligibility: DELETE-READY** for all three (0 unique commits, 0
unique objects, no open PR, no worktree). **Owner per-branch authorization
required — one explicit authorization per branch, not a blanket approval.**

## 8. Open-PR inventory at generation time (context, non-candidates)

`gh pr list --state open --repo jnhu76/Sluice`:

| PR | Head | Note |
|---|---|---|
| #594 | `research/593-key-path` | out of scope (open PR head) |
| #614 | `research/retention-wsl2-audit` | this audit's own PR; out of scope |
| #615 | `research/archive-575-evidence` | ARCHIVE_FIRST vehicle for §5 |
| #616 | `research/archive-product-integration-records` | ARCHIVE_FIRST vehicle for §6 |

## 9. Decision-time remote identity snapshots (`for-each-ref`, post-fetch)

Captured 2026-10-30T07:57:11+08:00, immediately after
`cd /home/hoo/Projects/Sluice && git fetch -p origin` and
`cd /home/hoo/Source/dududu && git fetch -p origin` (both produced no
output: nothing new to fetch, nothing to prune).

Sluice (`git -C /home/hoo/Projects/Sluice for-each-ref refs/remotes/origin`):

```
refs/remotes/origin/HEAD 84e1b7d578c2c58a3c46033487d9e1ac7c639b8b (2026-10-29 19:41:19 +0800)
refs/remotes/origin/backup/pr-601-original-head-e4bb8833 e4bb8833699d0ca5dee191883f3fec6d41b221b2 (2026-10-27 00:50:07 +0800)
refs/remotes/origin/backup/pr-612-original-head-70d48f5e 70d48f5eb55a664544b5160cd4675d32150bb21e (2026-10-29 08:47:06 +0800)
refs/remotes/origin/master 84e1b7d578c2c58a3c46033487d9e1ac7c639b8b (2026-10-29 19:41:19 +0800)
refs/remotes/origin/research/575-native-e5-atomic-rmw e9baafb9d117cbaf387dde9f4433ebbacb4a73b3 (2026-10-24 19:11:32 +0800)
refs/remotes/origin/research/593-key-path 2714aa65fe4837d30f5fa3fcdb27db38c23b7136 (2026-10-26 05:08:06 +0800)
refs/remotes/origin/research/archive-575-evidence e75371adbb8aaaf6da974e8ded7984d6ca4ace35 (2026-10-30 07:02:30 +0800)
refs/remotes/origin/research/archive-product-integration-records 623f62f1d5309342c4d6198ca9561778bfd51f36 (2026-10-30 07:03:01 +0800)
refs/remotes/origin/research/product-integration-records 4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35 (2026-10-29 20:44:37 +0800)
refs/remotes/origin/research/r6-ac1-vectored-semantics 999b5305ae7d413a2a71acde926f8c6cfef6876a (2026-10-29 08:02:26 +0800)
refs/remotes/origin/research/retention-wsl2-audit 86277ece0e3c7723f0aa2c8de7c1c892b7bd8f97 (2026-10-30 01:57:52 +0800)
```

dududu (`git -C /home/hoo/Source/dududu for-each-ref refs/remotes/origin`):

```
refs/remotes/origin/HEAD 12a8712eb5ff5f48e570b49b1e01ad5821837dca (2026-10-29 12:47:38 +0800)
refs/remotes/origin/main 12a8712eb5ff5f48e570b49b1e01ad5821837dca (2026-10-29 12:47:38 +0800)
refs/remotes/origin/r1-request-geometry 47cd1976701a2c9e73fc31470f45aec236a3e0b2 (2026-10-29 20:00:28 +0800)
refs/remotes/origin/r5-async-host-probe 4eafbc06d6a2e476055fe45fc75656e7e28a2183 (2026-10-29 08:36:36 +0800)
refs/remotes/origin/sluice-r0-closure cc053cf9258b98546b16f9d2f08c5559fa73bc77 (2026-10-29 08:15:25 +0800)
refs/remotes/origin/sluice-r0-e5-verdict cc321a4994ee638493fecda0c437481528f5cea9 (2026-10-29 10:41:02 +0800)
```

## 10. Command appendix (exact commands behind every field)

Per branch and repo, run in the respective clone:

```
git fetch -p origin
git rev-parse origin/<branch>
git rev-list --objects origin/<branch> ^origin/master [<extra anchors>] | wc -l
git log --oneline origin/master..origin/<branch> | wc -l
git log --oneline origin/master..origin/<branch>
git worktree list | grep <branch>
gh pr list --head <branch> --state open [--repo jnhu76/<repo>]
gh pr list --head <branch> --state all --json number,state,title,mergedAt,mergeCommit [--repo jnhu76/<repo>]
```

True-residual computation (per §1):

```
git rev-list --objects origin/master | awk '{print $1}' | sort -u        # base set
git rev-list --objects origin/<branch> | awk '{print $1}' | sort -u      # branch set
comm -23 <branch set> <base set [∪ archive-branch set]> | wc -l          # true residual
git cat-file --batch-check='%(objectname) %(objecttype)' < residual-ids  # type breakdown
```

Squash/content identity checks:

```
mb=$(git merge-base origin/master origin/<branch>)
git diff --name-only $mb origin/<branch>
git rev-parse origin/master:<path>  vs  git rev-parse origin/<branch>:<path>   # per path
git branch -r --merged origin/master | grep -F <branch>                        # shown, never relied on alone
```

Recovery verification status: recovery commands are recorded per row but
were **not executed** (execution would require `git push`, which this audit
is forbidden to perform). Their form is the standard
`git push origin <full-sha>:refs/heads/<branch>` ref-restore; the objects
are present in the local clones used for this audit at the listed SHAs.
