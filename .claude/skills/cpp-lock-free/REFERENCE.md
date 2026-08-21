---
name: cpp-lock-free
description: Design, implement, audit, or reason about explicit lock-free C++ and advanced atomic-memory-order algorithms. Use for CAS loops, lock-free/wait-free/obstruction-free structures, ABA, memory reclamation, linearization points, atomic publication, weak memory ordering, hazard/epoch/RCU-style reclamation, and formal concurrency reasoning.
origin: custom
---

# C++ Lock-Free

Lock-free C++ is a specialist domain.

Do not treat atomics as faster mutexes.

Do not activate this skill merely because:

- a lock is contended
- a profiler shows synchronization wait
- a queue is shared
- low latency is desired
- the code "should scale"

Use `cpp-concurrency-guidelines` for normal concurrency correctness.

Use `cpp-concurrency-performance` to establish measured motivation.

This skill begins only after the task explicitly requires advanced atomics or a lock-free progress property.

## Canonical Sources

Use primary sources for semantics:

- the C++ standard or current working draft: https://eel.is/c++draft/
  - data races
  - happens-before
  - atomics
  - atomic ordering
  - compare-exchange
  - fences
- C++ Core Guidelines `CP.free`, including the cautious posture of `CP.100`, `CP.101`, and `CP.102`: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines
- the canonical paper or algorithm specification for the selected lock-free technique
- Clang ThreadSanitizer documentation for dynamic race detection evidence: https://clang.llvm.org/docs/ThreadSanitizer.html
- hardware architecture manuals when reasoning depends on a specific ISA
- formal-model or litmus-test tooling documentation when used

Do not learn a lock-free algorithm from an unattributed code snippet.

Do not invent memory-model guarantees.

## Core Posture

Use the spirit of `CP.100`:

> Do not use lock-free programming unless there is a demonstrated need.

Use `CP.101` and `CP.102` as reminders that compiler/hardware behavior and specialist literature matter.

A lock-free implementation is not complete because:

- it passes tests
- it works on x86
- TSan is clean
- every shared field is atomic
- it uses `compare_exchange_weak`
- it was copied from a known algorithm but modified

The implementation must have explicit arguments for:

- abstract operation semantics
- linearization
- publication
- memory ordering
- progress
- lifetime
- reclamation
- ABA exposure
- failure/retry behavior

## Lock-Free Entry Gate

Before implementation, answer all of the following.

### 1. Why Lock-Free?

State the requirement.

Examples:

- blocking is forbidden in a signal-sensitive path
- a documented progress property is required
- measured lock convoying dominates the target workload
- priority inversion is unacceptable for the contract
- the component is an explicit lock-free research subject

Weak motivations:

- lock-free is modern
- atomics should be faster
- mutexes are slow
- high performance requires lock-free

If performance is the reason, provide measured evidence from `cpp-concurrency-performance`.

### 2. What Is The Abstract Object?

Define the sequential abstraction.

Examples:

```text
stack:
    push(value)
    pop() -> optional<value>

MPSC queue:
    push(value)
    try_pop() -> optional<value>

counter:
    increment()
    load()
```

State:

- valid operations
- return values
- failure states
- ordering guarantees
- ownership transfer

Do not begin from atomic fields.

Begin from the abstract object.

### 3. What Is The Concurrency Domain?

State:

```text
SPSC
MPSC
SPMC
MPMC
```

or another explicit model.

Also state:

- fixed or dynamic participants
- thread registration requirements
- maximum participants if bounded
- thread migration assumptions
- process sharing or in-process only
- signal-handler involvement
- coroutine/task interaction if relevant

An SPSC proof does not generalize to MPMC.

### 4. What Progress Property Is Required?

Choose and define the intended property.

Examples:

- blocking
- obstruction-free
- lock-free
- wait-free

Do not label an algorithm lock-free merely because it contains no mutex.

State:

- system-wide progress guarantee
- per-thread progress guarantee
- starvation possibility
- retry behavior

If only non-blocking API semantics are required, do not automatically require a lock-free algorithm.

### 5. What Is The Linearization Point?

For every public operation, identify the logical instant at which the operation takes effect.

Example:

```text
push:
    successful CAS linking node into head

pop:
    successful CAS moving head from old node to next
```

