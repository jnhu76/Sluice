# C++ skill routing evals

Use these as manual/agent regression prompts when changing skill descriptions or workflow. The goal is routing correctness and useful completion behavior, not identical prose.

| Scenario | Expected skills | Must not happen | Success signal |
|---|---|---|---|
| Add a small value-type helper in `src/` with no shared state | `cpp-coding-standards` | performance/lock-free specialist loads | ownership/type/failure review + narrow patch |
| Review a raw-pointer lifetime bug in ordinary C++ | `cpp-coding-standards` | concurrency specialist loads without shared-state reason | lifetime owner/borrow is made explicit |
| Format a small C++ diff | `cpp-coding-standards` | generic Core Guidelines invent a competing style | `.clang-format` is treated as mechanical style authority and formatting stays local |
| Generic C++ guidance conflicts with an Accepted ADR | `cpp-coding-standards` | skill invents its own precedence order | agent follows `AGENTS.md §2` authority chain and reports the conflict |
| Review a one-call-site helper that isolates an I/O error or lifetime boundary | `cpp-coding-standards` | agent deletes it solely for having fewer than two call sites | call-site and protected invariant are checked before KEEP/SIMPLIFY |
| Consolidate duplicated wrappers around an existing standard-library operation | `cpp-coding-standards` | speculative reusable framework replaces simple code | real callers and contract-equivalent minimum change are identified |
| Rename touched internal byte counts and nanosecond durations | `cpp-coding-standards` | whole-repo naming churn or public/ABI renaming | changed names expose quantities and units without contract changes |
| Remove a supposedly redundant request counter shared across threads | base + `cpp-concurrency-guidelines` | “derived” is treated as proof it can be deleted | authority, physical obligation, publication and legal interleavings decide the outcome |
| Simplify a defensive branch in Linux close or accepted-work shutdown | base; add concurrency if shared-state protocol changes | generic anti-defensive advice deletes a required failure/lifetime path | concrete failure or invariant oracle distinguishes safe simplification from KEEP |
| Delete two tests that execute the same code path | `cpp-coding-standards` | coverage or similar lines are treated as proof of redundancy | each unique regression caught by the tests is named before deletion |
| Apply an upstream Python-only rule such as mandatory `else` or file-length limits to C++ | `cpp-coding-standards` | a second formatting policy or mechanical rewrite is introduced | language-specific rule is rejected; existing repository configuration remains authoritative |
| Fix a mutex/condition-variable lost-wake bug | base + `cpp-concurrency-guidelines` | performance optimization becomes the repair | shared-state map + predicate/wake/arming/recheck + deterministic evidence |
| Review a wait protocol that depends on a documented external event | base + concurrency | agent demands unconditional finite termination or treats one success path as liveness proof | external/fairness assumption is named and closed stuck states are checked under that assumption |
| Fix a scheduler liveness bug involving two independent atomics and a generation counter | base + concurrency + `cpp-lock-free` | model fuses observable C++ steps; cross-atomic visibility is assumed | split-step memory-order argument + negative control/deterministic C++ evidence |
| Audit an existing CAS-heavy protocol even though a mutex redesign might be simpler | base + concurrency + lock-free | lock-free entry gate rejects the audit because a simpler redesign could exist | existing protocol is analyzed first; redesign is treated as a separate architecture decision |
| Design a contention benchmark before baseline data exists | base + `cpp-concurrency-performance` | performance skill fails to trigger or production optimization proceeds without measurements | experiment contract is defined, missing measurements are explicit, code optimization remains gated |
| Investigate a throughput regression with a mutex high in a profile | base + `cpp-concurrency-performance`; add concurrency only if synchronization semantics will change | `cpp-lock-free` fires just because a mutex is hot | baseline/scaling/attribution before candidate change |
| Optimize false sharing with stable correctness semantics | base + performance | lock-free skill loads | same workload/environment A/B + layout/cache evidence |
| Implement or audit an MPMC lock-free queue | base + concurrency + lock-free; performance only if optimization is part of task | “no mutex” is treated as sufficient proof | abstract object + linearization + reclamation + memory-order + progress proof |
| TLA+ model has a dead branch but no C++ failure is known | no C++ defect claim from these skills; load specialists only if current C++ protocol is being audited | model bug is relabeled as implementation bug | current C++ facts are recovered before any implementation claim |
| Change only Markdown/docs with no C++ contract review | none of the C++ skills should be required | base C++ skill fires on unrelated prose | no unnecessary skill context |

## Completion-quality probes

When a skill is expected to fire, inspect whether the agent naturally produces or can answer the corresponding completion record without another prompt.

### Base C++

- governing authority from repository source of truth;
- change class;
- ownership/lifetime result;
- failure/interface result;
- complexity disposition with concrete call sites, protected obligations, and distinguishing evidence;
- mechanical style/lint authority applied;
- specialist skills applied;
- evidence actually run;
- residuals.

### Concurrency

- shared-state map;
- as-built state/protocol transitions;
- synchronization edges;
- lifetime/terminal authority;
- progress/fairness/external assumptions;
- closed-stuck-state analysis;
- deterministic evidence;
- remaining assumptions.

### Performance

- objective;
- reproducible baseline contract or explicit missing measurements;
- scaling evidence if collected;
- attribution classification;
- discriminating experiment;
- symmetric A/B result + noise when optimization is claimed;
- correctness equivalence and cost vector;
- keep/revise/revert/evidence-only decision.

### Advanced atomics

- entry class and authority;
- abstract operation / participant domain;
- linearization point;
- publication and memory-order edges;
- reclamation/ABA;
- progress property and assumptions;
- negative control / weak-memory evidence;
- cost if relevant.

## Reference-loading probes

When a detailed handbook is needed:

- the agent should search `REFERENCE.md` for a specific heading/topic first;
- it should read the smallest relevant section rather than loading the entire handbook;
- legacy frontmatter or historical “when to use” prose inside `REFERENCE.md` must not override current `SKILL.md` routing.

## Failure patterns to watch

A skill revision regresses if it causes any of these consistently:

- comprehensive reference loaded when the task only needs the procedure;
- specialist skills triggered by weak lexical matches (`mutex` => lock-free, `thread` => performance);
- generic guidance overriding Sluice `AGENTS.md` / ADR contracts;
- generic style guidance overriding `.clang-format` / `.clang-tidy`;
- external Python-specific rules applied mechanically to C++;
- removing a state, guard, helper, or test based only on line count, call-site count, or style taste;
- simplification that quietly changes OS errors, accepted-work obligations, or lifecycle semantics;
- a long checklist with no observable done/not-done criterion;
- repeated project facts copied across multiple skills;
- “be careful / be idiomatic / consider X” prose that does not change agent action;
- a liveness claim based only on the existence of one successful path;
- a performance implementation claim without a reproducible baseline contract;
- fixes justified by a green test/model that cannot distinguish the broken behavior.
