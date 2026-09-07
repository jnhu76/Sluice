# Sluice Agent Guide

This file contains only the rules an AI coding agent needs for the current repository.
It is not a history book, research log, roadmap, or architecture specification.

## Current authority

For current behavior, read the repository in this order:

1. `include/` — public headers
2. `src/` — production implementation
3. `apps/` — real consumers of the public API
4. `xmake.lua` and `xmake/` — current build structure

The current C++ implementation is the primary source of truth.

Do not use old Issues, PRs, deleted documents, Git history, old tests, old formal models, or pre-reset tags as design authority unless a task explicitly asks for historical investigation.

## Clean-room reset

The repository is intentionally being rebuilt from the retained C++ code.

Old tests, benchmarks, examples, scripts, CI workflows, documentation, research campaigns, and formal models have no inheritance right.
Their absence is intentional until a current need justifies rebuilding them.

`research/RESULTS.md` keeps conclusions only. Research process and chronology do not belong in the current tree.

## Engineering order

Unless a task explicitly says otherwise:

1. keep the retained implementation usable;
2. understand the current C++ as written;
3. remove unnecessary modules and abstractions;
4. freeze the smaller architecture;
5. rebuild tests from current behavior;
6. rebuild documentation from the frozen code;
7. rebuild formal verification and TLA+ from current C++ state machines;
8. optimize only measured local hotspots.

Correctness is an engineering requirement, not a separate research campaign.

## Architecture discipline

Prefer deletion and direct code over speculative abstraction.

- No abstraction for hypothetical future users.
- No subsystem survives merely because it existed before.
- `apps/` matter because they are real users of the library.
- Avoid unrelated redesign while performing cleanup work.

## C++ comments

Comments must describe the current implementation only.

Historical rationale, Issue/PR references, phase names, research terminology, and old formal-model references do not belong in production C++ comments.

When comments are reintroduced, keep only concise comments that explain a non-obvious **Why**, necessary **What**, or non-obvious **How**.
Do not comment code that already explains itself.

## Tests, docs, and formal verification

Rebuild all three from the final C++ implementation rather than from deleted artifacts.

Tests should follow current public behavior, lifecycle, state transitions, failure behavior, and real applications.
Documentation should explain only the current architecture and API.
Formal models should map explicit C++ transitions to TLA+ actions and be validated against the implementation.

## Performance

Performance work starts from measurement:

`profile -> hotspot -> local hypothesis -> isolated change -> benchmark -> keep or revert`

No measured hotspot means no optimization task.

## Repository hygiene

Keep `xmake.lua` and `xmake/` aligned with files and targets that actually exist.
Do not recreate CI, scripts, tests, docs, or formal tooling merely because the old repository had them.
Keep commits narrow and reviewable. Do not merge unless explicitly instructed.
