# Branch Cleanup Plan — retention-wsl2-audit

**Status: PLAN ONLY.** This phase authorizes and executes nothing. No branch,
worktree, PR or evidence directory was deleted, merged, closed or
force-pushed in any audit round, and none is deleted by this document. Every
item below becomes actionable only after the named Owner authorization
preconditions are met.

- Repositories: `jnhu76/Sluice` (`/home/hoo/Projects/Sluice`, origin
  `git@github.com:jnhu76/Sluice.git`, default branch `master`) and
  `jnhu76/dududu` (audit clone `/tmp/dududu-audit`, origin
  `https://github.com/jnhu76/dududu.git`, default branch `main`).
- Full-SHA recovery steps for every branch, including both backup branches'
  original commits, live in RECOVERY_MANIFEST.md and must be executed/kept
  current alongside any deletion.
- Per-branch evidence: BRANCH_RETENTION_MATRIX.md (sections referenced below
  as "Matrix §n").

## Eligibility rule

A branch enters this plan only when the audit record shows ALL of the
following (the condition set used by the per-branch retention reports):

1. Not the repository's default branch.
2. Not the head of an OPEN PR.
3. No active dependency: no script/CI/build consumer, no worktree checkout,
   no agent-process reference; for backup branches additionally, no
   master-archived directive designating the ref as a keep-alive.
4. Full HEAD SHA durably recorded, with recovery steps.
5. Recovery verified (a concrete, tested fetch/restore path exists).
6. No unique unarchived research data, and any raw data has a reliable
   backup elsewhere.

Plus the special backup-branch check: the original commits must remain
recoverable via immutable SHA, historical PR **and** object storage before
the ref may be dropped; if post-deletion recovery of the commit objects is
not guaranteed, the residual must be explicitly accepted by the Owner.

## Cleanup items (5)

### Item 1 — `backup/pr-612-original-head-70d48f5e` (Sluice) — Owner-gated

Matrix §3. HEAD `70d48f5eb55a664544b5160cd4675d32150bb21e`.

Conditions met (receipts `backup-pr-612-original-head-70d48f5e.txt` R1–R9,
`-recheck.txt` R1–R7): not an OPEN PR head; no pull ref equals the tip; no
script/CI/worktree consumer; SHA + chain provenance archived on master
(`EVIDENCE_MANIFEST.json:247,555`, `WSL2_VALIDATION_HANDOFF.md:11-29`,
`closure/perf-p2c1/SUMMARY.md:10-13,161`, `closure/r4-p2c1.md:15,70-71`);
tip content 6/6 blob-identical to master; strict master-unreachable set is
only 6 commits + 8 trees + 2 intermediate `src/blocking_file.cpp` blobs.

**Owner authorization preconditions (all required):**

1. Explicit Owner authorization to delete this remote branch.
2. Owner acknowledgment of the residual: after deletion the 4 original P2C1
   commits (`42f5eef9`, `cc8ad2ab`, `b12fb551`, `70d48f5e`) have NO
   guaranteed recovery channel (no PR ref contains them; bare-SHA fetch of
   unreachable objects is not guaranteed server-side); they survive only in
   local clones until gc. File content is not at risk regardless.
3. Owner adjudication of the recorded round conflict: an earlier tmp-only
   round judged ARCHIVE_FIRST; the two later durable rounds judged
   DELETE_CANDIDATE. If the Owner prefers zero residual, the alternative is
   a one-command archive first (e.g. `git bundle create
   sluice-pr612-original-head.bundle
   70d48f5eb55a664544b5160cd4675d32150bb21e` stored with the audit
   evidence, or a local tag), after which deletion is unconditionally safe.
   Note the sibling inconsistency: `P0_BASELINE_AND_OWNERSHIP.md` protects
   only the pr-601 backup — no master-archived directive protects this ref
   (verified: zero `70d48f5e`/`pr-612` hits in P0).
4. Before deleting, record the full chain SHAs (see RECOVERY_MANIFEST.md §2)
   in the deletion commit message or ticket.

**Planned deletion (NOT executed):**
`git push origin --delete backup/pr-612-original-head-70d48f5e`

