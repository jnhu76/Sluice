# P2 Experiment Plan — Direct Same-Semantics Work Elimination (Preregistered)

- Mission: `research/direct-beyond-parity/MISSION.md` §4 (P2 — Same-Semantics Work
  Elimination; single-factor experiments per P2.2, correctness gates per P2.3).
- Preregistration date: 2026-10-27. Primary platform: E5 Native Linux.
- **Status of this document: PREREGISTERED.** Candidate definitions, families,
  mechanisms, seam sets, hypotheses, beat-targets, cells, pairing counts,
  statistics, pinned MDEs and gate conventions below are fixed **before** any P2
  seam, ladder-arm, mechanism or measurement work runs. Mid-campaign deviations
  are recorded as numbered amendments with reason, never silently applied (P1
  precedent: `P1_MEASUREMENT_PLAN.md` Amendment 1).
- Inputs (all read for this plan): `CANDIDATE_GAP_REGISTER.md` (gap families,
  falsifiers, §4 seam deficits), `DIRECT_COST_FLOOR.md` (cost model, §6.2
  cross-round contradiction), `p1-audit/composition-read.md`,
  `p1-audit/combined-writes.md` (machine evidence, seam inventories),
  `P1_MEASUREMENT_PLAN.md` (§5 statistics, §6 calibers, §8 matrix commands,
  Amendment 1), `P0_BASELINE_AND_OWNERSHIP.md` §3.3 (C2 null boundary).
- Baseline identity (re-verified this session, read-only):
  `git -C /home/jnhu/Source/Sluice-600-wt rev-parse HEAD` →
  `77e5399a45998dea396699fa2e063c94a665945a`; `sha256sum …/libsluice_core.a` →
  `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0`.
  Kit: local branch `direct-cost-ladder` @ `e096b4a1e3b7fb1cbfedfcd5f10e7e6a88050637`,
  worktree `/home/jnhu/Source/Sluice-direct-cost-ladder` (both re-verified this
  session); B1-linked ladder binary sha256
  `0685c31f7585e437910ed671a7025a172de4303e756d5b6e31b2480e1c45812a`
  (`P1_MEASUREMENT_PLAN.md` §1).

## 0. The C2 null boundary (governs every admission below)

P0 §3.3: the #600 C2 prototype removed exactly the per-step re-admission class
(prevalidated step primitive; compose loop reduced to `pread`/`__errno_location`
calls) and measured AX−A2 +9.9 → +9.7 ns — the 0.2 ns change is within noise,
so **C2's null bounds only the re-admission item (G-REA-01, and by source shape
G-REX-01)**. C2 kept `compose()`, `compose_progress`/`compose_error`,
`CompositionState` and `direct_outcome` unchanged
(`p1-audit/composition-read.md` §3; patch re-verified present this session:
`~/sluice-600-direct-research/evidence/c1c2_full_worktree.patch`, 8
`prevalidated` occurrences), so the null **does not constrain G-REA-02/03**
(dead state round-trip, double exit materialization) — the register's leading
carriers for the measured aggregate. P2 candidates are scoped accordingly.

## 1. Admission decisions (fixed)

| ID | Family (register entries) | Mechanism (single factor) | Verdict basis |
|---|---|---|---|
| **P2C1** | G-READCOMP-01 = {G-REA-01, G-REA-02, G-REA-03} + shared-cursor twins {G-REX-01, G-REX-02, G-REX-03} | Combined read-composition branch per the register falsifier's merged shape: (a) prevalidated step primitive, (b) composition-step state register residency, (c) single-point outcome materialization | Measured wall EXCESS +18.992/+16.129 ns (both ≥ MDE 5.535, CI-excluding) + PMU +118.2/+123.4 instr/op; measuredSupport true |
| **P2C2** | G-WRITECOMP-01 = {G-WALLAT-01, G-WALLAT-02, G-WALLAT-03} + twins {G-WALL-01, G-WALL-02, G-WALL-03} | Staging out of the compose loop + dead-store elimination (exit-only outcome materialization) | Instruction-caliber gap: PMU +58.1 instr/op, composition-increment ratio ≈ 2.8×; wall +11.392 sub-MDE (15.695) → PARITY at wall, gap open at instruction caliber; measuredSupport true |

Deferred register entries (§5): G-READAT-01, G-WRITEAT-02, G-READAT-02,
G-META-01, G-OPEN-01, G-WRITEAT-01.

Scope notes pinned for both candidates:

- Public API, public semantics, `Result`/`CompositionOutcome` ABI, admission
  precedence, EINTR retry-without-count, stop set, effect certainty and the
  confirmed-prefix contract are untouched (MISSION P2.3 list). Public
  `read_at`/`read`/`write_at`/`write` bodies are not modified by either
  candidate.
- The impossible-count guards are **kept** in both candidates: keep/remove is
  an owner decision under the mechanism-justification gate, oracle-pinned by
  `impossible_count_stops_immediately` (register §3; `DIRECT_COST_FLOOR.md`
  §3.8). P2 adds the seam + the missing write-side script (§2.1) but does not
  remove the guard.
