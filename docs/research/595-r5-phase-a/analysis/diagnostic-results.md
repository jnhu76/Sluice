# #595 R5 Phase A — Tail-Latency Phase-Decomposition Diagnostic Results

Executed 2026-11-06T15:54Z–16:04Z on the native E5 host under the frozen
protocol `r5/analysis/diagnostic-protocol.md` (FROZEN BEFORE COLLECTION). This
file is the protocol §7 report; the protocol names it
`tail-phase-decomposition.md`, the round instruction names
`diagnostic-results.md` — this file follows the round instruction (deviation
recorded). Companion inputs: `r5/analysis/env-context.md` (environment,
lock convention), `r5/analysis/tail-evidence-review.md` (motivation).

## 0. Headline verdict

1. **Trace coverage is complete**: 480,000/480,000 ops fully joined
   (T0..T5), **zero** join failures, zero arena overflow, across all 24
   on-mode measured phases (§5).
2. **The pre-registered on/off perturbation check FAILED its acceptance
   window** (§4.2): off-mode R−A0 p99 delta **+15.18%** vs on-mode
   **+18.57%** (ratio of per-run means) / **+19.14%** (mean of per-pair
   deltas) → gap **+3.39pp / +3.96pp**, window ±3pp. Per the frozen rule,
   **all on-mode numbers in this report are PERTURBED diagnostics and the
   protocol does NOT attribute the tail gap to any interval.** Everything
   below §4 is a lead, not a decomposition.
3. Direction-level lead (joint, exactly additive interval means on the
   pooled top-1% ops, d16): R's tail ops carry **+20.2 µs in `prep` (T1−T0)
   and +3.2 µs in `submit` (T2−T1)** relative to A0 tail ops, partially
   offset by **−14.5 µs in `observe` (T5−T4)** and −3.9 µs in `kernel`
   (T3−T2); net +4.9 µs. The largest interval gap appeared in
   **prep/submit (submission-side serialization of restart refills)**.
4. Tail-op restart concentration is confirmed at the refill level but **not
   via `cap` re-entries**: 98.6% of R d16 tail ops were submitted at
   `caller_window=0`, yet only 7.7% were members of `stop=cap`
   presentations (body: 47.5%) — R tail ops sit in the *second*
   (`input_exhausted`) presentation of restart refills, whose T0 was
   stamped at refill-build time and whose T1 lands after the whole first
   batch was admitted and entered.
5. **Hkernel-a (intra-batch completion straggle) is refuted at the T3
   level**: T3 spread within R's d16 8-op presentations is p50 1.03 µs /
   p99 1.34 µs (max 50.2 µs, ~0.1% of presentations).
6. The d4 control **reproduces the R4 sign flip on-mode**: R−A0 p99
   **−9.5%** (ratio of means) / **−9.7%** (mean of paired deltas) vs R4's
   frozen −12.92% (`r5/r4-archive/TAIL-LATENCY.md:73`). Lead mechanism: on
   d4 tail ops R pays +10.7 µs in `submit` but gains −7.9 µs `kernel` and
   −5.9 µs `observe` (net −2.3 µs).

## 1. Provenance

- Diag branch `research-595-r5-diag`, worktree
  `/home/jnhu/sluice-595-e5/r5/worktree-diag`, baseline `f3b25329`
  (= frozen R4 anchor), instrument commit **`21374a40`**
  ("async+bench: per-op tail-phase trace instrument"). Worktree clean after
  commit; the frozen research worktree `/home/jnhu/sluice-595-e5/r5/worktree`
  was not modified by this diagnostic (its branch advanced f3b25329 →
  `b191bb89` during the round from *other* R5 agents' commits; observed,
  not touched; porcelain 0 whenever checked).
