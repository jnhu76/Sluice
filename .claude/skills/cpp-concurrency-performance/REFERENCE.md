---
name: cpp-concurrency-performance
description: Diagnose and optimize measured multithreaded C++ performance. Use for scalability, throughput, tail latency, contention, lock waits, scheduling overhead, task granularity, queueing, false sharing, cache-line bouncing, load imbalance, oversubscription, CPU affinity, locality, and NUMA investigations.
origin: custom
---

# C++ Concurrency Performance

Optimize concurrent C++ from evidence.

Do not optimize concurrency from folklore.

This skill is for measured performance work after the concurrency design has a credible correctness and lifecycle model.

Use `cpp-concurrency-guidelines` for correctness.

Use `cpp-lock-free` for explicit lock-free algorithm work.

This skill does not authorize lock-free programming merely because a mutex appears in a profile.

## Canonical Sources

Prefer primary tooling and platform documentation for measurement semantics:

- C++ Core Guidelines `CP.*` and `Per.*`: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines
- Intel VTune Profiler Threading analysis: https://www.intel.com/content/www/us/en/docs/vtune-profiler/user-guide/2026-1/threading-analysis.html
- Linux `perf` manual set, including `perf stat`, `perf sched`, `perf lock`, `perf mem`, and `perf c2c`: https://www.kernel.org/doc/man-pages/online/dir_all_by_section.html
- compiler/runtime documentation for the selected threading framework
- hardware vendor architecture and performance-monitoring documentation
- project benchmark contracts and persisted raw results

Do not assume a performance counter means the same thing on every microarchitecture.

Record the platform and tool.

## Core Principle

Concurrency performance work follows:

```text
baseline
    ↓
scaling curve
    ↓
bottleneck classification
    ↓
measured hypothesis
    ↓
one narrow change
    ↓
re-measure
    ↓
keep, revise, or revert
```

Do not begin with:

- remove locks
- use atomics
- add threads
- pin every thread
- add padding
- use work stealing
- use a lock-free queue
- use relaxed atomics

Begin with:

> What metric is bad, where does scaling stop, and what evidence identifies the limiting mechanism?

## Performance Work Entry Gate

Before changing code, answer:

1. What workload matters?
2. What metric matters?
3. What is the baseline?
4. What thread-count matrix exists?
5. What hardware and OS are used?
6. What compiler and build configuration are used?
7. Is correctness already established?
8. Is the benchmark stable enough to compare changes?

Do not optimize from a smoke test.

## Define The Performance Objective

State the primary metric.

Examples:

- operations per second
- bytes per second
- jobs per second
- median latency
- p95 latency
- p99 latency
- maximum acceptable queue delay
- CPU efficiency
- scaling efficiency
- energy per operation

Do not use "faster" as the entire objective.

For throughput systems, latency may still constrain batching.

For latency systems, throughput may still constrain overload behavior.

State the tradeoff.

## Freeze The Workload

Before comparison, record:

- dataset
- input size
- operation mix
- read/write ratio
- task size distribution
- concurrency level
- queue depth or offered load
- warmup policy
- run duration
- repetitions
- CPU set or affinity policy
- background load assumptions

Do not change the workload while claiming an implementation speedup.

If the workload must change, establish a new baseline.

## Record The Environment

Persist at least the relevant subset of:

```text
CPU model:
physical cores:
logical CPUs:
sockets:
NUMA nodes:
SMT:
memory:
kernel:
compiler:
compiler version:
C++ standard:
optimization flags:
link-time optimization:
sanitizers:
frequency governor:
turbo policy:
container/VM:
thread runtime:
benchmark commit:
```

Do not compare results collected under materially different environments without stating the difference.

## Build The Scaling Matrix

Measure thread counts across a meaningful range.

Typical example:

```text
T=1
T=2
T=4
T=8
T=16
...
```

Adapt to physical cores, SMT, NUMA topology, and workload.

Always include `T=1` unless the architecture fundamentally cannot run with one worker.

Record:

```text
threads
throughput
latency
speedup
parallel efficiency
CPU utilization
```

Use:

```text
speedup(T) = performance(T) / performance(1)

parallel efficiency(T) = speedup(T) / T
```

For lower-is-better metrics such as elapsed time, use an appropriate equivalent.

Do not interpret only the best thread count.

The shape of the scaling curve is evidence.

## Scaling Diagnosis Gate

When scaling degrades, classify the dominant candidate before changing code.

Primary dimensions:

1. contention
2. scheduling and context switching
3. task granularity
4. load imbalance
5. queueing and backpressure
6. locality and coherence traffic
7. false sharing
8. memory bandwidth / latency
9. NUMA placement
10. oversubscription
11. external serialization
12. I/O device limits