- P2C2 **does not** remove G-WALLAT-01/G-WALL-01 (per-step re-validation):
  single-factor discipline. The read-side C2 null (§0) removed exactly that
  class and measured ≈ −0.2 ns; no write-side experiment constrains its
  standalone dynamic effect and none is claimed. Its instruction cost remains
  visible in P2C2's instruction caliber and is attributed, not eliminated.

## 2. Cross-candidate protocol (binding on both)

### 2.1 Seams first (register §4 deficit set)

B1 has **no** composition mutation seam (verified this session by grep over
`/home/jnhu/Source/Sluice-600-wt/{include,src,xmake,tests}`: only
`SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED` `file_semantics.hpp:158`,
`SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE` `:168`,
`SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE` `:192`; zero
`SLUICE_DIRECT_MUTANT*` matches). Every P2 branch touching
`compose`/`direct_outcome` must land its seam set + killer scripts as
commits **before** the mechanism commit, prove BASE GREEN → MUTANT RED →
RESTORED GREEN at the pre-mechanism tree (`p2-scripts/<id>-mutants.sh`, §2.5),
then re-prove after the mechanism lands. Required seam set (compile-time
product mutants with xmake targets via the `direct_mutant_target()` pattern,
`xmake/tests.lua:93-99`):

| Seam (name) | Mutated behavior | Named killer(s) (verified at B1 this session) |
|---|---|---|
| `SLUICE_COMPOSE_MUTANT_ADVANCE_BY_REQUESTED` | `compose_progress` advances `confirmed_bytes` by requested/remaining instead of transferred | write: `short_writes_reach_full_progress` (`tests/direct_composition_fault_test.cpp:110`), `composition_advances_the_buffer_by_confirmed_bytes` (`:155`); read: `read_exact_reports_eof_before_full_with_the_prefix` (`tests/direct_composition_test.cpp:82`), `read_exact_reads_the_whole_request` (`:63`) |
| `SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS` | zero-progress does not stop; loop re-enters | `zero_progress_write_stops_after_one_attempt` (`tests/direct_composition_fault_test.cpp:132`, asserts `script.calls() == 1`), `write_all_zero_progress_stops_and_keeps_its_prefix` (`tests/semantic_short_io_reference_test.cpp:78`), `composition_never_spins_on_zero_progress` (`:90`); read EOF-stop side: `read_exact_reports_eof_before_full_with_the_prefix` |
| `SLUICE_COMPOSE_MUTANT_FAILURE_ACCOUNTED` | primitive-error publish maps `remaining = accounted` instead of `unknown` | `error_after_confirmed_prefix_keeps_the_prefix` (`tests/direct_composition_fault_test.cpp:197`), `a_primitive_error_reports_an_unaccounted_remainder_in_both_directions` (`:365`) |
| `SLUICE_COMPOSE_MUTANT_SKIP_IMPOSSIBLE_COUNT` | impossible-count guard skipped; over-count absorbed into `confirmed_bytes` | read: `impossible_count_stops_immediately` (`tests/direct_composition_fault_test.cpp:340`); write: **new script** `write_all_impossible_count_stops_immediately` (scripted pwrite over-count variant — the missing write-side impossible-count script, added with the seam) |
| `SLUICE_DIRECT_MUTANT_STEP_SKIPS_EINTR_RETRY` (P2C1 only; C2 lineage, patch lines 212-248) | step primitive returns `interrupted` on EINTR instead of re-issuing | `primitive_retries_eintr_without_a_completed_count` (`tests/direct_composition_fault_test.cpp:224`), `primitive_write_retries_eintr_then_stops_at_the_first_count` (`:277`) |
| `SLUICE_DIRECT_MUTANT_STEP_REPORTS_FULL_COUNT` (P2C1 only) | step reports `remaining` as transferred regardless of actual count | `short_writes_reach_full_progress` (buffer/count/offset assertions, `:110-130`); `read_exact_reads_the_whole_request` (`:63`) |

Kill discipline (MISSION P2.3): a kill is valid only when the mutant build
fails the **named assertion**; crash, build failure or timeout is not a kill
and the mutants gate exits non-zero. If any killer above proves insufficient at
seam-landing time, the landing commit adds the missing named assertion in the
same commit — never a weaker mutant.

### 2.2 Branch and worktree discipline

- Candidates are cut from B1 `77e5399a45998dea396699fa2e063c94a665945a`
  (branch `sluice-600-direct-c1` head), behavior-named, local-only, never
  pushed by the research run, never inside the frozen worktrees or the main
  workspace (P0 §1.3 ownership rules; P1 §8).
  - P2C1: branch `direct-read-compose-prevalidated-steps`, worktree
    `/home/jnhu/Source/Sluice-direct-read-compose-prevalidated-steps`.
  - P2C2: branch `direct-write-compose-exit-staging`, worktree
    `/home/jnhu/Source/Sluice-direct-write-compose-exit-staging`.
