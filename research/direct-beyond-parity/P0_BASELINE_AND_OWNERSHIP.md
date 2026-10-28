# P0 Baseline and Ownership Record — Direct Beyond-Parity (Sluice)

- Mission: `research/direct-beyond-parity/MISSION.md` §2 (P0 — C1 closeout and baseline freeze).
- Record date: 2026-10-27. Primary adjudication platform: E5 Native Linux.
- Scope: baseline identities (B0/B1/N0/N1), PR #601/#602 boundary adjudication,
  the cost eliminations already verified by #600 (cited, not re-claimed), the
  questions P0 leaves open, and the semantics/oracle floor that every later
  phase (P1–P6) must preserve.
- P0 baseline-freeze gates: no failures recorded during the freeze step
  (build verification of B0/B1 passed on first clean rebuild; gate failure list
  is empty).
- Revision 2026-10-27 (same day, after independent review): 15 findings
  corrected — comparator-path coverage wording (§5.2), probe/fixture
  persistence performed rather than recommended (§1.4, §4.8), #600 hash chain
  quoted in-record (§1.2), order-bias caliber note (§3.2), C2 noise wording
  (§3.3), C7/G0 sourcing added (§0, §3), toolchain install provenance (§1.5),
  ownership/worktree rules extended (§1.3), fixture identity closed (§1.4),
  source-increment wording (§0), unlabeled-citation legend default (§0),
  R1 dataset full digest recovered (§3.5), full-path citation (§4.10),
  matrix-commands gap declared (§3.6), assertion-layer change explained
  (§5.4).

## 0. Provenance and verification legend

Sources used by this record:

1. The #600 evidence pack, branch `sluice-600-direct-research`,
   `research/600-direct-excess-cost/{EVIDENCE_INDEX,PHASE_I_COST_ATTRIBUTION,DIRECT_COST_DAG,DIRECT_MACHINE_COST_LEDGER,PHASE3_VALIDATION}.md`
   (research commit `e4bb8833699d0ca5dee191883f3fec6d41b221b2`, whose diff
   relative to its parent adds exactly the 5 research files, +348 lines, all
   under `research/`; the full branch-vs-master diff is 9 files because the
   parent is the mirrored product commit — see §2.1).
2. The G0 evidence pack required by MISSION P0.1 ("G0 W-01 基准与 C7 历史结论"):
   `docs/evidence/g0-403-performance-baseline-20261015/{PREREGISTRATION,MEASUREMENT,G1_CANDIDATES,RECEIPT}.md`,
   read during the P0 methodology survey, plus the C7 chain through issue #541
   (archived "do not retry without materially new evidence") and the #578
   comment (2026-10-26T15:18:36Z) reaffirming that C7 stays binding. C7's
   content and its relationship to #600 are recorded in §3 (historical
   context).
3. GitHub records read during P0 evidence collection: issue #600 (two comments,
   2026-10-26), issues #541/#578, PR #601, PR #602 (via `gh` REST/GraphQL and
   `gh pr diff`), including review/comment endpoints.
4. Repository authority: `docs/explicit-io-contract.md` (v2-r1 root),
   `docs/roadmap/v2-conformance.md`, `docs/performance/methodology.md`.
5. P0 evidence-collection steps performed on this E5 host before this record
   was written: clean-room build verification of B0 and B1, PR boundary audit,
   Direct semantics/oracle inventory, methodology survey.
6. Persistence snapshot created during revision of this record:
   `research/direct-beyond-parity/probe-snapshot-600/` (§1.4) — byte-exact
   copies of the uncommitted #600 comparator kit, hashed in its README.

Legend used throughout:

| Tag | Meaning |
|---|---|
| `[RUN-THIS-SESSION]` | Command executed and output observed while writing this record; command cited inline. |
| `[RUN-P0]` | Executed during this mission's P0 evidence-collection steps (before this writing session); command and output recorded in the P0 inventory. |
| `[TRANSCRIBED]` | Number or claim transcribed from the #600 evidence documents, PR bodies, or issue comments; not re-measured in P0. |
| `[UNCERTAIN]` | Could not be fully verified from the repository or the given material; flagged rather than filled with a guess. |
| (no tag) | A static citation (file path, line anchor, or named assertion) taken from repository files or from the P0 evidence-collection inventory; not independently executed in this session. Its verification status is that of the surrounding section's text. |

## 1. Baseline identities

### 1.1 Summary

| Code | Role (MISSION P0.3) | Identity | Buildable from the repository alone? |
|---|---|---|---|
| B0 | Original Direct, #600 frozen baseline | commit `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` (= master at freeze) | Yes (xmake target `sluice_core`) |
| B1 | Direct after C1 (product candidate) | commit `77e5399a45998dea396699fa2e063c94a665945a` = PR #602 head, branch `sluice-600-direct-c1` (frozen in isolated worktree) | Yes (same target) |
| N0 | Bare-POSIX mechanism floor | Uncommitted probe `/home/jnhu/sluice-600-direct-research/bench/ladder.cpp`, `Arm::A0` bare `::pread` (`ladder.cpp:127-129`) | No — but a byte-exact snapshot is preserved at `research/direct-beyond-parity/probe-snapshot-600/` (§1.4) |
| N1 | Semantics-matched optimized native | Same probe file, `a1_read_at` (`ladder.cpp:42-63`) | No — same snapshot (§1.4) |

Name mapping to #600 evidence arms (the #600 pack uses its own ladder names):
B0 = #600 "baseline"/A2 arm at `bdfd0d8d`; B1 = #600 "candidate" (A2 after C1;
AX for `read_exact_at`); N0 = `A0_raw_pread`; N1 = `A1_native_matched`
(`ladder.cpp:110-111`). The probe also carries `A3_nosyscall_stub`
(`ladder.cpp:113`), a test-only no-kernel arm not used as a baseline here.

### 1.2 B0 — frozen #600 baseline

- Identity: `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`. Three-way agreement
  `[RUN-P0]`: `git rev-parse master` on this host; #600 completion comment
  (`BASE_SHA = bdfd0d8d…`); `EVIDENCE_INDEX.md` Provenance line. Re-confirmed
  this session: `git rev-parse master` → same SHA `[RUN-THIS-SESSION]`.
- Detached worktree `/home/jnhu/Source/Sluice-600-base` at that SHA; worktree
  end state has no tracked modifications, only the untracked build output
  directory (`git status --porcelain` → `?? build-baseline/`)
  `[RUN-THIS-SESSION]`.
- Build verification `[RUN-P0]`: inside that worktree,
  `xmake f -m release --liburing=n -y` (exit 0) then `xmake -P
  /home/jnhu/Source/Sluice-600-base -b sluice_core` (exit 0), after removing
  `.objs/sluice_core` and the old archive for an object-level clean rebuild
  (2026-10-27 01:28:55, `CCACHE_DISABLE=1`). Verbose log showed compile flags
  `-m64 -fvisibility=hidden -Wall -Werror -O3 -std=c++20 -DNDEBUG` and archive
  step `/usr/sbin/ar -cr -D` (deterministic archive).
