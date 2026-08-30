# TAX-0 EXP-U0 — Uring Router/Capacity Causal Attribution

Experiment: `TAX-0-EXP-U0` · Campaign: #250 TAX-0 · Freeze: `1c82799` (PR #254)
Base master: `e19b13b` (PR #253 merge; contains the frozen EXP-0 evidence)
Date: 2026-08-31

## 1 Verdict

**EXP-U0 PASS — ROUTER CAUSALITY STRONGLY SUPPORTED.**

At the exact EXP-0 geometry (READ, 4 KiB, D=8, Q=8, C ∈ {8, 32, 128, 512},
256 MiB, real liburing 2.14), changing ONLY the physical scan direction of
`find_live_router_cookie_` — forward production scan vs reverse research
ablation, identical matching predicate, identical router, identical cookies,
identical same-work accounting — removes the measured capacity tax:

| filesystem | b_instr forward | b_instr reverse | reduction |
| --- | --- | --- | --- |
| tmpfs (primary) | **+5.9922 instr/op/C** (R²=1.00000) | **−0.0039** | **99.9%** |
| btrfs warm (control) | **+6.0007 instr/op/C** (R²=1.00000) | **+0.0007** | **100.0%** |

The forward baseline reproduces EXP-0 almost exactly (EXP-0 measured
+5.994 tmpfs / +6.001 btrfs), inside the preregistered envelope
5.0 ≤ b ≤ 7.0, R² ≥ 0.98. All seven preregistered criteria for STRONGLY
SUPPORTED are met (§15). No material residual capacity slope remains
(|b_reverse| ≤ 0.004 ≪ the 1.5 threshold; §16).

T0-U-CAPACITY: MEASURED MATERIAL (EXP-0).
T0-U-ROUTER: CAUSALLY ATTRIBUTED (this experiment).
OPTIMIZATION: EARNED, BUT NOT IMPLEMENTED (§19).
FIX SELECTED: NO — production design selection is deferred to #255 (§19).

## 2 EXP-0 authority

- Report: `research/tax0/TAX0-B-EXP0-CAPACITY-INVARIANCE.md` (PR #253,
  frozen head `4e6bdb6`, merged `e19b13b`).
- Canonical evidence: `docs/results/performance-attribution/tax0b-exp0-*.json`
  (kind `tax0capacity`).
- Established: ThreadPool arm capacity-INVARIANT in tested geometry; Uring
  arm capacity-DEPENDENT — tmpfs b ≈ +5.994 instr/op/C (R² ≈ 1.0000),
  btrfs b ≈ +6.001; C=8→128 cycles +8.5% / +11.8%.
- Mechanism status before U0: `find_live_router_cookie_` = LEADING
  HYPOTHESIS, causality NOT proven. EXP-U0 was preregistered to prove or
  demote it before any production optimization.

## 3 Master / experiment freeze SHA

| item | value |
| --- | --- |
| EXP0_FINAL_HEAD (PR #253 head) | `4e6bdb603eab458e637355519d479bf055c13c5c` |
| EXP0_MERGE_SHA == base master | `e19b13b763126beb1176baa4c80e380b1a3abefc` |
| EXP-U0-FREEZE SHA (harness) | `1c8279956c086d9e821068a0ff2295089510f2a2` |
| Measurement git_sha (rows) | `1c8279956c08…` (clean tree) |
| binary sha256 | `b6faba05c63f1dab…` (identical file for both arms) |
| Draft PR | #254 `research(perf): attribute Uring capacity tax to router scanning` |

Hard-start gate executed before any work: PR #253 CI green on `4e6bdb6`
(Linux Clang Debug + Release both `success`), PR MERGED, master contains
the frozen head, branch cut from master.

## 4 Static router facts (bound to code before design freeze)

All bound at base `e19b13b`, re-verified on the freeze commit:

- **A.** `router_` is sized exactly `request_capacity` —
  `src/async/uring_backend.cpp:504` (`router_(config.request_capacity)`).
- **B.** Normal operation CQEs route through the cookie lookup —
  `handle_one_cqe(user_data, res)` → `find_live_router_cookie_(user_data)`
  (`src/async/uring_backend.cpp:1284` region, the operation-CQE path; one
  call per non-control CQE).
- **C.** The lookup is a FORWARD linear scan over the whole router —
  `for (std::size_t i = 0; i < router_.size(); ++i)` with the predicate
  `in_use && cookie == k`.
- **D.** Free-list placement keeps live entries at HIGH indices when
  C ≫ D: the free list is seeded ascending `[0..C-1]`
  (`uring_backend.cpp:507-509`), allocation pops `back()` (`:812-813`), so
  the first D dispatches occupy `C-1 … C-D`; retirement pushes the slot
  back onto the tail (`:1118` region), so LIFO recycling pins the live set
  inside the top D indices for the whole steady state.

Empirical binding (equivalence test, real ring): allocation k of a fresh
backend lands at router index `C-1-k`, forward lookup examines exactly
`C-k` entries, reverse exactly `k+1`. Placement assumption §3 CONFIRMED —
no redesign needed.

## 5 Causal hypothesis

**H-U0-ROUTER** (preregistered in the task before implementation): the
EXP-0 capacity slope is primarily the forward linear router scan crossing
~C dead entries to reach the D live entries parked at high indices.
Predictions if true: reversing only the scan direction (a) collapses the
scan-iteration slope, (b) collapses the instructions/op/C slope, (c)
removes the C=8→128 cycles penalty — with same-work, real io_uring,
identical D/Q/C, and identical semantic results.

Uniqueness argument (why direction cannot change semantics): operation
cookies are allocated from a no-wrap 64-bit counter and never reused, so
at most ONE live router entry matches any cookie value; traversal order
selects the same unique entry or the same miss. This argument is not
trusted alone — §7 checks it mechanically.

## 6 Test-only ablation design

- Seam (all inside `#if defined(SLUCE_ASYNC_INTERNAL_TESTING)`; compiled
  only into `sluice_async_internal_testing`):
  - `RouterScanModeForTest { forward_production, reverse_ablation }` —
    default `forward_production`; the internal-testing build with the seam
    untouched executes the production scan (plus diagnostic folding).
  - Exact per-call iteration accounting in `find_live_router_cookie_`
    (entries examined, hits/misses, matched index), folded per callsite
    family: operation CQE / tagged-control CQE / transport accounting.
  - Plain (non-atomic) `mutable` counters: every callsite is on the
    documented single-driver call domain (`uring_backend.cpp:150`). No
    allocation, no logging, no locks, no atomics on the hot path; layout
    cost exists only in internal-testing builds (AGENTS.md §15).
- `tax0u0_router_bench` links the internal-testing variant ONLY (never
  `sluice_async`); it installs the scan mode before the runtime starts
  driving the backend and snapshots per-rep witness at quiescent rep
  boundaries.
- The ablation changes ONLY the traversal order: same router vector, same
  cookie values, same uniqueness, same stale-cookie semantics, same
  matching predicate, same capacity, same Q/D, same io_uring batching, same
  scheduler, same wake policy.

Forbidden in U0 (and absent): O(1) lookup tables, cookie/user_data encoding
changes, router representation changes, slot-reuse redesigns, any public
API or config knob. Production `sluice_async` compiles none of the seam;
the production `find_live_router_cookie_` is byte-identical (guarded-diff
verified; production builds green).

## 7 Semantic equivalence gates

`tests/uring_router_scan_equivalence_test.cpp` (internal-testing, real
liburing; in the default gate — Debug run 202/202 PASS):

- **Matrix**: for C ∈ {1,2,4,8,32} × K ∈ 1..min(C,5) fresh-backend states:
  for every live cookie, `forward_lookup(R,k) == reverse_lookup(R,k)` —
  same found index, plus exact per-direction iteration counts (C−k / k+1)
  that also bind the placement fact.
- **Miss equivalence**: cookie 0, the next-unallocated cookie, a
  control-tagged encoding of a live cookie, and a distant unknown value
  all miss in both directions.
- **Retirement equivalence**: after full drain every former live cookie
  misses in both directions; stale re-injection through the real
  `handle_one_cqe` drops exactly once (one extra operation-path lookup,
  accounted as a miss) in both modes.
- **Real-CQE accounting**: K live ops ⇒ exactly K operation-CQE lookups,
  all hits, zero control/transport lookups, in both modes.
- **Cancel/control path**: kernel-blocked pipe read + running-cancel
  intent + EOF original result: identical disposition in both modes
  (verbatim 0-byte result, exactly one publication, control retired), with
  exactly one operation, one control, and one transport lookup.
- Existing suites unchanged and green: `uring_backend_test`,
  `uring_backend_c2b_identity_test` (stale cookie / cancel / duplicate /
  generation), `c2c`, `c2e`, `stats`, `submit_failure`, `write_batch`,
  `io_context`, `phase_g_closeout_uring_test` (all PASS with the seam
  present, default forward mode).
- Same-work at workload scale: every official U0 row carries
  ops/bytes/word-sum equality across BOTH arms (fail-closed in bench,
  runner, and validator); `matched_router_index` averages are identical
  across arms (507.5 at C=512) — the same entries are matched, only the
  scan work differs.

Claim scope: a bounded mechanical equivalence gate over the exercised
reachable states of this backend family plus the uniqueness theorem above
— NOT a formal proof over all interleavings or unexercised states. The
bounded gates, combined with the unique no-wrap cookie identity, strongly
support semantic equivalence for the exercised backend states.

## 8 U0-A scan witness

Diagnostic artifact: `tax0u0-witness-tmpfs.json` (kind `tax0u0witness`;
NOT performance evidence). Official matrices additionally embed the exact
witness per row. Medians (D=8, Q=8, READ, both arms):

| scan mode | C=8 | C=32 | C=128 | C=512 |
| --- | --- | --- | --- | --- |
| production_forward iterations/op | 4.50 | 28.50 | 124.50 | 508.50 |
| reverse_ablation iterations/op | 4.50 | 4.50 | 4.50 | 4.50 |

- Forward iterations/op = `C − (D − 1)/2` (equivalently `C − D/2 + 0.5`)
  exactly: the D live entries sit at the high indices C−D … C−1, one
  forward lookup per position yields arithmetic mean `C − (D − 1)/2`
  (b_iter = +1.0000 instr-slope analogue: one extra examined entry per
  unit of configured capacity), while active depth stays 8 throughout. Δ
  iterations/op / ΔC ≈ O(1) as predicted. For D=8: `C − 3.5` — matching
  the measured witness (C=8 → 4.5, C=32 → 28.5, C=128 → 124.5, C=512 →
  508.5).
- Reverse is flat at `(D + 1)/2` (equivalently `D/2 + 0.5`; for D=8:
  4.5) for every C (slope 0.0000).
- Lookup accounting per row: `operation_cookie_lookup_calls == ops`
  exactly (enforced fail-closed), hits == ops, misses = 0,
  `control_cookie_lookup_calls = 0`, `transport_cookie_lookup_calls = 0`
  — no control-CQE contamination, nothing unexplained.
- Per-iteration cost coherence: forward C=512−C=8 ⇒ 504 extra examined
  entries and ≈ +3018 instr/op ⇒ ≈ 5.99 instructions per examined router
  entry, matching b_instr_forward ≈ +5.99 per unit C.

## 9 Official measurement design

Frozen in code at `1c82799` BEFORE any official run (runner
`scripts/bench/perf-attribution.py tax0u0`):

- Geometry: READ, request_size 4096, D=8, Q=8, C ∈ {8, 32, 128, 512},
  total_bytes 256 MiB, warmup 2 full rounds, 11 measured reps, taskset
  `0,2,4,6` (same physical-core policy as EXP-0).
- Arms: `production_forward` vs `reverse_ablation` — 2 × 4 × 11 = 88
  measured process-per-rep rows per filesystem; everything else identical
  (same binary `b6faba05…`, same workload file, same offsets/validation).
- Randomization: blocked rounds over the 8 (scan_mode, C) cells, order =
  `random.Random(0x55304C55).sample(sorted(cells))` per round, generated
  before measurement, stored and validator-recomputed.
- Primary metric `instructions:u/op` (process-aggregate user-mode perf
  counters, identical fixed startup costs across cells, never subtracted);
  secondary cycles/wall/user/sys. No WRITE, no Q/D variation, no backlog,
  no workers axis.
- Preregistered baseline envelope (fixed in code): 5.0 ≤ b_forward ≤ 7.0,
  R² ≥ 0.98, primary tmpfs arm — never widened after observation.
- Preregistered decision thresholds: strong/partial/not-supported
  reductions 80%/50%/20%, |b_instr_reverse| ≤ 1.5.

## 10 Baseline reproduction

| gate | tmpfs | btrfs |
| --- | --- | --- |
| b_forward (instr/op/C) | +5.9922 | +6.0007 |
| R² | 1.00000 | 1.00000 |
| envelope 5.0–7.0 / R² ≥ 0.98 | **PASS** | PASS |
| EXP-0 reference | +5.994 | +6.001 |

The U0 forward arm reproduces the EXP-0 phenomenon to within 0.03%–0.1%.
The diagnostic/test build's fixed overhead does not disturb the slope.

## 11 Forward-scan results (production_forward)

Medians per cell (instructions/op, cycles/op, wall ns/op):

| fs | C | instr/op | cycles/op | wall ns/op | iters/op |
| --- | --- | --- | --- | --- | --- |
| tmpfs | 8 | 4759.0 | 3987.7 | 4133 | 4.50 |
| tmpfs | 32 | 4900.4 | 4088.5 | 4228 | 28.50 |
| tmpfs | 128 | 5472.5 | 4328.8 | 4300 | 124.50 |
| tmpfs | 512 | 7777.4 | 5157.1 | 4642 | 508.50 |
| btrfs | 8 | 4365.5 | 2509.1 | 1786 | 4.50 |
| btrfs | 32 | 4509.5 | 2601.7 | 1660 | 28.50 |
| btrfs | 128 | 5085.6 | 2820.6 | 1877 | 124.50 |
| btrfs | 512 | 7389.9 | 3553.8 | 1962 | 508.50 |

OLS slopes vs C: instr +5.9922/+6.0007, cycles +2.2663/+2.0206,
wall_ns +0.94/+0.45 (tmpfs/btrfs). Sample spreads tight (worst cell
C=512 tmpfs forward: 7764.9–7783.6 instr/op, ~0.2%).

## 12 Reverse-scan results (reverse_ablation)

Medians per cell:

| fs | C | instr/op | cycles/op | wall ns/op | iters/op |
| --- | --- | --- | --- | --- | --- |
| tmpfs | 8 | 4758.5 | 4016.1 | 4133 | 4.50 |
| tmpfs | 32 | 4768.9 | 4050.4 | 4188 | 4.50 |
| tmpfs | 128 | 4759.7 | 4037.4 | 4205 | 4.50 |
| tmpfs | 512 | 4761.0 | 4029.8 | 4135 | 4.50 |
| btrfs | 8 | 4367.5 | 2516.1 | 1779 | 4.50 |
| btrfs | 32 | 4367.5 | 2516.4 | 1824 | 4.50 |
| btrfs | 128 | 4367.6 | 2503.7 | 1801 | 4.50 |
| btrfs | 512 | 4367.9 | 2511.7 | 1628 | 4.50 |

Reverse instr/op is FLAT at the C=8 floor (tmpfs ≈ 4758–4769 across all C;
btrfs 4367.5–4367.9, a 0.4-instr/op spread). Same-work held in every row.

## 13 Scan-iteration slopes

| fs | b_iter forward | b_iter reverse | reduction |
| --- | --- | --- | --- |
| tmpfs | +1.0000 (R²=1.0) | −0.0000 | 100.0% |
| btrfs | +1.0000 (R²=1.0) | −0.0000 | 100.0% |

Exactly one additional examined router entry per unit of configured
capacity in the forward arm; zero in the reverse arm.

## 14 Instruction/cycle slopes

| quantity | tmpfs fwd | tmpfs rev | btrfs fwd | btrfs rev |
| --- | --- | --- | --- | --- |
| b_instr (instr/op/C) | +5.9922 | −0.0039 | +6.0007 | +0.0007 |
| R² instr | 1.00000 | 0.038 | 1.00000 | 0.99993 |
| b_cycles (cycles/op/C) | +2.2663 | −0.0058 | +2.0206 | −0.0062 |
| C=8→128 cycles | **+8.6%** | **+0.5%** | **+12.4%** | **−0.5%** |
| instruction slope reduction | — | **99.9%** | — | **100.0%** |

The C=8→128 cycles penalty (EXP-0: +8.5%/+11.8%) appears in the forward
arm at the same magnitude and disappears under the ablation. Wall-time
slopes collapse too (tmpfs +0.94 → −0.06 ns/op/C), but per the
preregistered rule instructions:u is the authority; cycles/wall only
establish material user impact.

## 15 Causal adjudication

Preregistered rule (§19 of the task, encoded in the runner and validator):
STRONGLY SUPPORTED requires (1) baseline gate pass, (2) iterations grow
with C, (3) iteration-slope reduction ≥ 80%, (4) instruction-slope
reduction ≥ 80%, (5) |b_instr_reverse| ≤ 1.5, (6) same-work/correctness
gates pass, (7) direction reproduces in the btrfs control.

| # | criterion | tmpfs | btrfs |
| --- | --- | --- | --- |
| 1 | baseline reproduction | PASS (5.9922, R²=1.0) | PASS |
| 2 | iterations grow with C | +1.0000/C | +1.0000/C |
| 3 | iteration reduction ≥ 80% | 100.0% | 100.0% |
| 4 | instruction reduction ≥ 80% | 99.9% | 100.0% |
| 5 | \|b_reverse\| ≤ 1.5 | 0.0039 | 0.0007 |
| 6 | same-work + correctness | PASS (all rows, both arms) | PASS |
| 7 | direction reproduces | — | PASS |

**Verdict: ROUTER CAUSALITY STRONGLY SUPPORTED.** The forward linear
`find_live_router_cookie_` scan is the cause of essentially the ENTIRE
EXP-0 Uring capacity tax in the tested geometry — not merely a
contributor.

Validator: `python3 scripts/bench/perf-evidence-validate.py` — 17/17
artifacts OK (kinds `tax0u0router` ×2, `tax0u0witness` ×1 among them);
self-test (12 cases incl. the U0 mutation detectors: scan-mode tampering,
seed-order tampering, iteration tampering, slope tampering, same-work
tampering, envelope tampering, witness-smuggling) OK.

## 16 Residual capacity tax

b_instr_reverse: tmpfs −0.0039, btrfs +0.0007 instr/op/C. This is ~0.07%
of the original slope, below any material threshold (the preregistered
"partial residual" category requires a MEANINGFUL remaining slope), and
within measurement noise (tmpfs reverse R² = 0.038, i.e. no monotone
structure at all). No `T0-U-CAPACITY-RESIDUAL` ledger entry is warranted
by this data. The C=2× … extreme-capacity behavior outside {8..512} and
other workload families (WRITE, cancel-heavy, backlog) remain unmeasured
by U0 — those are scope boundaries, not residuals (§17).

## 17 Scope / limitations

- Geometry: EXP-0's frozen single geometry only (READ 4 KiB, D=8, Q=8,
  C ∈ {8,32,128,512}, 256 MiB, tmpfs + warm btrfs, single-driver
  ApplicationRuntime, workers=1). No WRITE, no Q/D variation, no backlog,
  no multi-worker axis (EXP-U1 material).
- The ablation is RESEARCH-ONLY: reverse scanning is NOT proposed as the
  production fix; it exists to isolate the causal variable. It is not a
  tuning knob, not configurable in production builds, and not exposed.
- Process-aggregate perf counters include fixed startup/teardown costs,
  identical across cells (amortized per op; never subtracted).
- One machine, one session per filesystem pair, governor schedutil,
  taskset 0,2,4,6 — same policy as EXP-0; cross-machine claims are out of
  scope.
- The equivalence claim is bounded to reachable states exercised by the
  gates (fresh-backend allocations, drains, stale/duplicate injection,
  cancel flows); it is not a formal proof over all interleavings.

## 18 What is NOT proven

- That the reverse scan is a good production design (it is an instrument,
  not a proposal).
- Anything about capacity behavior beyond C=512 or below C=8; about WRITE
  workloads, cancellation-heavy workloads, dispatch backlog, Q≠D, or
  multi-driver access.
- That `find_live_router_index_` (the SlotHandle-keyed cancel-side scan,
  a different function NOT ablated here) is or is not capacity-tax
  material in cancel-heavy workloads — out of U0's question.
- Any formal/whole-program correctness claim; the evidence layers are the
  mechanical equivalence gates + the same-work proof + the uniqueness
  theorem.

## 19 Optimization authorization

**OPTIMIZATION EARNED = YES** — EXP-U0 established WHAT causes the tax:
the per-CQE forward router scan walking ~C dead entries to reach the D
live entries parked at high indices.

Production design selection is deferred to issue #255 (benchmark-driven
fix selection), which decides HOW the tax should be fixed. #254 selects
NO production candidate; it closes only the causal-attribution question.
The reverse-scan ablation is a causally validated candidate, not yet the
selected production design.

