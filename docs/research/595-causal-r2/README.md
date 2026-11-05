# #595 — D2 adaptive cross-layer causal attribution (causal-r2)

Research record for [issue #595](https://github.com/jnhu76/Sluice/issues/595)
(parent #563). This is round **R2**, an independent re-run and deeper causal
investigation of the D2 (`W02-uring-4k`, depth=2) CPU and latency regression
between `A·IMMEDIATE` and `B·BOUNDED_PROGRESS`.

This branch is a **research record intended as an open research PR for remote
review**. It is not merged. The diff touches only `docs/research/595-causal-r2/`.
No `src/`, `include/`, spec, test or build file changes are proposed.
`PRODUCT_CHANGE: NONE`, `PRODUCT_PR: NONE`, `MERGED: NO`.

R1 (`/home/jnhu/sluice-595-e5/evidence/`, verdict `KEEP_IMMEDIATE`) is untouched;
R2 neither overwrites nor reinterprets R1. Every performance number here was
produced by test-only research arms in isolated worktrees.

## Frozen base and environment

| Item | Value |
|---|---|
| Base SHA | `271b81bfc9d64eecc0266fe7066b9040525dd710` (master, clean at adjudication) |
| Host | E5, bare metal, native Linux |
| CPU | Intel Xeon E5-2666 v3, 10C/20T, 20 logical CPUs |
| Kernel | `7.2.11-200.fc44.x86_64` |
| Toolchain | g++ 16.2.1, xmake 3.1.2, liburing 2.13-2, Python 3.14.8 |
| Pinning / frequency | bench `taskset -c 2`; schedutil 1.2–3.5 GHz (requested); serial exclusive runs |
| Workload | `W02-uring-4k`: 64 MiB warm tmpfs, 20,000 × 4 KiB positional reads/rep |
| Protocol | `WARMUP=1 MEASURED=5`, AB/BA pairing, bootstrap 10,000 (seed 5950); instrumentation blocks separated from timing blocks |

## Verdicts (D2, unless stated otherwise)

| Domain | Verdict | Proposition |
|---|---|---|
| **CPU** | **ESTABLISHED (source level)** | B's **+3.70% CPU/op** is carried by the residue notification of the defer decision chain (`signal_ready_progress()` → eventfd `write`). The five-gate admission check, io-wq, `io_uring_enter`, page-fault/device and scheduler wait do not carry measurable CPU cost. The 82% share is an **upper-bound accounting**, not an exact decomposition. |
| **Latency / tail** | **ESTABLISHED (source level)** | The whole latency distribution shift (p50 +6.91%, p95 +4.96%, p99 +7.07%) is carried by **deferral timing** (L1: the deferred SQE is flushed at the next `poll()`, not at dispatch). Notification domination (L0) is **excluded** by CF-1; L0 residual ≈1 pp is not separately quantified. |
| **Tail component L2** | **PARTIAL** | The p99−p95 extra ≈+320 ns and the segment-level decomposition (G12) are **unobserved**. The ≥96.2% figure bounds the **p99 total effect**, not the p99−p95 extra tail. |
| **Depth boundary (D3/D16)** | **PARTIAL** | D3 −6.69% / D16 −26.36% improvements lie **inside the defer chain** (same `deferoff` mutant binary reduces them to +1.07%/+0.26%), but there is **no restore block** and the mechanism split is not closed (submitter kernel ≈54.8%/50.3%, io-wq 29.1%/21.0% UNOBSERVED, user 16.8%/28.8% indirect). |

### Key numbers

- **phaseA, 24 pairs, uninstrumented**: D2 CPU/op **+3.73%** [+3.04,+4.42];
  p50 **+6.91%**, p95 **+4.96%**, p99 **+7.07%**; throughput −3.58%.
  D1 −0.07% (mechanism off), D3 −6.69%, D16 −26.36%.
- **Cost split (D2)**: user +95.24, submitter kernel +79.37, io-wq +7.94 ns/op;
  `sqes_deferred` 0.5589/op; `flushes` = `actual_enters` = 1.0000/op;
  `signal` 277.5 ns/call; eventfd `write` 592.5 ns/call; syscw +86,912/inv.
- **CF-0 three arms** (A/IMMEDIATE, B/BOUNDED_PROGRESS, C/GATE_ONLY_IMMEDIATE):
  C−A D2 **−0.52%** [−1.28,+0.22], D16 −0.13% [−1.45,+0.82] ⇒ the admission
  check itself has no measurable cost; B−A D2 +3.21%, D16 −26.80%.
- **CF-1 `sigdrop`** (remove only the residue-signal term): CPU frozen
  **+3.34% → −0.26% [−0.67,+0.16]**, latency still **+4.30%/+4.87%**
  (p50/p99) ⇒ latency is not notification-dominated; restore +3.34%.
- **CF-2 `deferoff`** (single-point `return false` at the defer entry):
  CPU **+3.90% → −0.76% [−1.61,+0.06]**; p50/p95/p99 **−0.71/−0.87/−0.83%**
  (zeroed); restore +3.94%. Same mutant binary on D3/D16: +1.07% / +0.26%.
- **Resulting chain**: `O1` defer decision + residue signal → `C1-m` user
  signal → `C3-m` eventfd write → `C5-m` user microarchitectural stalls
  (~68–71 ns/op, CORRELATION_PARTIAL) → submitter CPU +189.15 ns/op;
  `O2` flush timing shift → latency (L1), with L2 unobserved.

## Document index

| File | Content |
|---|---|
| `ROOT_CAUSE.md` | Final adjudication: verdicts, ESTABLISHED gate-by-gate verification, causal DAG, rejected hypotheses, unresolved list |
| `RESULTS.md` | Full result tables and analysis threads |
| `PROTOCOL.md` | Frozen protocol, phaseA run record, deviations |
| `CAUSAL_DAG.md` | Pre-registered hypotheses, mechanism/performance predictions, counterfactual gates |
| `COST_CLASSIFICATION.md` | Cost taxonomy (C1–C6/U), originating/mediating/symptom classification |
| `EVIDENCE_GAPS.md` | Question/evidence/missing-observation/next-instrument/stop-condition ledger |
| `OBSERVABILITY_LIMITS.md`, `CAPABILITY_MATRIX.md` | What was actually observable on this kernel/session, with permissions and overhead |
| `CF0_THREE_ARM.md` | CF-0 three-arm counterfactual (frozen predictions, results, correctness) |
| `counterfactuals/CF1_SIGDROP.md` | CF-1 residue-signal-drop counterfactual (CPU closure, latency split) |
| `counterfactuals/CF2_DEFEROFF.md` | CF-2 defer-off counterfactual (CPU + latency zeroing, restore, depth reuse) |
| `analysis/depth-analysis/DEPTH_MECHANISM.md` | D2/D3/D16 depth mechanism analysis (enter amortization, batching, residuals) |
| `analysis/final-report-block.md` | Machine-readable final report block |
| `analysis/issue-comment-r2.md` | The comment published to issue #595 (R2) |
| `analysis/ARCHIVE_MOVES.md` | Archive move/copy log |
| `ARCHIVE-FULL-README.md` | Original archive-internal README (full local layout; byte-identical to the hosted archive copy) |
| `harness/` | `pair_runner.py`, `analyze.py` (protocol runner and statistics) |

## Shipped raw data (for remote spot-checks)

The full local archive holds **4739 files** (all raw JSONL, instrumentation
traces, binaries and receipts) and is verified by
`sha256sum -c MANIFEST.sha256` in `/home/jnhu/sluice-595-e5/causal-r2/evidence`
(full manifest SHA-256: `10a0fd601080bca59a28344c18c3864d758931fd603b6e9bec1818939646e4e0`).
This repository copy ships a compact, reviewable subset (2576 files, ~22 MiB):

- `raw/phaseA/` — all five uninstrumented phases (A/A, D1, D2, D3, D16), 24 pairs each, per-invocation `results.jsonl` + headers + logs.
- `counterfactuals/raw/ab-d2-{base,sigdrop,sigdroprep,restore,base-cf2,deferoff,deferoffrep,restore-cf2}/` — all CF-1/CF-2 timing blocks.
- `counterfactuals/three_arm/` — CF-0 trio raw blocks, analysis JSONs, correctness lanes, tools.
- `analysis/depth-analysis/raw/{deferoff-d3,deferoff-d16,cnt-d3-ab,cnt-d16-ab,pmu-d3-ab,pmu-d16-ab,strace-d3,strace-d16,split-d3,split-d16}/` — depth timing/counter summaries.
- `counters/` — cost table, C1–C4 reports, aggregation JSONs, tools.
- `counterfactuals/raw/{cnt-companion*,sysio-*,correctness,meta*}`, `traces/` (READMEs, analysis JSONs, probe sources/diffs), `traces/xlayer/` tools.

**Not shipped** (kept in the full local archive): multi-GB trace binaries,
strace text dumps, perf `.data`, instrumentation JSONL bulk, compiled binaries,
interpreter caches, and repeated debug outputs. Their digests remain covered by
the full local manifest above.

Verify the shipped subset:

```text
cd docs/research/595-causal-r2 && sha256sum -c MANIFEST.sha256
# → 2576/2576 OK
```

## Verification performed

- Full local archive: `sha256sum -c MANIFEST.sha256` → **4739/4739 OK**
  (re-checked by the run owner at final review).
- Independent subagents during the run: evidence audit of R1 (`ok=true`),
  reproduction judge recomputing phaseA from raw JSONL (`reproduced=true`),
  cost-branch review (`confirmed=true`), verdict review (initial `passed=false`
  → adjudicator revised and downgraded claims; the archived `ROOT_CAUSE.md` is
  the revised version), archive manifest gate (`exit=0`).
- Correctness oracles: phaseA 240/240 invocations exit 0 with exactly 100k ops;
  CF-1 96/96 and CF-2 96/96 likewise; CF-0 204/204; sigdrop B-mode lane 13/13
  rc=0; deferoff receipt 22 FAIL rc=134 is the **designed** rejection of the
  held-batch progress contract (proof the intervention reaches defer semantics).
- No benchmark was re-run during adjudication; recomputations used archived raw data only.

## Reproduce

```text
# Protocol runner (paths/semantics as recorded in PROTOCOL.md and ROOT_CAUSE.md §10.2)
python3 harness/pair_runner.py --phase <phase> --case '=W02-uring-4k-d2' --pairs 24 --measured 5 \
    --binary <frozen|mutant>   # taskset -c 2; SLUICE_BENCH_STATS=1

# Statistics: pair-relative (B−A)/A, bootstrap 10,000, seed 5950, percentile CI95
python3 harness/analyze.py <block-dir>
```

## Boundaries (do not over-read)

- Conclusions hold for the `W02-uring-4k` warm-tmpfs 4 KiB read shape only
  (D1/D2/D3/D16). No block device, cold cache, O_DIRECT, other block sizes, or
  other depths/backends are covered (C6 not applicable).
- The `deferoff` mutant **by design** does not satisfy the arm-B held-batch
  progress contract (595 oracle rc=134), so "the defer chain carries the whole
  effect" must not be extrapolated to shapes with held-batch stalls.
- CPU share decomposition is an upper bound (82%), not a 100% account; the
  C5 instruction-level source, eventfd internal stages and the 27–35 ns/op
  residual remain CORRELATION_PARTIAL / UNOBSERVED.
- Latency restore reproduces direction/domain, not per-percentile points;
  depth counterfactuals have **no restore block**.
- Root-only observability (`:k` PMU, sched/io_uring tracepoints, ftrace,
  eventfd internals) was unavailable in the clean-run sessions
  (`UNOBSERVED_ADMIN_UNAVAILABLE` / `perf_event_paranoid=2`).
