# RX-1 Report — Controlled Attribution Falsification Gate

Verdict (exactly one, pre-registered): **NOT SUPPORTED / STOP EXPANSION**

## BASE

```text
RX1_BASE_SHA     = 1e6af0824f98d12753d1cc48308b8b6e56834069 (= origin/master, PR #235 merge)
V0.0.1_SHA       = a38df5e9a7bee3603a439857f036de2b5a136bf2
AC1A_MERGE_SHA   = 1e6af0824f98d12753d1cc48308b8b6e56834069
PROTOCOL_COMMIT  = 45a993d (research(rx1): freeze attribution protocol v1)
PROTOCOL_SHA256  = cae8052accd26a073aad79f0fc41b979dd9202e3d34b04dde6dd5b9fec7dcdf7
formal HEAD      = research(rx1): formal matrix + analysis (this PR)
baseline gate    = Debug clang: sluice_core + sluice_async build, 191/191 tests PASS
                   (run before any edit; tree unchanged in production sources)
```

## QUESTION

Does Sluice's explicit internal resource information (the AC-1a accessors)
materially improve our ability to identify a deliberately injected
performance bottleneck beyond ordinary external system telemetry?

## HYPOTHESIS / NULL HYPOTHESIS

- H_RX1: first-class explicit resource state materially improves bottleneck
  attribution over conventional process/kernel telemetry for controlled
  ThreadPool workloads.
- H0: external telemetry is sufficient, or Sluice L1 signals do not improve
  attribution enough to justify deeper observability work.

**H0 is supported.** Falsification of H_RX1 succeeded on the frozen verdict
gate: Δaccuracy(E−C) = **−1.35 pp** ≤ 0 (criterion 1), and E materially
increased confident wrong-cause predictions 0.45% → 1.80% (criterion 2).

## ENVIRONMENT

WSL2, kernel 6.18.33.2-microsoft-standard-WSL2, Ubuntu 26.04, AMD Ryzen 7
5800H (8 logical CPUs), 16 GiB, cgroup v2, workload files on tmpfs `/tmp`,
clang 21.1.8 (Release `-O3`), xmake 3.0.9, perf 7.0.12 with
`perf_event_paranoid=2` (user-space-only counters; context-switch/migration
events read 0 — compensated by thread-summed `/proc/<pid>/task/*` accounting),
iostat unavailable, PSI cpu/io/memory available. CPU/Request/Worker/App
attribution valid under WSL2; device-level conclusions are NOT generalized
(I5 deferred). Ambient PSI: cpu some ≈ 2.8k µs/s idle, < ~90k under load
including spikes; the CPU gate (250k) sits between ambient and I4 readings.

## SOTA / EXTERNAL BASELINE ASSUMPTIONS

The C baseline was deliberately strong (task brief §27 fairness rule):
static workload configuration (sizes/depths/capacities/workers — what any
operator knows), workload outcome (throughput, latency percentiles, and the
caller-visible submit-rejection count that ANY I/O library surfaces to its
caller), process-level OS accounting (all-thread rusage CPU, context-switch
counters, schedstat wait/run/timeslices), system PSI (cpu/io/memory,
some+full), and perf stat (cycles/instructions per op; task-clock). Nearest
prior-art framing (uringscope, PSI, Tokio console, COZ interventionism) per
#234 §9; RX-1 intentionally stops before eBPF (§28).

## INTERVENTIONS

| id | label | mechanism | non-target resources |
|---|---|---|---|
| I0 | CONTROL | none (depth 16, cap 64, workers 4) | — |
| I1 | APP_PIPELINE_LIMITED | app depth 2 | capacity 64, workers 4 generous |
| I2 | REQUEST_CAPACITY_LIMITED | arena capacity 4 < offered depth 32 | workers 4, CPU/storage unstressed |
| I3 | THREADPOOL_WORKER_LIMITED | workers 1 | capacity 64, depth 32 ample |
| I4 | CPU_CONTENDED | bench + 4 pinned busy-loop stressors on the same 2 CPUs (taskset; stressor start/stop recorded per run) | capacity/workers generous |
| I5 | IO_SERVICE_CONTENDED | **ENVIRONMENT INVALID / DEFERRED** — tmpfs (no device service), iostat unavailable, WSL2 virtual-storage ambiguity | — |

Ground-truth validity (pre-registered, checked per run before scoring):
I2 rejections>0 ∧ high-water==capacity; I3 active-worker saturation ∧
dispatch queueing ∧ no dominant rejections; I1/I0 no internal saturation;
I4 stressor ran ∧ PSI evidence; CONTROL additionally requires workers not
saturated.

