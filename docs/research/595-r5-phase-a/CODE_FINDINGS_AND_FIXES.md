# #595 R5 Phase A — Code Findings and Fixes (C1–C7)

Deliverable: per-finding verdicts with source evidence, the implemented
mechanical fixes and their equivalence evidence, and the explicit
fixed-vs-clarified-vs-candidate classification. Written 2026-11-07 by the
Phase A synthesis writer from the two verifier reports
(`r5/analysis/findings-c1-c4.md`, `r5/analysis/findings-c5-c7.md`) and the
implementer's evidence file (`r5/evidence/equivalence-c1-c2.md`); the
verifiers' key citations were independently spot re-verified in the frozen
tree during synthesis.

Baseline: R4_ORIGINAL `f3b25329bc8a64a201961c426a58adacfb8fdf08`
(`/home/jnhu/sluice-595-e5/r5/worktree`, porcelain clean). Fixes live on
branch `research-595-r5` as `c55165a219a67058c97941c60ae520d1b1b45996` (C1)
and `b191bb893432c4df0551d24c85843d8513ed5a55` (C2). Patches:
`r5/evidence/cleanup-c1.patch` (sha256
`4ecbe2b6526e7e533c67c4eee08ee39f974300377533436e89b1222618373865`),
`r5/evidence/cleanup-c2.patch` (sha256
`39e9cf4e75e6d78122b6b502b95cfa6df4d11890351e2f40e3506dbacfb10bf6`).

## SUMMARY (fixed / clarified / candidate)

