# P2 Candidate Report — P2C2 write-composition exit staging

- Family: G-WRITECOMP-01 = {G-WALLAT-01, G-WALLAT-02, G-WALLAT-03} + twins
  {G-WALL-01, G-WALL-02, G-WALL-03} (`CANDIDATE_GAP_REGISTER.md`).
- Plan: `P2_EXPERIMENT_PLAN.md` §4 (hypothesis §4.2, mechanisms §4.3, cells
  §4.4, pass rules §4.5). Mission hooks: MISSION §4 P2.2/P2.3.
- Branch: `direct-write-compose-exit-staging` (local-only, never pushed), cut
  from B1 `77e5399a45998dea396699fa2e063c94a665945a`; worktree
  `/home/jnhu/Source/Sluice-direct-write-compose-exit-staging`; HEAD
  `476bd9ce9780594163350228e066a86b554c157d` `[RUN]` (`git rev-parse`,
  `git log 77e5399a..HEAD` → 2 commits).
- Candidate release archive sha256
  `672510281abadba1cbda0a0e89f15e1dbb2c69f5a15bceb306b70196b420a807`
  `[RUN]` (`sha256sum`; identical to `p2-raw/P2C2/matrix-release-archive-sha256.txt`).
- **Decision: KEEP — instruction caliber** (§9; G1+G2+G3+G4 all pass per
  §4.5). No wall claim is made or admitted: the H2 measurability upgrade was
  not achieved (and was optional).
- Provenance legend: identical to `P2_CANDIDATE_P2C1.md` — `[RUN]` = writer
  this session; `[RUN-IMPL]` = implementation session, review passed
  ("门禁与复核均通过", zero verify problems); `[RUN-MEAS]` = measurement
  session, raw archived; `[TRANSCRIBED]`/`[UNCERTAIN]` per P0 §0.

## 1. Source diff

Cumulative `git diff --numstat 77e5399a..476bd9ce` `[RUN]` (writer-run): 5
files, +249/−40 — `blocking_file.cpp` +45/−40, `P2C2-matrix.sh` +49/−0,
`P2C2-mutants.sh` +106/−0, `tests/direct_composition_fault_test.cpp` +27/−0,
`xmake/tests.lua` +22/−0. Per-commit (writer-run numstat; the implementation
record's prose attributions to the small files differ by a few lines — +17,
+25, +26, "+114", "−57/+35", "+3/−3" — while both commit totals match
exactly):

- `b6dacc89` (seams + killers), +226/−1: `src/blocking_file.cpp` +24/−1 —
  four `SLUICE_COMPOSE_MUTANT_*` compile-time seam guards embedded in the
  B1-shaped compose/`direct_outcome`, dormant (zero BASE code difference; see
  §2.2 bit-identity proof); `xmake/tests.lua` +22/−0 (six
  `direct_mutant_target()` targets incl. the detail rule-suite mutant
  control); `tests/direct_composition_fault_test.cpp` +27/−0 (new named
  killer `write_all_impossible_count_stops_immediately`, placed before its
  read counterpart); `p2-scripts/P2C2-matrix.sh` +47 / `P2C2-mutants.sh`
  +106.
- `476bd9ce` (mechanism), +41/−57: `src/blocking_file.cpp` +37/−55 — compose
  template rewritten: confirmed-progress register-carried; impossible-count /
  zero-progress / error / complete exit flavors each materialize
  `CompositionOutcome` at a single point; `direct_outcome`/`publish_certainty`
  deleted (no remaining consumer); the four seam guards migrate with the
  mechanism, semantics unchanged; impossible-count guard and per-step
  closed/access re-tests retained (G-WALLAT-01/G-WALL-01 deliberately kept —
  single-factor discipline, plan §1); `P2C2-matrix.sh` +4/−2 (ANSI
  adjudication fix).

Public `read_at`/`read`/`write_at`/`write` source is untouched by this
candidate (plan §1 scope pin; machine-verified in §2.2). Read-side
compositions share the rewritten compose template, so they inherit the
transform incidentally — disclosed in §2.4, no read-side claim is made by
this candidate.

## 2. Machine-work delta (static + PMU)

### 2.1 Static, implementation session `[RUN-IMPL]`, review-passed

