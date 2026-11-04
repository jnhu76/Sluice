# protocol.md — frozen experiment protocol (round E5 2026-11-04)

The authoritative frozen protocol is the verbatim copy
[`bench/research/uring-submit-policy/round-e5-20261104/PREREGISTERED.md`](../../../../../bench/research/uring-submit-policy/round-e5-20261104/PREREGISTERED.md),
frozen 2026-11-04 **before the first timing run** and never revised afterwards.
This page summarizes it and records the two protocol events that happened
between freezing and unblinding, both documented at the time with receipts.

Nothing below was tuned after results were seen. Thresholds in
[decision.md](decision.md) are the frozen ones.

## 1. Hypothesis and arms

| Arm | Policy |
|---|---|
| **A · IMMEDIATE** | Frozen `271b81bf`: `dispatch_after_accept` flushes each prepared SQE inline (`submit_transport_locked()`); existing control ordering |
| **B · BOUNDED_PROGRESS** | Hold the inline flush only when all four gate conditions hold (see [mechanism.md](mechanism.md)); otherwise identical |

Research question: can a *bounded* progress-driven deferral of
`io_uring_enter` reduce whole-executor CPU per useful task at equal request
semantics, without regressing short queues (D1/D2), external drivers (W-03),
cancellation, overflow/shutdown, or tail latency?

Both arms ship in **one binary per bench program**, selected at backend
construction by `SLUICE_595_ARM` (absent = A, `B` = B). Same code layout, no
binary-layout confound. The selector is research instrumentation, not a public
knob.

## 2. Environment identity (frozen in `platform.txt`)

Xeon E5-2666 v3 (10C/20T Haswell-EP, 1 NUMA), kernel 7.2.11-200.fc44.x86_64,
gcc 16.2.1, liburing 2.13-2, xmake 3.1.2, bare metal (no virtualization), warm
tmpfs `/tmp/sluice-595-bench` (64 MiB pattern fixture), schedutil 1.2–3.5 GHz
(never modified), `perf_event_paranoid=2`. All timed runs pinned `taskset -c 2`
unless stated; SMT {2,12} and cross-physical {2,8} placement checks recorded.
Baseline SHA verified equal to the #622 anchor before any run.

## 3. Measurement design

- **Sampling unit**: 20,000 valid 4 KiB reads per measured rep (64 KiB cell:
  its frozen 2,000), byte oracle verified per rep by the bench itself; every
  invocation must exit 0 — a failed rep invalidates the run entirely.
- **Pairing**: 24 balanced interleaved pairs per shape (even pair = A then B,
  odd = B then A). Pair statistic = relative difference per pair; bootstrap
  95 % CI over pairs, 10,000 percentile resamples, seed 5950. Direction:
  positive = B worse.
- **A/A calibration before any A/B**: 24 A-vs-A pairs on the main shape, gates
  on CI half-widths: CPU ≤ 5 pp, throughput ≤ 3 pp, p99 ≤ 3 pp.
- **CPU metric**: pre-timing determination (below) fixed the primary metric as
  the bench's per-measured-rep process-CPU delta — on this kernel io-wq
  workers are thread-group members, so `CLOCK_PROCESS_CPUTIME_ID` already
  covers the whole executor. Whole-invocation `wait4` rusage is the recorded
  secondary view (must agree in sign where the net-win gate is claimed).
- **Mechanism ledger**: separate runs, never mixed with timing: `strace -f -c
  -e io_uring_enter` per arm/shape, bench `SLUICE_BENCH_STATS=1` counters, PMU
  `perf stat` round (sudo, paranoid=2 noted).
- **Exclusion rule (frozen)**: a rep is invalid only if the bench's own
  oracle/stats gate fails (non-zero exit). No post-hoc outlier removal.
  Audit: 894/894 archived invocations exited 0; nothing was dropped.
- **Negative-control family** (Holm–Bonferroni at family α=0.05, one-sided
  non-inferiority; bounds CPU/p95/p99 ≤ 5 % worsening, throughput ≤ 3 %):
  **nine shapes**: D1, D2, D3, 64 KiB d16, W-03 d4, W-03 d16, W-03 d64,
  threadpool d16, saturation. The D2 replication run is *confirmation
  evidence only* and was not a family member (it was run after D2 failed, to
  confirm the direction, not to re-test the gate).
