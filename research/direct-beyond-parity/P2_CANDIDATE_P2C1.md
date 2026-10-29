# P2 Candidate Report — P2C1 read-composition prevalidated register-resident steps

- Family: G-READCOMP-01 = {G-REA-01, G-REA-02, G-REA-03} + shared-cursor twins
  {G-REX-01, G-REX-02, G-REX-03} (`CANDIDATE_GAP_REGISTER.md`).
- Plan: `P2_EXPERIMENT_PLAN.md` §3 (hypothesis §3.2, mechanisms §3.3, cells §3.4,
  pass rules §3.5). Mission hooks: MISSION §4 P2.2/P2.3.
- Branch: `direct-read-compose-prevalidated-steps` (local-only, never pushed), cut
  from B1 `77e5399a45998dea396699fa2e063c94a665945a`; worktree
  `/home/jnhu/Source/Sluice-direct-read-compose-prevalidated-steps`;
  HEAD `deaf03014173e13633a43f4db405c9a9e9860f23` `[RUN]` (`git rev-parse`,
  `git log 77e5399a..HEAD` → 4 commits).
- Candidate release archive sha256 `07d06803…57b580283df189f66dcdc2052049e`
  `[RUN]` (`sha256sum` on the worktree archive; identical to
  `p2-raw/P2C1/matrix-release-archive-sha256.txt`).
- **Decision: REVISE** (§9 below; KEEP conjunction G1+G2+G3+G5 not met — G5's
  `p2c1-negread-4k` cell is formally EXCESS under the preregistered rule).