## PILOT

Two calibration rounds at 64 KiB read (method §6 of RX1_METHOD.md). Round 1
reshaped the protocol (PSI ambient/file-creation pollution → pre-created
cache-warmed files + 250k CPU gate; frac_dispatch_nonzero stagger artifact →
mean dispatch occupancy; positive CONTROL rule; C per-worker proxy 1.10).
Round 2: all five families cleanly induced, validity 100%, C 19/19 E 19/19 —
the calibration point cannot distinguish C from E. STOP gate (≥3 non-control
families cleanly induced): passed 4/4 (I5 deferred). Pilot data is committed
as calibration evidence only and never scored as formal data.

## FROZEN PROTOCOL

`rx1_protocol_v1.json` @ 45a993d, before formal execution. Label set
(6 + UNKNOWN), feature whitelist (`rx1_classify.extract_features` — no
intervention/affinity/stress/index/label leakage), thresholds, rule
precedence (CPU > conflict-UNKNOWN > capacity > worker > app > control >
io > UNKNOWN), sampling (OBS-LOW 10 ms formal; OFF/1 ms tax block), matrix
(read/write × {4K 512 MiB, 64K 512 MiB, 1M 1 GiB} × I0–I4 × 8), tax block
(control × {64K, 1M} read × 3 modes × 8), seed 1380708657, invalid rules,
primary metrics, verdict thresholds. Classifier pipeline proven by a
synthetic dataset with known expected outputs BEFORE any real experiment
(self-test 32/32, committed in the freeze commit).

## CLASSIFIER C (external-only)

Interpretable rules over config + outcome + OS accounting + PSI + perf:
CPU = PSI cpu some > 250k µs/s; CAPACITY = caller-visible rejections > 0;
WORKER = per-worker CPU utilization ≥ 1.10 ∧ PSI quiet ∧ no rejections;
APP = cores ≤ 2.0 ∧ depth/capacity ≤ 0.10; CONTROL = cores ≥ 0.75 ∧ no
rejections ∧ PSI quiet; IO = PSI io > 50k; else UNKNOWN; capacity+worker
conflict → UNKNOWN.

## CLASSIFIER E (external + explicit-I/O)

Exactly C's features plus the nine AC-1a accessor aggregates: CAPACITY =
rejections_delta > 0 ∧ frac_slot_at_capacity ≥ 0.30; WORKER =
frac_active_at_configured ≥ 0.55 ∧ dispatch_occ_mean ≥ 10 ∧ PSI quiet ∧ no
rejections; APP adds slot_occ ≤ 0.10 ∧ frac_active ≤ 0.30; CONTROL adds
frac_active < 0.55 ∧ frac_slot < 0.30. Same precedence and UNKNOWN policy.

## FORMAL MATRIX

288 recorded runs (240 attribution + 48 observability-tax), each a fresh
Release process (1 unrecorded warmup + 6 measured internal repetitions,
files pre-created and cache-warmed), cell order seeded-randomized per
workload block, controls interleaved. **Correctness: 288/288 pass**
(exact op/byte accounting, word-sum verification, clean drain/join).
Valid attribution runs: **222/240**. All 18 invalid runs are CONTROL cells
where the frozen generous config physically saturates workers at slow-op
shapes (write/64K 8/8, write/1M 8/8, read/1M 2/8) — pre-registered validity
excluded them; invalid-rate is reported, never scored.

## RESULTS

| metric | C (external) | E (external + AC-1a) |
|---|---|---|
| Top-1 accuracy (222 valid of 240 attribution-matrix runs) | **0.9955** | 0.9820 |
| wrong-cause rate | 0.45% (1) | 1.80% (4) |
| UNKNOWN rate | 0 | 0 |
| macro-F1 | 0.9945 | 0.9788 |

**Primary comparison (preregistered, run-level paired bootstrap):
Δaccuracy = accuracy(E) − accuracy(C) = −1.35 pp, 95% CI [−3.60, +0.45].**

**ROBUSTNESS ANALYSIS — NOT PRIMARY PREREGISTERED SCORE.** Because the
disagreements cluster by workload × intervention cell (notably I3@4K), a
post-hoc paired block bootstrap with the workload-shape × intervention cell
as the resampling unit (28 non-empty cells of 6×5, seed 1702) gives
Δ = −1.35 pp, 95% CI [−4.55, +0.91]. The cell-clustered interval is wider,
as expected when the effective sample shrinks from 222 runs to 28 cells,
and still straddles zero on the same side; it does not replace or modify
the frozen run-level primary result or the verdict.