- **Placement**: W-02 cpu2 vs cpu8 (8 pairs, safety-only); W-03 (2,12) SMT and
  (2,8) cross-physical (8 pairs each, safety-only).
- **Resource gate**: peak RSS ≤ 2 % growth; zero new queue/thread/timer/
  per-request allocation (code audit of the patch).

## 4. Protocol event 1 — pre-timing machinery determination

Taken **before any timing receipt** (receipt: `PREREGISTERED.md` §
"Pre-timing measurement-machinery determination"). A live probe showed
io-wq workers (`iou-wrk-*`) are members of the ring owner's thread group on
this kernel (visible under `/proc/<pid>/task/`; unpinned process CPU 180–235 %
with no user threads). Consequences fixed at the time:

1. The originally registered additive `iou-wrk` /proc delta would
   **double-count** and is not added; the /proc enumeration is kept as a
   one-run-per-shape consistency check (archived: `raw-minimal/threadcheck-w02`;
   per-thread tick sum 1.290 s ≤ rusage 1.315 s).
2. Whole-invocation rusage is dominated by fixture setup (measured-phase CPU is
   ~0.06 s of 2.3–3.1 s), so the **primary CPU metric** is the per-measured-rep
   process-CPU delta; thresholds unchanged.
3. The unfavorable-attribution clause has nothing to shift (single accounting
   source); the pair bootstrap CI upper bound is used as registered.

## 5. Protocol event 2 — the single sanctioned A/A repair

The first A/A calibration (24 pairs, 1 measured rep per invocation) **failed**
the frozen p99 half-width gate: p99 3.71 pp > 3 pp (CPU 2.44 pp, throughput
2.50 pp passed). Diagnostics at the time: no order effect; within-invocation
rep-to-rep p99 sd 6.6 % vs between-invocation sd of a 10-rep mean 2.5 % —
single-rep p99 sampling. Placement probes were recorded as receipts but
rejected as repairs (SMT {2,12}: 5.35 pp; cpu8: 8.05 pp — noise is not
core-specific).

The one sanctioned repair, recorded before any A/B timing: each pair member
runs `WARMUP=1 MEASURED=5`, aggregated per invocation (CPU/op = Σcpu/Σops,
throughput = Σops/Σwall, quantiles = mean of the five per-rep values).
Thresholds, pairing, arm selector, gates and the control family are unchanged.
**Both the pre-repair and repaired receipts are kept**:

| receipt | phase dir (raw-minimal) | result |
|---|---|---|
| pre-repair | `aa-calib-w02-4k-d16` | p99 hw 3.71 pp FAIL |
| rejected alternatives | `aa-calib2-w02-4k-d16-smt`, `aa-probe-cpu8` | recorded, not used |
| repaired | `aa-repair-w02-4k-d16-m5` | cpu 0.44 / tput 0.43 / p99 1.08 pp PASS |
| machinery smoke | `harness-smoke` | 2-pair validation only, enters no analysis |

Per-shape A/A noise floors measured with the repaired machinery: D2
0.53/0.53/1.61 pp, W-03 d4 1.05/1.05/1.88 pp (p95 3.88 pp), W-03 d64
1.10/1.10/7.21 pp (cpu/tput/p99).

The repair is a measurement-condition change, disclosed wherever main-shape
numbers are quoted; it did not touch any threshold or the decision rule.

## 6. Decision gates (frozen; unchanged)

| Gate | Condition |
|---|---|
| Correctness | all mandatory oracles pass; data/result/cancel/lifetime/resource semantics equivalent |
| Mechanism | W-02 4k d16: pure submit enters per valid read reduced ≥ 50 % vs A; SQE/CQE ledger reconciles |
| Net win | whole-executor CPU per useful task: point ≤ −10 %, CI upper ≤ −5 %, at the unfavorable attribution end |
| Performance protection | main: throughput drop ≤ 3 %, p95/p99 worsening ≤ 5 %; each of the nine control shapes: CPU/p95/p99 worsening ≤ 5 %, throughput drop ≤ 3 % (Holm) |
| Resource | zero new queue/thread/timer/per-request allocation; peak RSS growth ≤ 2 % |

Verdicts: `GO_MECHANISM` (passing shapes only) / `KEEP_IMMEDIATE` /
`INCONCLUSIVE`. `PRODUCT = HOLD` this round regardless: #604 has not handed off
`READY_FOR_595` (verified 2026-11-04).
