# #595 R5 Phase A — Code-Issue Verdicts C5–C7

Role: code-issue verifier (C5–C7). Date: 2026-11-06. Source baseline:
`/home/jnhu/sluice-595-e5/r5/worktree` (research-595-r5 @ `f3b25329bc8a`,
byte-identical to the frozen R4_ORIGINAL tree; worktree left untouched,
porcelain clean). R4 evidence read read-only under
`/home/jnhu/sluice-595-e5/r4/`. Every claim below cites what was read or
run in this session. No timed runs were performed, so no measurement lock
was required (per round rule, non-timed work runs unlocked); the lock file
`/home/jnhu/sluice-595-e5/MEASUREMENT.lock` was not touched by this agent.
Cross-reference: the parallel tracker's
`/home/jnhu/sluice-595-e5/r5/analysis/call-patterns-complete.md` was
scanned AFTER my independent source verification; its facts agree with the
ones below; all verdicts are derived from my own reads.

Worktree/file hygiene: main repo `/home/jnhu/Source/Sluice` untouched;
`r4/` untouched; all new files written only under
`/home/jnhu/sluice-595-e5/r5/analysis/` (`findings-c5-c7.md`,
`corrections/`).

---

## C5 — "A small batch never truly yields" — verdict: CONFIRMED

Claim verified against source: after a quantum-bounded presentation of 2
(or any chunk), the adapter does not enter progress or observer code and
does not return to the caller's yield point; it continues the SAME
submission loop.

### 1. The chunk loop continues on the same thread with no yield

`bench/r3_ready_batch.hpp` (frozen R4 adapter):

- Presentation size is bounded by the decision quantum only:
  `present_len = std::min(decision.quantum, input.eligible_prefix)`
  (`:497-499`), then one backend call presents that prefix
  (`:527-529`).
- After the batch returns, the loop-exit condition is
  (`:559-575`):
  - `cap_reuse` = reuse_on_cap && progressed && stop==cap && more (`:561-563`)
  - `chunk_continue` = budgeted && !shadow && progressed &&
    stop==input_exhausted && more (`:564-567`)
  - `if (!cap_reuse && !chunk_continue) break;` (`:573-575`)
- For a quantum-2 chunk that is fully accepted, the backend returns
  `stop=input_exhausted` because `report.accepted == ready.size()` for the
  PRESENTED span (`src/async/uring_backend.cpp:599-600`; the backend's
  `ready` parameter is the presented prefix, `:104-105` and adapter
  `:527-529`). With more ops left in the caller's span, `chunk_continue`
  is TRUE → the next 2-op chunk is presented immediately: same `while`
  loop, same call stack, no `poll_progress`, no observer check, no
  condition-variable wait, no application code between chunks.
- The same applies to R (non-budgeted, full span): the backend caps a
  presentation at `ready_batch_cap = 8`
  (`include/sluice/async/detail/uring_backend.hpp:92`,
  `src/async/uring_backend.cpp:531`), returns `stop=cap`, and R's
  `reuse_on_cap` re-presents the remainder in the same loop
  (`r3_ready_batch.hpp:561-563,568-569`). Neither arm yields between
  chunks; they differ only in per-presentation size (S: 2, R: 8).
- Each presentation ends in its own kernel enter: on accepted work the
  backend calls `submit_transport_locked()` inside the same call
  (`src/async/uring_backend.cpp:577-582`), which increments
  `submit_flushes_` and issues `::io_uring_submit`
  (`src/async/uring_transport.cpp:223-240`). So "quantum 2" = one enter
  per ~2 SQEs — the presentation bound, not a yield.

### 2. The only true yield points, and what is stale inside one submit() call

Harness loop `bench/bench_w03_external_loop.cpp:395-573`:

- Refill builds up to `want = min(depth − window.size(), remaining,
  max_refill)` ops (`:396-398`) and calls `adapter->submit(...)` ONCE
  (`:423-424`); all chunk presentations of that round happen inside this
  one call.
- `DecisionFacts` (pressure snapshot) is captured ONCE per
  `adapter->submit()` under `shared.mutex` (`:415-422`), passed by const
  reference, and reused for EVERY chunk of that call
  (`r3_ready_batch.hpp:449-452`); only `ctx_.outstanding()` is re-read
  live per decision (`r3_ready_batch.hpp:467-471`).
- After the refill inner loop (one round when `max_refill != 0`,
  `:509-511`; else until the window is full, `:396`), the submitter's
  ONLY wait is `shared.cv.wait_for(..., completions > watermark)`
  (`:522-539`) — the single genuine yield to the driver, reached once per
  outer iteration.