### CONFUSION MATRICES (rows = ground truth)

C (one error: CONTROL→THREADPOOL_WORKER at read/1M):

| true\pred | CTRL | APP | CAP | WORKER | CPU | IO | UNK |
|---|---|---|---|---|---|---|---|
| CONTROL (30) | 29 | 0 | 0 | 1 | 0 | 0 | 0 |
| APP (48) | 0 | 48 | 0 | 0 | 0 | 0 | 0 |
| CAP (48) | 0 | 0 | 48 | 0 | 0 | 0 | 0 |
| WORKER (48) | 0 | 0 | 0 | 48 | 0 | 0 | 0 |
| CPU (48) | 0 | 0 | 0 | 0 | 48 | 0 | 0 |

E (four errors: WORKER→CONTROL at 4K read ×3 and 4K write ×1):

| true\pred | CTRL | APP | CAP | WORKER | CPU | IO | UNK |
|---|---|---|---|---|---|---|---|
| CONTROL (30) | 30 | 0 | 0 | 0 | 0 | 0 | 0 |
| APP (48) | 0 | 48 | 0 | 0 | 0 | 0 | 0 |
| CAP (48) | 0 | 0 | 48 | 0 | 0 | 0 | 0 |
| WORKER (48) | 4 | 0 | 0 | 44 | 0 | 0 | 0 |
| CPU (48) | 0 | 0 | 0 | 0 | 48 | 0 | 0 |

### PER-CLASS RESULTS

| class | precision C | recall C | precision E | recall E |
|---|---|---|---|---|
| CONTROL | 1.000 | 0.967 | 0.882 | 1.000 |
| APP_PIPELINE_LIMITED | 1.000 | 1.000 | 1.000 | 1.000 |
| REQUEST_CAPACITY_LIMITED | 1.000 | 1.000 | 1.000 | 1.000 |
| THREADPOOL_WORKER_LIMITED | 0.980 | 1.000 | 1.000 | 0.917 |
| CPU_CONTENDED | 1.000 | 1.000 | 1.000 | 1.000 |

### C VS E PAIRED DELTA

| transition | count |
|---|---|
| right → right | 217 |
| **right → wrong** | **4** |
| **wrong → right** | **1** |
| unknown → right / right → unknown / others | 0 |

### OBSERVABILITY TAX (paired per shape against the same-shape OBS-OFF baseline; n=8 per shape/mode)

Each OBS-LOW / OBS-HIGH measurement is normalized against the same-shape
OBS-OFF baseline first; normalized effects are aggregated afterward (median
of per-shape taxes). Shapes are never pooled into one raw aggregate.

| shape | OBS-OFF MB/s | OBS-LOW MB/s (tax) | OBS-HIGH MB/s (tax) | p99 tax LOW | p99 tax HIGH |
|---|---|---|---|---|---|
| read/64K | 12192 | 11932 (+2.1%) | 11933 (+2.1%) | +1.4% | −1.9% |
| read/1M | 5728 | 5646 (+1.4%) | 5502 (+4.0%) | +22.0% | +9.1% |

Aggregate (median of per-shape normalized effects): OBS-LOW +1.8%,
OBS-HIGH +3.0% throughput tax; p99 tax aggregate +11.7% (LOW) / +3.6%
(HIGH). Sample spreads overlap across modes (64K OFF 10244–12898 vs LOW
9366–12387 MB/s) and neither the throughput nor the p99 effect is monotonic
in sampling rate (HIGH repeatedly reads cheaper or cheaper-tailed than
LOW, which more sampling cannot produce). **No reproducible or monotonic
observation-tax signal was established at the current sample size.**
Information gain / observation cost is therefore reported as two honest
numbers, not one score: attribution gain ≤ 0 at v1 (≤ +0.45 pp at the
exploratory ceiling); OBS-LOW throughput-tax point estimates +1.4–2.1%
per shape.

### GENERALIZATION / HOLD-OUT

The calibration shape (read/64K) scores C = E = 1.000 — it cannot
distinguish the information sets. All C/E separation happened on hold-out
shapes: E lost 3 at read/4K and 1 at write/4K (worker-rule boundary);
E won 1 at read/1M (queue-depth evidence); E gained nothing elsewhere.
Per-shape accuracy: read/4K C 1.000 / E 0.925; read/64K 1.000 / 1.000;
read/1M 0.974 / 1.000; write/4K 1.000 / 0.975; write/64K 1.000 / 1.000;
write/1M 1.000 / 1.000 (last two with 8 invalid control cells each).

