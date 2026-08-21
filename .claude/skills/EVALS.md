# C++ skill routing evals

Use these as manual/agent regression prompts when changing skill descriptions or workflow. The goal is routing correctness and useful completion behavior, not identical prose.

| Scenario | Expected skills | Must not happen | Success signal |
|---|---|---|---|
| Add a small value-type helper in `src/` with no shared state | `cpp-coding-standards` | performance/lock-free specialist loads | ownership/type/failure review + narrow patch |
| Review a raw-pointer lifetime bug in ordinary C++ | `cpp-coding-standards` | concurrency specialist loads without shared-state reason | lifetime owner/borrow is made explicit |
| Fix a mutex/condition-variable lost-wake bug | `cpp-coding-standards` + `cpp-concurrency-guidelines` | performance optimization becomes the repair | shared-state map + predicate/wake/arming/recheck + deterministic evidence |
| Fix a scheduler liveness bug involving two independent atomics and a generation counter | base + concurrency + `cpp-lock-free` | model fuses observable C++ steps; cross-atomic visibility is assumed | split-step memory-order argument + negative control/deterministic C++ evidence |
| Investigate a throughput regression with a mutex high in a profile | base + `cpp-concurrency-performance`; add concurrency only if synchronization semantics will change | `cpp-lock-free` fires just because a mutex is hot | baseline/scaling/attribution before candidate change |
| Optimize false sharing with stable correctness semantics | base + performance | lock-free skill loads | same workload/environment A/B + layout/cache evidence |
| Implement or audit an MPMC lock-free queue | base + concurrency + lock-free; performance only if optimization is part of task | “no mutex” is treated as sufficient proof | abstract object + linearization + reclamation + memory-order + progress proof |
| TLA+ model has a dead branch but no C++ failure is known | no C++ defect claim from these skills; load specialists only if current C++ protocol is being audited | model bug is relabeled as implementation bug | current C++ facts are recovered before any implementation claim |
| Change only Markdown/docs with no C++ contract review | none of the C++ skills should be required | base C++ skill fires on unrelated prose | no unnecessary skill context |

## Completion-quality probes

When a skill is expected to fire, inspect whether the agent naturally produces or can answer the corresponding completion record without another prompt.

### Base C++

- governing authority;
- change class;
- ownership/lifetime result;
- failure/interface result;
- specialist skills applied;
- evidence actually run;
- residuals.

### Concurrency

- shared-state map;
- state/protocol transitions;
- synchronization edges;
- lifetime/terminal authority;
- wake/progress/shutdown closure;
- deterministic evidence;
- remaining assumptions.

### Performance

- objective and frozen workload;
- scaling evidence;
- attribution classification;
- discriminating experiment;
- symmetric A/B result + noise;
- correctness equivalence and cost vector;
- keep/revise/revert decision.

### Advanced atomics

- entry requirement;
- abstract operation / participant domain;
- linearization point;
- publication and memory-order edges;
- reclamation/ABA;
- progress property;
- negative control / weak-memory evidence;
- cost if relevant.

## Failure patterns to watch

A skill revision regresses if it causes any of these consistently:

- comprehensive reference loaded when the task only needs the procedure;
- specialist skills triggered by weak lexical matches (`mutex` => lock-free, `thread` => performance);
- generic guidance overriding Sluice `AGENTS.md` / ADR contracts;
- a long checklist with no observable done/not-done criterion;
- repeated project facts copied across multiple skills;
- “be careful / be idiomatic / consider X” prose that does not change agent action;
- fixes justified by a green test/model that cannot distinguish the broken behavior.
