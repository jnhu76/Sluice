# #595 R5-repair — CORRECTIONS (to the R5 Phase-A archive in PR #630)

Round r5-repair-measure, 2026-11-07 (UTC). Every item below was verified
against frozen source this round. Historical records are not overwritten:
the PR-branch documents receive these corrections as edits plus pointers to
this file; the frozen R4/R5 raw data and preregistrations stay as recorded.

## F1 — diagnostic instrument data races (CONFIRMED and fixed)

`21374a40:src/async/r5_tail_trace.cpp`:

1. **CONFIRMED** `g_cursor` (plain `size_t`) is written by the submitter
   (note_built outside any lock; note_admitted inside `dispatch_mtx_`) and
   read by the driver in `latest_for` — and the driver's `note_cqe`
   (from `reap_cqes()`) and `note_published` (from the `publish_one` loop)
   run **outside** `dispatch_mtx_` (`poll()` at uring_backend.cpp:875-925
   takes the lock only for the dispatch/submit segment). The v1 comment's
   claimed happens-before chain ("index written inside the dispatch
   mutex … read by the driver only after a kernel completion") is broken
   twice: note_built's cursor write is never under that mutex, and the
   driver's reads are not under it either.
2. **CONFIRMED** `begin_phase`/`end_phase` reset/export arena state with
   only `g_active.exchange(false)` — writers that already passed the
   active check can still be writing records/counts during reset/dump
   (failure paths with stragglers; inter-phase driver activity).
3. **CONFIRMED** `Record.generation` is `uint32_t` and `latest_for`
   compares `static_cast<uint32_t>(generation)` while `RequestKey::
   generation` is `uint64_t` — silent 64→32 truncation on every key.
4. **CONFIRMED (latent, new finding this round)** v1's built→admitted
   pairing (`g_t1_cursor` positional walk) misaligns under a front
   rejection (admissions are an in-order *subsequence* of builds, not a
   prefix pairing); v1's own cookie check would have counted such
   misalignments as join failures — 0 were recorded in the R5 data, so no
   archived trace was affected — but the defect was real.
5. `g_counts` cross-thread increments (distinct fields) were per-field
   single-writer, but end_phase's export read them without synchronization.

**Fix** (branch `research-595-r5rm-diag` @ `c0cdc3ea`): per-writer
preallocated event buffers (single-writer cursors, no online cross-thread
index), seq_cst in_hook/active protocol with bounded quiesce wait before
reset/export (breach recorded in dump flags), full 64-bit generations,
v2 dump format, offline join with duplicate/missing classification,
wake association only within one writer thread. Validation: 5-thread
synthetic harness under TSan — 0 reports (frozen v1 TU under the same
harness reports data races); real bench d16 arm=R full join 20000/20000
zero failure counters; real bench rebuilt with `--sanitizers=tsan` runs
rc=0 with 0 reports. **Old v1 traces stay frozen evidence under the v1
analyzer; nothing is retroactively upgraded.**

## F2 — p99 attribution rule abolished (method error)

`r5/analysis/diagnostic-protocol.md:30-32` froze "an interval accounting
for ≥50% of the total p99 gap attributes the tail" and
`r5/TAIL_ANALYSIS.md:281` recommended re-applying it. **Quantiles are not
additive**: per-request phase means sum within a request, but arm-level
tail-set means and percentile differences do not decompose this way, and a
stage's percentile-contribution share is not removable CPU cost. The rule
is abolished: this round's protocol (PREREGISTERED.md §7) computes no
percentile-contribution attribution; tail statements are conditional
means / conditional tail risk by real group position, and causal claims
come only from intervention contrasts (C−R, C−A0). The old
preregistration and old results are unchanged as history; they are not
re-judged PASS/FAIL under any new rule. TAIL_ANALYSIS.md:281's
recommendation is corrected to point at the intervention-based protocol.

## F3 — timestamp origin preserved; scope caveat stated

The three arms use the frozen R4 latency origin unchanged (T0 = per-item
ReadOp build instant; `bench_w03` identical at f3b25329 and b191bb89 — C2's
bench hunks rename only). No suffix re-stamping (candidate B not
implemented). Caveat now stated in PREREGISTERED.md §4: A0 builds each op
individually while R builds the group first, so descriptor-ready time is
call-pattern-dependent — the historical caliber measures pipeline latency
from build, not application arrival latency.

## F4 — event-semantics wording corrections

| Location (reviewed head) | Incorrect claim | Correction |
|---|---|---|
| `NEXT_EXPERIMENT.md` candidate B row | "the driver cannot publish mid-group" | The driver **can** publish between presentations: every presentation acquires/releases `access_mtx_`+`dispatch_mtx_`, and poll()'s publication loop runs outside the dispatch lock. Mid-group publication is source-possible; it is merely rare because the submitter reacquires the locks back-to-back. B's argument rests on created service opportunities, not on an impossibility. |
| `TAIL_ANALYSIS.md` §6 (R/Q8 row) | "not to kernel straggle/wake/reap-publish (all measured against)" | Kernel-side instants (SQE consumption, CQE write) are unobservable by construction; what was measured: user-land reap spread (T3), same-thread wake→reap ordering, reap→publish. Kernel completion tail before the user-land reap is **not excluded**, only not evidenced. |
| `TAIL_ANALYSIS.md` Hkernel-a bullet | "refuted at T3" | Small T3 spread refutes **user-observed** reap-side straggle only; it cannot exclude earlier kernel-completion tails (T3 is the user-land reap instant, not the kernel completion instant). |
| `CALL_PATTERNS.md` G1/G2 submit-path park edges; `NEXT_EXPERIMENT.md` "truly parks" | cv.wait_for described as actual parking | Entering `cv.wait_for` is a **potential** park: the predicate may already hold (no block) and spurious wakes exist. Corrected wording distinguishes service opportunity / actual service / actual block; per-iteration actual-park counts were never measured. |
| `NEXT_EXPERIMENT.md` candidate A row | "`RequestCore::outstanding_` remains the sole inflight authority" | outstanding = accepted∧unpublished (a drain detector). It is the sole authority for *that* fact; it is not kernel inflight, and outstanding()==0 does not imply no observable/cleanup work remains. |
| T2/T3 semantics (various) | T2 read as "kernel starts", T3 as "kernel completes" | T2 = submit_enter return instant (CQEs may already exist); T3 = user-land reap instant. Both carry F4 qualifiers in this round's protocol; negative intervals preserved, never clamped. |

## F6 — PR #630 archive gaps (CONFIRMED and backfilled)

EVIDENCE-INDEX.md §4 lists `raw-diag/scripts/` and `raw-diag/equiv/` as
part of the archive, but the reviewed head `828b10b4` contains neither.
All 68 missing files (4 scripts + 64 equiv outputs, 292 KB, all text)
exist on the E5 host and are backfilled into the PR branch in this round's
update. Existence table (after backfill):

| Item | In PR | On E5 host | Lost |
|---|---|---|---|
| raw-diag non-bin matrix runs (113 files) | yes | yes (originals) | — |
| raw-diag/scripts/ (4 drivers/analyzers) | backfilled | yes | — |
| raw-diag/equiv/ (64 oracle/suite outputs) | backfilled | yes | — |
| raw-diag .bin arena dumps (~82 MB) + smoke .bin | no (declared) | yes (r5/raw-diag) | — |
| r4/ round tree (raw, figures, scripts) | no (PR #629 scope) | yes | — |

The EVIDENCE-INDEX §4 claim is corrected by this backfill; a
summary-only archive cannot independently recompute per-request results —
the .bin arenas stay E5-local and this limitation is now stated in the
README.

## Environment anomaly (record, not a correction)

Direct `g++ -D<NAME>` invocations from this agent's shell intermittently
drop specific define arguments (observed on `-DSLICE_HAS_LIBURING`;
`/usr/bin/g++` included). Worked around with `#define`-wrapper TUs; all
round builds went through xmake (unaffected — the diag instrument
activated, binary SHAs distinct and pinned). Recorded for reproducibility.

## Review corrections (2026-11-07, external review of head 6d7e585e)

An external review of the published round identified five defects in the
analysis and reporting. All five were confirmed against source and data,
corrected offline (no measurement rerun; the raw records and bootstrap
CIs are untouched — the corrected `analyze_round.py` reproduces every CI
bit-identically), and the affected documents were rewritten. The
pre-correction analysis JSON is kept as `analysis/round-analysis.gatebug-archived.json`
for audit.

### R1 — `caller_window` is NOT member position; F5 interpretation withdrawn and recovered

`bench_w03_external_loop.cpp` sets `facts.caller_window = window.size()`
once per `submit()` group, so every member of a group records the SAME
value: the in-flight window occupancy at group construction. The first
published round labeled it "real in-window position" — wrong, and the
"position 0 vs position 8 (back half of a 16-op refill)" framing is
withdrawn. `make_figures.py` F5 additionally filtered only `arm == R`
without a shape filter, mixing d16 and d4 rows (pos0 n=465,072 = both
shapes), and the 55 µs position stats quoted in RESULTS came from an
unarchived one-off command.

Correction: true member order is recoverable offline because the bench
builds groups of exactly `want = min(depth − window.size(), remaining,
max_refill)` requests with consecutive seqs.
`scripts/recover_f5_position.py` chunk-splits the joined ops (sorted by
seq) into groups of `depth − caller_window`, validates seq contiguity,
and drops (counts) anything damaged: over 480,000 R-mode measured rows —
24 phases — 0 anomalous groups, 0 seq gaps, 7 terminal partial groups,
0 unpositioned rows. Recovered result (d16, lead-level, instrument
PERTURBED): within full refills (occupancy 0) tail(>55 µs) risk is
≈1.2% for member positions 0–7 and ≈9.0–9.2% for positions 8–15 — a
sharp step exactly at the presentation-quantum-8 boundary (front-chunk
tails stop under `cap`, back-chunk tails under `input_exhausted`) — and
refills built at occupancy 8 carry ≈26% risk at EVERY member position.
The old "5.2% vs 26.3% ≈5×" compared a mixed front+back bucket against
the occupancy-8 bucket and is replaced by the statements above. d4 shows
no elevation (≈0.02% everywhere), consistent with single-chunk groups.
F5 regenerated from `analysis/f5-position.json` (d16 only, corrected
labels); CSV `figures/figure-data/f5-position.csv`.

### R2 — protection cells were evaluated against the MAIN gate; one-sided/two-sided bounds conflated in prose

`analyze_round.py` defined PROT_GATE but `analyze_cell()` applied
MAIN_GATE to every cell, so protection cells were tested for "CPU
reduction ≤ −10%" instead of "CPU regression ≤ +5%", and the RESULTS
prose quoted two-sided ci95 uppers ("cpu upper +5.1 > +5% FAIL") where
the frozen gate uses the ONE-SIDED 95% bound. Concrete effect:
W03-d2serial C−A0 CPU point +3.90%, one-sided upper95 +4.90 ≤ +5 → the
CPU item PASSES the frozen protection gate (it failed in the published
table). Cell verdicts are unchanged — d2serial still FAILs (tput
lower −4.6%, p95 upper +10.8%, p99 upper +14.0% over their bounds) —
but per-item verdicts and the per-cell Holm family are recomputed in
`analysis/round-analysis.json` (`gate` key, per-cell family per
PREREGISTERED §5). Also fixed: the RESULTS absolute-mean table was
hand-transcribed and wrong (A0 3196/51.6 µs vs actual 3208.6/53.92 µs);
all numeric tables are now script-generated
(`analysis/summary-tables.md`).

### R3 — "calibrated" C−R claims exceeded what A0 calibration can identify

The published wording ("calibrated residual ≈ −2% real savings",
"never harmful after binary-layout calibration") treated A0c−A0o as a
transferable correction. A0c−A0o runs the identical product path in two
binaries; it demonstrates that build artifacts/runtime conditions shift
results by a comparable order — it does NOT establish that the shift
applies proportionally to the R path (different code, cache and branch
behavior), and no joint uncertainty was modeled. Corrected wording:
C−R effects are reported RAW alongside the calibration band as a
confounding observation; the only claims kept are the frozen-rule ones
(MAIN C−R p99 upper95 < 0 by the frozen criterion but within the
calibration band → not attributed; MAIN C−R cpu CI crosses 0 → not
established). The d4/d2serial raw CPU savings (−3.9/−4.4%) exceed the
calibration point estimates (−1.5/−2.3%) — "consistent with small real
savings; not formally established". Keeping C1 rests on its source-level
justification (removes a duplicate scan, equivalence proven), not on a
proven universal performance win. Similarly, "the p99 cost and the
shallow-queue tax are properties of the batching policy itself" is
downgraded: they are properties of the R-family adapter path as a whole
(cleanup-invariant); a single-path cell with almost no batching
(d2serial) still degrades, which indicates an adapter/call-path tax
component — "batching-inherent" is NOT established by this round.

### R4 — diagnostic instrument and analyzer failure paths hardened

The v2 instrument's begin_phase ignored wait_quiesce() failure and reset
buffers anyway, and end_phase exported events even on breach — the
"timeout flag" did not remove the concurrent access. Fixed (commits
`492086ac` on research-595-r5rm-diag, mirrored `3fd6476a` on
research-595-r5rm-diag-cleanup): on breach, begin_phase does not reset
and does not reactivate; end_phase writes a header-only failure dump
(flag bit 4, n_writers 0, no per-writer data, no event bytes) and the
instrument disables itself for the rest of the process. Compile-checked
in a scratch worktree (round binaries untouched) and the 5-thread TSan
harness re-run clean (7.08M ops, 0 reports). The breach path itself has
no deterministic forcing test (hooks cannot be stalled from outside) —
its dump format is covered by the analyzer abort case below; recorded as
a limitation.

The analyzer's FIFO pairing was unconditional and failure-blind: a
"first build rejected, second accepted" phase misbound rows that still
carried `join=ok`, and `main()` computed `bad` but returned 0. Fixed:
pairing is a validated hypothesis — count mismatch ⇒ no pairing is
fabricated, every row gets `join=fifo_count_mismatch` with slot/gen
null, keyed joins are skipped, T5 (seq-keyed) survives, and the process
exits 1 on any flag/counter/row anomaly. Deterministic counterexamples:
`harness/test_tail_trace_analyzer.py` (good / reject / duplicate-key /
header-only-abort, 4/4 PASS). Regression: the fixed analyzer on a real
round dump exits 0 with all 20,000 rows field-identical to the archived
join. DOCUMENTED BLIND SPOT (honest narrowing): an equal-count
mispairing (rejection + rebuild rebalancing the counts) is not
detectable from the recorded fields, because admitted events carry the
adapter's (slot,gen), not the bench seq; the next instrument revision
must record the bench seq or cookie on admitted events.

### R5 — headline scope qualifiers (CPU coverage, candidate B status)

"CPU −22%" is process-rusage CPU/op; kernel io-wq worker CPU is outside
rusage (ticks recorded separately, R/C ≤ A0) — every headline now says
"process-rusage CPU/op" and the coverage gap travels with it; it is not
a whole-system CPU number. NEXT_EXPERIMENT.md's Candidate B design still
re-stamps T0 at a later round's rebuild and thereby misses wait that
already happened — same-origin comparison is not recoverable by "same
gate thresholds" — and B's motivating lead now rests on the corrected
R1 semantics (quantum-8 chunk step + occupancy-8 refills), which this
round's PERTURBED diagnostic supports only at lead level. Candidate B is
therefore NOT green-lit; NEXT_SINGLE_ACTION is downgraded to "select the
next intervention after these corrections"; a dated warning is added to
NEXT_EXPERIMENT.md.