- Binary identity: `build-baseline/linux/x86_64/release/libsluice_core.a`,
  21072 bytes, sha256
  `489104b5819cdaca251471d5e0b10c20051071bca88dcb7bfc198f1336c99c5a` —
  byte-identical to the #600 frozen baseline hash (clean-room reproduction).
  The #600-side anchor, so the identity chain closes inside this record
  `[RUN-THIS-SESSION: git show
  sluice-600-direct-research:research/600-direct-excess-cost/EVIDENCE_INDEX.md]`,
  lines 6–8 read: "Libraries: baseline sha256
  489104b5819cdaca251471d5e0b10c20051071bca88dcb7bfc198f1336c99c5a, candidate
  sha256 d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0".
  B0's rebuild matches the first value byte-for-byte; B1's (§1.3) matches the
  second. Artifact hash re-verified while writing this record
  `[RUN-THIS-SESSION]`.

### 1.3 B1 — C1 product candidate (PR #602 head)

- Identity: `77e5399a45998dea396699fa2e063c94a665945a` = head of
  `sluice-600-direct-c1` = PR #602 head `[RUN-P0: gh pr view 602 --json
  headRefOid]`. Branch = exactly two commits over master:
  `4c3e698daf13e68912ccc19e39c1ceae12b916b8` (perf(direct): dispatch admission
  verdicts without Precheck materialization, #600 C1) and `77e5399a`
  (docs(ledger): #600 C1 SEM-03/04 mutation evidence — touches only
  `docs/roadmap/v2-conformance.md`, 1 file, +1/−1).
- Since PR #602 is still OPEN, MISSION P0.3 required freezing its
  implementation SHA in an isolated worktree: `/home/jnhu/Source/Sluice-600-wt`
  is at HEAD `77e5399a` with a clean `git status --porcelain`
  `[RUN-THIS-SESSION]`.
- **Ownership rules going forward (P1+):**
  1. No experimental commit may enter either #600 delivery branch —
     `sluice-600-direct-c1` (PR #602) or `sluice-600-direct-research`
     (PR #601) — regardless of the §2.3 shrink's execution state.
  2. The two frozen worktrees are identity anchors, not scratch space:
     `/home/jnhu/Source/Sluice-600-base` stays checked out at `bdfd0d8d` with
     its hashed archive in `build-baseline/`, and `/home/jnhu/Source/Sluice-600-wt`
     stays at `77e5399a` with its hashed archive in `build/`. P1 measurement
     may *link against* these archives read-only, but reconfiguring or
     rebuilding in place would invalidate the anchor: candidate builds happen
     in fresh worktrees.
  3. P1 candidate branches/worktrees are new, named for the behavior or
     mechanism under investigation (AGENTS.md active-naming rule; no phase or
     issue labels), created from `bdfd0d8d` (B0) or from the frozen B1 SHA
     `77e5399a` as the comparison requires — never on the #600 branches.
- Build verification `[RUN-P0]`: same recipe as B0 inside this worktree
  (`xmake f -m release --liburing=n -y`; `xmake -P
  /home/jnhu/Source/Sluice-600-wt -b sluice_core`, exit 0, object-level clean
  rebuild 2026-10-27 01:29:11, `CCACHE_DISABLE=1`, same flags/archive mode).
- Binary identity: `build/linux/x86_64/release/libsluice_core.a`, 19704 bytes,
  sha256
  `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0` —
  byte-identical to the #600 frozen *candidate* value (the second hash quoted
  in §1.2 from `EVIDENCE_INDEX.md:6-8`). Because the #600 candidate was
  measured at `ea2fc4fc` and `77e5399a` differs only by the pure documentation
  commit, the identical library hash also proves B1's product code is
  equivalent to the measured candidate object-for-object. Artifact hash
  re-verified this session `[RUN-THIS-SESSION]`.

### 1.4 N0 / N1 — native comparator arms (weakest reproduction link)

- Identity: single uncommitted research probe
  `/home/jnhu/sluice-600-direct-research/bench/ladder.cpp`, sha256
  `d879b2685b14539efa6552b9918776a401ee5633339df4db1e58c70c252fe884`
  `[RUN-THIS-SESSION]`. The file header self-declares "Research probe: lives in
  the #600 workspace, not the repository".
  - N0 = `Arm::A0` branch, bare `::pread` (`ladder.cpp:127-129`), arm name
    `A0_raw_pread` (`ladder.cpp:110`). A syscall-entry floor: it still enters
    the kernel at 0 B, so it bounds mechanism cost and is not a
    semantics-carrying replacement.
  - N1 = `a1_read_at` (`ladder.cpp:42-63`), arm name `A1_native_matched`
    (`ladder.cpp:111`): hand-written minimal `read_at` with the same check
    order, error codes, EINTR retry and `Result<size_t>` ABI, an independent
    TU boundary, public Sluice headers only (no `detail/` headers), linked
    against `libsluice_core.a`.
- Repository buildability: none. `ladder` appears in no `xmake.lua`/`xmake/`
  target on master or on `sluice-600-direct-research` (grep: 0 matches)
  `[RUN-P0]`, and the original compile command is recorded nowhere —
  `paired.sh`/`paired2.sh` in the same workspace only invoke the prebuilt
  `./ladder` binary. `EVIDENCE_INDEX.md` calls the workspace "reproducible from
  this index", but the probe source itself existed only as that uncommitted
  copy — the weakest link of the #600 evidence chain (if the workspace were
  lost, N1 and every ladder number could not be rebuilt from the repository).
- Persistence (performed at revision; closes the above gap for the source
  identity) `[RUN-THIS-SESSION]`: byte-exact copies of the comparator kit now
  live in this mission directory at
  `research/direct-beyond-parity/probe-snapshot-600/` — `ladder.cpp`,
  `mkfixture.cpp`, `paired.sh`, `paired2.sh`, `scripts/analyze_pairs.py`.
  Every copy's sha256 equals its workspace original (verified at copy time;
  `ladder.cpp` = `d879b2685b14539efa6552b9918776a401ee5633339df4db1e58c70c252fe884`
  on both sides); the snapshot's README records all five hashes and the
  reproduction anchors. The originals' self-declared status ("lives in the
  #600 workspace, not the repository") is unchanged — whether the snapshot is
  committed is an owner decision. Still outside the snapshot: the compiled
  binaries and the raw series under `~/sluice-600-direct-research/evidence/`
  (workspace only).
- Fixture identity (performed at revision) `[RUN-THIS-SESSION]`: the 64 MiB
  fixture `/tmp/sluice600/data64m.bin` sits on tmpfs and vanishes on reboot;
  its sha256 is
  `bc020fd15a622e081793b0fb8ff2383838e27f6d1cb2135f204aad8383b2993a`.
  `mkfixture.cpp` is fully deterministic (fixed LCG byte pattern, no seed,
  no randomness): `g++ -O2 -std=c++20 mkfixture.cpp -o mkfixture && ./mkfixture
  <path> 67108864` regenerated a fixture that is byte-identical to the
  original (`cmp` clean, same sha256). The `21233664` figure in the
  reconstruction check below is the ladder arms' output checksum, not the
  fixture digest. The fixture generation method is therefore closed.
- Reconstruction check `[RUN-P0]` (command *inferred* from source
  dependencies, not the evidence-recorded command — the latter does not exist):

  ```sh
  g++ -std=c++20 -O3 -DNDEBUG -fvisibility=hidden -Wall -Werror \
      -I/home/jnhu/Source/Sluice-600-base/include \
      /home/jnhu/sluice-600-direct-research/bench/ladder.cpp \
      /home/jnhu/Source/Sluice-600-base/build-baseline/linux/x86_64/release/libsluice_core.a \
      -o /tmp/n01-check/ladder_n01
  ```

  Exit 0; 36768 bytes, same size as the workspace `ladder_base` binary. Running
  the a0 and a1 arms on `/tmp/sluice600/data64m.bin` (64 MiB fixture,
  4096 B × 1024 ops × 5 reps): exit 0, checksum `21233664` identical across
  a0/a1/a2/ax arms, no VERIFY_FAIL; a1 ns/op ≈ 775, in the same band as the
  other arms. The rebuild is therefore feasible, but the authoritative compile
  identity of the *measured* binaries remains `[UNCERTAIN]`.

### 1.5 Toolchain, build recipe, and binary identity

- Toolchain (identical for B0 and B1, same host and session), with install
  provenance `[RUN-THIS-SESSION: rpm -q gcc clang binutils glibc kernel-core
  xmake; which xmake gcc clang ar]`:
  - GCC 16.2.1 20260819 — Fedora package `gcc-16.2.1-2.fc44.x86_64`. Note
    `which gcc` resolves to `/usr/lib64/ccache/gcc` (ccache wrapper first on
    PATH); the clean rebuilds ran with `CCACHE_DISABLE=1`.
  - binutils ld/ar 2.46.1 — `binutils-2.46.1-1.fc44.x86_64` (`ar` at
    `/usr/sbin/ar`, `-D` deterministic archives).
  - glibc `glibc-2.43-9.fc44.x86_64` (the 2.46.1 figure sometimes quoted
    alongside #600 is binutils' `ld`, not glibc).
  - Running kernel 7.2.11-200.fc44.x86_64 — `kernel-core-7.2.11-200.fc44.x86_64`
    (two older 7.2.9 kernels are also installed).
  - Clang 22.1.8 — `clang-22.1.8-4.fc44.x86_64`; present and relevant to later
    compiler-comparison work; no Clang build has been frozen in P0.
  - xmake: the build-affecting binary is `/home/jnhu/.local/bin/xmake`,
    reporting v3.1.2+HEAD.5d21e9d — a **user-local install that shadows the
    Fedora package** (`xmake-3.0.6-2.fc44.x86_64` is also installed but not
    what runs). The install channel of the local build is not recorded
    `[UNCERTAIN]`; on a new host, reproducing the build environment means
    reproducing xmake v3.1.2+HEAD.5d21e9d at that path, not just `dnf install
    xmake`.
- Build recipe (both library baselines): `xmake f -m release --liburing=n -y`
  then `xmake -b sluice_core`, executed from inside the respective worktree
  directory. Reproduction caveat `[RUN-P0]`: xmake 3.1.2 resolves the relative
  `builddir` against the process cwd, so running with `-P` from another
  directory (e.g. `/tmp`) misplaces the build tree — always `cd` into the
  worktree first.
- Host and measurement environment (matches the #600 measurement
  configuration): Intel Xeon E5-2666 v3 (10C/20T, 1 socket, L3 25 MiB, single
  NUMA node), Fedora 44, `scaling_governor=performance`,
  `perf_event_paranoid=1`, `nmi_watchdog=0`, `/tmp` is tmpfs, root filesystem
  btrfs+zstd `[RUN-P0]`. Whether `drop_caches` is usable on this host was not
  tested — `[UNCERTAIN]`, relevant to any future controlled cold-ish work.
- Binary identity discipline: every performance claim must name the commit and
  the sha256 of the linked `libsluice_core.a` (or binary) it measured, exactly
  as `EVIDENCE_INDEX.md` does for #600. The two frozen hashes above are the
  anchors for all P1+ comparisons.

## 2. PR #601 / #602 boundary adjudication

### 2.1 Established facts

All items `[RUN-P0]` via `gh pr view/diff`, `git show`, `git diff`, `git
patch-id`, `gh api` unless noted; PR-head states re-confirmed this session via
`git ls-remote origin` (`sluice-600-direct-c1` → `77e5399a`, 
`sluice-600-direct-research` → `e4bb8833`) `[RUN-THIS-SESSION]`.

1. PR #601 (head `e4bb8833699d0ca5dee191883f3fec6d41b221b2`) presents itself as
   evidence-only ("Evidence-only diff: research/600-direct-excess-cost/ … the
   code change is the separate product PR") yet its own body admits "commit
   mirrored here as ea2fc4fc". Its actual diff is 9 files: the 5 research
   documents plus the 4 product/test/build files
   `include/sluice/detail/file_semantics.hpp`, `src/blocking_file.cpp`,
   `tests/semantic_range_test.cpp`, `xmake/tests.lua`.
2. The mirror commit `ea2fc4fca1a65a63ef3664f71dab1014e98c3b25` (research
   branch) and the product commit `4c3e698d…` (#602 branch) are the same C1
   change: both patches are 359 lines and byte-identical (patch comparison
   exit 0), equal `git patch-id`
   `8ed5d3c0ae157859c44548fe838824aecbfbf520`, and the two PR heads differ on
   none of the 4 shared files (`git diff ea2fc4fc 77e5399a -- <each>` → 0
   lines).
3. PR #602 additionally carries the ledger commit `77e5399a`
   (`docs/roadmap/v2-conformance.md` only). PR #601 additionally carries only
   the 5 research documents.
4. Review state: PR #601 has 0 reviews and 0 inline comments (REST endpoints
   empty); its only comment is a coderabbitai bot comment. PR #602 likewise
   has `reviewDecision=""`, 0 reviews, 0 inline comments, one coderabbitai bot
   comment; CI 6/6 checks pass; `mergeable_state=clean`; body says "OPEN for
   owner decision; not to be merged without review".
5. The frozen evidence documents hard-reference the measurement identity:
   `EVIDENCE_INDEX.md:3-4` "candidate ea2fc4fca1a6 (this branch)" and
   `PHASE3_VALIDATION.md:3-4` "CANDIDATE_SHA: ea2fc4fca1a6 (branch
   sluice-600-direct-research, …)" `[RUN-THIS-SESSION: git show
   sluice-600-direct-research:research/600-direct-excess-cost/…]`. The #600
   completion comment likewise records "CANDIDATE_SHA = ea2fc4fca1a6 (research
   head; product branch: 4c3e698d + ledger 77e5399a)".
6. The adjudication plan below is **not executed**: the backup branch it
   defines does not exist on the remote, and both branch heads still equal the
   pre-split values `[RUN-THIS-SESSION: git ls-remote]`.

### 2.2 Adjudication

- **PR #602 is the delivery anchor and already conforms to the MISSION P0.2
  boundary.** Its two commits are exactly product+tests+mutation
  (`4c3e698d`) and Conformance Ledger (`77e5399a`). The branch stays as is:
  review-driven changes may only *append* commits on top of `77e5399a`;
  rewriting `4c3e698d` is forbidden because the byte-identity/patch-id
  equivalence chain to the research-measured `ea2fc4fc` (and thus to the #600
  evidence) would break. Merge strategy: prefer a merge commit so
  `4c3e698d`/`77e5399a` keep their SHAs in master; squash would not dangle any
  document reference (the frozen docs cite `ea2fc4fc`, not these SHAs) but
  loses the original commit identity.
- **PR #601 violates the boundary and must shrink to research-evidence-only.**
  The mirror of the product commit is the ambiguity MISSION P0.2 requires
  removing. After the shrink the two PRs have zero file intersection and their
  merge order decouples.
- **No re-labeling of measurements.** The frozen evidence text (including its
  "(this branch)"/"(branch sluice-600-direct-research)" wording) is not edited;
  the measurement-identity change is carried by PR-body errata and appended
  comments only, per MISSION P0.2 ("不得将原始证据重新标记…").

### 2.3 Execution plan (PROPOSED — not executed; requires remote mutation)

Precondition re-check (immediately before execution): `git rev-parse master`
still `bdfd0d8d…` or a known descendant; PR #601 reviews/comments endpoints
still empty (a new human review comment would pause the plan and re-anchor it
first).

1. **Stage 0 — backup before any history rewrite:** `git push origin
   origin/sluice-600-direct-research:refs/heads/backup/600-research-pre-split-e4bb8833`,
   then verify `git ls-remote` shows
   `e4bb8833699d0ca5dee191883f3fec6d41b221b2`. This branch is the remote
   keep-alive for both `ea2fc4fc` (CANDIDATE_SHA) and `e4bb8833`; GitHub's
   `refs/pull/601/head` will not preserve the old chain after a force push.
   Keep it long-term — at minimum past the planned WSL2 portability task;
   recommended permanently.
2. **Stage 1 — shrink locally, in a temporary worktree** (MISSION P0.2 requires
   an isolated worktree; using the main tree was avoided per review): `git
   checkout -B sluice-600-direct-research-split bdfd0d8d…` then `git
   cherry-pick e4bb8833…`. The commit's diff adds only the 5 research files
   (+348), so it applies cleanly. Self-check gates before pushing: `git show
   --stat HEAD` lists only the 5 research files; `git diff bdfd0d8d..HEAD --
   include/ src/ tests/ xmake/` is empty.
3. **Stage 2 — safe force-push:** `git push --force-with-lease=
   sluice-600-direct-research:e4bb8833699d0ca5dee191883f3fec6d41b221b2 origin
   sluice-600-direct-research-split:sluice-600-direct-research`. The lease
   fails safely if anyone advanced the branch. Afterwards PR #601's diff must
   contain only the 5 research documents.
4. **Stage 3 — PR #601 body erratum** (`gh pr edit 601`): remove the "commit
   mirrored here as ea2fc4fc" sentence; replace with the measurement identity
   chain: measurements were taken on candidate `ea2fc4fca1a6`, the pre-split
   head of this branch, preserved verbatim on
   `backup/600-research-pre-split-e4bb8833`; it is byte-identical (equal
   patch-id `8ed5d3c0ae157859c44548fe838824aecbfbf520`) to product commit
   `4c3e698d` in PR #602; this PR now carries research evidence only. The
   erratum must also state that the branch name `sluice-600-direct-research`
   cited in `PHASE3_VALIDATION.md:4` now points at the shrunk branch and that
   the measured tree is preserved on the backup branch, so the wording and the
   document text cannot be misread as contradicting each other.
5. **Stage 3b — #600 appended erratum comment** (add, never edit): the #600
   completion comment's "research head" phrasing for `ea2fc4fca1a6` goes stale
   after the shrink; append a comment recording that the measured candidate
   head now lives on the backup branch.
6. **Stage 4 — #602 disposition:** branch unchanged; review changes
   append-only on `77e5399a`; merge-commit preferred (see §2.2).
7. **Stage 5 — closeout:** PR #601 (now research-only) reviews and merges with
   any strategy; branch cleanup must not delete
   `backup/600-research-pre-split-e4bb8833`, and local cleanup must not delete
   the history reachable from local `sluice-600-direct-research` (still at
   `e4bb8833`), which is the second, local keep-alive.
8. **Alternative path A** (if #602 is already merged when execution starts):
   instead of the cherry-pick, `git checkout -B sluice-600-direct-research-split
   e4bb8833 && git rebase --onto origin/master ea2fc4fc…`. Mechanism note
   (corrected per review): the replay range `(ea2fc4fc, e4bb8833]` contains
   only the research commit — the product mirror is excluded by the rebase
   upstream parameter directly, not dropped by patch-id detection; `git cherry`
   output (`- ea2fc4fc` / `+ e4bb8833`) independently confirms the patch
   equivalence. Stages 0/2/3/3b/5 are unchanged.

### 2.4 Risks

1. After the shrink, remote reachability of `CANDIDATE_SHA ea2fc4fca1a6`
   depends on a single branch, the backup. Mitigation: stage 0 runs first and
   the backup is long-lived (§2.3.1, §2.3.7). Local `sluice-600-direct-research`
   at `e4bb8833` is a second, local keep-alive — not a substitute for the
   remote backup.
2. Measurement-identity wording: the frozen documents keep saying "(this
   branch)"/"(branch sluice-600-direct-research)" while the branch now carries
   only the documents. Mitigation: errata carry the identity chain (§2.3.4);
   document text is never rewritten. Any future revision may only add a
   supersession note in a new commit.
3. If #602's review ever demands rewriting `4c3e698d` (instead of appending),
   the `ea2fc4fc ↔ 4c3e698d` equivalence assertion voids and all errata must
   be redone — hence the append-only rule.
4. The cherry-picked research commit gets a new SHA; no repository document
   references `e4bb8833` itself (`git grep` over the research tree: none), so
   no dangling reference is created; external links to `e4bb8833` resolve via
   the backup branch. The #600 comment's references were not re-checked
   link-by-link `[UNCERTAIN]`.
5. Review anchors: currently 0 reviews / 0 inline comments on both PRs, so no
   inline anchors exist to invalidate; if any appear before execution, the
   precondition re-check pauses the plan.
6. `--force-with-lease` only guards the push instant; the plan assumes no
   parallel developer on #601 (supported by the empty review/comment state,
   not a guarantee). If the lease trips, re-evaluate the incoming change
   before retrying.

### 2.5 Open questions (boundary)

1. Why the research PR mirrored the product commit at all (e.g., to make the
   evidence self-contained on its own branch) is nowhere documented and no
   comment discusses it — motivation `[UNCERTAIN]`.
2. Owner decisions outstanding: merge order, whether C1 enters master via #602
   at all, and whether #601 merges before or after the shrink executes. Both
   PRs remain OPEN; nothing is merged.
3. PR #602's validation claims (58/58 matrix, mutation kills, E5 numbers) are
   stated in its body/ledger; whether they were executed against the identical
   patch in both PRs cannot be confirmed from any review record (there are
   none) — validation attribution `[UNCERTAIN]`.
4. Review-stage corrections already folded into §2.3: the #600-comment erratum
   obligation (§2.3.5), the branch-name wording in `PHASE3_VALIDATION.md:4`
   (§2.3.4), the worktree requirement (§2.3.2), the path-A mechanism note
   (§2.3.8), and the local keep-alive (§2.3.7). No further boundary questions
   were raised in review.

## 3. Verified cost elimination (inherited from #600 — cited, not re-claimed)

Per MISSION §2 (P0.1), the following are **existing #600 conclusions**, not
new findings of this mission. They are transcribed from
`research/600-direct-excess-cost/` on `sluice-600-direct-research` and the
#600 completion comment; P0 did not re-measure any of them. The only P0-level
re-verifications are the two library sha256 reproductions (§1.2, §1.3) and the
oracle/mutation-seam existence checks (§5).

Historical context (the "G0 W-01 baseline and C7 historical conclusions"
reading MISSION P0.1 requires): the G0-era candidate ledger's C7 entry closed
the W-01 direct path as `NO_OPTIMIZATION_JUSTIFIED`
(`docs/evidence/g0-403-performance-baseline-20261015/G1_CANDIDATES.md` line 26:
"W-01 direct path: 1.00 pread64/op, p50 912 ns for 4 KiB on tmpfs … Expected
benefit: None identified, Decision: NO_OPTIMIZATION_JUSTIFIED"; also
`RECEIPT.md:40`). Issue #541 (CLOSED) archived it under "Do not retry without
materially new evidence", and the #578 comment of 2026-10-26T15:18:36Z
reaffirmed "Historical W-01 #541 C7 negative findings remain binding until
counterevidence" `[TRANSCRIBED]`. #600 is precisely that counterevidence: C7
was decided from syscall-count parity on G0 (WSL2 host) before #600 measured
the E5 userspace implementation excess (≈28 instructions, +12.8–19.3 ns) —
which is why C1 exists and why C7 does not bar this mission. Two C7 facts
stay binding: the direct path was already at 1.00 pread64/op, and the WSL2
G0 absolute numbers (p50 912 ns @4 KiB) are historical shape references, not
E5 targets.

### 3.1 Excess proven (Phase I, baseline vs matched native)

- `A2 − A1` excess on E5, 16 ABBA pairs/cell: +12.8 ns at 0 B (t=+148 — the
  0 B cell has no kernel path, isolating the excess as userspace), +19.3 ns at
  1 B, +15.8 ns at 4 KiB (t=+19.5); 64 KiB +6.2 ns not significant.
- Static attribution, `read_at` success path at -O3: ≈70 instructions vs ≈42
  for the matched native A1 → ≈28 excess instructions/call, attributed to
  Precheck structure materialization (≈27) plus the redundant
  `checked_posix_offset` sign test (≈2); the composition step costs ≈45–60
  instructions/step. Sources: `DIRECT_MACHINE_COST_LEDGER.md`,
  `PHASE_I_COST_ATTRIBUTION.md`.
- Verdict recorded by #600: `EXCESS_MACHINE_WORK_PROVEN`.

### 3.2 C1 elision (B1's product change)

- Success-path static instructions 70 → 40 (`PHASE3_VALIDATION.md` V4;
  assembly archived by #600).
- E5 paired deltas (candidate − baseline, 16 ABBA pairs, taskset cpu2, tmpfs;
  A/A pair-delta sd 5.0 ns, minimum detectable effect ≈2.5–3 ns):
  0 B −13.3 ns (−57%, t=−179.8); 1 B −16.4 (t=−10.0); 64 B −15.8 (t=−11.4);
  512 B −16.2 (t=−10.1); 4 KiB −16.6 (t=−12.4; independent re-run −14.2,
  t=−10.2); 64 KiB −3.2 n.s. (below noise, expected);
  `read_exact_at` 4 KiB −33.8 ns (−3.3%, t=−22.4). Net: −13.3…−16.6 ns/op on
  warm ≤4 KiB reads (≈1.4–3.0%).
- Statistical caliber of the figures above: per
  `DIRECT_MACHINE_COST_LEDGER.md`, #600 quantified a systematic −5.1 ns
  first-run-faster order effect and cancels it *by the ABBA design itself*
  (alternating arm order per pair); the per-cell deltas above are the ABBA
  pair statistics as recorded in `PHASE3_VALIDATION.md` V2. Whether any
  additional numeric post-hoc correction beyond the ABBA cancellation was
  applied is not stated in the transcribed material — P1 engineers using
  these numbers as controls must read the raw series
  (`~/sluice-600-direct-research/evidence/e3_*.txt`, workspace) for the exact
  statistic convention `[UNCERTAIN]`.
- Verdict: `EXCESS_MACHINE_WORK_ELIDED` (mechanism level).

### 3.3 C2 composition pre-validation — implemented, then reverted

The combined pre-validation step primitive was implemented and proven at the
machine level (compose loop reduced to `pread`/`__errno_location` calls; the
out-of-line precheck clone disappeared; fault-injection mutants killed), but
it produced no measurable gain over C1, failing the admission rule ("only on
repeatedly measurable gain"). Caliber note: the AX−A2 composition gap moved
from +9.9 ns to +9.7 ns — what lies within noise (minimum detectable effect
≈2.5–3 ns, §3.2) is the **0.2 ns change**, i.e., C2 did not measurably shrink
the gap; the **≈+9.7 ns residual gap itself is resolvable** and stands
recorded as unexplained composition cost (§4.7). Reverted; the patch is
preserved at `~/sluice-600-direct-research/evidence/c1c2_full_worktree.patch`
(existence confirmed during P0 evidence collection).

### 3.4 D3 — Result/ABI shape

The `Result<size_t>` sret + spills + errno-store code shape is emitted by GCC
identically in the hand-written matched-native control (A1); it is necessary
ABI cost of the public interface, not Sluice-specific excess. Verdict:
`NO_CHANGE_JUSTIFIED`.

### 3.5 R1 Parquet application shape — no transfer, no regression

Frozen dataset, sha256
`6116e69cf20a89b7ca8a23e4d4dbd7d3759225a68e379d4776b3727bf3c05af6`
(`medium-none.parquet`, recorded file size 181,966,794 B), 57 reads /
181,996,415 B. Digest provenance: the #600 pack itself truncates to
`6116e69c…` (`EVIDENCE_INDEX.md:31`, `PHASE3_VALIDATION.md:58`); the full
value was recovered this session from the raw trace header
`/tmp/sluice600/trace_medium_t3_p1.jsonl:1` (`"dataset_sha"`,
`"dataset_path":"build/parquet-data/medium-none.parquet"`)
`[RUN-THIS-SESSION: grep -rn 6116e69c /tmp/sluice600/]`. Caveat: this is the
trace's *self-declared* digest — P0 did not re-hash the parquet file itself
(it lives on the dududu workspace side), so the dataset identity closes only
to the trace record `[UNCERTAIN at the file level]`. Preregistered arithmetic:
57 × ~16 ns ≈ 0.9 µs against ≈240 ms per pass = 0.0004% — undetectable by
design. Measured (16 ABBA pairs): Replay/sluice −82.5 µs (t=−0.20);
Replay/native control +536.0 µs (t=+2.15); T3_fullscan/p2_sluice −31.9 µs
(t=−0.07); T3_fullscan/p1_native −429.1 µs (t=−0.42); the P1 control arm's
±0.2–0.5 ms wander bounds harness noise far above the possible effect. For
this application shape the #600 verdict is `MECHANISM_ONLY_GAIN` (the
mechanism-level elision is real; nothing transfers at this call granularity),
with no regression. The dududu R1 raw receipts and #593 evidence were declared
untouched by #600 and remain untouched by P0.

### 3.6 Correctness matrix attached to the #600/C1 claims

`[TRANSCRIBED]` from `PHASE3_VALIDATION.md` V1 and the PR #602 body/ledger:
`xmake test` 58/58 across release / debug / asan / asan+ubsan; three named
admission mutants each killed by its named suite assertion with
GREEN→RED→GREEN (plus two more mutants recorded under C2); package verifier
all gates PASS; W-01 repeated with zero failures; strace shows exactly 1.000
pread64/op on A0=A1=A2=AX and 0 syscalls on the zero-length path. The
mutation-kill and matrix runs were **not** re-executed in P0; the mutant
seams, their xmake targets, and the killer assertions were re-verified to
exist at B1 (§5.3). Command-reproducibility gap: the four configurations are
the modes `release` and `debug` (xmake built-ins) plus the repo-defined
`asan` (`xmake.lua:7`) and `asanubsan` (`xmake.lua:16`)
`[RUN-THIS-SESSION: grep -n 'is_mode' xmake.lua]`, but the exact
configure/build/test command lines per configuration — including the
`--liburing` setting each mode used — are **not captured anywhere in this
record or the transcribed material**. P1 must pin and record the exact matrix
commands before treating a re-run as the same check (§5.3 names the matrix a
prerequisite at the next merge gate); no command is reconstructed here by
guesswork.

## 4. Unresolved questions

Ordered by impact on the next phases; each carries its source.

1. **Productization undecided.** PR #601/#602 both OPEN, `MERGED = NO`; C1's
   entry into master awaits owner review/decision (PR #602 body; §2.5.2).
2. **Boundary plan unexecuted.** The §2.3 plan requires remote mutation and an
   owner go-ahead on sequencing; backup branch absent, heads unchanged
   `[RUN-THIS-SESSION]`.
3. **Tail latency unsupported at current effect sizes.** Per-op sampling clock
   overhead ≈26 ns (W-01 CAL) exceeds the −13…−17 ns mechanism effect, so
   #600 recorded `E5_TAIL_DELTA = NOT_SUPPORTABLE`; per-op p95/p99 unmeasured
   (`PHASE3_VALIDATION.md` V2). `docs/performance/methodology.md:28` names
   "latency tails" but defines no procedure when effect < sampling overhead,
   while MISSION P4.4 requires a `P99_DELTA` per candidate — the gap must be
   resolved (or explicitly waived) before P4.
4. **The many-small-reads application shape is not landed.** The mechanism
   gain only realizes in that shape, which per #600 "is available only if
   admitted"; the only measured application shape (R1 Parquet) is
   NONE_DETECTABLE. No admitted high-frequency small-I/O workload exists yet
   (`PHASE3_VALIDATION.md` V3; #600 review E5; MISSION P4.2 gating).
5. **dududu adapter double-`fstat` confounder unfixed.** P2 open path repeats
   file_info+size `fstat` (`backend_sluice.cpp:63,75` in dududu); #600
   recorded it as dududu's defect to fix separately; no parity-corrected
   comparator arm exists yet (EVIDENCE_INDEX / DIRECT_MACHINE_COST_LEDGER E4).
6. **Platform generalization unverified.** `PLATFORM_SCOPE =
   E5_NATIVE_LINUX`; `WSL2_VALIDATION = SEPARATE_TASK`; no claim for other
   hardware/OS/workloads (`PHASE3_VALIDATION.md` V4). The WSL2 G0-403 numbers
   (different host, different harness) are historical shape references only,
   not targets.
7. **C2 residual composition cost unexplained.** After C1+C2, AX−A2 remained
   +9.7 ns (within noise); the composition state machine and residual
   admission cost were not further attributed (`PHASE3_VALIDATION.md` C2
   record). Candidate territory for P1/P2.
8. **ladder probe reproducibility — partially closed at revision, remainder
   open.** The comparator kit source and fixture generator are now preserved
   byte-exact with hashes in
   `research/direct-beyond-parity/probe-snapshot-600/`, and the 64 MiB fixture
   is regenerable deterministically (§1.4). Still open: (a) the snapshot is
   untracked — it survives only if the owner commits the mission directory;
   (b) the exact compile command of the *measured* ladder binaries remains
   unrecorded (`[UNCERTAIN]`, inferred command only); (c) the compiled
   binaries and raw series stay workspace-only under
   `~/sluice-600-direct-research/`. New P1 comparator arms must not inherit
   this pattern: they need a committed, rebuildable identity from the start.
9. **#578 R0 (fio + DuckDB) entirely pending.** All four arms
   (Native DuckDB / Native Wrapper / Sluice Direct / combined mechanism) at
   zero completion; no runner/oracle exists — `methodology.md` tool boundary
   covers only the G0 matrix (`bench/run_matrix.sh` + `check_run.py`), so new
   workloads need their own runner configuration before results count as
   evidence. DuckDB layers in P4 wait on R0 admission.
10. **Cold-cache control has no standard procedure.** G0's WSL2 cold cells
    could only `fadvise(DONTNEED)` per file and its "-cold" latency column is
    effectively warm
    (`docs/evidence/g0-403-performance-baseline-20261015/MEASUREMENT.md:156-160`);
    E5 `drop_caches` usability untested `[UNCERTAIN]`. MISSION P1.4 requires
    strict disclosure; the operational definition must be fixed before any
    cold-ish cell counts.
11. **15-pair rule not formalized.** Precedents differ: mw-hw-1 acceptance
    ratio ≤0.95 with ≥12/15 pairs; dp3a/h2b regression gate median ≤1.03 with
    ≤10/15 pairs >1.03 (true p≈0.059); #600 used 16 ABBA pairs with A/A sd
    calibration and a −5.1 ns order-bias correction internal to #600. CI
    method, order-bias handling, and thresholds are not in
    `methodology.md`; each new round must preregister its rule and cite the
    precedent it follows.
12. **CPU/op caliber not pinned.** Three coexisting calibers (G0 `cpu_s`/
    `cpu_util`; #600 `rusage` user/sys ns/op with ±30 ns tick granularity plus
    `perf instructions:u`; mw-hw-1 `cpu_ns_per_op`). MISSION P4.3's principle
    that PMU diagnostics and uninstrumented wall time are different
    measurement conditions has no operational definition yet; the
    instructions/op numbers (70→40) rest on static disassembly with perf
    corroboration, whose acceptance boundary is also not codified.
13. **E5 ladder coverage is read-path only.** The single E5 A0/A1/A2/AX ladder
    covers `read_at`/`read_exact_at`; no E5 ladder exists for write paths or
    metadata paths (open/info/size/close) — P1 must extend the ladder before
    claiming anything about those paths.
14. **Direct-specific mutation seam inventory unverified.** The ledger's 19
    registered mutation targets are mostly async/request-side seams
    (`docs/roadmap/v2-conformance.md:434-437`); whether a separate registered
    seam list exists for the Direct path (beyond the three C1 admission
    mutants) was not surveyed in P0.

## 5. Must-preserve semantics and test oracles

This is the non-negotiable floor for P2/P3 candidates (MISSION P2.3): any
candidate that lifts Direct cost must keep every behavior below observable,
and must pass `BASE GREEN → MUTANT RED → RESTORED GREEN` with mutants killed
by named assertions, never by crash/build failure/timeout. Contract anchors
are v2-r1 requirement IDs in `docs/explicit-io-contract.md` (SEM-03 heading
verified at `docs/explicit-io-contract.md:398` `[RUN-THIS-SESSION]`; finer
line anchors are from the P0 inventory).

### 5.1 Semantic obligations and their oracles

| Obligation | Contract anchor | Primary oracles (named assertions) |
|---|---|---|
| Closed-file precedence: closed → `invalid_state` before access/range checks | SEM-03 step 1; V01 | `tests/semantic_validation_precedence_test.cpp` (21 frozen scenarios, `tests/semantic_scenarios.hpp:28-83`); `tests/file_access_precedence_test.cpp` (16 `blocking_*`); `tests/semantic_reference_case_test.cpp` `v01_closed_file_with_zero_buffer_is_invalid_state` |
| Access-mode legality table; closed > access > offset precedence | SEM-03 steps 1–2 | `tests/file_access_precedence_test.cpp` (8 access + 4 priority scenarios); `access_illegal_*` scenarios; `tests/blocking_file_state_test.cpp` `resize_on_read_only_reports_invalid_argument`; `v02` |
| Offset/length/overflow: no wrap/truncate; native-invalid range → `invalid_argument`; stricter limits → `not_supported` | SEM-04 | `tests/semantic_range_test.cpp` (`range_table_holds`, `no_wrap_at_the_top_of_the_range`, `zero_length_has_no_range`, `checked_offset_reports_invalid_argument`, `native_transfer_limit_is_a_range_rejection`, `canonical_positional_io_reports_invalid_argument`); `tests/blocking_file_read_test.cpp` `read_with_illegal_offset_reports_invalid_argument` (B1: `tests/blocking_file_read_test.cpp:122` `[RUN-THIS-SESSION]`); `tests/blocking_file_write_test.cpp` `write_with_illegal_offset_reports_invalid_argument`; `tests/blocking_file_state_test.cpp` `resize_overflow_reports_invalid_argument` (B1: `:320`) |
| Zero-length = logical no-op, skips offset/range checks, no OS call | SEM-03 step 3 | `tests/direct_composition_fault_test.cpp` `zero_length_requests_make_no_native_call` (fault script injects EIO, proving no syscall); `read_empty_buffer_returns_zero_without_operation`; `sequential_zero_length_read/write_succeeds`; `blocking_read_at_empty_buffer_ignores_unrepresentable_offset`; `tests/explicit_file_ref_test.cpp` `zero_length_op_succeeds_before_offset_validation` |
| Null buffer = empty span → zero-length no-op; still `invalid_state` on closed file | SEM-03 step 3; V01 form | `blocking_*_closed_with_empty_buffer_reports_invalid_state` (4, all pass `std::span{}`); `blocking_*_on_*_with_empty_buffer_reports_invalid_argument` (4) |
| EINTR: retry only without a completed count; never replay confirmed bytes | SEM-05; ERR-01 | `tests/direct_composition_fault_test.cpp` `primitive_retries_eintr_without_a_completed_count` (script `{{-1,EINTR},{4,0}}`), `primitive_write_retries_eintr_then_stops_at_the_first_count`; `tests/semantic_errno_mapping_test.cpp` `EINTR_is_interrupted` |
| Partial read/write: 0<n≤requested is success; primitive does not loop | SEM-05 table | `short_read_reports_only_available_bytes`; `write_primitive_result_does_not_exceed_request`; `short_reads_reach_full_progress`, `primitive_short_write_returns_without_retry_to_full`; `tests/semantic_short_io_reference_test.cpp` frozen table |
| EOF: read 0 at observation point is success, not error; `eof_before_full` keeps prefix | SEM-05 | `read_past_end_returns_zero`; `sequential_read_at_eof_returns_success_zero`; `tests/direct_composition_test.cpp` `read_exact_reports_eof_before_full_with_the_prefix` |
| No-progress: `write_all` stops on zero progress; never spins | SEM-05 | `zero_progress_write_stops_after_one_attempt`; `write_all_zero_progress_stops_and_keeps_its_prefix`, `composition_never_spins_on_zero_progress` |
| Confirmed bytes: results distinguish confirmed progress from error; prefix survives failure | ERR-02; SEM-05 | `composition_advances_the_buffer_by_confirmed_bytes`, `error_after_confirmed_prefix_keeps_the_prefix`; `tests/semantic_effect_outcome_test.cpp` `case_b_confirmed_prefix_survives_an_error`, `success_is_exact_progress` |
| Effect certainty: uncounted physical attempts report unknown, not 0; no rollback promise | ERR-02 | `tests/semantic_effect_outcome_test.cpp` (`case_*_unknown_remaining_effect`, `failed_dispatched_attempt_is_unknown_in_both_directions`, `pre_execution_cancel_reports_known_zero`); `tests/direct_composition_fault_test.cpp` `a_primitive_error_reports_an_unaccounted_remainder_in_both_directions`, `every_stop_reason_is_pinned_against_the_oracle_error_rule`; `v15_unknown_effect_write_failure` (/dev/full); `CompositionOutcome` carries `EffectCertainty` (`include/sluice/blocking/file.hpp:40-48`) |
| File::open legality table (3 independent choices; NUL rejection; close-on-exec + 0644 via umask) | SEM-02 | `tests/file_open_contract_test.cpp` (36 exhaustive scenarios); `tests/semantic_open_test.cpp` frozen 18-combo table + `embedded_nul_is_rejected_before_native_open`, `embedded_nul_leaves_target_untouched`; `tests/file_resource_test.cpp` |
| File::close: first native close attempt consumes ownership (also on EINTR/error); closed/moved-from close is a no-op; destructor best-effort, unreportable | SEM-02 | `tests/file_close_semantics_test.cpp` (all 7, incl. `close_eintr_consumes_ownership_without_retry`, `consumed_close_never_retries_a_reused_descriptor`, `close_on_closed_file_is_not_a_native_call`) |
| Shared cursor: Linux open-file-description cursor for read/write; positional ops never move it; hidden seek+read/write illegal | SEM-04; SEM-07 | `tests/direct_cursor_position_test.cpp` (all 5, incl. `shared_cursor_is_shared_through_native_duplication`, `resize_does_not_move_the_shared_cursor`); `positional_operations_do_not_move_shared_position`; `tests/file_info_identity_test.cpp` `metadata_and_positional_io_do_not_move_the_shared_cursor` |
| Composition stop conditions (full/eof/zero-progress/error) with separate reporting; rejections follow the shared precedence table | SEM-05; SEM-03 | `tests/direct_composition_test.cpp` (all 7, incl. `composition_rejections_follow_the_shared_precedence`); `composition_surfaces_obey_the_precedence_table` |
| file_info/size: explicit identity availability; observation-time size; closed → `invalid_state` | SEM-07 | `tests/file_info_identity_test.cpp`; `tests/blocking_file_state_test.cpp` (`size_*`, incl. `size_on_read_only_file_succeeds`/`size_on_write_only_file_succeeds`) |
| resize: read_only → `invalid_argument`; grow exposes zero-fill, shrink discards; cursor unmoved | SEM-07; SEM-03/04 | `tests/blocking_file_state_test.cpp` (`resize_grow_zero_fills` … `resize_on_write_only_file_succeeds`) |
| sync_data/sync_all: coverage of completed writes (and completed resize on Linux), not submitted-only; failure = no guarantee, never fabricated success | SEM-06 | `tests/blocking_file_sync_data_test.cpp`, `tests/blocking_file_sync_all_test.cpp`; `tests/semantic_durability_reference_test.cpp` (`v16_submitted_write_is_not_covered`, `v27_completed_resize_is_covered`, `v17_coverage_is_not_a_snapshot`, `failed_sync_covers_nothing_and_metadata_needs_sync_all`) |
| Allowed concurrency: caller serializes move/close/destroy; overlapping writes and concurrent resize get no extra atomicity; disjoint buffers order nothing; owner move legal if resource continuity holds | THREAD-01; SEM-04; LIFE-01 | `concurrent_sequential_reads_partition_content`; `shared_cursor_is_shared_through_native_duplication`; `tests/file_move_borrow_test.cpp` `moved_owner_keeps_outstanding_borrow_settling`, `second_submission_through_moved_owner_settles`; `moved_file_owns_nothing_and_target_keeps_resource` |
| W-01 Direct independence: no IoContext/worker/request table/progress source required | W-01; INV-01 | `tests/direct_w01_consumer_probe.cpp` (static_asserts the direct headers pull no AsyncIoContext/Backend/Scheduler/Fiber/Runtime, then drives the full W-01 sequence); cited by ledger G1 (`docs/roadmap/v2-conformance.md:49`) |

### 5.2 Cross-path frozen oracles — design intent vs P0-executed evidence

The shared-semantic suites are *built* to compare the frozen scenario tables
(`tests/semantic_scenarios.hpp:28-83`, 21 scenarios) across the
direct / ThreadPool / io_uring paths:
`tests/semantic_validation_precedence_test.cpp` (direct + ThreadPool halves)
and `tests/semantic_backend_conformance_test.cpp` (uring half), plus the
shared tables in `semantic_short_io_reference_test.cpp`,
`semantic_range_test.cpp`, `semantic_effect_outcome_test.cpp`,
`semantic_errno_mapping_test.cpp` (14 errno mappings).

What was actually executed in P0 (`[RUN-P0]`, output in §5.3) covers **only
part of that design**: `semantic_validation_precedence_test` compared direct
21/21 (0 divergences — this is the Direct-side floor and it is executed),
ThreadPool 19/21 (2 scenarios not drivable on that path), and **io_uring
0/21 compared**; the uring-side `semantic_backend_conformance_test` was
**not run in P0 at all**. The ledger's "0 recorded divergences" for the
three-path comparison (`docs/roadmap/v2-conformance.md` G1)
`[TRANSCRIBED]` is the recorded historical state, not a P0 re-execution.

Consequences for P1+ readers:

1. A Direct-side semantic drift **is** caught by executed P0 evidence (the
   direct 21/21 comparison).
2. Do **not** claim from this record that io_uring-side parity has an
   executed P0 oracle behind it — that protection is currently
   ledger-carried; run `semantic_backend_conformance_test` before relying on
   it at any decision point.

### 5.3 Mutation discipline at B1 and what P0 actually ran

- The C1 restructure carries three named admission mutant seams at B1,
  re-verified this session `[RUN-THIS-SESSION]`:
  `SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED` and
  `SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE` embedded in `precheck_data_op`, and
  `SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE` in `precheck_resize`
  (`include/sluice/detail/file_semantics.hpp:158,168,192` at B1); xmake
  targets `blocking_file_mut_admit_invalid_range` /
  `blocking_file_mut_empty_before_closed` /
  `blocking_file_mut_admit_unrepresentable_size` are declared via
  `direct_mutant_target()` (`xmake/tests.lua:81-97` at B1) and are documented
  as executed and recorded manually, never as regular tests.
- Killer assertions exist at B1 `[RUN-THIS-SESSION]`:
  `read_with_illegal_offset_reports_invalid_argument`
  (`tests/blocking_file_read_test.cpp:122`, registered at `:183`),
  `sequential_read_on_write_only_file_rejected_upfront`
  (`tests/blocking_file_sequential_test.cpp:392`, registered at `:600`),
  `resize_overflow_reports_invalid_argument`
  (`tests/blocking_file_state_test.cpp:320`, registered at `:406`).
- Oracles actually executed during P0 evidence collection `[RUN-P0]`:
  `xmake build -y direct_composition_fault_test && xmake run
  direct_composition_fault_test` → "all 14 direct composition fault-injection
  tests passed"; `xmake run file_access_precedence_test` → "all 36 … passed";
  `xmake run semantic_validation_precedence_test` → "21 precedence scenarios:
  direct 21 compared, 0 not expressible; ThreadPool 19 compared, 2 not
  drivable; io_uring 0 compared; 0 recorded request-side divergences". The
  full 58/58 four-configuration matrix and the mutation kills were **not**
  re-run in P0 (§3.6); they are prerequisites at the next merge-gating
  boundary, not P0 deliverables.

### 5.4 Caveats for later readers

1. `tests/semantic_reference_case_test.cpp` prints DEFERRED for the v15/v16
   request-side effect-certainty channel, referencing historical issue #400;
   the Direct side is fully covered. This DEFERRED is request-side legacy,
   not a Direct gap — do not misread it as missing Direct evidence.
2. `checked_offset_reports_invalid_argument` moved from a helper-level unit
   test to the admission-verdict boundary; both forms were read this session
   `[RUN-THIS-SESSION]`. On B0
   (`tests/semantic_range_test.cpp:92-105`) it calls the (now deleted)
   helper directly: `checked_posix_offset(kMaxNativeOffset)` must accept with
   value == `off_t` max, and `checked_posix_offset(kMaxNativeOffset + 1)`
   must reject with `invalid_argument`. On B1
   (`tests/semantic_range_test.cpp:91-104` at `77e5399a`) it constructs a
   `sluice::detail::DataOpRequest{closed=false, read_only, read,
   offset=kMaxNativeOffset+1, size=1}` and asserts `precheck_data_op` returns
   `reject_range`, and `rejection_of(verdict)` maps it to
   `invalid_argument`. The observable rule — one-past-native-max offset →
   `invalid_argument` — is unchanged; only the tested seam moved, because C1
   deleted the helper. Public-path coverage of the same rule remains in the
   same suite on both baselines via
   `canonical_positional_io_reports_invalid_argument`. When citing the
   assertion, name the baseline.
3. The frozen evidence numbers in §3 belong to the #600 measurement identity
   (`ea2fc4fc`, libraries hashed in §1); new measurements in P1+ must not
   silently mix B0/B1 artifacts — each claim names its library sha256.