- Build (distinct from any formal build; output only under
  `/tmp/r5-build-taildiag`):
  `xmake f -y -m release --liburing=y --r5_tail_trace=y --builddir=/tmp/r5-build-taildiag`
  then `xmake build -y bench_w03_external_loop`.
  Binary `/tmp/r5-build-taildiag/linux/x86_64/release/bench_w03_external_loop`,
  sha256 **`78a228f267005a8f9638f0249a1f0fc1ae01e599f4a029968c0a8d8fdba4a3ba`**
  (recorded per run in `r5/raw-diag/matrix/manifest.json`; no rebuild during
  any timed session). Note: the xmake option flag is `--r5_tail_trace=y`
  (underscores); a first attempt with `--r5-tail-trace=y` failed with
  "Invalid option" and that config error predates every timed run.
- Instrument (diag branch only; env-gated `SLUICE_595_R5_TAIL_TRACE=1`,
  default off; no public header change, no new thread/timer):
  - `include/sluice/async/detail/r5_tail_trace.hpp`,
    `src/async/r5_tail_trace.cpp` — arena (2 × `kInstrumentedSampleCap` =
    400,000 records; 88 B each), counters, binary dumper.
  - Event anchors (diag-branch line numbers): T0
    `bench/bench_w03_external_loop.cpp:423` (adapter path) and `:509` (A0
    path); T1 `src/async/uring_backend.cpp:575` (batch admit loop), `:501`
    (single path), `:776` (A0 `submit_request`); T2 batch
    `src/async/uring_backend.cpp:608`, single
    `src/async/uring_transport.cpp:95`; T_wake
    `bench/bench_w03_external_loop.cpp:214`; T3
    `src/async/uring_completion.cpp:153`; T4
    `src/async/uring_backend.cpp:859`; T5
    `bench/bench_w03_external_loop.cpp:589`. Cookie stamped in
    `src/async/uring_transport.cpp:207`. All line numbers are diag-branch
    post-commit positions; the frozen-tree anchors from protocol §3 were
    verified before implementation.
  - Dump: 168-byte header (magic 0x5441494C54524143, version 1, record
    bytes 88, n_records, 18 counters) then records
    `u32 seq, slot, generation, presentation; u64 cookie; u32 flags, pad;
    i64 t0,t1,t2,twake,t3,t4,t5`. flags: stop-kind bits 0..2
    (0 plain single, 1 input_exhausted, 2 rejected, 3 cap, 4 sq_budget,
    5 control, 6 recovery, 7 ineligible), caller_window bits 3..10.
  - Known harness quirk: the instrumented subphase's diag tag does not
    include warmup/measured (`bench_w03_external_loop.cpp:779-781` vs
    `:680-684`), so per-process dump index disambiguates: with
    WARMUP=1 MEASURED=1, `tail-000-*` = warmup instrumented phase,
    `tail-001-*` = measured instrumented phase (the one analyzed).
- Run driver: `r5/raw-diag/scripts/run_diag_matrix.py`; analyzer:
    `r5/raw-diag/scripts/analyze_tail_trace.py` +
    `aggregate_tail_trace.py`; outputs `r5/raw-diag/onmode-trace-analysis.json`,
    `r5/raw-diag/aggregate-summary.json`, per-run artifacts under
    `r5/raw-diag/matrix/{off,on}-W03-uring-4k-d{16,d4}/pNN-pos-ARM/`
    (bench.jsonl, bench.log, bench.err, attempt.json, dump/).
- Environment: native E5 (env-context §1), governor schedutil, taskset cpu2,
  `SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=1 SLUICE_BENCH_STATS=0
  SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-r5diag`, arms via
  `SLUICE_595_R3_ARM` (R4 MAIN parity, `r4/raw/matrix/MAIN-W03-D16/block-00.json`
  invocation env).

## 2. Sessions and measurement-lock receipts