- Provenance legend: `[RUN]` = executed/re-verified by the report writer this
  session (command named inline); `[RUN-IMPL]` = executed by the implementation
  session whose record passed its independent review (gate note "门禁与复核均
  通过", zero verify problems), raw archived; `[RUN-MEAS]` = executed by the
  measurement session, raw archived under `p2-raw/`; `[TRANSCRIBED]` = taken
  from a session record the writer could not re-derive; `[UNCERTAIN]` per
  `P0_BASELINE_AND_OWNERSHIP.md` §0. Cell numbers below were re-read by the
  writer from the named raw file unless marked otherwise.

## 1. Source diff

Cumulative `git diff --numstat 77e5399a..deaf0301` `[RUN]` (writer-run): 6
files, +301/−37 —

| File | Δ | Content |
|---|---|---|
| `include/sluice/detail/file_semantics.hpp` | +9/−0 | Three `SLUICE_COMPOSE_MUTANT_*` seam guards on `compose_progress` (dormant; zero BASE code change) |
| `src/blocking_file.cpp` | +91/−37 | Six compose seam guards; `step_retry_on_eintr`; private `prevalidated_pread_step`/`prevalidated_read_step` (pread/read + EINTR retry + count, entered after one outer admission); per-exit-flavor outcome materializers (complete / stalled / primitive_error) feeding the sret directly; register-resident compose loop. `direct_outcome` and `publish_certainty` deleted (no remaining consumer). 128 changed lines total; the implementation record's prose "+128/−37" reads the diffstat bar — corrected here to the measured numstat |
| `tests/direct_composition_fault_test.cpp` | +27/−0 | New named killer `write_all_impossible_count_stops_immediately` (the missing write-side impossible-count script, plan §2.1) |
| `xmake/tests.lua` | +21/−0 | Seven `direct_mutant_target()` targets for the seam set |
| `research/direct-beyond-parity/p2-scripts/P2C1-matrix.sh` | +50/−0 | Four-configuration matrix gate (P1 §8 legs, `--liburing=n`) |
| `research/direct-beyond-parity/p2-scripts/P2C1-mutants.sh` | +103/−0 | GREEN→RED→RESTORED kill gate |

Commit order (narrow commits, plan §2.2) `[RUN]` (`git log`, per-commit
numstat): `d79244b0` seams + killers (+78/−4) → `31d18b6a` mechanism
(`blocking_file.cpp` +81/−44) → `5180e27a` gate scripts (+50/+103) →
`deaf0301` ANSI-parse fix in the matrix script. Public
`read_at`/`read`/`write_at`/`write` bodies are not modified (plan §1 scope pin;
verified at machine level in §2). The write-side compositions share the new
loop/exit machinery, but their step primitives remain public
`write_at`/`write` — single-factor discipline; the write-side per-step factor is
P2C2's, not this candidate's (G-WALLAT-01/G-WALL-01 explicitly retained here).

Frozen-worktree discipline `[RUN-MEAS]`: `/home/jnhu/Source/Sluice-600-wt`
(HEAD still `77e5399a`, status clean) and the frozen archive
`d1e351a54ded3dfa…3007f0` were used read-only; the writer re-hashed the frozen
archive this session `[RUN]` (`d1e351a54ded3dfa` prefix match).

## 2. Machine-work delta (static + PMU)

### 2.1 Static, implementation session `[RUN-IMPL]`, review-passed

Counting convention: P1 audit exact-path convention
(`p1-audit/composition-read.md` header — legal nonempty call, first step full
count, all conditional branches/syscall `call`/`ret` on the path, alignment
nops excluded). The session's deterministic path counter first reproduced all
B1 reference numbers on the frozen B1 archive (read_at 39 / read 29;
read_exact_at 100/44; read_exact 82/34) before counting the candidate; same
counter, same convention on both sides.

| Mechanism (plan §3.3) | Objdump gate | Result |
|---|---|---|
| M1 prevalidated step primitive (G-REA-01/G-REX-01) | no closed/access/range/transfer re-test sequence inside the compose loop; exactly 1 pread/read call per step | re-test grep count 0 in the `read_exact_at` step loop (0xfc0–0xfe5 + pread call + back-edge 0x1130–0x1157) and in the `read_exact` loop (0x1600–0x161d + 0x1780–0x17a7); B1 carried ≈13 re-tests per step at 0xfc0–0xfff |
| M2 state register residency (G-REA-02/G-REX-02) | zero textual `movaps …0x20(%rsp)` / `…0x10(%rsp)`; no `movdqa`+`movaps` round-trip on the back-edge | 0/0 in-function; whole-object grep 0 hits for both displacements (B1 dead-store blocks 0x1277/0x19f7 and the 8-instr/7-store 0x1250–0x127f per-step block gone) |
| M3 single-point exit materialization (G-REA-03/G-REX-03) | one materialization site per exit flavor; B1 dead zero-stores absent | complete = 0x1170 block, eof = 0x11e8, primitive_error/impossible_count share 0x104f; B1 dead zero-stores (0x1285/0x1294, 0x1a05/0x1a14) gone; the live 16-B store at complete-exit 0x1179 is read back by 0x107d into the sret |

Pinned recount bounds (plan §3.3) `[RUN-IMPL]`, all PASS:

| Path | B1 | Candidate | Bound | Verdict |
|---|---|---|---|---|
| `read_exact_at` 1-step | 100 | 68 | ≤ 75 | PASS (−32) |
| `read_exact_at` per-extra-step arc | 44 | 25 | ≤ 26 | PASS (−19) |
| `read_exact` 1-step | 82 | 55 | ≤ 60 | PASS (−27) |
| `read_exact` per-extra-step arc | 34 | 21 | ≤ 21 | PASS (−13, exactly at the bound) |
| public `read_at` | 39 | 39 | ±0 | PASS |
| public `read` | 29 | 29 | ±0 | PASS |

Impossible-count guard retained on the back-edge (0x1130/0x1133 cmp+jb) —
oracle-pinned, kept by the fixed admission decision (plan §1). Disclosed
exception `[RUN-IMPL]`: one *new* dead zero-store on the primitive_error exit
(`0x1067 movq $0,0x30(%rsp)` overwritten at 0x1074) — error exit only, on no
measured success path.

### 2.2 Static, writer re-verification `[RUN]` (this session)

On the three release archives (frozen B1 `d1e351a5…`, candidate `07d06803…`):

- `movaps` dead-store gate: extracted `blocking_file.cpp.o` from each archive
  (`ar x`), `objdump -d --demangle`; grep `movaps.*0x20(%rsp)` = **2** and
  `movaps.*0x10(%rsp)` = **2** in B1 (read 0x1277/0x19f7 + write
  0x1677/0x1d77), **0 and 0** in the candidate. The M2 gate holds on the
  delivered artifact.
- Public-op ±0 at instruction-stream level: address-stripped disassembly of
  `sluice::blocking::read_at/read/write_at/write` is byte-identical (sha-equal)
  across B1 and the candidate — stronger than the ±0 path count. A
  candidate-caused regression of the untouched public ops is excluded at
  machine level (relevant to the G5 adjudication in §8).
- Composition symbols shrink as claimed: whole-function disassembly lines
  read_exact_at 242→174, read_exact 210→167, write_all_at 242→211,
  write_all 209→183 (writer's own line counts; direction and rough magnitude
  consistent with the session's exact-path numbers).
- Kit bridge `[RUN-MEAS]` (`p2-raw/kit/ladder-identity.txt`): N1 wrappers
  inlined into `one_op`, whose frame grew (`addq $248→$296`) — pre-existing
  arm instruction streams are not byte-identical between the P1 and P2 kit
  binaries; disclosed per plan §2.3/§6.3. Mitigation actually applied: no
  existing arm body source changed (commit diff touches none), old-binary
  functional checksum bridge MATCH×4, and both sides of every cross-binary
  cell build from the same kit source (common-mode harness TU).

### 2.3 Measurement-session static segment `[RUN-MEAS]` — NOT_RUN disclosure

The measurement segment did **not** re-run the §3.3 audit-convention per-path
recount; it disclosed symbolic-level substitutes instead (read_exact_at
215→162 objdump lines, read_exact 193→157; dead stores 4→0 — independently
confirmed by the writer's grep in §2.2; call shape 1×pread + 1×__errno_location;
public symbols IDENTICAL; N1 `one_op` identical except 3 call addresses).
The gated static evidence of record is therefore the review-passed
implementation-session recount (§2.1) plus the writer's artifact checks (§2.2);
the recount re-run remains open work if the G5 re-measurement (§9) is executed.

### 2.4 PMU corroboration (non-gating, plan §3.2) `[RUN-MEAS]`, raw re-read `[RUN]`

`perf stat instructions:u`, whole-process, per arm /4096 ops; never a wall
source (P1 §6). From `p2-raw/p2c1/p2c1-base-composition-4k-pmu.txt` and
`p2c1-cand-composition-4k-pmu.txt` (writer recomputed the deltas):

- AX−A1x (positioned composition excess): B1 **+128.0** instr/op (524,385/4096;
  P1 session references +118.2/+123.4 — same order, 4.8 instr/op above the
  higher reference; non-gating corroboration caliber — the raw file's own tail
  comment says "same band", which overstates the match) → candidate **+66.1**
  (270,619/4096);
  drop **61.9 ≥ 30** → preregistered corroboration threshold met (static
  −21…−30 at the observed ≈3× PMU/static ratio).
- AC−A1c (cursor twin, first measurement): B1 **+90.0** (368,791/4096) →
  candidate **+38.1** (155,868/4096); drop 52.0.

### 2.5 Syscall parity `[RUN-MEAS]`

strace whole-trace net counting (first attempt with `-e trace=pread` was
rejected by this host's strace — disclosed, method switched): seq
2.000 `pread64`/op (positional, matched); cursor 2.000 `read`/op with
**0.000 lseek**/op (no-seek streaming discipline proven); tail 2.6667
timed+verify = 2× the preregistered 1.3333 (timed-window caliber note recorded
in the plan's terms); arm-identical in every cell. Raw:
`p2-raw/p2c1/*composition*.syscalls.txt`.

### 2.6 Independent re-verification status (inputs for the P5 re-review)

Not independently re-executed by any later session (the writer's §2.2 checks
are the weaker symbol-level gates; the measurement segment likewise did not
re-run it, §2.3): the audit-convention per-path instruction recount of §2.1 —
the one-off /tmp counters and the session-derived candidate address intervals
are not archived under `p2-raw`, so the 68/25/55/21 numbers and their
bound-PASS verdicts rest on the review-passed implementation record. Also
carried from the run material with no `p2-raw` archive: the
implementation-phase review conclusion itself ("门禁与复核均通过", zero
verify problems). Neither item changes a gate derivation above that is not
already grounded in archived raw or writer-run checks as labeled; both are
explicit inputs to the P5 independent re-review.

## 3. Semantic proof (against P0 §5)

Floor: `P0_BASELINE_AND_OWNERSHIP.md` §5.1 obligations, all anchored to v2-r1
requirement IDs. This candidate touches only composition internals; the
unchanged-surface argument is machine-verified (public four-op instruction
streams identical, §2.2) and behavior-verified by the suite matrix (§4) and
the smoke gate `[RUN-MEAS]` (`p2-raw/kit/smoke-ladder.txt`: "SUMMARY: ALL
GATED GROUPS IDENTICAL" over read {a0,a1,a2,a1x,ax,a1c,ac} seq×7 sizes + tail
+ rand, write group with `content_ok=1`, metadata×3, closed group, EINTR
diagnostics).

Composition-internal obligations carried by named oracles (P0 §5.1 rows):

| Obligation (P0 §5.1 anchor) | Oracle that pins it | Status under this candidate |
|---|---|---|
| EINTR: retry without completed count (SEM-05/ERR-01) | `primitive_retries_eintr_without_a_completed_count`, `primitive_write_retries_eintr_then_stops_at_the_first_count` | seam-provable (§4); suite green |
| EOF: 0 at observation is success, prefix kept (SEM-05) | `read_exact_reports_eof_before_full_with_the_prefix`, `short_reads_report_eof_before_full_with_the_prefix` | seam-provable; suite green |
| Partial transfer is success, primitive does not loop (SEM-05) | `short_reads_reach_full_progress`, frozen `semantic_short_io_reference_test` table | seam-provable; rule suite stays green under mutation (§4) |
| No-progress stop (SEM-05) | `zero_progress_write_stops_after_one_attempt`, `composition_never_spins_on_zero_progress`, `read_exact_accumulates_a_short_prefix_then_reports_eof` | seam-provable; suite green |
| Confirmed bytes / effect certainty (ERR-02) | `error_after_confirmed_prefix_keeps_the_prefix`, `a_primitive_error_reports_an_unaccounted_remainder_in_both_directions`, `every_stop_reason_is_pinned_against_the_oracle_error_rule` | seam-provable; suite green |
| Impossible-count stop (register §3 / `DIRECT_COST_FLOOR.md` §3.8) | `impossible_count_stops_immediately` + new `write_all_impossible_count_stops_immediately` | guard kept; seam-provable (§4) |
| Closed/access precedence, range/overflow, zero-length, null buffer (SEM-03/04) | 58-test matrix incl. `semantic_validation_precedence_test` (direct 21/21 executed floor, P0 §5.2) | matrix green ×4 configs (§4) |

Authority-contraction audit (AGENTS.md; recorded with the mechanism commit
`[RUN-IMPL]`): `direct_outcome`/`publish_certainty` lost their last production
consumer and were deleted; `detail::compose_progress`/`compose_error`/
`composition_effect_certainty` remain as the shared SEM-05 rule table —
non-authoritative for production flow, but the oracle directly consumed by
`semantic_short_io_reference_test` and the every-stop-reason rules tests (P0
§5 oracle status). The mechanism adds no runtime state; it removes per-step
state materialization.

Review conclusion (given in the run material): implementation stage VERIFIED,
gate note "门禁与复核均通过", zero verify problems — i.e. the §2.1 static
claims and gate runs above carry a passed independent review in addition to
the writer's §2.2 artifact checks.

## 4. Mutation evidence (seams and named kills)

Gate: `p2-scripts/P2C1-mutants.sh` (committed on the branch), run with CWD =
candidate worktree; exit-code adjudicated; RED requires exit code exactly 1
with the precise line `FAIL: <named killer>` on stderr — timeout/crash/build
failure is a gate failure, not a kill (plan §2.1 kill discipline).

`[RUN-MEAS]`+`[RUN-IMPL]`, records re-read `[RUN]`:

- Archived evidence vs the three-tree claim (corrected after independent
  review): only the final-tree leg is fully archived (`mutants-final-run.txt`
  + head marker `mutants-head.txt` = `deaf0301`, run 21:24; the 22:10
  `.err`/build/config artifacts come from a second, later rerun on the same
  tree). The
  seams-tree leg originally left only a head marker
  (`mutants-head-seams-commit.txt` = `d79244b0`, no run output) and the
  `5180e27a` leg left no archived output — `P2C1-mutants.sh` writes fixed
  filenames and each later run overwrote the earlier evidence (gate-script
  defect, disclosed; the P2C2 script timestamps its archives). Gap closed by
  the writer this session `[RUN]`: the gate was re-run on a scratch
  `git archive d79244b0` export with the branch-head script byte-identical
  except WT/RAW redirected to /tmp (diff-verified) — **exit 0, 7/7 seams
  GREEN→RED(named)→RESTORED GREEN**, identical named killers; record at
  `p2-raw/P2C1/mutants-seams-rerun-20261027T2325.txt`. Both preregistered
  legs (pre-mechanism tree, post-mechanism tree) now have archived evidence —
  `5180e27a`/`deaf0301` carry production code identical to the mechanism
  commit `31d18b6a` (the two later commits touch only the gate scripts), so
  the archived `deaf0301` run is the post-mechanism leg; the original
  11:03-era seams run and the `5180e27a` run themselves remain unarchived
  `[RUN-IMPL]`.
- Final run (`p2-raw/P2C1/mutants-final-run.txt`), 7/7 seams
  GREEN→RED(named)→RESTORED GREEN:
  `ADVANCE_BY_REQUESTED`, `RETRY_ZERO_PROGRESS` (fault_test leg →
  `short_reads_report_eof_before_full_with_the_prefix`; rules-suite leg →
  `read_exact_accumulates_a_short_prefix_then_reports_eof`),
  `FAILURE_ACCOUNTED` → `error_after_confirmed_prefix_keeps_the_prefix`,
  `SKIP_IMPOSSIBLE_COUNT` → `impossible_count_stops_immediately`,
  `STEP_REPORTS_FULL_COUNT` → `short_reads_report_eof_before_full_with_the_prefix`,
  `STEP_SKIPS_EINTR_RETRY` → `primitive_retries_eintr_without_a_completed_count`.
- Two-stage seam placement (by design): at the seams commit the EINTR-retry
  mutation wraps the four public primitives (on that tree the composition step
  *is* the public primitive); the mechanism commit moves it inside
  `prevalidated_*_step`; public `read_at`/`read` shapes stay 39/29 on both
  trees. On the mechanism tree the three `SLUICE_COMPOSE_MUTANT_*` guards also
  sit inside the new production loop (otherwise only the oracle rule functions
  would mutate and production mutation force would be lost).
- Disclosed incidents, both resolved and re-run: one mutant build failed
  `-Werror` on an unused function (not a kill) → `[[maybe_unused]]` on
  `stalled_outcome`, rerun clean; one stale-binary exit 127 → the script now
  forces `-r` rebuild of mutant targets.
- Killer-mapping honesty notes `[RUN-IMPL]`: `read_exact_reads_the_whole_request`
  (8/8 full read) cannot by construction distinguish a fabricated full count —
  the fabrication mutants are actually killed by the scripted short-read
  assertions above; the plan's write-side killers that sit after the
  first-failure point in suite order (e.g. `short_writes_reach_full_progress`)
  are unreachable as first-kill in a single-main suite — the landing commit
  added the missing write-side named script
  (`write_all_impossible_count_stops_immediately`) in the same commit per plan
  §2.1 discipline.

Matrix gate: `P2C1-matrix.sh` exit 0 — release/debug/asan/asanubsan
(`--liburing=n`) each 58/58, 0 failed `[RUN]` (re-read
`p2-raw/P2C1/matrix-{release,debug,asan,asanubsan}.txt`; release leg tail:
"100% tests passed, 0 test(s) failed out of 58"). First matrix run failed only
because the script's parser was blinded by xmake ANSI color codes (all four
legs had already passed; verified leg-by-leg, fixed in `deaf0301`, whole gate
re-run to exit 0) — disclosed as a gate-script defect, not a suite failure.

## 5. E5 A/A (session-opening noise floors)

Per plan §2.4 addition 2, each binary first, serially exclusive `[RUN-MEAS]`.
Attempt chain re-read `[RUN]` from the raw headers: read-base attempt1 void
(fixture incident, §6 preamble); attempt2 valid but sd_AA = 17.272 ns
(> 10 ns #600 band) → one §11.10 quiesce rerun (45 s, load 0.10 at start)
accepted at sd_AA = 14.602:

| Cell | sd_AA | MDE′ = 2.131·sd_AA/√16 | Recorded verdict |
|---|---|---|---|
| `p2c1-aa-read-4k-base` (B1 binary) | 14.602 ns | 7.779 ns | accepted; wider than pinned 5.535 → dual-reported |
| `p2c1-aa-read-4k-cand` (candidate binary) | 15.562 ns | 8.28 ns as recorded (recomputes to 8.29 from the raw sd; immaterial to every verdict below) | accepted |

Both read-track sd_AA exceed the 10 ns #600 noise band; persistent session
noise disclosed (P1 accepted 10.39/29.46 in its session). The one-way MDE′
rule (plan §2.4 addition 1): MDE and MDE′ are dual-reported and any claim
that would flip under MDE′ is downgraded — applied in §6/§8/§9.

## 6. Paired A/B (16 ABBA pairs, interleaved, taskset -c 2, warm tmpfs)

All integrity gates green per accepted cell (exit 0, no VERIFY_FAIL, in-group
checksum equality, read-fixture sha equal to the pinned value at session start
and session close) `[RUN-MEAS]`; analysis tails re-read by the writer `[RUN]`
from the named files.

**Mid-session fixture incident (disclosed; raw re-read `[RUN]` from the
INCIDENT note in `p2-raw/kit/smoke-ladder.txt` and the voided attempt
`p2-raw/p2c1/p2c1-aa-read-4k-base-attempt1.txt`):** the smoke script's
old-binary bridge section ran the write arms `w1x`/`wx` against the read
fixture `/tmp/sluice600/data64m.bin`, whose prime pass rewrote it (sha
`bc020fd1…` at smoke start → `fc37759c…` after). Detected immediately by the
first A/A cell's a1 content-oracle gate (VERIFY_FAIL — the gate worked as
designed; attempt archived and voided). The fixture was regenerated
deterministically (mkfixture; sha back to the pinned
`bc020fd15a622e081793b0fb8ff2383838e27f6d1cb2135f204aad8383b2993a`) and every
accepted cell in §6 ran on the restored fixture; the smoke bridge section was
repaired to use a dedicated scratch. Group audits preceding the bridge
section were unaffected (bridge checksums are file-independent). Every wall
number below therefore depends on the restored, pinned fixture — the
"unchanged" claim holds across accepted cells, not across the whole session
wall-clock.

### 6.1 H0 settlement — `p2c1-base-composition-4k` (B1 binary, A1x vs AX)

Raw `p2-raw/p2c1/p2c1-base-composition-4k.txt`:

- **S = +15.439 ns** median EXCESS (mean +18.994, mean-CI [+8.906, +29.082]
  excludes 0, 14/16 pairs B1-arm slower; AB −14.178 / BA −20.264).
- Outcome (a) S ≥ 2×MDE = 11.07 at the pinned floor: **holds** — the P1
  magnitude reproduces on this host state (fresh mean +18.994 vs P1 median
  +18.992); #600's +9.7…+9.9 `[TRANSCRIBED]` is *attributed* to cross-round
  drift (comparator/host/kernel state or residual contention —
  `DIRECT_COST_FLOOR.md` §6.2 candidate explanations; recorded as attribution,
  not proof, per plan §6.5).
- Outcome (b) (S < 11.07, resolve toward the #600 band): decisively excluded
  (S exceeds the threshold by 4.4 ns under any floor; P1 session-state
  inflation explanation refuted).
- MDE′ flip check: 2×MDE′(base) = 15.558 > S by 0.119 → label (a) downgrades
  to NO_CLAIM at the MDE′ floor (0.12 ns margin; does not reopen (b)).
- **Beat-target fixed here**: all P2C1 improvement statistics are S-relative to
  this session's S = +15.439 — never a transcribed prior-round number.
- First-measurement twins on the B1 binary: S_eof = +16.577 (CI-excluding,
  EXCESS under both floors); cursor read AC−A1c = +17.508 (EXCESS, first-ever
  `read_exact` wall cell); PMU +128.0 (§2.4).

### 6.2 H1 primary — `p2c1-delta-composition-4k` (cross-binary, ax, cand−baseline)

Raw `p2-raw/p2c1/p2c1-delta-composition-4k.txt`: candidate median 720.428 /
B1 median 736.626 ns; **Δ_own = −16.565 ns** median (mean −14.038, sd 12.704,
95% CI **[−20.806, −7.270] excludes 0**, 15/16 cand-faster).

- G3 claim rule (plan §3.2): Δ_own ≤ −5.535 AND CI excludes 0 → **met**;
  robust at both MDE′ floors (|Δ_own| = 16.565 > 8.28/7.779, no flip).
- Refutation band: I = 16.565 ≥ 3 ns → H1 refutation avoided; the +~19 ns
  aggregate *is* carried by the G-REA-01/02/03 package on this host.
- Consistency: S (+15.439) → D_cand (−3.563) ≈ 19 ns drop, matching |Δ_own|.
- Out-of-band disclosure: −16.565 exceeds the preregistered prediction band
  [−12, −5] (favorable direction; recorded as band-external, no gate attached).

`p2c1-delta-eofmix-4k` (tail): Δ_own_eof = **−9.813 ns** median (mean −11.458,
CI [−17.947, −4.968] excludes 0, 14/16) — measurable at both floors.

### 6.3 H2 endpoint — candidate-binary composition and eofmix cells

- `p2c1-cand-composition-4k`: A1x 701.591 vs AX 699.824; paired delta
  (A1x−AX) median +3.563 ns, mean-CI [−3.981, +6.693] includes 0; in the
  EXCESS convention (D = AX−A1x) that is D_cand = **−3.563 ns**
  < max(5.535, 1%·701.591 = 7.016) → **PARITY_REACHED**: the
  G-READCOMP-01 wall EXCESS on the 4 KiB composition cell is eliminated
  (S +15.439 → −3.563). Same verdict under either MDE′.
- `p2c1-cand-eofmix-4k`: A1x 701.044 vs AX 706.970; paired delta (A1x−AX)
  median −10.228 ns, mean-CI [−15.019, −2.115] excludes 0; in the EXCESS
  convention D_cand_eof = **+10.228 ns** (EXCESS-sign mean-CI
  [+2.115, +15.019]) ≥ max(5.535, 7.010) → **still EXCESS**
  (also EXCESS under MDE′ 7.779). Improved ≈6.3 ns from S_eof = +16.577 but
  the residual stands → per plan §3.2 H2, the residual must be re-attributed
  under H1's refutation discipline. The quantitative split (tail cells are
  EOF-boundary multi-step compositions where the per-extra-step gain is
  smaller than the 1-step path; §3.3 per-extra-step recount not re-run in the
  measurement segment, §2.3) is **open work** — recorded, not resolved.

### 6.4 H3 twin — cursor `read_exact` cells (first-ever wall measurements)

- Baseline AC−A1c = +17.508 EXCESS (§6.1).
- Candidate `p2c1-cand-cursor-composition-4k`: A1c 719.575 vs AC 720.054;
  paired delta (A1c−AC) median −4.589 ns, mean-CI [−10.115, +5.790] includes
  0; EXCESS-convention D = **+4.589** < max(5.535, 7.196) → PARITY (EXCESS
  eliminated on the first candidate twin).
- Cross-binary `p2c1-delta-cursor-composition-4k`: Δ = **−16.555 ns** median
  (mean −13.884, CI [−22.510, −5.259] excludes 0, 11/16; sd 16.190 with
  **5/16 positive pairs** (+5.240, +2.095, +7.572, +3.164, +2.561 — count
  corrected after independent review from the raw 16 deltas) — wider spread
  disclosed; median verdict robust) — twin
  improvement claim holds at both floors, same magnitude as the positional
  twin (−16.565). PMU AC−A1c 90.0 → 38.1 (§2.4).

## 7. Benefit boundary

- Measured wall benefit: Δ_own −16.565 ns/op at 4 KiB seq (CI-excluding, both
  floors) and −9.813 ns/op at the tail mix; twin −16.555 ns/op. Mechanism
  class: elimination of per-step re-admission + per-step state round-trip +
  double exit materialization inside the four composition functions only.
- No benefit is claimed for: public single ops (machine-identical, negative
  controls §8); per-item ns splits inside the package (static-derived; the
  combined-branch design means a wall refutation would refute the package,
  not an item — plan §6.1; here the package is confirmed, items stay
  static-attributed `[UNCERTAIN]` per-item); tail-latency/P99 (out of scope,
  plan §6.6); any host other than E5 native Linux (plan §6.4).
- Wall caliber only at 4 KiB-class shapes; multi-step composition benefit is
  bounded below by the per-extra-step arc reduction (44→25 / 34→21 static)
  but has no dedicated wall cell (scripted short reads are test-seam
  territory, P2C2 §4.4 analogue disclosure).
- The eofmix endpoint keeps a +10.228 ns residual (§6.3) — the benefit does
  **not** reach wall parity on the tail mix yet.

## 8. Negative controls (G5)

- `p2c1-negread-1b` (a1 vs a2, 1 B): median −1.057 ns, CI includes 0 →
  **PARITY, PASS** (raw `p2c1-negread-1b.txt`).
- `p2c1-negread-4k` (a1 vs a2 @ candidate binary): A1 708.115 / A2 720.335;
  delta median **−9.685 ns**, mean −7.837, CI [−14.424, −1.250] **excludes
  0** → A2 EXCESS +9.685 ≥ max(5.535, 7.081) → **formally EXCESS — G5 FAILS
  the preregistered rule** ("pass = PARITY … any EXCESS is a regression",
  plan §3.2/§3.5). Raw `p2-raw/p2c1/p2c1-negread-4k.txt`.
- Attribution evidence recorded with the cell (does not waive the rule):
  (i) the `read_at` instruction stream is identical between the B1 and
  candidate archives — writer-verified this session `[RUN]` (§2.2), so a
  candidate-caused regression of this op is excluded at machine level;
  (ii) the same-session B1-side twin cell `p2c1-negread-4k-b1side` shows the
  same/larger excess (median −10.538, CI [−17.117, −2.205] excludes 0) — the
  candidate side is 0.85 ns *smaller*; (iii) P1's same cell measured −9.037
  median with CI including 0. Reading: this host session places the a1/a2
  4 KiB paired difference in a +9…+10 ns band that crossed the CI threshold —
  a probable false positive. The measurement record defers to owner
  discretion; under the preregistered rules as written, the cell is a G5
  failure until a compliant re-run (§2.4 serial exclusivity, quiescent host,
  with the B1-side control) adjudicates it. No such re-run was executed.

## 9. Decision — REVISE

Gate-by-gate derivation (plan §3.5, numbers from §2–§8):

| Gate | Rule | Recorded outcome | Verdict |
|---|---|---|---|
| G1 machine (gating) | §3.3 objdump gates + recount bounds + public-op shapes + N1 bridge | objdump gates pass (impl review-passed; dead-store and public-op gates independently re-verified `[RUN]` §2.2); bounds 68≤75, 25≤26, 55≤60, 21≤21, public ±0; N1 bridge disclosed + common-mode mitigation (§2.2) | **PASS** |
| G2 correctness (gating) | mutants.sh exit 0 at seams-only and mechanism commits; matrix.sh exit 0 | exit 0 on three trees; matrix exit 0, 58/58 ×4 legs (§4) | **PASS** |
| G3 improvement | Δ_own ≤ −5.535 AND CI excludes 0 | −16.565, CI [−20.806, −7.270] excl 0; robust under MDE′; I = 16.565 ≥ 3 (§6.2) | **PASS** |
| G4 parity endpoint | D_cand and D_cand_eof no longer EXCESS | composition −3.563 PARITY_REACHED; eofmix **+10.228 EXCESS** (§6.3) | **PARTIAL — recorded; caps the claim at "measurable improvement with re-attributed residual"** |
| G5 negative controls | negread-* PARITY; no cell EXCESS | negread-1b PARITY; negread-4k formally **EXCESS** (§8) | **FAIL** |

KEEP-eligible iff G1+G2+G3+**G5** → conjunction not met on the recorded
numbers. REJECT triggers (I < 3 ns; static bounds failing; G2 red) — none
apply. Therefore: **REVISE**.

REVISE conditions (preregistered-rule-compliant closure, no rule waiver):

1. Re-run the negative-control cells (`p2c1-negread-4k`, `p2c1-negread-1b`)
   under §2.4 serial exclusivity on a quiescent host, with the B1-side control
   cell, and re-derive G5 from the re-run record. If PARITY is restored, the
   KEEP conjunction is then G1+G2+G3+G5 with all gates green.
2. Execute the H2-mandated re-attribution of the +10.228 ns eofmix residual —
   quantitatively, via the per-extra-step recount split (measurement-segment
   recount currently NOT_RUN, §2.3) — before any wall-parity claim extends to
   the tail mix. Until then the wall claim is capped at "measurable
   improvement with re-attributed residual" (plan §3.5 language).
3. Nothing in the mechanism is falsified: static decomposition confirmed
   (bounds), aggregate attribution confirmed (I = 16.565 ≥ 3), correctness
   seams intact, cross-binary improvement CI-excluding on both twins. The
   branch and its evidence stand as recorded; REVISE is a measurement-closure
   verdict, not a code verdict.

Terminal-state record: no GATE_FAILED or IMPL_FAILED candidates exist in this
run — P2C1 reached VERIFIED implementation (review passed, zero problems) and
full measurement; the failure mode here is a preregistered measurement gate
(G5) formally unmet on the recorded cell.

## 10. Deferred register entries (plan §5, shared across P2)

| ID | Reason (ledger-cited) |
|---|---|
| G-READAT-01 (+ read twin) | Null-data guard (3 instr + 1 not-taken branch, `src/blocking_file.cpp:86-87`/`:126-127`): delete-or-anchor-in-root is an owner decision under the AGENTS.md mechanism-justification gate; the frozen N1 comparator carries the same check, so removal yields no beyond-parity gain and a successor N1 arm must drop it in lockstep — a semantics decision, not a P2 performance candidate. |
| G-WRITEAT-02 | Same owner gate as G-READAT-01; 2–3 instr + 1 branch per site (`blocking_file.cpp:107-108`/`:144-145`), ≪ any MDE; comparator carries it. |
| G-READAT-02 | Register marks record-only: EINTR re-entry re-spill (4 of 7 re-entry instructions, `blocking_file.cpp:89-92`), error-path only, no admitted workload at measurable frequency. |
| G-META-01 | No executable admissibility gate: build defines no msan mode (`xmake.lua:3-16`); ASan/UBSan do not detect uninitialized reads; requires an msan mode or equivalent uninit oracle first. Static-only until then. |
| G-OPEN-01 | Measured PARITY (open+close +11.487 ns, CI spans 0, < 15.969); the register's own falsifier anticipates no repeatable gain (kernel floor); rejected for standalone admission by the #600 rule. |
| G-WRITEAT-01 | RESOLVED by P1 (write ladder executed: wall PARITY at every claimable size, +19 instr/op PMU constant, 1.000 pwrite64/op); entry retained for traceability. |

## 11. Combined (P2C1+P2C2) outcome

**Not executed.** The optional combined measurement (MISSION P2.2: run only
if the single factors all hold; independent gains must never be added) did not
run: its precondition was not met this session — P2C1's G5 gate is formally
unresolved (§8) — and no combined cell was measured. See
`P2_CANDIDATE_P2C2.md` §11 for the same statement from the write-side record.

## 12. REVISE Closure Addendum (2026-10-28)

This addendum closes the two preregistered REVISE conditions of §9 from two
new records and re-derives the verdict under plan §3.5's conjunction rule
(§12.5); the G5 re-run stands on this report's own §8/§9 preregistered
disposition (§12.1 provenance note) — no rule waived, no criterion loosened
for the outcome. §1–§11 above remain the frozen original record;
nothing there is re-adjudicated beyond the two conditions' own scope. Inputs:

- Condition 1 (G5 re-run): negative-control re-run session, 2026-10-28, raw
  `p2-raw/p2c1/*-rerun*.txt` + derivation record
  `p2-raw/p2c1/p2c1-rerun-g5-derivation.txt` (prior records untouched).
  Labeled `[RUN-MEAS]` below; analysis tails re-read by this closure writer
  `[RUN]`.
- Condition 2 (eofmix re-attribution): static recount session, 2026-10-28,
  raw `p2-raw/P2C1/recount-20261028/` (objdump transcripts +
  `recount-records.md`). Also labeled `[RUN-MEAS]`; the writer's own
  re-executions/re-reads this session are marked `[RUN]` with the command
  named.

### 12.1 Condition 1 — G5 re-derivation from the compliant re-run

Procedural provenance (independent-review note): the re-run's adjudicative
force derives from this report's own frozen disposition — §8 ("a G5 failure
until a compliant re-run (§2.4 serial exclusivity, quiescent host, with the
B1-side control) adjudicates it") and §9 condition 1 — not from an explicit
re-test clause in plan §3.5. The plan's Amendment log correctly stays "None"
(`P2_EXPERIMENT_PLAN.md`:529-532): the re-run changed no plan parameter (same
cells, commands, pairing, statistics and verdict rules; only executed to the
§2.4 discipline), and it was a single compliant re-run with the original
EXCESS record preserved as archived — a disclosed preregistered disposition,
not a silent deviation and not repetition-until-pass. Later citations of the
re-test-adjudication procedure should cite §8/§9 of this report, not the
plan.

Session discipline `[RUN-MEAS]`, record re-read `[RUN]`
(`p2c1-rerun-quiesce.txt`): quiescent host (load 0.03/0.06/0.08 at session
open, background-process survey clean, zero concurrent
measurement/build/ladder processes), strictly serial cells under §2.4 serial
exclusivity, `taskset -c 2` via `paired.sh`, arms/commands byte-identical to
the §8 cells, read fixture pinned-sha verified at session open and close. No
sudo/perf/PMU was used — G5 cells need only uninstrumented wall; sudo
credentials were neither used nor written anywhere.

A/A floors first (§2.4 addition 2) `[RUN-MEAS]`, tails re-read `[RUN]`:

| Cell | sd_AA | MDE′ = 2.131·sd_AA/√16 | Disposition |
|---|---|---|---|
| `p2c1-aa-read-4k-base-rerun` attempt1 | 12.871 ns | (voided) | > 10 ns #600 band → voided; §11.10 one 45 s quiesce rerun |
| `p2c1-aa-read-4k-base-rerun` attempt2 | 10.807 ns | 5.758 ns | accepted; still marginally > 10 ns band → persistent session noise **disclosed** (same caliber as the original session's accepted 14.602 and P1's 10.39); not absorbed by repetitions, no amendment filed |
| `p2c1-aa-read-4k-cand-rerun` | 7.028 ns | 3.744 ns | within band, accepted first attempt |

Gate cells (candidate binary — the G5 cells) `[RUN-MEAS]`, tails re-read
`[RUN]`; rule: EXCESS iff the 95% mean-CI excludes 0 AND |median| ≥
max(MDE, 1%·N1 median) (P1 §5.5; plan §3.2 negative-control rule):

| Cell (raw) | N1 median | Δ(A1−A2) median | mean | sd | 95% CI | pos | threshold | Verdict |
|---|---|---|---|---|---|---|---|---|
| `p2c1-negread-4k-rerun` | 707.980 | +1.259 | −0.070 | 13.395 | [−7.206, +7.066] incl 0 | 9/16 | max(5.535, 7.080) | **PARITY** |
| `p2c1-negread-1b-rerun` | 553.395 | −0.263 | −1.578 | 7.886 | [−5.779, +2.623] incl 0 | 7/16 | 5.535 | **PARITY** |

MDE′ dual-report (§2.4 addition 1): no verdict flips under either fresh floor
(3.744 / 5.758) — both CIs include 0, so no floor can make either cell
EXCESS.

**G5 re-derivation: both `p2c1-negread-*` cells PARITY, no cell EXCESS →
G5 = PASS** under the preregistered rule. The original §8 `negread-4k` EXCESS
(median −9.685, CI [−14.424, −1.250] excluding 0) did not reproduce under the
compliant quiet serial re-run (+1.259, CI including 0) — consistent with §8's
recorded "probable false positive" attribution. The original cell record
stands as archived; the re-run is its preregistered disposition (§9 condition
1), not a re-adjudication of the frozen number.

### 12.2 B1-side same-unit controls (attribution evidence, not gate cells)

`[RUN-MEAS]`, tails re-read `[RUN]`:

| Cell (raw) | N1 median | Δ median | mean | sd | 95% CI | Classification |
|---|---|---|---|---|---|---|
| `p2c1-negread-4k-b1side-rerun` | 711.452 | −7.565 | −7.287 | 14.141 | [−14.820, +0.247] incl 0 | between-band: not EXCESS (CI incl 0) but \|median\| ≥ 7.115 — disclosed |
| `p2c1-negread-1b-b1side-rerun` | 552.611 | −1.502 | −0.297 | 8.296 | [−4.717, +4.123] incl 0 | PARITY (sub-MDE) |

Scope disclosure: `p2c1-negread-1b-b1side-rerun` is a **new control cell** —
the original session archived only the 4 KiB B1-side twin; this re-run paired
each gate cell with a same-unit B1-side control (command = the original
`negread-1b` form with the B1 binary, mirroring the archived 4k-b1side
pattern).

Adjudication reading (recorded with the cells; G5 itself is decided on the
candidate-binary cells of §12.1 only): on this host the a1↔a2 4 KiB paired
difference keeps its sign on the B1 binary (original b1side −10.538,
CI-excluding; re-run −7.565, CI-including — attenuated below the CI
threshold) while the candidate-binary cell this session sits at +1.259. The
original session's +9…+10 ns band was therefore a host/session-state artifact
of the a1↔a2 comparison, not a candidate regression — a candidate-caused
regression of `read_at` remains excluded at machine level (§2.2:
instruction-stream identical across the two archives).

### 12.3 Retest-session disclosures

1. A/A base session noise (§12.1): persistent, dual-reported, never absorbed
   by repetitions.
2. A/A cand within-pair position effect ≈4–5 ns: pair-delta median −4.178,
   mean-CI [−9.038, −1.550] **excludes 0**; AB/BA subgroup medians symmetric
   (−4.233/−4.178) — the second invocation of each pair runs systematically
   faster. This is exactly the class the ABBA alternation cancels in the A/B
   cells (per-cell ABmed/BAmed recorded in every raw tail above). Disclosed;
   not a gate input.
3. B1-side 4k between-band residual −7.565 (§12.2).
4. Integrity-record blemish: the closing integrity scan's first pass
   self-matched its own header text mid-write; the rescan recorded in the same
   file is the authoritative clean scan (`p2c1-rerun-integrity-close.txt`
   header). Record blemish, not a measurement anomaly. Gates otherwise all
   green, re-read `[RUN]`: fixture sha = pinned `bc020fd1…993a` MATCH;
   ladder/ladder-c1 sha256 equal the recorded values (`89450367…010e8` /
   `8305d22c…0381`); no VERIFY_FAIL anywhere; 7/7 cell files `exit=0`;
   in-group checksum equality 32/32 per accepted cell (4k 25165824, 1b
   24576); 32 arm lines = 16 ABBA pairs per cell.

### 12.4 Condition 2 — quantitative re-attribution of the +10.228 ns eofmix residual; cap status

Artifacts `[RUN-MEAS]` (recount session) with writer re-verification `[RUN]`
this session: `sha256sum` on the frozen B1 archive →
`d1e351a5…3007f0` and on the candidate release archive → `07d06803…2049e`
(both equal the report header and the recount record); full objdump
transcripts archived. The M2 dead-store gate re-verified by the writer on the
archived objdumps `[RUN]`: `movaps …0x20(%rsp)` / `…0x10(%rsp)` = 2/2 in B1,
**0/0 in the candidate**.

- **Anchors** `[RUN-MEAS]`: all eight audit-convention anchors reproduced
  (B1 `read_exact_at` 1-step 100 / arc 44; `read_exact` 82/34; candidate
  68/25, 55/21). §2.1's review-passed counts are now independently
  re-executed — this closes the §2.3 NOT_RUN disclosure and, for the static
  counts, the §2.6 P5 input. Bound verdicts unchanged (68≤75, 25≤26, 55≤60,
  21≤21; public-op ±0 per §2.2).
- **New eofmix shape counts** `[RUN-MEAS]` (tail rotation per the kit's
  `Pattern::tail` i%3 {full, half, at-EOF} switch, `ladder.cpp:784-795`,
  re-read by the writer; timed window 1.3333 pread/op ⇒ 1/3 extra step per
  op, consistent with §2.5 strace 2.6667 timed+verify): `read_exact_at`
  at-EOF B1 95 / candidate 69; half 139 / 94; per-extra-step memory ops
  B1 13 (4L/9S; the state block alone owns 7) → candidate 4 (3L/1S) =
  −9 memory ops/step.
- **AX−A1x static excess** `[RUN-MEAS]` (A1x inline arm 54/23/56/79 as the
  within-binary control; the recount re-verified the A1x blocks identical
  across both kit binaries and the linked functions identical to the archived
  `.o`): seq E_B1 = +64 / E_cand = +32 instr/op; eofmix rotation means
  E_B1_eof = (64+78+57)/3 = +66.33 / E_cand_eof = (32+33+31)/3 = +32.0
  instr/op. Decomposition of that excess by shape class, corrected on
  independent review (writer recomputed `[RUN]` from the recount's own
  per-shape table): the arc-driven excess is (arc_AX − arc_A1x) = (25−23) = 2
  instr per half-op, and only the half shape (1 of 3 rotation positions)
  carries an arc → rotation-mean arc-class excess ≈ 0.67 instr/op (≈2% of
  32.0); the one-step/exit machinery carries ≈ 31.33 instr/op (≈98%). This
  corrects the recount record's "arc 26% (8.33/32.0) / one-step+exit 74%"
  split, which differenced the candidate AX arm's absolute arc cost (25/3)
  against zero instead of against the A1x arm's own arc (23/3 ≈ 7.67
  instr/op ≈ 1.8–1.9 ns misallocated between the buckets);
  `recount-records.md` stands as archived. The corrected caliber strengthens
  the directional conclusion: the residual is almost entirely *not* a
  per-extra-step quantity.
- **λ calibration and prediction** (writer recomputed `[RUN]`): on the two
  CI-excluding B1 baseline cells, S = +15.439 ↔ +64 ⇒ λ₁ = 0.241 and
  S_eof = +16.577 ↔ +66.33 ⇒ λ₂ = 0.250 ns/instr — 4% apart, i.e. the
  static×single-λ model is self-consistent on the B1 side. Predicted
  candidate eofmix residual: 32.0 × λ = **+7.7…+8.0 ns** (arc-class ≈
  0.16–0.17; one-step/exit ≈ 7.5–7.8). Measured D_cand_eof = **+10.228**
  median (mean +8.567, EXCESS-sign CI [+2.115, +15.019]): the prediction
  lies inside the measured CI; the unattributed gap is **2.2–2.5 ns
  (21.8–24.7%)** at the median caliber and 0.6–0.9 ns (6.6–10.1%) at the
  mean caliber. Attributed share: ≈75–78% (median) / 90–93% (mean).

**Cap status: maintained (not lifted).** Reasons, per the recount record
`[RUN-MEAS]` re-read `[RUN]`:

1. The lift criterion — quantitatively account for the +10.228 ns point
   value — is unmet: the 2.2–2.5 ns median gap sits within session noise
   (sd_AA 14.6–15.6 ns, MDE′ 7.8–8.3 ns) but cannot be driven to zero by the
   static model.
2. Cross-binary λ′ anchors are mutually inconsistent (Δ_own seq −16.565 ↔
   −32 ⇒ 0.518 vs Δ_own_eof −9.813 ↔ −34.33 ⇒ 0.286; 1.8× apart) — the
   static→wall mapping itself carries pattern/binary-level systematic
   differences of order ±30–80%.
3. Decisive internal contradiction: the candidate seq and tail cells carry
   the **same** +32.0 instr/op static excess while their walls are −3.563
   (CI including 0, PARITY) and +10.228 (CI excluding 0, EXCESS) — ~13.8 ns
   apart. The EOF-boundary pattern (short return + 0-return, 2/3 of ops
   through the eof exit, one loop back-edge per 3 ops) dynamically amplifies
   the AX−A1x difference beyond static counts; a static×steps model cannot
   produce this structure.
4. Plan §3.5 G4 is a measured-wall criterion (D_cand_eof ≥ max(5.535, 7.010)
   is EXCESS); a static recount cannot overturn it.

Condition-2 obligation: **executed and archived** (this section +
`recount-records.md`). Result: partial attribution — +32.0 instr/op
candidate-side rotation-mean static excess (arc-class ≈ 0.67 instr/op ≈ 2% /
one-step+exit ≈ 31.33 ≈ 98%) × pattern-dependent λ ∈ [≈0, 0.320] +
unattributed EOF-boundary dynamic amplification ≈ 2.2–2.5 ns (median
caliber). The claim label remains
"measurable improvement with re-attributed residual" — "re-attributed" is now
quantitative. The tail-mix wall-parity subclaim stays restricted until the
residual is eliminated (shrink the ≈98% one-step/exit share of E_cand_eof) or
a §2.4-compliant re-measurement re-adjudicates the cell.

### 12.5 Verdict re-derivation (plan §3.5, verbatim application)

| Gate | §3.5 rule | Status after this addendum | Verdict |
|---|---|---|---|
| G1 machine (gating) | §3.3 objdump gates + recount bounds + public-op shapes + N1 bridge | frozen §9 PASS; now independently re-executed by the recount (8/8 anchors; M2 gate writer-re-verified `[RUN]` §12.4) | **PASS** |
| G2 correctness (gating) | mutants.sh exit 0 at seams/mechanism commits; matrix.sh exit 0 | frozen §9 PASS (§4) | **PASS** |
| G3 improvement | Δ_own ≤ −5.535 AND CI excludes 0 | frozen §9 PASS: −16.565, CI [−20.806, −7.270] excl 0, robust under MDE′ (§6.2) | **PASS** |
| G4 parity endpoint | D_cand and D_cand_eof no longer EXCESS | composition −3.563 PARITY_REACHED; eofmix +10.228 EXCESS, now 75–78%/90–93% attributed (§12.4) | **PARTIAL — recorded; caps the claim** |
| G5 negative controls | negread-* PARITY; no cell EXCESS | re-derived from the compliant re-run (§12.1): both cells PARITY, no cell EXCESS | **PASS** |

§3.5 as preregistered: "KEEP-eligible iff G1+G2+G3+G5 (G4 recorded; its
absence caps the claim at 'measurable improvement with re-attributed
residual')." The conjunction is met, exactly along the path §9 condition 1
preregistered ("If PARITY is restored, the KEEP conjunction is then
G1+G2+G3+G5 with all gates green"); condition 2's obligation is executed with
its result recorded (partial attribution; cap maintained, §12.4). REJECT
triggers (I < 3 ns; static bounds failing; G2 red): none apply.

**Decision: KEEP — superseding §9's REVISE** (the header bullet above keeps
the frozen REVISE record of the original session). The claim is capped at
"measurable improvement with re-attributed residual" (re-attribution now
quantitative, §12.4); the tail-mix wall-parity subclaim remains restricted.
The frozen §1–§11 record, including the original §8 EXCESS cell, stands as
archived; this addendum is its preregistered disposition, not a
re-adjudication.

### 12.6 Combined-arm precondition (MISSION P2.2) — status after closure

§11 recorded "not executed; precondition not met — P2C1's G5 gate formally
unresolved". That P2C1-side blocker is now removed: with KEEP restored here,
both single factors hold (P2C1 KEEP, scoped claim capped per §12.4; P2C2
KEEP — instruction caliber, `P2_CANDIDATE_P2C2.md` §9), so MISSION P2.2's
combined-measurement precondition (MISSION.md:404 — run the combined
measurement only if the single factors all hold; independent gains must
never be added) is **satisfied**. Execution is assigned to the P5
interaction matrix; **no combined cell was measured in this closure**
(unchanged from §11).

P5 review inputs carried forward from this closure: (i) the ≈2.2–2.5 ns
unattributed EOF-boundary dynamic amplification and the
same-static/different-wall seq-vs-tail contradiction (§12.4) — the subject of
the maintained cap; (ii) the B1-side between-band residual −7.565 (§12.2);
(iii) the A/A within-pair position effect (§12.3). The §2.6
static-recount P5 input is closed by §12.4.
