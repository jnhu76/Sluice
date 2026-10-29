# Recovery Manifest — retention-wsl2-audit

Complete HEAD SHAs and verified recovery steps for every audited branch, in
both repositories. This manifest is the durable credential for recovering
any audited ref even after its deletion. Recovery steps marked **verified**
were executed or preflight-checked during the audit rounds (receipts cited
in BRANCH_RETENTION_MATRIX.md); steps marked *recorded* are derived from
verified facts but were deliberately not executed, because executing them
would create refs and violate the campaign's non-destructive discipline.

- Sluice remote: `origin` = `git@github.com:jnhu76/Sluice.git` (default
  branch `master`), local clone `/home/hoo/Projects/Sluice`.
- dududu remote: `origin` = `https://github.com/jnhu76/dududu.git` (default
  branch `main`), local audit clone `/tmp/dududu-audit`.
- Nothing was deleted in any audit round; every "after deletion" pathway
  below is conditional on a future, Owner-authorized deletion (see
  BRANCH_CLEANUP_PLAN.md).

## Quick reference

| Branch | Repo | Full HEAD SHA | Post-deletion recovery channel |
|---|---|---|---|
| `master` | Sluice | `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` | Default branch (not deletable normally); `git fetch origin master` |
| `backup/pr-601-original-head-e4bb8833` | Sluice | `e4bb8833699d0ca5dee191883f3fec6d41b221b2` | NONE upstream after deletion — local clones only (see §A) |
| `backup/pr-612-original-head-70d48f5e` | Sluice | `70d48f5eb55a664544b5160cd4675d32150bb21e` | Partial: base pair via `refs/pull/602/head`; 4 P2C1 commits local-only (see §B) |
| `research/575-native-e5-atomic-rmw` | Sluice | `e9baafb9d117cbaf387dde9f4433ebbacb4a73b3` | `refs/pull/584/head` (verified live) |
| `research/593-key-path` | Sluice | `2714aa65fe4837d30f5fa3fcdb27db38c23b7136` | `refs/pull/594/head` while PR #594 exists (verified live) |
| `research/product-integration-records` | Sluice | `4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35` | NONE — no PR exists; local clones / pre-GC bare-SHA fetch only |
| `research/r6-ac1-vectored-semantics` | Sluice | `999b5305ae7d413a2a71acde926f8c6cfef6876a` | `refs/pull/611/head` (verified live, end-to-end fetch) |
| `r1-request-geometry` | dududu | `47cd1976701a2c9e73fc31470f45aec236a3e0b2` | `refs/pull/8/head` while PR #8 is OPEN (verified live) |
| `r5-async-host-probe` | dududu | `4eafbc06d6a2e476055fe45fc75656e7e28a2183` | Reachable from `main`, `r1-request-geometry`, `sluice-r0-e5-verdict` |
| `sluice-r0-closure` | dududu | `cc053cf9258b98546b16f9d2f08c5559fa73bc77` | In `main` history via merge `12a8712e` (deletion-state simulated) |
| `sluice-r0-e5-verdict` | dududu | `cc321a4994ee638493fecda0c437481528f5cea9` | In `main` history via merge `12a8712e` + `refs/pull/7/head` (verified live) |

## 1. Per-branch recovery steps

### 1.1 `master` (Sluice) — `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`

The audited ref is the default branch. GitHub protects it from ordinary
deletion; remote `refs/heads/master` and `HEAD` both carry the SHA
(ls-remote verified). Restore any lost local ref:

```
git fetch origin master            # verified concept: ls-remote shows remote master at this SHA
```

Fallbacks: 376 `refs/pull/*/head` refs retain merged PR history; remote
backup refs `backup/pr-601-original-head-e4bb8833` and
`backup/pr-612-original-head-70d48f5e` exist as additional anchors.

### 1.2 `backup/pr-601-original-head-e4bb8833` (Sluice) — `e4bb8833699d0ca5dee191883f3fec6d41b221b2`

**While the ref exists (verified, ls-remote):**

