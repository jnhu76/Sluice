---
name: cpp-concurrency-guidelines
description: Design, write, review, or refactor correct multithreaded C++ using the C++ memory model, C++ Core Guidelines CP rules, explicit ownership, lifecycle, synchronization, and shutdown semantics. Use for threads, tasks, worker pools, queues, mutexes, condition variables, atomics, concurrent backends, and shared state.
origin: custom
---

# C++ Concurrency Guidelines

Write concurrent C++ as a concurrency design, not as several threads mutating shared state.

This skill is primarily about:

- correctness
- ownership
- lifetime
- synchronization
- publication
- shutdown
- cancellation
- liveness
- reviewability

It is not primarily a concurrency performance tuning skill.

It does not authorize speculative lock-free programming.

## Canonical Sources

Use these as primary references when exact semantics or rule IDs matter:

- C++ Core Guidelines, especially `CP.*`: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines
- the current C++ working draft memory-model clauses: https://eel.is/c++draft/intro.races and https://eel.is/c++draft/atomics.order
- Clang Thread Safety Analysis documentation: https://clang.llvm.org/docs/ThreadSafetyAnalysis.html
- Clang ThreadSanitizer documentation: https://clang.llvm.org/docs/ThreadSanitizer.html
- SEI CERT C++ concurrency rules where applicable: https://cmu-sei.github.io/secure-coding-standards/sei-cert-cpp-coding-standard/

Verify exact Core Guidelines and CERT rule IDs before citing them.

Do not invent rule IDs.

## Authority Order

When sources conflict, use this order:

1. the C++ language and library semantics for the project's supported standard
2. accepted project contracts and ADRs
3. documented public API semantics
4. C++ Core Guidelines and applicable CERT rules
5. this skill's design heuristics
6. current implementation behavior

A guideline preference does not silently override a documented project contract.

Report conflicts.

## Core Principle

Concurrency correctness is not established by the presence of:

- `std::mutex`
- `std::atomic`
- `std::condition_variable`
- `std::jthread`
- a thread-safe container
- a lock-free queue
- ThreadSanitizer passing once

A concurrency design is reviewable only when it is possible to identify:

- the unit of concurrency
- thread or task ownership
- shared state
- writers and readers
- synchronization edges
- lifetime boundaries
- waiting predicates
- shutdown semantics
- failure behavior

Prefer designs that make those relationships local and explicit.

## Prefer Tasks And Ownership Over Raw Threads

Use `CP.4` as a design signal: think in terms of tasks rather than threads.

Before deciding how many threads to create, identify the work unit.

Examples:

- operation
- request
- job
- task
- partition
- shard
- pipeline stage
- actor message
- blocking syscall
- completion

Do not begin a design with:

> We need N threads.

Begin with:

> What work exists, who owns it, what state does it need, and what execution mechanism is required?

Threads are an execution resource.

They are not the domain model.

## Concurrency Design Synthesis Gate

Before implementing a significant concurrent component, perform this gate.

Do not skip directly to mutex placement.

### 1. Identify The Unit Of Concurrency

State the independently scheduled or coordinated work.

Ask:

- What is one work item?
- Can work items execute independently?
- What ordering constraints exist?
- Is affinity required?
- Is work CPU-bound, blocking, or mixed?
- Can work suspend or block?
- What defines completion?

Examples:

```text
thread pool:
    unit = submitted task

blocking I/O backend:
    unit = blocking I/O operation

partitioned join:
    unit = partition task

actor:
    unit = message handling step
```

Do not confuse a worker thread with a work item.

### 2. Inventory Shared State

Build a shared-state map before writing synchronization code.

Use a table when the component is non-trivial:

| State | Owner | Readers | Writers | Protection / Publication |
|---|---|---|---|---|
| queue | pool | workers | submitters | mutex |
| stopping | pool | workers, submitters | owner | same mutex |
| active count | pool | shutdown | workers | same mutex |
| completion state | operation | waiter | worker | operation synchronization |

Every mutable shared object should have a concurrency story.

For each item, ask:

- Is sharing required?
- Can ownership be transferred instead?
- Can state be partitioned?
- Can mutation be thread-confined?
- Can an immutable snapshot be published?
- Can the state live inside the object whose lock protects it?