| Class | Items | Meaning |
|---|---|---|
| **FIXED (implemented, equivalence proven)** | C1, C2 | Mechanical cleanup commits on `research-595-r5`; behavior identical by deterministic transport-oracle comparison (18/18 scenarios, byte-identical outputs, three stages) plus in-repo suites; no predicate, quantum, refill, or completion-order change. |
| **CLARIFIED (documentation/caliber only, no code change)** | C3, C4, C7.1–C7.6 | Lock-acquisition accounting and the SHADOW−R caliber caveat; `outstanding()` semantics and rename guidance; six R4 record/protocol corrections. No runtime behavior touched. |
| **COST-REDUCTION CANDIDATE (designed, NOT implemented)** | Candidate A (from C1/C3's overhead structure + the L1 diagnostic lead, see TAIL_ANALYSIS.md §5) | Lighter R/S execution chain on top of R_cleanup: deduplicate the remaining eligibility/validation passes, hoist per-group information out of the per-chunk loop, gate trace-only record fields; policy action unchanged. See NEXT_EXPERIMENT.md Candidate A. |
| **POLICY CANDIDATES (designed, NOT implemented)** | from C5/C6 | Candidate B (real service opportunity via residue-aware budget, from C5's no-true-yield finding) and Candidate C (observation/apply decoupling, from C6) — see NEXT_EXPERIMENT.md. Changing any predicate/quantum is out of Phase A scope. |

---

## C1 — `batch_shaped_prefix(rest)` rescans the remaining span on every re-entry

**Verdict: CONFIRMED. Class: FIXED (`c55165a2`).**

Source evidence (frozen tree, re-verified this session):

- `bench/r3_ready_batch.hpp:189-196` — the scan walks the span front while
  members are 4 KiB with non-null dst; with an all-eligible span it inspects
  the entire remaining span.
- `bench/r3_ready_batch.hpp:442-448` — the while loop computed
  `record.eligible_prefix = batch_shaped_prefix(rest)` at `:448` **before**
  the `if (policy.budgeted)` branch at `:455`, i.e. unconditionally for every
  arm including R and SHADOW, on every re-entry with
  `rest = ready.subspan(begin)` (`:443`).
- Re-entry sites: `chunk_continue` (`:564-567`), `cap_reuse` (`:561-563`).

Source-level inspection counts (loop structure cross-checked against archived
R4 counters — these are inspection counts, not measured instruction counts):
N=16/q8 → 16+8 = 24; N=16/q2 → 16+14+…+2 = 72; N=64/q8 → 288; N=64/q2 →
1056. Dynamic corroboration: `r4/logs/r4_diag_lines_sample.log` shows
`ready/eligible = 16,14,12,10,8,6,4,2` per S group;
`r4/logs/w03_smoke_Q2.log` presentations=32 for 64 ops; `w03_smoke_Q8.log`
presentations=9; `w03_smoke_R.log` presentations=11 with budget counters 0
(R paid the scan with zero benefit); SHADOW `r4/raw/diagnostics/logs/
b00-p0-SHADOW-S-a1.log` 2,501 presentations / 1,250 groups.

Fix (`r5/evidence/cleanup-c1.patch`, one file `bench/r3_ready_batch.hpp`):

- `budget::Input::eligible_prefix` → `eligible_prefix_bounded`, fed by new
  `batch_shaped_prefix_bounded` inspecting at most
  `budget::kEligiblePrefixScanBound = UringAsyncBackend::ready_batch_cap` (8)
  leading members — equals `min(full_prefix, 8)`.
- `DecisionRecord.eligible_prefix` keeps its full-prefix meaning and is
  filled only when a trace sink is attached (diag lines stay byte-identical).
- Non-budgeted arms (R) skip the scan entirely without a trace sink.

Equivalence argument (PROVEN, from the closed consumer set — grep over the
tree found exactly three consumers: row `front_ineligible` `r3:227` tests
`== 0`; `present_len = min(quantum, eligible_prefix)` `r3:497`; trace record
`r3:346,544` diagnostics-only):

1. `front_ineligible`: `min(ep_full, B) == 0 ⟺ ep_full == 0` since B ≥ 1.
2. `present_len`: for every emittable quantum q ≤ B (base 8 `r3:265`, clamp2
   takes min(q,2) `:269`, pinned 2/8 `:172-175,278`, immediate 1 `:284`),
   `min(q, ep_full) = min(q, min(ep_full, B))`. Identical presented span ⇒
   identical backend input ⇒ identical accepted identities/order, stops,
   residue, notifications, slots.
3. SHADOW: `present_len` never derived from `eligible_prefix` (`:482-485`).
4. R: `eligible_prefix` feeds no control flow (`:521-525`).
5. Trace path unchanged (full scan retained when `trace_ != nullptr`).

Constraints honored: bounded scan reads a strict subset of the members the
full scan read (`op.file` never touched); no eligibility caching across
re-entries or `NativeFileRef`/borrow lifetimes; naming split avoids silent
trace-field redefinition; no simultaneous policy change. Recorded caveat:
the bound must stay ≥ any future pinned quantum (constant comment in the
patch).

Actual tests run (native E5, `r5/evidence/equivalence-c1-c2.md`):
deterministic ready-reuse transport oracle (R4 archived 18-scenario harness,
compiled in `/tmp/sluice-r5-eq/` with exact `xmake/tests.lua` flags) — full
run + per-scenario `--only` at baseline / C1 / C2: **18/18 rc=0 each, full
outputs byte-identical to baseline**. In-repo suites: `r4_arm_smoke` 57/57
PASS byte-identical; `uring_ready_batch_deterministic_test` (26 scenarios),
`_research_test`, `_fault_test`, `uring_progress_race_test`,
`uring_external_loop_test`, `submit_concurrency_uring_test`,
`shutdown_lifecycle_uring_test` — rc=0, byte-identical. The only two output
differences anywhere (a poll-count diagnostic line in
`shutdown_lifecycle_test`, one INFO line in
`uring_request_core_integration_test`) were reproduced by re-running the
**same** binary — proven unrelated to the change. W03 `r3d16cap` real-kernel
per-arm runs rc=0 for all 8 arms at all 3 stages; their presentation counters
were shown scheduling-nondeterministic by an A/B repeat study (5 interleaved
reps per binary per arm: S 20→28 presentations, R 9→10 across reps of the
*same* binary), so they are informational, not an oracle — per the round's
exemption for real-kernel scheduling.

No candidate/counterexample patch needed: equivalence holds, so
`c1-candidate.patch` was correctly not written.

---

## C2 — the predicate `completed>0 || immediate_work_remains` does not mean "completion pressure now"

**Verdict: CONFIRMED. Class: FIXED as naming/comments/assertions only
(`b191bb89`); the predicate itself is untouched (changing it is a policy
experiment).**

Source evidence:

- `pass.completed = poll()` counts results **published during that pass**
  (`src/async/uring_backend.cpp:314`, publications `:861-880`) — a historical
  quantity at any later decision point.
- The harness stores the **final** pass of the drain loop
  (`bench/bench_w03_external_loop.cpp:231-245`), publishes under
  `shared.mutex` (`:254-257`); on normal exit `last_pass_immediate_work` is
  false by construction (loop-break condition) — re-verified this session at
  `bench_w03:242-244`.
- Predicate: `bench/r3_ready_batch.hpp:474-476`. The counterexample state
  (`completed>0 ∧ no pending work`) is the common steady state: archived
  `r4_diag_lines_sample.log` shows `pass_completed=16 pass_immediate=0
  active=0` at decisions where the submitter had already collected all 16
  results; R4 aggregate 99.92% truth (`r4/RESULTS.md:199`).
- `pressure_known` is a sticky "a pass ever happened" witness
  (`bench_w03:158,257,421`), and the snapshot is frozen per refill group
  (`bench_w03:415-422` → reused at `r3:449-452`).
- The 99.92% truth rate is a structural artifact of the completion-driven
  harness (the submitter only refills after a pass published completions).

Fix (`r5/evidence/cleanup-c2.patch`, 3 files: `bench/r3_ready_batch.hpp`,
`bench/bench_w03_external_loop.cpp`, `bench/r4_arm_smoke.cpp`): pure renames
and comment corrections — `pressure_known`→`owner_pass_sampled`,
`read_pressure`→`read_owner_pass_activity`,
`reads_pressure_facts`→`reads_owner_pass_facts`,
`pressure_hint_active/pressure`→`owner_pass_activity_hint_active/
owner_pass_had_activity`, row `completion_pressure`→`owner_pass_activity`,
DecisionRecord trace fields and diag-line labels follow; comments now state
the lag, the published-work meaning, and the never-cleared flag;
`r4_arm_smoke` assertions that read the signal as a completion backlog are
re-worded while testing the same values. **Deliberately unchanged:** arm
tokens SNI/SNP, the `r4_*` record/counter schema including
`r4_pressure_decisions` (frozen R4 evidence join key), and every predicate
evaluation.

Equivalence evidence: same oracle protocol as C1; the archived 18-scenario
harness references the renamed identifiers, so the C2-stage harness copy
(`harness_c2names.cpp`, full mechanical diff archived at
`r5/raw-diag/equiv/c2/harness_c2name_adaptation.diff`, 9 hunks, renames
only) was used — **18/18 rc=0, full output byte-identical to baseline**;
`r4_arm_smoke` 57/57 PASS with only the 8 intentionally corrected labels
differing; all other suites rc=0, byte-identical.

---

## C3 — reusing the same lock objects still adds lock acquisitions

**Verdict: CONFIRMED. Class: CLARIFIED (documentation/caliber correction; any
lock-path change is a perf/policy experiment, out of Phase A).**

Verified counts (submitter path, d16 → 16-op group):

| Scope | Lock | Count | Site |
|---|---|---|---|
| per refill group | harness `SharedLoop::mutex` | +1 (S/SNI/SNP/SHADOW only) | facts snapshot `bench_w03:417-422`, gated by `reads_owner_pass_facts()` `r3:375-378` |
| per decision (budgeted, inflight-reading arms) | `access_mtx_` + core `mutex_` | 1 + 1 | `ctx_.outstanding()` `r3:469` → `async_io_context.cpp:837-840` → `request_core.cpp:93-96` |
| per presentation | `access_mtx_` ×1 (whole call) + `dispatch_mtx_` ×1 (whole admit+dispatch+enter) + core `mutex_` ×1 when stats on | 1/1/1 | `test_access.hpp:69`; `uring_backend.cpp:504-590`; `uring_backend.cpp:459-465,610` |
| per accepted op | core `mutex_` ×3 | 3 | reserve (`uring_backend.cpp:666`), accept (`:726`), `claim_execution` (`uring_transport.cpp:156-157`) |
| driver side, per request (arm-independent) | core `mutex_` ×~5, several under `dispatch_mtx_` | — | offer/release/begin/complete + observer claim/retire (`uring_backend.cpp:801-830`) |

Lock order `access_mtx_ → dispatch_mtx_ → core mutex_` documented at
`test_access.hpp:47`.

**Frequency asymmetry (do not misapply SHADOW−R):** counting per **full
16-op restart group** (`max_refill=0` ⇒ effective bound = depth = 16,
`bench_w03:384-385`), S makes ≈8 decisions per such group (≈8
`outstanding()` double-lock reads), SHADOW ≈2 (≈R's presentation frequency,
¼ of S), R 0; on the steady-state mean group (≈7.9 ops, mean ready 7.92,
`r4/RESULTS.md:204-206`) the counts are ≈4 and ≈1 respectively. The
S:SHADOW ≈4:1 ratio is group-size-invariant (it is a per-op ratio: one read
per 2 ops vs one per 8 ops, T:47 counters). The R4 SHADOW−R instrumentation
figure (CPU +1.02%, upper +1.94%; p99 +1.22%; `r4/RESULTS.md:182`)
therefore bounds "read + evaluate + record" at SHADOW's read frequency only
and **cannot** substitute for S's ~4× higher read frequency. Also: for
budgeted arms the timed path (compile gate off, no trace sink) still
constructs the ~30-field DecisionRecord and executes ~10 `r4_*` counter
increments per decision (`r3:446-517`; only the trace push at `:537` is
gated) — these costs are part of the measured S/SHADOW arms and must not be
deleted before any comparison against historical R4 numbers.

---

## C4 — `active_data` is the core's accepted-but-unpublished count, not "kernel inflight"

**Verdict: CONFIRMED (name/semantics mismatch; the sharper hypothesis
"accepted-but-uncollected" is refuted in the narrower direction — the count
excludes published-but-uncollected results). Class: CLARIFIED (naming +
documentation, research surface only; no production rename, no new global
atomic).**

Lifecycle (source): `++outstanding_` at **accept** (`request_core.cpp:202`),
`--outstanding_` at **complete_publication** (`:473`); no decrement at
terminal-chosen, at CQE, or at collection (`:323-344` does not touch it).
Zero therefore implies "no accepted-unpublished work" (a drain detector — in
W03 without cancels, a fully drained pipeline at the observation instant),
not "kernel idle"; nonzero does not imply kernel inflight. Zero does not
imply the SQ ring is empty or that no published-uncollected results exist.
This is the structural seed of the R4 SNP anomaly (zero-inflight moments =
drain/restart moments; R4 SNP: 35% small batches, p99 +34.48%,
`r4-archive/TAIL-LATENCY.md:50-65`).

Rename guidance recorded (research fields/reports only):
`active_data`→`unpublished_accepted`, row `inflight_zero`→
`no_unpublished_accepted`, flags `read_inflight`/`inflight_hint_active`→
`read_unpublished`/`unpublished_hint_active`. Not applied this round (the C2
commit deliberately kept the `r4_*` schema; a further rename would break the
R4 join keys for no behavioral gain).

---

## C5 — "a small batch never truly yields"

**Verdict: CONFIRMED. Class: CLARIFIED (structural finding); the fix is a
Phase-B policy candidate (Candidate B), not implemented.**

Verified: after a quantum-bounded presentation the adapter does not enter
progress or observer code and does not return to the caller's yield point —
`chunk_continue` (`r3:564-567`) and `cap_reuse` (`:561-563`) re-enter the
same while loop, same call stack, no `poll_progress`, no observer check, no
cv wait, no application code between chunks. Each presentation ends in its
own kernel enter (`uring_backend.cpp:577-582` → `uring_transport.cpp:223-240`)
— "quantum 2" means one enter per ~2 SQEs, a presentation bound, not a
yield. The submitter's only genuine wait is `shared.cv.wait_for`
(`bench_w03:522-539`), once per outer iteration. Distinguished quantities:
**batch quantum** = `present_len` per backend call; **work quantum** = the
whole `want` span presented back-to-back inside one `submit()` call (plus,
for `max_refill == 0`, consecutive refill rounds before the first wait).
Work-quantum size on W03-uring-4k-d16, by corpus: `max_refill=0` ⇒ effective
refill bound = `depth` = 16 (`bench_w03:384-385`), so a fully drained window
restarts with a 16-op group (S: 8 chunks / 8 enters; R: 2 / 2), while steady
state has `want = depth − window.size() < 16` with mean ready 7.92 per
decision (`r4/RESULTS.md:204-206`) — a mean group of ≈8 ops, i.e. ≈4 chunks /
≈4 enters per yield for S. Neither figure is 2. (CALL_PATTERNS.md §2.3
carries the same dual-corpus statement.)
The driver is a genuinely concurrent thread; the chunk loop simply never
consults it. W03 attaches no library observer; application observation is
submitter-thread polling of the window front.

Candidate directions noted by the verifier (NOT implemented): saving/borrowing
the unaccepted suffix across a yield; bounded progress responsibility for a
re-schedule; a bounded re-entry path not depending on a future request
arrival — carried into NEXT_EXPERIMENT.md Candidate B.

---

## C6 — caller observation and buffer safety

**Verdict: CONFIRMED. Class: CLARIFIED (contract documentation + hazard
counterexample); the separation experiment is Candidate C, not implemented.**

Verified: W03 drains strictly from the deque head (`bench_w03:541-572`; the
loop stops at the first not-ready front); `slot = sequence % depth`
(`:404-413`,`:463-466`,`:484`) is safe under the current discipline by three
code facts — window capacity ≤ depth (`:396`), window contents are
consecutive sequences (push_back/pop_front), an entry leaves only after
ready+take (`:541-571`). Hazard (documented counterexample, depth=4):
out-of-order pops admit `sequence+depth` reusing the buffer of an in-flight
op — data corruption; the round's prohibition on deque-scan +
old-slot-allocation is necessary, not stylistic. The public API already
permits independent, non-consuming observation (`Request::ready()`
`request.hpp:130-132`, `try_result()` `:134-144`); the FIFO restriction is a
harness discipline. Any Phase-B separation must (i) keep the application's
in-order completion endpoint (settle/verify/take terminal for every op) and
(ii) count reorder bookkeeping and any extra buffers as resource cost.

---

## C7 — research instrumentation and verdict errors (six sub-items)

**Verdict: CONFIRMED (all six). Class: CLARIFIED — corrections written to
`r5/analysis/corrections/`; the R4 archive is NOT modified (no retro-flip of
any frozen verdict). No new tests (pure wording/protocol changes).**

| Sub | Finding | Status | Correction artifact |
|---|---|---|---|
| C7.1 | R−B/B−R direction labels: **arm B** here is the R4 "B supplement" baseline arm R3_FIXED_PREFIX (one full-span presentation, no cap re-entry, remainder via per-op suffix singles; `arm_policy` `bench/r3_ready_batch.hpp:176-181` returns `{reuse_on_cap=false, budgeted=false}` for `Arm::b` — glossary in CALL_PATTERNS.md §1.0; not to be confused with NEXT_EXPERIMENT.md's Candidate B or its budget variable `B_ops`). Frozen scripts compute `(B−R)/R` for cost and `(R−B)/R` for tput (`r4/harness/analyze_pairs.py:5-11`, `r4/scripts/bsupp_descriptive.py:36-43`), but `r4/RESULTS.md:188,394` label the row "R−B" — no number is wrong, only labels/keys self-contradict | CONFIRMED | `corrections/C7-1-sign-convention.md` (unified per-metric-formula wording; rule adopted for R5 tables) |
| C7.2 | figure-data.csv structural index defects: `cell` not a cell id (F3 uses the case string for both MAIN and B-SUPP rows; 96 F2/F5 rows empty; TRAJECTORY source unnamed), `arm` holds contrast labels in 69 rows, `segment` overloaded, `n` mixes blocks with decision-lines. All 370 values re-verified consistent with MAIN_SUMMARY (zero mismatches, command run this session by the verifier) | CONFIRMED (residual, index/metadata only) | `corrections/C7-2-figure-data-columns.md` + `figure_data_normalize.py` + `figure-data.normalized.csv` (adds source_cell/contrast/dataset/n_unit; original values byte-identical; run output rows=370 cell_corrections=42 arm_column_disambiguations=69) |
| C7.3 | T1 `shadow_outstanding_sum==0` is a legal observation on d2serial: the frozen oracle (`r4/PREREGISTERED.md:100-105`) conflates instrument liveness with "state non-zero at least once"; on max_refill=1 (`bench_w03:856`) every decision point has the previous op published-and-consumed, so the live instrument correctly reports zero (block↔attempt mapping: 24 TRAJECTORY blocks × 3 attempts = 72 attempt records in `r4/raw/matrix/TRAJECTORY/attempts.jsonl`, all invalidated; TAIL_ANALYSIS.md §1.1's "0/24 valid" counts **blocks**. The runs themselves exited 0 with 20,000 decisions/rep) | CONFIRMED (oracle defect diagnosed structurally) | `corrections/C7-3-shadow-oracle-protocol.md` — shape-aware oracle for R5+ (liveness via `r4_shadow_decisions == submitted_ops`; `STRUCTURAL_ZERO` as a legal class). R4's INCOMPLETE verdict stands as a faithful application of the frozen criterion |
| C7.4 | "39 protected members failed" ≠ 39 independent significant regressions: all 39 are bound violations (point/upper beyond limit) with `holm_reject=false`, min p = 0.243; correlated (cell, arm, metric) tuples over 7 cells sharing 24 matched blocks (D2SERIAL alone contributes 12); the 4 NOT_ESTABLISHED are in-bounds-but-uncertifiable (evidence insufficiency, safe side) | CONFIRMED (presentation-layer risk; `r4/RESULTS.md:80,95` already definitionally correct) | `corrections/C7-4-fprot-wording.md` |
| C7.5 | W02-d3 missing case = harness gap, not hardware: pre-disclosed in `r4/PREREGISTERED.md:177-180`, recorded `r4/RESULTS.md:335`; W02 uring depths are 1/4/16/64 (+opt-in d2 `bench_w02:930-935`), no depth-3 anywhere; W03 has `r3d3` (`bench_w03:857`) — a different workload | CONFIRMED | `corrections/C7-5-w02-d3-record.md` — recommendation: do not backfill on its own (neighboring d1/d2/d16 already fail 6/6/8 of 12 members) |
| C7.6 | Resource-evidence wording: R4 archive conforms (RSS/threads PASS are measured with honest NOT_RUN tail: static audit/strace/valgrind NOT_RUN ⇒ overall NOT_ESTABLISHED `r4/RESULTS.md:159-166`); no zero-cost claim found by grep | CONFIRMED (rules codified) | `corrections/C7-6-resource-wording.md` — R1 missing evidence ≠ correctness error; R2 untested ≠ zero cost; R3 measured PASS must name sub-gate scope; R4 measured 0.00% RSS is legitimate but must not be shortened to "no resource cost" |

---

## Cross-check note (synthesis-level)

Overlapping claims were checked across the two tracers and two verifiers; no
substantive contradiction was found. Specifically: the control-point reading
(submit tracer §0) and C5's no-yield finding agree (two independent lines ⇒
verified); the `outstanding()` semantics (C4) agrees with the
completion-side tracer §4; the lock inventory (C3) agrees with both tracers'
lock tables; the d2serial structural zero (C7.3) agrees with the
completion-side tracer's case-matrix note. The two tracers used different
diagram arrow conventions (declared by the completion-side tracer §0 as
needing merge-time harmonization) — harmonized in CALL_PATTERNS.md §0; this
is a notation difference, not a factual conflict. One precision note carried
from the environment verifier: the input digest's "only p99 exceeds" for the
R4 main gate describes §7.1's statistical conditions; §10.5 protocol
preconditions (r3d2pos positive control, enter/CQE reconciliation) were
independently NOT_RUN and also bar any PASS declaration (`r4/RESULTS.md:354-355`).