For operations with helping, descriptors, or multi-step publication, the linearization point may be non-local.

State it precisely.

If the linearization point cannot be identified, do not implement yet.

### 6. What State Is Published?

List ordinary and atomic state.

Example:

```text
Node::value       ordinary
Node::next        atomic or immutable after publication
head              atomic
```

Then state:

- initialization order
- publication operation
- observation operation
- writes that must become visible
- lifetime during observation

Do not assume an atomic pointer automatically publishes a fully initialized pointee under every memory order.

### 7. What Is The Reclamation Strategy?

For pointer-based lock-free structures, this is mandatory.

Choose or identify the actual strategy:

- no reclamation until global teardown
- bounded static storage
- reference counting with a proven algorithm
- hazard-pointer style reclamation
- epoch-based reclamation
- quiescent-state / RCU-style reclamation
- another documented scheme

Do not write:

```cpp
delete old_head;
```

after removing a node from a concurrent pointer structure unless the proof establishes that no other thread can still access it.

Logical removal is not physical reclamation.

If reclamation is deferred to teardown, state the memory-growth and lifecycle consequences.

### 8. What Is The ABA Story?

Ask whether an observed atomic value can change:

```text
A → B → A
```

while a thread retains stale assumptions.

Analyze:

- pointer reuse
- index reuse
- counter wraparound
- freelists
- node recycling
- allocator reuse

Possible techniques may include:

- tagged/versioned state
- delayed reclamation
- hazard-based protection
- wider CAS state
- algorithm-specific avoidance

Do not say:

> pointers are atomic, so ABA is impossible

### 9. What Memory Orders Are Required?

For every atomic operation, record:

```text
atomic object:
operation:
memory order:
role:
required predecessor writes:
required successor reads:
synchronizes-with edge:
why stronger ordering is unnecessary:
why weaker ordering is insufficient:
```

Do this before or together with implementation.

Do not scatter memory orders into code and reverse-engineer the proof later.

### 10. What Is The Failure And Retry Model?

For every CAS loop, analyze:

- expected failure
- spurious failure
- contention failure
- state refresh
- side effects before CAS
- side effects after CAS
- retry allocation
- retry destruction
- backoff
- starvation

Do not perform irreversible side effects before a CAS unless the algorithm accounts for retries.

## Memory Model Reasoning

Use standard terminology carefully.

### Conflicting Actions And Data Races

Ordinary conflicting accesses across threads require a valid synchronization relationship or other standard-defined exclusion.

An actual C++ data race is undefined behavior.

Do not use hardware cache coherence as a substitute for C++ memory-model reasoning.

### Happens-Before

State important happens-before relationships.

Do not draw a happens-before edge merely because one event happened earlier in wall-clock time.

### Synchronizes-With

Identify the exact standard synchronization relation used.

Examples can include:

- mutex unlock to later successful lock on the same mutex
- release/acquire relationships on atomics under the standard's rules
- library synchronization specified by the C++ standard

Do not use "barrier" as a vague synonym for synchronization.

### Sequential Consistency

Sequentially consistent atomics provide a stronger global ordering model for seq_cst operations.

Start with a simple correct ordering when practical.

Weaken ordering only with a documented proof and, when relevant, performance evidence.

### Relaxed Ordering

`memory_order_relaxed` provides atomicity and modification-order properties for the atomic object but does not by itself publish unrelated ordinary state.

Every relaxed operation must state why no cross-object publication ordering is required.

Common legitimate patterns may include:

- approximate statistics
- independent counters
- sequence numbers whose ordering role is separately established

Do not copy these patterns without checking the algorithm.

### Acquire And Release

Use acquire/release reasoning for publication only when the exact standard relationship is established.

Document:

```text
producer ordinary writes
    ↓ sequenced-before
release operation
    ↓ synchronizes-with
acquire operation that observes the required release sequence/value
    ↓ sequenced-before
consumer ordinary reads
```

Do not say:

> acquire/release is faster seq_cst

as the entire justification.

The first question is semantic sufficiency.

### Fences

Treat explicit atomic fences as a high-risk review area.

Prefer expressing synchronization through atomic operations when the algorithm permits.

When a fence is required:

