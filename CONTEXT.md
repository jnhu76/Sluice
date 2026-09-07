# Sluice Context

This file is a short snapshot of the **current** repository. It intentionally avoids historical phases, campaign names, old Issues, and retired research theses.

## Current state

Sluice is a C++20 I/O library/runtime being reduced to a smaller, stable engineering baseline.

The retained implementation lives in:

```text
include/   public C++ headers
src/       production implementation
apps/      real applications using the library
```

The repository currently keeps only two documentation families:

```text
docs/adr/           retained architectural decisions
docs/architecture/  current architecture descriptions
```

Past research is reduced to:

```text
research/RESULTS.md
```

That file records conclusions, not research process.

## Deliberately absent

The previous tests, benchmarks, examples, scripts, CI workflows, formal models, TLA+ specifications, and research campaign scaffolding were intentionally removed from the current tree.

Their absence does not mean correctness or performance no longer matter. It means both will be rebuilt from the retained implementation instead of inherited from historical project structure.

Git history and the pre-reset repository snapshot remain the archive when historical recovery is explicitly needed.

## Current direction

The project is no longer organized around broad research questions.

The working sequence is:

```text
usable C++
    -> smaller implementation
    -> fixed architecture
    -> new tests from current behavior
    -> new formal models from current C++
    -> explicit C++ <-> TLA+ correspondence
    -> fine-grained measured optimization
```

Correctness remains mandatory. Formal verification will be rebuilt after the implementation has been reduced and its important state machines are clear.

Performance work will be local and evidence-driven rather than architecture-wide.

## Authority

For current behavior, read the code first:

1. `include/`
2. `src/`
3. `apps/`

Use `docs/adr/` and `docs/architecture/` to explain the retained design, not to resurrect deleted historical structure.

Use `AGENTS.md` for AI-agent working rules.
Use `README.md` for the human-facing project entry point.
