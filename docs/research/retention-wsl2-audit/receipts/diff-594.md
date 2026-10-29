# Receipt: PR #594 head vs origin/master — exhaustive diff audit

- Date: 2026-10-29
- Auditor: 594 差异分析员 (workflow subagent)
- PR head: `2714aa65fe4837d30f5fa3fcdb27db38c23b7136` (branch `research/593-key-path`)
- Compared against: `origin/master`
- Discipline: audit only; no merge, no branch/worktree/evidence deletion, no product-code change; writes limited to this receipts directory and `/tmp/sluice-val-594-kp` (extract-only copy of the PR evidence trees).

## 0. Branch and head verification

Local branch `research/593-key-path` was absent; fetched per instruction.

```
$ git rev-parse --verify research/593-key-path
fatal: Needed a single revision
$ git fetch origin research/593-key-path
From github.com:jnhu76/Sluice
 * branch              research/593-key-path -> FETCH_HEAD
$ git rev-parse FETCH_HEAD
2714aa65fe4837d30f5fa3fcdb27db38c23b7136
$ git for-each-ref refs/remotes/origin/research/593-key-path --format='%(refname) %(objectname)'
refs/remotes/origin/research/593-key-path 2714aa65fe4837d30f5fa3fcdb27db38c23b7136
```

Fetched tip == PR head == `2714aa65fe4837d30f5fa3fcdb27db38c23b7136`. `origin/master` = `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`.

## 1. Commit ancestry and merge-base

```
$ git merge-base origin/master 2714aa65fe4837d30f5fa3fcdb27db38c23b7136
bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57
$ git rev-list --count origin/master..2714aa65fe4837d30f5fa3fcdb27db38c23b7136   # 594-ahead
2
$ git rev-list --count 2714aa65fe4837d30f5fa3fcdb27db38c23b7136..origin/master   # 594-behind
5
$ git merge-base --is-ancestor 2714aa65fe4837d30f5fa3fcdb27db38c23b7136 origin/master; echo exit=$?
exit=1        # 594 head NOT merged into master
$ git merge-base --is-ancestor origin/master 2714aa65fe4837d30f5fa3fcdb27db38c23b7136; echo exit=$?
exit=1        # master NOT an ancestor of 594 head (true divergence)
```

Unique commits on each side:

```
$ git log --format='%H %ad %s' --date=iso origin/master..2714aa65fe4837d30f5fa3fcdb27db38c23b7136
2714aa65fe4837d30f5fa3fcdb27db38c23b7136 2026-10-26 05:08:06 +0800 research(593): anchor final block to PR #594
7bf55a875896377b6c1250b206b427cac1c11f7e 2026-10-26 04:20:32 +0800 research(key-path): archive #593 E5 critical-path campaign

$ git log --format='%H %ad %s' --date=iso 2714aa65fe4837d30f5fa3fcdb27db38c23b7136..origin/master
84e1b7d578c2c58a3c46033487d9e1ac7c639b8b 2026-10-29 19:41:19 +0800 perf(direct): P2C1 read compositions run prevalidated register-resident steps (#608) (#612)
4b962abbd26156bb15c4378b90811e9889795c03 2026-10-29 19:38:10 +0800 ci(guard): keep comment-authority scan off archived research evidence (#613)
43cb8029ca60b572c91a9aa9dccb17d4300e2f88 2026-10-29 14:08:36 +0800 perf(direct): dispatch admission verdicts without Precheck materialization (#600 C1) (#602)
ac995641e7394bfd82f7edb1caafca6ebea21c13 2026-10-29 13:30:53 +0800 research(direct-beyond-parity): archive #603 P0-P6 evidence with sha256 manifest (#610)
84a0cc1dd958997c16e84fed233b74600a608964 2026-10-29 12:54:26 +0800 research(direct): #600 E5 excess-cost attribution and C1 validation evidence (#601)
```

Merge-base `bdfd0d8d` is itself a research-archive commit ("research(uring): archive #580 evidence — NO_TRIGGER_NO_CHANGE (#592)", 2026-10-25). The two PR commits are linear on the base (parent chain `2714aa65 -> 7bf55a87 -> bdfd0d8d`).

## 2. Per-file comparison

### 2a. Three-dot diff (merge-base -> 594 head): what the PR itself changes