## FAILURE-CASE ANALYSIS (all five disagreements)

1. **run 0080 (C wrong, E right) — TRUE INFORMATION GAIN.** CONTROL at
   read/1M: per-worker CPU utilization reads 1.11 (just over C's 1.10
   saturation proxy) because four workers legitimately busy ≈ 4.4 cores; C
   misattributes THREADPOOL_WORKER_LIMITED. E's dispatch mean occupancy
   (3.2, shallow queue vs ≥ 19 in genuinely worker-limited runs) correctly says the
   workers are busy, not bottlenecked. This is exactly the occupancy-vs-
   bottleneck separation external CPU accounting cannot make.
2. **runs 0002/0005/0025/0130 (C right, E wrong) — CLASSIFIER DESIGN
   FAILURE (shape instability of a signal, honestly measured).** I3 at 4K:
   `frac_active_at_configured` reads 0.52–0.55, straddling the frozen 0.55
   threshold. At 4 KiB the worker's syscall duty cycle is ~53% (handoff
   dominates the op cycle) vs 0.78–0.89 at the 64 KiB calibration point —
   "workers observed at configured count" stops meaning "saturated" when
   per-op overhead dominates. E's AND-composed worker rule inherited that
   instability while its other conjunct (deep queue, mean 19.5–22.5) had
   the right answer. Not a sampling miss (207–232 samples); not
   environment; not multi-cause.

No SAMPLING MISS, no MULTI-CAUSE/UNKNOWN, no ENVIRONMENT INVALID among the
disagreements; zero disagreement involved capacity, app, or CPU classes —
for those families external evidence and internal evidence agree.

**EXPLORATORY ONLY (does not replace the v1 score):** a post-hoc v2 worker
rule keyed on deep-queue alone (dropping the shape-unstable frac_active
conjunct) reaches E2 = 1.000 accuracy, fixing all four 4K errors while
keeping the 1M gain — Δ(E2−C) = **+0.45 pp [95% CI +0.00, +1.35]**. Even
granting E the best plausible rule over the same frozen features, the
information ceiling of the AC-1a signal set on this workload family is
below half a percentage point over the strong external baseline — an order
of magnitude short of the +15 pp SUPPORTED bar. This is the strongest form
the negative result can take.

## FALSIFICATION RESULT

Both pre-registered NOT-SUPPORTED criteria fired:

1. Δaccuracy(E−C) = −1.35 pp ≤ 0 (CI [−3.60, +0.45] does not exclude zero
   on the positive side);
2. E materially increased confident wrong-cause predictions (×4 in count).

Additionally the generalization criterion fired at v1: E's calibration-point
parity became a net loss on hold-out shapes, and the exploratory ceiling
(+0.45 pp) shows the deficit is not a rule-composition artifact.

## RESEARCH IMPLICATION

For pipeline-shaped, single-host, cache-hot ThreadPool workloads — the
family RX-1 was authorized to test — conventional external telemetry
(configuration + outcome + process accounting + PSI) is already sufficient
for bottleneck attribution: a strong external classifier reached 99.5%.
The AC-1a internal signals add a real but tiny quantum of separable
information (the busy-vs-bottleneck queue signal: +1 run, −4 runs at v1,
≤ +0.45 pp at the exploratory ceiling). H_RX1 is not supported; H0 stands.
This resolves the "PROMISING BUT NOVELTY UNCLEAR" verdict of #234 §15 for
the attribution half of the thesis: the cheap falsification killed the
cheap version of the hypothesis, as designed.

Scope: RX-1 falsifies the incremental attribution value of AC-1a for the
tested family — **single-machine, ThreadPool backend, cache-hot (tmpfs,
page-cache-warm), known-configuration, controlled pipeline workloads**. It
does NOT establish a universal theorem that explicit-I/O observability can
never add value. Not tested and not covered by this verdict: multi-tenant
or device-bound regimes (I5 was environment-invalid), degraded-telemetry
environments (no PSI / no perf), cross-layer identity joins (RX-4/eBPF
territory), and non-pipeline workload shapes.

The roadmap consequence remains: **no evidence currently justifies L2
timestamps, RequestKey telemetry, eBPF integration, RX-2/RX-3, or other
observability expansion.** Future reopening requires a materially
different, preregistered hypothesis and new evidence.