More than one may exist.

Do not label every scaling failure "lock contention".

## 1. Contention

Investigate when threads spend time waiting on synchronization or repeatedly contending on shared atomic state.

Collect available evidence such as:

- lock wait time
- number of contended acquisitions
- average wait
- blocked time
- synchronization-related context switches
- lock hold duration
- critical-section duration
- atomic retry count
- CAS failure rate
- cache-line ownership transfer on hot synchronization state

Ask:

- Which shared invariant requires the lock?
- Is the protected state too broad?
- Is one lock protecting unrelated state?
- Is the critical section performing blocking or expensive work?
- Is unknown code called while locked?
- Is the hot path writing globally shared metadata?
- Can ownership or partitioning remove sharing?

Preferred optimization order:

1. remove unnecessary work from the critical path
2. remove unnecessary sharing
3. partition independent state
4. change ownership or batching
5. narrow synchronization while preserving invariants
6. change synchronization primitive when evidence supports it
7. consider lock-free only through `cpp-lock-free`

Do not mechanically split a mutex if the original lock protects one multi-field invariant.

## 2. Scheduling And Context Switching

Investigate:

- runnable threads
- active threads
- blocked threads
- involuntary context switches
- voluntary context switches
- wakeup frequency
- migrations
- scheduler latency
- synchronization waits

Ask:

- Are there more runnable threads than useful execution resources?
- Are workers frequently sleeping and waking for tiny work?
- Does each task cause a wakeup?
- Are multiple runtimes creating independent pools?
- Are threads migrating across CPUs?
- Are blocking tasks occupying a CPU-oriented pool?

Do not assume 100% CPU utilization means useful parallelism.

Do not assume low CPU utilization means more threads are required.

Use threading analysis to distinguish waiting, preemption, and lack of work when tooling supports it.

## 3. Task Granularity

Measure or estimate:

```text
useful work per task
enqueue cost
dequeue cost
allocation cost
synchronization cost
wakeup cost
completion cost
```

A task system can be slower than serial execution when scheduling overhead dominates useful work.

Investigate:

- median task service time
- task-size distribution
- queue operations per unit of useful work
- wakeups per task
- completion synchronization per task

Possible experiments:

- batch multiple operations
- increase chunk size
- fuse adjacent stages
- use per-worker batches
- amortize completion signaling

Do not increase granularity blindly.

Larger tasks can worsen:

- tail latency
- load balance
- cancellation responsiveness
- cache behavior

Measure the tradeoff.

## 4. Load Imbalance

Measure per-worker or per-partition work.

Useful observations:

```text
tasks completed per worker
busy time per worker
idle time per worker
service-time distribution
queue ownership
barrier wait
partition size
```

Example:

```text
worker 0: 100 ms busy
worker 1:  98 ms busy
worker 2: 102 ms busy
worker 3:   3 ms busy
```

Do not average this into "75% utilization" and stop.

Ask:

- Is work statically partitioned?
- Is the input skewed?
- Are some operations much more expensive?
- Is affinity preventing stealing?
- Is one producer feeding workers unevenly?
- Is one serial stage limiting downstream work?

Possible experiments:

- different partitioning
- dynamic scheduling
- work stealing
- smaller chunks
- weighted partitioning

Each changes overhead and locality.

Measure both.

## 5. Queueing And Backpressure

For executors, pools, runtimes, I/O systems, and pipelines, instrument queue behavior.

Observe:

- enqueue rate
- dequeue rate
- queue depth over time
- time in queue
- service time
- active workers
- rejected work
- producer blocking
- batching

Distinguish:

```text
queue delay
+
service time
=
operation time inside the backend
```

A growing queue may mean the service capacity is below offered load.

Adding a faster queue cannot fix an overloaded service stage.

Ask:

- Is arrival rate greater than sustainable service rate?
- Is one stage serial?
- Is queue depth bounded?
- Is backpressure explicit?
- Does batching hide overload until tail latency explodes?

Do not benchmark only at maximum throughput.

Include overload and saturation behavior when relevant.

## 6. Locality And Coherence

Investigate cross-thread handoff and shared-write behavior.

Map important state:

```text
producer CPU
queue ownership
worker CPU
data location
completion CPU
```

Ask:

- How many threads write the same cache line?
- How often does ownership of hot state move between cores?
- Is an operation handed across multiple threads?
- Does a consumer immediately touch data recently written by another CPU?
- Are worker-local data structures truly local?
- Are threads frequently migrated?

Possible design experiments:

- owner-computes
- partition by shard/file/connection/key
- per-worker queues
- local batching
- reduce global counters
- deferred aggregation
- completion on owner thread

Do not force locality when the added routing or imbalance costs more.

Measure.

