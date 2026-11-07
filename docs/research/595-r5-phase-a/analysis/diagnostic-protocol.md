# #595 R5 Phase A — Tail-Latency Phase-Decomposition Diagnostic Protocol

Status: **FROZEN BEFORE COLLECTION** (written 2026-11-06 by the p99 evidence
reviewer; no data has been collected under this protocol yet). This protocol is
a design artifact only; executing it is a separate decision by the round owner.
Companion analysis: `tail-evidence-review.md` (same directory).

## 1. Target question and hypotheses to discriminate

Question: for `W03-uring-4k-d16` (warm tmpfs, 4 KiB, closed-loop depth 16),
R4 measured R p99 +21.85% (upper +23.58%) vs A0 while p50/p95 stayed within
+4.75%/+2.71% (`r4/RESULTS.md:62`, `r5/r4-archive/TAIL-LATENCY.md:24`). Where
in the request pipeline is the added tail time spent?

Existing evidence (see `tail-evidence-review.md` §5–§6) cannot separate the
four candidate locations. This protocol discriminates them per request:

| ID | Hypothesis | Discriminating interval (see §4) |
|----|------------|----------------------------------|
| Hprep | submission-side serialization: ReadOp build → accepted → SQE prepared → `io_uring_enter` return takes longer for restart bursts | `T2 − T0` (build→enter-returned) |
| Hkernel-a | kernel/io-wq completion delay: SQEs of an 8-batch complete late relative to their single enter | `T3 − T2` on the driver-wake boundary (`T3 − T_wake` small) |
| Hkernel-b | driver-wake latency: CQEs exist but the driver's epoll wake arrives late | `T_wake − T2` vs `T3 − T_wake` |
| Hreap | CQEs visible but reap/publication deferred inside the poll pass (pass-bound batching, mutex serialization) | `T4 − T3` |
| Hobserve | request published but the caller observes it late (cv wake, in-order window-front blocking) | `T5 − T4` |

Falsifiable decision rule (primary, per shape, arms R vs A0): compute the
per-op interval durations, take per-arm p50/p95/p99 of each interval and of
the total `T5 − T0`, then attribute the total-tail gap
(p99_total(R) − p99_total(A0)) to the interval(s) whose p99 arm delta carries
≥ 50% of the gap. The interval set is a partition of `T5 − T0` up to
measurement boundaries, so exactly one interval family is expected to dominate;
if no interval reaches 50% the result is "distributed" and is reported as such
(no forcing).

Secondary pre-registered checks:

1. **Tail-op phase membership.** For the top 1% ops by total latency in each
   arm, report the distribution of (a) `caller_window` at submit time and (b)
   whether the op was a member of a `stop=cap` restart presentation
   (`outstanding()==0` at presentation, `r3_ready_batch.hpp:557-558`). R4's
   SNP anomaly predicts tail ops concentrate in restart presentations. If tail
   ops are uniformly spread across phases, the restart-concentration reading
   of the SNP result is refuted for R.
2. **Burst structure.** Report the completion timestamp spread
   `max(T3) − min(T3)` within each presented batch (grouped by presentation
   identity, §4). Hkernel-a predicts a wide spread for 8-batches that is
   absent for A0 singles; inline completion predicts near-zero spread.
3. **Control shape.** On `W03-uring-4k-d4` (R4: R p99 −12.92%, tail improves,
   `r5/r4-archive/TAIL-LATENCY.md:73`) the same intervals must show where the
   improvement comes from; a protocol whose instrument cannot reproduce the
   sign flip is reported as perturbed (see §6).

## 2. Workload, arms, and run plan

- Binary: `bench_w03_external_loop` built from the diag branch
  `research-595-r5-diag` (`/home/jnhu/sluice-595-e5/r5/worktree-diag`), HEAD
  recorded in the receipt; build output to `/tmp/r5-build-taildiag` only; the
  frozen worktree `/home/jnhu/sluice-595-e5/r5/worktree` stays untouched
  (porcelain = 0 before and after).
- Cases: `SLUICE_BENCH_CASE_FILTER=W03-uring-4k-d16` (primary) and
  `=W03-uring-4k-d4` (control; exists in the frozen bench set,
  `bench_w03_external_loop.cpp:836-840`).
- Arms: `A0` and `R` only (`SLUICE_595_R3_ARM`). Minimum pair that isolates
  the regressed comparison; no S/Q8/SNP in this protocol (R4 counters already
  characterize their decision mixes; adding arms doubles lock time for no new
  discrimination).
