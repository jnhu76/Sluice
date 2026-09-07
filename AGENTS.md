# Sluice Agent Guide

This file contains only the rules an AI coding agent needs for the **current** repository.
It is not a history book, research log, roadmap, or architecture specification.

## 1. Current authority

The current C++ implementation is the primary source of truth:

- `include/` — public headers
- `src/` — production implementation
- `apps/` — real consumers of the public API

`docs/adr/` and `docs/architecture/` describe retained design intent, but they must follow the current implementation. If prose and code disagree, inspect the code first and update the prose rather than forcing the implementation to preserve historical structure.

Do not use old Issues, PRs, deleted documents, Git history, old formal models, or pre-reset tags as design authority unless a task explicitly asks for historical investigation.

## 2. Repository reset rules

The repository is undergoing a clean-room reduction.

Do not resurrect deleted infrastructure merely because it existed before. In particular, old tests, benchmarks, examples, scripts, CI workflows, research campaigns, and formal models have no inheritance right.

The absence of one of these components is intentional until it is rebuilt from the retained C++ code.

`research/RESULTS.md` keeps conclusions only. Research process, candidate ladders, campaign chronology, corrective history, and experimental scaffolding do not belong in the current tree.

## 3. Engineering priorities

Work in this order unless a task explicitly says otherwise:

1. keep the retained implementation usable;
2. understand the current C++ as written;
3. remove unnecessary modules and abstractions;
4. freeze a smaller architecture;
5. rebuild tests from the frozen implementation;
6. rebuild formal verification and TLA+ from the current C++ state machines;
7. optimize only measured, local hotspots.

Correctness is an engineering requirement, not a separate research campaign.

Do not start broad Safety, Control, Tax, or generic-performance research programs. Do not add architecture-wide optimization machinery without a measured local cause.

## 4. Architecture discipline

Prefer deletion and direct code over speculative abstraction.

- No abstraction for hypothetical future users.
- No fallback, adapter, backend, scheduler, policy, or framework merely for generality.
- Existing modules are not automatically permanent.
- If a module no longer has a real product role, removal is preferred to preservation for history.
- `apps/` are important because they are real users of the library and may justify retained functionality.

Avoid unrelated redesign while performing cleanup work.

## 5. C++ comments

Comments must describe the **current** implementation only.

Historical rationale, Issue/PR references, phase names, research terminology, and old formal-model references do not belong in production C++ comments.

When comments are reintroduced, prefer a small number of comments that explain:

- **Why** a non-obvious invariant or ordering exists;
- **What** a state or responsibility means when names are insufficient;
- **How** a non-obvious mechanism preserves that invariant.

Do not comment code that already explains itself.

## 6. Tests and formal verification

Old tests and old formal models were deliberately removed.

When tests are rebuilt, derive them from the current public behavior, lifecycle, state transitions, failure behavior, and real applications. Do not recreate old test taxonomy merely because names can be recovered from Git.

When formal verification is rebuilt, derive the model from the frozen C++ implementation. Establish explicit correspondence between important C++ transitions and TLA+ actions. The model validates the implementation; it does not dictate a historical architecture.

## 7. Performance work

Performance work starts from measurement:

`profile -> hotspot -> local hypothesis -> isolated change -> benchmark -> keep or revert`

No measured hotspot means no optimization task.

Prefer fine-grained changes with a clear rollback boundary. Avoid coarse architecture-wide optimization campaigns.

## 8. Build and repository hygiene

`xmake.lua` and `xmake/` define the current build. Keep them aligned with the files and targets that actually remain in the repository.

Do not add CI, scripts, test harnesses, formal tooling, or build targets unless the current task needs them.

Keep commits narrow and reviewable. Do not merge a pull request unless explicitly instructed.