## 7. False Sharing

False sharing is a layout and access-pattern problem.

Different variables can still share a cache line.

Suspicious patterns include:

```cpp
struct WorkerStats {
    std::atomic<std::uint64_t> completed;
};

std::vector<WorkerStats> stats;
```

when every worker frequently writes its own adjacent element.

Investigate with platform-appropriate evidence.

Ask:

- Which CPUs write the line?
- Are writes to logically independent variables?
- What is the write frequency?
- Is the object array contiguous?
- Is the platform cache-line size known or exposed through a supported facility?

Possible experiments:

- per-thread aggregation with less frequent merge
- different layout
- cache-line separation
- eliminate hot writes

Do not add padding everywhere.

Padding increases memory footprint and can hurt locality.

Use it for a measured false-sharing mechanism.

## 8. Memory Bandwidth And Latency

A scaling plateau may be a memory-system limit rather than synchronization.

Investigate:

- memory bandwidth
- cache miss behavior
- LLC miss rate
- memory-bound metrics
- prefetch behavior
- working-set size
- data layout
- remote NUMA access

Ask:

- Does throughput plateau while CPU threads increase?
- Is bandwidth near platform limits?
- Does per-thread bandwidth collapse?
- Is the workload pointer-chasing?
- Is the working set larger than relevant caches?
- Is the algorithm generating unnecessary memory traffic?

Do not "fix" a bandwidth-bound workload with more worker threads.

## 9. NUMA

Activate NUMA reasoning when the machine or deployment actually has relevant topology.

Record:

- sockets
- NUMA nodes
- CPU-to-node topology
- memory policy
- thread placement
- allocation/first-touch policy

Investigate:

- local versus remote memory
- thread migration
- producer/consumer placement
- shared cross-node state
- interleaved versus local allocation

Possible experiments:

- first-touch placement
- worker/node partitioning
- node-local queues
- node-local allocation
- explicit affinity
- controlled interleaving

Do not claim NUMA effects from a VM where host placement semantics are unknown unless the experiment is explicitly about virtualized behavior.

Do not copy a bare-metal NUMA conclusion to cloud VMs without evidence.

## 10. Oversubscription

Inventory all thread sources:

```text
application threads
backend pools
runtime pools
OpenMP
oneTBB
BLAS
database/client libraries
logging
GC/runtime threads where applicable
```

Compare runnable concurrency with:

- physical cores
- logical CPUs
- CPU quota / cpuset
- container limits

A system with multiple independently sized pools can oversubscribe badly.

Ask:

- Which layer owns scheduling policy?
- Can nested pools execute simultaneously?
- Can a blocking pool starve CPU-bound work?
- Can a library create hidden workers?

Do not size each pool independently as if it owned the whole machine.

## 11. External Serialization

Find serial resources outside the C++ synchronization code.

Examples:

- one file descriptor
- one disk/device queue
- one database connection
- one allocator lock
- one kernel path
- one global logger
- one callback thread
- one downstream stage

A mutex may only make the real bottleneck visible.

Map the full pipeline.

## Hypothesis Format

Before code changes, write:

```text
Observation:
Scaling degrades from T=4 to T=8.

Evidence:
Workers spend 31% of elapsed time waiting on queue_mutex_.
Queue lock average wait and synchronization context switches increase with T.

Hypothesis:
A single shared queue causes producer/consumer contention.

Experiment:
Replace only the queue topology with per-worker queues plus round-robin
submission. Preserve task ownership, completion, and shutdown semantics.

Expected signal:
Lower queue-lock wait and context switches at T=8.
Higher throughput without regression at T=1/T=4.

Reject condition:
If lock wait decreases but throughput does not improve, investigate service
capacity, locality, and load balance rather than declaring success.
```

Do not write:

> optimize thread pool locking

as a performance hypothesis.

## One-Narrow-Change Rule

Prefer one causal experiment per change.

Do not simultaneously:

- split locks
- add padding
- change worker count
- batch tasks
- add affinity
- change the allocator

and then claim one of them caused the result.

When a broad redesign is necessary, establish intermediate evidence points where possible.

Persist raw data.

## Thread Count Is A Tunable Parameter, Not A Constant Of Nature

Do not default to:

```cpp
std::thread::hardware_concurrency()
```

as a universally correct pool size.

Consider:

- blocking ratio
- CPU-bound work
- SMT
- NUMA
- CPU quotas
- co-located workloads
- nested runtimes
- latency goals

Expose or derive policy at the correct architectural layer.

Do not let a low-level backend silently own global scheduling policy when the runtime is supposed to own it.

## Do Not Optimize Away Correctness

Performance work must preserve:

- task acceptance semantics
- ownership
- lifetime
- completion
- error propagation
- shutdown
- cancellation
- public I/O or API contracts

