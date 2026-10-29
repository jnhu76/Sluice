# Branch Retention Matrix — retention-wsl2-audit

Consolidated per-branch retention matrix for the WSL2 branch-retention audit
campaign. It merges the per-branch audit entries (17 audit records covering 11
unique branches across two repositories) into one section per branch.

- Repositories: `jnhu76/Sluice` (`/home/hoo/Projects/Sluice`, default branch
  `master`) and `jnhu76/dududu` (local audit clone `/tmp/dududu-audit`, default
  branch `main`).
- All decisions are audit conclusions only. Nothing was deleted, merged,
  closed or force-pushed in any round; see BRANCH_CLEANUP_PLAN.md for the
  plan-only cleanup proposal and RECOVERY_MANIFEST.md for full-SHA recovery
  steps.
- Decision vocabulary: `KEEP` (retain), `ARCHIVE_FIRST` (unique unarchived
  research data must land on the default branch before any deletion),
  `DELETE_CANDIDATE` (meets all cleanup conditions; deletion remains an Owner
  decision), `UNKNOWN`.

## Decision summary

| # | Branch | Repo | HEAD | Final decision | Audit rounds | Cleanup-eligible |
|---|--------|------|------|----------------|--------------|------------------|
| 1 | `master` | Sluice | `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` | KEEP | 1 | No (default branch) |
| 2 | `backup/pr-601-original-head-e4bb8833` | Sluice | `e4bb8833699d0ca5dee191883f3fec6d41b221b2` | KEEP | 2 (conflicting; KEEP stands) | No |
| 3 | `backup/pr-612-original-head-70d48f5e` | Sluice | `70d48f5eb55a664544b5160cd4675d32150bb21e` | DELETE_CANDIDATE | 3 (early ARCHIVE_FIRST dissent documented) | Yes (Owner-gated) |
| 4 | `research/575-native-e5-atomic-rmw` | Sluice | `e9baafb9d117cbaf387dde9f4433ebbacb4a73b3` | ARCHIVE_FIRST | 2 (agree) | No |
| 5 | `research/593-key-path` | Sluice | `2714aa65fe4837d30f5fa3fcdb27db38c23b7136` | KEEP | 2 (agree) | No |
| 6 | `research/product-integration-records` | Sluice | `4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35` | ARCHIVE_FIRST | 2 (agree) | No |
| 7 | `research/r6-ac1-vectored-semantics` | Sluice | `999b5305ae7d413a2a71acde926f8c6cfef6876a` | DELETE_CANDIDATE | 3 (agree) | Yes |
| 8 | `r1-request-geometry` | dududu | `47cd1976701a2c9e73fc31470f45aec236a3e0b2` | KEEP | 1 | No (OPEN PR head) |
| 9 | `r5-async-host-probe` | dududu | `4eafbc06d6a2e476055fe45fc75656e7e28a2183` | DELETE_CANDIDATE | 2 (agree) | Yes |
| 10 | `sluice-r0-closure` | dududu | `cc053cf9258b98546b16f9d2f08c5559fa73bc77` | DELETE_CANDIDATE | 2 (agree) | Yes |
| 11 | `sluice-r0-e5-verdict` | dududu | `cc321a4994ee638493fecda0c437481528f5cea9` | DELETE_CANDIDATE | 2 (agree) | Yes |

Tally by audit record (17 records): KEEP 5, ARCHIVE_FIRST 5,
DELETE_CANDIDATE 7, UNKNOWN 0.
Tally by unique branch after round resolution (11 branches): KEEP 4,
ARCHIVE_FIRST 2, DELETE_CANDIDATE 5, UNKNOWN 0.

---

## 1. `master` (Sluice, origin/master)

- **Branch**: The audit label `sluice/master` does not exist as a literal ref
  (`git rev-parse sluice/master` and `refs/remotes/sluice/master` both fatal);
  it resolves as the Sluice repository's default branch `origin/master`. The
  resolved HEAD matched the expected SHA, confirming the interpretation. Both
  `git ls-remote` listings (Sluice and dududu) contain no `sluice/*` ref.
- **HEAD**: `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` (matches expectation).
- **Associated PR**: None. No PR headRef equals `master`, and no headSha equals
  `84e1b7d5…` (nearest: #612 headSha `e4c9588a…`, the merged PR branch head,
  not master).
- **Commit Reachability**: The audited ref is the default branch itself.
  `git branch -a --contains 84e1b7d5…` → `master`, `remotes/origin/HEAD`,
  `remotes/origin/master` (the only three refs); remote `HEAD` and
  `refs/heads/master` point at the same SHA (ls-remote). `git tag --contains`
  → empty (all 4 tags `pre-rewrite-2026-09-07`, `v0.0.1`–`v0.0.3` predate it).
  `git rev-list --count origin/master..origin/master` → 0 (trivial: both sides
  are the same ref).
- **Blob Reachability**: `git rev-list --objects origin/master ^origin/master`
  → 0; `git diff --name-only origin/master...origin/master` → 0. Trivial empty
  sets (template commands with the same ref on both sides); branch content is
  100% the default-branch content.
- **Unique Evidence**: No unique unarchived data. Master itself carries the
  archives: #610 evidence under `research/direct-beyond-parity/` (MISSION,
  P0–P6, `EVIDENCE_MANIFEST.json`, verified via `git show --stat ac995641`)
  and `research/closure/`; `docs/research/` additionally holds `mw-hw-1`,
  `native-linux-m2`, `state-complexity`, `uring-router-580` archive dirs.
- **Active Dependency**: Yes, by design. (a) CI: `.github/workflows/ci.yml:6,18`
  push/pull_request triggers include `- master`. (b) Issue templates
  `.github/ISSUE_TEMPLATE/{bug,audit,performance}.yml` use `master@<sha>` as a
  placeholder; `config.yml` links master paths. (c) `CONTRIBUTING.md:38`
  requires branching from master. No branch-name references in `scripts/` or
  `xmake/` (grep empty).
