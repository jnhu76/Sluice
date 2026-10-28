# P5 Interaction Plan — Multi-Candidate Combination Matrix (Preregistered)

- Mission: `research/direct-beyond-parity/MISSION.md` §7 (P5 — Integration and
  Independent Review; interaction matrix §7.1, independent correctness review
  §7.2/P5.2, independent performance review §7.3/P5.3). The combined
  measurement this plan owns was assigned here by
  `P2_CANDIDATE_P2C1.md` §12.6 (MISSION P2.2 precondition satisfied: both
  single factors KEEP) and by `P4_WORKLOAD_COMPETITION_PLAN.md` §0 rule 1 /
  §7.3 rule 6 (no P2C1+P2C2 stacked cell exists in P4; P5 owns it; independent
  gains are never added — MISSION.md:404).
- Preregistration date: 2026-10-28. Primary platform: E5 Native Linux.
- **Status of this document: PREREGISTERED.** The A+B branch recipe (§2), the
  static/correctness gates (§4 S0–S1), every matrix cell, arm identity,
  pairing count, statistic, caliber, verdict rule and stacking adjudication
  below are fixed **before** any P5 branch, build or measurement runs.
  Mid-campaign deviations become numbered amendments with
  trigger/change/reason — never silently applied (precedents:
  `P1_MEASUREMENT_PLAN.md` Amendment 1; `P4_WORKLOAD_COMPETITION_PLAN.md`
  Amendment 1; `P2_CANDIDATE_P2C1.md` §12 preregistered-disposition
  discipline).
- Inputs read for this plan: `MISSION.md` §4/§7/§11;
  `P0_BASELINE_AND_OWNERSHIP.md` (§1 baselines/ownership, §5 semantic floor);
  `P1_MEASUREMENT_PLAN.md` (§3 environment pin, §5 statistics, §6 calibers,
  §7 cold discipline, §8 matrix commands, §9 oracle gates, Amendment 1);
  `P2_EXPERIMENT_PLAN.md` (§1 admission scope pins, §2 cross-candidate
  protocol, §2.4 measurement units + pinned MDEs, §3.3/§4.3 objdump gates and
  recount bounds, §3.5/§4.5 pass rules);
  `P2_CANDIDATE_P2C1.md` (KEEP §12.5, caps §12.4, P5 inputs §2.6/§12.6);
  `P2_CANDIDATE_P2C2.md` (KEEP instruction-caliber §9, P5 inputs §2.7);
  `P4_WORKLOAD_COMPETITION_PLAN.md` (§0 candidate set, Amendment 1
  common-kit-source clause, §6 statistics, §7 verdict rules);
  `REAL_WORKLOAD_COMPETITION.md` (P4 measured outcomes: P2C1
  `WIN_CONFIRMED` ×3 composition cells at fresh MDE′, seven shapes
  `NO_CLAIM`, negative controls `PARITY`; P2C2 wall `NO_CLAIM` with the
  instruction caliber retained; PARQUET workload-scale `PARITY` ×2).

## 0. Candidate set and fixed rules

| Arm | Identity (re-verified read-only this session, §1) | Evidence status carried in |
|---|---|---|
| **BASE (B1)** | frozen archive `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0`, worktree `/home/jnhu/Source/Sluice-600-wt` HEAD `77e5399a45998dea396699fa2e063c94a665945a`, clean | P0 §1.3 anchor |
| **A = P2C1** (read-composition prevalidated register-resident steps) | branch `direct-read-compose-prevalidated-steps` (local-only), HEAD `deaf03014173e13633a43f4db405c9a9e9860f23`, release archive `07d06803…2049e` | KEEP (P2C1 §12.5): wall −16.565 ns/op @ `read_exact_at` 4 KiB seq CI-excluding; claim capped "measurable improvement with re-attributed residual"; P4 load re-confirmation `WIN_CONFIRMED` ×3 (comp-read −19.048, eofmix −15.907, cursor −15.130 ns/op, all ≥ fresh MDE′ 12.414) |
| **B = P2C2** (write-composition exit staging) | branch `direct-write-compose-exit-staging` (local-only), HEAD `476bd9ce9780594163350228e066a86b554c157d`, release archive `67251028…20a807` | KEEP — instruction caliber only (P2C2 §9): static bounds met, PMU +64.076 → +44.030 instr/op (drop ≥ 20, margin 0.046 flagged); no wall claim; P4 wall `NO_CLAIM` confirmed with PMU −20 retained |
| **A+B** (integration candidate) | branch `direct-compose-stack-readpreval-writeexit` (new; §2 recipe), release archive sha256 recorded at build | none yet — this plan's subject |

Rules fixed now:

1. **Matrix scope.** The interaction matrix is exactly `BASE / A / B / A+B /
   negative controls` (MISSION.md:741). P3 admitted no product mechanism
   (AC-1/2/3 remain Awaiting Owner; every P4 Layer C strategy arm measured
   `REGRESSION`), so no third factor exists to stack.
2. **Single factors stand on their own evidence.** P5 wall cells never
   re-adjudicate the frozen P2/P4 single-factor verdicts; the fresh single-arm
   cells here (§3 group 3) exist for same-session interaction attribution and
   reproduction context only. No cross-session numeric comparability is
   claimed (P4 §6 item 8 discipline).