Use `CP.3` as a design principle: minimize explicit sharing of writable data.

Prefer eliminating sharing over adding more synchronization.

### 3. Identify Thread And Task Ownership

For every thread or long-lived worker, identify:

- creator
- owner
- start point
- stop request
- join or wait point
- destruction point

For every submitted task, identify:

- task owner before submission
- ownership transfer at submission
- captured state
- completion owner
- whether cancellation can occur
- whether referenced state may die before completion

A thread without an owner is a design defect.

A task whose captures cannot be explained is a lifetime risk.

Treat joining threads as scoped resources in the spirit of `CP.23`.

Do not detach threads in normal library or application code.

Use `CP.26` as the default: avoid `detach()`.

If process-lifetime detached execution is an explicit architecture choice:

1. document it
2. identify all process-lifetime state
3. prevent access to shorter-lived objects
4. test teardown behavior where teardown is expected

### 4. Define The Object Invariant

For each concurrent abstraction, state what is always true while the object is valid.

Examples:

```text
WorkerPool invariant:
- workers_ are owned by the pool
- queue_ and state_ are protected by mutex_
- each accepted task is queued, active, completed, or explicitly canceled
- destruction does not return while a worker can access pool state
```

Do not use synchronization to compensate for an undefined invariant.

Avoid designs where callers must remember undocumented thread-affinity or call-order rules.

If thread affinity is required, expose and document it explicitly.

### 5. Define The Lifecycle State Machine

Concurrent objects frequently fail during transitions rather than steady state.

Model states explicitly.

Example:

```text
constructing
    ↓
running
    ↓ request_stop
stopping
    ↓ drain/join
stopped
```

For each transition, define:

- who initiates it
- which lock or synchronization protects it
- whether new work is accepted
- what happens to queued work
- what happens to active work
- what wakes waiting threads
- what completion means

Do not treat shutdown as destructor cleanup trivia.

Shutdown is part of the concurrent protocol.

### 6. Analyze Submission Versus Shutdown

For pools, executors, queues, backends, and runtimes, explicitly analyze:

```text
submit || shutdown
```

Answer:

- Can submission race with shutdown?
- What is the linear decision point for accept versus reject?
- Can accepted work be lost?
- Can rejected work appear accepted?
- Does shutdown drain queued work?
- Does shutdown cancel queued work?
- Can active work continue?
- What does the API report?

The acceptance decision and queue insertion often need one synchronization story.

Do not use an atomic `stopping` flag as a substitute for reasoning about queue state.

### 7. Build The Synchronization Graph

For non-trivial shared state, identify the publication and observation edges.

Ask:

- What operation publishes the state?
- What operation observes it?
- What establishes `synchronizes-with`?
- What therefore establishes `happens-before`?
- Which ordinary non-atomic writes rely on that edge?

Examples:

```text
producer:
    lock(m)
    mutate queue
    unlock(m)

consumer:
    lock(m)
    inspect queue

The mutex synchronization protects and publishes queue state.
```

For atomics:

```text
producer:
    write payload
    flag.store(true, release)

consumer:
    if flag.load(acquire):
        read payload
```

Only claim publication when the required memory-model relationship exists.

Do not use phrases such as:

> atomic makes it thread-safe

Specify what is made atomic or synchronized.

### 8. Justify Every Atomic

For each atomic object, answer:

1. What invariant or synchronization role does it serve?
2. Which threads access it?
3. Is only atomicity required?
4. Is it publishing other state?
5. Is there related non-atomic state?
6. Would a mutex make the invariant easier to express?
7. Is it a hot shared write location?
8. Is the atomic part of a lock-free algorithm?

Do not introduce atomics merely to avoid a mutex.

Do not split one logical invariant across unrelated atomics without a proof of how readers observe a consistent state.

An atomic flag does not automatically protect surrounding state.

### 9. Justify Non-Default Memory Ordering

The default for ordinary `std::atomic` operations is sequentially consistent ordering.

Do not weaken memory order from the default merely because weaker ordering may be faster.

Any explicit use of:

- `memory_order_relaxed`
- `memory_order_acquire`
- `memory_order_release`
- `memory_order_acq_rel`

requires a concise memory-order justification.