- cite the algorithm or standard reasoning
- identify associated atomic operations
- state the fence synchronization relationship
- add focused tests/litmus reasoning

Do not add a fence as a debugging superstition.

## Compare-Exchange Review

For each `compare_exchange_*` use, answer:

- weak or strong?
- success order?
- failure order?
- is the failure order valid?
- what happens to `expected` on failure?
- is the loop written to consume the updated `expected` value?
- are side effects repeated?
- can retries starve?

Example review shape:

```text
CAS target:
expected state:
desired state:
linearization role:
success ordering:
failure ordering:
retry state refresh:
```

Do not choose `compare_exchange_weak` merely because it appears in lock-free examples.

Use weak in retry loops when its semantics fit.

Use strong when spurious failure would complicate a one-shot decision.

## Immutable-After-Publication State

Prefer immutable node fields after publication when the algorithm permits.

This can simplify reasoning.

Example:

```text
construct node
initialize value
initialize stable links/metadata
publish node
never mutate stable payload again
```

Do not mutate published ordinary fields concurrently without a synchronization story.

## Ownership In Lock-Free Structures

Atomics do not erase ownership.

For every object, distinguish:

- logical ownership
- memory ownership
- temporary observation
- protected observation
- reclamation authority

Example:

```text
queue logically owns linked nodes
consumer removes node logically
reclamation scheme determines when physical deletion is safe
```

Do not represent all four concepts with `Node*` and comments.

Use local wrapper types or project abstractions where they improve the proof without obscuring the algorithm.

## Reclamation Gate

Pointer reclamation requires a separate design section.

Document:

### Protected Observation

How does a thread announce or establish that an observed node cannot be reclaimed?

### Retirement

When is a logically removed node retired?

### Reclamation

What condition proves no thread can access a retired node?

### Thread Exit

What happens when a participating thread exits?

### Registration

Must threads register with the reclamation domain?

### Shutdown

How are retired objects reclaimed at component destruction?

### Boundedness

Can unreclaimed memory grow without bound under a stalled participant?

Do not call a reclamation scheme complete until these questions are answered.

## Hazard-Style Reasoning

When using a hazard-pointer-style technique, analyze the conceptual protocol:

```text
load candidate
publish protection
re-check shared pointer
retry if candidate changed
access protected node
clear protection
retire removed node
scan protected set before reclaim
```

Do not omit the re-check step unless the selected documented algorithm proves it unnecessary.

Do not assume the term "hazard pointer" makes a custom implementation correct.

Prefer a vetted project/library implementation when available and compatible.

## Epoch-Style Reasoning

When using epoch-based reclamation, analyze:

- participant registration
- entry into read-side critical sections
- quiescent state
- epoch advancement
- stalled participants
- retirement lists
- shutdown
- memory growth

A stalled participant can affect reclamation.

State the operational consequence.

## RCU-Style Reasoning

When using an RCU or quiescent-state scheme, follow the exact implementation's documented semantics.

Do not generalize Linux-kernel RCU semantics to an unrelated user-space library.

Identify:

- read-side critical section
- grace period
- updater behavior
- callback/reclamation mechanism
- thread registration and teardown

## Bounded Index-Based Structures

For bounded queues/rings using indexes and sequence numbers, analyze:

- capacity invariant
- wraparound
- sequence width
- counter overflow assumptions
- slot ownership
- producer/consumer publication
- false sharing
- progress under contention

Do not assume unsigned wraparound alone proves correctness.

State how stale observations are distinguished from current slot generations.

## Progress Analysis

### Lock-Free

Show why, under contention, failure of one operation implies that some operation made progress.

A CAS loop is not automatically lock-free if it depends on a blocking allocator, blocking reclamation path, or external lock in the required operation.

State the scope of the progress claim.

Example:

> The queue mutation is lock-free, but node allocation is outside the progress
> guarantee.

Be precise.

### Wait-Free

A wait-free claim requires a bounded-step argument for each participating operation under the stated model.

Do not use "wait-free" to mean "doesn't call wait()".

### Starvation

A lock-free algorithm may still starve one participant.

State whether starvation is possible.

Review fairness assumptions.

## Hardware And Compiler Boundary

The C++ abstract machine is the primary language-level contract.