```
$ git diff --name-status origin/master...2714aa65fe4837d30f5fa3fcdb27db38c23b7136
```

56 entries, **all `A` (added), 0 M, 0 D**. Grouped:

| Directory | Files |
|---|---:|
| `bench/research/key-path-593/` (README.md) | 1 |
| `bench/research/key-path-593/harness/` (24 scripts + SHA256SUMS) | 25 |
| `bench/research/key-path-593/patches/` (14 patch/diff/rej + SHA256SUMS) | 15 |
| `docs/research/key-path-593/` (15 markdown records) | 15 |

The PR touches **nothing** outside those two subtrees: no `include/`, `src/`, `apps/`, `tests/`, `xmake`, `scripts/`, `.github/`, no ledger/roadmap edits.

### 2b. Two-dot diff (origin/master vs 594 head): where the tips currently differ

```
$ git diff --name-status origin/master..2714aa65fe4837d30f5fa3fcdb27db38c23b7136
```

- 56 `A` — exactly the same 56 paths as the three-dot list (verified `IDENTICAL-56` via `comm`/`diff` of the two path sets).
- 824 `D` — files master has and 594 lacks: 819 under `research/direct-beyond-parity/` (#610 archive, added by master commit ac995641) + 5 under `research/600-direct-excess-cost/` (#601). These are master-side additions after divergence, **not** PR 594 content.
- 8 `M` — `docs/roadmap/v2-conformance.md`, `include/sluice/detail/file_semantics.hpp`, `scripts/check_cpp_comment_authority.py`, `scripts/test_check_cpp_comment_authority.py`, `src/blocking_file.cpp`, `tests/direct_composition_fault_test.cpp`, `tests/semantic_range_test.cpp`, `xmake/tests.lua`.

Provenance of the 8 `M` files — proven to be **master-only** changes (PR 594 side identical to merge-base):

```
for p in <the 8 paths>:
  a=$(git rev-parse 2714aa65fe4837d30f5fa3fcdb27db38c23b7136:"$p")
  b=$(git rev-parse bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57:"$p")
-> SAME-AS-BASE for all 8
```

So the PR does not modify any pre-existing file; every product-looking difference vs master comes from master's own 5 newer commits.

### 2c. Blob-SHA comparison per file

All 56 PR files were compared by blob SHA between the two trees:

```
$ git ls-tree -r 2714aa65fe4837d30f5fa3fcdb27db38c23b7136 > /tmp/ls-594.txt   (3082 entries)
$ git ls-tree -r origin/master                      > /tmp/ls-master.txt (3850 entries)
$ comm -12 <(594 56 paths) <(master all paths) | wc -l
0
$ git ls-tree -r --name-only origin/master | grep -iE 'key-path|593'
(no output — no master path contains "key-path" or "593")
```

- **0 of 56 paths exist on master at all.**
- **0 of the 56 file blobs exist anywhere in master's tree** (content-equality by git blob SHA, checked against all 3850 master paths):

```
$ awk (56 paths) ls-594 -> /tmp/blobs-of-56.txt   # 56 distinct SHAs
$ awk '$2=="blob"{print $3}' ls-master | sort > /tmp/blobs-master.txt  # 3850-entry tree
$ comm -12 /tmp/blobs-of-56.txt /tmp/blobs-master.txt | wc -l
0
```

- Whole-tree blob intersection (context): 3018 of 594's 3082 blobs are shared with master (the common codebase); the 64 non-shared blobs are exactly the 56 PR files + 8 master-side-changed files seen from the 594 snapshot. Classification-relevant conclusion: **no PR 594 content is already in master, at any path.**

## 3. Patch equivalence and unique objects

```
$ git rev-list --objects 2714aa65fe4837d30f5fa3fcdb27db38c23b7136 ^origin/master | wc -l
72
```

Breakdown by kind (via `git cat-file -t` over all 72): **2 commits, 13 trees, 57 blobs**.

- Unique commits: `7bf55a87` (adds all 56 files, "56 files changed, 12245 insertions(+)") and `2714aa65` (1 file, 1+/1−: `docs/research/key-path-593/decision.md`).
- The 57th blob (beyond the 56 final files) is `7673dfe8c223b49612828733a9bba3024850b8de` = the **intermediate** version of `docs/research/key-path-593/decision.md` from `7bf55a87`, superseded in `2714aa65`. The anchor diff:

```
$ git show 2714aa65 -- docs/research/key-path-593/decision.md   (changed lines)
-RESEARCH_PR: branch research/593-key-path at the frozen base (PR not opened in this run; push and PR creation are a subsequent step; no PR number exists). Once opened it must remain open and unmerged per brief §14.
+RESEARCH_PR: https://github.com/jnhu76/Sluice/pull/594 (branch research/593-key-path at the frozen base; opened and remains OPEN, unmerged per brief §14).
```

Note: the archived research record itself states the PR "must remain open and unmerged per brief §14" — relevant to any later close/merge decision; recorded here as file content, not as this audit's instruction.

Patch-id equivalence:

```
$ git cherry origin/master 2714aa65fe4837d30f5fa3fcdb27db38c23b7136
+ 7bf55a875896377b6c1250b206b427cac1c11f7e
+ 2714aa65fe4837d30f5fa3fcdb27db38c23b7136
```

Both `+` ⇒ **no commit on master is patch-equivalent to either PR commit.**

### #610 archive overlap (path and hash)

- The #610 archive lives at **`research/direct-beyond-parity/` (top-level `research/`)** on master — master's `docs/research/` contains only `mw-hw-1`, `native-linux-m2`, `state-complexity`, `uring-router-580` and has **no** `direct-beyond-parity` and no key-path content. (The ask's path `docs/research/direct-beyond-parity` does not exist on master; the actual location is as stated above.)
- Manifests on master checked: `research/direct-beyond-parity/EVIDENCE_MANIFEST.json` (341,513 bytes, sha256-per-path JSON) plus every sha256/manifest-like file in the whole master tree (`docs/evidence/**`, `research/**`, `docs/research/**`), yielding 1,369 distinct 64-hex hashes:

```
$ git ls-tree -r --name-only origin/master | grep -iE '(sha256|SHA256SUMS|EVIDENCE_MANIFEST|manifest)' | while read p; do git show "origin/master:$p" | grep -oE '\b[0-9a-fA-F]{64}\b'; done | tr A-F a-f | sort -u > /tmp/master-all-hashes.txt   # 1369
$ sha256sum over all 56 extracted PR files -> /tmp/594-content-sha256.txt (56)
$ comm -12 <(sort -u 594-hashes) master-all-hashes | wc -l
0
$ git show origin/master:research/direct-beyond-parity/EVIDENCE_MANIFEST.json | grep -c 'key-path'
0
```

**Triple-negative result: 0/56 by path, 0/56 by git blob SHA, 0/56 by content sha256 against every manifest hash on master. None of PR 594's content was archived by #610 (or by anything else on master).** #610 archived the #603 direct-beyond-parity campaign; PR 594 archives the #593 key-path campaign — disjoint, verified by hash rather than assumed.

### Internal integrity of the PR archive (context check)

```
$ git archive 2714aa65 bench/research/key-path-593 docs/research/key-path-593 | tar -x -C /tmp/sluice-val-594-kp
$ (cd .../harness && sha256sum -c SHA256SUMS)   -> 24/24 OK, exit 0
$ (cd .../patches && sha256sum -c SHA256SUMS)   -> 14/14 OK, exit 0
```

The PR's own SHA256SUMS pins 38 of the 56 files (24 harness + 14 patches) and verifies clean; the 15 docs + bench README + the 2 SHA256SUMS files themselves are outside their own sums.

## 4. docs / harness / script distinction; uniqueness; product code

- **docs** (15, `docs/research/key-path-593/*.md`): the campaign record (README, decision, correctness/measurement reviews, per-path analyses k1–k4, atlas, ledger, index). Unique to the PR.
- **harness** (25, `bench/research/key-path-593/harness/`): 24 run/analysis scripts (`.sh`, `.py`) + `SHA256SUMS`. Run scripts for the #593 measurement arms. Unique to the PR.
- **patches** (15, `bench/research/key-path-593/patches/`): 13 `.patch/.diff` measured-arm definitions + 1 `.rej` (archived conflict hunk) + `SHA256SUMS`. Text artifacts only; not applied to any product tree in this branch.
- **唯一提交**: yes — 2 commits (`7bf55a87`, `2714aa65`) exist only on this branch (`git rev-list` and `git cherry` both confirm).
- **唯一内容**: yes — all 56 files/blobs absent from master by path, blob SHA, and content sha256.
- **唯一复现入口**: yes — per `bench/research/key-path-593/README.md`: patches apply to frozen base `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` to reconstruct every measured arm; the harness scripts drive the runs; SHA256SUMS pins exact bytes. Loss of this branch loses the only copy of that reproduction entry.
- **产品代码**: **none**. The PR adds files only under `bench/research/` and `docs/research/`; it modifies no `include/`, `src/`, `apps/`, `tests/`, `xmake*`, build or CI wiring. The `.patch/.diff` files are inert text in the tree. `bench/.../README.md` self-describes: "experiment provenance, not product tooling: not built by xmake, not wired into CI".

## 5. Per-file classification

Classes: `ALREADY_ARCHIVED` (content already on master via #610 or otherwise) / `UNIQUE_RESEARCH_EVIDENCE` / `REBUILDABLE_TEST_HARNESS` / `PRODUCT_CODE_PRESENT` / `UNRESOLVED_PROVENANCE`.

- `ALREADY_ARCHIVED`: **0 files** (triple check above).
- `PRODUCT_CODE_PRESENT`: **0 files**.
- `UNRESOLVED_PROVENANCE`: **0 files** (every file's provenance is established: commit `7bf55a87`, 2026-10-26, frozen base `bdfd0d8d`).
- `UNIQUE_RESEARCH_EVIDENCE`: **31** — 15 docs + bench README + 15 patch-archive artifacts (frozen measured-arm bytes; irreplaceable provenance).
- `REBUILDABLE_TEST_HARNESS`: **25** — 24 harness scripts + harness SHA256SUMS (re-runnable against the archived patches; "rebuildable" only from this branch — no copy exists on master, and re-derivability from the docs alone was NOT independently verified).

Full table follows.

| Path | Class | Note |
|---|---|---|
| bench/research/key-path-593/README.md | UNIQUE_RESEARCH_EVIDENCE | blob cdf643c3c853b7a6fffb29309b3e7241459d041c; archive entry/provenance doc; branch-unique bytes |
| bench/research/key-path-593/harness/SHA256SUMS | REBUILDABLE_TEST_HARNESS | blob 91cbef8d0382b3ec26cbbf35050f5e57e10b82cd; pins 24 harness files; verifies 24/24 OK; branch-unique bytes |
| bench/research/key-path-593/harness/analyze_baseline.py | REBUILDABLE_TEST_HARNESS | blob a0682ca1bd87b5300f2abf710f57b35e79ad03f1; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/analyze_census.py | REBUILDABLE_TEST_HARNESS | blob d16ea79a74e68adf01f7d43ab8cf85424fc35248; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/analyze_pairs-k1.py | REBUILDABLE_TEST_HARNESS | blob eb4cc8ff0f214a26d198ecdf7db7d6a382ff089e; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/analyze_pairs-k3.py | REBUILDABLE_TEST_HARNESS | blob 1ced2bf88d9bdfa88dd9f5c4168651c53a3492a9; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/analyze_pairs.py | REBUILDABLE_TEST_HARNESS | blob bdddea9cd5c0aba08aaec050bbfa0dab51d32d9b; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/attribution-census-run.sh | REBUILDABLE_TEST_HARNESS | blob 131965229a57a02dc581151752085b617727429c; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/attribution-overhead-run.sh | REBUILDABLE_TEST_HARNESS | blob 2eda9561b4d7cd9b961fdfedc5d2a1120390525a; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/baseline-attribution.sh | REBUILDABLE_TEST_HARNESS | blob 766b1574f2dc44f36eac25fd11b64a881c4be517; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/baseline-run_stage.sh | REBUILDABLE_TEST_HARNESS | blob f4b3fdb214d15165b337d896696b55b38107fdb1; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/census-all-k2a.sh | REBUILDABLE_TEST_HARNESS | blob c07629a8436c4a8b1475c0e049934a4486d42678; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/census-combo.sh | REBUILDABLE_TEST_HARNESS | blob 653b439a9ea33cf53bea686e1376d24fba8c1fa6; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/census-k2a.sh | REBUILDABLE_TEST_HARNESS | blob 867bdfc81d0d64501961615f5387e57ce0f085a4; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/census-k4b.sh | REBUILDABLE_TEST_HARNESS | blob 4eac0934b5a2ae45fb80f0ab45367fd6f8ac1bb7; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/pair-run-combo-increment.sh | REBUILDABLE_TEST_HARNESS | blob 4bd4c4f7db6bf101292c6d2402af92d43e1d150d; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/pair-run-combo.sh | REBUILDABLE_TEST_HARNESS | blob a45c4623bad380ca16a6ac9c6aeb9a4530bad908; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/pair-run-k1.sh | REBUILDABLE_TEST_HARNESS | blob 25a75daaccebd67aab1c130139e73860226042a4; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/pair-run-k2.sh | REBUILDABLE_TEST_HARNESS | blob f23860f9f49c9a96408322f1057411165d98a0ba; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/pair-run-k2a.sh | REBUILDABLE_TEST_HARNESS | blob d0bc2c87e42eff751e25a048b58ca3293ed4fa6e; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/pair-run-k3.sh | REBUILDABLE_TEST_HARNESS | blob adb4423e57195442ac699f9c7aa71dcdade37c11; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/pair-run-k4.sh | REBUILDABLE_TEST_HARNESS | blob bfecc243b42acf6f23f23f233f3a31cdef02d68f; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/pair-run-k4b.sh | REBUILDABLE_TEST_HARNESS | blob 87232199e55568875f7c0378b0456026a16ab7ca; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/run-all-pairs-k2a.sh | REBUILDABLE_TEST_HARNESS | blob 8bdadf85fff066872a0b50c35f750365c3a2a2d3; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/run-all-pairs-k4b.sh | REBUILDABLE_TEST_HARNESS | blob ef110acf38620068a4a0715645d129013cd039c6; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/harness/summarize_census.py | REBUILDABLE_TEST_HARNESS | blob b1c549dc918cadf4136f8b45c8757d82fdf2a45d; run/analysis script for #593 arms; branch-unique bytes (no copy on master); re-runnable only from this branch's archived patches; re-derivability from docs NOT verified |
| bench/research/key-path-593/patches/SHA256SUMS | UNIQUE_RESEARCH_EVIDENCE | blob d602a3da2edea459394816423ff8ed1ae42845a6; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/baseline-bench-w02-uring-core64.patch | UNIQUE_RESEARCH_EVIDENCE | blob 2393de63f69fcd357cb863a99ba528eef22c62d7; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/combo-k4b-k1ab-merge-rejected-hunk.rej | UNIQUE_RESEARCH_EVIDENCE | blob fe4bef519e2059e790c9383503bcf527eafd3067; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/combo-k4b-k1ab.diff | UNIQUE_RESEARCH_EVIDENCE | blob 0decb62bdacae8a34dc5f4dd8f2e93a10b3f0274; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/k1-elision-observe.patch | UNIQUE_RESEARCH_EVIDENCE | blob 494575ddd02e4eb2f311713db437afcf06fcaa85; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/k1-elision.patch | UNIQUE_RESEARCH_EVIDENCE | blob f31fdaf676d7539ca51c4d3183050ec4ea9d75b8; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/k2-e18-arm.patch | UNIQUE_RESEARCH_EVIDENCE | blob bcfd509f167ca5de21b19275e491788078c90b41; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/k2-e18-test-repins.patch | UNIQUE_RESEARCH_EVIDENCE | blob d0da4dc6886d049bced3de506ad5c7af3b840862; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/k2a-final.diff | UNIQUE_RESEARCH_EVIDENCE | blob 12c924198c2d4ccacdc7b38c0861ae9e8258712d; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/k3-fusion-arm.patch | UNIQUE_RESEARCH_EVIDENCE | blob 3ddb4341e86feeea41ba7c570fb82a83426453cf; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/k4-defer-arm.patch | UNIQUE_RESEARCH_EVIDENCE | blob 204ad6b1a95a2f669b30528d9ebfe7bfad67de97; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/k4-defer-observe.patch | UNIQUE_RESEARCH_EVIDENCE | blob f4174968954fc3fa52da072a8ce45c394865ea6b; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/k4-defer-test-repins.patch | UNIQUE_RESEARCH_EVIDENCE | blob bab7c80702c58f493a5a796279fd9861075ec40b; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/k4b-final.diff | UNIQUE_RESEARCH_EVIDENCE | blob a534d944791cc91231a30908a767610df0b84f8e; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| bench/research/key-path-593/patches/observe-instrumentation.diff | UNIQUE_RESEARCH_EVIDENCE | blob f7ccd0c0ecd1ceac1160887ce3adec7cb81eaa4a; frozen measured-arm patch text (test-only, inert); pinned by patches/SHA256SUMS (14/14 OK); required to rebuild arms on base bdfd0d8d; no copy on master |
| docs/research/key-path-593/README.md | UNIQUE_RESEARCH_EVIDENCE | blob 9eec584b32c491235fb320b4986b0c912a3c493f; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/correctness-review.md | UNIQUE_RESEARCH_EVIDENCE | blob d4426b935d88632506dc8d80199e4db132486204; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/counterfactuals.md | UNIQUE_RESEARCH_EVIDENCE | blob e3cf4d8cf1cda9d7f416e20da7477febd1b18b2e; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/critical-path-atlas.md | UNIQUE_RESEARCH_EVIDENCE | blob e8d8b3ecc8bd1142c89d519487fa9188451bced1; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/cross-path-interactions.md | UNIQUE_RESEARCH_EVIDENCE | blob 238a8a9431a8fdf8b043a82cff83fc3045dd743b; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/decision.md | UNIQUE_RESEARCH_EVIDENCE | blob d8825dc6c9ca1e45562c0456927d1fffa62d1cfc; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/evidence-index.md | UNIQUE_RESEARCH_EVIDENCE | blob 7c5b70763c1cd49f7ad735a8283304d559e5da1c; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/execution-dag.md | UNIQUE_RESEARCH_EVIDENCE | blob 471468bbbe0a42910dd0cfca9697acd523f27d7a; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/k1-completion-publication.md | UNIQUE_RESEARCH_EVIDENCE | blob a3f0cdc26af5b1d078b3de2257da16297f7e6584; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/k2-submit-dispatch.md | UNIQUE_RESEARCH_EVIDENCE | blob 099eea00e9fe7d52543e7312e727c0add90149a1; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/k3-progress-wakeup.md | UNIQUE_RESEARCH_EVIDENCE | blob fb3384c7b7a4d29dc08bca17b8ee38a9e7620904; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/k4-sqe-cqe-batching.md | UNIQUE_RESEARCH_EVIDENCE | blob 0587cb970b096d09bc6b49d0ea4a793055758dd3; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/measurement-review.md | UNIQUE_RESEARCH_EVIDENCE | blob b42912736ebd3dd141e71aa457767a35d86f6b95; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/mechanism-ledger.md | UNIQUE_RESEARCH_EVIDENCE | blob 61a6d0c5ec62dbe27671b91c6e0467effc93023a; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |
| docs/research/key-path-593/opportunity-ranking.md | UNIQUE_RESEARCH_EVIDENCE | blob 5b6911aed293ac6618d9a6ef93198e76fb153aa2; campaign record doc; branch-unique bytes (absent from master by path/blob-SHA/sha256) |

## 6. Bottom line

PR #594 (head `2714aa65`) is a pure research-evidence archive for the #593 E5 key-path campaign: 2 unique commits adding 56 files (12,246 insertions) under `bench/research/key-path-593/` and `docs/research/key-path-593/`. Nothing in it is merged, archived, or hash-present on `origin/master` (0/56 by path, blob SHA, and content sha256; `git cherry` shows both commits patch-unique). It contains no product code and modifies no existing file. If the branch/PR were discarded without merging, the #593 archive and its only reproduction entry (patches + harness pinned by SHA256SUMS, frozen base `bdfd0d8d`) would be lost entirely. Note also the archived `decision.md` records an obligation that PR #594 "remain OPEN, unmerged per brief §14".

## 7. Not run / caveats

- No build, test, or benchmark was executed (discipline: no builds in the main workspace; this audit is static). Internal SHA256SUMS verification used `sha256sum -c` on a `/tmp` extract, not a build.
- `docs/research/direct-beyond-parity` as named in the ask does not exist on master; the #610 archive is at top-level `research/direct-beyond-parity/` — checked there and via all manifest hashes, so the intent of the check (was this content archived by #610?) is fully covered.
- Re-derivability of the harness scripts from the docs alone (i.e., whether `REBUILDABLE_TEST_HARNESS` files could be reconstructed without this branch) was not verified; they are treated as branch-unique for retention purposes.
