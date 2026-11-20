---
name: cpp-coding-standards
description: C++ engineering workflow for Sluice. Use for any C++ source/header/API edit, simplification, refactor, or review. Drives ownership, lifetime, type, failure, evidence-gated simplicity, and narrow-patch review; routes shared-state concurrency, performance investigation, and advanced atomic protocols to specialist skills.
---

# C++ engineering baseline

Use this skill to make a C++ change predictable and reviewable. The target is a narrow patch whose ownership, lifetime, state, failure behavior, and evidence are explicit. Simplicity means fewer unjustified mechanisms, not fewer lines at the expense of contract or proof.

## Authority and mechanical sources of truth

Use the authority chain in `AGENTS.md §2`. Do not maintain a second precedence list here. Project contracts and accepted ADRs decide allowed semantics; generic C++ guidance does not override them.

For mechanical C++ style and static-analysis policy, use the repository itself:

- `.clang-format` is the formatting source of truth;
- `.clang-tidy` is the configured static-analysis source of truth;
- `xmake.lua` / `AGENTS.md` define the applicable repository commands and gates.

Do not invent a second style guide inside this skill. If generic guidance disagrees with project authority or tool configuration, follow the project and report the conflict.

## Step 1 — classify the change

Before editing, identify the branches that apply:

- **resource/lifetime** — ownership, cleanup, borrow duration, object validity;
- **interface/type** — public/private API shape, result types, ranges, enums, conversions;
- **failure** — `Result<T>` / `IoError`, fail-fast, exception boundary, OS error preservation;
- **low-level boundary** — syscall, C ABI, wire/layout, allocator, SIMD, platform code;
- **concurrency** — shared mutable state, threads/fibers, queues, locks, atomics, wake/park, cancellation, shutdown;
- **performance** — the task asks to measure, diagnose, review, or optimize concurrent performance;
- **advanced atomics** — correctness depends on CAS, cross-operation memory ordering, linearization, ABA, reclamation, or a non-blocking progress property;
- **simplification** — a proposed removal or consolidation of helpers, wrappers, state, defensive paths, comments, or tests. This class still routes to concurrency/performance/atomic specialists when its effects require them.

Route specialist branches:

- concurrency → load `cpp-concurrency-guidelines`;
- concurrent performance → load `cpp-concurrency-performance`;
- advanced atomics / lock-free proof → load `cpp-lock-free`.

If `AGENTS.md` classifies the change as architecture-sensitive, complete its architecture gate before production implementation.

**Completion criterion:** you can name the governing repository authority, change class, and required specialist skills before editing production C++.

## Step 2 — model ownership and valid state

For each changed abstraction, answer only the questions that apply:

- What resource or state does it own?
- What is borrowed, and for exactly how long?
- What states are valid?
- What invariant must hold after construction and after every public operation?
- Who performs cleanup?
- Where does ownership transfer?
- What failure states are observable?
- Which representation is deliberately low-level, and where does that boundary stop?

Prefer the type system and object lifetime to caller memory:

- RAII for owned resources;
- value semantics when identity/shared mutation are unnecessary;
- references/spans/observers for non-owning access with a clear lifetime;
- scoped enums and named result/state types when they prevent invalid combinations;
- complete valid construction where practical.

Keep intentionally C-like representation narrow at real low-level boundaries. Do not rewrite a sound syscall/ABI/layout boundary merely to make it look object-oriented.

**Completion criterion:** no ownership, cleanup, borrow, or valid-state assumption needed by the patch is implicit.

## Step 3 — shape the interface

Prefer interfaces that expose intent directly:

- return values instead of routine output parameters;
- `std::span` or another appropriate range abstraction instead of `(ptr, size)` when the interface is a range rather than a raw platform boundary;
- `const` / `constexpr` where mutation is not part of the contract;
- explicit/narrow conversions at type boundaries;
- names that reveal the domain quantity and its physical unit when relevant (for example, `bytes`, `nanoseconds`, `request_count`), while preserving established public/ABI names and avoiding unrelated renames;
- `enum class`, variants, optionals, or distinct types when they materially eliminate magic values or invalid states;
- unique ownership by default; shared ownership only when the ownership model genuinely requires it.

For Sluice I/O paths, preserve the repository failure model. Do not introduce exception-based public I/O control flow merely because a generic C++ example uses exceptions.

**Completion criterion:** callers can determine ownership, mutability, failure, and state meaning from the interface without reconstructing an undocumented protocol.

## Step 4 — justify simplicity, then implement the smallest coherent patch

Before adding, extracting, merging, or deleting code, inspect actual call sites and apply this gate:

- **Abstractions:** name the current users and the concrete duplication, invariant, or misuse they eliminate. Prefer existing standard-library facilities when they fit. Do not create speculative interfaces, registries, wrappers, or helpers for hypothetical users. Multiple call sites are useful evidence, not a mandatory threshold; a single-use helper may still isolate a real error, lifetime, or protocol boundary.
- **State and guards:** identify the authority for each state, flag, counter, fallback, or defensive branch. Ask what real failure, legal interleaving, resource obligation, or contract diagnostic breaks if it is removed. Apply the Mechanism Justification Gate in `AGENTS.md`; derived state is not automatically removable, and physical/lifetime obligations must survive.
- **Control and failure flow:** remove unreachable or duplicated branches only with a stated invariant and a distinguishing check. Preserve syscall retry rules, `Result<T>` / `IoError`, accepted-work settlement, cancellation, and shutdown behavior. Never turn an unexpected failure into a fabricated success.
- **Dead code and comments:** demonstrate the absence of a current consumer or required obligation before deleting code. Follow `AGENTS.md` for exceptional local C++ comments; do not move contract authority or issue history into source commentary.
- **Tests:** name the independent regression, invariant, or legal interleaving each test distinguishes. Similar-looking tests are not duplicates if they catch different failures. Before deleting one, state which regression would become undetected and how remaining evidence covers it.

For every simplification candidate, record the before/after behavior, protected obligation, validation oracle, and smallest coherent edit. If necessity cannot be disproved, retain the mechanism or mark the proposal as an audit finding instead of deleting it.

During implementation:

- preserve existing architecture and authority unless the task explicitly changes them;
- keep cleanup automatic;
- keep raw ownership and manual `new`/`delete` out of ordinary code;
- use scoped locking rather than manual lock/unlock;
- keep casts narrow and justified;
- avoid opportunistic class hierarchies, framework layers, manager objects, or generic abstractions unrelated to the defect/feature;
- avoid unrelated formatting and cleanup;
- add a short comment only when a non-obvious local hazard or invariant cannot be made clear through naming or structure; keep contracts, authority, and review history in their designated documentation.

Use `.clang-format` for touched C++ when formatting is needed; do not turn a local repair into a repository-wide style migration.

**Completion criterion:** every changed line is causally necessary for the requested behavior, evidence, contract update, or local mechanical conformance.

## Step 5 — review the changed C++

Before declaring implementation complete, inspect the final diff and answer:

1. **Lifetime:** can any pointer/reference/callback/task outlive what it observes?
2. **Ownership:** is any owner represented as a raw observer or external cleanup protocol?
3. **State:** can the patch create a meaningless or partially initialized state?
4. **Failure:** is any correctness-relevant failure ignored, collapsed, or changed from the project contract?
5. **Types:** did a boolean/integer/sentinel/output parameter hide a domain state that should be explicit?
6. **Boundary:** did syscall/C/ABI representation leak farther into ordinary C++ than necessary?
7. **Complexity:** for each added or removed abstraction, guard, or state field, what current use or proven invariant justifies the decision?
8. **Names:** do touched identifiers distinguish physical units and domain quantities without breaking a public or ABI contract?
9. **Tests:** which plausible future change makes each added/retained test fail, and which regression would a removed test stop catching?
10. **Specialists:** did concurrency/performance/advanced-atomic code receive the corresponding specialist review?
11. **Mechanical policy:** does changed code follow `.clang-format` and the repository's configured `.clang-tidy` policy rather than a generic taste preference?

**Completion criterion:** every applicable question has a concrete answer grounded in the final diff or in an explicit repository configuration.

## Step 6 — produce evidence

Use repository-defined gates from `AGENTS.md`; do not duplicate or weaken them here.

For a bug/correctness repair, prefer this evidence order:

1. pre-fix reproducer or precise invariant violation;
2. focused post-fix regression exercising the same cause;
3. applicable project build/test/sanitizer/formal/performance gates;
4. final diff review and residual-risk statement.

Report only commands actually executed. A passing test that could not fail for the old behavior is not repair evidence. A simplification must show behavioral equivalence with the relevant oracle; fewer lines alone are not a performance claim. Use a measured baseline and the performance skill before claiming runtime improvement.

## Completion record

A finished C++ task should be able to state, concisely:

```text
authority:
change class:
ownership/lifetime result:
failure/interface result:
complexity disposition (kept/removed mechanism and evidence):
mechanical style/lint authority applied:
specialist skills applied:
evidence actually run:
residuals / follow-ups:
```

## Detailed reference

`REFERENCE.md` preserves the previous comprehensive handbook and examples. It is long-form reference, not routing or project authority. Search for the exact heading/topic first and read only the smallest relevant section; do not load the whole handbook by default. Current invocation, workflow, and completion rules live in this `SKILL.md`.
