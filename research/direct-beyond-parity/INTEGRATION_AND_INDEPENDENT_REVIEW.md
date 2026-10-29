# P5 Integration and Independent Review — Final Record

- Authority: `MISSION.md` §7 (P5.1 interaction matrix, P5.2 independent
  correctness review, P5.3 independent performance review; MISSION.md:733-800).
  Adjudication rules are the preregistered `P5_INTERACTION_PLAN.md` §4
  (S0–S6 conjunction and fallback), fixed before any P5 branch, build or
  measurement. The verdict carrier produced at review close is
  `P5_INTERACTION_OUTCOME.md`; this document is the English consolidation of
  the matrix results, both independent reviews, the fix-round record, and the
  final composition recommendation.
- Objects under review (identities re-read this session from
  `p5-raw/identity.txt` and re-verified against the repository, §1):
  - **BASE (B1)** — frozen archive
    `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0`,
    worktree HEAD `77e5399a45998dea396699fa2e063c94a665945a`.
  - **A = P2C1** — `direct-read-compose-prevalidated-steps` (local-only),
    HEAD `deaf03014173e13633a43f4db405c9a9e9860f23`, archive
    `07d06803…2049e`. Frozen KEEP (P2C1 §12.5; P4 `WIN_CONFIRMED` ×3).
  - **B = P2C2** — `direct-write-compose-exit-staging` (local-only), HEAD
    `476bd9ce9780594163350228e066a86b554c157d`, archive `67251028…20a807`.
    Frozen KEEP — instruction caliber only (P2C2 §9).
  - **A+B** — `direct-compose-stack-readpreval-writeexit` (local-only),
    HEAD `4ab6ef170ff939001cc1459445b77c21df9e7d86`. Composition gates
    passed at this HEAD (the gate-pass input to this record: S0 mutant and
    matrix gates, §2.1).
- Dates: measurements and gates 2026-10-28 (P5 sessions); independent
  reviews 2026-10-28; review close (fix round) 2026-10-28. Host E5, kernel
  `7.2.11-200.fc44.x86_64`, governor schedutil, `perf_event_paranoid` 2
  (`p5-raw/env-pin-2026-10-28.txt`).