## ARCHITECTURE IMPLICATION

- **Stop deeper observability research**: L2 lifecycle timestamps,
  RequestKey telemetry, cancel-winner split instrumentation, resource-vector
  frameworks, and the RX-2/RX-3 tier-cost experiments are NOT authorized by
  this result. Do not build them for attribution purposes.
- **Keep AC-1a as engineering metrics** (it is already merged, zero-cost by
  construction, and directly useful for resource display and debugging: the
  four accessors answer "how full / how busy is my runtime" without any
  new hot-path work). This matches the ENGINEERING-VALUE-ONLY component of
  the outcome.
- The one durable technical lesson for future signal design (recorded, not
  acted on): `active_workers == configured` is a duty-cycle proxy, not a
  saturation predicate; queue-side occupancy is the shape-stable quantity.
  If any future engineering metric is added to the ThreadPoolBackend, mean
  dispatch occupancy is the evidence-backed candidate — but adding it is
  NOT authorized by RX-1.
- Continue the architecture roadmap (#225/#227 sequencing: Wait-domain
  refactors, AC-2) using justified engineering metrics only.
- Reopening observability research later requires a materially different,
  preregistered hypothesis and new evidence — not a re-tuned version of the
  falsified one.

## DEFERRED QUESTIONS

- Would explicit internal signals matter where external telemetry is
  structurally degraded (containers without PSI, no perf, noisy neighbors)?
  Unmeasured; requires new authorization and a different environment.
- Would RequestKey-level cross-layer identity (RX-4-style eBPF join) change
  the picture for kernel-side attribution? Unmeasured; explicitly out of
  RX-1 scope and not recommended without a new concrete case.
- Observation tax under sustained (not sampled) observation modes — RX-1
  only measured pull-based sampling; unmeasured and now moot for research
  purposes.

## Harness defect disclosure (post-freeze, documented per §15)

The run-time perf decoder dropped the `:u` event-name suffix that
`perf_event_paranoid=2` adds, so the stored perf feature dicts were empty.
Fixed in a separate commit by re-parsing the preserved raw perf evidence at
feature-extraction time. No frozen rule reads perf features; the fix was
verified prediction-invariant (C/E predictions and validity identical on
all 288 runs before/after). No protocol field changed; no rerun required.
Raw artifacts were never modified (scoring writes `.scored.json` siblings).

## Evidence-artifact retention

Decision: **KEEP RAW IN GIT.** All 288 raw formal run artifacts
(`results/formal/run_*.json`, ~4.6 MB) and the 28 pilot artifacts are
retained verbatim in the repository; the derived `.scored.json` siblings
and `results/analysis/` are deterministically regenerable from the raw
artifacts by `classify`/`analyze`. Integrity anchor:
`results/ARTIFACT_MANIFEST.json` records the SHA256 of every raw run
artifact plus the frozen protocol at review-correction time, so any later
mutation of raw evidence is detectable by re-hashing. Nothing was deleted
for aesthetics; no storage-form change was made, so no byte/hash
correspondence proof is required beyond the manifest itself.

## Post-review corrections (evidence-package only; raw data untouched)

Per review: attribution denominator clarified (222 valid of 240
attribution-matrix runs; the 48 observation-tax runs are a separate matrix
and never enter the attribution denominator); observability-tax aggregation
reworked to per-shape pairing with aggregate-of-normalized-effects;
a cell-level paired block bootstrap added as a clearly labeled robustness
analysis; tax wording standardized ("no reproducible or monotonic
observation-tax signal was established at the current sample size" — no
equivalence claim); scope and reopening conditions tightened; retention
decision + artifact manifest added; the generated `tables.md` EOF blank
line that failed the CI range-mode `git diff --check` repaired in the
generator; the perf `:u` decoder regression locked in self-test and the
dead `task_clock_ratio` feature renamed to the protocol's `task_clock_s`.
No classifier rule, threshold, or UNKNOWN policy changed, no raw formal
artifact was modified, and C/E predictions plus validity were re-verified
identical across all 288 runs after every code touch (diff = 0). The
preregistered primary verdict is unchanged.

## FINAL STOP

Draft PR opened with the harness, frozen method/protocol, compact
machine-readable results, analysis and this report. Not merged; no
completion comments posted to #227/#234. Production code, public API, and
the observability surface are unchanged by RX-1. Awaiting human adversarial
review.
