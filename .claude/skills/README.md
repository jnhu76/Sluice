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
        +--> cpp-concurrency-performance  measured performance work
                    |
                    +--> cpp-lock-free     only if the chosen candidate needs it
```

`SKILL.md` contains the procedure, branching, and completion criteria. `REFERENCE.md` contains detailed guidance and examples and should be read only for the branch currently under investigation.

Repository authority (`AGENTS.md`, accepted ADRs, explicit task/issue scope) remains above these skills. The skills standardize **how an agent reasons about and reviews C++ changes**, not what architecture Sluice is allowed to have.

## Maintenance rules

- Keep trigger descriptions narrow enough that unrelated specialist skills do not fire.
- Keep the base C++ skill broad: any C++ production edit/review should receive it.
- Keep one meaning in one place. Do not copy project facts or long Core Guidelines material into multiple `SKILL.md` files.
- Put executable steps and checkable completion criteria in `SKILL.md`; put detailed reference material in `REFERENCE.md`.
- Prefer positive target behavior over long prohibition lists.
- When a new rule is easy to discover from code/config/`AGENTS.md`, point to that authority rather than caching it here.
- Add a specialist skill only when it has a distinct trigger and workflow; do not split merely to reduce line count.
- Update `EVALS.md` when routing or completion behavior changes.

## What success means

The skill bank is useful only if it changes agent behavior. A successful run should make a C++ change arrive with:

- explicit authority and scope;
- ownership/lifetime/failure reasoning;
- specialist concurrency/performance/atomic reasoning only when applicable;
- a narrow implementation;
- checkable completion evidence;
- no duplicated or stale project contract invented by the skill itself.