Record:

```text
atomic:
writer operation:
reader operation:
published state:
required ordering:
why the selected ordering is sufficient:
why weaker ordering is insufficient:
```

For `memory_order_relaxed`, state explicitly:

> This operation requires atomicity/order on the atomic object only and does not publish unrelated state.

If that statement is false or unclear, do not use relaxed ordering.

Escalate explicit non-trivial memory-order algorithms to the `cpp-lock-free` skill when appropriate.

### 10. Design Condition-Variable Predicates

Use `CP.42`: do not wait without a condition.

For every condition-variable wait, identify the predicate.

The predicate must describe shared state.

Example:

```cpp
cv_.wait(lock, [this] {
    return stopping_ || !queue_.empty();
});
```

Then identify:

- the mutex protecting predicate state
- every writer that can make the predicate true
- the notification point
- whether notification occurs before or after unlock and why
- behavior during shutdown
- behavior after spurious wakeup

Do not treat notification as stored state.

The predicate is the state.

The notification is only a wakeup mechanism.

Use CERT concurrency guidance for spurious wakeups and condition-variable liveness where applicable.

### 11. Review Locking As An Invariant

Use RAII for lock ownership in the spirit of `CP.20`.

Prefer:

- `std::lock_guard`
- `std::unique_lock`
- `std::scoped_lock`

according to required semantics.

Avoid manual `lock()` / `unlock()` in ordinary code.

When multiple mutexes must be acquired together, use a deadlock-safe mechanism such as `std::scoped_lock` when compatible with the design.

Review lock ordering.

Document a predefined lock order when nested lock acquisition is unavoidable.

Use applicable CERT deadlock guidance.

### 12. Do Not Call Unknown Code While Holding A Lock

Use `CP.22`.

Unknown code includes:

- callbacks
- virtual calls into external implementations
- user-provided function objects
- completion handlers
- logging hooks with unknown locking
- plugin code

Before invoking unknown code, ask:

- Can it re-enter this object?
- Can it acquire this lock?
- Can it block?
- Can it destroy related state?

Prefer:

1. establish state under the lock
2. release the lock
3. invoke external code

Do not blindly move calls outside locks when the state transition requires atomicity.

Redesign the transition if necessary.

### 13. Distinguish Thread Safety From Reentrancy

A thread-safe function is not automatically reentrant.

Ask whether callbacks or nested calls can re-enter the same object.

Review:

- recursive mutex temptations
- callback-under-lock
- observer notification
- completion delivery
- destructors triggered while locked

Do not use `std::recursive_mutex` as the default repair for unclear reentrancy.

First identify the call cycle.

### 14. Distinguish Blocking From Synchronization

A correct concurrent abstraction should state where blocking can occur.

Examples:

- queue wait
- thread join
- filesystem syscall
- socket operation
- future wait
- condition variable
- external callback

Do not hide potentially unbounded blocking behind an interface that appears cheap without documentation.

For I/O systems, keep separate:

- I/O contract semantics
- blocking behavior
- execution backend
- runtime scheduling policy

A thread pool can move blocking work.

It does not make the underlying operation non-blocking.

### 15. Define Failure And Exception Behavior

Follow the project's error model.

For exceptions, analyze:

- can a worker entry function throw out of the thread?
- who records task failure?
- how is failure delivered?
- does cleanup remain automatic?
- can a failed task corrupt pool state?

Do not allow an exception to escape a top-level `std::thread` function.

For non-exception error models, define equivalent propagation and completion behavior.

A task failure must not silently disappear.

### 16. Define Shutdown Semantics

For every concurrent owner, answer:

```text
new work during shutdown:
queued work:
active work:
blocked workers:
waiting callers:
callbacks:
completion objects:
threads:
destructor:
```

Use one of the documented policies, for example:

- drain accepted work
- cancel queued work and finish active work
- cooperative cancellation
- process-lifetime owner

Do not leave policy implicit.

Destructor behavior must match the documented shutdown model.

Do not let a worker access owner state after destruction begins unless the lifetime model explicitly guarantees safety.

### 17. Prefer Structured Lifetime

Prefer concurrent work whose lifetime is nested inside an owner scope.