Counter: the session rebuilt the audit-convention closed-interval path counter
(`/tmp/p2c2-count/count_paths.py`, one-off probe per AGENTS rules) and first
reproduced the p1-audit appendix-A reference counts on the frozen B1 archive
(`write_all_at` 101/44, `write_all` 82/34 — exact match) before counting the
mechanism tree. Same counter, same convention both sides.

| Mechanism (plan §4.3) | Objdump gate | Result |
|---|---|---|
| W-M1 exit-only staging (G-WALLAT-02/G-WALL-02) | no staging-store sequence (`0x40/0x48/0x4c`-class stores + `movdqa` reload) on the loop-back path; staging only on exit paths | back-edge blocks (`write_all_at` 0x15d8–0x15f7, `write_all` 0x1c80–0x1c9e: impossible/zero-progress/advance + closed/access re-tests) carry no staging store and no `movdqa` reload; B1 per-iteration staging blocks (0x1650–0x167f / 0x1d50–0x1d7f, 10 instr each) gone; outcome materialized once per exit flavor (complete 0x1630–0x163e / 0x1cd0–0x1cde and the error exits) with the shared sret tail |
| W-M2 dead-store elimination (G-WALLAT-03/G-WALL-03; canary) | zero textual occurrences of `movaps %xmm0,0x20(%rsp)` (B1 `write_all_at` 0x1677) and `…,0x10(%rsp)` (B1 `write_all` 0x1d77) | 0/0; the file's remaining 19 `movaps` all sit at 0x30–0xc0(%rsp) (the `file_info`/`size` 144-B stat clear, G-META-01 zone, untouched) |

Pinned recount bounds (plan §4.3) `[RUN-IMPL]`, all PASS:

