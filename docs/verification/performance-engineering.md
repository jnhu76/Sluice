# Performance Engineering Methodology

Status: **Active** — governing methodology for every Sluice performance
investigation and optimization. Established 2026-08 with the performance
governance corrective (PR #126); the concrete first application is the grep
round-1 case study in [`performance-attribution.md`](performance-attribution.md).

This document defines *how* performance work is done. The normative agent
constraints live in `AGENTS.md` §"Performance Change Gate"; the machine
enforcement lives in `scripts/bench/perf-attribution.py` (runner + fingerprint),
`scripts/bench/perf-evidence-validate.py` (evidence structure), and the
pre-push/CI gate that runs them. Law, methodology, and enforcement are
deliberately separate layers — prose alone is not a safeguard.

## 1. The failure mode this methodology exists to prevent

```text
profile shows mutex = 14%
        ↓
agent rewrites the mutex

perf shows DTLB misses
        ↓
agent adds huge pages

Fiber allocation appears in flamegraph
        ↓
agent introduces a stack cache
```

Each jump skips the questions that decide whether the work is even correct:

- is the real application actually bottlenecked there?
- is the cost APP, Boundary, Core, OS/environment, or benchmark artifact?
- does the effect scale with bytes / requests / Fibers / workers, and how?
- is it a common tax or a workload cliff?
- is the engineering tradeoff worthwhile?
- does the feature belong in the default Core at all?
- would a compile-time policy / backend / optional mechanism contain it better?
- does the real application improve after the microbenchmark improves?

## 2. The Bidirectional Performance Funnel

Discovery flows down; verification must flow back up.

```text
                     REAL APPLICATION
                            │
                            ▼
                     symptom / SLO
                            │
                            ▼
                End-to-End Measurement
                            │
                            ▼
                      Competent Baseline
                            │
                            ▼
                     APP Normalization
                            │
                            ▼
                 Boundary Counterfactual
                            │
                            ▼
                      Core Increment
                            │
                            ▼
                     Scaling Signature
                 /       |       |      \
              bytes   requests fibers workers
                            │
                            ▼
                 Minimal Core Reproducer
                            │
                            ▼
               Microarchitecture Drilldown
                            │
              ┌─────────────┼──────────────┐
              │             │              │
           CPU/TMA      Memory/TLB   Scheduling/I/O
              │             │              │
              └─────────────┼──────────────┘
                            ▼
                        Hypothesis
                            ▼
                 Engineering Economics
                            ▼
                        PLACEMENT
             ┌──────────────┼───────────────┐
             │              │               │
            APP        Core/default      Optional
                                  Backend / Policy / Mechanism
                            ▼
                       Implementation
                            ▼
                      Microbenchmark
                            ▼
                     Normalized App
                            ▼
                       Real App
                            ▼
                    Regression Corpus
```

> **A microbenchmark win is not a successful optimization until the benefit
> survives back up the funnel.** (§10)

## 3. Ownership domains: APP, Boundary (diagnostic), Core, environment

Final attribution labels stay small:

| Domain | Owns |
|--------|------|
| **APP** | algorithm, data structure, matcher/parser, line splitting, copy/materialization, buffering, batching, output formatting, traversal, codec/crypto implementation |
| **CORE** | public I/O abstraction overhead, submit path, RequestArena, Completion, scheduler, wait/wake, queue contention, runtime task lifecycle, backend dispatch, syscall interaction, io_uring bookkeeping, ThreadPoolBackend |
| **PLATFORM / ENVIRONMENT** | page cache, filesystem, tmpfs-vs-disk, WSL/host virtualization, cold state that is not owned by the code under test |
| **BENCHMARK_ARTIFACT** | Debug builds, mislabeled cache state, asymmetric warming, output asymmetries, instrument bias |

### BOUNDARY is a diagnostic domain, not a final owner

During diagnosis, a fourth label is useful:

> **BOUNDARY** — the App using the Core *poorly*, or the Core abstraction
> *forcing* the App into a poor execution pattern?

Example: a task doing `submit; await; submit; await` serially may mean
"APP failed to pipeline" **or** "the Core API makes pipelining impractical".
The first resolves to APP; the second is a Boundary symptom that may reveal
a Core abstraction deficiency (and feeds the App Feedback Ledger /
`docs/applications/app-feedback-ledger.md`).

BOUNDARY findings MUST eventually be resolved into APP, CORE, environment,
or artifact. They may open API work; they never authorize silently changing
concurrency semantics.

## 4. Normalize APP first — not "fix APP first"

The purpose of APP normalization is to **establish a competent application
baseline** so a poor application algorithm cannot be misattributed to Core.
It is *not* "optimize the application to theoretical perfection" — endless
App-local workaround tuning can equally hide a Core abstraction problem.

```text
MEASURE
→ NORMALIZE APP        (competent baseline; document the algorithm class)
→ TEST BOUNDARY COUNTERFACTUAL   (would a different Core usage shape avoid the cost?)
→ ISOLATE CORE         (ladder increments; see performance-attribution.md)
```

A known-and-documented APP algorithm gap (e.g. grep's missing kwset/SIMD
class) does not block measuring the Core increment — it only blocks
claiming the *total* gap is Core.

## 5. Scaling signatures

A single isolated timing proves little; the *shape* of scaling is the
evidence. Every serious finding should characterize at least the axes it
claims to involve:

| Axis | Model / sweep | Typical domains |
|------|---------------|-----------------|
| bytes | `T ≈ α·bytes` | scan, copy, hash, memory bandwidth, parser, codec |
| request count | `T ≈ α·request_count + β·bytes` | submit, queue, wake, completion, reap, context switching, syscall fixed cost |
| Fiber count | 1, 10, 100, 1k, 10k, 100k | stack allocation, stack page faults, scheduler metadata, TLB pressure, RSS, runqueue scaling |
| worker count | 1, 2, 4, 8, 16 | lock contention, cross-worker wake, stealing, cache-line bouncing, NUMA, synchronization |
| in-flight depth | 1 … capacity | pipeline overlap, backend queueing, reap batching, I/O parallelism |

Interpretation rule: if a cost scales with request count at fixed bytes, a
per-request fixed cost is supported (fixed handoff/wake/reap tax); if it
scales with bytes, it is bandwidth/copy-class. The first Core Cost
Decomposition experiment (buffer-size sweep) is specified in
`docs/history/archive/Sluice-roadmap.md` (Milestone 7; historical record).

## 6. Microarchitecture drill-down (progressive, M0 → M4)

Drill down only as far as the evidence requires; each level must justify
the next.

| Level | Measures | Examples |
|-------|----------|----------|
| **M0** end-to-end | wall time, throughput, p50/p95/p99, CPU utilization, RSS | every claim starts here |
| **M1** core efficiency | cycles, instructions, IPC, user/system time, context switches, CPU migrations, syscalls, page faults | attribution of time to kernel vs userspace vs switching |
| **M2** top-down CPU | Retiring / Frontend Bound / Backend Bound / Bad Speculation (where supported) | "is the matcher compute-bound or stall-bound?" |
| **M3** memory/TLB | L1D/L2/LLC misses, dTLB/iTLB misses, memory bandwidth, major/minor faults | stack-cache / hugepage / access-pattern hypotheses |
| **M4** topology | NUMA local/remote, cache-line bouncing, false sharing, remote wakeups, hugepage behavior | multi-worker and large-Fiber-count investigations |

> **PMU counters are diagnostic evidence, not optimization authorization.**
> A high counter is a clue, not a mandate. Never optimize a counter just
> because it is high; the funnel (real-application symptom → normalized
> baseline → increment → scaling) is what authorizes work.

## 7. Normalized metrics and ratios

Raw event counts are not comparable across machines or workload sizes.
When the data exists, derive:

```text
per-request : ns/request, cycles/request, instructions/request,
              context-switches/request, syscalls/request,
              wakeups/request, LLC-misses/request, DTLB-misses/request
per-byte    : ns/byte, cycles/byte, instructions/byte
per-Fiber   : page-faults/Fiber, RSS/Fiber, stack-bytes/Fiber
```

Core-side comparisons use:

| Metric | Definition |
|--------|------------|
| **Core Increment** | `T_normalized_sluice − T_normalized_direct` (ladder: L4 − L3) |
| **Core Overhead Ratio** | `(T_sluice − T_direct) / T_direct` |
| **Core Share** | `CoreIncrement / T_sluice` |

Later extension work may add an **Abstraction Tax**
(`T_extension_seam − T_direct_primitive`) for stackless/optional seams.
Do not invent a single "performance score"; ratios with units only.

Important wording rule (learned in round 1): an aggregate ladder increment
proves that a Core-owned cost *exists* after APP normalization. It does not
decompose that cost among runtime lifecycle, admission, submit, handoff,
syscall interaction, wait/wake, reap, or Fiber resume — internal
composition is a *hypothesis* until a decomposition experiment runs.

## 8. Common Tax and Cliff Weakness — when Core work is justified

A Core performance change is promoted through **either** evidence path:

- **Common Tax** — the same cost appears repeatedly across independent
  workload classes (grep, copy, WAL, network, KV all showing a similar
  completion→wake→resume tax). Strong signal the cost belongs in Core work.
- **Cliff Weakness** — one important workload class shows catastrophic or
  superlinear scaling (normal workloads +5%, small random I/O +250%).
  One class is enough when the cliff is material.

The existing two-app rule from the App Feedback Ledger still governs
*convenience abstractions*. Performance architecture uses the rule above:
Common Tax OR material Cliff Weakness.

## 9. Engineering economics — who should pay the cost?

A technical speedup does not automatically belong in default Core. Every
serious candidate records a decision record (not a fake score):

```text
gains          : real-app gain, normalized-app gain, microbench gain
breadth        : affected workload classes, regressions elsewhere
costs          : CPU, RSS delta, allocation delta, tail-latency delta
restrictions   : platform / kernel / hardware requirements
complexity     : code size, maintenance burden, review surface
risk           : correctness risk, formal-verification burden
```

Core question: **who should pay the cost?** A mechanism that helps one
workload class 20% while taxing every other user 2% is a placement
question, not a "should we implement it" question.

## 10. Placement

Every optimization candidate chooses a placement before implementation:

```text
APP-local
Core mandatory semantic mechanism
Core default mechanism
compile-time policy
runtime service / backend
optional mechanism (experimental component)
reject / no implementation
```

This separates "worth implementing?" from "should everyone pay for it?".
Illustrative (NOT implemented, methodology only): a Fiber stack cache that
gives a 100k-short-Fiber server +20% throughput and −60% page faults while
costing +15% RSS and +0.5% on grep would justify
`IMPLEMENT: yes; DEFAULT CORE: no; PLACEMENT: optional stack policy`.

## 11. Small Semantic Core + Composable Mechanisms

Semantic Core owns what defines what Sluice *means*:

```text
request identity (RequestKey), Completion semantics, ownership, lifetime,
cancellation outcome, wait/wake correctness, Scheduler correctness
invariants, backend contract, shutdown/drain semantics
```

These MUST NOT become optional plugins — correctness semantics are
mandatory. Composable mechanisms (subject to evidence and economics) may
include AsyncBackend, StackPolicy, allocation policy, scheduling policy,
NUMA policy, instrumentation, tracing, experimental fast paths. Preference
order:

```text
1. compile-time policy
2. runtime service / backend
3. dynamic binary plugin only if genuinely necessary
```

> Everything optional should be composable when the abstraction tax and
> engineering economics justify it. Composable mechanisms are NOT a dynamic
> plugin framework — `AsyncBackend` and the experimental io_uring backend
> are the existing examples of composition; a generic shared-object
> (`.so` / `.dll`) plugin
> loader remains a near-term non-goal (see the roadmap's explicit
> non-goals).

## 12. Workload signatures

Every serious reference app / benchmark records its workload signature so
results are not over-generalized. Suggested fields: request size, request
count, read/write mix, sequential/random, in-flight depth, Fiber count,
worker count, compute/I/O ratio, latency sensitivity, throughput
sensitivity, memory footprint, lifetime duration, burstiness, durability
requirements. Round-1 signatures are recorded in
[`performance-attribution.md`](performance-attribution.md).

## 13. Machine-readable evidence and enforcement

Canonical evidence lives in `docs/results/performance-attribution/` as
runner-produced JSON (never hand-created; see the artifact README for
provenance). The runner embeds the full environment fingerprint
(git SHA + dirty, build, kernel/CPU/glibc, WSL state, filesystem mounts
via `/proc/self/mountinfo`, tool versions), workload parameters, warmups,
iterations, raw per-iteration samples (timing-loop artifacts — ladder and
CLI; `perf` captures a single run's counters plus the verbatim perf
output), and derived statistics.

`scripts/bench/perf-evidence-validate.py` structurally validates every
committed artifact (fail-closed, self-tested) and runs in the pre-push gate
and CI. Compatibility warnings fire at compare time when two artifacts
differ in CPU / filesystem / build / compiler / WSL state / workload
config. Absolute speed thresholds deliberately stay out of CI: shared
runners make them brittle; structure, not speed, is what the gate enforces.

## 14. Governing principles

```text
Applications discover.
Attribution isolates.
Scaling characterizes.
Microarchitecture explains.
Economics prioritizes.
Placement contains cost.
Core generalizes.
Applications verify.
```

```text
A performance optimization may be worth implementing
without being worth putting in the default Core.
```

```text
Correctness semantics are mandatory.
Performance mechanisms may be composable.
```