- Commit order per candidate: (1) seams + killers (+ the missing write-side
  impossible-count script) — BASE GREEN mutants proof; (2) mechanism — mutants
  re-proof + static gates; (3) nothing else rides along (narrow commits).
- The frozen worktree `/home/jnhu/Source/Sluice-600-wt` stays untouched;
  baseline cells link the existing B1 ladder binary only.

### 2.3 Ladder twin-arm extension (hard measurement preconditions)

The register carries a hard precondition on every G-REX/G-WALL entry: no wall
claim for `read_exact`/`write_all` until a committed twin arm (shared-cursor
composition, N1-matched control included) runs a 16-ABBA cell. P2 delivers this
first, as one commit on the kit branch `direct-cost-ladder` (P0 §4.8 committed
comparator identity):

- New arms: `ac` (Sluice `read_exact` via the linked archive), `a1c`
  (N1-matched native `read_exact`: public headers only, separate TU, exact B1
  public-path check order, EINTR retry without count, EOF/complete/error stops
  with accounted/unknown certainty mapping and confirmed-prefix preservation);
  `wc`/`w1c` same for `write_all`. Existing `a1x`/`ax`/`w1x`/`wx` arms are
  unchanged (kit dispatch verified this session at
  `research/direct-cost-ladder/ladder.cpp:705-711`).
- Two-binary pairing mode: committed extension of `paired.sh` (or a committed
  `paired2.sh`) accepting two ladder binaries, alternating binary order per
  pair (ABBA), one invocation per arm sample, same sample format for
  `analyze_pairs.py`. This is the caliber #600 used for candidate-vs-baseline
  build pairs (P0 §3.2 precedent).
- Smoke gate before any cell: all families × patterns (seq/rand/tail) × states
  (normal/closed/eintr) build, run, checksum-identical inside the extended
  equality groups (read {a0,a1,a2,a1x,ax,a1c,ac} minus a3; write
  {w0,w1,w2,w1x,wx,w1c,wc} with `content_ok=1`); raw →
  `p2-raw/<id>/smoke-ladder.txt`.
- Record the kit commit SHA, both new ladder binary sha256s (B1-linked
  rebuild for uniformity, plus candidate-archive builds per §2.4), and the N1
  mirror source hashes. N1 mirror validity follows P1 §11.1: the candidates
  change no public header (mechanisms live in `src/`/`detail/`), so mirrors
  stay valid; any public-header change invalidates the mirror and blocks
  comparison until regenerated.
- Cross-binary N1 bridge check: `objdump -d` diff of the `a1x`/`w1x`/`a1c`/
  `w1c` code between the B1-linked and candidate-linked binaries; any
  instruction difference beyond build metadata is disclosed and the N1 cells
  are rerun on both binaries before any cross-binary claim.

### 2.4 Measurement units and statistics (inherited + P2 additions)

Statistics inherit `P1_MEASUREMENT_PLAN.md` §5 verbatim: interleaved ABBA, 16
pairs/cell (≥15 required), one process invocation per arm sample, arm order
alternating per pair, `taskset -c 2`; point estimate = median of paired
deltas; 95% t-CI on the paired mean (t(0.975,15)=2.131·sd/√16); sign count as
corroboration; raw 16 deltas archived; run-integrity gates P1 §9 checked
before any number is read (exit 0, no VERIFY_FAIL, checksum equality,
`content_ok=1` on write cells, fixture sha unchanged, ladder identity
recorded). Wall comes only from uninstrumented runs; PMU
(`instructions:u`,`cycles:u`) and strace `syscalls/op` are separate diagnostic
calibers, never wall sources (P1 §6).

P2 additions (preregistered):

1. **Pinned decision MDEs.** Decision floors are the P1 session values:
   read **5.535 ns**, write **15.695 ns**. Fresh A/A cells run FIRST (below);
   if the fresh sd_AA implies a wider MDE′ = 2.131·sd_AA′/√16, both MDE and
   MDE′ are reported and any claim that would flip under MDE′ is downgraded to
   NO_CLAIM — never silently decided at the narrower floor.
2. **A/A first, per binary.** Each session opens with `aa-read-4k`
   (`a1 a1 4096 1024 1024 5 16 $READ_FIX`) and, for write sessions,
   `aa-write-4k` (`w1 w1 4096 1024 1024 5 16 <scratch>`), run on **both** the
   B1-linked and candidate-linked binaries.
3. **Cross-binary candidate cells.** The mechanism's own wall effect is
   measured by ABBA pairings of the same arm across the two binaries
   (candidate − baseline convention), the #600 candidate-cell caliber. This is
   the primary improvement statistic; the A1x/W1x/A1C/W1C-paired cells give
   the composition-excess verdicts per the P1 rule.
4. **Serial exclusivity.** All cells run strictly serially on a quiescent
   host: no concurrent cells, sessions, builds or unrelated load; `lsof`
   isolation check before every write cell. (P1 Amendment 1 collisions and
   the disclosed concurrent sibling runs behind `DIRECT_COST_FLOOR.md` §6.2's
   cross-round contradiction motivate this; the settlement in §3 depends on
   it.)
