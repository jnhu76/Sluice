# #595 R5 Phase A — Tail (p99) Evidence Review

Role: p99 evidence inventory (task §6). Written 2026-11-06 on the native E5
host. Everything below was read or executed in this session; source citations
are `path:line` against the frozen R4-original source at
`/home/jnhu/sluice-595-e5/r5/worktree` (HEAD `f3b25329bc8a…`, verified in
`env-context.md` §2). R4 evidence under `/home/jnhu/sluice-595-e5/r4/` was
read only; nothing there was modified. No measurements were taken in this
session (no lock session; only non-timed reads and Python re-analyses of
frozen R4 raw data).

Deliverables of this review: this file, plus
`/home/jnhu/sluice-595-e5/r5/analysis/diagnostic-protocol.md` (frozen design
for the one diagnostic this review concludes is necessary; not executed).

---

## 1. R4 evidence inventory — what exists and at what observational level

Every artifact below was opened this session.

| Artifact | Contents | Observational level |
|---|---|---|
| `r4/raw/matrix/MAIN-W03-D16/block-*.json` (24 blocks × 8 arms) | per invocation: metrics (p50/p95/p99_ns, cpu_ns_per_op, ops_per_sec, wall, `r3_*`/`r4_*` decision counters, `submits`), per-rep `reps[]` (n=20000: `lat_p50/p95/p99/max/mean/stddev_ns`, `notifications`, `passes`, `empty_wakes`), env pre/post (cpu_busy, freq, loadavg, tmp), `irq_delta` per CPU, `ru_*` incl. `ru_nvcsw`/`ru_nivcsw`, CPU tick buckets (10 ms granularity) | invocation + rep summaries; **no per-request records, no phase timing** |
| `r4/raw/matrix/CORE-W03-{D4,D64,D2SERIAL}`, `CORE-W02-*`, `MAIN` others | same schema for the depth sweep / W02 cells | same |
| `r4/raw/matrix/TRAJECTORY/` (+ `attempts.jsonl`) | 0/24 valid; SHADOW/T1 tripped the frozen `shadow_outstanding_sum==0` oracle (structural zero under d2serial: every decision point has exactly 1 op in flight) | decision records only |
| `r4/raw/diagnostics/*.diag` | per-decision lines for S and SHADOW on W03-D16 and T1–T3: `ready/eligible/present/quantum/row/active(outstanding)/pressure/pass_completed/caller_window/accepted/stop/cause` (`bench/r3_ready_batch.hpp:337-356` formatter) | decision-time snapshots, **no timestamps** |
| `r4/raw/extension/DIAG-64K-STRACE/e64k-{A0,R,S,Q8}.strace` | `strace -f -ttt -e trace=io_uring_enter`, µs timestamps, 64 KiB shape only | syscall-level timing, **wrong shape** (64k reads are batch-ineligible → immediate path; `batch_eligible_read` requires len==4096, `src/async/uring_backend.cpp:437-439`) |
| `r4/analysis/tail-latency-p50-p95-p99.csv` (42 rows) + `MAIN_SUMMARY.json` + `extension/EXTENSION_SUMMARY.json` | frozen per-cell/arm percentile aggregates with block min/max | derived summaries |
| `r4/RESULTS.md`, `r5/r4-archive/TAIL-LATENCY.md` | frozen gates, arm tables, CPU buckets, §13 NOT_RUN list, TAIL §6 mechanism candidates | conclusions |
| `r4/logs/w03_smoke_*.log`, `r4/raw/matrix/*/logs/b*.log` | per-rep bench stdout lines (same numbers as reps[]) | rep summaries |
| A/A cells `r4/raw/aa/` | A0-vs-A0 reproducibility: p99 half-width 2.10 pp on W03-D16 (`r4/RESULTS.md:329`) | noise floor |