When the algorithm depends on ISA-specific behavior:

- state the supported architectures
- cite architecture documentation
- isolate architecture-specific code
- preserve compiler constraints
- test each supported target

Do not reason:

> x86 is strongly ordered, therefore this C++ data race is safe.

Compiler transformations remain governed by the C++ program semantics.

Do not use `volatile` for inter-thread synchronization.

Use `CP.8` as a guideline signal.

## False Sharing And Atomic Hotspots

Lock-free can perform worse because of:

- one hot atomic
- cache-line ownership bouncing
- failed CAS retries
- excessive fences
- helping traffic
- reclamation metadata
- backoff defects

Measure:

- throughput scaling
- CAS attempts
- CAS failures
- retries per operation
- atomic hot lines
- contested cache accesses
- CPU utilization
- tail latency

Do not claim lock-free is faster without measurement.

Activate `cpp-concurrency-performance` for the experiment.

## Backoff

Do not add exponential backoff, pause loops, yield, or sleep by folklore.

For a contention loop, measure:

- retry rate
- threads
- latency
- throughput
- CPU consumption

A backoff policy changes:

- fairness
- tail latency
- CPU usage
- throughput

Treat it as a performance experiment.

It does not repair an incorrect algorithm.

## Testing Strategy

Tests cannot prove a lock-free algorithm correct.

Still use multiple evidence layers.

### Sequential Contract Tests

Verify abstract operations against the sequential specification.

### Concurrent History Tests

Record operation histories where practical.

Check invariants and observable outcomes.

For suitable data structures, use a linearizability checker or equivalent history analysis when available.

### Forced Interleavings

Use test hooks or controlled phases to force meaningful algorithm edges.

Examples:

```text
T1 loads old head
T2 removes old head
T2 retires/reuses state
T1 attempts CAS
```

Use this to exercise ABA and reclamation scenarios.

### Stress Tests

Vary:

- producer count
- consumer count
- operation mix
- allocation pressure
- thread churn
- long-lived stalled participant
- wraparound pressure for bounded indexes
- repeated construction/destruction

Assert invariants.

### Sanitizers

Run TSan where supported.

Understand that TSan is not a proof of:

- linearizability
- progress
- correct weak-memory ordering in unexecuted paths
- correct reclamation
- ABA freedom

Use ASan/UBSan where relevant for lifetime and undefined behavior.

Do not mix sanitizer performance with benchmark claims.

### Architecture Matrix

When the project supports multiple architectures and memory ordering is non-trivial, test relevant targets.

Do not validate only x86 and declare ARM behavior proven.

Use actual target execution, emulation with stated limitations, or formal/litmus tooling appropriate to the claim.

## Formal And Litmus Reasoning

For non-trivial memory-order algorithms, consider:

- litmus tests
- model checkers
- linearizability checking
- algorithm-specific proof artifacts

The exact tool depends on the algorithm and project.

Do not write pseudo-formal comments that merely restate code.

Useful proof notes identify:

- invariant
- publication edge
- linearization point
- reclamation condition
- progress argument

Keep proof notes near the implementation or in a referenced design document.

## Lock-Free Design Document Template

Before a new non-trivial lock-free structure, produce:

```text
# Abstract object

Operations:
Semantics:

# Concurrency domain

SPSC/MPSC/SPMC/MPMC:
Participants:
Registration:

# Progress property

Claim:
Scope:
Blocking dependencies:
Starvation:

# State

Atomic state:
Ordinary state:
Immutable-after-publication state:

# Linearization points

operation A:
operation B:

# Publication graph

writer:
release:
acquire:
reader:

# Memory orders

atomic | operation | order | role | justification

# ABA

Exposure:
Mitigation:

# Reclamation

Protection:
Retirement:
Reclamation:
Thread exit:
Shutdown:
Boundedness:

# Retry behavior

CAS loops:
Backoff:
Side effects:

# Verification

Sequential tests:
Concurrent histories:
Forced interleavings:
Stress:
Sanitizers:
Architecture matrix:
Formal/litmus evidence:

# Performance motivation

Measured bottleneck:
Expected benefit:
Benchmark:
```

Do not skip the design document for a novel MPMC or reclamation algorithm.

A trivial independent atomic counter does not require this full template.