| Path | B1 | Candidate | Bound | Verdict |
|---|---|---|---|---|
| `write_all_at` 1-step | 101 | 81 | ≤ 93 | PASS (−20) |
| `write_all_at` per-extra-step | 44 | 35 | ≤ 37 | PASS (−9; predicted band −7…−10, in band) |
| `write_all` 1-step | 82 | 62 | ≤ 77 | PASS (−20) |
| `write_all` per-extra-step | 34 | 25 | ≤ 30 | PASS (−9; predicted band −4…−7 — **exceeds the band**, favorable direction, disclosed: the 1-step −20 decomposes as per-extra −9 + complete-exit/publish single-point materialization −11, which is inside the mechanism's description "outcome built once at exit", not a second factor; plan §4.3 REFUTATION adjudicates only on the pinned bounds, all PASS) |
| public `write_at` | 40 (audit) / 39 (session caliber) | 39 | ±0 same-caliber | PASS (the ±1 vs the audit number is an endpoint-nop counting difference, disclosed; the ±0 judgment is same-caliber on both sides) |
| public `write` | 29 | 29 | ±0 | PASS |

Impossible-count guard retained on the back-edge (`write_all_at` 0x15d8
cmp/jb, `write_all` 0x1c80 cmp/jb) and still mutation-reachable (§4).

Non-gating predictions, reported as the plan requires ("reported either way,
not gating") `[RUN-IMPL]`: the caller-saved `total` spill pair
(`write_all_at` 0x141b/0x1425 → 0x13de/0x13e8; `write_all` 0x1b3e/0x1b48 →
0x1ab2/0x1abc) and the success-path EINTR feeder store (0x140d → 0x13d0,
0x18(%rsp)) did **not** disappear — register allocation did not shift to the
A1 callee-saved shape. Recorded, not gating.

### 2.2 Writer re-verification `[RUN]` (this session)

On the frozen B1 archive (`d1e351a5…`), the P2C1 archive, and this
candidate's archive (`67251028…`):

- W-M2 dead-store gate re-run by the writer: `objdump -d` of the extracted
  `blocking_file.cpp.o` gives `movaps…0x20(%rsp)` = 2 and `movaps…0x10(%rsp)`
  = 2 in B1 (0x1677/0x1d77 write side + 0x1277/0x19f7 read side — the "B1
  4 处" of the measurement note) and **0/0 in this candidate**. Gate holds on
  the delivered artifact.
- Public-op ±0 at instruction-stream level: address-stripped disassembly of
  `sluice::blocking::read_at/read/write_at/write` is identical (sha-equal)
  across B1 and this candidate.
- Composition symbols shrink: whole-function lines `write_all_at` 242→211,
  `write_all` 209→195 (writer's counts; read twins 242→226 / 210→196 — the
  shared-template incidental effect).
- BASE bit-identity of the seams `[RUN-IMPL]`, writer re-verified `[RUN]`:
  with the seam defines dormant, the rebuilt release archive is bit-identical
  to the frozen B1 archive — writer check this session: scratch
  `git archive b6dacc89` export, `xmake f -c -m release --liburing=n -y &&
  xmake -y` (exit 0), archive sha256 =
  `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0` —
  exactly the pinned frozen-B1 value (record:
  `p2-raw/P2C2/b6dacc89-dormant-seam-rebuild-sha256.txt`); the implementation
  session additionally reports the object file `blocking_file.cpp.o`
  bit-identical. The seam commit alone changes zero delivered code.

### 2.3 Measurement-session static segment `[RUN-MEAS]` — NOT_RUN disclosure

As on the read side, the measurement segment did not re-run the §4.3
audit-convention per-path recount; symbolic substitutes disclosed
(`write_all_at` 215→199 objdump lines, `write_all` 192→177; dead stores 4→0 —
writer-confirmed §2.2; call shape 1×pwrite64 + 1×__errno_location; public
`write_at`/`write`/`read_at`/`read`/`file_info` symbols IDENTICAL). Its note
"G3 完整合取待重计后闭合" is about that segment's own evidence chain; the
gated static half of record is the review-passed implementation-session
recount (§2.1) plus the writer's artifact checks (§2.2). The recount re-run
remains available if the owner wants the conjunction re-closed on fresh
binaries.

### 2.4 PMU (H3 corroboration, gating for G3) `[RUN-MEAS]`, raw re-read `[RUN]`

Whole-process `instructions:u`, per arm /4096 ops; never a wall source.

- Fresh P_base (`p2-raw/p2c2/p2c2-base-composition-4k-pmu.txt`):
  WX 1,145,238,109 − W1x 1,144,975,649 = 262,460/4096 = **+64.076 instr/op**
  (P1 session reference +58.1 — same order, 6.0 instr/op above it; non-gating
  baseline corroboration caliber).
- P_cand (`p2c2-cand-composition-4k-pmu.txt`): WX 1,145,155,960 − W1x
  1,144,975,633 = 180,327/4096 = **+44.030 instr/op**.
- Gate: P_cand ≤ P_base − 20 → 44.030 ≤ 44.076 → **PASS with margin 0.046
  instr/op — marginal, flagged as such** (static −8…−12 at the observed ≈2.0×
  PMU/static ratio). The pass stands as preregistered; the margin is part of
  the record.
- Twin: WC−W1c 45.26 → 27.06 instr/op (drop 18.2); N1-arm instruction counts
  stable across binaries (w1x −16 of 1.14 G, w1c +828) — comparator drift
  bounded.

### 2.5 Read-side incidental disclosure (no claim)

Because the compose template is shared, the read compositions inherit the
same transform: `read_exact_at` 1-step 100 (audit; 98 session caliber, extra
44 exact; the 1-step ±2 is an inferred endpoint) → 81, per-extra 44→35;
`read_exact` 82→62, 34→25 — structurally identical to the write twins
`[RUN-IMPL]`. P2C2's bounds and verdicts pin the write side only; the read
side belongs to P2C1's family and is adjudicated there.

### 2.6 Syscall parity `[RUN-MEAS]`

1.000 `pwrite64`/op ≥ 1 B (positional) or 1.000 `write`/op (cursor); 0.000 at
0 B; both binaries identical (whole-trace net counting; the first
`-e trace=pread` attempt was rejected by this host's strace — disclosed,
method switched). Tail 2.6667 = 2× the preregistered 1.3333 (timed-window
caliber note, arm-identical). Raw: `p2-raw/p2c2/*composition*.syscalls.txt`.

### 2.7 Independent re-verification status (inputs for the P5 re-review)

Not independently re-executed by any later session (the writer's §2.2 checks
are the weaker symbol-level gates; the measurement segment likewise did not
re-run it, §2.3): the audit-convention per-path instruction recount of §2.1 —
the one-off /tmp counter and the session-derived candidate address intervals
are not archived under `p2-raw`, so the 81/35/62/25 numbers and their
bound-PASS verdicts rest on the review-passed implementation record (the
counter itself, `/tmp/p2c2-count/count_paths.py`, still exists but was not
re-run by the writer or the independent reviewer). Also carried from the run
material with no `p2-raw` archive: the implementation-phase review conclusion
itself ("门禁与复核均通过", zero verify problems). The dormant-seam BASE
bit-identity formerly in this list was closed by the writer's rebuild (§2.2).
Neither remaining item changes a gate derivation above that is not already
grounded in archived raw or writer-run checks as labeled; both are explicit
inputs to the P5 independent re-review.

## 3. Semantic proof (against P0 §5)

Floor: `P0_BASELINE_AND_OWNERSHIP.md` §5.1. The candidate touches only the
compose loop internals; public ops machine-identical (§2.2); smoke gate
"ALL GATED GROUPS IDENTICAL" over the write family {w0,w1,w2,w1x,wx,w1c,wc}
with `content_ok=1` (0 B cells −2 skipped, P1 convention), metadata×3, closed
group, EINTR diagnostics `[RUN-MEAS]` (`p2-raw/kit/smoke-ladder.txt`, verdict
line verified `[RUN]`).

Composition-internal obligations and their named oracles:

| Obligation (P0 §5.1 anchor) | Oracle | Status |
|---|---|---|
| Partial write is success; no retry-to-full in the primitive (SEM-05) | `short_writes_reach_full_progress`, `primitive_short_write_returns_without_retry_to_full`, frozen `semantic_short_io_reference_test` table | seam-provable (§4); suite green |
| No-progress stop (SEM-05) | `zero_progress_write_stops_after_one_attempt` (`script.calls() == 1`), `write_all_zero_progress_stops_and_keeps_its_prefix`, `composition_never_spins_on_zero_progress` | seam-provable; suite green |
| Confirmed bytes / accounted-vs-unknown remainder (ERR-02) | `error_after_confirmed_prefix_keeps_the_prefix`, `a_primitive_error_reports_an_unaccounted_remainder_in_both_directions` | seam-provable; suite green |
| EOF-equivalent write boundary / advance rule (SEM-05) | `read_exact_reports_eof_before_full_with_the_prefix` (direct_composition_test leg), `composition_advances_the_buffer_by_confirmed_bytes` | seam-provable; suite green |
| Impossible-count stop | `write_all_impossible_count_stops_immediately` (new) + `impossible_count_stops_immediately` | guard kept; seam-provable (§4) |
| EINTR retry without count (SEM-05/ERR-01) | `primitive_write_retries_eintr_then_stops_at_the_first_count` | suite green (no EINTR seam in this candidate's set — the EINTR seam is P2C1's §2.1 row; the write EINTR oracle still runs green in the matrix) |
| Closed/access precedence, range, zero-length, null (SEM-03/04) | 58-test matrix incl. executed direct 21/21 precedence comparison (P0 §5.2) | matrix green ×4 configs (§4) |

Authority-contraction audit (recorded with the mechanism commit `[RUN-IMPL]`):
`direct_outcome`/`publish_certainty` deleted (no remaining consumer);
`detail::compose_progress`/`compose_error`/`composition_error`/
`composition_effect_certainty` step off the production path but are retained
as the SEM-05 rule table — the P0 §5 oracle directly consumed by
`semantic_short_io_reference_test` and the fault-test stop-reason rules; the
P2C2 mutants gate additionally proves the rule suite stays green *under
mutation* (`compose_mut_rules_stay_green_short_io` control, §4). The
mechanism adds no runtime state.

Review conclusion (run material): implementation stage VERIFIED, gate note
"门禁与复核均通过", zero verify problems.

## 4. Mutation evidence (seams and named kills)

Gate: `p2-scripts/P2C2-mutants.sh` (committed on the branch), exit-code
adjudicated, RED = named-assertion failure only.

- Exit 0 on the seams tree and the mechanism (= final) tree `[RUN-IMPL]`;
  records re-read `[RUN]` (`p2-raw/P2C2/mutants-20261027T*.txt`, six runs).
  Record→tree mapping is timestamp-inferred because the P2C2 script does not
  write a tree head into its records (disclosed evidence-structure limitation
  — unlike P2C1's `mutants-head.txt`; fix before reuse): `T111328` (11:13:28)
  predates the seams commit `b6dacc89` (11:13:55) by 27 s — pre-commit
  working tree with the seam content; `T111608` (11:16:08) is the seams tree
  `b6dacc89` (the preregistered pre-mechanism leg); `T112757` (11:27:57)
  postdates the mechanism commit `476bd9ce` (11:27:31) by 26 s — mechanism
  tree; `T205824`/`T211413`/`T221148` are final-tree `476bd9ce` re-checks
  (commit timestamps verified this session `[RUN]`, `git log --format=%cI`).
  Both preregistered legs are therefore archived; the era labels in an
  earlier draft of this section ("11:13/11:16/11:27 = seams era") were
  imprecise and are corrected here.
- Final record, per seam GREEN→RED(named)→RESTORED GREEN:
  `ADVANCE_BY_REQUESTED` (fault_test leg →
  `short_reads_report_eof_before_full_with_the_prefix`; direct_composition_test
  leg → `read_exact_reports_eof_before_full_with_the_prefix`),
  `RETRY_ZERO_PROGRESS` → `short_reads_report_eof_before_full_with_the_prefix`,
  `FAILURE_ACCOUNTED` → `error_after_confirmed_prefix_keeps_the_prefix`,
  `SKIP_IMPOSSIBLE_COUNT` → **`write_all_impossible_count_stops_immediately`**
  (the new write-side script) — plus the control: detail rule suite GREEN
  under the mutant define.
- Named-kill completeness for the plan §2.1 write-side killers that cannot be
  first-kill in a single-main suite (`short_writes_reach_full_progress`,
  `zero_progress_write_stops_after_one_attempt`): proven by independent
  probes (`/tmp/p2c2-killprobe/`, one-off per AGENTS rules) — under
  `-DSLUICE_COMPOSE_MUTANT_*` exit 1 with the corresponding `FAIL:` name on
  stderr; BASE probe exit 0 `[RUN-IMPL]`. The independent report review
  (2026-10-27) re-executed the probes itself with matching results
  (`probe_base` exit 0; `ADVANCE_BY_REQUESTED` → exit 1,
  `FAIL: short_writes_reach_full_progress`; `RETRY_ZERO_PROGRESS` → exit 1,
  `FAIL: zero_progress_write_stops_after_one_attempt`).
- Disclosed gate-scope decision: the read-EOF real-file killer
  (`read_exact_reports_eof_before_full_with_the_prefix` in
  `direct_composition_test`) *deadlocks* under `RETRY_ZERO_PROGRESS` (EOF
  returns 0 forever once the mutant restores re-entry) — a timeout is not a
  kill, so that suite is not used for that seam; the seam is gated by the
  fault_test scripted killers (read and write sides) plus the probes above.

Matrix gate: `P2C2-matrix.sh` exit 0 — release/debug/asan/asanubsan
(`--liburing=n`) each 58/58, 0 failed `[RUN]` (re-read
`p2-raw/P2C2/matrix-{release,debug,asan,asanubsan}.txt`). First run exited 1
falsely: the grep adjudication was blinded by xmake ANSI color codes (all four
legs had 0 failed); fixed to strip color before matching and re-run to exit 0
— disclosed as a gate-script bug, corrected, not a suite failure.

## 5. E5 A/A (session-opening noise floors)

`[RUN-MEAS]`; write-base attempt chain re-read `[RUN]` from the raw headers:
attempt1 valid but sd_AA = 17.021 ns (> 10 ns #600 band) → one §11.10 quiesce
rerun accepted (sd_AA = 18.463):

| Cell | sd_AA | MDE′ | Recorded verdict |
|---|---|---|---|
| `p2c2-aa-write-4k-base` (B1 binary) | 18.463 ns | 9.833 ns | accepted; *narrower* than pinned 15.695 → one-way rule: pinned floor retained for decisions, both reported |
| `p2c2-aa-write-4k-cand` (candidate binary) | 14.128 ns | 7.526 ns | accepted; noise-floor delivery only (not a beat-target unit) |

## 6. Paired A/B (16 ABBA pairs; warm tmpfs; Amendment-1 per-cell scratch —
mkfixture before, scratch+lsof isolation, deleted after; fixture/scratch
integrity green every cell. The kit-phase read-fixture incident — smoke
bridge wrote arms onto `data64m.bin`, caught by the first A/A content-oracle
gate, fixture deterministically rebuilt to the pinned sha before any accepted
cell — is shared measurement-session history and is recorded in full in
`P2_CANDIDATE_P2C1.md` §6; no write cell consumed the affected fixture, and
every write cell used its own Amendment-1 scratch.)

### 6.1 Fresh baseline — `p2c2-base-composition-4k` (B1 binary, W1x vs WX)

Raw `p2-raw/p2c2/p2c2-base-composition-4k.txt`: **S_w = +15.541 ns** median
EXCESS (mean +12.741, mean-CI [+2.045, +23.436] excludes 0, 12/16) — but
|median| = 15.541 < MDE 15.695 by 0.154 ns → per the P1 rule as preregistered:
**wall PARITY at the margin, disclosed as marginal** (MDE′ 9.833 is narrower;
the one-way rule keeps the pinned floor, so PARITY stands; the marginality is
recorded, not silently resolved). This reproduces the register's standing
fact — the write gap is instruction-caliber, not wall-caliber. P_base =
+64.076 instr/op (§2.4).

First-ever `write_all` wall cells on B1: WC−W1c = **+7.834 ns** (CI excludes
0 but sub-floor → PARITY, §6.3).

### 6.2 H2 wall probe — `p2c2-delta-composition-4k` (cross-binary, wx, cand−baseline)

Raw: candidate median 773.650 / B1 median 782.327; **Δ_own_w = −8.812 ns**
median (mean −1.071, sd 14.516, CI [−8.805, +6.663] **includes 0**, 11/16
cand-faster — i.e. **5 positive pairs** (+10.05, +11.601, +14.131, +25.475,
+30.975 ns; count corrected after independent review from the raw 16 deltas)
pull the mean up — median/mean divergence disclosed). Sub-MDE 15.695 →
**no wall measurability claim**
(exactly the preregistered expectation: predicted ≈4–7 ns < MDE,
`[UNCERTAIN]` scaled). Recorded, not claimed. No wall regression (§6.3).

### 6.3 G4 endpoint and twins on the candidate binary

- `p2c2-cand-composition-4k`: W1x 772.458 vs WX 775.929; paired delta
  (W1x−WX) median −3.356 ns, mean-CI [−8.179, +9.427] includes 0;
  EXCESS-convention D_cand_w = **+3.356 ns** < max(15.695, 7.725) →
  **PARITY — no wall regression** (baseline S_w was already sub-MDE PARITY;
  the candidate sits closer to zero).
- `p2c2-cand-cursor-composition-4k` (H4 first-measurement candidate side):
  W1c 782.290 vs WC 787.130; paired delta (W1c−WC) median −4.430 ns, mean-CI
  [−9.159, −1.347] excludes 0; EXCESS-convention D = **+4.430**
  (EXCESS-sign mean-CI [+1.347, +9.159]) < max(15.695, 7.823) → PARITY
  (improved from baseline +7.834, still sub-floor).
- `p2c2-delta-cursor-composition-4k` (cross-binary twin): Δ = **−8.637 ns**
  median (mean −8.432, sd 5.442, CI [−11.331, −5.533] excludes 0, **16/16**
  cand-faster) — sub-MDE, recorded as twin delta only, consistent
  same-direction corroboration of the instruction-caliber claim.

## 7. Benefit boundary

- Admitted benefit class: **instruction caliber only** — per-extra-step
  staging elimination (−9 static, in-prediction for `write_all_at`, above-band
  favorable for `write_all`), 1-step −20 static, dead 16-B stores 4→0
  (writer-verified), PMU excess +64.076 → +44.030 instr/op (drop 20.05 across
  the preregistered ≥20 threshold, margin 0.046 — marginal, flagged).
- Wall: no benefit claimed. Δ_own_w sub-MDE with CI spanning 0 (§6.2); D_cand_w
  PARITY (§6.3). Per plan §4.5, product admission of a wall claim would
  additionally require the measurability upgrade — not achieved, not needed
  for the instruction-caliber KEEP.
- No benefit is claimed for: public single ops (machine-identical; controls
  §8); G-WALLAT-01/G-WALL-01 per-step re-validation (retained by the fixed
  admission decision; its cost is *attributed* inside the residual P_cand
  +44.03, not eliminated — the read-side C2 null bounds only the read twin of
  that item, plan §0/§1); register-caliber spill relief (predicted
  conditionally, did not materialize, reported per plan §4.2); multi-step
  wall shapes (no cell exists or was added — scripted short writes are
  test-seam territory; multi-step is covered by the §4 killer set and the
  per-extra-step static recount; disclosed as a caliber limitation, plan
  §4.4); read-side effects (§2.5); P99/tail latency (out of scope); non-E5
  hosts.

## 8. Negative controls (G4 component)

- `p2c2-negwrite-4k` (w1 vs w2 @ candidate): W1 767.594 / W2 771.505; delta
  median **−4.567 ns**, CI [−11.796, +0.519] **includes 0** → +4.567 <
  max(15.695, 7.676) → **PARITY, PASS** (raw `p2c2-negwrite-4k.txt`).
- `p2c2-negwrite-1b`: delta median **−4.059 ns**, CI [−11.672, −0.570]
  excludes 0 but +4.059 < max(15.695, 5.884) → sub-floor **PARITY, PASS**
  (EXCESS requires ≥ MDE). Consistent with the P1 record: `write_at` carries
  a small stable sub-MDE excess (CI-excluding, far below the write floor) —
  pre-existing, not introduced by this candidate.

## 9. Decision — KEEP (instruction caliber)

Gate-by-gate derivation (plan §4.5):

| Gate | Rule | Recorded outcome | Verdict |
|---|---|---|---|
| G1 machine (gating) | §4.3 objdump gates + recount bounds + public-op shapes unchanged | gates pass (impl review-passed; W-M2 dead-store gate and public-op identity writer-verified `[RUN]` §2.2); bounds 81≤93, 35≤37, 62≤77, 25≤30; write_at/write ±0 same-caliber; guard retained; measurement-segment recount NOT_RUN with symbolic substitutes disclosed (§2.3) | **PASS** |
| G2 correctness (gating) | mutants.sh exit 0 (seam subset) at seams-only and mechanism commits; matrix.sh exit 0 | exit 0 ×3 trees incl. rule-suite-under-mutant control (§4); matrix exit 0, 58/58 ×4 legs | **PASS** |
| G3 instruction caliber (primary) | static bounds met AND P_cand ≤ P_base − 20 | bounds met (§2.1); 44.030 ≤ 64.076 − 20 = 44.076 (§2.4) | **PASS — margin 0.046 instr/op, marginal, flagged** |
| G4 wall | D_cand_w not EXCESS; negative controls PARITY; Δ_own_w recorded; upgrade optional | D_cand_w +3.356 PARITY; negwrite-4k/1b PARITY (§8); Δ_own_w −8.812 sub-MDE recorded (§6.2); upgrade not achieved (optional) | **PASS** |

KEEP-eligible (instruction caliber) iff G1+G2+G3+G4 → **all pass → KEEP**,
scoped as follows:

1. The KEEP is **instruction-caliber**: no wall improvement is claimed or
   admitted; the optional measurability upgrade was not achieved.
2. The G3 PMU margin (0.046 instr/op) is marginal; it passes the
   preregistered threshold as written, and the marginality is part of the
   record. Any future re-arbitration at a stricter threshold is a new
   preregistered decision, not a retroactive one.
3. The measurement segment's own recount re-run (NOT_RUN there, §2.3) is
   recorded as open optional work; the gated static evidence of record is the
   review-passed implementation recount plus the writer's artifact
   verification.

Terminal-state record: no GATE_FAILED or IMPL_FAILED candidates exist in this
run — P2C2 reached VERIFIED implementation (review passed, zero problems) and
full measurement; every gate it failed along the way (the two gate-script
defects: ANSI blinding in both scripts' first runs) was a tooling bug
disclosed, fixed and re-run to exit 0, never a suite or mutant failure.

## 10. Deferred register entries (plan §5, shared across P2)

Identical table to `P2_CANDIDATE_P2C1.md` §10: G-READAT-01, G-WRITEAT-02,
G-READAT-02, G-META-01, G-OPEN-01, G-WRITEAT-01 — owner-gate semantics
decisions, record-only items, a gate with no executable oracle, a measured
PARITY rejection, and a P1-resolved entry retained for traceability
respectively.

## 11. Combined (P2C1+P2C2) outcome

**Not executed.** MISSION P2.2 permits an additional combined measurement
only when the single factors all hold and forbids adding independent gains.
This session's precondition was not met (P2C1's G5 negative-control gate is
formally unresolved — see `P2_CANDIDATE_P2C1.md` §8/§9), and no combined cell
was measured. The two candidates' shared-template interaction is bounded
statically (each candidate's archive was measured against the same frozen B1
baseline; the read-side incidental effect of this candidate is disclosed in
§2.5 and adjudicated under P2C1) — but no stacked wall/PMU cell exists, and
none is claimed.