All lock sessions used exclusive flock on
`/home/jnhu/sluice-595-e5/MEASUREMENT.lock` (R5 round constraint;
env-context A1) via `r5/raw-diag/scripts/lock_session.sh`; receipts in
`r5/raw-diag/LOCK_RECEIPT.md`. Sessions: `r5diag-A-build-smoke` (build +
A0/R functional smoke, 15:54:34Z–16:03:11Z, 10 sub-sessions, includes
build), `r5diag-B-matrix` (timed matrix, **16:04:26Z → 16:04:40Z**, rc=0,
loadavg 0.11 pre / 0.67 post, /tmp 33.4 GB free, governors schedutil,
cpu2/cpu12 idle). Second-fd exclusivity probes all blocked as required
(rc=1). Two receipt defects recorded honestly in the receipt file: the
in-wrapper post-release probe is self-blocked (wrapper still holds fd 9;
clean release verified externally, rc=0), and `cpu_busy_100ms` rendered
{} due to a sampler keying bug (loadavg/governor//tmp captured). Lock file
size stayed 0. No parallel builds/compression ran during session B; the
parallel R5 agents' `r5/raw-diag/equiv` activity predates 15:54Z.

## 3. Run plan executed (protocol §2)

- Shapes: `W03-uring-4k-d16` (primary), `W03-uring-4k-d4` (control).
- Arms: A0, R only. 24 measured phase executions (2 shapes × 2 arms × 3
  blocks × 2 interleaved orderings A0→R / R→A0), each a fresh pinned
  process with WARMUP=1 MEASURED=1; plus the §4.2 off-mode A0/R pair on d16
  (run first in the session; the protocol fixes the pair but not its
  position — recorded decision). 26 invocations, all exit 0, no retries.
  24/24 planned phase executions completed → the §2 degradation clause for
  incomplete runs does not trigger; §4.2's perturbation clause does (§0.2).

## 4. On/off perturbation check (protocol §4.2) — FAILED

d16, R−A0 p99 of the harness instrumented subphase (jsonl `lat_p99_ns`):

| quantity | value |
|---|---|
| off-mode pair delta | **+15.18%** (A0 52,775 ns; R 60,786 ns) |
| on-mode, ratio of per-run means | +18.57% (A0 54,412.5; R 64,516.5; n=6,6) |
| on-mode, mean of per-pair deltas | +19.14% (pairs: +26.28/+16.85/+23.80/+26.69/+16.70/+4.53%) |
| gap (ratio-of-means − off) | **+3.39pp** |
| gap (mean-paired − off) | **+3.96pp** |
| acceptance window | within +3pp, same sign as R4 (+21.85%) |

Sign condition holds (all positive); the magnitude condition fails under
both aggregations. **Consequence per the frozen protocol: on-mode numbers
are reported as perturbed; the protocol does not attribute.** The
instrument inflates R's p99 more than A0's (R on-vs-off p99 64,516 vs
60,786 = +6.1%; A0 54,412 vs 52,775 = +3.1%), i.e. a differential cost of
~3–4pp — larger than R4's read-and-record precedent (+1.22% p99,
`r4/RESULTS.md:182`). The single-pair off-mode reference is itself noisy
(one A0 rep ranged 51.1–62.4 µs across the six on-mode pairs); the protocol
did not pre-register more off-mode reps, and none were added post hoc.

## 5. Coverage accounting (protocol §5)

Across the 24 on-mode measured phases (6 per arm per shape):
records 480,000; `t0=t1=t2(cookie+cqe+published+taken)=480,000` each
(t2_keys 239,988 + t2_single 240,012); wake epochs 86,576; overflow 0;
join failures 0 (t1/t2/cookie/cqe/published/taken all 0); seq-out-of-range
0; presentations 285,022. Join success 100% ≥ 95% everywhere. Identity
triple (slot, generation, cookie) present on every record; the library-side
join is generation-checked, bench-side join is sequence-indexed (never slot
alone). Per-file counters: `r5/raw-diag/onmode-trace-analysis.json`.
Negative differences (kept, never clamped): `Tw−T2 < 0` (inline
completions, CQE visible before enter returned) — d16: A0 18,660/120,000
(15.6%), R 4,368 (3.6%); d4: A0 28,166 (23.5%), R 18,475 (15.4%).