5. **Amendment 1 scratch discipline** applies to every warm tmpfs write cell:
   per-cell scratch `/tmp/sluice600/p2-<cellid>.bin`, `mkfixture` immediately
   before, deleted after; no other command token changes.
6. Raw records: `research/direct-beyond-parity/p2-raw/<candidate-id>/<cell>.txt`
   with date/host/command headers (P1 §10 conventions); analysis re-runnable
   via `python3 $KIT/analyze_pairs.py`.
7. Environment pin: E5, kernel 7.2.11-200.fc44, GCC 16.2.1, binutils 2.46.1,
   `scaling_governor=performance`, `perf_event_paranoid=1`, fixtures per P1 §3
   (`$READ_FIX=/tmp/sluice600/data64m.bin`; write cells per-cell scratch).

### 2.5 Gate scripts (convention, exit-code adjudicated)

Both scripts are committed on each candidate branch under
`research/direct-beyond-parity/p2-scripts/` (main-workspace research dir is
untracked on master; scripts are referenced by absolute path, mirroring P1
§8's tee convention):

- **`p2-scripts/<id>-matrix.sh`** — run with CWD = the candidate worktree
  root. Executes the exact P1 §8 four-configuration matrix:
  ```
  for m in release debug asan asanubsan; do
    xmake f -c -m $m --liburing=n -y && xmake -y && xmake test -y \
      | tee /home/jnhu/Source/Sluice/research/direct-beyond-parity/p2-raw/<id>/matrix-$m.txt
  done
  ```
  `--liburing=n` pinned for all four legs (frozen-library identity; P1 §8).
  Exit 0 iff every leg configures, builds, and reports 0 failed tests (the
  suite count may exceed 58 once seam tests land — the gate is 0 failed, count
  recorded); exit 1 naming the failing leg otherwise. The release leg's
  archive sha256 is recorded (it feeds the candidate ladder).
- **`p2-scripts/<id>-mutants.sh`** — run with CWD = the candidate worktree
  root. For each seam in the candidate's §2.1 table: (a) BASE build (release,
  `--liburing=n`) → named killer GREEN; (b) mutant target build (seam define
  via the `direct_mutant_target()` pattern) → killer fails **with the named
  assertion** (RED; crash/build-failure/timeout = gate failure); (c) restored
  BASE build → killer GREEN. Exit 0 iff every seam completes
  GREEN→RED→RESTORED GREEN; exit 1 naming the seam. Run at the seams-only
  commit (pre-mechanism) and again at the mechanism commit.

### 2.6 Execution order and decision vocabulary

Per MISSION P2.2 (correctness before measurement): (1) kit twin arms + smoke;
(2) seams + killer scripts on the candidate branch, `mutants.sh` exit 0;
(3) mechanism commit, `mutants.sh` exit 0 again, static/objdump gates green;
(4) `matrix.sh` exit 0; (5) session A/A cells; (6) baseline (B1-binary) cells;
(7) candidate cells + cross-binary cells + negative controls; (7b) PMU/strace
diagnostics; (8) verdicts per §3/§4 pass rules; (9) per-candidate report
`P2_CANDIDATE_<ID>.md` (source diff, machine-work diff, semantic proof,
mutation evidence, A/A, paired A/B, negative controls, benefit boundaries,
decision) — KEEP / REJECT / REVISE per MISSION. A red suite at any step stops
measurement for that candidate (MISSION §9). Each candidate is decided
independently.

## 3. P2C1 — read-composition step residency (G-READCOMP-01 family)

### 3.1 Branch

`direct-read-compose-prevalidated-steps` from `77e5399a`; worktree
`/home/jnhu/Source/Sluice-direct-read-compose-prevalidated-steps`.

### 3.2 Preregistered hypothesis and beat-target (settles the 2× divergence)

**Contradiction being settled** (register G-READCOMP-01 falsifier;
`DIRECT_COST_FLOOR.md` §6.2): #600 recorded a post-C1/C2 AX−A2 residual of
+9.7…+9.9 ns `[TRANSCRIBED]` (P0 §3.3); the P1 session measured the same
quantity shape at +18.992 ns paired (A1x vs AX, 4 KiB) / +16.129 (EOF mix) /
≈+19.0 unpaired (AX 721.206 vs a2 702.210). The ~2× median spread is not
explained by the wider P1 noise floor.

- **H0 (settlement, runs first):** the same-session fresh B1 baseline cell
  `p2c1-base-composition-4k` (B1 binary, A1x vs AX, 16 ABBA) yields S; its
  tail twin yields S_eof. **The candidate's beat-target is S (and the
  cross-binary Δ_own) — never a transcribed prior-round number.** Both
  outcomes are preregistered: (a) S ≥ 2×MDE = 11.07 ns → the P1 magnitude
  reproduces on the current host state and #600's +9.7…+9.9 is attributed to
  cross-round drift (comparator-arm, host/kernel state, or residual contention
  — the §6.2 candidate explanations); (b) S < 11.07 ns → the P1 +18.992
  carried session-state inflation and the contradiction resolves toward the
  #600 band. Either way the adjudication is recorded as an outcome, and the
  improvement statistics below are S-relative.