- **Working Tree**: Main workspace `/home/hoo/Projects/Sluice` checks out this
  branch (`git worktree list` first line `[master] @84e1b7d5`).
  `git status --porcelain` → only `?? docs/research/retention-wsl2-audit/`
  (this audit family's untracked report directory); no tracked-file
  modification or deletion.
- **Recovery**: Remote always present: `git ls-remote origin` →
  `refs/heads/master = 84e1b7d5…`, remote HEAD identical; `git fetch origin
  master` restores any lost local ref. GitHub protects the default branch from
  ordinary deletion; `refs/pull/*/head` (376 refs) retain PR history; two
  remote backup refs (`backup/pr-601-original-head-e4bb8833`,
  `backup/pr-612-original-head-70d48f5e`) provide additional fallbacks.
- **Decision**: KEEP
- **Confirmation**: All required checks executed and receipted in-session
  (HEAD resolution, PR matching, containment, rev-list counts/objects, diff,
  unique evidence, CI/script grep, worktree list, status, recovery ls-remote).
  No deletions, merges/closes, force-pushes or builds; no product-code change.
- **Evidence**: `sluice-master-retention-value.md`;
  `receipts/sluice-master-01-head-resolution.txt` … `-06-610-archive.txt`.

---

## 2. `backup/pr-601-original-head-e4bb8833` (Sluice)

- **Branch**: Remote branch `backup/pr-601-original-head-e4bb8833` on origin;
  local tracking ref `refs/remotes/origin/backup/pr-601-original-head-e4bb8833`.
  The `sluice/` prefix in the audit label is the repo name, not part of the
  ref. The P0-planned name `backup/600-research-pre-split-e4bb8833` is absent
  from the remote; the pushed branch fulfills that Stage-0 role.
- **HEAD**: `e4bb8833699d0ca5dee191883f3fec6d41b221b2` (matches the
  branch-name suffix). Chain: `e4bb8833` (research tip) → `ea2fc4fc`
  (`ea2fc4fca1a65a63ef3664f71dab1014e98c3b25`, the #600 C1 perf commit,
  `CANDIDATE_SHA`) → merge-base `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`.
- **Associated PR**: None by headRef match. The name references MERGED PR
  #601 (squash `84a0cc1d`); this branch is #601's pre-split, pre-rewrite
  original head, set aside when the work split into #601 (research) + #602
  (C1 product change, squash `43cb8029`). Both squashes are ancestors of
  origin/master (`merge-base --is-ancestor` exit 0). Neither original commit
  is an ancestor of `refs/pull/601/head` (`2fcc7964`, rewritten) nor of PR
  #602 head `77e5399a` nor of OPEN PR #594 head `2714aa65`.
- **Commit Reachability**: Contained only by this branch's own remote-tracking
  ref (`git branch -a --contains`, before any ref deletion). 2 commits unique
  vs origin/master. `refs/pull/601/head` keeps old base `bdfd0d8d` but neither
  tip.
- **Blob Reachability**: Authoritative full object-set difference: branch
  34,667 objects vs master 35,416 → exactly 4 unique objects (2 commits +
  2 root trees); zero unique blobs or subtrees. The earlier "21 unique
  objects" from listing-mode `rev-list --objects … ^origin/master` is git's
  known over-approximation (proven by a controlled minimal-repo experiment,
  `/tmp/sluice-val-pr601-t/excl-test`). Per-file: 5
  `research/600-direct-excess-cost/*.md` docs + `tests/semantic_range_test.cpp`
  + subtree tree `45d388ea` blob-identical to origin/master tip; 3 code files
  blob-identical to ancestor `43cb8029` (#602 squash);
  `git diff ea2fc4fc 43cb8029 -- <4 code paths>` empty.
- **Unique Evidence**: No unarchived file content — all 9 file blobs are on
  master (via #601/#602 squashes). What only this ref preserves is the two
  commit objects themselves, and `ea2fc4fc` is the `CANDIDATE_SHA` cited by
  master's own evidence (`research/600-direct-excess-cost/PHASE3_VALIDATION.md:4`,
  part of merged #601). Commit identity (both SHAs + patch-ids
  `8ed5d3c0ae157859c44548fe838824aecbfbf520` /
  `70a0899cf8d428c3bbee01163d8c34129df7b25c`, byte-verified; diffstat
  9 files +463/−98) is archived at
  `research/direct-beyond-parity/closure/r2-pr-boundary.md:78-90` — but the
  #610 archive documents the branch without containing its objects.
- **Active Dependency**: Documented evidence keep-alive.
  `P0_BASELINE_AND_OWNERSHIP.md` §2.3.1 (lines ~328-336), §2.3.7 (~365-368),
  §2.4.1 (~383) — merged authority on master via #610 — designate this branch
  as the remote keep-alive for `ea2fc4fc` (CANDIDATE_SHA) + `e4bb8833`,
  "Keep it long-term … recommended permanently", "branch cleanup must not
  delete" the backup. The P0-planned second LOCAL keep-alive branch at
  `e4bb8833` no longer exists (empty `git branch --contains`), making this ref
  the sole holder. No script/CI/worktree consumer; dududu has zero references.
- **Working Tree**: Not applicable — no worktree checks out this branch
  (main repo on master; all `/tmp/sluice-val-*`/`/tmp/sluice-perf-*` worktrees
  detached at other SHAs).
- **Recovery**: Today fully recoverable — remote branch at the exact SHA;
  full SHA + re-push command archived on master
  (`r2-pr-boundary.md:64-65`). After deleting the ref: NO remaining remote ref
  retains either commit (verified in a disposable bare clone holding all
  heads + tags + all 376 `refs/pull/*/head`, with the audited ref deleted:
  `for-each-ref --contains=e4bb8833` and `--contains=ea2fc4fc` both empty);
  objects survive only in local clones until gc; content remains recoverable
  from master, commit identity from the archived SHAs/patch-ids.
- **Decision**: KEEP — with a documented round conflict. Pass-1 (23:07) and a
  later recheck (23:56, `backup-pr-601-original-head-e4bb8833-recheck.md`)
  concluded DELETE_CANDIDATE (deletion precondition "after #602 AND #601
  merge" at `r2-pr-boundary.md:166-170` is satisfied; content/identity fully
  archived). The corrective pass-2 (`backup-pr-601-original-head-e4bb8833-retention.md`,
  23:43) found the P0 keep-alive designation those rounds missed (P0 names the
  branch as `backup/600-research-pre-split-e4bb8833`/"the backup", not the
  pushed ref name, so name-greps miss it — verified this session:
  `P0_BASELINE_AND_OWNERSHIP.md` has zero occurrences of the literal
  `pr-601`, and its keep-alive/cleanup language covers only
  `ea2fc4fc`/`e4bb8833`). KEEP stands because a master-archived directive
  ("branch cleanup must not delete", deletion reserved to Owner/R9 closeout)
  cannot be silently overridden by an audit round; the special backup-branch
  check also fails (post-deletion recovery of the original commit objects is
  not guaranteed). Any future drop requires the Owner to first supersede the
  P0 designation on master or relocate the objects to another ref/tag.
- **Confirmation**: Three independent rounds ran the full check set; the
  recheck re-verified uniqueEvidence (object-set diff → 4 unique objects, 0
  unique blobs), activeDependency and recovery with fresh commands, and its
  factual findings are consistent with pass-2 — the disagreement is solely the
  normative call on the commit-object residual, already made on master by P0.
  All rounds read-only; the only ref deletion happened inside a disposable
  `/tmp` bare clone.
- **Evidence**: `backup-pr-601-original-head-e4bb8833-retention.md` (KEEP),
  `backup-pr-601-original-head-e4bb8833-recheck.md` (DELETE_CANDIDATE);
  `receipts/backup-pr-601-original-head-e4bb8833.txt` (pass 1 R1–R8),
  `-pass2.txt`, `-recheck.txt` (R1–R11).

---

## 3. `backup/pr-612-original-head-70d48f5e` (Sluice)

- **Branch**: Remote branch `backup/pr-612-original-head-70d48f5e` on origin;
  local tracking ref `refs/remotes/origin/backup/pr-612-original-head-70d48f5e`.
  (The ask's literal `origin/origin/…` spelling does not resolve — rev-parse
  exit 128; audited under the actual ref.)
- **HEAD**: `70d48f5eb55a664544b5160cd4675d32150bb21e` (matches the
  branch-name suffix; tip commit "docs(ledger): record #608 P2C1 composition
  evidence and THREAD-01 boundary", 2026-10-29 +0800). Chain over merge-base
  `bdfd0d8d`: `4c3e698daf13e68912ccc19e39c1ceae12b916b8` →
  `77e5399a45998dea396699fa2e063c94a665945a` (pre-squash #602-era base pair;
  `77e5399a` = PR #602 headSha) → `42f5eef999ed524de8eef1cf8de3dfb05a20c675`
  → `cc8ad2abd07bf48f03ed99bce0ee8bba126039d6` →
  `b12fb551db87c507d9def2e38065bbc0d826b79d` → `70d48f5e`.
- **Associated PR**: None by headRef match. The name references MERGED PR
  #612 (head `e4c9588ae8754ea44cf52194294103a7795aa1a0` =
  `refs/pull/612/head`): after #602 squashed, the PR branch was rebased from
  base `77e5399a` onto master, so the backup preserves the pre-rebase original
  head. `merge-base --is-ancestor 70d48f5e e4c9588` → exit 1 (NOT ancestor).
- **Commit Reachability**: Only this branch's own ref contains the tip
  (`git branch -a --contains`); no tag. 6 commits unique vs origin/master
  (full SHAs above). `4c3e698d`/`77e5399a` remain durable via
  `refs/pull/602/head`; `42f5eef9`/`cc8ad2ab`/`b12fb551`/`70d48f5e` are held
  upstream only by this backup ref.
- **Blob Reachability**: Range listing `rev-list --objects origin/master..branch`
  → 42 objects (6 commit + 24 tree + 12 blob, census re-verified). Strict
  master-unreachable set (comm of full object sets): 6 commits + 8 trees +
  **2 blobs** — both intermediate review versions of `src/blocking_file.cpp`
  (`6448f1a1`, `eb9b35be`; evolution `6660a423` base → `eb9b35be` @42f5eef9 →
  `6448f1a1` @cc8ad2ab → `8a27525d` final = master). An early round's
  "all 12 unique blobs" over-stated unreachability (10 of 12 are reachable
  from origin/master). Per-file blob compare at tip: 6/6 SAME —
  `docs/roadmap/v2-conformance.md` (`cfd8e7db`),
  `include/sluice/detail/file_semantics.hpp` (`d0090897`),
  `src/blocking_file.cpp` (`8a27525d`),
  `tests/direct_composition_fault_test.cpp` (`1945fc79`),
  `tests/semantic_range_test.cpp` (`0d3eb7e0`), `xmake/tests.lua`
  (`c55e97be`). `git diff --stat 70d48f5e` vs rebase tips `645082821a11ffcc1f835ab46088f54a5add4a64`
  and `e4c9588` on the 6 PR files: both empty.
- **Unique Evidence**: No unique research data — no docs/scripts/receipts that
  master lacks (the branch tip tree has no `research/` directory at all; two
  dot diff vs master: 790–824 D master-side + 8 M shared-history files, zero A
  entries). Chain identity and provenance are archived on master by MERGED
  #610: `research/direct-beyond-parity/EVIDENCE_MANIFEST.json:247,555`
  (`source_sha = 70d48f5e…`, sha256-manifested),
  `WSL2_VALIDATION_HANDOFF.md:11-29` (chain composition; dududu cherry-pick
  origins `d79244b0`/`31d18b6a`), `closure/perf-p2c1/SUMMARY.md:10-13,161`
  (candidate SHA + worktree recreation command), `closure/r4-p2c1.md:15,70-71`.
  The ref-unique residue is the 4 original P2C1 commit objects + 2
  intermediate blobs — superseded intermediate development history with no
  second upstream copy.
- **Active Dependency**: None. Not an OPEN PR head (only open PR is #594);
  no pull ref head equals the tip (grep count 0); no script/CI/xmake
  reference on master or in the working tree outside this audit directory;
  no worktree checks it out (15–16 worktrees, main on master, rest detached
  at `84e1b7d5`/`77e5399a`/`2714aa65`); dududu unrelated. Limitation: running
  agent sessions cannot be enumerated from this environment.
- **Working Tree**: Not applicable — not checked out by any worktree.
- **Recovery**: Before deletion: `git fetch origin
  backup/pr-612-original-head-70d48f5e` recovers the whole chain (ls-remote
  re-verified at the exact SHA). After deletion: `4c3e698d`/`77e5399a` remain
  via `refs/pull/602/head`; the 4 original P2C1 commits have NO guaranteed
  recovery channel (no PR ref contains them; bare-SHA fetch of unreachable
  objects is not guaranteed server-side) — objects survive only in local
  clones until gc. Local restore command while objects exist:
  `git push origin 70d48f5eb55a664544b5160cd4675d32150bb21e:refs/heads/backup/pr-612-original-head-70d48f5e`.
- **Decision**: DELETE_CANDIDATE (latest formal round
  `backup-pr-612-original-head-70d48f5e-retention.md` 23:48 + recheck 2026-10-30
  00:13, both with the six-condition table satisfied). An EARLIER round
  (evidence in `/tmp/sluice-val-pr612-audit/`, 22:56–23:07, no durable
  report) concluded ARCHIVE_FIRST, treating the ref-unique commit chain as
  unarchived data; the later formal rounds superseded it (strict unique set is
  only 2 blobs; provenance sha256-archived on master; content 6/6
  blob-identical). Residual: post-deletion loss of the 4 original commit
  objects is accepted on the same basis as the sibling audit — note,
  however, that the sibling `backup/pr-601` pass-2 reached the OPPOSITE
  verdict (KEEP) on the same residual because P0 designates that branch (and
  only that branch) as a keep-alive; P0 contains no designation for
  `70d48f5e`/pr-612 (verified this session: zero grep hits). Deletion is
  therefore an explicit Owner decision; see BRANCH_CLEANUP_PLAN.md item 1.
- **Confirmation**: Recheck re-ran the load-bearing fields fresh: object
  census 42 (6/24/12) matches the original exactly; per-file 6/6 SAME_BLOB;
  strict set-difference 6 commits + 8 trees + 2 blobs; fresh `gh pr list`
  (only #594), ls-remote pull-ref grep (0 matches), `branch -a --contains`
  (only the backup ref), 15 worktrees none on the branch, working-tree grep
  hits only this audit directory; `merge-base --is-ancestor 70d48f5e e4c9588`
  exit 1; both rebase-equivalence diffs empty. No fetch, no deletion, no
  build.
- **Evidence**: `backup-pr-612-original-head-70d48f5e-retention.md`;
  `receipts/backup-pr-612-original-head-70d48f5e.txt` (R1–R9, 77 KB),
  `receipts/backup-pr-612-original-head-70d48f5e-recheck.txt` (R1–R7 +
  verdict); early-round artifacts `/tmp/sluice-val-pr612-audit/`
  (unique-objects.txt, unique-ids.txt, branch-all.txt, master-all.txt,
  branch-named.txt).

---

## 4. `research/575-native-e5-atomic-rmw` (Sluice)

- **Branch**: `refs/remotes/origin/research/575-native-e5-atomic-rmw`; no
  local branch, no tag. (The ask's `origin/origin/…` spelling does not
  resolve; audited under the actual ref.)
- **HEAD**: `e9baafb9d117cbaf387dde9f4433ebbacb4a73b3` (matches expectation;
  the ask's doubled-prefix command failing is a ref-naming artifact, not a
  SHA mismatch).
- **Associated PR**: #584 — CLOSED (not merged), "research(575): native E5
  atomic RMW elimination evidence — NO_CHANGE_JUSTIFIED". Matched by headRef +
  headSha; `git ls-remote origin refs/pull/584/head` → same SHA.
- **Commit Reachability**: `git branch -a --contains` / `for-each-ref
  --contains` → only this branch's own ref; no tag contains it; master does
  not contain it. 9 unique commits vs origin/master (all `research(575)`
  evidence commits, 2026-10-24); merge-base `3df10dc4` (#583 v0.0.3 release
  merge, the campaign baseline in manifest.json).
- **Blob Reachability**: 196 branch-unique objects (156 under the evidence
  dir). `diff --name-status origin/master...branch` = 115 A + 1 M (second
  round: 116 files). Per-file blob compare: SAME=0, ABSENT_ON_MASTER=114,
  DIFFERENT=2 — both superseded: `tests/uring_flush_count_oracle_test.cpp`
  branch blob `28e77926` vs master `990cb80c` differs only in the 13-line
  leading comment (code identical); `xmake/tests.lua` +2-line registration
  exists on master at `xmake/tests.lua:578-579` via #585 (commit `1df8b4b8`,
  "test(uring): pin positive io_uring transport flush accounting"). No
  unmerged product change remains.
- **Unique Evidence**: UNIQUE AND UNARCHIVED — the entire
  `docs/research/mw-hw-575-native-e5/` evidence tree (114 files) exists only
  on this branch: 13 research docs (R0-IDENTITY … R9-REVIEWS, README,
  VERDICT.md recording THREADPOOL/URING/ISSUE_575 = NO_CHANGE_JUSTIFIED,
  PRODUCT_CHANGE_AUTHORIZED=NO, MERGED=NO), `manifest.json` (sha256
  inventory, 113/113 verified against branch tip), 6 assembly/GIMPLE dumps,
  9 patches (three-arm, mutants, plain-increment control arms), 81 raw
  measurement files (A/B, calibration, parity jsonl/csv from host E5 Xeon
  E5-2666 v3 / Fedora 44 / GCC 16.2.1 — non-regenerable), 4 scripts. Not on
  master (its `docs/research/` holds only mw-hw-1, native-linux-m2,
  state-complexity, uring-router-580), not in the #610 archive (different
  campaign), not in dududu. Negative-result value: causal-analysis
  deliverable (±0.7–3.8% swings proven code-shape artifacts via control
  arms), methodology findings, R1 semantics finding — direct input to any
  future counter-removal proposal.
- **Active Dependency**: None. #584 CLOSED; no OPEN PR head; zero grep hits
  for the branch name or "575" in master's `.github`/scripts/xmake; no
  worktree checks it out; dududu (`/tmp/dududu-audit`, main @ `12a8712`)
  zero hits for `575`/`e9baafb`.
- **Working Tree**: N/A — not checked out by any worktree; no local branch
  `research/575*` exists.
- **Recovery**: Today: remote branch AND `refs/pull/584/head` both at the
  exact SHA (both ls-remote-verified live) — two independent fetch routes.
  Post-deletion: `git fetch origin pull/584/head && git branch
  research/575-native-e5-atomic-rmw FETCH_HEAD` (CLOSED-unmerged PR head-ref
  persistence is GitHub platform behavior, not re-verified post-deletion;
  fetch-by-bare-SHA not verified — `uploadpack.allowAnySHA1InWant` unknown).
  PR #584 page records the headSha for traceability.
- **Decision**: ARCHIVE_FIRST (both rounds agree). The branch fails the
  "no unique unarchived data" precondition: 114 evidence files are reachable
  only from this branch, archived nowhere; per the campaign-agreed archive
  pattern (#610-style sha256-manifest archive) they should land on master
  before any branch deletion. All other DELETE_CANDIDATE preconditions hold.
  The manifest is archive-ready as-is (self-consistency 113/113 verified).
- **Confirmation**: Both rounds executed the full check set in-session
  (ref resolution, PR match, containment, counts, 196-object listing, 116-file
  per-file compare, master coverage, dependency/worktree, dududu greps,
  recovery ls-remote, manifest sha256 verify). One intermediate receipt was
  re-run after an awk bug produced a wrong inventory (final: 114 files = 14
  top-level + 6 assembly + 9 patches + 81 raw + 4 scripts). No destructive
  operations.
- **Evidence**: `branch-research-575-native-e5-atomic-rmw-retention-audit.md`;
  `receipts/branch-575-native-e5-atomic-rmw-01…08-*.txt` + `-03b-changed-file-list.txt`.

---

## 5. `research/593-key-path` (Sluice)

- **Branch**: `refs/remotes/origin/research/593-key-path` (the ask's
  `origin/origin/…` spelling does not resolve, rev-parse exit 128).
- **HEAD**: `2714aa65fe4837d30f5fa3fcdb27db38c23b7136` (matches expectation).
  2 unique commits over merge-base `bdfd0d8d`: `2714aa65` "anchor final block
  to PR #594" and `7bf55a87` "archive #593 E5 critical-path campaign".
- **Associated PR**: #594 — OPEN, headRef + headSha double match;
  independently re-verified `gh pr view 594` → state=OPEN, mergedAt=null.
  The only OPEN PR in Sluice.
- **Commit Reachability**: `git branch -a --contains 2714aa65` → only this
  branch; master does not contain it; `git tag --contains` empty;
  `rev-list --count origin/master..branch` = 2. Textual anchor: master's
  `research/direct-beyond-parity/closure/E5_CLOSURE_SNAPSHOT.md:150,167`
  references the branch by name + full SHA.
- **Blob Reachability**: 56 unique files, purely additive
  (`diff --stat` = 56 files, +12245/−0; no src/include/apps/xmake changes).
  Per-file blob compare 56/56 NOT_IN_MASTER (56 unique blobs listed; comm
  count = 56; master ls-tree for both directories empty) — no content-
  equivalent copy exists on master or in the #610 archive.
- **Unique Evidence**: 56 files of unique, unarchived research data, sole
  in-repo copy: `docs/research/key-path-593/` (15 docs: execution-dag,
  critical-path atlas, mechanism ledger, K1–K4 lane reports, counterfactuals,
  correctness/measurement reviews, decision.md with K2=ELISION_CANDIDATE_FOUND
  and K4=ELISION_PERF_SUPPORTED at −28% to −37% CPU in gated D=4/16/64
  configs, evidence-index) + `bench/research/key-path-593/` (41 files: 14
  patches + SHA256SUMS, 24 harness scripts + SHA256SUMS, README). OPEN issue
  #595 explicitly depends on the K4-b patches, two counterfactual test
  sources and the reproduction harness. The raw 1.33 GiB / 6,870-file
  measurement evidence is NOT in git — it lives on the unreachable E5 host
  (`/home/jnhu/sluice-593-research/evidence`), with
  `E5_RAW_EVIDENCE_PRESERVATION_PLAN.md` recording `E5_RAW_ARCHIVE_PENDING`
  and forbidding closing/deleting PR #594 before raw-evidence archival. The
  branch is currently the irreplaceable in-repo anchor.
- **Active Dependency**: Yes, multiple: OPEN PR #594 head (bit-exact);
  master's archived E5_CLOSURE_SNAPSHOT.md name+SHA references; issue #595's
  dependency on branch-resident artifacts. No CI dependency (harness
  self-describes as not wired into CI). No local worktree checkout; the
  remote E5-host worktree (`/home/jnhu/sluice-593-research/worktrees/pr`,
  recorded clean in the closure snapshot) is on an unreachable host — state
  UNKNOWN from here, honestly not assumed.
- **Working Tree**: Not checked out locally; the detached validation worktree
  `/tmp/sluice-val-k594-retention` pinned at the tip is clean
  (`git status --porcelain` = 0); detached, so it holds no branch ref.
- **Recovery**: Three verified anchors: (1) remote branch
  `git fetch origin research/593-key-path`; (2) `refs/pull/594/head` = same
  SHA while PR #594 exists; (3) local object DB (pre-GC):
  `git branch research/593-key-path 2714aa65…`. SHA also recorded in
  E5_CLOSURE_SNAPSHOT.md:150.
- **Decision**: KEEP (both rounds agree). Not eligible for any cleanup while
  PR #594 is open, issue #595 depends on branch-resident artifacts, and the
  E5 raw evidence remains `E5_RAW_ARCHIVE_PENDING`.
- **Confirmation**: Both rounds non-destructive (only state change: a
  `git fetch origin --prune` with zero ref changes, disclosed). All checks
  run with `git -C`; UNKNOWN items (E5-host worktree live state; raw-evidence
  backup) stated as UNKNOWN, not guessed. `E5_RAW_EVIDENCE_PRESERVATION_PLAN.md`
  and the prior-round report read and cross-checked.
- **Evidence**: `branch-research-593-key-path-retention-audit.md` (r1),
  `branch-research-593-key-path-retention-audit-r2.md` (r2),
  `key-path-593-retention-value.md`, `E5_RAW_EVIDENCE_PRESERVATION_PLAN.md`;
  `receipts/branch-593-key-path-01…07-*.txt`, `receipts/branch-593-key-path-r2-01…06-*.txt`.

---

## 6. `research/product-integration-records` (Sluice)

- **Branch**: `refs/remotes/origin/research/product-integration-records`
  (the ask's literal `origin/origin/…` spelling does not resolve; audited
  under the actual ref).
- **HEAD**: `4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35` (matches expectation).
  2 unique commits over merge-base `4b962abb` (= parent of master tip
  `84e1b7d5`): `8c3adf6c` "archive E5 product integration records and Phase
  3.3 final-head perf receipts" (38 files, +5025) and `4d621d91` "record
  archive push SHA in Phase 8 log" (+3).
- **Associated PR**: None — deliberately pushed without opening a PR
  (INTEGRATION_LOG.md §8.4). No headRef/headSha match among all 376 PRs
  (grep count 0, cross-checked against `state-freeze-report.md:21`).
- **Commit Reachability**: `git branch -a --contains` → only this branch's
  own ref; `git tag --contains` empty; tip not an ancestor of origin/master;
  reverse count 1 (master ahead by the #612 merge commit).
- **Blob Reachability**: `diff --name-only origin/master...branch` → 38
  files, purely additive (+5028), zero product paths. Per-file blob compare
  against ALL 35,416 master-reachable objects: 38/38 UNIQUE_TO_BRANCH
  (byte-identical content nowhere on master). 43 unique objects = 2 commits
  + 7 trees + 34 blobs (5 `machid/` paths share 3 blobs between sibling
  paths — intra-branch reuse, not master reachability). Master's #610
  archive has only `perf-p2c1/`, not `perf-p2c1-finalhead/`.
- **Unique Evidence**: Three classes, none on master: (1)
  `research/direct-beyond-parity/closure/perf-p2c1-finalhead/` 36 files =
  Phase 3.3 final-HEAD ABBA raw performance receipts (PREREGISTERED_PLAN,
  ENV, SUMMARY with base=`43cb8029` / candidate=#612 re-anchored head
  `64508282`, aa-*/ab-* raw outputs, machid machine fingerprints, run/build
  scripts) — the raw evidence behind #612's performance conclusions
  (eofmix&cursor WIN_CONFIRMED, comp-read-4k INCONCLUSIVE, negative control
  PARITY); (2) `INTEGRATION_PLAN.md` (pre-integration plan: squash/merge
  conventions, Head SHA Guard, merge order); (3) `INTEGRATION_LOG.md`
  (Phase 0–8 closeout log; §8.4 records these receipts were previously
  uncommitted). The workflow-drafts copies mentioned in §8.4 live on the
  unreachable E5 host — from this machine, this branch is the only
  verifiable persistent copy.
- **Active Dependency**: None. Not an OPEN PR head; master-tree grep zero
  hits; working-tree hits only in this audit directory; no worktree checkout
  (main on master, /tmp worktrees detached); no process reference.
- **Working Tree**: Not applicable — not checked out by any worktree.
- **Recovery**: Remote ref exists (ls-remote verified; exactly 1 remote ref
  holds the SHA). Deleting only the local tracking ref: `git fetch origin`
  restores it. If the remote branch is also deleted: no PR ref preserves the
  objects (no PR exists by design); recovery only via explicit-SHA fetch
  before server GC (requires push permission, not guaranteed) or from local
  clones holding the objects. Hence archive-first is mandatory before any
  deletion.
- **Decision**: ARCHIVE_FIRST (both rounds agree; second pass re-ran all key
  commands and appended receipts/pir-05-second-pass-verification.txt). The
  branch fails "no unique unarchived data" (38/38 UNIQUE_TO_BRANCH) and
  "raw data has a reliable backup" (its only persistent copy is the branch
  itself). Recommended: fold the 38 files into the master research archive
  using the #610 sha256-manifest pattern, then re-evaluate for deletion.
  Even though #612 is merged, its Final-HEAD performance receipts never
  reached master — this alone blocks DELETE_CANDIDATE.
- **Confirmation**: All checks executed in-session; the second pass
  re-verified every key command and persisted the rerun output. No deletions,
  merges, force-pushes, or builds.
- **Evidence**: `research-product-integration-records-retention-value.md`;
  `receipts/pir-01-head-and-refs.txt` … `pir-04-remote-and-dependency.txt`,
  `receipts/pir-05-second-pass-verification.txt`.

---

## 7. `research/r6-ac1-vectored-semantics` (Sluice)

- **Branch**: `refs/remotes/origin/research/r6-ac1-vectored-semantics` (ask
  label `sluice/…` = repo prefix; `origin/origin/…` spelling does not
  resolve).
- **HEAD**: `999b5305ae7d413a2a71acde926f8c6cfef6876a` (re-verified: local
  tracking ref, GitHub `refs/heads/…` and `refs/pull/611/head` all at this
  exact SHA). Single-parent tip over parent/merge-base
  `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` (ancestor of origin/master,
  exit 0). 1 unique commit; `bdfd0d8d..origin/master` = 5 commits.
- **Associated PR**: #611 — CLOSED (unmerged, mergeCommit null), base
  master, "research… (#605)" where #605 is an OPEN ISSUE, not a PR (gh
  issue view 605). Re-verified via `gh pr view 611`; no OPEN PR carries this
  headRef (only open PR is #594).
- **Commit Reachability**: `branch -a --contains 999b5305` → only the
  branch's own ref; `tag --contains` empty; not shallow, no grafts, no
  replace refs. Post-deletion, no master history is lost (only tip commit +
  4 container trees leave master's reachable set).
- **Blob Reachability**: Authoritative object-level check: branch 34,680
  objects vs master 35,416 → exactly 5 unique objects = 1 commit + 4 trees,
  ZERO blobs. Full-tree per-path compare over all 3,060 branch files: 3,052
  blob-identical at identical paths on master tip, 0 missing-on-master, 8
  DIFF paths whose branch-side blobs are each proven reachable from master's
  history (master-side forward evolution: the 8 M entries; tip-vs-tip diff =
  0 A + 8 M + 790 D). Three-dot diff = 34 files, all 34 blob-identical on
  master tip. The earlier "rev-list lists r6-ac1 blobs as branch-only" was
  arbitrated as rev-list's known over-approximation via full (non-sampled)
  membership testing of all 34 listed objects against master's reachable set.
- **Unique Evidence**: None unarchived. All branch content landed on master
  via PR #610 squash `ac995641`. `EVIDENCE_MANIFEST.json` sha256 cross-check:
  34/34 matched, 0 mismatched, on both branch and master blobs. Master
  provenance: `research/direct-beyond-parity/closure/r9-archive-increment.md:94-96`
  (branch name + commit + PR; notes the intentional overlap with the sibling
  research branch on the `closure/r6-ac1*` subtree; line 96's "OPEN"
  description of #611 is now slightly stale — #611 is CLOSED unmerged — but
  the branch+commit+PR traceability is accurate).
- **Active Dependency**: None. Only 1 prose provenance reference on master
  (which stays valid post-deletion since the SHA remains recoverable); no
  script/CI consumer (grep over xmake.lua/xmake/scripts/.github exit 1); no
  worktree checkout (master + 15 detached /tmp worktrees); dududu zero
  references (remote refs + main grep). Limitation: other live agent
  sessions cannot be enumerated from this sandbox.
- **Working Tree**: N/A — not checked out by any worktree.
- **Recovery**: Fully verified, end-to-end. `refs/heads/…` and
  `refs/pull/611/head` both at the exact SHA (ls-remote). Live proof: in a
  throwaway `/tmp` repo, `git fetch --depth=1 origin refs/pull/611/head`
  fetched `999b5305` exactly (cat-file -t = commit; ls-tree contains all 34
  r6-ac1 paths). Post-deletion restore:
  `git fetch origin refs/pull/611/head && git branch research/r6-ac1-vectored-semantics FETCH_HEAD`.
  Deeper history is covered by master's reachable set. GitHub's long-term
  retention of closed-PR pull refs is platform behavior — what was verified
  is that the ref exists now and fetches the exact SHA.
- **Decision**: DELETE_CANDIDATE (three rounds agree; r2 re-verification and
  r3 full arbitration both maintained it with progressively stronger
  evidence). Meets all six conditions + the special recovery check.
- **Confirmation**: r3 re-ran every load-bearing check with bash -x traced
  receipts: containment/counts/merge-base; 34-object rev-list arbitration
  (both `--not` and `^` forms, per-object classification, 0 unique blobs);
  3,060-file per-path compare; manifest sha256 34/34; dependency scans
  (gh/ls-remote/grep/worktree/dududu); end-to-end recovery fetch. No
  deletions, no fetch into the main repo, no worktree add/remove, no build.
- **Evidence**: `r6-ac1-vectored-semantics-retention-value.md`,
  `r6-ac1-vectored-semantics-reverification.md` (r2),
  `r6-ac1-vectored-semantics-retention-audit-r2.md`,
  `r6-ac1-vectored-semantics-retention-audit-r3.md`;
  `receipts/r6ac1-01…12-*.txt`, `receipts/r6ac1-rv-01…06-*.txt`,
  `receipts/r6ac2-01…07-*.txt`, `receipts/r6ac3-01…06-*.txt`.

---

## 8. `r1-request-geometry` (jnhu76/dududu)

- **Branch**: `r1-request-geometry` on dududu origin; local audit clone
  `/tmp/dududu-audit` (pre-existing, untouched checkout).
- **HEAD**: `47cd1976701a2c9e73fc31470f45aec236a3e0b2` (matches expectation;
  rev-parse and ls-remote agree).
- **Associated PR**: dududu PR #8 "docs(bench): record metadata micro-read
  study and opportunity gate" — **OPEN**, base `main`, headRef + headSha
  double match; `gh pr view 8` re-verified state=OPEN,
  headRefOid=`47cd1976…`.
- **Commit Reachability**: `branch -a --contains` → only
  `remotes/origin/r1-request-geometry`; repo has no tags; `refs/pull/8/head`
  = same SHA. 4 unique commits ahead of main (b2dced4 analyzer, 434416e
  geometry findings, 47c9a29 gate verdict, 47cd197 AC recommendations);
  merge-base `12a8712eb5ff5f48e570b49b1e01ad5821837dca` = current main HEAD —
  the branch strictly leads main.
- **Blob Reachability**: 27 unique objects vs main; `diff --name-status
  main...branch` = 8 files, all A, 996 lines, all under
  `benchmarks/duckdb-request-geometry/`. Per-file blob compare: 8/8 ABSENT
  on main, and the same blob SHAs are unreachable from main's object set
  (rules out rename/move) — content has not reached the default branch.
- **Unique Evidence**: Unique, unarchived research data: request-geometry
  findings (250 lines), opportunity-gate verdict NO_MATERIAL_OPPORTUNITY
  (257 lines), SLUICE_AC_RECOMMENDATIONS for AC-1/2/3 (127 lines),
  `analyze_pread_geometry.py` (282 lines), trace-index provenance (48
  lines), README, 2 .gitignore. `NO_MATERIAL_OPPORTUNITY` has zero hits on
  Sluice master; the #610 archive does not cover it. Mitigating fact: the 5
  raw io-trace inputs cited by trace-index are tracked by Sluice master
  under `research/direct-beyond-parity/closure/r5-native/duckdb/` with
  sha256 all matching — raw inputs are reproducible; but the
  conclusions/analyzer themselves are unarchived outside this branch/PR.
- **Active Dependency**: Yes — OPEN PR #8 head (GitHub holds
  `refs/pull/8/head` at the same SHA). Otherwise no consumers: no
  branch-name self-reference outside its own research docs; dududu's only
  CI (`repo-policy.yml`) is pull_request-triggered with no branch pin; zero
  Sluice references (outside the audit directory); no worktree checks it
  out.
- **Working Tree**: Not checked out by any worktree → no branch dirty state
  to observe; the audit clone itself is clean. Other-host checkouts
  (trace-index records an analysis host `/home/jnhu/...`) are not
  observable from this machine — recorded as UNKNOWN; does not affect KEEP
  (OPEN PR head independently locks the ref).
- **Recovery**: SHA still on the remote (`refs/heads/r1-request-geometry`
  and `refs/pull/8/head` both = `47cd1976…`). Even after a branch deletion,
  the PR ref preserves the tip while PR #8 exists: `git fetch origin
  refs/pull/8/head` then create a local branch. Full SHA + steps recorded
  in the report §1/§3 and receipts.
- **Decision**: KEEP (OPEN PR head).
- **Confirmation**: All required checks executed in-session with receipts;
  PR matched from the ask's list and independently verified via gh; per-file
  blob comparison run as asked. No product-code change, no merge/close, no
  deletions, no force-push, no re-clone (existing `/tmp/dududu-audit`
  reused, checkout untouched).
- **Evidence**: `r1-request-geometry-retention-value.md`;
  `receipts/branch-r1-request-geometry-01…07-*.txt` (incl. `-05b-raw-input-preservation.txt`).

---

## 9. `r5-async-host-probe` (jnhu76/dududu)

- **Branch**: `r5-async-host-probe` on dududu origin; audited in
  `/tmp/dududu-audit` (default branch `main`).
- **HEAD**: `4eafbc06d6a2e476055fe45fc75656e7e28a2183` (matches expectation;
  ls-remote, local tracking ref and cat-file all agree; tip commit "bench(duckdb):
  probe async host protocol boundary"). Branch segment: 4 commits
  (`bbce768`/`732a8fb`/`cc053cf`/`4eafbc0`) over `b94041d918672d113ea0ca667ba8ccdabf329c80`.
- **Associated PR**: None — no PR in any state carries this headRef (gh
  `--state all` and `--head r5-async-host-probe` both empty; all 5 dududu
  PRs enumerated: #1/#2/#3/#7 MERGED, #8 OPEN on a different branch). The
  tip IS an ancestor of `origin/r1-request-geometry` (OPEN PR #8 head;
  is-ancestor exit 0), so deleting this branch's ref does not affect #8.
- **Commit Reachability**: After fresh `fetch --prune`:
  `rev-list --count origin/main..branch` = 0; merge-base = the tip itself;
  `merge-base --is-ancestor` vs main exit 0. `branch -a --contains 4eafbc06`
  → main, origin/HEAD, origin/main, origin/r1-request-geometry,
  origin/sluice-r0-e5-verdict — three independent remote refs contain the
  tip, so deleting this branch's ref loses nothing.
- **Blob Reachability**: `rev-list --objects branch ^main` → 0 rows (zero
  unique commits/trees/blobs); three-dot diff empty. Stronger per-file
  check: the 9 files introduced by the branch segment (b94041d..4eafbc0, all
  A, no M/D/R) were blob-SHA-compared between branch tip and main tip:
  9/9 MATCH (`benchmarks/duckdb-r0/{README.md d5a62995…, async-host-probe/README.md
  5d5b8520…, async_host_probe.cpp a6cdfc0f…, oracle.sql 34a8a953…,
  results/prep-record.md 19ffa28c…}`, `benchmarks/fio-r0/{README.md 4b6a2b7e…,
  r0-randrw-4k.fio 3d1ca048…, r0-read-4k.fio 4a3f2c07…, r0-seqread-1m.fio
  8e1526b9…}`).
- **Unique Evidence**: None unarchived. `origin/main` (`12a8712e`) is the
  authoritative backup; `docs/DUCKDB_FIO_R0_PREREG.md:225` on main anchors
  the campaign by name+SHA ("branch r5-async-host-probe @ 4eafbc0"); the 4
  branch-segment commits are directly retrievable from main's log. Sluice's
  #610 archive covers a different repo/campaign — no overlap needing this
  branch.
- **Active Dependency**: None. Four textual references on main, none a
  consumer: 3 documentation provenance lines (`benchmarks/duckdb-r0/async-host-probe/README.md:40`,
  `docs/DUCKDB_FIO_R0_PREREG.md:225` (SHA-anchored) and `:300`) + 1 runtime
  temp path in `async_host_probe.cpp:405` (`/tmp/sluice-r5-async-host-probe.bin`,
  unrelated to the branch's existence). `.github` zero hits; no worktree
  checkout; no local branch; status/stash clean; no other dududu clone on
  this machine (the only other `/tmp` hit is a plain-text prereg draft,
  verified non-git via `file`).
- **Working Tree**: Not applicable — not checked out by any worktree.
- **Recovery**: All preconditions verified live: (a) recorded SHA matches
  remote (`ls-remote` exact); (b) objects fetchable (`fetch origin --prune`
  exit 0; `cat-file -t` = commit); (c) tip contained by main +
  r1-request-geometry + sluice-r0-e5-verdict (three is-ancestor exit 0), so
  deleting the branch ref loses nothing. Restore (either):
  `git fetch origin && git branch r5-async-host-probe 4eafbc06d6a2e476055fe45fc75656e7e28a2183`,
  or `git push origin 4eafbc06…:refs/heads/r5-async-host-probe`. (The
  restore command itself was NOT executed — running it would create a ref,
  violating the round's non-destructive discipline; its two prerequisites
  were directly verified.)
- **Decision**: DELETE_CANDIDATE (original audit + re-validation round agree;
  re-validation strengthened blob-uniqueness to bit-exact SHA equality).
- **Confirmation**: All three load-bearing fields re-verified with fresh
  commands after a fresh fetch; no check relaxed; no UNKNOWN items. Only
  audit-directory writes; the recovery command recorded, not executed.
- **Evidence**: `branch-dududu-r5-async-host-probe-retention-audit.md` (§10
  re-validation appended);
  `receipts/dududu-r5-host-probe-05-reval-head-commit-reachability.txt`,
  `-06-reval-blob-uniqueness.txt`, `-07-reval-active-dependency.txt`,
  `-08-reval-recovery.txt` (original receipts 01–04 unchanged).

---

## 10. `sluice-r0-closure` (jnhu76/dududu)

- **Branch**: `sluice-r0-closure` on dududu origin; audited in
  `/tmp/dududu-audit`.
- **HEAD**: `cc053cf9258b98546b16f9d2f08c5559fa73bc77` (matches expectation;
  ls-remote agrees with the local tracking ref after `fetch --all --prune`
  exit 0).
- **Associated PR**: None — `refs/pull/*/head` = only {1,2,3,7,8}, none
  pointing at `cc053cf9…` (grep count 0); gh `--state all` confirms no
  headRefName = sluice-r0-closure (5 PRs; only OPEN is #8
  r1-request-geometry). #7 MERGED @ `cc321a49` is the sibling verdict
  branch, not this one.
- **Commit Reachability**: `rev-list --count origin/main..branch` = 0 (zero
  unique commits); merge-base = the tip itself; is-ancestor of main exit 0.
  `branch -r --contains cc053cf9` → main, r1-request-geometry,
  r5-async-host-probe, sluice-r0-e5-verdict + itself. The tip entered main
  history via the true merge commit `12a8712e` (parents `b94041d9` +
  `cc321a49`, GitHub committer, GPG-signed).
- **Blob Reachability**: Zero unique objects — stronger than asked:
  `rev-list --objects origin/sluice-r0-closure --not --all` is EMPTY (unique
  against all refs, not just main); three-dot diff empty. The branch
  introduced exactly 7 files (b94041d..tip, all A:
  `benchmarks/duckdb-r0/{README.md, oracle.sql, results/prep-record.md}`,
  `benchmarks/fio-r0/{README.md, r0-randrw-4k.fio, r0-read-4k.fio,
  r0-seqread-1m.fio}`); per-file compare 7/7 SAME blob SHA, and ls-tree
  confirms all 7 paths exist on the main tree.
- **Unique Evidence**: None — the "covered by main/archive" question is an
  empty set for this branch; the long-term authoritative location of its
  content is dududu `origin/main`. The Sluice-side closure/PREREG documents'
  name+SHA provenance references remain resolvable after deletion (SHA is in
  main history — proven by a main-only clone test, below).
- **Active Dependency**: None (r2 re-verified). PR-ref channel absent (none
  needed); both repos' greps hit only 4+3 .md provenance lines (same
  files/lines as the original audit), `.github`/xmake zero; no worktree
  checkout across 16 Sluice + 1 dududu worktrees; no process reference.
- **Working Tree**: Not applicable — dududu clone is on main with no local
  `sluice-r0-closure`; no checkout instance exists anywhere observable.
- **Recovery**: Verified by an actual deletion-state simulation: a fresh
  `git clone --single-branch --branch main` of the repo into
  `/tmp/sluice-val-dududu-recovery-clone` (main-only) contains
  `cc053cf9…` (cat-file -t = commit; log metadata correct; is-ancestor of
  main holds; spot-checked blob SHAs match the per-file compare) — after
  deleting the branch ref, any `git fetch origin` fully restores objects and
  content; recovery does not depend on the deleted ref. Restore command:
  `git fetch origin && git push origin cc053cf9258b98546b16f9d2f08c5559fa73bc77:refs/heads/sluice-r0-closure`
  (or local `git branch` then push). Bare-SHA fetch of unreachable objects
  has no server-side guarantee — but is not needed here.
- **Decision**: DELETE_CANDIDATE (original audit + r2 recheck agree; r2
  strengthened uniqueEvidence to "--not --all" scope and performed the
  post-deletion recovery simulation).
- **Confirmation**: All three delegated fields re-verified by command in the
  r2 round; receipts retain exact commands and outputs. Only non-destructive
  operations (fetch, read-only checks, 7 rr receipts + 1 r2 report).
- **Evidence**: `branch-dududu-sluice-r0-closure-retention-audit.md`,
  `branch-dududu-sluice-r0-closure-retention-audit-r2.md`;
  `receipts/r0closure-01…08-*.txt`, `receipts/r0closure-rr-01…07-*.txt`.

---

## 11. `sluice-r0-e5-verdict` (jnhu76/dududu)

- **Branch**: `sluice-r0-e5-verdict` on dududu origin; audited in
  `/tmp/dududu-audit`.
- **HEAD**: `cc321a4994ee638493fecda0c437481528f5cea9` (matches expectation).
  6 branch-segment commits (`bbce768`, `732a8fb`, `cc053cf`, `4eafbc0`,
  `a57fb09`, `cc321a4`) over pre-merge main `b94041d9`.
- **Associated PR**: dududu PR #7 "docs(bench): record e5 r0 verdict and
  owner-gate prereg" — **MERGED** (gh re-verified: state=MERGED, base main,
  headRefOid bit-identical, merged 2026-10-29T04:47:38Z). Not an OPEN PR
  head (only open PR is #8, a different branch).
- **Commit Reachability**: `rev-list --count origin/main..branch` = 0;
  merge-base = the tip; is-ancestor exit 0. The true merge commit
  `12a8712e` (parents `b94041d9` + `cc321a49`) directly contains it;
  `branch -a --contains` → main, origin/HEAD, origin/main,
  r1-request-geometry + itself. No tag (ls-remote --tags empty).
- **Blob Reachability**: Zero unique objects: `rev-list --objects branch
  --not main` → 0 rows; tip tree SHA == main tree SHA
  (`731e518af903fe49beb798d80d269eeaceb53794`, bit-identical). 11 files
  introduced by the branch (6 commits; the original audit listed only the 2
  receipt docs, missing 9 bench scaffolding files): 11/11 IDENTICAL blob
  compare (incl. `docs/DUCKDB_FIO_R0_PREREG.md` = `2d625d61…`,
  `benchmarks/duckdb-r0/results/lane-record.md` = `34c17758…`); both receipt
  docs sha256-equal on both sides (`b617adf4…`, `74500d79…`).
- **Unique Evidence**: None — the branch's entire content is byte-preserved
  on dududu main. Cross-checks: Sluice's #610 copy
  `research/direct-beyond-parity/closure/DUCKDB_FIO_R0_PREREG.md` is a
  different host variant (295 lines vs dududu's 301; dududu's copy is kept
  by dududu main; the variant difference does not affect the verdict); the
  raw run outputs live in Sluice's `closure/r5-native/` (per lane-record
  rule 6, deliberately not in dududu git).
- **Active Dependency**: None (r2 re-verified). Both repos' greps exit 1; no
  worktree checkout; no process; gh confirms #7 MERGED with bit-identical
  headRefOid. Environment change noted honestly: a parallel verification
  clone `/tmp/sluice-val-dududu-recovery-clone` appeared after the original
  audit; read-only inspection shows it on main, clean, not holding this
  branch — not a dependency.
- **Working Tree**: N/A — not checked out anywhere (audit clone on main,
  status clean throughout).
- **Recovery**: Three independently verified layers: (1) content already on
  the default branch (is-ancestor exit 0 via merge `12a8712e`; tip tree ==
  main tree); (2) remote still holds the tip directly
  (`refs/heads/sluice-r0-e5-verdict` = `cc321a49…`); (3) even after remote
  branch deletion, GitHub keeps `refs/pull/7/head` = `cc321a49…` (ls-remote
  verified): `git fetch origin pull/7/head:refs/heads/sluice-r0-e5-verdict`.
- **Decision**: DELETE_CANDIDATE (original audit + r2 recheck agree; r2
  widened the file census 2 → 11, all verified on main — a strengthening,
  not a revision).
- **Confirmation**: All three required fields re-run per the ask; verdict
  maintained with stronger evidence; the census correction recorded without
  changing the decision. Fully read-only (no fetch; PR-ref liveness verified
  by ls-remote).
- **Evidence**: `branch-sluice-r0-e5-verdict-retention-audit.md`,
  `branch-sluice-r0-e5-verdict-retention-audit-r2.md`;
  `receipts/branch-sluice-r0-e5-verdict-01…06-*.txt`,
  `receipts/branch-sluice-r0-e5-verdict-r2-01…05-*.txt`.

---

## Cross-branch notes

1. **Round conflicts and how they were resolved.** Two branches have
   conflicting decisions across audit rounds; both are documented above
   rather than silently averaged:
   - `backup/pr-601-original-head-e4bb8833`: DELETE_CANDIDATE (pass-1 and
     the 23:56 recheck) vs KEEP (pass-2, 23:43). KEEP stands because master's
     archived `P0_BASELINE_AND_OWNERSHIP.md` explicitly protects the backup
     ("branch cleanup must not delete", permanent retention recommended) and
     the recheck's name-based grep could not see that designation (P0 refers
     to the branch as `backup/600-research-pre-split-e4bb8833`/"the backup";
     zero literal `pr-601` occurrences — verified this session). Only the
     Owner can supersede a master-archived directive.
   - `backup/pr-612-original-head-70d48f5e`: an early tmp-only round
     concluded ARCHIVE_FIRST; the two later durable rounds (formal report +
     recheck) concluded DELETE_CANDIDATE with a strict-set refinement (2, not
     12, master-unreachable blobs). DELETE_CANDIDATE stands as the latest
     formal verdict, with the early dissent and the commit-object residual
     recorded; deletion stays Owner-gated (see BRANCH_CLEANUP_PLAN.md).
     Note: the formal report's special-check paragraph cites the pr-601
     sibling as having "accepted" the same residual, but the sibling's
     corrective pass actually flipped to KEEP on that residual due to the P0
     designation — a cross-reference inconsistency that further supports
     keeping pr-612's deletion an explicit Owner decision rather than a
     mechanical one.
2. **rev-list caveat.** Multiple rounds independently established that
   listing-mode `git rev-list --objects A ^B` over-reports unique blobs
   (shared blobs still listed). Where it mattered (pr-601, pr-612, r6-ac1),
   authoritative unique-object counts were computed as full reachable-set
   differences.
3. **UNKNOWN items** across the campaign: E5-host worktree live states
   (`/home/jnhu/...` unreachable from this machine, affects #593/#594
   context and pr-612 §8.4 workflow-drafts copies), other-host dududu
   checkouts, other live agent sessions, GitHub's long-term retention
   promises for closed-PR pull refs and bare-SHA fetches. None was filled
   with a guess; all are recorded as UNKNOWN or as verified-platform-state-
   now.