- Repetition: 3 blocks × 2 interleaved ABR/RBA orderings × 1 measured rep per
  arm per shape (6 phase-pairs per shape, 24 timed phase executions including
  the harness's internal oracle+instrumented subphases). Each phase = 20,000
  ops → 200 samples at p99 per phase, 1,200 per arm per shape.
- Mode: **diagnostic-only runs**, never mixed with clean gate blocks (R4
  precedent: `r4/raw/diagnostics/header.json` "not timed evidence, never mixed
  with clean blocks"). Results feed mechanism attribution only; no gate in
  this protocol can PASS or FAIL a R4/R5 claim.

Honest degradation: if fewer than 24 phase executions complete or trace
coverage (§5) falls below 95%, report only direction-level clues ("the largest
interval gap appeared in X"), explicitly labeled as a lead, not a decomposition.
No sample-size rescue by re-running until a preferred interval wins.

## 3. Instrumentation design (all env-gated, default off)

Compile-time constant gate `SLUICE_595_R5_TAIL_TRACE` (diag branch only; no
public header change; no new thread, timer, or runtime state beyond a
preallocated arena):

| Event | Recorded at (diag branch, source anchor in frozen tree) | Thread |
|---|---|---|
| T0 `built` | ReadOp construction, same site as `ready_ns` today (`bench/bench_w03_external_loop.cpp:413` adapter path, `:491` A0 path) | submitter |
| T1 `accepted` | after `admit_` returns a key, per key: batch path inside the admit loop (`src/async/uring_backend.cpp:541-552`), single path after `admit_` (`:467-489`) | submitter |
| T2 `enter_returned` | after `submit_transport_locked()` returns, stamped once and applied to every key of that presentation (`src/async/uring_backend.cpp:577-582` batch path; `src/async/uring_transport.cpp:92` single path) | submitter |
| T_wake `epoll_return` | driver epoll_wait return carrying the notification fd (`bench/bench_w03_external_loop.cpp:209-217`) | driver |
| T3 `cqe_reaped` | in `handle_one_cqe` after the live router lookup resolves (`src/async/uring_completion.cpp:148-151`) | driver |
| T4 `published` | in `publish_one` after `complete_publication` returns (`src/async/uring_backend.cpp:821-830`) | driver |
| T5 `taken` | at `take_result` success, same site as the latency end (`bench/bench_w03_external_loop.cpp:543-567`) | submitter |

Record layout (one per op, written in-order into a preallocated arena; no
allocation, no formatting, no I/O on the hot path): sequence (u32),
slot (u32), generation (u32), cookie (u64), 7 timestamps (i64 ns each),
presentation id (u32, monotonic per submit call), flags (u32: stop-kind of the
owning presentation, caller_window at submit). Arena capacity
`2 * kInstrumentedSampleCap` (cap constant already exists at
`bench_w03_external_loop.cpp:48`); overflow drops the record and increments a
lost-counter reported with the results.

Presentation identity: one incrementing id per `Adapter::submit` /
single-submit call; batch membership is reconstructable as
(presentation id, index) from T1/T2 sharing the same id.

Kernel-consumption time (the instant the kernel actually issues IO) is
**unavailable** by construction; T2→T3 is labeled "enter-returned → reaped",
which contains kernel completion plus wake plus reap work, decomposed further
by T_wake. The kernel's CQE-write instant is likewise unavailable; T3 is the
first userspace observation of it.

## 4. Perturbation audit (mandatory before interpreting)

1. Both arms run with the identical instrument; comparison is within-mode.
2. On/off check: run the same A0/R pair once with the arena writes compiled
   out (env gate off) under the same conditions and compare p50/p95/p99
   totals. Acceptance: the R−A0 p99 delta with tracing on must keep the R4
   sign and be within +3pp of the off-mode delta on the same session; else
   all on-mode numbers are reported as perturbed and the protocol does not
   attribute. Precedent bound: R4's read-and-record instrumentation
   (SHADOW−R) cost p99 +1.22% (`r4/RESULTS.md:182`).
3. Per-request ready queries or observers are NOT added (they can change
   publication/observation behavior); all events hook existing lifecycle
   points listed in §3.
4. The instrumented phase remains the harness's third subphase
   (`bench_w03_external_loop.cpp:779-787`); no change to phase structure.

## 5. Request identity and clock rules

- Identity triple (slot, generation, cookie) maps bench sequence →
  `RequestReservation` → `RequestKey` → SQE `user_data`
  (`src/async/uring_transport.cpp:156-204`; lookup
  `src/async/uring_transport.cpp:556-562`; control cookies are
  tag-disjoint, `src/async/uring_internal.hpp:48-56`). Records carry all
  three; analysis must never join on slot alone (slots are recycled per
  `request_core.cpp` reclaim).
- Clock: `std::chrono::steady_clock` for every timestamp (same source as the
  existing latency pass, `bench/bench_common.hpp:25-34`). Cross-thread
  intervals use raw differences; negative differences are kept and reported
  (they indicate genuine reorderings, e.g. T4 of a later op before T3 of an
  earlier one), never clamped to zero.
- Coverage accounting: the dump reports count(T0), count(T3), count(T4),
  count(T5), arena overflow count, and per-event join success. Join failure
  > 5% invalidates attribution (report as incomplete trace).

## 6. Measurement-lock discipline

- Every session (including the on/off check) holds exclusive flock on
  `/home/jnhu/sluice-595-e5/MEASUREMENT.lock` for its full duration, with
  LOCK_RECEIPT entries per R4 convention (UTC acquire/release, `fuser -v`
  on the lock, second-fd exclusivity probe must block, pre/post state JSON
  with cpu_busy_100ms for CPUs {2,12}, governor, `/tmp` free, loadavg;
  `env-context.md` §9). No parallel builds/compression during timed phases.
- Environment invalidation: same criteria as R4 PREREG §10.1 (governor
  change, foreign load on cpu2/cpu12, loadavg > 4, /tmp < 1 GiB, lock not
  acquirable in 1800 s).
- If the r3-era lock `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock` is held
  by another session, avoid all heavy activity regardless of the R5 lock
  (record the observation; do not clear foreign locks).

## 7. Reporting obligations

Output (English) to `/home/jnhu/sluice-595-e5/r5/analysis/tail-phase-decomposition.md` with: exact commands, binary sha256,
env, lock receipts, per-interval percentile tables per arm/shape, tail-op
phase membership (§1 check 1), burst spreads (§1 check 2), on/off perturbation
check result, coverage accounting, and explicit limitations: no claim of a
complete p99 causal decomposition; results are mechanism-attribution evidence
for the interval partition only, and the kernel-side instants remain
unavailable.