Admitted candidate families (evidence status as of this freeze; listed,
not ranked as a selection — none wins in #254):

| id | design family | evidence status |
| --- | --- | --- |
| R0 | current forward-scan baseline | production baseline, measured (EXP-0 / U0 forward arm) |
| R1 | reverse scan (`for i = size; i-- > 0;`) | **DIRECTLY MEASURED BY EXP-U0** — capacity slope collapses 99.9–100% in the frozen geometry |
| R2 | low-index-first router-slot placement | **PLAUSIBLE PLACEMENT-DUAL / NOT YET BENCHMARKED** — predicted to remove the same high-index traversal mechanism, but performance is NOT YET MEASURED |
| R3 | bounded cookie→router index (O(1) lookup/table) | **DESIGN CANDIDATE / NOT BENCHMARKED** — not ranked out solely from complexity or Big-O intuition |
| R4 | direct/index+generation variants | admissible only if the explicit-request identity contract (§10, ADR-explicit-io-request-contract) permits |

R2 must not be stated as "expected gain ≈ R1" as a measured fact; R3 must
not be excluded by complexity intuition alone. Any chosen design needs
its own correctness gates plus a before/after remeasure on the EXP-0
matrix before the #250 ledger closes the entry.

## 20 Next step — #255 router candidate shootout

#254 closes the causal-attribution question only. Fix selection is owned
by #255:

    prior art
        ↓
    semantic admission
        ↓
    multi-geometry matched benchmark
        ↓
    robust winner selection
        ↓
    separate production PR
        ↓
    canonical EXP-0 before/after
        ↓
    recovered/residual tax ledger

EXP-U1 (Q ≠ D, backlog, WRITE, multi-worker axes) remains AFTER router
fix selection + production remeasurement, not before.

STOP — no production optimization was implemented or selected by EXP-U0.