## 6. Interval tables (PERTURBED — leads)

Pooled percentiles over all 120,000 ops per arm/shape ("pooled p99" =
percentile of the mixed distribution; distinct from "run-level mean of
per-run p99", both below; the bench's own per-run p99 means match the
trace `T5−T0` run-level means exactly, e.g. d16 A0 54,412.5 ns).

W03-uring-4k-d16, ns:

| interval | A0 p50 | A0 p95 | A0 p99 | R p50 | R p95 | R p99 | Δp99 (R−A0) |
|---|---|---|---|---|---|---|---|
| prep (T1−T0) | 142 | 169 | 202 | 1,257 | 5,306 | 25,432 | **+25,230** |
| submit (T2−T1) | 772 | 5,908 | 8,001 | 3,269 | 4,505 | 16,103 | **+8,102** |
| kernel+wake (T3−T2) | 27,194 | 34,267 | 37,401 | 24,459 | 28,858 | 35,074 | −2,327 |
| reap→publish (T4−T3) | 2,111 | 2,400 | 2,523 | 2,146 | 2,460 | 2,582 | +59 |
| observe (T5−T4) | 7,319 | 10,974 | 29,341 | 6,204 | 12,872 | 16,763 | **−12,578** |
| **total (T5−T0)** | 37,681 | 46,397 | 55,035 | 39,023 | 48,200 | 64,604 | **+9,569** |
| wake part (Tw−T2) | 23,730 | 32,643 | 35,405 | 21,779 | 26,803 | 32,903 | −2,502 |
| reap part (T3−Tw) | 2,805 | 36,408 | 38,093 | 2,631 | 3,781 | 19,971 | −18,122 |

Run-level mean of per-run p99 (d16): total A0 54,412.5 / R 64,516.5;
prep 195.3 / 25,391.2; submit 7,978.8 / 16,064.2; kernel 38,639.0 /
34,922.5; reap 2,627.5 / 2,577.2; observe 29,481.0 / 16,915.0.

W03-uring-4k-d4, ns (pooled p99 A0 | R):

| interval | A0 p99 | R p99 | Δp99 |
|---|---|---|---|
| prep | 204 | 579 | +375 |
| submit | 7,870 | 12,865 | +4,995 |
| kernel+wake | 17,043 | 10,689 | **−6,354** |
| reap→publish | 737 | 654 | −83 |
| observe | 12,430 | 5,896 | **−6,534** |
| total | 29,511 | 26,022 | **−3,489** |

Pre-registered attribution rule applied mechanically (d16): the total pooled
p99 gap is +9,569 ns; intervals whose p99 arm delta carries ≥50% of the
gap: prep (+25,230; 264%), submit (+8,102; 85%) — and observe exceeds 50%
with the *opposite* sign (−12,578; −131%). More than one family crosses 50%
in opposing directions, so the "exactly one family dominates" expectation
fails; combined with §4 the formal result is **perturbed / not attributed**
and the joint view below is the lead.

## 7. Joint phase decomposition of the tail (lead)

Exactly additive interval means (ns) on pooled bands (mean of a sum = sum
of means; "tail" = top 1% by total T5−T0, n=1,200; "mid40-60" and "body"
shown to avoid slow-sample-only bias):

d16 tail ops:

| interval | A0 mean | R mean | Δ (R−A0) |
|---|---|---|---|
| prep | 190.9 | 20,437.7 | **+20,246.8** |
| submit | 4,293.2 | 7,473.1 | +3,179.9 |
| kernel | 33,335.0 | 29,461.9 | −3,873.1 |
| reap | 2,162.2 | 1,971.7 | −190.5 |
| observe | 24,909.9 | 10,441.6 | **−14,468.3** |
| total | 64,891.2 | 69,786.1 | +4,894.9 |