3. **Independent gains are never added** (MISSION.md:404). Interaction terms
   are recorded as attribution, never as claims (§4 S6).
4. **No new mechanism enters P5.** Integration only. Any further optimization
   discovered mid-P5 is out of scope and becomes a new preregistered
   candidate.
5. **Public-surface pin.** Public `read_at`/`read`/`write_at`/`write` bodies,
   public semantics, `Result`/`CompositionOutcome` ABI, admission precedence,
   EINTR retry-without-count, the stop set, effect certainty and the
   confirmed-prefix contract remain untouched by the stack (P2 plan §1 scope
   pins, inherited by both factors).
6. **Ownership.** The A+B branch is local-only, never pushed by the research
   run, never inside the frozen worktrees or the main workspace; the frozen
   worktrees are linked read-only (P0 §1.3; P2 plan §2.2). No merge to any
   upstream branch; P5 ends at OPEN state (MISSION §1: no auto-merge).

## 1. Asset inventory (read-only, executed 2026-10-28 before this plan)

Every check below was run in this session on E5; command classes and results
are the preregistration evidence. Legend as P0 §0: `[RUN]` executed this
session.

- Branches present, local-only `[RUN]` (`git branch --list 'direct-*'`):
  `direct-cost-ladder`, `direct-read-compose-prevalidated-steps`,
  `direct-write-compose-exit-staging`. Worktree HEADs `[RUN]`
  (`git -C <wt> rev-parse HEAD`): P2C1 `deaf0301…`, P2C2 `476bd9ce…`, B1
  `77e5399a…` — all equal the P2/P4 records.
- Archive identities re-hashed `[RUN]` (`sha256sum`): B1
  `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0`; P2C1
  `07d0680307c71cc412dfd021fd47c3098de57b580283df189f66dcdc2052049e`
  (= `p2-raw/P2C1/matrix-release-archive-sha256.txt`); P2C2
  `672510281abadba1cbda0a0e89f15e1dbb2c69f5a15bceb306b70196b420a807`
  (= `p2-raw/P2C2/matrix-release-archive-sha256.txt`).