Do not weaken synchronization until the new synchronization graph is explained.

Do not replace a correct mutex design with atomics merely because the mutex appears in a profile.

A lock can be hot because the architecture shares too much state.

Solve the sharing problem when that is the measured cause.

## Lock-Free Escalation Gate

Before suggesting a lock-free replacement, require:

```text
measured bottleneck:
why lock contention is causal:
why partitioning/batching/ownership changes are insufficient:
required progress property:
expected benefit:
benchmark signal:
```

Then activate `cpp-lock-free`.

Do not implement a lock-free queue inside this skill.

## Benchmark Method

Use the repository's benchmark harness where available.

At minimum:

1. build an optimized non-sanitized benchmark configuration
2. retain a correctness-tested configuration separately
3. warm up where appropriate
4. run repeated samples
5. persist raw observations
6. compare distributions or stable summary statistics
7. report environment metadata
8. include thread-count scaling

Do not benchmark TSan/ASan builds as representative performance.

Do not compare Debug and Release builds.

Do not report one unusually good run as the result.

## Measurement Perturbation

Profilers and instrumentation change execution.

Record the tool used.

Distinguish:

- benchmark timing
- sampling profiler evidence
- instrumentation profiler evidence
- tracing
- sanitizer execution

Use low-overhead evidence first when enough.

Use heavier instrumentation for diagnosis.

Re-run the uninstrumented benchmark after a change.

## Metrics By Symptom

### Throughput Stops Scaling

Inspect:

- scaling efficiency
- CPU utilization
- synchronization wait
- context switches
- load imbalance
- memory bandwidth
- queue depth
- external serialization

### Tail Latency Explodes

Inspect:

- queue delay
- queue depth distribution
- batching
- long critical sections
- worker stalls
- preemption
- service-time outliers
- overload

### CPU Is High But Throughput Is Flat

Inspect:

- spinning
- CAS retries
- cache-line bouncing
- false sharing
- memory bandwidth
- useless scheduler work
- polling frequency

### CPU Is Low And Throughput Is Flat

Inspect:

- blocking
- device/service limit
- synchronization wait
- starvation
- serial stage
- insufficient offered load

### Performance Regresses With More Threads

Inspect:

- oversubscription
- contention
- false sharing
- memory bandwidth
- NUMA
- load imbalance
- task granularity

## Performance Review Findings

Label findings as:

- `Measured Bottleneck`
- `Performance Hypothesis`
- `Benchmark Defect`
- `Measurement Gap`
- `Scalability`
- `Contention`
- `Scheduling`
- `Granularity`
- `Queueing`
- `Locality`
- `False Sharing`
- `NUMA`
- `Oversubscription`

Example:

> **Measurement Gap — no scaling matrix**
>
> The benchmark reports only the default eight-worker configuration. There is
> no T=1 baseline or thread-count curve, so the current data cannot distinguish
> parallel benefit from a fixed-cost regression.

Example:

> **False Sharing — measured hot shared line**
>
> Per-worker counters are logically independent, but the profiler attributes
> cross-core contested accesses to the cache line containing adjacent counters.
> Test deferred aggregation or cache-line separation as a narrow experiment.

Do not present an unmeasured suspicion as a measured bottleneck.

## Performance Completion Gate

### Objective

- [ ] Primary performance metric is explicit
- [ ] Workload is representative and frozen
- [ ] Environment metadata is recorded
- [ ] Correctness baseline is green

### Baseline

- [ ] Raw baseline results are persisted
- [ ] T=1 is measured when meaningful
- [ ] A thread-count scaling matrix exists
- [ ] Variance/repetition policy is documented

### Diagnosis

- [ ] Scaling failure or target bottleneck is classified
- [ ] Evidence points to a specific mechanism
- [ ] Alternative explanations are considered
- [ ] Tool perturbation is understood

### Experiment

- [ ] Hypothesis is written before the change
- [ ] Expected metric movement is stated
- [ ] Change is narrow enough for causal interpretation
- [ ] Correctness and shutdown semantics are preserved
- [ ] Lock-free work is escalated to `cpp-lock-free`

### Result

- [ ] Same workload is re-measured
- [ ] Relevant thread counts are re-measured
- [ ] Raw results are persisted
- [ ] Uninstrumented benchmark is run
- [ ] Regressions are reported
- [ ] The result is kept, revised, or reverted according to evidence

## Completion Report

When this skill is active, report:

- performance objective
- environment
- workload
- baseline
- scaling curve
- measured bottleneck evidence
- hypothesis
- code or configuration change
- before/after results
- correctness verification
- remaining uncertainty

Do not claim a concurrency optimization without measurement.