**Recovery (verified while the ref exists):** `git fetch origin
backup/pr-612-original-head-70d48f5e`. Post-deletion: base pair via
`refs/pull/602/head` (`77e5399a…`); the 4 P2C1 commits only from local
clones pre-gc — `git push origin
70d48f5eb55a664544b5160cd4675d32150bb21e:refs/heads/backup/pr-612-original-head-70d48f5e`.

### Item 2 — `research/r6-ac1-vectored-semantics` (Sluice)

Matrix §7. HEAD `999b5305ae7d413a2a71acde926f8c6cfef6876a`.

Conditions met (receipts `r6ac1-rv-01…06`, `r6ac3-01…06`): CLOSED-unmerged
PR #611 head; zero unique blobs (only tip commit + 4 container trees leave
master's reachable set); all 34 branch-introduced files blob-identical on
master tip and 34/34 sha256-matched against master's `EVIDENCE_MANIFEST.json`;
no script/CI/worktree/agent consumer (1 prose provenance line on master,
`r9-archive-increment.md:96`, remains valid post-deletion); recovery proven
end-to-end by a real `git fetch --depth=1 origin refs/pull/611/head` in a
throwaway repo.

**Owner authorization preconditions:**

1. Explicit Owner authorization to delete this remote branch.
2. Acceptance that post-deletion recovery relies on GitHub's `refs/pull/611/head`
   (verified live today; long-term retention of closed-PR pull refs is
   platform behavior, not contractually guaranteed) plus master's reachable
   history — both recorded here as the recovery channels.

**Planned deletion (NOT executed):**
`git push origin --delete research/r6-ac1-vectored-semantics`

**Recovery (verified):** `git fetch origin refs/pull/611/head && git branch
research/r6-ac1-vectored-semantics FETCH_HEAD` (FETCH_HEAD =
`999b5305ae7d413a2a71acde926f8c6cfef6876a`).

### Item 3 — `r5-async-host-probe` (dududu)

Matrix §9. HEAD `4eafbc06d6a2e476055fe45fc75656e7e28a2183`.

Conditions met (receipts `dududu-r5-host-probe-05…08`): not any PR head in
any state (and the tip is an ancestor of OPEN PR #8's head, so #8 is
unaffected); zero unique objects (`rev-list --objects branch ^main` = 0
rows; 9 branch-introduced files 9/9 blob-SHA-identical on main tip); no
consumer (3 doc provenance lines + 1 runtime temp path, none a ref
consumer); SHA + campaign anchored on main at `docs/DUCKDB_FIO_R0_PREREG.md:225`.

**Owner authorization preconditions:**

1. Explicit Owner (dududu) authorization to delete this remote branch.

**Planned deletion (NOT executed):**
`git push origin --delete r5-async-host-probe`

**Recovery (verified):** tip contained by `origin/main`,
`origin/r1-request-geometry`, `origin/sluice-r0-e5-verdict` (three
is-ancestor exit 0), so any fetch restores it: `git fetch origin && git
branch r5-async-host-probe 4eafbc06d6a2e476055fe45fc75656e7e28a2183`.

### Item 4 — `sluice-r0-closure` (dududu)

Matrix §10. HEAD `cc053cf9258b98546b16f9d2f08c5559fa73bc77`.

Conditions met (receipts `r0closure-01…08`, `r0closure-rr-01…07`): no PR
association at all; zero unique objects even against ALL refs
(`rev-list --objects branch --not --all` empty); 7 branch-introduced files
7/7 blob-identical on main; no consumer; deletion-state recovery simulated
for real in a main-only clone (`/tmp/sluice-val-dududu-recovery-clone`) —
tip fully recoverable from any plain clone of main.

**Owner authorization preconditions:**

1. Explicit Owner (dududu) authorization to delete this remote branch.

**Planned deletion (NOT executed):**
`git push origin --delete sluice-r0-closure`

**Recovery (verified by simulation):** tip is in main history via merge
`12a8712eb5ff5f48e570b49b1e01ad5821837dca`; restore with
`git fetch origin && git push origin
cc053cf9258b98546b16f9d2f08c5559fa73bc77:refs/heads/sluice-r0-closure`.