Use judgment.

## Lock-Free Review Findings

Use labels:

- `Memory Model`
- `Linearizability`
- `Progress`
- `ABA`
- `Reclamation`
- `Publication`
- `CAS Loop`
- `Atomic Ordering`
- `Lifetime`
- `Architecture Assumption`
- `Measurement Gap`
- `Project Contract`

Prioritize:

1. data race / undefined behavior
2. use-after-free or unsafe reclamation
3. missing publication
4. incorrect linearization
5. ABA defect
6. invalid progress claim
7. incorrect CAS loop
8. unjustified memory ordering
9. architecture-specific assumption
10. performance issue

Example:

> **Reclamation — unsafe physical deletion**
>
> `pop()` deletes `old_head` immediately after the successful CAS. Another
> consumer can still hold `old_head` from an earlier load and dereference it
> before its CAS. Logical removal does not prove that physical reclamation is
> safe.

Example:

> **Atomic Ordering — relaxed publication defect**
>
> The producer initializes `node->value` and then publishes `node` with a
> relaxed store. The consumer uses a relaxed load and reads `value`. The
> algorithm requires publication of ordinary node initialization, but no
> corresponding synchronization edge is established.

## Lock-Free Rejection Conditions

Stop implementation and report the design gap when:

- the abstract semantics are unclear
- the concurrency domain is unspecified
- the progress property is used as marketing language
- no linearization point can be identified
- pointer reclamation is hand-waved
- ABA exposure is ignored
- explicit weak memory orders have no synchronization graph
- the algorithm was copied and materially modified without re-deriving the proof
- the implementation depends on undocumented hardware behavior
- performance motivation exists but no measurement supports it

Do not fill proof gaps with confidence.

## Lock-Free Completion Gate

### Requirement

- [ ] Lock-free/advanced atomic work is explicitly required
- [ ] Motivation is documented
- [ ] Performance motivation has measured evidence when applicable

### Abstract Semantics

- [ ] Sequential abstraction is defined
- [ ] Operations and return semantics are defined
- [ ] Ownership transfer is defined
- [ ] Concurrency domain is explicit

### Progress

- [ ] Progress property is named correctly
- [ ] Scope of the progress claim is explicit
- [ ] Blocking dependencies are listed
- [ ] Starvation behavior is understood

### Linearization

- [ ] Every public operation has a linearization argument
- [ ] Helping/multi-step operations are analyzed
- [ ] Observable histories match the abstract object

### Memory Model

- [ ] Shared atomic and ordinary state are inventoried
- [ ] Publication graph is documented
- [ ] Every weak memory order is justified
- [ ] Relaxed operations explicitly state why no publication is required
- [ ] Fences have formal reasoning
- [ ] CAS success/failure orders are valid and justified

### Lifetime

- [ ] Ownership is explicit
- [ ] Logical removal is separated from physical reclamation
- [ ] Reclamation strategy is documented
- [ ] Thread exit is handled
- [ ] Shutdown reclamation is handled
- [ ] Memory growth under stalled participants is understood

### ABA

- [ ] ABA exposure is analyzed
- [ ] Reuse/wraparound is considered
- [ ] Mitigation is documented where required

### Verification

- [ ] Sequential contract tests pass
- [ ] Concurrent invariant/history tests exist
- [ ] Critical interleavings are forced where practical
- [ ] Stress matrix covers the concurrency domain
- [ ] TSan is run where supported
- [ ] ASan/UBSan are run where relevant
- [ ] Relevant architecture targets are considered
- [ ] Formal/litmus evidence is used for non-trivial ordering when appropriate

### Performance

- [ ] Lock-free is not assumed faster
- [ ] Scaling and latency are measured when performance is claimed
- [ ] CAS retry/failure behavior is observed
- [ ] Cache-line contention is considered
- [ ] Comparison uses the same workload and environment

## Completion Report

When this skill is active, report:

- why lock-free or advanced atomics are required
- abstract object semantics
- concurrency domain
- progress property
- linearization points
- publication and memory-order model
- ABA analysis
- reclamation strategy
- verification evidence
- performance evidence when relevant
- unresolved proof or portability limitations

Never claim a lock-free algorithm is correct when a proof obligation remains unresolved.