- **H1 (primary improvement):** the combined mechanism removes the per-step
  re-admission (G-REA-01/REX-01), the dead `CompositionState` round-trip
  (G-REA-02/REX-02) and the double exit materialization (G-REA-03/REX-03) —
  static ≈ −21…−30 instructions on the 1-step path, ≈ −21 per additional step
  (`p1-audit/composition-read.md` §1.2/§2.2, §4 aggregate falsifier).
  Preregistered wall prediction: **Δ_own ∈ [−12, −5] ns** on the 4 KiB
  composition cell (audit band −7…−10 ns with session slack).
  - *Measurable improvement (claim)*: Δ_own ≤ −5.535 ns AND 95% CI excludes 0
    (cross-binary cell), corroborated by D_cand = S − |Δ_own|-consistent drop
    in the A1x-paired candidate cell.
  - *Refutation (register line)*: improvement I = |Δ_own| (equivalently
    S − D_cand) < 3 ns → the +~19 ns aggregate is **not** carried by
    G-REA-01/02/03 and the static decomposition is refuted; re-attribution
    (e.g. front-end/branch effects) becomes mandatory and the candidate is
    REJECTed for wall caliber (static gains survive only as instruction-
    caliber record; #600 admission rule bars product admission without
    repeatedly measurable gain).
  - *Band 3 ns ≤ I < 5.535 ns*: decomposition alive, below the pinned session
    floor → NO_CLAIM at wall, INCONCLUSIVE for the wall claim.
- **H2 (parity endpoint):** after the mechanism, D_cand (candidate A1x vs AX)
  satisfies |median| < max(5.535, 1%·A1x median) → G-READCOMP-01's wall EXCESS
  is eliminated (PARITY_REACHED). If Δ_own is measurable but D_cand remains
  EXCESS, the residual is re-attributed under H1's refutation discipline.
- **H3 (twin, first measurement):** `read_exact` (shared cursor) gets its
  first-ever wall cells (base + candidate + cross-binary delta); verdicts per
  the P1 rule with no extrapolation cited as measurement (register
  precondition).
- **Negative controls:** `read_at` single-op cells unchanged (mechanism
  touches only composition internals; public `read_at`/`read` untouched):
  pass = PARITY per rule on the candidate binary; any EXCESS is a regression.
- **PMU corroboration (non-gating):** AX−A1x instruction delta drops by
  ≥ 30 instr/op from the fresh baseline's expected ≈ +118 (static −21…−30 at
  the observed ≈3× PMU/static ratio); strace syscall parity preserved
  (2.000 pread64/op timed+verify at 4 KiB seq; 1.3333/op at the tail mix; 0 on
  zero-length).

### 3.3 Mechanisms (single combined factor; static predictions and objdump gates)

Counting convention: the P1 audit exact-path convention
(`p1-audit/composition-read.md` header). B1 references: `read_exact_at`
1-step 100 / per-extra-step 44; `read_exact` 82 / 34.

1. **Prevalidated step primitive (G-REA-01/G-REX-01; C2 shape).** Private step
   function per composition (pread/read + EINTR retry + count return) used
   after the one outer admission; public `read_at`/`read` unchanged. Patch
   lineage: `~/sluice-600-direct-research/evidence/c1c2_full_worktree.patch`
   (re-verified present this session). Prediction: −13 instr/step
   (`read_exact_at`, block 0xfc0-0xfff), −9 (`read_exact`, 0x1790-0x17ad).
   Objdump gate: the compose loop contains no closed/access/range/transfer
   re-test sequence; step machinery reduces to the native call + arg
   derivation.
2. **Composition-state register residency (G-REA-02/G-REX-02).** Loop carries
   confirmed/stop/end/error in registers; the per-step `CompositionState`
   materialization block (8 instr / 7 memory ops; `read_exact_at`
   0x1250-0x127f, `read_exact` 0x19d0-0x19ff) is eliminated. Prediction: −8
   instr/step each. Objdump gates: zero textual occurrences of the never-read
   16-B stores `movaps …,0x20(%rsp)` (0x1277) and `…,0x10(%rsp)` (0x19f7);
   no `movdqa`+`movaps` round-trip on the loop-back path.
3. **Single-point outcome materialization (G-REA-03/G-REX-03).** The
   `Result<CompositionOutcome>` sret is constructed directly per exit flavor
   (≈5 direct stores, N1-arm shape) instead of the intermediate stack object
   (~12 memory ops + dead zero-stores; `read_exact_at` 0x10d3-0x1132 +
   0x1285/0x1294; `read_exact` 0x1873-0x18c7 + 0x1a05/0x1a14). Prediction:
   −7…−9 instr per exit. Objdump gates: the dead zero-stores are absent; a
   single materialization site per exit flavor.