- Kit `[RUN]`: kit source commit `2364be4a` exists (`git cat-file -t` →
  commit); binaries at
  `/home/jnhu/Source/Sluice-direct-cost-ladder/research/direct-cost-ladder/`:
  `ladder` `89450367…010e8` (the P4-Amendment-1 common-source rebuild,
  byte-exact the P2 session's B1-linked binary), `ladder-c1`
  `8305d22c…0381`, `ladder-c2` `8bb6473b…0065` — all equal
  `P4_WORKLOAD_COMPETITION_PLAN.md` §3.1 / `REAL_WORKLOAD_COMPETITION.md`
  identity records. `paired.sh`, `paired2.sh` (two-binary ABBA), and
  `analyze_pairs.py` present and executable.
- **Merge surface measured, not guessed** `[RUN]`
  (`git merge-tree $(git merge-base …) direct-read-compose-prevalidated-steps
  direct-write-compose-exit-staging`; merge base = B1 `77e5399a`): the
  trivial merge emits conflict markers in exactly two files —
  `src/blocking_file.cpp` (changed in both, 2 conflict hunks) and
  `xmake/tests.lua` (changed in both, 1 conflict hunk);
  `tests/direct_composition_fault_test.cpp` is changed in both but carries
  no conflict marker in the preview (both branches add the same-named
  killer — the §2 R3 dedupe rule still applies, since trivially merged
  duplicate additions would break the build, not merge semantics); the
  P2C2-only gate scripts (`p2-scripts/P2C2-{matrix,mutants}.sh`)
  merge clean ("added in remote"). The conflict-resolution rules in §2 are
  pinned against this measured surface.
- **Seam-set union measured** `[RUN]` (`grep -rhoE
  'SLUICE_(COMPOSE|DIRECT)_MUTANT_[A-Z_]+' <worktree>/{src,include} | sort
  -u` per branch): P2C1 defines the four compose seams
  (`ADVANCE_BY_REQUESTED`, `RETRY_ZERO_PROGRESS`, `FAILURE_ACCOUNTED`,
  `SKIP_IMPOSSIBLE_COUNT`) **plus** the two step seams
  (`STEP_REPORTS_FULL_COUNT`, `STEP_SKIPS_EINTR_RETRY`); P2C2 defines exactly
  the same four compose seams. Union = 6 seam defines; both branches carry
  the named killer `write_all_impossible_count_stops_immediately` (3
  occurrences each — the §2 dedupe rule).
- Warm fixture and cold fixtures per P1 §3 / P4 §3.1 (`$READ_FIX` pinned sha
  `bc020fd1…b2993a` regenerated by `mkfixture` when absent; btrfs
  `/var/tmp/sluice600-p1/{cold64m,coldwrite64m}.bin` persistent).

## 2. A+B branch construction (preregistered recipe)

- Branch `direct-compose-stack-readpreval-writeexit`, cut from B1
  `77e5399a`; worktree
  `/home/jnhu/Source/Sluice-direct-compose-stack-readpreval-writeexit`
  (fresh worktree, never a frozen one, never the main workspace).
- **Integration shape:** merge commit of P2C2 `476bd9ce` into P2C1
  `deaf0301` (merge base B1). Narrow commit series, nothing rides along:
  (1) the merge commit with the §2 conflict resolutions; (2) gate scripts
  `research/direct-beyond-parity/p5-scripts/P5AB-matrix.sh` and
  `P5AB-mutants.sh` (P2 plan §2.5 conventions; the mutants script records
  tree head **and** timestamps — closing the two disclosed gate-script
  defects: P2C1's fixed-filename overwrites, P2C2's missing tree head).
- **Conflict-resolution rules (pinned against the §1 measured surface):**
  - R1 `src/blocking_file.cpp` — read-side compose functions
    (`read_exact_at`/`read_exact`) resolve to **P2C1's end state**
    (prevalidated step primitive + register-resident state + single-point
    exit materialization); write-side compose functions
    (`write_all_at`/`write_all`) resolve to the **semantic union**:
    exit-only staging with confirmed/total register-carried, outcome
    materialized once per exit flavor, **steps remaining public
    `write_at`/`write` with per-step re-tests retained** (both factors' §1
    scope pins: P2C1 "write-side step primitives remain public
    `write_at`/`write"; P2C2 "per-step closed/access re-tests retained").
    The four shared seam guards land inside the merged production loop (not
    only in the oracle rule functions — P2C1 §4 two-stage placement note).
  - R2 seam defines = the §1 union (6); dormant in BASE builds (zero BASE
    code difference from the mechanism itself); `xmake/tests.lua` mutant
    targets = union with name dedupe.
  - R3 `tests/direct_composition_fault_test.cpp` — the shared named killer
    `write_all_impossible_count_stops_immediately` is deduplicated (one
    definition, both branches' copies are byte-identical in intent; any
    textual divergence resolves to the assertion-named behavior, never a
    weaker script).
  - R4 impossible-count guards retained on all four compose back-edges
    (oracle-pinned; P2 plan §1).
  - R5 `include/sluice/detail/file_semantics.hpp` takes P2C1's version
    (P2C2 does not touch it; merges clean).
- **Build and binary identity:** release build via the P1 §8 matrix commands
  (`xmake f -c -m release --liburing=n -y && xmake -y`); release archive
  sha256 recorded in `p5-raw/P5AB/matrix-release-archive-sha256.txt`. Kit
  binary `ladder-ab` built from the **one kit source commit `2364be4a`**
  (common-kit-source clause, P4 Amendment 1) by the kit's `build.sh` against
  the stacked worktree's include + release archive; its sha256 recorded in
  `p5-raw/identity.txt` before any cell. Any future kit-source change
  requires rebuilding **all four** binaries plus a numbered amendment.
- **Execution order** (P2 plan §2.6 analogue): (1) merge + gate scripts;
  (2) `P5AB-mutants.sh` exit 0 (union seam set; BASE GREEN → MUTANT RED with
  the **named assertion** → RESTORED GREEN; crash/build-failure/timeout is a
  gate failure, not a kill); (3) `P5AB-matrix.sh` exit 0 (four configs, 0
  failed; P1 §8 exact commands); (4) static gates of §4 S1 on the release
  archive; (5) kit smoke gate across all four binaries
  (`p5-raw/kit-smoke.txt`: read equality group {a0,a1,a2,a1x,ax,a1c,ac} over
  `ladder`/`ladder-c1`/`ladder-ab`; write group {w0,w1,w2,w1x,wx,w1c,wc}
  with `content_ok=1` over `ladder`/`ladder-c2`/`ladder-ab`; metadata ×3,
  closed group, EINTR diagnostics — "ALL GATED GROUPS IDENTICAL"); (6) the
  §3 cells. A red suite at any step stops measurement (MISSION §9).

## 3. Interaction-matrix cells (preregistered roster)

Common: 16 ABBA pairs/cell (≥15 mission minimum, 16 pinned to the #600
precedent), one process invocation per arm sample, arm order alternating per
pair, `taskset -c 2`, warm tmpfs unless stated, strictly serial cells on a
quiescent host. Cross-binary cells use `paired2.sh <candidate-binary>
<ladder> <arm> …` (candidate − baseline per-pair statistic); within-binary
cells use `paired.sh`. 4 KiB shape `4096 1024 1024 5 16`; 1 B shape
`1 4096 4096 5 16`. `$READ_FIX=/tmp/sluice600/data64m.bin`; every warm write
cell uses an Amendment-1 per-cell scratch `/tmp/sluice600/p5-<cellid>.bin`
(mkfixture before, `lsof` isolation check, deleted after — P1 Amendment 1 /
P2 §2.4 addition 5). Wall comes exclusively from uninstrumented runs; PMU and
strace cells run separately (§5).

Group 1 — A/A floors (first, serially exclusive, per binary and track; one
§11.10-style 45 s quiesce rerun per failed cell, rerun value stands):

| id | command core | floor for |
|---|---|---|
| `p5-aa-read-4k-b1` | `paired.sh ladder a1 a1 4096 1024 1024 5 16 $READ_FIX` | read track, B1 side |
| `p5-aa-read-4k-c1` | same on `ladder-c1` | cross-binary read floor (max-of-pair, P4 convention) |
| `p5-aa-read-4k-ab` | same on `ladder-ab` | cross-binary read floor |
| `p5-aa-write-4k-b1` | `paired.sh ladder w1 w1 4096 1024 1024 5 16 <scratch>` | write track |
| `p5-aa-write-4k-c2` | same on `ladder-c2` | cross-binary write floor |
| `p5-aa-write-4k-ab` | same on `ladder-ab` | cross-binary write floor |
| `p5-aa-cold` | cold A/A loop (P1 §4.5 exact `run_cold` shape, `a1 a1`, btrfs `$COLD_FIX`, drop before every invocation, reps=1) | cold track |

Group 2 — BASE arm (fresh same-session mechanism-excess anchors; recorded,
verdicts per the P1 rule):

| id | pairing | statistic |
|---|---|---|
| `p5-base-composition-4k` | `a1x` vs `ax` @ `ladder` | **S** |
| `p5-base-eofmix-4k` | `a1x` vs `ax` tail @ `ladder` | **S_eof** |
| `p5-base-cursor-composition-4k` | `a1c` vs `ac` @ `ladder` | **S_cur** |
| `p5-base-wcomp-4k` | `w1x` vs `wx` @ `ladder` (scratch) | **S_w**, P_base_w |

Group 3 — single-factor arms (fresh references for same-session interaction
attribution; **no new claim attaches to these cells** — the frozen P2/P4
verdicts stand; a fresh reference contradicting the frozen evidence is
recorded as an environment alarm and routed to the §7 performance review):

| id | pairing | statistic |
|---|---|---|
| `p5-delta-comp-read-4k-a` | `paired2.sh ladder-c1 ladder ax …` | **Δ_A** |
| `p5-delta-comp-eofmix-4k-a` | same, pattern `tail` | **Δ_A_eof** |
| `p5-delta-comp-cursor-4k-a` | `paired2.sh ladder-c1 ladder ac …` | **Δ_A_cur** |
| `p5-delta-wcomp-4k-b` | `paired2.sh ladder-c2 ladder wx …` (scratch) | **Δ_B_w** (wall record; expected `NO_CLAIM`, P2C2 §6.2/P4 precedent) |

Group 4 — A+B claim units on the stacked binary (the ask's retest set; the
primary stacking carriers):

| id | pairing | statistic / rule |
|---|---|---|
| `p5-delta-comp-read-4k-ab` | `paired2.sh ladder-ab ladder ax …` | **Δ_AB** — S2 |
| `p5-delta-comp-eofmix-4k-ab` | same, `tail` | **Δ_AB_eof** — S2 |
| `p5-delta-comp-cursor-4k-ab` | `paired2.sh ladder-ab ladder ac …` | **Δ_AB_cur** — S2 |
| `p5-delta-wcomp-4k-ab` | `paired2.sh ladder-ab ladder wx …` (scratch) | **Δ_AB_w** — wall recorded, must not be EXCESS (S3); instruction caliber adjudicated by `p5-pmu-ab-wcomp-4k` |

Group 5 — A+B within-binary endpoints (P1 EXCESS/PARITY rule; S6):

| id | pairing | statistic |
|---|---|---|
| `p5-ab-composition-4k` | `a1x` vs `ax` @ `ladder-ab` | **D_ab** |
| `p5-ab-eofmix-4k` | `a1x` vs `ax` tail @ `ladder-ab` | **D_ab_eof** |
| `p5-ab-cursor-composition-4k` | `a1c` vs `ac` @ `ladder-ab` | **D_ab_cur** |
| `p5-ab-wcomp-4k` | `w1x` vs `wx` @ `ladder-ab` (scratch) | **D_ab_w** |

Group 6 — negative controls on the stacked binary (mandatory; S4):

| id | pairing |
|---|---|
| `p5-ab-negread-4k` | `a1` vs `a2` @ `ladder-ab` |
| `p5-ab-negread-1b` | `a1` vs `a2` 1 B @ `ladder-ab` |
| `p5-ab-negwrite-4k` | `w1` vs `w2` @ `ladder-ab` (scratch) |
| `p5-ab-negwrite-1b` | `w1` vs `w2` 1 B @ `ladder-ab` (scratch) |

Group 7 — cross-payload regression face (cross-binary `ladder-ab` vs
`ladder`; expectation `PARITY`/`NO_CLAIM` — the public-op/metadata paths are
machine-identical by S1's sha-equal gate, so this prediction is binding per
P4 §7.3 rule 4; any `EXCESS` = `REGRESSION` per S5):

| id | pairing | payload covered |
|---|---|---|
| `p5-reg-readat-4k` | `paired2.sh ladder-ab ladder a2 4096 …` | public single-op `read_at` |
| `p5-reg-writeat-4k` | `paired2.sh ladder-ab ladder w2 4096 …` (scratch) | public single-op `write_at` |
| `p5-reg-meta-oc` | `paired2.sh ladder-ab ladder m2oc 0 2048 512 5 16 $READ_FIX` | metadata `open`+`close` |
| `p5-reg-meta-fi` | `paired2.sh ladder-ab ladder m2fi 0 16384 2048 5 16 $READ_FIX` | metadata `file_info` |
| `p5-reg-cold-4k` | cold ABBA loop (P1 §4.5 `run_cold` shape), `a2` cross-binary, btrfs `$COLD_FIX`, drop per invocation, reps=1, ops=16384 | one cold btrfs unit (bounds the claim; expected `NO_CLAIM`, cold floor ≫ any composition-internal effect) |

Group 8 — diagnostic calibers (separate runs, **never wall sources**; §5
separation rule):

| id | content |
|---|---|
| `p5-pmu-base-composition-4k` / `p5-pmu-ab-composition-4k` | `perf stat -e instructions:u` per arm (`a1x`,`ax`) on `ladder` / `ladder-ab` → **P_base** / **P_ab** (read-side retention corroboration, non-gating, reported either way) |
| `p5-pmu-base-wcomp-4k` / `p5-pmu-ab-wcomp-4k` | same per arm (`w1x`,`wx`) → **P_base_w** / **P_ab_w** — **gating for S3** |
| `p5-strace-*` | `strace -c` integral syscall counts (≥3 exact repeats) on every group-4 cell, the group-6 controls, and the group-7 warm face cells: syscall parity + the user/kernel seesaw evidence (§6 row 4) |

Layer A/B context arms (`a0`,`w0`,`m0oc`,`m0fi`; Layer C collapses onto
Layer B at every single-span kit shape — P4 §2 structural fact) are recorded
unpaired into the same cell files (P1 §4 convention).

Minimum coverage (P4 §7.4 analogue): the §3 roster is complete when every
group has run or carries `NOT_RUN_WITH_REASON`; group 4, 6 and 7 are
non-waivable; group 3 cells may be waived only by numbered amendment
(the stack adjudication then runs on the pinned lines without same-session
single-factor attribution, disclosed).

## 4. Stacking adjudication rules (preregistered; no post-hoc reinterpretation)

Verdict vocabulary per cell: as P4 §7.2 (`WIN_CONFIRMED` / `PARITY` /
`REGRESSION` / `NO_CLAIM` / `NOT_RUN_WITH_REASON` /
`NOT_SUPPORTABLE_WITH_REASON`), with EXCESS in the P1 §5.5 sense (CI excludes
0 AND |median| ≥ max(MDE_track, 1%·N1 median) in the unfavorable direction).
Stack verdicts: `STACK_ADMITTED` / `STACK_LOSS(unit)` /
`STACK_REGRESSION` / `BLOCKED_CORRECTNESS` / `INCONCLUSIVE` /
`NOT_RUN_WITH_REASON`.

- **S0 correctness precondition (gating).** On the stacked tree:
  `P5AB-mutants.sh` exit 0 over the union seam set (named killers exactly the
  single-factor sets: the four compose seams' killers from P2 plan §2.1 plus
  P2C1's two step-seam killers, plus P2C2's rule-suite-under-mutant control);
  `P5AB-matrix.sh` exit 0 (four configs, 0 failed). The stacked tree
  re-records the authority-contraction audit: **no new runtime state**
  (both factors' audits recorded none; `direct_outcome`/`publish_certainty`
  absent), oracle rule table (`detail::compose_progress` & co.) retained as
  the non-authoritative SEM-05 oracle. Any red → `BLOCKED_CORRECTNESS`, no
  wall cell runs (MISSION §9). Lifecycle/error-semantics adjudication is the
  §7 correctness review's, not a wall number's.
- **S1 static retention (gating, machine level).** On the A+B release
  archive vs the frozen B1 archive (same-session `[RUN]`-caliber checks,
  archived under `p5-raw/P5AB/static/`):
  1. Read side: P2C1 §3.3 objdump gates M1/M2/M3 and recount bounds
     `read_exact_at` 1-step ≤ 75 / per-extra-step ≤ 26; `read_exact` ≤ 60 /
     ≤ 21 (B1 references 100/44, 82/34).
  2. Write side: P2C2 §4.3 gates W-M1/W-M2 and bounds `write_all_at` 1-step
     ≤ 93 / per-extra-step ≤ 37; `write_all` ≤ 77 / ≤ 30 (B1 101/44, 82/34).
  3. Public ops: `read_at` 39 / `read` 29 / `write_at` 40 (audit; 39 session
     caliber — the ±1 endpoint-nop caliber note, P2C2 §2.1) / `write` 29 ±0,
     **and** address-stripped instruction streams of the four public ops
     sha-equal to B1's (the machine-level public-op regression exclusion).
  4. Impossible-count guards present on all four compose back-edges.
  5. **Audit-convention recount re-executed on all four archives** (B1, C1,
     C2, AB) with anchors reproducing (B1 100/44, 82/34 read and 101/44,
     82/34 write; C1 68/25, 55/21; C2 81/35, 62/25) — this simultaneously
     closes P2C2 §2.7's and re-verifies P2C1 §12.4's P5 inputs.
  6. N1 bridge (P2 plan §2.3): objdump diff of `a1x`/`w1x`/`a1c`/`w1c` arms
     across the four binaries; differences beyond build metadata disclosed
     and affected N1-paired cells rerun before any dependent claim.
  7. **Mechanism-duplication audit** (MISSION §7.1 row 1): per-path table of
     A-end-state / B-end-state / AB-end-state for the four compose functions
     (objdump gates + recount numbers) plus the `src/blocking_file.cpp`
     diff-intersection line count — the shared-template overlap (P2C1 §1
     write-side inheritance; P2C2 §2.5 read-side inheritance) quantified.
  Failing any bound = the stack regressed a single factor's machine work →
  `INTERFERENCE_STATIC` recorded; the affected side's stacked wall claim is
  not admitted (no wall number can repair a failed static bound).
- **S2 — A's win units retained in the stack (gating).** For each unit
  u ∈ {comp-read-4k, eofmix, cursor}: **Δ_AB(u) ≤ −5.535 ns** (the pinned
  read floor — the single factor's own established line, P2C1 §3.5 G3 /
  P4 `WIN_CONFIRMED` caliber) **AND** 95% CI excludes 0. One-way MDE′ rule:
  fresh MDE′ dual-reported; a claim that would flip under MDE′ downgrades to
  `NO_CLAIM` — never silently decided at the narrower floor, and never
  decided at a floor **wider** than the pinned line. The line is fixed here,
  before data: sub-floor or CI-spanning → `STACK_LOSS(u)` recorded; Δ_AB(u)
  CI-excluding unfavorable at/above floor → `REGRESSION`. eofmix note: the
  product-claim wording keeps P2C1 §12.4's cap ("measurable improvement with
  re-attributed residual"; P4 recorded the cap direction falsified but the
  cap stands as the conservative bound); P5 records its own fresh verdict and
  **does not lift single-factor caps**.
- **S3 — B's instruction-caliber marginal retained (gating).**
  **P_ab_w ≤ P_base_w − 20 instr/op** (P2C2 §4.5 G3 verbatim, same PMU
  caliber: whole-process `instructions:u`, cross-arm deltas are the signal)
  AND S1's write-side static bounds pass. Wall: Δ_AB_w recorded; must not be
  EXCESS; **no wall claim is admitted for B in the stack** (the measurability
  upgrade remains optional and unclaimed, P2C2 §9).
- **S4 — negative controls (gating).** All four group-6 cells `PARITY` (P1
  rule). Any `EXCESS` → `REGRESSION` recorded. The **only** disposition is
  the preregistered compliant re-run (P2C1 §8/§12.1 precedent: §5 serial
  exclusivity, quiescent host, with the B1-side same-unit control cell; one
  compliant re-run; the original record stands archived) — never sample
  deletion, never repetition-until-pass, never a loosened threshold.
- **S5 — regression face (gating).** `p5-reg-readat-4k`, `p5-reg-writeat-4k`,
  `p5-reg-meta-oc`, `p5-reg-meta-fi`: expected and required
  `PARITY`/`NO_CLAIM`; any CI-excluding unfavorable delta ≥ its track floor =
  `REGRESSION`. `p5-reg-cold-4k`: expected `NO_CLAIM` (bounds the claim); a
  CI-excluding unfavorable delta ≥ the cold floor = `REGRESSION`. **Any cell
  in the matrix that computes to EXCESS/REGRESSION is recorded as REGRESSION;
  no rule is loosened to admit the stack** (the compliant re-run of S4 is a
  preregistered disposition, not a loosening).
- **S6 — endpoints and interaction attribution (recorded, non-gating for
  admission except D_ab_w).** D_ab, D_ab_eof, D_ab_cur per the P1
  EXCESS/PARITY rule: a read-endpoint EXCESS does not by itself fail the
  stack while S2 holds — it **caps** the stacked wall-parity subclaim under
  the inherited P2C1 §12.4 discipline (quantitative re-attribution
  obligation; per-extra-step recount split available from S1 item 5) —
  exactly the single factor's own PARTIAL-G4 semantics, no tighter, no
  looser. **D_ab_w must not be EXCESS** (B's own G4 no-regression rule).
  Interaction terms I(u) = Δ_AB(u) − Δ_A(u) recorded per unit (and
  I_w = Δ_AB_w − Δ_B_w) as attribution only: **independent gains are never
  added** (MISSION.md:404); no claim rests on I; I carries the disclosed
  ±30–80% static→wall systematic (P2C1 §12.4) as a caliber caveat.

**Stack verdict conjunction.** `STACK_ADMITTED` iff S0 ∧ S1 ∧ S2 (all three
units) ∧ S3 ∧ S4 ∧ S5, with S6 recorded. Fallback composition per MISSION
§7.1 ("最有效、语义完整、长期维护成本合理"): if S3 fails alone, the recorded
integration candidate is **A alone** (its KEEP stands on P2/P4 evidence); if
S2/S4/S5 fail, likewise whichever factor's own gates still hold; single-factor
KEEP verdicts are never withdrawn by P5 wall numbers. Statistical
non-significance after the preregistered budget → `INCONCLUSIVE` (MISSION §9),
never resolved by added post-hoc sampling without amendment.

## 5. Statistics discipline (inherited verbatim; P1 §5 + P2 §2.4 + P4 §6)

1. **A/A first**, per session, track and binary (group 1): sd_AA is the
   session noise floor; fresh MDE′ = 2.131·sd_AA/√16; cross-binary floors use
   the max-of-pair convention (P4 precedent); fresh MDE′ dual-reported with
   the pinned inherited floors (read **5.535 ns**, write **15.695 ns**, cold
   **1416.185 ns**); one-way downgrade rule (a claim that flips under MDE′
   downgrades to NO_CLAIM). One quiesce rerun per failed A/A; persistent
   noise disclosed, never absorbed by reps without amendment.
2. **≥16 ABBA pairs/cell (16 pinned)**, arm order alternating per pair, one
   process invocation per arm sample, `taskset -c 2`.
3. **Serial exclusivity:** strictly serial cells on a quiescent host — no
   concurrent cells, sessions, builds or unrelated load; `lsof` isolation
   before every write cell; the per-session pin record (`p5-raw/env-pin-<date>
   .txt`: load averages, background-process survey, environment values) must
   show **0 matching measurement processes before each session's cells
   count** — the pre-cell check that P4 could only archive at close becomes a
   requirement here (P4 §"Host quiescence" disclosed gap).
4. **Amendment-1 scratch** for every warm tmpfs write cell
   (`/tmp/sluice600/p5-<cellid>.bin`); cold discipline per P1 §7 (sudo drop,
   btrfs only, `NOT_RUN_WITH_REASON` fallback, never fadvise/tmpfs
   substitution).
5. **Point estimate** = median of the 16 paired deltas; **primary CI** = 95%
   t on the paired mean (t(0.975,15) = 2.131·sd/√16); sign count and AB vs BA
   subgroup medians recorded; all 16 raw deltas archived per cell.
6. **Wall source:** uninstrumented runs only. PMU (`perf stat -e
   instructions:u[,cycles:u]`), rusage (±30 ns coarse corroboration) and
   strace `-c` integral counts are separate diagnostic calibers, never
   differenced as wall (P1 §6 separation rule). The analyzer used for
   two-binary series must be committed to the kit branch or archived under
   `p5-raw/` (closing P4's disclosed `analyze2.py`-not-archived
   reproducibility gap).
7. **Run-integrity gates** (P1 §9) checked before any number is read: exit 0,
   no VERIFY_FAIL, checksum equality per cell's equality group,
   `content_ok=1` on write cells, fixture sha unchanged at session close,
   binary identity recorded per cell header. One failing invocation
   invalidates the cell series (rerun the cell; never drop samples).
8. **Environment drift disclosed, not absorbed:** actual governor,
   `perf_event_paranoid`, kernel recorded per session; every gate uses fresh
   MDE′ under the actual environment; **no cross-session or cross-kernel
   comparability is claimed** — P5 numbers are fresh measurements, never
   deltas against P1/P2/P4 values.
9. No family-wise correction across cells (P1 §5.6 precedent, disclosed).
   P99 remains `NOT_SUPPORTABLE_WITH_REASON` on every row (candidate effects
   < 10× the ≈26 ns per-op sampling overhead; no tail claim favorable or
   unfavorable — P4 §0 discipline).

## 6. MISSION §7.1 checklist → evidence map (fixed)

| §7.1 check (MISSION.md:743-751) | Evidence carrier in this plan |
|---|---|
| 机制是否重复 (mechanism duplication) | **Static:** S1 item 7 duplication audit — shared-template overlap quantified (A's write-side inheritance / B's read-side inheritance), per-path end-state table; the stacked tree's idempotence expectation is explicit (read side = A's end state; write side = B's bounds met, with A's exit machinery already carrying much of B's transform — outcome recorded either way) |
| 成本是否迁移 (cost migration) | **PMU attribution:** cross-arm `instructions:u` deltas are the signal (P1 §6.3); PHYSICAL_WORK_DELTA = 0 required on every claim unit (strace `-c` integral counts ≥3 exact repeats, bytes equal by construction/checksum); rusage user/sys split recorded — user→kernel migration would appear as sys/op growth with PMU constant; cycles:u recorded but not cited under schedutil (P4 caliber note) |
| 新增状态是否抵消收益 (new state offsetting gains) | **Static + wall:** S0's re-recorded authority-contraction audit (the stack must add no runtime state, remove no guard, delete `direct_outcome`/`publish_certainty` once) **and** S2 cells — if statics hold while the stacked win is lost, state-offset/interference is the recorded hypothesis with a re-attribution obligation (S6/S2 disciplines) |
| 用户态/内核态此消彼长 (user/kernel seesaw) | **Syscall counts + PMU:** identical integral syscall counts per arm (strace, count caliber) exclude syscall-count migration; PMU instruction deltas localize remaining user-space change; rusage user vs sys ns/op split per cell recorded — a seesaw requires sys-growth + user-shrink with syscall counts flat, which the record would expose |
| 对其他 Direct 操作的回归 (other Direct ops) | **Regression-face cells** (group 7): public `read_at` (`p5-reg-readat-4k`), public `write_at` (`p5-reg-writeat-4k`), `open`+`close` (`p5-reg-meta-oc`), `file_info` (`p5-reg-meta-fi`); plus the machine-level exclusion (S1 item 3: public-op instruction streams sha-equal to B1) |
| 对不同 Payload 的回归 (different payloads) | The matrix spans payloads: 4 KiB seq composition, tail/EOF-boundary rotation, shared-cursor composition, write composition, 1 B/4 KiB public ops, metadata, cold btrfs (group 7). **Boundary, stated honestly:** P4's seven single-factor `NO_CLAIM` shapes (hf-1b/4k, seq-1m, rand-4k, meta-*, cold) are not all re-measured here; for shapes whose entire candidate exposure is the public ops (hf/seq/rand/meta), S1's sha-equal public-op gate is the structural coverage argument, recorded as structure — not as fresh measurement |
| 对现有 v2 生命周期与错误语义的影响 (v2 lifecycle/error semantics) | **Handed to the independent correctness review** (MISSION §7.2; this plan §7): the performance plan supplies S0's evidence (union-seam mutant gate with named kills, four-config matrix, P0 §5.1 obligation table on the stacked tree); the reviewer independently checks v2-r1 correspondence, public behavior, range/zero-length/error precedence, buffer/file lifetime, partial effects, composition state, concurrent calls, fault injection, mutation oracle, product-vs-research diff — never by re-quoting the implementer's conclusions |

## 7. Independent reviews (MISSION §7.2/§7.3; P5 deliverables after measurement)

- **P5.2 correctness review.** Inputs: the stacked branch, S0 gate records,
  the S1 static dossier, the P0 §5.1 obligation table restated for the
  stacked tree, the merge diff (§2 resolutions auditable commit-by-commit).
  Output: `APPROVE`/`REVISE`/`INCONCLUSIVE`/`REJECT` with findings. If the
  review is unavailable, that fact is recorded — **no self-approval**
  (MISSION.md:798-800).
- **P5.3 performance review.** Inputs: all four binaries'/archives' identity
  records, kit source pin, raw cell files with 16 deltas, A/A floors and
  MDE′ derivations, ABBA order blocks, PMU/strace/rusage sections, the §6
  map, application-transfer evidence (structural: the Parquet adapter path
  is `read_at`, machine-identical in the stack by S1 item 3, and P4 measured
  both single factors `PARITY` at workload scale — no A+B PARQUET cell is
  preregistered; adding one is a numbered amendment following the P4 §4.1
  staging discipline). The reviewer independently re-derives at least the
  three S2 cells' statistics from the raw deltas and checks comparator
  fairness, kernel/userspace attribution and regression boundaries. Same
  verdict vocabulary; `REVISE` requires re-verification of affected evidence
  at the final HEAD.

## 8. Raw recording conventions

- All raw series → `research/direct-beyond-parity/p5-raw/<cell-id>.txt`, plus
  `p5-raw/identity.txt`, `p5-raw/env-pin-<date>.txt`, `p5-raw/kit-smoke.txt`,
  `p5-raw/P5AB/` (matrix/mutants/static dossiers). Headers carry date, host,
  kernel, exact producing command. Analysis re-runnable via the archived
  order-aware analyzer (§5 item 6).
- No frozen artifact of P1/P2/P4 (`p1-raw/`, `p2-raw/`, `p4-raw/`,
  `build/parquet-results/`) is edited; P5 adds files only.

## 9. Limitations (preregistered)

1. E5 single host; no cross-host or cross-kernel comparability; wall
   magnitudes carry the environment caveat actually recorded at session time.
2. The matrix is kit-shape evidence; application-scale transfer of the stack
   is structural (public-op machine identity + P4 single-factor `PARITY`),
   not freshly measured (§7).
3. Multi-step composition wall shapes remain test-seam territory (P2 §4.4
   caliber limitation inherited); per-extra-step adjudication stays static
   (S1 item 5).
4. P99/tail: `NOT_SUPPORTABLE_WITH_REASON` everywhere; no tail statement.
5. P5 does not lift single-factor caps (P2C1 §12.4) and does not re-adjudicate
   frozen single-factor verdicts; fresh single-arm cells are attribution
   references only (§0 rule 2, §3 group 3).
6. Interaction attribution I(u) is descriptive; the static→wall mapping
   carries the ±30–80% systematic disclosed in P2C1 §12.4.
7. If any prerequisite (sudo/cold, quiescence, PMU availability) is
   unavailable: `NOT_RUN_WITH_REASON` in the cell record; no substitution.

## Amendment log

1. **2026-10-28, implementation session — gate-script filenames and
   rules-control define.** Trigger: the P5 implementation task pinned the
   gate paths as `p5-scripts/p5-combined-matrix.sh` /
   `p5-scripts/p5-combined-mutants.sh`, while §2 preregistered the names
   `P5AB-matrix.sh` / `P5AB-mutants.sh`. Change: the committed scripts use
   the task's filenames in the same directory
   (`research/direct-beyond-parity/p5-scripts/`), with the preregistered
   content obligations unchanged (P1 §8 four-configuration legs; §1 seam
   union named-kill rows GREEN→RED(named)→RESTORED GREEN; exit-code
   adjudication; tree head + timestamped archives closing the two P2
   gate-script defects). Raw archives stay under `p5-raw/P5AB/` as
   preregistered. Reason: same mechanism, same location, same gates —
   filename only; recorded so the §2 recipe and the delivered tree cannot
   drift apart silently. Disclosed in the same entry: commit `4ab6ef17`
   retargets the rule-suite-under-mutant control's define from
   `ADVANCE_BY_REQUESTED` to `FAILURE_ACCOUNTED` — a consequence of
   resolution R5 (P2C1's header carries ADVANCE/SKIP/RETRY guards on the
   oracle rule table, so those defines no longer isolate the production
   path on the stacked tree); `FAILURE_ACCOUNTED` is a shared compose seam
   present only in the production TU, preserving the control's
   preregistered meaning (rule suite GREEN while a production compose seam
   is mutated). Verified RED under the old define
   (`p5-raw/P5AB/mutants-20261028T171339/mutants-rules-control.err`).
