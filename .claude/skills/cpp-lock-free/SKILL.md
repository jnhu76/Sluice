---
name: cpp-lock-free
description: Specialist proof workflow for advanced C++ atomics and lock-free algorithms in Sluice. Use when correctness depends on CAS loops, cross-operation memory ordering, linearization, ABA, reclamation, or an explicit lock-free/wait-free/obstruction-free progress property. Do not use merely because code contains atomics.
origin: custom
---

# Advanced atomics / lock-free C++

Use this skill only when ordinary mutex/ownership reasoning is insufficient or the task explicitly concerns an existing advanced atomic protocol. It complements `cpp-concurrency-guidelines`.

## Entry gate

Before implementation, state why advanced atomics are required:

- the existing implementation already depends on them and must be repaired/audited;
- blocking is forbidden by a real contract;
- a progress property is explicitly required;
- measured evidence justifies a lock-free candidate;
- the task is specifically to study a lock-free algorithm.

“Atomics are faster” and “a mutex appears in a profile” are not entry evidence.

**Completion criterion:** the requirement cannot be satisfied more simply without losing a stated contract or measured objective.

## Step 1 — define the abstract operation

Describe the sequential meaning before the atomic fields:

```text
operation:
inputs:
result / failure:
ordering guarantee:
ownership transfer:
```

State the participant domain (`SPSC`, `MPSC`, `SPMC`, `MPMC`, fixed/dynamic participants, migration assumptions) and the required progress property.

**Completion criterion:** reviewers can tell what behavior is being implemented without reading the atomic code.

## Step 2 — identify linearization and publication

For each operation, name the logical point where it takes effect. If helping or multi-step publication makes the point non-local, explain that explicitly.

Then inventory state:

| State | atomic? | initialized by | published by | observed by | lifetime |
|---|---|---|---|---|---|
| ... | ... | ... | ... | ... | ... |

Do not treat “stored in an atomic pointer” as a complete publication argument.

**Completion criterion:** every successful abstract operation has a defensible linearization/publication story.

## Step 3 — write the memory-order proof

For every correctness-relevant observation, write the exact C++ relation:

```text
writer operation:
reader operation:
atomic object:
order on writer:
order on reader:
value/read-from condition:
happens-before consequence:
```

Treat each atomic object as having its own modification order. A read from atomic A does not automatically observe a later write to atomic B. `seq_cst` supplies a global order for seq_cst operations; it does not fuse separate operations into one indivisible transition.

Use the weakest order only after the proof exists. Prefer a stronger, simpler order when the measured cost does not justify proof complexity.

For multi-atomic protocols, enumerate observable intermediate states rather than reasoning as though the source lines were one transaction.

**Completion criterion:** every non-relaxed visibility assumption is tied to a specific synchronizes-with / happens-before / modification-order argument; no cross-atomic visibility is assumed by intuition.

## Step 4 — prove lifetime, reclamation, and ABA

For pointer/reference-based algorithms, state when removed storage may actually be reclaimed.

Name the strategy:

- no reclamation until teardown;
- bounded/static storage;
- hazard pointers;
- epoch/quiescent-state reclamation;
- another proven scheme.

Analyze whether a value can change `A -> B -> A` while another participant retains stale assumptions. If ABA is impossible, explain what invariant, generation/tag, ownership, or non-reuse rule makes it impossible.

Logical removal is not physical reclamation.

**Completion criterion:** no participant can dereference/reuse retired state outside the proven lifetime, and the ABA assumption is explicit.

## Step 5 — prove retry and progress

For each CAS/retry loop, answer:

- what makes one iteration fail;
- whether failure implies someone else progressed;
- whether one participant can starve;
- whether backoff/yield is correctness or only performance;
- what happens during cancellation/shutdown;
- which progress property is actually guaranteed.

Do not call code lock-free merely because it contains no mutex.

**Completion criterion:** the claimed progress property follows from the retry/ownership protocol rather than terminology.

## Step 6 — verify the real C++ protocol

Use multiple evidence layers where appropriate:

1. deterministic C++ regressions for the concrete failure/interleaving;
2. TSan for ordinary data races (supporting evidence, not a memory-order proof);
3. focused litmus/model checking for the actual C++ atomic kernel when practical;
4. TLA+ or another abstract model for protocol exploration, with C++-visible split operations preserved when competitors can observe them;
5. negative controls: restore the broken order/guard/site and require the intended failure/CEX.

A green abstract model does not by itself prove the C++ memory model. Model the repaired **as-built** protocol rather than a cleaner imaginary transaction.

**Completion criterion:** the evidence can distinguish the repaired protocol from at least the relevant pre-fix or weakened variant when such a variant exists.

## Step 7 — account for cost

If an atomic/synchronization hot path changes, measure the relevant cost when practical:

```text
RMW/CAS count:
cache-line sharing:
retry rate:
ns/op or cycles/op:
throughput/latency delta:
noise/range:
```

Correctness is mandatory, but an unnecessary locked RMW or globally contended atomic should not be hidden behind “it is only one instruction.”

## Completion record

Before declaring advanced atomic work complete, state:

```text
entry requirement:
abstract operation / participant domain:
linearization point(s):
publication + memory-order edges:
reclamation / ABA:
progress property:
negative control / deterministic evidence:
weak-memory / formal evidence actually run:
performance cost if relevant:
remaining assumptions:
```

## Detailed reference

`REFERENCE.md` contains the previous comprehensive lock-free handbook, source links, examples, reclamation discussion, memory-order guidance, and detailed review material. Read it on demand for the exact algorithm or proof question being investigated.