Legend (carried from the mission's P0 convention, adapted to this
consolidation's provenance):

| Tag | Meaning |
|---|---|
| `[RUN]` | The cited value was re-read (or, where stated, re-derived) by this writing session from the named repository file — the command output was observed here. No measurement, gate or timing was re-executed by this document's author: this session's command inventory was read-only (list/read/grep/git inspection plus arithmetic on archived values); it created no archive under `p5-raw/` and modified no frozen artifact. |
| `[TRANSCRIBED]` | Transcribed from the independent-review records supplied to this writing task, or from frozen P2/P4/P5 session documents; not re-derived here. |
| `[UNCERTAIN]` | Could not be verified from the repository or the supplied material; flagged rather than filled with a guess. |

Statuses of the research campaign are unchanged by this document: all four
branches are local-only, present on no remote `[RUN]` (`git branch -r
--contains <sha>` → empty for `deaf0301`, `476bd9ce`, `4ab6ef17`), master
remains `bdfd0d8d` whose `src/blocking_file.cpp:250-298` still carries the
B1-form compose (`CompositionState` + `direct_outcome`) `[RUN]`. **Nothing
in P5 is merged, pushed or productized; P5 ends OPEN (plan §0 rule 6).**

## 1. Arm identities

`p5-raw/identity.txt` `[RUN]` (read in full this session):

| Arm | HEAD | release archive sha256 | kit binary (source `2364be4a`, common-kit-source clause) |
|---|---|---|---|
| B1 | `77e5399a…` | `d1e351a5…3007f0` | `ladder 89450367…010e8` |
| A (P2C1) | `deaf0301…` | `07d06803…2049e` | `ladder-c1 8305d22c…0381` |
| B (P2C2) | `476bd9ce…` | `67251028…20a807` | `ladder-c2 8bb6473b…0065` |
| A+B | `4ab6ef17…` | **`07d06803…2049e` (= A)** | **`ladder-ab 8305d22c…0381` (= ladder-c1)** |

The stacked archive and kit binary are byte-identical to A's. The merge
commit `060a33c2` has parents `deaf0301` + `476bd9ce` `[RUN]` (`git log
--format='%h %p' -1 060a33c2`) — the preregistered merge shape (plan §2); the
branch carries two further commits (`96d74e15` gate scripts, `4ab6ef17`
rules-control retarget per Amendment 1). The identity consequence — merge
resolution R1 took P2C1's `src/blocking_file.cpp` wholesale, so **zero lines
of B's textual end state survive in the stacked tree** — is quantified in
`p5-raw/P5AB/static/static-recount-20261028.md` §"Item 7" `[RUN]`: changed
lines B1→A 128, B1→B 85, A↔B 121, **B1→AB 128 = B1→A exactly**; AB's
`blocking_file.cpp.o` sha256 = C1's (`d5c58bb3…87a8b`). The interaction
matrix therefore measured **BASE / A / B / A′** where A′ is A's library code
sampled independently (§2.4 row 1, §2.5 item 1; review problem 2, §4.3
row 2).

## 2. Interaction matrix results (MISSION §7.1 / plan §3–§4)

### 2.1 Correctness and static gating preconditions

- **S0 correctness gate — PASS (gating).** Composition gates passed at
  `4ab6ef170ff939001cc1459445b77c21df9e7d86` (the gate-pass input to this
  record):
  `p5-combined-mutants.sh` exit 0 — archive
  `p5-raw/P5AB/mutants-20261028T185712/` `[RUN]` (directory listed;
  `mutants-head.txt` read = `4ab6ef17…`; per-seam `.err` files present for
  all 8 seam legs + rules control); record: 8 seam rows GREEN→RED(named)→
  RESTORED GREEN over the 6-define union seam set, rules control GREEN under
  `FAILURE_ACCOUNTED` `[TRANSCRIBED]` (matrix record supplied to this task).
  `p5-combined-matrix.sh` exit 0 — archive
  `p5-raw/P5AB/matrix-20261028T185735/` `[RUN]` (`matrix-head.txt` =
  `4ab6ef17…`; release and asan legs' final lines read: "100% tests passed,
  0 test(s) failed out of 58"); record: release/debug/asan/asanubsan 58/58,
  0 failed `[TRANSCRIBED]`. The closing session re-ran both gates at the
  same HEAD via the main-workspace script copies (archives
  `matrix-20261028T215804`, `mutants-20261028T215835`) with identical
  results `[TRANSCRIBED]` (P5_INTERACTION_OUTCOME.md §13). Authority
  contraction re-recorded: no new runtime state; `direct_outcome` /
  `publish_certainty` absent from the stacked TU; `CompositionState` out of
  the production path; the `detail::compose_progress` rule table retained as
  the non-authoritative SEM-05 oracle (green-under-mutation proven by the
  control leg) `[TRANSCRIBED]` (outcome §2).
- **S1 static retention — read side PASS, write side FAIL →
  INTERFERENCE_STATIC.** From `p5-raw/P5AB/static/static-recount-20261028.md`
  `[RUN]` (re-read in full):
  - Read side (= A's end state): `read_exact_at` 68 ≤ 75 / arc 25 ≤ 26;
    `read_exact` 55 ≤ 60 / arc 21 ≤ 21 (at bound — exactly C1's own §2.1
    verdict). M1 (0 re-tests in step loops, exactly 1 pread/read per step),
    M2 (`movaps 0x20/0x10(%rsp)` dead stores 0/0 vs B1's 2/2), M3
    (single-point exit materialization) all PASS. Back-edge guards
    0x1130/0x1133 and 0x1780/0x1783 present.
  - **Write side FAIL: `write_all_at` per-extra-step arc 39 > 37 (+2)**
    → per plan §4 S1, **INTERFERENCE_STATIC** recorded and the write side's
    stacked claims are **not admitted** (no wall number repairs a failed
    static bound). Remaining write bounds pass: `write_all_at` 1-step 79 ≤
    93, `write_all` 61 ≤ 77 / arc 29 ≤ 30. The +4 vs B's own arc 35 is
    decomposed in the dossier: A's back-edge shape (advance → complete-check
    → zero-progress-check → loop-condition, confirmed-update mov inside the
    back-edge) does not fuse C2's merged block.
  - All 8 recount anchors reproduce across the four archives (B1 100/44,
    82/34 read and 101/100, 82/34 write; C1 68/25, 55/21; C2 81/80, 62/25)
    with the preregistered ±1 audit-caliber note confined to
    `write_all_at` 1-step (B1 101/100, C2 81/80); arcs carry no caliber
    delta; the failing gate fails by +2, beyond any ±1 ambiguity.
  - Public-op machine identity: address-stripped instruction streams of the
    four public ops sha-equal AB vs B1 (`read_at 762b2ae4…`, `read
    27b981d9…`, `write_at a2e22afb…`, `write 8dba982c…`) ⇒ exact-path ±0 by
    structure. Impossible-count guards present on all four compose
    back-edges (`read_exact_at` 1130/1133, `read_exact` 1780/1783,
    `write_all_at` 14a0/14a3, `write_all` 1a90/1a93).

### 2.2 Cell-by-cell results

All wall cells: 16 ABBA pairs, arm order alternating, one process invocation
per arm sample, `taskset -c 2`, serial cells on a quiescent host; medians and
95% t-CIs below were re-read this session from each cell's archived analysis
tail (`p5-raw/<cell>.txt`) `[RUN]`; verdict labels follow the plan §4
vocabulary. Sign convention: each row uses its cell file's native pairing
convention (cross-binary cells: candidate − baseline; within-binary
endpoint cells: native − sluice arm), except the group-5 D rows, which are
reported in the unfavorable-positive form used by the adjudication carrier
(median and CI endpoints both sign-flipped from the native form — the D_ab
row shows its native values inline). Analyzer-convention note: the archived
P5 analysis tails pair A/A samples consecutively; P4's analyzer used an
alternating sign-flip convention — each session is internally
self-consistent and no cross-session numeric comparison is made anywhere
(plan §5 item 8), so no verdict depends on the difference (§4.3 row 7).

**Group 1 — A/A floors** (one §11.10-style 45 s quiesce rerun per failed
cell, rerun value stands; attempt-1 files archived `[RUN]` — both present in
`p5-raw/`):

| Cell | sd_AA (ns) | fresh MDE′ = 2.131·sd/√16 (ns) | Disposition |
|---|---|---|---|
| `p5-aa-read-4k-b1` | 15.475 (attempt1 29.417) | 8.24 | accepted; persistent noise disclosed (§2.5) |
| `p5-aa-read-4k-c1` | 20.243 (attempt1 23.302) | 10.78 | accepted; disclosed |
| `p5-aa-read-4k-ab` | 19.445 (attempt1 23.591) | 10.35 | accepted; disclosed |
| `p5-aa-write-4k-b1` | 53.481 (attempt1 54.820) | 28.48 | accepted; native first-side jitter, disclosed |
| `p5-aa-write-4k-c2` | 33.026 | 17.59 | accepted; disclosed |
| `p5-aa-write-4k-ab` | 28.272 | 15.05 | accepted; disclosed |
| `p5-aa-cold` | 476.079 | 253.6 | accepted; in-band vs P1 precedent sd 2242 |

Cross-binary floors (max-of-pair): read A-cells 10.78, AB-cells 10.35 vs
pinned 5.535; write 28.48 both vs pinned 15.695 `[TRANSCRIBED]` (matrix
record) — the persistent >10 ns session noise band is formally disclosed in
§2.5, not absorbed by repetition.

**Group 2 — BASE arm (fresh same-session mechanism-excess anchors; recorded
per the P1 rule):**

| Cell | Statistic (ns, median [95% CI]) | Class |
|---|---|---|
| `p5-base-composition-4k` (S) | −12.079 [−20.605, −3.087], CI excludes 0; \|m\| ≥ max(MDE′ 8.24, 1%·N1 9.31) | **EXCESS-class** (fresh same-session mechanism excess) |
| `p5-base-eofmix-4k` (S_eof) | −16.912 [−19.553, −10.997], excludes 0; pos 0/16 | EXCESS-class |
| `p5-base-cursor-composition-4k` (S_cur) | −13.647 [−22.394, −3.049], excludes 0 | EXCESS-class |
| `p5-base-wcomp-4k` (S_w / P_base_w context) | −13.732 median (mean +3.609) [−24.140, +31.357] spans 0; \|m\| < MDE′ 28.48; content_ok 32/32 | PARITY — the write gap stays instruction-caliber, not wall |

**Group 3 — single-factor fresh references (attribution only; the frozen
P2/P4 single-factor verdicts stand; no new claim attaches):**

| Cell | Statistic (ns) | Disposition |
|---|---|---|
| `p5-delta-comp-read-4k-a` (Δ_A) | −21.928 [−30.708, −4.178], excludes 0 | CI-excluding fresh reference — reproduces the frozen evidence same-session |
| `p5-delta-comp-eofmix-4k-a` (Δ_A_eof) | −13.332 [−18.517, +2.811] spans 0 | direction consistent with frozen P4 eofmix (CI-excluding −15.907) but underpowered → classified **attribution reference, not an environment alarm** (no contradiction with frozen evidence; §2.5 and review problem 6) |
| `p5-delta-comp-cursor-4k-a` (Δ_A_cur) | −14.571 [−22.442, −4.431], excludes 0 | CI-excluding fresh reference |
| `p5-delta-wcomp-4k-b` (Δ_B_w) | −10.817 [−38.195, +18.930] spans 0 | NO_CLAIM — as expected (P2C2 §6.2 / P4 precedent) |

**Group 4 — A+B claim units (the primary stacking carriers; S2):**

| Cell | Statistic (ns) | Verdict |
|---|---|---|
| `p5-delta-comp-read-4k-ab` (Δ_AB) | −10.880 [−20.020, +3.034] **spans 0**; \|m\| ≥ pinned 5.535 | **STACK_LOSS(comp-read-4k)** — S2 FAIL |
| `p5-delta-comp-eofmix-4k-ab` (Δ_AB_eof) | −10.244 [−14.102, +9.670] spans 0; dual-report 10.244 < MDE′ 10.35 (would be NO_CLAIM even if CI excluded) | **STACK_LOSS(eofmix)** — S2 FAIL |
| `p5-delta-comp-cursor-4k-ab` (Δ_AB_cur) | −8.107 [−18.592, +1.860] spans 0; dual-report 8.107 < MDE′ 10.35 | **STACK_LOSS(cursor)** — S2 FAIL |
| `p5-delta-wcomp-4k-ab` (Δ_AB_w) | −7.915 [−21.948, +25.706] spans 0; \|m\| < 28.48; content_ok 32/32 | wall recorded; NOT EXCESS (S3 wall component satisfied; instruction caliber adjudicated by PMU → S3 FAIL, below) |

**Group 5 — A+B within-binary endpoints (S6, recorded; only D_ab_w enters
the conjunction):**

| Cell | Statistic (ns, unfavorable-positive D form: median and CI both sign-flipped from the file's native `A1x − AX` delta) | Verdict |
|---|---|---|
| `p5-ab-composition-4k` (D_ab) | +8.248, CI [+3.433, +11.288] **excludes 0** (native −8.248 [−11.288, −3.433]); \|m\| 8.248 < max(MDE′ 10.35, 1%·N1 ≈ 9.33) | **not EXCESS** under the P1 §5.5 rule verbatim (both terms); the CI-excluding fact is disclosed, the MDE-only reading that would call it EXCESS at the pinned 5.535 floor is recorded alongside (§4.3 row 5); cap semantics moot given S2 |
| `p5-ab-eofmix-4k` (D_ab_eof) | +6.895, CI [−2.987, +13.452] spans 0 | PARITY/NO_CLAIM |
| `p5-ab-cursor-composition-4k` (D_ab_cur) | +2.777, CI [−10.818, +11.331] spans 0 | PARITY |
| `p5-ab-wcomp-4k` (D_ab_w) | +1.123, CI [−38.993, +17.317] spans 0 | **NOT EXCESS** — B's G4 no-regression rule satisfied |

**Group 6 — negative controls on the stacked binary (S4, gating):**

| Cell | Statistic (ns) | Verdict |
|---|---|---|
| `p5-ab-negread-4k` | +4.291 [−4.099, +17.003] spans 0 | PARITY |
| `p5-ab-negread-1b` | −1.663 [−4.907, +3.563] spans 0 | PARITY |
| `p5-ab-negwrite-4k` | −5.017 [−21.121, +30.978] spans 0; content_ok 32/32 | PARITY |
| `p5-ab-negwrite-1b` | −2.003 [−2.735, +1.035] spans 0; content_ok 32/32 | PARITY |

**S4 PASS (4/4).**

**Group 7 — cross-payload regression face (S5, gating):**

| Cell | Statistic (ns) | Verdict |
|---|---|---|
| `p5-reg-readat-4k` | +1.860 [−10.630, +7.310] spans 0 | PARITY — public `read_at` machine-identical (S1 item 3), wall parity confirmed |
| `p5-reg-writeat-4k` | −3.098 [−16.996, +28.953] spans 0 | PARITY |
| `p5-reg-meta-oc` | +1.469 [−4.542, +23.584] spans 0 | PARITY |
| `p5-reg-meta-fi` | −2.344 [−3.787, +2.341] spans 0 | PARITY |
| `p5-reg-cold-4k` | +134.407 [−579.172, +399.377] spans 0; ≪ cold floor max(pinned 1416.185, fresh 253.6); 32 arm samples, all cold-band: ns_per_op min 9565.594 / max 12330.270, arm medians 9761.305 / 9623.690 per the analysis tail | NO_CLAIM (bounds the claim; not REGRESSION). The session matrix record's "one 1378 ns/op partial-warm outlier" is **not supported by the archive** — 1378.510 is the first arm line's `sys_ns_per_op` field, not an ns_per_op sample; corrected against the archive (§2.5 item 7) |

**S5 PASS (5/5).**

**Group 8 — diagnostic calibers (never wall sources):**

- `p5-pmu-base-wcomp-4k` (P_base_w): WX−W1x = **+64.026 instr/op**
  (mean-of-window-means; this session re-derived from the six raw
  `instructions:u` windows `[RUN]`: base WX mean 1,145,237,961 − W1x mean
  1,144,975,712 = 262,249/4096) — reproduces P2C2's archived caliber +64.076
  (Δ 0.02–0.05 across aggregation calibers). 3 exact repeats per arm, spread
  ≤ 0.13 instr/op `[TRANSCRIBED]` (matrix record).
- `p5-pmu-ab-wcomp-4k` (P_ab_w): WX−W1x = **+44.063 instr/op** `[RUN]`
  (same re-derivation: 1,145,156,036 − 1,144,975,569 = 180,466/4096).
- **S3 PMU gate — FAIL (marginal):** required P_ab_w ≤ P_base_w − 20 =
  44.026; observed 44.063 → miss **+0.037 instr/op** (mean caliber,
  re-derived this session). The matrix record's median caliber reports
  44.057 vs 44.056 (miss +0.0015) and mean caliber +0.033 `[TRANSCRIBED]`;
  the performance reviewer's independent recompute reports +0.03–0.04
  `[TRANSCRIBED]`. **Every reported or recomputed aggregation caliber fails
  the preregistered gate; the FAIL is recorded as the rule's verbatim
  outcome, not waved through on marginality** (review problem 3; outcome
  §5). Same-source corroboration: S1's write arc +4 vs B's own 35. S3 also
  fails independently on the S1 write static bound (39 > 37). No wall claim
  for B in the stack (preregistered; the measurability upgrade stays
  unclaimed).
- `p5-pmu-{base,ab}-composition-4k` (read corroboration, non-gating):
  P_base = +128.0 → P_ab = +66.0 instr/op (−62; static −32 at 1-step;
  PMU/static ≈ 1.94, consistent with P2C2's ≈ 2.0) `[TRANSCRIBED]` (matrix
  record; outcome §8 records +128.03 → +65.92 re-computed from the six
  windows) — read-side instruction-caliber retention corroborated: the
  stacked library's read work is A's end state.
- `p5-strace-*` (12 cells: group-4 ×4, group-6 ×4, group-7 warm ×4): **PASS
  — syscall parity EXACT; PHYSICAL_WORK_DELTA = 0 on every claim unit and
  the regression face.** 3 repeats per arm with identical per-syscall
  counts; paired integral count maps identical on every shared arm (e.g.
  `ax` 7170×`pread64`, `ac` 7172×`read`, `wx` 22528×`pwrite64`, `a2` 1 B
  28674×`pread64`, `m2oc` 12295×`openat`/12294×`close`) `[RUN]` (the
  `p5-strace-comp-read.txt` header and integral `pread64 7170` line re-read
  this session; full per-cell counts per the matrix record)
  `[TRANSCRIBED]`. No syscall-count migration; user/kernel split recorded
  per cell — a seesaw would require sys-growth with counts flat; none
  observed.
- Kit smoke: 85 checks, 0 failures, "ALL GATED GROUPS IDENTICAL" — read
  group {a0,a1,a2,a1x,ax,a1c,ac} over `ladder`/`ladder-c1`/`ladder-ab`;
  write group {w0..wc} `content_ok=1` over `ladder`/`ladder-c2`/`ladder-ab`;
  metadata ×3; closed-state rejection; EINTR diagnostics `[RUN]`
  (`p5-raw/kit-smoke.txt` summary lines re-read; the file's header records
  the explicit `ladder-ab` ≡ `ladder-c1` identity note).

### 2.3 Stacking adjudication (plan §4 conjunction, preregistered)

| Gate | Result |
|---|---|
| S0 correctness precondition | **PASS** |
| S1 static retention | read **PASS**; write **FAIL** (arc 39 > 37) → **INTERFERENCE_STATIC** |
| S2 A's win units retained (all three required) | **FAIL** — Δ_AB medians −10.880 / −10.244 / −8.107 all pass the pinned −5.535 line but all three CIs span 0 → **STACK_LOSS(comp-read-4k, eofmix, cursor)** per the preregistered line ("CI-spanning → STACK_LOSS(u)"). Not REGRESSION (no CI-excluding unfavorable delta); not INCONCLUSIVE (the 16-pair budget was met; the failure mode is CI-spanning, not an under-budget non-verdict). |
| S3 B's instruction-caliber marginal | **FAIL** — PMU 44.063 > 44.026 (miss +0.037 mean caliber; every caliber fails) AND S1 write static bound failed |
| S4 negative controls | **PASS** 4/4 |
| S5 regression face | **PASS** 5/5 (D_ab_w not EXCESS under S6) |
| S6 endpoints/interaction | recorded (below) |

**Stack verdict: NOT STACK_ADMITTED** — the conjunction S0 ∧ S1 ∧ S2 ∧ S3 ∧
S4 ∧ S5 fails on S1 (write), S2 (×3) and S3. Per the preregistered fallback
(plan §4: "if S2/S4/S5 fail, likewise whichever factor's own gates still
hold"): **the recorded integration candidate is A ALONE.** A's KEEP stands
on its frozen P2/P4 evidence (P2C1 §12.5; P4 `WIN_CONFIRMED` ×3), with
same-session corroboration from the group-3 fresh references (2/3 units
CI-excluding). B's single-factor KEEP (instruction caliber, P2C2 §9) is
**not withdrawn** by P5 wall numbers — single-factor verdicts stand on
their own evidence (plan §0 rule 2). Terminal vocabulary recorded:
`STACK_LOSS(unit)` ×3 + `INTERFERENCE_STATIC` (write side) + S3 FAIL,
fallback A alone; OPEN state, no merge.

Two preregistration-text disclosures attach to this conjunction (audit
observations, no outcome change under either reading):

- **Fallback enumeration gap.** The plan §4 fallback enumerates only two
  branches — "if S3 fails alone → A alone" and "if S2/S4/S5 fail →
  likewise whichever factor's own gates still hold". The actual failure set
  additionally contains S1 (write-side INTERFERENCE_STATIC), a branch the
  preregistered fallback text does not enumerate. The recorded outcome does
  not rest on stretching an unlisted branch: it is independently supported
  by the enumerated S2/S3 branches (both failed) and by the MISSION §7.1
  selection rule ("most effective": the stack is not more effective than A
  and is byte-identical to it). Recorded here rather than silently applied;
  a one-line closure note in the plan's Amendment log would close the gap
  formally (not added by this document, which does not amend the plan).
- **STACK_LOSS vs INCONCLUSIVE textual tension.** Plan §4 preregisters both
  the S2-specific line ("sub-floor or CI-spanning → STACK_LOSS(u)
  recorded") and the conjunction-general rule ("Statistical
  non-significance after the preregistered budget → INCONCLUSIVE"). A
  CI-spanning S2 outcome is literally also "statistical non-significance
  within the budget". The adjudication carriers took the specific rule over
  the general one and argued the distinction explicitly (budget met; the
  failure mode is CI-spanning, not an under-budget non-verdict; no sampling
  added — that would need a numbered amendment). Both readings produce
  S2 FAIL, break the conjunction, and yield the same fallback (A alone);
  recorded as an audit observation of tension in the preregistered text,
  not as a violation.

Measurement-power context (recorded attribution, not a claim revision):
because `ladder-ab` ≡ `ladder-c1` (§1), the group-4 and group-3 units are
the same library code sampled independently — the A-arm references
CI-exclude 2/3 units in the same session while the AB-arm units span 0 at
effect medians 8.1–10.9 ns against fresh cross-binary MDE′ 10.35–10.78 ns.
This demonstrates a session power limitation, not mechanism loss; it is
disclosed honestly and **no post-hoc sampling was added** (out-of-budget
sampling would require a numbered amendment) `[TRANSCRIBED]` (outcome §4;
reviewer's same-binary re-sampling observation).

Interaction terms (S6, attribution only — independent gains are never
added, MISSION.md:404): I(comp-read) = Δ_AB − Δ_A = +11.048 ns; I(eofmix) =
+3.088; I(cursor) = +6.464; I_w = Δ_AB_w − Δ_B_w = +2.902 `[RUN]` (arithmetic
on the §2.2 values re-read above). The stacked-arm same-session effects are
smaller than the single-factor-arm effects — consistent with "the stack is
A's library code + session power", not with a mechanism difference (the two
arms differ only in run time). The ±30–80% static→wall systematic caveat
(P2C1 §12.4) is inherited by all I values.

### 2.4 MISSION §7.1 checklist — item-by-item answers

MISSION.md:743-751 requires the interaction matrix to check seven things.
Answers, each tied to the evidence carrier fixed in plan §6:

1. **Is the mechanism duplicated? (机制是否重复)** — Yes, materially, and
   the merge is idempotent. A's shared compose template already carries B's
   exit-staging transform on the semantic axis (single-point materialization
   per exit flavor, register-carried confirmed/total, public write steps
   with per-step re-tests retained), so resolution R1 delivered A's
   `src/blocking_file.cpp` wholesale: B1→AB changed lines 128 = B1→A
   exactly; **zero lines of B's textual end state survive**; AB's object,
   archive and kit binary are byte-identical to A's (§1). The negative
   conclusion is therefore measured, not guessed: **the two factors are not
   additive on the shared compose template — merging takes A.** The cost of
   that idempotence is quantified: the AB write arc is 39 vs B's own 35
   (+4 instructions per extra step) — A's loop shape does not fuse C2's
   merged block (static dossier decomposition) — which is exactly the
   failed S1 write bound. The plan §6 row-1 preregistered idempotence
   expectation ("outcome recorded either way") landed on its failure branch
   for B's write-side bounds and is recorded as such.
2. **Did cost migrate? (成本是否迁移)** — No migration found. Syscall
   parity is EXACT on every claim unit and the regression face
   (PHYSICAL_WORK_DELTA = 0; identical integral per-syscall counts across
   paired arms, ≥3 exact repeats, §2.2 group 8); PMU cross-arm deltas
   localize the remaining change to user space (read +128 → +66 instr/op);
   the write-side wall gap stays instruction-caliber (S_w PARITY while
   P_base_w = +64.0 instr/op).
3. **Does new state offset the gains? (新增状态是否抵消收益)** — No new
   runtime state exists to do so: the stacked TU adds none (S0
   authority-contraction re-record; only local variables + template
   parameters), removes no guard (all four impossible-count back-edge
   guards present), and deletes `direct_outcome`/`publish_certainty` once.
   The S2 STACK_LOSS verdicts are attributed to session noise power on a
   binary byte-identical to A (§2.3), not to state offset — recorded as
   attribution with the re-derivation shown, per the plan §6 row-3 rule.
4. **User/kernel seesaw? (用户态/内核态此消彼长)** — None observed. A
   seesaw requires sys/op growth with syscall counts flat and PMU constant;
   the record shows counts identical per arm, PMU deltas tracking the static
   expectation (≈1.94–2.0×), and per-cell rusage user/sys lines archived
   (`user_ns_per_op`/`sys_ns_per_op` in every cell file `[RUN]`).
5. **Regression on other Direct operations? (对其他 Direct 操作的回归)** —
   None: the four warm regression-face cells are PARITY and the cold cell
   is NO_CLAIM (S5), and the machine-level exclusion holds
   (address-stripped public-op streams sha-equal to B1 ⇒ exact-path ±0 by
   structure, S1 item 3). The kit smoke's closed-state, metadata and EINTR
   groups are identical across the four binaries.
6. **Regression on different payloads? (对不同 Payload 的回归)** — None
   found across the measured span: 4 KiB sequential composition, tail/EOF
   boundary rotation, shared-cursor composition, write composition, 1 B
   public ops, metadata open/close and file_info, and cold btrfs. Boundary
   stated honestly (plan §6 row 6): P4's seven single-factor `NO_CLAIM`
   shapes (hf-1b/4k, seq-1m, rand-4k, meta, cold) are not all re-measured
   here; for shapes whose entire candidate exposure is the public ops, the
   S1 sha-equal gate is the structural coverage argument, recorded as
   structure — not as fresh measurement.
7. **Impact on existing v2 lifecycle and error semantics? (对现有 v2 生命
   周期与错误语义的影响)** — Handed to the independent correctness review
   (plan §6 row 7; §3 below): **APPROVE ×3** with two non-blocking
   observations (THREAD-01 concurrent-close boundary on the A-form read
   compose; one error-path dead store), both recorded in §3.4 and carried
   into the adoption preconditions (§6.2). No contract violation was found
   on any object.

Selection rule applied (MISSION.md:753 — "most effective, semantically
complete, reasonable long-term maintenance cost"): the stack is neither more
effective (S1 write regression, S2 ×3, S3 FAIL) nor distinct (byte-identical
to A); **A alone is the recorded integration candidate**; B remains a
single-factor KEEP on its own instruction-caliber evidence, unstacked.

### 2.5 Anomalies and disclosures (as-recorded)

1. **A+B is A′** — the R1 merge outcome makes the stacked production TU
   byte-identical to P2C1; B's independent object code (C2's compose) does
   not exist in the stacked tree. Idempotence quantified per §2.4 row 1;
   not a completeness accident (all cross-arm checksums green; the kit
   smoke records the identity explicitly).
2. **S1 INTERFERENCE_STATIC** — `write_all_at` per-extra-step 39 > 37 (+2);
   write-side stacked claims not admitted per the preregistered rule.
3. **S3 PMU marginal FAIL** — every aggregation caliber fails by
   +0.0015…+0.037 instr/op; recorded FAIL verbatim (§2.2 group 8).
4. **S2 CI-spanning ×3** — medians pass the pinned line, CIs span 0; the
   same-binary fact (ladder-ab ≡ ladder-c1) points to session power, not
   mechanism loss; disclosed, no post-hoc sampling.
5. **Persistent session noise** — six warm A/A cells all first-attempt
   sd > 10 ns; one silent 45 s quiesce rerun each per P1 §11.10, rerun
   values stand; residual 15–53 ns disclosed, not absorbed. Write-track
   first-pair outliers (+140.6…+204.2 ns) appear in 7 write cells'
   `raw_deltas[0]` `[TRANSCRIBED]` (outcome §11); medians robust, samples
   never deleted.
6. **Group-3 Δ_A eofmix** — CI-spanning while frozen P4 eofmix was
   CI-excluding; direction consistent → attribution reference, not an
   environment alarm (§2.2 group 3; the matrix record initially routed it
   to the §7 performance review, which confirmed no alarm — reviewer
   problem 6 and outcome §11 agree on the classification).
7. **Cold regression cell — correction against the archive.** The session
   matrix record described "32/32 cold-shaped (one 1378 ns/op partial-warm
   outlier)", which is internally contradictory and unsupported: this
   session extracted all 32 arm samples' `ns_per_op` from
   `p5-raw/p5-reg-cold-4k.txt` `[RUN]` (awk on the field after
   `ns_per_op=`): min 9565.594 / max 12330.270 — no sample near 1378; the
   1378.510 figure is the **first arm line's `sys_ns_per_op` field value**,
   not an ns_per_op sample. All 32 samples are cold-band; arm medians
   9761.305 / 9623.690 match the analysis tail; the paired deltas include
   wide values (raw_deltas −2758.859 … +1728.423) which are cross-arm pair
   differences, not partial-warm samples. The NO_CLAIM verdict is
   unaffected (CI [−579.172, +399.377] spans 0). The erroneous detail is
   corrected wherever this document reports the cell (§2.2 group 7).
8. **Audit caliber ±1** — the P1 audit caliber counts one endpoint nop more
   than the session caliber on `write_all_at` 1-step (B1 101/100, C2
   81/80); arcs carry no caliber delta; no verdict changes under either
   caliber.
9. **Unclaimed P5AB gate archives.** `p5-raw/P5AB/matrix-20261028T215941`
   (mtime 22:00:03) and `p5-raw/P5AB/mutants-20261028T220011` (22:00:25),
   both heads `4ab6ef17` `[RUN]` (stat + head files read), were created
   after the outcome document's last write (21:59:14) and before this
   consolidation (22:12:09), and are claimed by no carrier document (both
   carriers claim only 215804/215835). They are not products of this
   document's author (this session's command inventory was read-only —
   legend, §0). Same HEAD, same gates ⇒ no verdict impact; recorded here
   because every archived artifact having a named origin/disposition is the
   campaign's recording discipline (§5 item 6).
10. **Fallback enumeration gap** — the preregistered plan §4 fallback does
    not enumerate an S1-failure branch; the executed fallback additionally
    rests on the enumerated S2/S3 branches and the MISSION §7.1 selection
    rule. Disclosure and reasoning in §2.3.
11. **STACK_LOSS vs INCONCLUSIVE textual tension** — plan §4 preregisters
    both an S2-specific CI-spanning line and a conjunction-general
    non-significance rule; the adjudication took the specific rule with the
    distinction argued explicitly. Both readings give the same terminal
    state. Disclosure in §2.3.

## 3. Independent correctness review (MISSION §7.2)

Three objects reviewed (review records supplied to this writing task;
verbatim verdicts `[TRANSCRIBED]`, bases condensed but not re-interpreted;
each reviewer statement below is the reviewer's own evidence claim, not this
document's re-execution):

| Object | Verdict |
|---|---|
| P2C1 (A) | **APPROVE** |
| P2C2 (B) | **APPROVE** |
| Stacked branch `direct-compose-stack-readpreval-writeexit` @ `4ab6ef17` | **APPROVE** |

**Provenance limitation (disclosed).** The reviewers' own records are **not
archived in this repository**: this session verified that no file under
`p5-raw/` contains any review verdict (`grep -rln APPROVE p5-raw/` → empty)
and that the only review narration in `research/direct-beyond-parity/` is
the plan's verdict vocabulary and the two carrier documents (the outcome
document and this consolidation) `[RUN]`. The reviewers' working evidence
described in their bases — the RAW-redirected `/tmp` script copies, the
`/tmp/c1-blocking_file.cpp` / `/tmp/c2-blocking_file.cpp` reads, their own
sha-equal comparison scripts, their 41-cell re-derivation worksheets —
lives outside the repository and cannot be re-verified from it. Two
consequences are recorded honestly: (a) the reviews exist in-repo **only as
the transcriptions in this document**, so MISSION §7.2's independence
requirement ("must not merely repeat the implementer's test conclusions")
is checkable here only to the depth of those transcriptions — internal
evidence within them is favorable (they carry reviewer-originated
recomputations that differ from the implementer's figures, e.g. the S3
marginal +0.03–0.04 vs the implementer's +0.037/+0.033, and reviewer-side
corrections of the reviewer's own analyzer slip), but that check cannot
reach the reviewers' actual work; (b) of the reviewer-attributed re-runs,
only one has a compatible in-repo artifact by timestamp —
`p5-raw/P5AB/mutants-20261028T213248` (21:33, head `4ab6ef17` `[RUN]`) is
consistent with the correctness reviewer's in-place mutants re-run but is
not named by it, and the attributed release-leg matrix re-run has **no**
corresponding archive entry that any document claims (the 21:58
`matrix-20261028T215804` is claimed by the closing session). Archiving the
review records themselves remains an open P5 closeout item for the Owner.

### 3.1 §7.2 checklist — per-object coverage

MISSION.md:759-772 requires ten independent checks ("must not merely repeat
the implementer's test conclusions"). Coverage as performed by the
reviewers:

| §7.2 item | P2C1 (A) | P2C2 (B) | Stacked branch |
|---|---|---|---|
| v2-r1 correspondence | Five stop states derived independently from B1's detail rule table (`file_semantics.hpp` compose_progress/compose_error/composition_effect_certainty) vs C1's complete/stalled/primitive_error outcomes (`/tmp/c1-blocking_file.cpp:252-337`, from `git show deaf0301`): materializations isomorphic; SEM-05 stop set and ERR-02 confirmed-prefix/unknown-remainder equivalent; EINTR retried only on n<0 && EINTR (inlined retry path read in disassembly fea–ffd→1160→fdf); SEM-01 safe repeated primitive use — exactly 1 pread per step (objdump fc0–fed, single call@fe5) | Per-branch derivation of the B1→C2 template diff (`/tmp/c2-blocking_file.cpp`, `git show 476bd9ce`): zero-progress check moved before `confirmed_bytes += transferred` is equivalent (transferred==0 → identity); all four stop-state materializations equal B1's direct_outcome/publish_certainty outputs; SEM-05/ERR-02 preserved; step primitives remain the public ops — no admission-frequency difference | Production TU byte-identical to C1 (c1.o = ab.o = `d5c58bb3…87a8b`; both archives `07d06803…`) → P2C1's analysis transfers mechanically; R1's semantic-union obligations independently machine-verified (read: 1 pread/step, no re-admission; write: per-step range/representable re-checks, back-edge fd/access reloads, pwrite@1303; register residency movaps 2/2→0/0 by the reviewer's own grep) |
| Public behavior changed? | Four public-op instruction streams sha-equal B1 vs candidate (reviewer's own script on the two archives' `blocking_file.cpp.o`); compose entry rejection order verbatim B1 (closed→access→zero-length no-op→range) | Same sha-equal result on C2's archive (`67251028…20a807`); `include/sluice/blocking/file.hpp` (CompositionOutcome/ABI) untouched | Reviewer's independent B1-vs-AB comparison (not the dossier's): sha-equal on all four |
| Range / zero-length / error precedence | Entrance `range_is_valid` (length−1 ≤ kMax−offset) once ⇒ every sub-step legal by arithmetic; zero-length no-op with no syscall asserted by `zero_length_requests_make_no_native_call` + precedence-table `no_op_without_dispatch` (test bodies read) | Entry code verbatim B1 (`precheck_data_op` order unchanged); compose entry order unchanged | Inherited (TU byte-equal; entry code identical) |
| Buffer/File lifetime | Borrowed span + `const File&`, zero allocation in loop, sret materialized then return | Same | Inherited |
| Partial effects | `error_after_confirmed_prefix_keeps_the_prefix` + `every_stop_reason_is_pinned_against_the_oracle_error_rule` (rule table driven in parallel; test bodies read) pin prefix retention and unknown remainder | Prefix test pins it; FAILURE_ACCOUNTED seam proves the production path live (not dead code) | Inherited; test face vs B1 adds only the named killer (tests diffed: additions only, no weakening) |
| Composition state | `CompositionState` out of production path; `direct_outcome`/`publish_certainty` deleted; no new runtime state (full-text read: only locals + template parameters); rule table kept as non-authoritative oracle | Same deletions; `compose_mut_rules_stay_green_short_io` control proves the oracle green under mutation | Re-recorded on the stacked TU (S0); no new state |
| Concurrent calls | THREAD-01 places File move/close/destruction under caller serialization with "no outstanding borrows"; concurrent close mid-composition is out of library obligation (observation 1 records the boundary) | No synchronization-assumption change; per-step admission identical to B1 ⇒ no concurrently observable difference | Inherited (semantics identical to A's TU) |
| Fault injection | NativeScript intercept at `native_pread`/`native_read`; `prevalidated_*_step` still routes through it (source :279-299); fault suite green | Intercept layer untouched; new named killer `write_all_impossible_count_stops_immediately` asserts `script.calls()==1` | Inherited; union-seam gate re-run by the reviewer in the `4ab6ef17` worktree: exit 0, 8 rows GREEN→RED(named)→RESTORED + control green under FAILURE_ACCOUNTED |
| Mutation oracle | Reviewer re-ran P2C1-mutants.sh via a RAW-redirected /tmp copy (1-line diff): exit 0, 7/7 seams GREEN→RED(named)→RESTORED; matrix release leg re-run exit 0 (58/58) on the byte-identical 4ab6ef17 TU | Reviewer re-ran P2C2-mutants.sh the same way: exit 0, 4 seams GREEN→RED(named)→RESTORED + rules control green; ADVANCE_BY_REQUESTED production-TU-only on the C2 tree makes the control's original define legal there | Reviewer re-ran p5-combined-mutants.sh in place: exit 0, 8 joint seam rows + control; independently verified the Amendment-1 control retarget rationale (the macro exists in both rule table and production TU on the stack; FAILURE_ACCOUNTED is production-TU-only) and read the archived old-define RED (`mutants-20261028T171339/mutants-rules-control.err`: "FAIL: read_exact_accumulates_a_short_prefix_then_reports_eof") |
| Product vs research difference | master `bdfd0d8d` compose is still B1-form (`CompositionState`+`direct_outcome`, `src/blocking_file.cpp:257-298`) with a separate Precheck/checked_posix_offset admission refactor; candidates local-only; static evidence anchored to B1, not current product — adoption requires re-anchoring | Same finding | Same finding; additionally: P5 ends OPEN, product master contains neither factor |

### 3.2 Per-object verdicts and bases

- **P2C1 — APPROVE.** Basis: all ten §7.2 items checked on evidence the
  reviewer read or executed independently (§3.1 column 2), chief among them
  the stop-state derivation from the two sources' own code (B1's rule table
  vs C1's outcome functions, read from `git show deaf0301`), the
  disassembly-verified EINTR retry path and exactly-one-pread-per-step step
  loop, the reviewer's own sha-equal public-op comparison on the two
  archives' objects, and in-place re-runs of the mutant gate and the release
  matrix leg. Two non-blocking findings (§3.3).
- **P2C2 — APPROVE.** Basis: all ten items checked (§3.1 column 3); the
  decisive observations are the per-branch equivalence derivation on the
  B1→C2 template diff (zero-progress check relocation is an identity when
  transferred==0; all four materializations equal B1's outputs), the
  unchanged step lambdas — the four compose step primitives remain the
  public ops with full per-step admission, so no admission-frequency
  semantic difference exists — and the reviewer's re-run of the 4-seam
  mutant gate with named kills. No findings.
- **Stacked branch — APPROVE.** Basis: the production TU is byte-identical
  to C1's (object sha256 `d5c58bb3…87a8b`; both archives `07d06803…`),
  verified object-level by the reviewer, so P2C1's correctness analysis
  transfers mechanically; on top of that transfer the reviewer
  machine-verified R1's write-side obligations (per-step re-checks, back-edge
  fd/access reloads, register residency 2/2→0/0), confirmed the R4 guards
  one-by-one in `ab.dis` (1130/1133, 1780/1783, 14a0/14a3, 1a90/1a93),
  verified the Amendment-1 control retarget with the archived old-define
  RED, re-ran the combined mutant gate and the release matrix leg in the
  `4ab6ef17` worktree, and read the kit-smoke record (85 checks, 0
  failures, all gated groups identical). Two carried records, neither a
  correctness impact (§3.3).

### 3.3 Findings carried into this record (reviewer "problems")

1. **THREAD-01 concurrent-close boundary (P2C1 observation 1; applies to A
   and to the stacked tree — identical TU).** `read_exact_at`/`read_exact`
   no longer re-run admission per step. If a caller violates THREAD-01
   serialization (closes the File concurrently mid-composition), B1 would
   detect at the next step and stop with `invalid_state`; the candidate
   continues issuing `pread` on the closed descriptor, surfacing as a
   backend/EBADF-class primitive error. The interleaving is outside the
   contract (SEM-03 caller precondition; THREAD-01 caller serialization +
   "no outstanding borrows") — not a violation — but P2C1 §3's obligation
   table did not record the difference; it is recorded here and in
   P5_INTERACTION_OUTCOME.md §12, and carried as an adoption precondition
   (§6.2). Write-side functions are unaffected (step primitives remain the
   public `write_at`/`write` with per-step admission).
2. **Error-path dead store (P2C1 observation 2; implementer-disclosed,
   independently confirmed in machine code).** One new dead store on the
   primitive_error exit (`0x1067 movq $0,0x30(%rsp)` overwritten at
   `0x1074`); error path only, no success path. Maintained as a disclosed
   `[RUN-IMPL]` exception, cross-referenced in outcome §12.
3. **Performance facts carried by the stacked-branch review (no correctness
   impact):** INTERFERENCE_STATIC (write arc 39 > 37) and B's zero textual
   survival — inputs to the §4 S1/S3 adjudication, recorded in §2.
4. **Structural record (stacked review):** R1 leaves zero lines of B's
   textual end state; B's exit-staging transform is carried semantically by
   A's shared template but not instruction-for-instruction — a preregistered
   possible outcome (plan §6 row 1), quantified in the dossier and admitted
   to the adjudication record here (§2.4 row 1, §2.5 item 1).

## 4. Independent performance review (MISSION §7.3)

Four objects reviewed (records supplied to this writing task; verdicts
`[TRANSCRIBED]`). The same **provenance limitation** as §3 applies: the
reviewers' worksheets and re-derivation scripts are not archived in this
repository; these reviews exist in-repo only as the transcriptions here and
in the outcome document.

| Object | Verdict |
|---|---|
| P2C1 (A) | **APPROVE** |
| P2C2 (B) | **APPROVE** |
| P4 workload conclusions | **APPROVE** |
| Stacked branch (P5 matrix) | **REVISE** → closed by the fix round (§5) |

### 4.1 §7.3 checklist — coverage summary

MISSION.md:777-789 requires thirteen independent checks. The reviewers'
coverage (each row: what the reviewer independently did):

| §7.3 item | Coverage (all four reviews) |
|---|---|
| Baseline/candidate identity | All four reviews re-hashed archives, kit binaries and worktree HEADs against the records (d1e351a5…/07d06803…/89450367…/8305d22c…; 77e5399a/deaf0301/476bd9ce/4ab6ef17); the stacked review independently confirmed AB archive and ladder-ab byte-identical to C1. This session re-read `p5-raw/identity.txt` `[RUN]` with the same values. |
| Binary/build profile | Four-configuration matrix records read (release-leg tails "100% / 0 failed"); kit source pinned to one commit `2364be4a` (P4 Amendment 1 clause); N1 bridge difference disclosed with common-mode mitigation. |
| Actual call path | M1/M2/M3 objdump gates reproduced at eight anchors (B1 100/44, 82/34; candidate 68/25, 55/21), re-reproduced on the same object in the P5 S1 dossier; write-side W-M1/W-M2 gates and bounds independently re-reproduced at four archived recounts (C2 81/35, 62/25). |
| Comparator fairness | `a1x` verified as a B1-common-path hand-matched mirror (Layer B): strace 2.000 pread64/op, cursor 0 lseek parity; negative control a1-vs-a2 same binary/path. W01 Layer C structural collapse recorded as structural fact; strategy arms explicitly marked NEW APPLICATION STRATEGY per MISSION §6.2; gather comparison footprint-matched; the 56-vs-57 count difference recorded unresolved. |
| Raw samples | P2C1/P2C2/P4 cells and the P5 cells' median/mean/sd/CI/positive-pair counts re-derived from `raw_deltas` by the reviewers — the stacked-object review reports "41/41 exact MATCH". That 41-cell figure has **no reconstructable decomposition in the archive**: this session counts 32 unique wall cells (`grep -l 'pairs=16' p5-raw/*.txt` → 38 files = 32 wall cells + 6 A/A attempt-1 reruns; plus 4 PMU + 12 strace = 54 cell-type files) `[RUN]`; 41 matches no natural count (32/38/36/48). Transcribed as the reviewer's own count with the discrepancy disclosed; this session independently re-read the analysis tails of every matrix cell cited in §2.2 `[RUN]`. |
| A/A noise floor | 5.535 = 2.131×10.39/√16 lineage self-consistent; P4's four warm rerun sds and cold 2242.535 reproduced exactly (with the reviewer's own analyzer-convention slip located and corrected); P5's session floors recomputed per the archived convention (read 15.475/20.243/19.445; write 53.481/33.026/28.272). |
| ABBA pairing | Per-pair alternating emission order verified on the P5 comp-read-ab cell; subgroup medians recorded in every cell. |
| Kernel/userspace attribution | PMU `instructions:u` cross-arm deltas (P2C1: AX−A1x 128.0→66.1, AC−A1c 90.0→38.1; P4: −62 instr/op recomputed 62.8; P5: +128→+66 read, +64.03→+44.06 write) + rusage user/sys lines + syscall-count equality — no seesaw. |
| Actual syscalls and data movement | strace integral counts equal per arm (pread64 7170=7170 ×3, fstat 83975=83975; P5: 12 cells, PHYSICAL_WORK_DELTA=0). |
| Statistical calibers | Fresh MDE′ + one-way downgrade + dual-report verified as preregistered; no cross-session/cross-kernel numeric comparison; no family-wise correction disclosed; the S3 gate recomputed under both aggregation calibers (both FAIL); the D_ab EXCESS rule applied verbatim including the 1%·N1 term (outcome §8) with the MDE-only reading recorded alongside (review problem 5). |
| Application transfer | Carried by the P4 review (W01/Parquet cells re-derived; both single factors PARITY at workload scale); P5's transfer argument is structural (public-op machine identity), correctly claimed as structure, not fresh measurement. |
| Regression boundaries | Negative-control PARITY gates hold the P2C1/P4 WINs; public-op sha-equal exclusion dual-sourced; non-claimed regions (tail/P99, multi-step wall, non-E5) explicitly excluded; P4's eofmix direction reversal vs the P2 cap recorded as falsified-direction, not escalated (conservative). |
| Mutation oracle (supporting) | mutants archives re-read (heads verified = the pinned SHAs; 7/4/8 seam rows GREEN→RED(named)→RESTORED). |

### 4.2 Per-object notes

- **P2C1 — APPROVE**, no findings. The reviewer explicitly re-derived all
  raw-cell statistics, verified the G5 negative-control re-run disposition
  (original FAIL archived; compliant re-run spans 0 → PASS re-derivation
  holds), and confirmed the cand-eofmix EXCESS cap maintained. Read-only
  review; no gates re-run.
- **P2C2 — APPROVE**, no findings. PMU G3 recomputed (+64.077 → +44.030,
  PASS at margin 0.047 — flagged as marginal, honestly recorded);
  negwrite-1b sub-floor PARITY rule application verified; register-spill
  prediction unfulfilled reported "either way" per the plan.
- **P4 — APPROVE**, four self-disclosed non-blocking gaps verified as
  disclosed (pre-cell silence check not archived; `analyze2.py` not
  archived; strategy-arm oracle narrative downgraded to session narrative;
  one multi-series raw file needing series-3-block citation). The P5 plan
  closed the first two as hard requirements and archived
  `p5-raw/analyze2-p5.py` + a session-open pre-cell env-pin check `[RUN]`
  (both files present; env-pin shows "matching measurement processes:
  none" before the cells and a close block after).
- **Stacked branch — REVISE**, seven findings. The REVISE is about the
  **record being incomplete and specific verdicts needing to be carried**,
  not about any number being wrong: the reviewer's own re-derivation
  ("41/41 exact MATCH" — cell-count figure discussed in §4.1) matched every
  archived statistic, and the reviewer's independent recomputations
  **support** the FAIL verdicts the fix round then recorded verbatim (S2
  STACK_LOSS ×3 as CI-spanning; S3 FAIL at +0.03–0.04 instr/op, explicitly
  "must be recorded FAIL, not waved through on marginality").

### 4.3 REVISE findings and dispositions

| # | Finding (performance review, stacked object) | Disposition (this record / outcome doc) |
|---|---|---|
| 1 | Missing synthesis adjudication document — matrix terminal state dangling | P5_INTERACTION_OUTCOME.md (full conjunction derivation §2–§9) + §2.3 above |
| 2 | A+B degenerates to A (R1 delivered P2C1's file wholesale; B's write-side object code zero lines surviving; arc 39 worse than B's own 35); the matrix measured BASE/A/B/A′; must enter the adjudication record | §1, §2.4 row 1, §2.5 item 1; outcome §10 |
| 3 | S3 verdict missing; preregistered rule not satisfied (recompute 44.06 > 44.03, both calibers); must be recorded FAIL, same order as P2C2 G3's 0.046 pass margin — not waived on marginality | §2.2 group 8, §2.3; outcome §5 (FAIL, margins 0.0015–0.037 by caliber, disclosed) |
| 4 | Persistent session noise not formally disclosed per plan §5.1 (read sd 15.5–20.2, write 28.3–53.5 all > 10 ns after the quiesce rerun; write first-pair outliers in ≥5 write cells) | §2.5 item 5; outcome §11 (A/A table + 7-cell first-pair outlier list; no absorption, no added sampling) |
| 5 | S6 D_ab = +8.248 CI-excluding: pinned-5.535 caliber would read EXCESS, fresh 10.36 not — dual-report disposition and cap semantics must be recorded | §2.2 group 5; outcome §8 adjudicates by the P1 §5.5 rule verbatim (both terms: 8.248 < max(10.35, 1%·N1 9.333) → not EXCESS), records the CI-excluding fact and the MDE-only reading; neither reading changes any S-gate; cap moot given S2 |
| 6 | Group-3 Δ_A eofmix underpowered — classify honestly as attribution reference, not alarm | §2.2 group 3, §2.5 item 6; outcome §11 (same classification) |
| 7 | A/A analyzer pairing conventions differ between P4 (alternating sign flips) and P5 (consecutive pairs) — note the convention in the synthesis | §2.2 preamble (analyzer-convention note); outcome §13; both sessions internally self-consistent, reviewer reproduced both exactly; §5 item 8 of the plan forbids cross-session numeric comparison, so no verdict depends on the difference |

## 5. Fix-round record

Review-close session of 2026-10-28, recorded in P5_INTERACTION_OUTCOME.md
§13–§14 `[TRANSCRIBED]`; this session verified the artifacts it names
`[RUN]` where possible:

1. **No production-code or test changes after any review.** The three
   correctness APPROVEs required no fixes. The performance REVISE was
   resolved by producing the missing adjudication carrier and recording the
   required verdicts verbatim — a documentation obligation, not a code
   change. "No code/script changes after review; the adjudication does not
   depend on any new run" (outcome §13).
2. **Gates re-verified at the final HEAD.** Both gate scripts (committed on
   the branch at `96d74e15`, sha-equal copies in the main workspace's
   `research/direct-beyond-parity/p5-scripts/`) were re-run from the main
   workspace after the review: matrix exit 0 (archive
   `P5AB/matrix-20261028T215804`, four legs 0 failed) and mutants exit 0
   (archive `P5AB/mutants-20261028T215835`, 8 named kills + control)
   `[TRANSCRIBED]`; both archives exist `[RUN]` (directory listing). The
   reviewers also report re-running gates in the `4ab6ef17` worktree
   `[TRANSCRIBED]` — attribution caveat per §3's provenance limitation: the
   only in-repo artifact compatible by timestamp with the correctness
   reviewer's mutants re-run is `P5AB/mutants-20261028T213248` (21:33,
   unclaimed by name), and no archive entry corresponds to the attributed
   release-leg matrix re-run (the 21:58 `matrix-20261028T215804` is claimed
   by the closing session).
   Invocation footnote: one CWD-relative invocation from inside `p5-raw`
   returned 127 (invocation-location error, not a script defect; absolute
   paths unaffected) — disclosed in outcome §13.
3. **Pre-review implementation-session amendments (numbered, not silent):**
   plan Amendment 1 — gate-script filenames (`p5-combined-*` instead of the
   preregistered `P5AB-*`, same content obligations) and the rules-control
   define retarget (`ADVANCE_BY_REQUESTED` → `FAILURE_ACCOUNTED`, commit
   `4ab6ef17`, `xmake/tests.lua` 1 line `[RUN]`), with the old-define RED
   evidence archived (`mutants-20261028T171339/mutants-rules-control.err`).
4. **Prior-phase disclosed gaps closed as preregistered P5 obligations**
   (not review fixes): session-open pre-cell env-pin check
   (`env-pin-2026-10-28.txt` `[RUN]`: "none" before cells, close block
   after) and the two-binary analyzer archived (`p5-raw/analyze2-p5.py`
   `[RUN]`, file present).
5. **Single-candidate frozen evidence untouched.** The two P2C1 reviewer
   observations were recorded to documents (outcome §12; §3.3 here) without
   modifying `P2_CANDIDATE_P2C1.md`/`P2_CANDIDATE_P2C2.md` or any
   `p2-raw/`/`p4-raw/` artifact.
6. **Unclaimed gate archives from the review window (recorded, not
   attributed).** `P5AB/matrix-20261028T215941` and
   `P5AB/mutants-20261028T220011` (heads `4ab6ef17`, mtimes 22:00:03 /
   22:00:25 `[RUN]` via stat + head reads) postdate the outcome document's
   last write (21:59:14) and predate this consolidation (22:12:09); no
   carrier document claims them, and they are not this document's author's
   work (this session was read-only, legend §0). Same HEAD and same gates
   as the claimed archives ⇒ no verdict impact; recorded so that every
   archived artifact carries at least an explicit no-origin disposition
   (§2.5 item 9). Attributing or deleting them is an Owner closeout action.

## 6. Final composition recommendation

### 6.1 Recorded integration candidate

**A alone (P2C1, read-composition with prevalidated register-resident
steps)** is the recorded integration candidate, per the preregistered
fallback: its own gates hold on frozen P2/P4 evidence (KEEP, wall
−16.565 ns/op CI-excluding at P2; `WIN_CONFIRMED` ×3 at P4 fresh MDE′) with
same-session P5 corroboration (group-3 fresh references CI-exclude 2/3
units), and it is the only factor whose static end state survives the
merge. **The stack is not admitted** (S1 write INTERFERENCE_STATIC, S2
STACK_LOSS ×3, S3 FAIL). **B (P2C2) remains a single-factor KEEP on its own
instruction-caliber evidence** (static bounds met; PMU +64.076 → +44.030
instr/op on its own tree; no wall claim) — P5 did not withdraw it, and B's
evidence stands should a future preregistered integration attempt want to
re-derive a write-side stack that actually carries B's object code (this
P5's R1 outcome shows the shared-template merge does not).

### 6.2 Productization path and preconditions

Nothing is productized by this record. Any adoption is a separate,
Owner-gated engineering change following the repository's engineering order
(AGENTS.md). Preconditions, in order:

1. **Owner decision to open a product slice.** MISSION §1: nothing
   auto-adopts; P5 ends OPEN; the branches stay local-only, unmerged and
   unpushed (verified §0/§1 `[RUN]`). No merge happens without explicit
   instruction.
2. **Re-anchor the static evidence to the then-current product tree.** The
   correctness reviews' item-10 finding: master's compose is still B1-form
   (`src/blocking_file.cpp:250-298` `[RUN]`) and carries a separate
   admission refactor (`Precheck`/`checked_posix_offset`); all P2/P5 static
   gates are anchored to B1, not to current master. Adoption requires
   re-running the objdump gates and the mutant/matrix gates against the
   rebased change.
3. **Carry the recorded observations into the obligation table.** The
   THREAD-01 concurrent-close boundary (§3.3 item 1) must be recorded in
   the conformance-ledger obligation row for the compose functions at
   adoption time; the error-path dead store (§3.3 item 2) stays disclosed
   or is removed.
4. **Ledger and review discipline.** Architecture-changing PR cites affected
   root IDs, current/target behavior, validation evidence (the archived
   gates and cells), and records the authority-contraction audit (no new
   runtime state; `direct_outcome`/`publish_certainty` deleted once;
   `CompositionState` out of the production path with the rule table kept
   as the non-authoritative SEM-05 oracle). Independent review before merge
   (the §3 APPROVEs cover the research trees; the product PR needs its own).
5. **Performance claims travel capped.** A's product claim keeps the P2C1
   §12.4 cap ("measurable improvement with re-attributed residual"); P5
   recorded its own fresh verdicts and lifted no single-factor caps; no
   wall claim exists for B; P99/tail remain `NOT_SUPPORTABLE_WITH_REASON`
   everywhere.

### 6.3 Public-capability boundary

The composition work changes **no public surface** — the four public ops
are machine-identical (sha-equal streams), the `Result`/`CompositionOutcome`
ABI is untouched, and adoption of A would be an internal implementation
change under the existing v2-r1 contract, still requiring Owner acceptance
and review per the path above.

**Any new public capability is limited to the AC-1/AC-2/AC-3 design
candidates** filed in `API_CAPABILITY_CANDIDATES.md`, all in the explicit
**Awaiting Owner** state (each candidate's §6): AC-1 positional vectored
read/write, AC-2 gathered positioned read with per-request results, AC-3
explicit file-to-file copy. They are design candidates, not adopted
contracts; no v2-r1 text is rewritten by them; no SDK extension follows from
microbenchmark speed alone; their adoption requires the Owner to accept the
contract and resolve the listed Owner-decision questions (OQ items).
Nothing in P5 changes that state, and this document creates no new
capability proposal.

### 6.4 Handoff

The campaign's remaining stage is P6 (GitHub integration and closeout,
MISSION §8): the umbrella issue structure, research-vs-product PR split,
and the closeout block (MISSION.md:963 `P5_INDEPENDENT_REVIEWS` = this
record) — all Owner-gated, none automatic.