### Item 5 — `sluice-r0-e5-verdict` (dududu)

Matrix §11. HEAD `cc321a4994ee638493fecda0c437481528f5cea9`.

Conditions met (receipts `branch-sluice-r0-e5-verdict-01…06`, `-r2-01…05`):
PR #7 MERGED (not OPEN); zero unique objects (tip tree == main tree,
`731e518a…`); 11/11 introduced files identical on main; no consumer; three
independent recovery layers verified.

**Owner authorization preconditions:**

1. Explicit Owner (dududu) authorization to delete this remote branch.

**Planned deletion (NOT executed):**
`git push origin --delete sluice-r0-e5-verdict`

**Recovery (verified):** (a) content already on main via merge `12a8712e`;
(b) `refs/pull/7/head` persists: `git fetch origin
pull/7/head:refs/heads/sluice-r0-e5-verdict`; (c) remote branch itself
until deleted.

## Excluded branches (6) — not in this plan

| Branch | Repo | Decision | Blocking reason |
|---|---|---|---|
| `master` | Sluice | KEEP | Default branch; CI triggers (`ci.yml:6,18`), issue templates and `CONTRIBUTING.md:38` depend on it. |
| `backup/pr-601-original-head-e4bb8833` | Sluice | KEEP | Master-archived keep-alive directive (`P0_BASELINE_AND_OWNERSHIP.md` §2.3.1/§2.3.7/§2.4.1: "branch cleanup must not delete", permanent retention; sole holder of `CANDIDATE_SHA ea2fc4fc` cited by `PHASE3_VALIDATION.md:4`); post-deletion recovery of the 2 original commit objects is not guaranteed (verified: no remaining remote ref retains them). Deletion requires the Owner to first supersede the P0 designation on master or relocate the objects to a tag/bundle. |
| `research/593-key-path` | Sluice | KEEP | OPEN PR #594 head; issue #595 depends on branch-resident K4-b patches/tests/harness; E5 raw evidence `E5_RAW_ARCHIVE_PENDING` forbids closing/deleting PR #594 first. |
| `r1-request-geometry` | dududu | KEEP | OPEN PR #8 head; unique unarchived research data (8 files/996 lines). |
| `research/575-native-e5-atomic-rmw` | Sluice | ARCHIVE_FIRST | 114 unique evidence files (verdict + non-regenerable raw A/B/calibration/parity data + assembly/patches/scripts + sha256 manifest) exist only on this branch; archive to master (#610-style) before any deletion. |
| `research/product-integration-records` | Sluice | ARCHIVE_FIRST | 38 unique files (perf-p2c1-finalhead raw ABBA receipts behind #612's conclusions + INTEGRATION_PLAN/LOG) exist only on this branch, deliberately pushed without a PR (no pull-ref fallback would survive deletion); archive to master first. |

Note on `backup/pr-612-original-head-70d48f5e`: it is listed as Item 1
despite the early-round ARCHIVE_FIRST dissent because the two later durable
rounds (formal report + recheck) re-derived DELETE_CANDIDATE with a strictly
smaller unique set and sha256-archived provenance on master; the dissent,
the residual and the required adjudication are all explicit in Item 1's
preconditions. If the Owner prefers the conservative route, apply the
bundle/tag archive of Item 1 precondition 3 and the branch becomes as safe
to delete as Items 2–5.

## Execution discipline (for the future execution phase)

- Execute only after explicit, per-item Owner authorization; never batch
  items beyond what was authorized.
- Re-run the recovery preflight from RECOVERY_MANIFEST.md immediately before
  each deletion (ls-remote exact-SHA check + containment check), and the
  post-deletion recovery verification immediately after.
- Deletion order within Sluice, if both backup items were ever authorized:
  never delete `backup/pr-601-original-head-e4bb8833` under this plan (KEEP
  — excluded); it appears here only to document that boundary.
- No `gc`/`prune` on local clones until post-deletion recovery has been
  verified, since local clones are the only remaining holder of the pr-612
  original commits if Item 1 proceeds as planned.
- Update this file and RECOVERY_MANIFEST.md with execution date, executing
  operator and verification output for every item performed.