Use C++20 `std::jthread` and `std::stop_token` when they fit the project's standard, architecture, and cancellation model.

Do not recommend them mechanically.

They do not solve:

- task cancellation semantics
- queue draining
- shared-state invariants
- completion delivery
- work accepted during shutdown

A stop request is a mechanism.

It is not a shutdown policy.

## Review Rules

### Data Races

Use `CP.2`: avoid data races.

In C++, a data race is not a benign performance issue.

Treat an actual data race as a correctness defect.

Review all conflicting accesses to shared memory.

Do not reason from observed hardware behavior alone.

### Writable Sharing

Use `CP.3`: minimize explicit sharing of writable data.

Prefer:

- ownership transfer
- partitioning
- per-worker state
- immutable snapshots
- message passing
- reduction

before introducing a highly contended shared structure.

### Tasks

Use `CP.4`: think in terms of tasks rather than threads.

Do not expose worker-thread identities to ordinary application logic unless affinity or thread-local semantics are part of the contract.

### Locks

Use `CP.20` for RAII lock ownership.

Use `CP.21` when acquiring multiple mutexes.

Use `CP.22` to avoid unknown code under lock.

Use `CP.43` as a review signal for long critical sections, but do not break invariants merely to shorten a lock scope.

Correctness comes first.

### Waiting

Use `CP.42`: wait on a predicate.

Review spurious wakeups.

Review lost-liveness scenarios.

Review whether every state transition that can satisfy a predicate wakes appropriate waiters.

### Threads

Use `CP.23` and `CP.26` as lifecycle design signals.

A raw `std::thread` member is a resource.

Review who joins it.

Prefer an ownership abstraction that makes thread lifetime explicit.

### Lock-Free

Use `CP.100` as the default posture:

> Do not use lock-free programming unless it is required.

For explicit lock-free work, activate `cpp-lock-free`.

## Concurrency Anti-Patterns

Treat these as strong review signals.

### Threads Sharing A Random Context

```cpp
struct Context {
    std::vector<Job> jobs;
    bool stopping;
    int active;
    std::mutex mutex;
};

void worker(Context*);
void submit(Context*, Job);
void shutdown(Context*);
```

The problem is not that free functions are forbidden.

The problem is that ownership, invariant, acceptance, shutdown, and task completion may be only procedural conventions.

Ask whether the state belongs to a concurrent abstraction with a defined invariant and lifetime.

### Atomic Flag As Global Thread-Safety Repair

```cpp
std::atomic<bool> stopping;
std::vector<Job> jobs;
```

`stopping` being atomic says nothing about concurrent access to `jobs`.

Identify synchronization for every shared object.

### Busy Wait By Default

```cpp
while (!ready.load()) {
}
```

Do not introduce spinning by default.

Ask:

- expected wait duration
- CPU availability
- oversubscription
- latency requirement
- backoff/yield policy
- measurement evidence

Spin policy belongs in performance work after correctness is established.

### Detached Worker Capturing `this`

```cpp
std::thread([this] { run(); }).detach();
```

Reject by default.

The lifetime of `this` is not tied to the detached thread.

### Callback Under Lock

```cpp
std::lock_guard lock{mutex_};
state_ = State::done;
callback_();
```

Review reentrancy, unknown locking, blocking, and destruction.

### Split Invariant Across Atomics

```cpp
std::atomic<int> state;
std::atomic<std::size_t> count;
```

Do not assume two atomics provide an atomic snapshot of one logical state.

### Boolean Lifecycle Soup

```cpp
bool started;
bool stopping;
bool stopped;
bool failed;
```

Ask whether the lifecycle should be one explicit state machine.

### Sleep-Based Synchronization

```cpp
std::this_thread::sleep_for(10ms);
assert(done);
```

Do not use sleep as correctness synchronization.

Use deterministic coordination.

Sleep may be appropriate in stress or timing tests only when the test explicitly studies time behavior.

## Concurrency Review Output

For substantive findings, use one of these labels:

- `C++ Memory Model`
- `Core Guideline`
- `CERT Concurrency`
- `Concurrency Design`
- `Project Contract`
- `Tool Diagnostic`

Examples:

> **Concurrency Design — shutdown race**
>
> `submit()` reads `stopping_` before locking `queue_mutex_`, while shutdown
> changes the state and drains the queue under the mutex. The acceptance
> decision and queue insertion do not form one synchronization protocol, so a
> task can be accepted after the drain decision.

> **C++ Memory Model — publication**
>
> `ready_` uses a relaxed store/load but is also expected to publish ordinary
> writes to `payload_`. Relaxed ordering does not provide that publication
> relationship.

Prioritize:

1. data races
2. lifetime and use-after-destruction
3. broken shutdown or acceptance semantics
4. missing publication / invalid memory ordering
5. deadlock and liveness
6. condition-variable predicate defects
7. unknown code under lock
8. weak ownership or invariant design
9. unnecessary sharing
10. local concurrency style issues

Do not bury critical lifetime defects under dozens of minor lock-style comments.

## Mechanical Enforcement

Use available tooling.

### Clang Thread Safety Analysis

When the project uses Clang and annotations are appropriate, consider:

- `GUARDED_BY`
- `REQUIRES`
- capability annotations
- `-Wthread-safety`

Use these to encode lock-based invariants.

Do not force annotations into a project without considering compiler portability and repository policy.

### ThreadSanitizer

Run TSan for changes involving:

- new shared mutable state
- synchronization changes
- worker pools
- thread lifecycle
- condition variables
- atomics where TSan can observe relevant races

TSan is a dynamic race detector.

A clean TSan run does not prove:

- absence of unexecuted races
- deadlock freedom
- starvation freedom
- correct memory-order reasoning
- correct shutdown semantics
- linearizability

Use it as evidence, not proof.

### Stress Tests

Create stress tests for interleavings relevant to the contract.

Examples:

- many submitters
- concurrent submit and shutdown
- empty shutdown
- shutdown with queued work
- shutdown with active work
- worker failure
- repeated construct/destroy
- high iteration count
- randomized scheduling pressure

Stress tests should assert invariants and completion.

Do not merely check that the process did not crash.

### Deterministic Lifecycle Tests

Prefer barriers, latches, promises, test hooks, or controlled blocking points to force important interleavings.

For a shutdown race, explicitly force:

```text
submit observes state
shutdown begins
submit attempts queue insertion
```

or the equivalent meaningful protocol points.

Do not rely only on probabilistic reproduction for a known lifecycle edge.

## Concurrency Completion Gate

Before marking concurrent C++ work complete, answer:

### Design

- [ ] What is the unit of concurrency?
- [ ] What mutable state is shared?
- [ ] Can any sharing be removed?
- [ ] Who owns every thread?
- [ ] Who owns every task before and after submission?
- [ ] What is the object invariant?
- [ ] What is the lifecycle state machine?

### Synchronization

- [ ] What publishes shared state?
- [ ] What observes it?
- [ ] What establishes the required happens-before relationships?
- [ ] Does every atomic have a stated role?
- [ ] Is every explicit weak memory order justified?
- [ ] Does every condition-variable wait have a predicate?
- [ ] Is the predicate protected consistently?
- [ ] Is lock order safe?
- [ ] Is unknown code called outside locks where required?

### Shutdown

- [ ] Can submission race with shutdown?
- [ ] Is accept versus reject decided atomically with the relevant state?
- [ ] What happens to queued work?
- [ ] What happens to active work?
- [ ] Who wakes blocked workers?
- [ ] Who joins workers?
- [ ] Can any worker or callback outlive referenced state?
- [ ] Does destructor behavior match the shutdown contract?

### Failure

- [ ] Can worker exceptions escape?
- [ ] How is task failure represented?
- [ ] Can failure corrupt shared state?
- [ ] Are waiting callers completed on every terminal path?

### Evidence

- [ ] Focused concurrency tests pass
- [ ] Important lifecycle interleavings are tested
- [ ] TSan is run when supported and relevant
- [ ] Static thread-safety analysis is used when configured
- [ ] Full relevant test suite passes

## Completion Report

When this skill is active, report:

- unit of concurrency
- shared-state model
- thread/task ownership
- synchronization model
- lifecycle and shutdown policy
- atomic and memory-order decisions
- failure propagation
- verification performed
- unresolved concurrency limitations

Do not claim a concurrent component is safe merely because tests pass.