d16 mid40-60: total 37,678.5 (A0) vs 39,062.3 (R), +1.4 µs — the body-wide
shift R4 already reported (p50 +4.75%); body means 37,808.7 vs 39,517.6.
d4 tail ops: total 35,616.1 (A0) vs 33,322.1 (R), **−2,294.0**;
submit +10,709.0, kernel −7,851.0, observe −5,933.4.

Reading (lead, perturbed data): R's d16 tail time is created on the
submission side of restart refills — the back ops of a 16-op refill wait in
`prep`/`submit` while the refill's earlier presentations are admitted,
prepared as SQEs and entered (T0 is stamped at refill build for all 16) —
while R's publish→observe tail is *shorter* than A0's, whose own tail is
observe-dominated (24.9 µs mean: window-front blocking behind unpublished
earlier ops). At d4 the same submission-side cost is outweighed by shorter
kernel and observe intervals, producing the sign flip.

## 8. Secondary pre-registered checks

1. **Tail-op phase membership** (d16, pooled top 1%):
   - R: stop-kind 1,108 `input_exhausted` / 92 `cap` (cap share **7.7%**;
     body cap share 47.5%); caller_window=0 for **98.6%** (body 93.9%).
   - A0: caller_window=0 for 36.6% of tail ops (body 5.7%) — A0's tail also
     concentrates at refill moments, but far less sharply.
   - R4's SNP reading "tails concentrate at restart presentations" is
     supported at the restart-refill level and *sharpened*: within a
     restart, the tail sits in the second (`input_exhausted`) presentation,
     not the `cap` re-entry. d4 R: 100% `input_exhausted` at cw=0.
2. **Burst structure** (T3 spread within a presentation, ≥2 ops, keyed by
   run+presentation): d16 R 8-op batches (14,985/15,010 ≥2-op
   presentations are size 8; ≈2,502 presentations/run, matching R4's
   2,508): spread p50 1,028 / p95 1,272 / p99 1,335 / max 50,240 ns. d4 R
   (size-4 batches): p50 401 / p99 463 ns. A0: no multi-op presentations.
   Hkernel-a's predicted wide 8-batch straggle is absent at T3; the rare
   ~50 µs max spread (≤0.1% of presentations) is the only kernel-side tail
   candidate visible.
3. **Control shape**: on-mode d4 R−A0 p99 −9.5%/−9.7% (§0.6) reproduces
   the R4 sign flip (−12.92%) with the instrument on; per-pair spread is
   wide (+8.3%/+5.3% in 2 of 6 pairs) — single-session noise.

## 9. Limitations (explicit)

- **No claim of a complete p99 causal decomposition.** The §4.2 acceptance
  failed; all interval numbers are perturbed-mode leads (instrument
  differentially inflates R's p99 by ~3–4pp).
- Kernel-side instants (SQE consumption, CQE write) are unavailable by
  construction; `T3−T2` = enter-returned → reaped, decomposed only by the
  driver's epoll return (`Tw`), which can precede T2 (negative `Tw−T2`,
  §5) for inline completions; deferred driver-side transport retries would
  also fold into this interval.
- One session, one host, 6 pairs per arm per shape, single-rep phases; the
  off-mode reference is a single pair. p50/p95 off-mode deltas (−4.9%/+3.7%
  off vs +3.7%/+2.1% on) are within ordinary single-rep noise.
- The instrument's T3/T4 stamps are taken under `dispatch_mtx_`
  (`src/async/uring_completion.cpp:100`, `uring_backend.cpp:864-880` poll
  path); their cost is part of the measured perturbation.
- No ECDF plotted: per-request data exists only for on-mode runs, and the
  perturbed mode does not warrant distributional claims; off-mode has no
  per-request records by design.