**Pinned static pass bounds (recount under the audit convention):**
`read_exact_at` 1-step ≤ 75 (from 100), per-extra-step ≤ 26 (from 44);
`read_exact` 1-step ≤ 60 (from 82), per-extra-step ≤ 21 (from 34); public
`read_at` 39 / `read` 29 unchanged (±0); impossible-count guard still present
(oracle-pinned, kept). Failing any bound falsifies the static decomposition at
machine level.

### 3.4 Measurement units (all warm tmpfs, 16 ABBA pairs, reps=5 best-of, taskset -c 2)

`$PAIRED`= kit paired.sh; `$LADDER_B1` = B1-linked binary (sha256 0685c31f…);
`$LADDER_C1` = same ladder.cpp linked against the candidate release archive;
4 KiB shape `4096 1024 1024 5 16`. `$READ_FIX=/tmp/sluice600/data64m.bin`.

| id | pairing | binary | command shape |
|---|---|---|---|
| p2c1-aa-read-4k-base | a1 vs a1 | B1 | `$PAIRED $LADDER_B1 a1 a1 4096 1024 1024 5 16 $READ_FIX` |
| p2c1-aa-read-4k-cand | a1 vs a1 | cand | same with `$LADDER_C1` |
| p2c1-base-composition-4k | a1x vs ax | B1 | `$PAIRED $LADDER_B1 a1x ax 4096 1024 1024 5 16 $READ_FIX` → **S** |
| p2c1-base-eofmix-4k | a1x vs ax tail | B1 | `… a1x ax 4096 1024 1024 5 16 $READ_FIX tail` → **S_eof** |
| p2c1-cand-composition-4k | a1x vs ax | cand | same on `$LADDER_C1` → **D_cand** |
| p2c1-cand-eofmix-4k | a1x vs ax tail | cand | → **D_cand_eof** |
| p2c1-delta-composition-4k | ax(B1) vs ax(cand) | both | two-binary ABBA → **Δ_own** (primary) |
| p2c1-delta-eofmix-4k | ax tail, both | both | → Δ_own_eof |
| p2c1-base-cursor-composition-4k | a1c vs ac | B1 | first-ever `read_exact` wall cell |
| p2c1-cand-cursor-composition-4k | a1c vs ac | cand | candidate twin |
| p2c1-delta-cursor-composition-4k | ac(B1) vs ac(cand) | both | twin improvement |
| p2c1-negread-4k | a1 vs a2 | cand | negative control |
| p2c1-negread-1b | a1 vs a2 1 B | cand | `… a1 a2 1 4096 4096 5 16 $READ_FIX` |

PMU diagnostic per cell and arm (P1 §6.2 command); strace `pread64`/`read`
counts on the four composition cells (both binaries).

### 3.5 Pass rules (preregistered)

- **G1 machine (gating):** §3.3 objdump gates + recount bounds + public-op
  shapes unchanged + N1 bridge equivalence (§2.3) or disclosed rerun.
- **G2 correctness (gating):** `p2c1-mutants.sh` exit 0 at seams-only commit
  and at mechanism commit; `p2c1-matrix.sh` exit 0.
- **G3 improvement:** Δ_own ≤ −5.535 ns, CI excluding 0 (§3.2 H1 claim rule).
- **G4 parity endpoint:** D_cand (and D_cand_eof) no longer EXCESS per the P1
  rule.
- **G5 negative controls:** p2c1-negread-* PARITY; no cell EXCESS.
- KEEP-eligible iff G1+G2+G3+G5 (G4 recorded; its absence caps the claim at
  "measurable improvement with re-attributed residual"). Improvement < 3 ns →
  H1 refuted → REJECT (wall), instruction-caliber record only. Any G2 red →
  BLOCKED_CORRECTNESS, measurement stops (MISSION §9).

## 4. P2C2 — write-composition exit staging (G-WRITECOMP-01 family)

### 4.1 Branch

`direct-write-compose-exit-staging` from `77e5399a`; worktree
`/home/jnhu/Source/Sluice-direct-write-compose-exit-staging`.

### 4.2 Preregistered hypothesis and beat-target

Standing measured facts (register G-WRITECOMP-01): wall +11.392 ns sub-MDE →
PARITY at the wall caliber; **PMU +58.1 instr/op** cross-tier (WX−W1x);
same-caliber composition-increment ratio (WX−W2)/(W1x−W1) = +61.0/+22.0 ≈
2.8×. The gap is instruction-caliber; the candidate's primary claim is
instruction-caliber.

- **H1 (primary, machine/instruction caliber):** moving the
  `CompositionState`→outcome staging out of the compose loop (outcome
  materialized only at exit; confirmed/total register-carried) removes the
  per-iteration staging block and, as its canary, the dead 16-B store.
  Preregistered: per-extra-step −7…−10 instr (`write_all_at`), −4…−7
  (`write_all`); 1-step path −8…−12 / −5…−8 (pinned bounds in §4.3). The
  register-caliber spill relief (caller-saved `total` pair, EINTR-feeder
  store) is predicted if allocation matches the A1 arm's callee-saved shape —
  reported either way, not gating.
