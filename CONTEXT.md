# Sluice Context

> **Orientation only — not authority.**
>
> This file is a short stable orientation/vocabulary map for humans and coding
> agents. It does **not** define architecture, public API, execution order, or
> any normative rule, and it is not part of the authority chain. If this file
> conflicts with any authority, the authority wins. Changing execution status
> belongs to GitHub Issues, never to this file.
>
> Keep this file short: short statement + pointer, no copied prose.

---

## 1. What Sluice is

Sluice is an experimental C++20 explicit-I/O and control-flow library: a
bounded, explicit, inspectable I/O execution layer that makes I/O capability,
request identity, resource ownership, execution policy, and backend mechanism
visible, controllable, and verifiable.

## 2. North star (frozen)

> **Minimal semantics. Explicit authority. Named bounds. Replaceable execution.**

The retired thesis `Explicit control, implicit correctness.` (falsified and
retired by #283) is **not** the project thesis. There is no generic
Control lane and no project-level obligation that explicit I/O produce generic
control. SAFETY, PERFORMANCE, and SEMANTIC AUTHORITY are independent proof
lines.

## 3. Responsibility vocabulary

These six classes are distinct architecture responsibilities. Do not collapse
them and do not infer one from another:

```text
SEMANTIC SURFACE      caller-visible observable contract
CORRECTNESS KERNEL    internal authority enforcing those semantics
RESOURCE BOUNDS       real, named finite resources
BACKEND CAPABILITY    mechanism availability
EXECUTION POLICY      choice among already-legal mechanisms
OBSERVATION / HINT    information that grants no authority by itself
```

Related discipline: information ≠ semantic authority ≠ backend mechanism ≠
unique incremental value ≠ material performance.

## 4. Authority pointers

| Question | Entry point |
| --- | --- |
| Durable agent governance + routing | `AGENTS.md` |
| Developer documentation router | `docs/README.md` |
| Architecture constitution (AC-N) | `docs/architecture/architecture-constitution.md` |
| Public contract | `include/sluice/` + `docs/reference/api.md` |
| Decisions | `docs/adr/README.md` |
| Verification methods | `docs/verification/README.md` |
| Failure / assert authority | `docs/architecture/failure-model.md` |
| Historical evidence | `docs/history/` |

## 5. Where changing status lives

What we are doing now, in what order, and where work stops:

- `#227` — sole project execution-order roadmap;
- `#289` — Boundary / Safety research roadmap;
- `#259` — Performance / data-movement research roadmap;
- `#225` — architecture constitution (responsibilities and invariants, not
  execution order).

Do not copy moving status, phase state, or campaign results into this file.
Stale statements here are deleted or fixed, not preserved as sediment.