```
git fetch origin backup/pr-601-original-head-e4bb8833
```

**After deletion: NO upstream channel.** Verified in a disposable bare clone
holding ALL remote refs (heads + tags + all 376 `refs/pull/*/head`) with the
branch removed: `git for-each-ref --contains=e4bb8833` and
`--contains=ea2fc4fc` are both EMPTY. `refs/pull/601/head` = `2fcc7964`
(rewritten head) retains old base `bdfd0d8d` but neither original commit;
`refs/pull/602/head` = `77e5399a` likewise excludes them. See §A for the
full original-commit recovery path.

Local restore (works only while local clones still hold the objects, pre-gc;
*recorded*):

```
git push origin e4bb8833699d0ca5dee191883f3fec6d41b221b2:refs/heads/backup/pr-601-original-head-e4bb8833
```

Content recovery is unaffected by deletion (all 9 file blobs merged on
master via squashes `84a0cc1d`/#601 and `43cb8029`/#602); commit identity is
archived at `research/direct-beyond-parity/closure/r2-pr-boundary.md:78-90`
(SHAs + patch-ids `8ed5d3c0ae157859c44548fe838824aecbfbf520`,
`70a0899cf8d428c3bbee01163d8c34129df7b25c`, byte-verified) and the re-push
command at `r2-pr-boundary.md:64-65`.

### 1.3 `backup/pr-612-original-head-70d48f5e` (Sluice) — `70d48f5eb55a664544b5160cd4675d32150bb21e`

**While the ref exists (verified, ls-remote):**

```
git fetch origin backup/pr-612-original-head-70d48f5e
```

**After deletion:** partial upstream channel only. `4c3e698daf13e68912ccc19e39c1ceae12b916b8`
and `77e5399a45998dea396699fa2e063c94a665945a` remain reachable via
`refs/pull/602/head` = `77e5399a…` (ls-remote verified). The 4 original P2C1
commits (`42f5eef9…`, `cc8ad2ab…`, `b12fb551…`, `70d48f5e…`) have NO
guaranteed recovery channel (`merge-base --is-ancestor 70d48f5e e4c9588`
exit 1; no pull ref head equals the tip; bare-SHA fetch of unreachable
objects is not guaranteed server-side). See §B for the full original-commit
recovery path.

Local restore (pre-gc; *recorded*):

```
git push origin 70d48f5eb55a664544b5160cd4675d32150bb21e:refs/heads/backup/pr-612-original-head-70d48f5e
```

Chain identity is sha256-archived on master:
`research/direct-beyond-parity/EVIDENCE_MANIFEST.json:247,555`
(`source_sha` = the tip, twice), `WSL2_VALIDATION_HANDOFF.md:11-29` (chain
composition; dududu cherry-pick origins `d79244b0`/`31d18b6a`),
`closure/perf-p2c1/SUMMARY.md:10-13,161` (candidate SHA + detached-worktree
recreation command), `closure/r4-p2c1.md:15,70-71`. Final file content is
provably safe: `git diff --stat 70d48f5e 64508282` and `git diff --stat
70d48f5e e4c9588` on the 6 PR files are both empty (verified).

### 1.4 `research/575-native-e5-atomic-rmw` (Sluice) — `e9baafb9d117cbaf387dde9f4433ebbacb4a73b3`

Two independent routes verified live (branch ref and `refs/pull/584/head`
both ls-remote at the exact SHA):

```
git fetch origin research/575-native-e5-atomic-rmw
# or, after any branch deletion (CLOSED-unmerged PR #584):
git fetch origin refs/pull/584/head && git branch research/575-native-e5-atomic-rmw FETCH_HEAD
```

Local object DB also holds everything (`cat-file -t` verified for the tip
and evidence blobs). PR #584's page records the headSha for provenance.
Caveats recorded honestly: pull-ref persistence for CLOSED-unmerged PRs and
bare-SHA fetch (`uploadpack.allowAnySHA1InWant`) are platform behaviors not
guaranteed contractually; route 3 (bare-SHA fetch) was not executed against
the server. The branch holds 114 unarchived evidence files — recovery of the
REF does not substitute for the ARCHIVE_FIRST obligation (archive the data
to master first).

### 1.5 `research/593-key-path` (Sluice) — `2714aa65fe4837d30f5fa3fcdb27db38c23b7136`

Three verified anchors:

```
git fetch origin research/593-key-path                        # remote branch (ls-remote verified)
git fetch origin refs/pull/594/head                           # PR #594 head ref (ls-remote verified, same SHA)
git branch research/593-key-path 2714aa65fe4837d30f5fa3fcdb27db38c23b7136   # local object DB, pre-GC
```

The SHA is additionally recorded on master at
`research/direct-beyond-parity/closure/E5_CLOSURE_SNAPSHOT.md:150,167`.
PR #594 is OPEN — do not delete/close it before the E5 raw evidence
(1.33 GiB, `E5_RAW_ARCHIVE_PENDING`) is archived; see
`E5_RAW_EVIDENCE_PRESERVATION_PLAN.md`.

### 1.6 `research/product-integration-records` (Sluice) — `4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35`

**While the remote ref exists (verified, ls-remote — the ONLY ref anywhere
holding this SHA):**

```
git fetch origin research/product-integration-records
```

If only the local tracking ref was deleted, the fetch above restores it.
**After remote deletion: no PR fallback exists** (the push was deliberately
made without a PR, per `INTEGRATION_LOG.md` §8.4). Remaining channels:
explicit-SHA fetch before server GC (`git fetch origin 4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35`,
requires push permission and is not guaranteed), or local clones holding
the objects. This is precisely why the branch's decision is ARCHIVE_FIRST:
archive the 38 files to master (e.g. `git fetch origin
research/product-integration-records && git checkout FETCH_HEAD --
INTEGRATION_LOG.md INTEGRATION_PLAN.md
research/direct-beyond-parity/closure/perf-p2c1-finalhead/` — *recorded,
not executed*; a real archive PR should re-verify paths) before any
deletion.

### 1.7 `research/r6-ac1-vectored-semantics` (Sluice) — `999b5305ae7d413a2a71acde926f8c6cfef6876a`

Verified end-to-end this campaign: in a throwaway `/tmp` repo,
`git fetch --depth=1 origin refs/pull/611/head` returned exactly this SHA
(cat-file = commit; ls-tree contains all 34 r6-ac1 paths); the branch ref
itself also fetches to the same SHA.

```
git fetch origin refs/pull/611/head && git branch research/r6-ac1-vectored-semantics FETCH_HEAD
```

Deeper history needs nothing from the branch: its single parent
`bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` is an ancestor of origin/master,
and all 34 file blobs are on master (34/34 sha256-matched against
`EVIDENCE_MANIFEST.json`). Provenance also on master:
`closure/r9-archive-increment.md:94-96`.

### 1.8 `r1-request-geometry` (dududu) — `47cd1976701a2c9e73fc31470f45aec236a3e0b2`

Verified live: remote branch and `refs/pull/8/head` both at this SHA; PR #8
is OPEN.

```
git fetch origin r1-request-geometry
# or after any branch deletion (while PR #8 exists):
git fetch origin refs/pull/8/head && git branch r1-request-geometry FETCH_HEAD
```

Branch is KEEP (OPEN PR head); these steps are contingency only.

### 1.9 `r5-async-host-probe` (dududu) — `4eafbc06d6a2e476055fe45fc75656e7e28a2183`

All three recovery preconditions verified (ls-remote exact SHA; objects
fetchable, `cat-file -t` = commit; tip contained by `origin/main`,
`origin/r1-request-geometry`, `origin/sluice-r0-e5-verdict` — three
is-ancestor exit 0):

```
git fetch origin && git branch r5-async-host-probe 4eafbc06d6a2e476055fe45fc75656e7e28a2183
# or
git push origin 4eafbc06d6a2e476055fe45fc75656e7e28a2183:refs/heads/r5-async-host-probe
```

(*Recorded, not executed* — execution would create a ref.) Campaign
provenance on main: `docs/DUCKDB_FIO_R0_PREREG.md:225` ("branch
r5-async-host-probe @ 4eafbc0").

### 1.10 `sluice-r0-closure` (dududu) — `cc053cf9258b98546b16f9d2f08c5559fa73bc77`

Verified by an actual deletion-state simulation: a fresh
`git clone --single-branch --branch main` (main-only) contains the tip
(cat-file = commit; log metadata correct; is-ancestor of main; spot-checked
blob SHAs match). Recovery therefore needs no special ref at all:

```
git fetch origin && git push origin cc053cf9258b98546b16f9d2f08c5559fa73bc77:refs/heads/sluice-r0-closure
```

The tip entered main history via the true merge commit `12a8712eb5ff5f48e570b49b1e01ad5821837dca`
(parents `b94041d9…` + `cc321a49…`); three other remote branches also
contain it. No `refs/pull/N/head` backup exists — and is not needed.

### 1.11 `sluice-r0-e5-verdict` (dududu) — `cc321a4994ee638493fecda0c437481528f5cea9`

Three verified layers:

```
# (a) content is already on main (is-ancestor exit 0 via merge 12a8712e; tip tree == main tree 731e518a…)
# (b) remote branch still holds the tip directly (ls-remote verified)
git fetch origin sluice-r0-e5-verdict
# (c) even after remote branch deletion, PR #7's head ref persists (ls-remote verified):
git fetch origin refs/pull/7/head:refs/heads/sluice-r0-e5-verdict
```

11/11 introduced files byte-identical on main (incl. sha256-verified
`DUCKDB_FIO_R0_PREREG.md` and `lane-record.md`).

## §A. Backup branch original commits: `backup/pr-601-original-head-e4bb8833`

Full commit chain (merge-base → tip):

| Commit | Full SHA | Role |
|---|---|---|
| base | `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` | merge-base with master; retained on master and in `refs/pull/601/head` history |
| C1 perf | `ea2fc4fca1a65a63ef3664f71dab1014e98c3b25` | #600 C1 candidate — **`CANDIDATE_SHA` cited by master evidence** (`research/600-direct-excess-cost/PHASE3_VALIDATION.md:4`) |
| research tip | `e4bb8833699d0ca5dee191883f3fec6d41b221b2` | #601 pre-split original head; HEAD of the branch |

Recovery paths, by channel:

1. **While the branch ref exists (verified):** `git fetch origin
   backup/pr-601-original-head-e4bb8833` — fetches both commits.
2. **Historical PR refs: DO NOT cover these commits.** Verified two ways:
   ancestry (`ea2fc4fc` not an ancestor of `77e5399a`; `e4bb8833` ≠
   `2fcc7964`; neither an ancestor of #594's `2714aa65`) and, decisively, a
   disposable bare clone carrying all 376 `refs/pull/*/head` refs with the
   branch deleted: `for-each-ref --contains` for both SHAs is empty.
3. **After ref deletion:** the two commit objects survive ONLY in local
   clones (e.g. `/home/hoo/Projects/Sluice`'s
   `origin/backup/pr-601-original-head-e4bb8833` tracking ref), until their
   gc. Restore from any such clone (*recorded*):
   `git push origin e4bb8833699d0ca5dee191883f3fec6d41b221b2:refs/heads/backup/pr-601-original-head-e4bb8833`.
4. **Identity archive (loss of ref ≠ loss of identity):**
   `r2-pr-boundary.md:78-90` records both SHAs and stable patch-ids
   `8ed5d3c0ae157859c44548fe838824aecbfbf520` / `70a0899cf8d428c3bbee01163d8c34129df7b25c`
   (recomputed and byte-matched during the audit); diffstat 9 files
   +463/−98 at `:84`. All file content is on master (5 research docs +
   `semantic_range_test.cpp` blob-identical at master tip; 4 C1 code blobs
   identical to squash `43cb8029`); what only this ref preserves is the two
   commit objects themselves — which is why the master-archived P0
   directive keeps this branch (`KEEP` in the matrix).

**If the Owner ever decides to drop the ref**, first do one of: supersede
the P0 keep-alive designation on master, or relocate the objects (e.g.
`git tag keep-alive-600-candidate ea2fc4fca1a65a63ef3664f71dab1014e98c3b25`
and push the tag, or a `git bundle` stored with the audit evidence). Until
then: do not delete.

## §B. Backup branch original commits: `backup/pr-612-original-head-70d48f5e`

Full commit chain (merge-base → tip):

| Commit | Full SHA | Post-deletion upstream channel |
|---|---|---|
| base | `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` | master history (safe) |
| #602-era base | `4c3e698daf13e68912ccc19e39c1ceae12b916b8` | ancestor of `refs/pull/602/head` — safe |
| #602 head | `77e5399a45998dea396699fa2e063c94a665945a` | `refs/pull/602/head` itself (ls-remote verified) — safe |
| P2C1 test seams | `42f5eef999ed524de8eef1cf8de3dfb05a20c675` | NONE — local clones only |
| P2C1 mechanism | `cc8ad2abd07bf48f03ed99bce0ee8bba126039d6` | NONE — local clones only |
| P2C1 refactor | `b12fb551db87c507d9def2e38065bbc0d826b79d` | NONE — local clones only |
| ledger tip (HEAD) | `70d48f5eb55a664544b5160cd4675d32150bb21e` | NONE — local clones only |

Recovery paths:

1. **While the branch ref exists (verified):** `git fetch origin
   backup/pr-612-original-head-70d48f5e` — fetches the whole chain.
2. **After ref deletion:** recover the base pair via
   `git fetch origin refs/pull/602/head` (= `77e5399a…`). The 4 P2C1
   commits and the 2 unique intermediate `src/blocking_file.cpp` blobs
   (`6448f1a1`, `eb9b35be`) are then recoverable only from local clones
   pre-gc (*recorded*): `git push origin
   70d48f5eb55a664544b5160cd4675d32150bb21e:refs/heads/backup/pr-612-original-head-70d48f5e`.
3. **Identity archive (sha256-manifested, on master):**
   `EVIDENCE_MANIFEST.json:247,555` (`source_sha`), `WSL2_VALIDATION_HANDOFF.md:11-29`
   (chain over base `77e5399a`; commits are cherry-picks of dududu-side
   `d79244b0`/`31d18b6a` — a partial second home for the change content),
   `closure/perf-p2c1/SUMMARY.md:161` (worktree recreation command from the
   candidate SHA), `closure/r4-p2c1.md:15,70-71`. Final content of all 6
   files equals master (and equals the rebased tips `645082821a11ffcc1f835ab46088f54a5add4a64`
   / `e4c9588…`, empty diffs verified).

The zero-residual alternative, if this branch is ever authorized for
deletion: archive first (bundle or tag the tip SHA), per
BRANCH_CLEANUP_PLAN.md Item 1 precondition 3.

## Preflight and postflight checklists (for any future deletion)

Preflight (per branch, immediately before deletion):

1. `git ls-remote origin <ref>` returns exactly the SHA recorded in this
   manifest (any drift = stop and re-audit).
2. `git fetch origin <ref>` (or the PR-ref equivalent) succeeds into a
   scratch repo.
3. For backup branches: confirm the applicable §A/§B residuals are covered
   by an explicit Owner decision recorded next to the execution log.

Postflight (immediately after deletion):

1. Re-fetch via the post-deletion channel of §Quick reference and verify
   `git rev-parse FETCH_HEAD` equals the recorded SHA.
2. Record date, operator, command output in this file.
3. No `git gc`/`git prune` on local clones until step 1 passes for every
   deleted branch whose recovery depends on local objects
   (`backup/pr-612…`, and `backup/pr-601…` only if ever authorized
   contrary to its KEEP decision).