- **H2 (wall caliber, measurability probe only):** predicted wall effect
  ≈ 4–7 ns/op scaled `[UNCERTAIN]` < MDE 15.695 → expected outcome stays
  instruction-caliber-only (register: "wall ≥ MDE would establish
  measurability, sub-MDE keeps it instruction-caliber only"). A wall
  improvement ≥ 15.695 with CI excluding 0 on `p2c2-delta-composition-4k` is
  recorded as a measurability upgrade, not a pass requirement. **No wall
  regression is tolerated:** D_cand_w must not be EXCESS.
- **H3 (PMU corroboration, non-gating):** fresh baseline PMU delta P_base
  (expected ≈ +58 instr/op); candidate P_cand predicted ≤ P_base − 20
  (static −8…−12 × the observed ≈2.0× PMU/static ratio 58.1/29).
- **H4 (twin, first measurement):** `write_all` (shared cursor) first-ever
  wall cells (base/candidate/cross-binary), verdicts per rule.
- **Negative controls:** `write_at`/`write` single-op cells unchanged
  (mechanism touches only the compose loop; public ops and the step lambda's
  public `write_at`/`write` calls untouched — G-WALLAT-01/G-WALL-01 retained
  by the fixed admission decision, §1): pass = PARITY on the candidate binary.
- **Syscall parity preserved:** 1.000 pwrite64/op ≥1 B, 0.000 at 0 B, both
  binaries (strace caliber).

### 4.3 Mechanisms (single factor; static predictions and objdump gates)

B1 references (`p1-audit/combined-writes.md` §2): `write_all_at` 1-step 101 /
per-extra-step 44; `write_all` 82 / 34.

1. **Exit-only staging (G-WALLAT-02/G-WALL-02).** The per-iteration staging
   block (`write_all_at` 0x1650-0x167f, 10 instr incl. the 16-B `movdqa`
   load + `movaps` store; `write_all` 0x1d50-0x1d7f) leaves the loop;
   confirmed/total/end/error live in registers; the outcome is built once at
   exit (the A1 arm demonstrates the shape: 3-instr per-iteration analogue).
   Prediction: −7 instr/iteration floor (`write_all_at`), −4 (`write_all`);
   1-step −8…−12 / −5…−8. Objdump gates: no staging store sequence
   (`0x40/0x48/0x4c`-class stores + `movdqa` re-load) on the loop-back path;
   staging appears only on exit paths.
2. **Dead-store elimination (G-WALLAT-03/G-WALL-03; canary of 1).** The
   never-read 16-B stores `movaps %xmm0,0x20(%rsp)` (`write_all_at` 0x1677)
   and `movaps %xmm0,0x10(%rsp)` (`write_all` 0x1d77) vanish. Prediction: −1
   store16/iteration (sub-MDE by construction; instruction-caliber evidence).
   Objdump gate: zero textual occurrences of both displacement stores in the
   candidate disassembly (grep over `objdump -d`).

**Pinned static pass bounds (audit convention recount):** `write_all_at`
per-extra-step ≤ 37 (from 44), 1-step ≤ 93 (from 101); `write_all`
per-extra-step ≤ 30 (from 34), 1-step ≤ 77 (from 82); public `write_at` 40 /
`write` 29 unchanged (±0); impossible-count guard still present. Failing the
bounds (staging survives the source change) falsifies the transform — the A1
arm's demonstrated shape is the feasibility reference.

### 4.4 Measurement units (warm tmpfs; Amendment-1 per-cell scratch; 16 ABBA)

Scratch per cell: `/tmp/sluice600/p2-<cellid>.bin` (mkfixture before, delete
after). `$LADDER_B1`/`$LADDER_C2` as §3.4. 4 KiB shape `4096 1024 1024 5 16`.

| id | pairing | binary | command shape |
|---|---|---|---|
| p2c2-aa-write-4k-base | w1 vs w1 | B1 | `$PAIRED $LADDER_B1 w1 w1 4096 1024 1024 5 16 <scratch>` |
| p2c2-aa-write-4k-cand | w1 vs w1 | cand | same on `$LADDER_C2` |
| p2c2-base-composition-4k | w1x vs wx | B1 | `$PAIRED $LADDER_B1 w1x wx 4096 1024 1024 5 16 <scratch>` → **S_w**, P_base |
| p2c2-cand-composition-4k | w1x vs wx | cand | → **D_cand_w**, P_cand |
| p2c2-delta-composition-4k | wx(B1) vs wx(cand) | both | two-binary ABBA → Δ_own_w (measurability probe) |
| p2c2-base-cursor-composition-4k | w1c vs wc | B1 | first-ever `write_all` wall cell |
| p2c2-cand-cursor-composition-4k | w1c vs wc | cand | candidate twin |
| p2c2-delta-cursor-composition-4k | wc(B1) vs wc(cand) | both | twin delta |
| p2c2-negwrite-4k | w1 vs w2 | cand | negative control |
| p2c2-negwrite-1b | w1 vs w2 1 B | cand | `… w1 w2 1 4096 4096 5 16 <scratch>` |

Multi-step shapes: no wall cell exists or is added (scripted short writes are
test-seams territory, `src/file_test_seams.hpp` NativeCall enum verified to
carry pread/pwrite injection); multi-step behavior is covered by the §2.1
killer set (`short_writes_reach_full_progress`, zero-progress, EINTR,
impossible-count) and the per-extra-step static recount — disclosed as a
caliber limitation (P1 §11.7 analogue).

### 4.5 Pass rules (preregistered)

- **G1 machine (gating):** §4.3 objdump gates + recount bounds + public-op
  shapes unchanged.
- **G2 correctness (gating):** `p2c2-mutants.sh` exit 0 (its seam subset:
  the four compose seams + the new write-side impossible-count script) at
  seams-only and mechanism commits; `p2c2-matrix.sh` exit 0.
- **G3 instruction caliber (primary):** static bounds met AND PMU corroboration
  P_cand ≤ P_base − 20 instr/op.
- **G4 wall:** no regression (D_cand_w not EXCESS; negative controls PARITY);
  Δ_own_w recorded; ≥ MDE improvement = measurability upgrade (optional).
- KEEP-eligible (instruction caliber) iff G1+G2+G3+G4; product admission of a
  wall claim additionally requires the measurability upgrade. Static bounds
  failing → mechanism falsified → REJECT. G2 red → BLOCKED_CORRECTNESS.

## 5. Deferred register entries (with ledger reasons)

| ID | Reason (ledger-cited) |
|---|---|
| G-READAT-01 (+ read twin) | Null-data guard (3 instr + 1 not-taken branch, `blocking_file.cpp:86-87`/`:126-127`): delete-or-anchor-in-root is an owner decision under the AGENTS.md mechanism-justification gate (register: "delete the guards … **or** anchor the defensive rejection in the root"); the frozen N1 comparator carries the same check, so removal yields no beyond-parity gain and a successor N1 arm must drop it in lockstep — a semantics decision, not a P2 performance candidate. |
| G-WRITEAT-02 | Same decision gate as G-READAT-01 (register); 2–3 instr + 1 branch per site, ≪ MDE; A1-pattern comparator carries it. Deferred with G-READAT-01 to the owner gate. |
| G-READAT-02 | Register marks it record-only, not proposed for admission: EINTR re-entry re-spill (4 of 7 re-entry instructions), error-path only, no admitted workload triggers it at measurable frequency. |
| G-META-01 | Admissibility gate has no executable form: the build defines no msan mode (verified this session: `xmake.lua:3-16` offers only debug/release/valgrind + asan/tsan/ubsan/asanubsan) and ASan/UBSan do not detect uninitialized reads, so the 4-config matrix cannot arbitrate; requires adding an msan mode or equivalent uninit oracle before the noinit variant may be admitted. Static-only until then. |
| G-OPEN-01 | Measured PARITY (open+close pair +11.487 < max(10.216, 1%·1596.9)=15.969, CI spans 0); the register's own falsifier anticipates that no repeatable gain (kernel floor dominates) is rejected by the #600 admission rule for standalone admission. Static ablation only if ever re-opened. |
| G-WRITEAT-01 | RESOLVED by the P1 campaign (write ladder executed: wall PARITY at every claimable size, +19 instr/op PMU constant, syscall parity); entry retained for traceability; only a cold-write A/A would ever be needed if a cold-write claim arises. |

## 6. Limitations and caveats (preregistered)

1. Per-item ns splits inside the composition gaps remain static-derived
   (`DIRECT_COST_FLOOR.md` §6.3); P2C1's combined-branch design means a wall
   refutation refutes the package, not individual items — per-item dynamic
   isolation is out of P2 scope and stated as such.
2. The C2-null boundary (§0) justifies the package only for the read side;
   P2C2's staging factor has no prior null either way — its first dynamic
   evidence is this experiment.
3. Cross-binary cells compare two archive builds; the N1 bridge check (§2.3)
   and pinned public-shape gates bound comparator drift, but PLT/layout
   differences between builds are disclosed caliber structure, not eliminated.
4. Wall claims are E5-native-Linux only; PMU absolute values are whole-process
   approximations whose cross-arm deltas are the signal; static disassembly is
   the authority for absolute counts (P1 §6/§11).
5. The 2× settlement (H0) adjudicates the *baseline magnitude* contradiction;
   it cannot retroactively explain #600's number — candidate explanations are
   recorded, not proven, per `DIRECT_COST_FLOOR.md` §6.2.
6. No tail-latency claim: per-op sampling clock overhead ≈26 ns exceeds the
   predicted effects (P0 §4.3); P99 cells are out of P2 scope pending the P4
   gate resolution.
7. If sudo/cold discipline or any prerequisite becomes unavailable:
   `NOT_RUN_WITH_REASON` in the cell record; no substitution (P1 §7
   precedent). P2 has no cold cells; the rule carries for completeness.

## Amendment log

None. Deviations during execution are appended here with trigger, change, and
reason — never applied silently.
