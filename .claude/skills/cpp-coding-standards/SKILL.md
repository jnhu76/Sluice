---
name: cpp-coding-standards
description: C++ engineering workflow for Sluice. Use for any C++ source/header/API edit or review. Drives ownership, lifetime, type, failure, and narrow-patch review; routes shared-state concurrency, performance investigation, and advanced atomic protocols to specialist skills.
---

# C++ engineering baseline

Use this skill to make a C++ change predictable and reviewable. The target is a narrow patch whose ownership, lifetime, state, failure behavior, and evidence are explicit.

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
- **advanced atomics** — correctness depends on CAS, cross-operation memory ordering, linearization, ABA, reclamation, or a non-blocking progress property.

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
- `enum class`, variants, optionals, or distinct types when they materially eliminate magic values or invalid states;
- unique ownership by default; shared ownership only when the ownership model genuinely requires it.

For Sluice I/O paths, preserve the repository failure model. Do not introduce exception-based public I/O control flow merely because a generic C++ example uses exceptions.

**Completion criterion:** callers can determine ownership, mutability, failure, and state meaning from the interface without reconstructing an undocumented protocol.

## Step 4 — implement the smallest coherent patch

During implementation:

- preserve existing architecture and authority unless the task explicitly changes them;
- keep cleanup automatic;
- keep raw ownership and manual `new`/`delete` out of ordinary code;
- use scoped locking rather than manual lock/unlock;
- keep casts narrow and justified;
- avoid opportunistic class hierarchies, framework layers, manager objects, or generic abstractions unrelated to the defect/feature;
- avoid unrelated formatting and cleanup;
- write comments for invariants, authority, non-obvious lifetime, memory ordering, or "why" — not narration.

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
7. **Complexity:** did the patch add an abstraction that does not remove a real invariant, duplication, or misuse class?
8. **Specialists:** did concurrency/performance/advanced-atomic code receive the corresponding specialist review?
9. **Mechanical policy:** does changed code follow `.clang-format` and the repository's configured `.clang-tidy` policy rather than a generic taste preference?

**Completion criterion:** every applicable question has a concrete answer grounded in the final diff or in an explicit repository configuration.

## Step 6 — produce evidence

Use repository-defined gates from `AGENTS.md`; do not duplicate or weaken them here.

For a bug/correctness repair, prefer this evidence order:

1. pre-fix reproducer or precise invariant violation;
2. focused post-fix regression exercising the same cause;
3. applicable project build/test/sanitizer/formal/performance gates;
4. final diff review and residual-risk statement.

Report only commands actually executed. A passing test that could not fail for the old behavior is not repair evidence.

## Completion record

A finished C++ task should be able to state, concisely:

```text
authority:
change class:
ownership/lifetime result:
failure/interface result:
mechanical style/lint authority applied:
specialist skills applied:
evidence actually run:
residuals / follow-ups:
```

## Detailed reference

`REFERENCE.md` preserves the previous comprehensive handbook and examples. It is long-form reference, not routing or project authority. Search for the exact heading/topic first and read only the smallest relevant section; do not load the whole handbook by default. Current invocation, workflow, and completion rules live in this `SKILL.md`.