Explicitly absent (verified by search): per-request latency vectors (discarded
after `compute_stats`, `bench/bench_w03_external_loop.cpp:787`); any
enter→CQE or reap/publish timing on the 4 KiB shape (R4 §10.5 enter/CQE
reconciliation NOT_RUN, `r4/RESULTS.md:342`); PMU/IRQ attribution
(`perf_event_paranoid=2`, `r4/RESULTS.md:439`); ns-level entry→enter
observation (stripped binaries, `r4/RESULTS.md:439`).

## 2. Observable-event semantics bound to source (frozen tree)

W03 harness topology: one **submitter** thread (builds ReadOps, presents them,
parks on a condition variable, drains results strictly in window order) and
one **driver** thread (progress owner; parks in `epoll_wait` on the backend
eventfd; reaps and publishes). Setup: `bench/bench_w03_external_loop.cpp:182`
(driver), `:281` (phase loop).

Event partial order per request (submitter thread S, driver thread D):

1. **Descriptor ready / ReadOp built** — `bench_w03_external_loop.cpp:404-413`
   (adapter arms) and `:482-491` (A0). This is the latency origin `ready_ns`
   (comment at `:370-374`). In W03 there is no separate "application
   dependency satisfied" event: ops are unconditionally ready when built, so
   these two names denote the same instant here and must not be conflated in
   general.
2. **Prepare/adapter begin-end** — arm policy evaluation and presentation
   bounding `bench/r3_ready_batch.hpp:442-525`; group preparation
   `batch_shaped_prefix` `:189-196`; entry `TestAccess::context_submit_ready_reads`
   (`include/sluice/async/detail/test_access.hpp:49`, takes `ctx.access_mtx_`
   at `:69`) or per-op `AsyncIoContext::submit_read`
   (`src/async/async_io_context.cpp:245`, takes `access_mtx_` per op).
3. **Accept** — `admit_`: validate → `core_->reserve()` → `prepared_ops_`
   write → delivery reset → `core_->accept` at
   `src/async/uring_backend.cpp:634-733` (accept call at `:726`). The slot is
   now bound (`binding_live`); `RequestReservation{slot, generation}` is the
   identity from here on.
4. **SQE preparation** — `dispatch_one_locked`:
   `claim_execution` (`src/async/uring_transport.cpp:157`), `io_uring_get_sqe`
   (`:146`), `io_uring_prep_read` (`:173`), `user_data = op_cookie`
   (`:194`), router entry + transport-ledger append (`:195-204`).
5. **Enter (flush)** — `submit_transport_locked` → `::io_uring_submit`
   (`src/async/uring_transport.cpp:223-240`). Cadence per arm on W03-d16
   (from `r4/RESULTS.md:226-233`): A0 one enter per op; R one per ≤8 ops
   (2,508/rep); S/Q2 one per 2 ops; all flushed under `dispatch_mtx_`
   (batch path `src/async/uring_backend.cpp:554-582`; single path
   `src/async/uring_transport.cpp:84-93`).
6. **Kernel consumes SQE / executes IO** — inside `io_uring_enter`; **not
   independently observable from userspace** (record as *unavailable*; any
   statement about it must come from CQE-relative timing or kernel tracing,
   neither of which exists for this shape). Note completions on tmpfs can
   happen inline during the enter, so a CQE may exist **before** the submit
   call returns; `submit_return` is therefore not a valid "kernel-phase
   start" for all ops.
7. **CQE kernel publication** — kernel posts the CQ entry and writes the
   registered eventfd (`io_uring_register_eventfd`,
   `src/async/uring_backend.cpp:284`). First userspace visibility: driver's
   `epoll_wait` return (`bench_w03_external_loop.cpp:209-217`) and
   `io_uring_cq_ready()` at reap (`src/async/uring_completion.cpp:229`).
