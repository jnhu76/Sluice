---
name: cpp-lock-free
description: Advanced C++ atomic and lock-free proof workflow for Sluice. Use when correctness depends on CAS loops, cross-operation memory ordering, linearization, ABA, reclamation, or an explicit lock-free/wait-free/obstruction-free progress property. Do not trigger merely because code contains atomics.
---

# Advanced atomics / lock-free C++

Use this skill when the task audits, repairs, or introduces an advanced atomic protocol. It complements `cpp-concurrency-guidelines` and does not by itself authorize a new lock-free design.

## Entry gate

First classify why this skill is active:

- **existing protocol** — current C++ already depends on advanced atomic semantics and must be audited/repaired;
- **contract** — blocking is forbidden or a non-blocking progress property is explicitly required;
- **performance candidate** — measured evidence justifies evaluating a lock-free/advanced-atomic alternative;
- **research** — the task explicitly studies such an algorithm.

For an audit or repair, the existence of the current advanced-atomic protocol is sufficient to enter this workflow; deciding whether to replace it with a simpler design is a separate architecture decision.

For a new lock-free implementation, simpler synchronization must be ruled out by the governing contract or measured objective before implementation.

**Completion criterion:** the task's entry class and authority are explicit, and a new advanced-atomic design is not being smuggled in by a mere performance intuition.

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

Inventory state:

| State | atomic? | initialized by | published by | observed by | lifetime |
|---|---|---|---|---|---|
| ... | ... | ... | ... | ... | ... |

An atomic pointer/value is not a complete publication argument by itself.

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

Treat each atomic object as having its own modification order. A read from atomic A does not automatically observe a later operation on atomic B. `seq_cst` supplies a global order for seq_cst operations; it does not fuse separate source operations into one indivisible transition.

Choose a weaker memory order only after the proof exists. Prefer a stronger, simpler order when measured cost does not justify extra proof complexity.

For multi-atomic protocols, enumerate observable intermediate states rather than reasoning as though adjacent source lines were one transaction.

**Completion criterion:** every visibility assumption is tied to a specific synchronizes-with / happens-before / modification-order argument; no cross-atomic visibility is assumed by intuition.

## Step 4 — prove lifetime, reclamation, and ABA

For pointer/reference-based algorithms, state when removed storage may actually be reclaimed.

Name the strategy:

- no reclamation until teardown;
- bounded/static storage;
- hazard pointers;
- epoch/quiescent-state reclamation;
- another proven scheme.

Analyze whether a value can change `A -> B -> A` while another participant retains stale assumptions. If ABA is impossible, name the invariant, generation/tag, ownership, or non-reuse rule that prevents it.

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

Absence of a mutex is not itself a lock-free proof.

**Completion criterion:** the claimed progress property follows from the retry/ownership protocol and its stated assumptions.

## Step 6 — verify the real C++ protocol

Use multiple evidence layers where appropriate:

1. deterministic C++ regressions for the concrete failure/interleaving;
2. TSan for ordinary data races (supporting evidence, not a memory-order proof);
3. focused litmus/model checking for the actual C++ atomic kernel when practical;
4. TLA+ or another abstract model for protocol exploration, preserving C++-visible split operations when competitors can observe them;
5. negative controls that restore the relevant broken order/guard/site and require the intended failure/CEX.

A green abstract model does not by itself prove the C++ memory model. Model the repaired **as-built** protocol rather than a cleaner imaginary transaction.

**Completion criterion:** when a relevant pre-fix/weakened variant exists, the evidence distinguishes it from the repaired protocol; otherwise the untested assumption is recorded rather than silently promoted to proof.

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
entry class / authority:
abstract operation / participant domain:
linearization point(s):
publication + memory-order edges:
reclamation / ABA:
progress property + assumptions:
negative control / deterministic evidence:
weak-memory / formal evidence actually run:
performance cost if relevant:
remaining assumptions:
```

## Detailed reference

`REFERENCE.md` preserves the previous long-form lock-free handbook. Search for the exact algorithm/proof/topic heading first and read only the smallest relevant section; do not load the whole handbook by default. Current routing and completion criteria live here.
