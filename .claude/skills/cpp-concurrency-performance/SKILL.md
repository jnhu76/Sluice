---
name: cpp-concurrency-performance
description: Concurrent C++ performance workflow for Sluice. Use when the task asks to measure, diagnose, review, benchmark, or optimize throughput, latency, scaling, contention, scheduling, cache/coherence, locality, oversubscription, or NUMA behavior.
---

# C++ concurrency performance

Use this skill to turn a performance question into a reproducible, attributable experiment. It is not a correctness skill and does not authorize lock-free code by itself.

Apply `cpp-concurrency-guidelines` whenever a candidate changes synchronization, ownership, wake/progress, cancellation, or shutdown semantics.

Load `cpp-lock-free` only if the chosen candidate genuinely requires advanced atomic/lock-free reasoning.

A performance investigation may start before a baseline exists. Production optimization is blocked until the baseline/experiment contract below is established.

## Step 1 — freeze the experiment

Before changing production code, record the material comparison conditions:

```text
revision:
workload / dataset:
operation mix:
input size:
thread/worker counts:
queue depth / offered load:
metric:
warmup / duration / repetitions:
CPU / topology / VM/container:
OS/kernel:
compiler/build flags:
backend/storage:
```

If the task is only to design or review a benchmark, this step defines what must be measured next; do not invent results that do not exist.

Keep correctness/output semantics equivalent across compared runs.

**Completion criterion:** either a reproducible baseline contract exists, or the missing measurements are explicitly identified and no production optimization claim proceeds past this gate.

## Step 2 — establish the baseline shape

Measure the metric across a useful concurrency range, including one worker/thread when the architecture permits it.

Record enough of the distribution to see noise rather than only the best run:

```text
threads
median
min/max or another stated spread
throughput / latency
speedup
parallel efficiency when meaningful
CPU utilization / blocking evidence when available
```

The scaling curve is evidence. Do not infer a bottleneck from one thread count or one profiler screenshot.

**Completion criterion:** the regression or scaling limit is repeatable enough to distinguish from environment noise, or the task remains an evidence-gathering experiment rather than an optimization claim.

## Step 3 — classify before optimizing

Classify the leading candidate using measured evidence:

- lock or atomic contention;
- scheduler/context-switch overhead;
- task granularity;
- load imbalance;
- queueing/backpressure;
- cache-line bouncing / false sharing;
- memory bandwidth/latency;
- NUMA placement;
- oversubscription;
- external serialization;
- I/O/device limit;
- algorithm/application work;
- unknown.

A sampled symbol percentage is not causal attribution by itself. Look for a scaling signature, request-count relationship, controlled toggle, ladder, counter, or another discriminating experiment.

**Completion criterion:** the candidate has at least one observation that would differ if the hypothesis were false.

## Step 4 — design one discriminating change

Prefer the smallest experiment that tests the hypothesis:

- move work out of a critical path;
- eliminate unnecessary shared writes;
- batch or partition independent work;
- change task granularity;
- reduce an identified wake/syscall/timestamp cost;
- improve locality or layout;
- change an algorithmic class when APP-layer evidence points there.

If the change modifies synchronization semantics, apply `cpp-concurrency-guidelines` before treating its speed as meaningful.

**Completion criterion:** the experiment changes one primary causal mechanism while preserving the tested contract.

## Step 5 — re-measure symmetrically

Compare baseline and candidate under the same experiment contract.

Report:

```text
baseline revision/results:
candidate revision/results:
absolute delta:
relative delta:
spread/noise:
correctness equivalence:
resource/cost changes:
```

Keep raw or canonical artifacts when the repository has a performance-evidence path.

A candidate is not a win if it shifts cost into unacceptable tail latency, memory, CPU, wakeups, allocation, or correctness risk.

**Completion criterion:** the claimed improvement is larger or structurally different enough from observed noise to justify the claim.

## Step 6 — decide keep / revise / revert

- **keep** when the hypothesis is supported, correctness is preserved, and the gain matters for the stated objective;
- **revise** when the experiment taught something but did not isolate the mechanism;
- **revert** when the gain is noise, workload-specific without justification, or paid for by a worse contract/cost vector.

Do not preserve speculative complexity merely because it once benchmarked faster.

## Completion record

A finished performance task should state:

```text
objective:
baseline contract / missing evidence:
scaling evidence:
attribution classification:
discriminating experiment:
A/B result and noise:
correctness equivalence:
cost vector / regression risk:
decision: keep / revise / revert / evidence-only
```

## Detailed reference

`REFERENCE.md` preserves the previous long-form performance handbook. Search for the bottleneck/tool/topic heading first and read only the smallest relevant section; do not load the whole handbook by default. Current routing and completion criteria live here.