8. **Userspace reap** — driver pass loop `bench_w03_external_loop.cpp:231-245`
   → `poll_progress` (`src/async/uring_backend.cpp:312-336`) → `poll()`
   (`src/async/uring_backend.cpp:839-912`) → `reap_cqes` (bounded peek of 32,
   bound = CQ state at entry; `src/async/uring_completion.cpp:200-229`) →
   per-CQE `handle_one_cqe` **takes `dispatch_mtx_` per CQE**
   (`src/async/uring_completion.cpp:100`).
9. **Terminal + execution retirement** — `offer_terminal` then
   `release_execution`, then `publication_pending_->push_back`
   (`src/async/uring_completion.cpp:70-93`). Condition: terminal chosen and
   execution refs zero.
10. **Publication (caller-first-observable)** — driver drains
    `publication_pending_` up to the pass bound
    (`src/async/uring_backend.cpp:861-880`), `publish_one` →
    `begin_publication` (`:806-807`) → `complete_publication` sets
    `published = true` (`src/async/detail/request_core.cpp:478`). From this
    instant `Request::ready()` is true
    (`include/sluice/async/request.hpp:130-132` via
    `RequestCore::lookup`, `src/async/detail/request_core.cpp:251-258`).
    Ordering vs (8): terminal (9) strictly precedes publication (10);
    publication can lag CQE reap by the rest of the pass.
11. **Driver publishes completions to the harness** — under `shared.mutex`:
    `completions += batch`, `cv.notify_all`
    (`bench_w03_external_loop.cpp:246-264`).
12. **Submitter wake + in-order observation + take_result** — submitter waits
    for `completions > watermark` (`bench_w03_external_loop.cpp:522-539`),
    then drains the window **front-ready prefix only**
    (`:541-572`): `window.front().request.ready()`, `take_result()` at
    `:543` (`request.hpp:146-160`), latency end stamped at `:565-567`
    (`now_ns() − front.ready_ns`). A published request that is not at the
    window front is unobservable until every earlier item is published —
    this is the structural Hobserve latch. Application in-order delivery is
    the same instant here (single consumer pops in sequence order).

Additional driver-side events: `acknowledge_progress_notification` drains the
eventfd (`bench_w03_external_loop.cpp:213`,
`include/sluice/async/detail/progress_source.hpp:274-278`,
`:434-452`); backend-originated wakes (zero-op publish, residue advertisement,
poison) also write the eventfd via `signal_ready_progress`
(`src/async/uring_backend.cpp:614-617`,
`include/sluice/async/detail/progress_source.hpp:252-263`).

**Request identity mapping.** Bench sequence → `slot = sequence % depth`
(buffer slot only; `bench_w03_external_loop.cpp:406/464`) →
`RequestReservation{slot, generation}` → `RequestKey{context, slot,
generation}` (`src/async/uring_transport.cpp:156`) → `op_cookie`
(monotonic `next_cookie_`, `:169`, `include/sluice/async/detail/uring_backend.hpp:350`)
→ SQE `user_data` → `RouterEntry{cookie, handle}` (`:195-199`) → back via
`find_live_router_cookie_` (`src/async/uring_transport.cpp:556-562`).
Generations separate slot reuse (`resolve_public_` fails stale lookups,
`src/async/detail/request_core.cpp:251-258`); control (cancel) CQEs carry a
tag-disjoint cookie (`src/async/uring_internal.hpp:48-56`), so data/cancel
confusion is structurally excluded. Any future sampling instrument must key on
(slot, generation, cookie) — slot alone is unsafe because `prepared_ops_[slot]`
is rewritten on the next admit after reclaim.

**Clocks.** All latency timestamps are `std::chrono::steady_clock`
(`bench/bench_common.hpp:25-34`); origin and end are both taken on the
submitter thread, so the existing latency number has no cross-thread or
cross-clock skew. Percentiles via `quantile_sorted` on n=20000
(`bench/bench_common.hpp:96-118`). Process CPU uses
`CLOCK_PROCESS_CPUTIME_ID` (`bench_common.hpp:43-50`). Note the latency
statistics come from the harness's **third** subphase (instrumented run,
`bench_w03_external_loop.cpp:779-787`), distinct from the timed
throughput/CPU phase — same conditions, both arms identical in structure.