### 3. Batch quantum vs work quantum (the distinction the claim asks for)

- Batch quantum = `present_len` per backend call = `min(decision.quantum,
  eligible_prefix)` (`r3_ready_batch.hpp:497`) — bounds SQEs per
  `io_uring_submit`.
- Work quantum (one continuous submission run with zero interleaved
  progress) = the whole `want` span of a refill round — presented as
  ⌈want/quantum⌉ back-to-back chunks inside one `submit()` call — plus,
  for `max_refill == 0` cases, potentially consecutive refill rounds while
  `window.size() < depth` (`:396`) before the first wait. With S on
  W03-d16 (mean ready 7.92 per R4), the work quantum per yield is ~8 ops
  presented as ~4 enters, not 2. This matches the R4 finding that S
  behaves as fixed small-batch Q2 (99.97% quantum=2, RESULTS.md §3/§8),
  and explains the ~4× presentation rate (CAPTIONS F2: "0.125/op →
  0.500/op").

### 4. External driver concurrency, lock traffic, observer thread

- Concurrency: the driver is a separate `std::thread`
  (`bench_w03_external_loop.cpp:329`) that claims the progress owner
  (`ctx.claim_progress_owner()`, `:183`), parks in `epoll_wait` on the
  notification fd (`:207-217`), acknowledges the notification, then runs
  `ctx.poll_progress()` passes in a loop until no immediate work remains
  (`:231-245`) and publishes completions under `shared.mutex` +
  `notify_all` (`:247-264`). Driver and submitter genuinely overlap:
  completions can be reaped while the submitter is mid-loop between
  chunks; the chunk loop simply never consults them.
- Lock traffic between the two threads:
  - `shared.mutex` (harness): driver per epoll iteration/publication;
    submitter per `adapter->submit()` for pressure arms
    (`reads_pressure_facts()`, `r3_ready_batch.hpp:375-379`;
    `bench:417-422`) and during the cv wait.
  - `AsyncIoContext::access_mtx_`: submitter via
    `TestAccess::context_submit_ready_reads`
    (`include/sluice/async/detail/test_access.hpp:69`) and — for
    S/SNP/SHADOW — via `ctx_.outstanding()` per decision
    (`src/async/async_io_context.cpp:837-840`, lock at `:839`);
    driver via `acknowledge_progress_notification()`
    (`src/async/async_io_context.cpp:738-742`).
  - `UringAsyncBackend::dispatch_mtx_`: submitter inside
    `submit_ready_reads` (`src/async/uring_backend.cpp:504`); driver
    inside `poll_progress` tail (`:318-330`) and CQE reap. Submission and
    reap therefore serialize on `dispatch_mtx_`.
- Observer thread: W03 attaches NO library observer. Application-level
  observation happens on the SUBMITTER (main) thread by polling the front
  of its deque (`window.front().request.ready()`, `:541`,
  `include/sluice/async/request.hpp:130-132`); results are published into
  RequestCore by the driver thread and consumed by the main thread via
  `take_result()` (`request.hpp:146-159`).

Phase-B candidate directions noted (NOT implemented this round, per the
ask): saving/borrowing the unaccepted suffix across a yield, handing
progress responsibility for a bounded re-schedule, or a bounded re-entry
path that does not depend on a future request arriving.

---

## C6 — Caller observation and buffer safety — verdict: CONFIRMED

### 1. W03 consumes strictly from the queue head

`bench_w03_external_loop.cpp:541-572`:

```cpp
while (!window.empty() && window.front().request.ready()) {
    Inflight& front = window.front();
    const auto observed = front.request.take_result();
    ... verify_pattern(buffers[front.slot].data(), ...) ...
    window.pop_front();
}
```

`window` is a `std::deque<Inflight>` filled by `push_back` in submission
order (`:461-467`, `:499-502`) and drained only from the front. The drain
loop STOPS at the first not-ready front entry (`:541`): a straggler at the
head blocks release of every ready entry behind it (head-of-line), which
is the structural source of the refill-timing coupling described below.

### 2. The `submitted % depth` buffer-reuse assumption, verified

Allocation and slot assignment (`:359-364`, `:404-413`, `:463-466`):

- `buffers` = exactly `depth` buffers; `slot = sequence % params.depth`;
  the ReadOp destination is `buffers[slot].data()`; `Inflight` records the
  same slot.

Safety proof from these three code facts:

(a) window capacity ≤ depth: refill runs only while
    `window.size() < params.depth` (`:396`);
(b) window contents are CONSECUTIVE sequence numbers (push_back on
    submission order, pop_front on front order);
(c) an entry leaves the window only after `ready()` was true AND
    `take_result()` consumed it (`:541-571`) — i.e. after the kernel wrote
    the buffer AND the application verified it (`:554-563`).

When sequence' = sequence + depth is being built (reusing slot s), (a)
gives `window.size() ≤ depth − 1` just before the push, so with (b) the
oldest possible resident sequence is sequence' − (depth − 1) > sequence'
− depth = sequence: the previous user of slot s has already been popped —
its read completed and was observed — before its buffer is overwritten.
Reuse by modulo is therefore safe under the CURRENT discipline.

### 3. What changes if observation/consumption order changes

- Application output order: each op is independent (offset =
  `sequence % blocks`, verify keyed by the entry's own slot+offset,
  `:411`, `:554-563`); there is no cross-op output dependency in this
  harness. Reordering observation would NOT change delivered bytes; it
  changes which latencies are attributed at which settle instant and the
  order in which failures (short read / not-successful) surface.
- Window release and refill timing: currently release is FIFO (a ready
  entry behind a not-ready front stays held). Releasing ready entries as
  they become ready would free window slots earlier and advance refill —
  this is precisely the separation the round asks to keep researchable.
- Slot/buffer use — the hazard: a naive "scan the deque, consume whoever
  is ready" change BREAKS the proof above, because (b) fails: out-of-order
  pops leave gaps, and the window can hold `sequence` (still in flight)
  while `window.size() < depth` admits `sequence + depth`, whose
  `dst = buffers[slot]` is the SAME buffer the in-flight op is reading
  into. Minimal counterexample (depth=4): window {s0,s1,s2,s3}; s1,s2,s3
  ready and popped; window {s0} (size 1 < 4) → refill builds s4, slot
  4%4=0 = s0's slot → the kernel starts a second read into `buffers[0]`
  while s0's read may still be writing it → corrupted data for s0. This
  is why the round FORBIDS the deque-scan + old-slot-allocation
  combination, and my source verification confirms the prohibition is
  necessary, not stylistic.

### 4. What the public API already permits, and the required endpoint

- Independent, non-consuming observation is already public:
  `Request::ready()` (`include/sluice/async/request.hpp:130-132`, const,
  non-blocking core lookup via `RequestBridge::ready`,
  `src/async/detail/request_core.cpp:710-714`) and
  `Request::try_result()` (observe without consume,
  `request.hpp:134-144`; pending leaves the handle unchanged). So
  per-entry readiness polling in any order is API-legal TODAY; the FIFO
  restriction is a harness discipline, not an API constraint.
- Researchable separation (Phase B, not implemented): observe per-request
  readiness independently while preserving the application's in-order
  completion endpoint. Any such design must (i) keep the application's
  required completion point (the settle/verify/take terminal for every op,
  as at `:541-572`), and (ii) account for the added reorder bookkeeping
  and any extra buffers/entries as a counted resource cost. Options that
  keep safety: per-entry buffer ownership (a slot is reusable only after
  its entry is consumed, not merely after it leaves the front), or a
  delivery-order reorder stage decoupled from completion observation —
  both change refill timing and must be measured as such.

---

## C7 — Research instrumentation and verdict errors — verdict: CONFIRMED (six sub-items; corrections written, R4 archive untouched)

All corrections live in `/home/jnhu/sluice-595-e5/r5/analysis/corrections/`
(new directory; nothing under `r4/` was modified). No new tests were added
(round rule: no tests for pure wording changes).

### C7.1 R−B / B−R and throughput sign conventions — CONFIRMED

- Frozen orientation: `r4/harness/analyze_pairs.py:5-11` — cost metrics
  `(m2−m1)/m1`, tput `(m1−m2)/m1`, "positive = second member worse";
  main matrix `r4/scripts/matrix_analysis.py:6-9` identical.
- B supplement follows it: `r4/scripts/bsupp_descriptive.py:36-43` —
  cpu/p95/p99 = `(B−R)/R` under container key
  `paired_diff_B_minus_R_pct`, but tput = `(R−B)/R` filed in the SAME
  container as `tput_drop` (one container, two algebraic directions).
- Presentation defect: `r4/RESULTS.md:188` labels the row "R−B（正=B 比
  R 差）" and `RESULTS.md:394` repeats "R−B 的 CPU +9.04%"; no formula
  labelled R−B reproduces those numbers (they are B−R for cost, R−B for
  throughput). The per_cell_table (`r4/analysis/per_cell_table.md:75-76`)
  does not restate the numbers, so the defect is confined to RESULTS
  §7-table and §16.1 plus the figure-data F3 note ("positive =
  first-named worse", `make_figures.py:231,245-247`).
- No numeric value is wrong; only direction labels/keys.
- Correction: `corrections/C7-1-sign-convention.md` (unified
  per-metric-formula wording, before/after).

### C7.2 figure-data segment-index / unit problems — CONFIRMED (residual, structural)

- R4 had already caught and fixed two value bugs (RESULTS.md §17.2: F5
  T1/T2 rows copied T3 via `ready[-1]`; F5 quantum2_share stored percent
  under `unit=fraction`). I re-verified the frozen CSV (sha256
  `c2c3294b…`, matching RESULTS.md:480) against
  `r4/analysis/MAIN_SUMMARY.json`: all 370 rows numerically consistent —
  command run this session, zero mismatches.
- Remaining defects verified this session (all index/metadata, no values):
  P1 `cell` not a cell id (F3 uses case string `W03-uring-4k-d16` for BOTH
  MAIN and B-SUPP rows — 42 rows, two source cells indistinguishable; 96
  F2/F5 rows have empty cell; F5b's source cell TRAJECTORY unnamed); P2
  `arm` holds contrast labels in 69 rows; P3 `segment` overloaded; P4 `n`
  silently switches between matched-blocks (24) and decision-lines
  (94..60000).
- Correction: `corrections/figure_data_normalize.py` (read-only input) +
  `corrections/figure-data.normalized.csv` (adds source_cell / contrast /
  dataset / n_unit; original values byte-identical; run output
  `rows=370 cell_corrections=42 arm_column_disambiguations=69`) + record
  `corrections/C7-2-figure-data-columns.md`. Figure images are unaffected
  (they consume MAIN_SUMMARY directly).

### C7.3 T1 shadow_outstanding_sum==0 as a legal observation — CONFIRMED

- Frozen criterion: `r4/PREREGISTERED.md:100-105` — any SHADOW timed
  invocation with `shadow_outstanding_sum==0` is invalid (oracle-failure
  class), calibrated on a W03-d16 smoke run that produced 76.
- Why it structurally fires on T1: d2serial = depth 2 / max_refill 1
  (`bench_w03_external_loop.cpp:856`): one op per refill round
  (`:397-398`, break `:509-511`), wait for its completion (`:522-539`),
  settle it (`:541-572`) → at every decision point the previous op is
  published and consumed, so `outstanding()` (read per decision at
  `bench/r3_ready_batch.hpp:467-471`) is structurally 0 — the instrument
  is live (20 000 decisions per rep) and correctly reports zero.
- Run evidence read this session:
  `r4/raw/matrix/TRAJECTORY/attempts.jsonl` — 72/72 attempts invalidated
  with reason exactly "shadow_outstanding_sum==0 (prereg 1.4 oracle)";
  `r4/raw/matrix/TRAJECTORY/jsonl/b00-p7-SHADOW-T1-a1.jsonl` — exit 0, all
  records ok, 5 measured reps × 20 000 ops, per-rep
  `r4_shadow_decisions=20000`, `r4_shadow_outstanding_sum=0`,
  `r4_shadow_window_sum=0`.
- Diagnosis: the criterion conflates instrument liveness (its purpose)
  with "state non-zero at least once" (a shape property that is
  structurally false on d2serial), so the oracle has zero discriminating
  power there and converted 24 good blocks into INCOMPLETE.
- Correction (protocol for R5+, NO retro-flip of R4's FAIL):
  `corrections/C7-3-shadow-oracle-protocol.md` — shape-aware oracle:
  liveness via `r4_shadow_decisions == submitted_ops` (+diag
  `active_read=1` lines), state expectation conditional on shape class;
  structural-zero shapes record `STRUCTURAL_ZERO` as a legal observation.

### C7.4 "39 protected members failed" ≠ 39 independent significant regressions — CONFIRMED

- Frozen data verified: `r4/analysis/MAIN_SUMMARY.json` `gates.F_PROT` —
  39× `FAIL_BOUNDS` (each with `point_ok=false` and/or `upper_ok=false`,
  all `holm_reject=false`, member p ≥ 0.2432; sample
  CORE-W03-D2SERIAL/Q8/cpu p=1.0), 4× `NOT_ESTABLISHED` = in-bounds
  (point_ok=true, upper_ok=true) but Holm-non-certifiable (sample
  CORE-W02-D1/Q8/cpu mean +3.88%, upper +4.58% ≤ +5%, p=0.0051 vs
  Holm threshold 0.00119) — the 4 are evidence insufficiency on the SAFE
  side, not regressions.
- RESULTS.md:80,95 are definitionally correct; the compressible top-line
  phrasing (:14, :358, :457) is where the misreading ("39 significant
  regressions") can occur. The 39 are correlated (cell, arm, metric)
  tuples over 7 cells sharing 24 matched blocks per cell (D2SERIAL alone
  contributes 12; W02-D16 8) — far fewer than 39 independent
  demonstrations.
- Correction: `corrections/C7-4-fprot-wording.md` (before/after summary
  phrasing distinguishing gate violation vs statistical proof vs
  insufficient evidence).

### C7.5 W02-d3 missing case = harness gap, not hardware — CONFIRMED

- Pre-disclosed and frozen: `r4/PREREGISTERED.md:177-180` (no frozen W02
  d3 case; nearest frozen depth-3 shape is W03 r3d3, descriptive only);
  recorded `r4/RESULTS.md:335` NOT_RUN_WITH_REASON.
- Harness verification: `bench/bench_w02_pipeline.cpp` — uring W02 depths
  1/4/16/64 (`:916-919`), d2 only behind opt-in `SLUICE_BENCH_595_DEPTHS`
  (`:930-935`, comment "D2 absent from the frozen G0 list"), NO depth-3
  case anywhere; the W03 harness has `r3d3`
  (`bench_w03_external_loop.cpp:857`) — a different workload.
- Record + worth-it assessment: `corrections/C7-5-w02-d3-record.md` —
  recommendation: do NOT backfill on its own (neighboring d1/d2/d16
  already FAIL 6/6/8 of 12 members); if the shallow-depth transition of
  the pipeline ever becomes a hypothesis, preregister a small descriptive
  sweep next to the existing r3d3 slot. No matrix expansion this round.

### C7.6 Resource-evidence wording — CONFIRMED (archive conforms; rules codified)

- Verified: `r4/RESULTS.md:159-166` — RSS/threads PASS are backed by
  measured data (0.00% deltas, wait4-vs-peak cross-check, 24/24 thread
  comparison with an honest non-candidate anomaly note); overall gate
  NOT_ESTABLISHED precisely because static-audit/strace/valgrind are
  NOT_RUN ("依 §8.1「NOT_RUN → NOT_ESTABLISHED」…整体不得 GO"); HOLD basis
  repeats it (:457). Grep for zero-cost phrasing across RESULTS /
  PREREGISTERED / TAIL-LATENCY: no matches — the archive neither fakes
  correctness errors from missing resource docs nor claims unmeasured
  zero cost.
- Correction (rules for R5 summaries): `corrections/C7-6-resource-wording.md`
  — missing evidence = NOT_ESTABLISHED (never a correctness error);
  untested dimensions must never be phrased as zero cost; measured 0.00%
  RSS must be quoted with its sub-gate scope and the NOT_RUN tail.

---

## Verdict summary

| Item | Verdict | One-line |
|---|---|---|
| C5 | CONFIRMED | Quantum bounds only the per-enter presentation; chunk_continue/cap_reuse keep the same loop presenting the whole refill span with no progress/observer interleave; the sole yield is the submitter's cv wait; driver is a genuinely concurrent thread (shared.mutex, access_mtx_, dispatch_mtx_ are the contention points); app observation runs on the submitter thread, no library observer in W03. |
| C6 | CONFIRMED | W03 drains strictly from the deque head; `sequence % depth` buffer reuse is safe only under FIFO + window-capacity==depth; a naive ready-first deque scan breaks that proof (documented counterexample); independent observation is already public API (`ready()`/`try_result()`), separable from in-order delivery in Phase B with the completion endpoint preserved and reorder costs counted. |
| C7 | CONFIRMED | All six sub-items verified against frozen scripts/data; two label/convention defects (R−B direction; figure-data column semantics) fixed as new-side corrections with no numeric change; T1 oracle defect diagnosed structurally with a shape-aware replacement protocol; 39-member, W02-d3 and resource wording rules codified; R4 archive untouched. |

Correction index: `corrections/C7-1-sign-convention.md`,
`corrections/C7-2-figure-data-columns.md` (+ `figure_data_normalize.py`,
`figure-data.normalized.csv`),
`corrections/C7-3-shadow-oracle-protocol.md`,
`corrections/C7-4-fprot-wording.md`,
`corrections/C7-5-w02-d3-record.md`,
`corrections/C7-6-resource-wording.md`.

Not run / limitations: no builds, no benchmark executions, no timed
comparisons (out of this role's scope this round); no modification of any
r4 file or the r5 worktree; `figure_data_normalize.py` was executed once
(non-timed) and its output verified by row count and spot reads.
