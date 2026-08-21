# Sluice C++ skill bank

These project skills are a small procedural stack for C++ work. They are intentionally **not** four independent handbooks loaded on every task.

## Composition

```text
cpp-coding-standards            base workflow for any C++ edit/review
        |
        +--> cpp-concurrency-guidelines   shared-state correctness
                    |
                    +--> cpp-lock-free     advanced atomics / lock-free proof
        |
        +--> cpp-concurrency-performance  concurrent performance work
                    |
                    +--> cpp-lock-free     only if the chosen candidate needs it
```

All four are model-invoked because the base workflow must be able to route to specialists and users may also ask for a specialist task directly.

## Sources of truth

Keep these roles separate:

| Concern | Source of truth |
|---|---|
| repository/architecture semantics | explicit task/issue, `AGENTS.md`, accepted ADRs/docs |
| C++ formatting | `.clang-format` |
| configured static-analysis policy | `.clang-tidy` and repository tooling |
| procedural agent workflow | the relevant `SKILL.md` |
| long-form examples/background | the relevant `REFERENCE.md` |

A skill must not cache repository facts or mechanical style that the agent can read directly from those sources.

## Progressive disclosure

`SKILL.md` contains routing, ordered procedure, and completion criteria. `REFERENCE.md` preserves detailed guidance and examples.

The reference files are intentionally long-form. When one is needed, search for the exact heading/topic and read only the smallest relevant section. Do not load an entire handbook merely because the skill fired. Routing and termination always come from `SKILL.md`, not legacy invocation prose inside a reference.

## Maintenance rules

- Keep the base C++ skill broad: any C++ production edit/review should receive it.
- Keep specialist descriptions narrow enough that lexical matches alone do not fire unrelated workflows.
- Keep model-invoked frontmatter portable: use the standard `name` and `description` discovery fields unless a documented platform feature genuinely requires more.
- Keep one meaning in one place. Point to `AGENTS.md`, configs, code, or canonical references instead of copying discoverable facts.
- Put ordered actions and checkable completion criteria in `SKILL.md`; put detailed examples/background in `REFERENCE.md`.
- Prefer positive target behavior over long prohibition lists.
- Add a specialist skill only when it has a distinct trigger and workflow; do not split merely to reduce line count.
- Update `EVALS.md` whenever routing, completion behavior, or a source-of-truth boundary changes.

## What success means

The skill bank is useful only if it changes agent behavior. A successful C++ run should arrive with:

- explicit authority and scope;
- ownership/lifetime/failure reasoning;
- specialist concurrency/performance/atomic reasoning only when applicable;
- repository-native formatting/static-analysis policy rather than invented style;
- a narrow implementation;
- checkable completion evidence;
- no duplicated or stale project contract invented by the skill itself.