## 3. Fresh re-analyses of frozen R4 raw data (commands run this session)

Python over `r4/raw/matrix/MAIN-W03-D16/block-*.json` (all 24 blocks, 5
measured reps each, 120 rep-records per arm):

1. Driver cadence per arm (mean over reps): passes = notifications for every
   arm (each driver iteration is one epoll wake); A0 1650, R 1290, Q8 1292,
   SHADOW 1293, S 1380, Q2 1376, SNI 1371, SNP 1362 per 20,000 ops. R wakes
   21.8% less often than A0; S wakes 6.9% less than A0 but 7% more than R.
2. Distribution shape (`lat_*` means, ns): A0 p50/p95/p99/max =
   36864/45118/51942/98773; R = 38613/46315/63196/**209153**; Q8 max
   **416760**; SHADOW max **576852**; SNP max **314790**; while Q2/S/SNI max
   = 111609–132899 with max/p99 ≈ 1.87 like A0's 1.90. So the 8-batch arms
   stretch specifically above p95 (p99−p95: 6.8 µs → 16.9–19.1 µs) **and**
   carry rare extreme outliers (210–577 µs) that A0 (99 µs) and the 2-batch
   arms never show; the 2-batch arms shift the whole body right instead.
3. Per-block paired correlations (24 blocks): corr(d_p99, d_passes) = −0.057
   (R), −0.217 (Q8), +0.219 (S), +0.073 (Q2), −0.078 (SNP) — block-level p99
   deltas are not explained by wake-count differences; corr(d_p99, d_tput) is
   weakly positive for the 8-batch arms (R +0.109, Q8 +0.278, SNP +0.251).
4. Scheduling/IRQ per invocation: `ru_nivcsw` A0 57476 vs R 26589 (R has
   *fewer* involuntary switches); `ru_nvcsw` 87588 vs 55065; `irq_delta` on
   CPU2 ≈ 1159–1387 for all arms (no arm signal). All phases pinned to
   `cpu_set='2'` with max_user_threads=3 (submitter+driver+iou-wq workers on
   one CPU).
5. Decision-trace structure (from `r4/raw/diagnostics/W03-D16__SHADOW__S.diag`,
   7,504 decisions, action ≡ R): `outstanding()==0` at 4,188/7,504 (55.8%) of
   decisions; **all 3,425 `stop=cap` truncations except one occur with
   `outstanding()==0` and `caller_window==0` and `ready=16`** — the cap fires
   exactly on full-window restart bursts after a drain. `input_exhausted`
   presentations (4,079) have `ready=8`, `outstanding∈{8 (3,308), 0 (764)}`.
   S's trace (`W03-D16__S__S.diag`, 29,998 decisions): `pass_completed=16` at
   75% of decisions; outstanding spread evenly 0–14 as its quantum-2 chunking
   walks each refill.

## 4. What existing evidence already answers

- The regression is p99-concentrated for 8-batch arms and body-wide for
  2-batch arms (§3.2) — R4's "p99-only" reading confirmed with shape data.
- It is not the cost of reading state: SHADOW−R p99 +1.22% / CPU +1.02%
  (`r4/RESULTS.md:182`); not block-level wake-count variation (§3.3); not
  gross measurement noise (A/A p99 half-width 2.10 pp vs +21.85% effect,
  `r4/RESULTS.md:329`).
- Restart/drain phases are structurally special: R's cap-truncations sit
  exactly at `outstanding()==0` full refills (§3.5); SNP (small batches only
  at zero-outstanding moments, 35% of presentations) reaches S-level p99
  while uniform Q8 stays at +23% (`r5/r4-archive/TAIL-LATENCY.md:50-65`) —
  the tail is dominated by what happens at/after drain restarts, not by
  average batch size.
- Batching helps the tail when completions are slow (btrfs R p99 −30.43%,
  `r5/r4-archive/TAIL-LATENCY.md:95`) and at low depth (d4 R p99 −12.92%,
  `:73`); hurts at d16/d64 on fast completions.
- CPU attribution at 10 ms granularity: submitter 38–46%, io-wq 32–36%,
  driver ≤0.5%, residual 22–26% (`r4/RESULTS.md:212-224`) — driver-side work
  is not a major CPU consumer, but tick-level buckets cannot see tail events.
- R's restart-burst structure is fully characterized counter-wise (2,508
  presentations, 1,236 cap re-entries per rep; `r4/logs/...b00-p1-R...log`).

## 5. Six hypotheses — predicted evidence, refutation, minimal intervention

**Hprep — submission-side preparation cost (repeated scanning, group
preparation, lock acquisition before the kernel enter).**
If true: latency added before the enter, paid by every op in a presentation;
uniform body shift proportional to presentations/op. Refuted as the *sole*
cause for R by shape: R's p50 +4.75%/p95 +2.71% vs p99 +21.85% — a
presentation is 1/8 of ops for R, so prep cost lands at most ~⅛-weighted on
the body; it cannot produce a p99-only +22% nor 200–577 µs outliers. S
(4× R's presentation density) does show the uniform shift (p50 +13%, p95
+33.7%) — Hprep plausibly dominates *S*. Minimal intervention: none needed to
answer; per-op T0→T2 timing (protocol §1) quantifies R's residual share.

**Hkernel — kernel/io-wq execution or scheduling delay.**
If true: the extra tail time sits between enter-return and CQE reap;
8-batches complete in a long straggle (wide intra-batch completion spread);
outliers coincide with io-wq wakeup events; d4/btrfs sign flips follow from
completion-speed interaction. Refuting evidence would be tight intra-batch
spreads and T3≈T2 with the gap living later. Existing evidence is consistent
but not discriminating: io-wq share rises with batching (31.8%→36.1%,
`r4/RESULTS.md:212-224`), R has *fewer* context switches than A0 (§3.4) —
so it is not userspace preemption, but nothing times the kernel side.
**Unresolvable without new data.** Minimal intervention: protocol §1
(T2/T_wake/T3 per op) + intra-batch spread (§1 check 2).

**Hreap — CQE already present but reap/publication deferred.**
If true: gap concentrated between CQE visibility and publication; expect
publication batches clumped at pass boundaries and tails correlating with
back-to-back driver passes. Refuting evidence: T4−T3 negligible. Existing
evidence: structurally possible (bounded 32-CQE reap + pass-bound
publication, `src/async/uring_completion.cpp:200-229`,
`src/async/uring_backend.cpp:861-880`; per-CQE `dispatch_mtx_`
`src/async/uring_completion.cpp:100` contends with the submitter's batch
admission under the same mutex); R wakes 21.8% less often than A0 (§3.1),
so average CQE-sitting time can be longer — but block-level correlation of
d_p99 with d_passes is ≈0 (§3.3). **Unresolvable without new data.**
Minimal intervention: protocol §1 (T3, T4 per op).

**Hobserve — published request observed late by the caller.**
If true: gap concentrated after publication; tail ops are window-front
blocked behind unpublished earlier ops at restart bursts. Refuting evidence:
T5−T4 negligible. Existing evidence: the latch is real (front-only drain,
`bench_w03_external_loop.cpp:541`), and restart bursts make publication
clump in 8s, but no publish/take timing exists. **Unresolvable without new
data.** Minimal intervention: protocol §1 (T4/T5 + tail-op phase membership,
§1 check 1).

**Hfeedback — closed-loop completion→refill feedback reshapes in-flight
distribution.**
Already substantially supported by existing evidence: R4's SNP anomaly
(wrong-moment small batches ≈ S-level p99, `r5/r4-archive/TAIL-LATENCY.md:58-65`);
cap-fires only at `outstanding()==0` restarts (§3.5); arm-dependent wake
cadence (§3.1); d4/d16/d64/btrfs sign pattern. What existing evidence does
NOT establish: *which pipeline stage* the reshaped bursts hurt (that is
exactly the Hkernel/Hreap/Hobserve split above). Minimal intervention: none
for the distributional claim itself; the protocol's tail-op phase membership
check (§1 check 1) confirms or refutes that R's tail ops sit in restart
presentations.

**Hmeasure — boundary/sampling/ordering/instrumentation artifacts.**
Largely excluded by existing evidence: A/A noise 2.10 pp ≪ effect; identical
harness structure per arm with a unified latency origin
(`bench_w03_external_loop.cpp:370-374`); SHADOW−R bounds read-and-record
cost at p99 +1.22%. Residual risks that remain open and are carried forward:
(i) latency stats come from the harness's separate instrumented subphase
(`:779-787`) rather than the timed subphase — same conditions both arms, but
phase-3 dynamics could in principle differ from phase-1; (ii) two
`steady_clock` reads per op add ~tens of ns to both arms equally. Neither can
plausibly produce a p99-only +22% with 24/24 sign consistency. Verdict:
Hmeasure is not the explanation; the protocol's on/off check
(`diagnostic-protocol.md` §4) keeps it excluded when new instruments land.

## 6. REQUIRED_INFORMATION_GAP

No existing artifact contains any timestamp between ReadOp build and
take_result other than the two endpoints, for the 4 KiB shapes. Therefore the
decomposition `T5−T0 = (T2−T0) + (T3−T2) + (T4−T3) + (T5−T4)` —
submission-side / kernel+wake / reap-to-publish / publish-to-observe — is
**entirely unmeasured** for W03-d16, and the three location hypotheses
(Hkernel, Hreap, Hobserve) plus the residual Hprep share cannot be
distinguished by any re-analysis of R4 data (they produce identical
predictions at the rep-summary level; §3.3 shows the available
covariates do not separate them). The kernel-side instants (SQE consumption,
CQE write) remain unavailable to userspace observation by construction and
must be reported as such, not inferred.

## 7. Diagnostic decision

A new, minimal, non-timed diagnostic is **necessary**: existing evidence +
source reading cannot distinguish the key remaining hypotheses (§6), and the
R4 review itself left exactly this open (`r5/r4-archive/TAIL-LATENCY.md:112-130`,
candidates 1–3 map onto Hkernel/Hreap/Hobserve). The design is frozen in
`/home/jnhu/sluice-595-e5/r5/analysis/diagnostic-protocol.md`: A0/R only,
W03-d16 primary + W03-d4 control, 7 preallocated per-op timestamps keyed by
(slot, generation, cookie), steady-clock only, env-gated on the diag branch,
flock + LOCK_RECEIPT discipline, mandatory on/off perturbation check, honest
degradation to "lead only", and an explicit prohibition on claiming a full
p99 causal decomposition from it. The protocol was written before any
collection; nothing has been run.

## 8. Anomalies / caveats found during this review

- R4's diag traces cover only S and SHADOW (`r4/raw/diagnostics/header.json`);
   no R-specific trace exists, but SHADOW acts identically to R
   (`bench/r3_ready_batch.hpp:482-502` presentation override) and its trace
   is used above as R's action record.
- `stats_*` counters are zero in MAIN-W03-D16 reps (`SLUICE_BENCH_STATS=0`,
   block env) — the reps[] have no poll_calls/completed_ops for this cell;
   cadence numbers in §3.1 therefore come from `notifications`/`passes`.
- The measured (timed) phase and the latency-collecting phase are distinct
   executions (§2 note); all latency claims are about the instrumented
   subphase by construction of the harness.
- TRAJECTORY T1's structural zero (SHADOW under d2serial) is a property of
   the frozen oracle, not new information about the tail.
