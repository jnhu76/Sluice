# Sluice Context

Sluice is a C++20 I/O library/runtime being reduced to a small engineering baseline.

## Current repository

```text
include/              public C++ headers
src/                  production implementation
apps/                 real applications using the public API
research/RESULTS.md    retained research conclusions only
xmake.lua, xmake/      build configuration
```

The implementation under `include/` and `src/` is the primary source of truth.
`apps/` are retained because they are real users of the library.

## Deliberately absent

The old tests, benchmarks, examples, scripts, CI workflows, documentation, formal models, TLA+ specifications, and research campaign scaffolding were intentionally removed.

Git history and the pre-reset snapshot remain the archive when historical recovery is explicitly needed.

## Current direction

```text
usable C++
    -> smaller implementation
    -> frozen architecture
    -> new tests from current behavior
    -> new docs from current code
    -> new formal models from current C++
    -> explicit C++ <-> TLA+ correspondence
    -> fine-grained measured optimization
```

Correctness remains mandatory. Tests, documentation, and formal verification will be rebuilt from the reduced implementation rather than inherited from historical structure.

Use `AGENTS.md` for AI-agent working rules and `README.md` for the human-facing entry point.
