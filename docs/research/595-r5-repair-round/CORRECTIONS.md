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
