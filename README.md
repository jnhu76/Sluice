# Sluice

Sluice is a C++20 explicit-I/O library and runtime.

Its purpose is to **expose only the information required to preserve observable I/O semantics, correctness, and real resource boundaries; make semantic authority explicit; keep execution mechanisms and policy local and replaceable; and retain only mechanisms that have earned their cost through evidence.**

[中文说明](README.zh-CN.md)

## Mission

Sluice follows six long-term principles distilled from the retained research results:

```text
Minimal semantics.
Clear boundaries.
Explicit authority.
Named bounds.
Replaceable execution.
Minimum mechanism.
```

> **语义最少，边界清晰，权威显式，资源有界，执行可换，机制最小。**

- [`docs/mission.md`](docs/mission.md) — frozen normative mission.
- [`docs/adr/0001-explicit-io-design-doctrine.md`](docs/adr/0001-explicit-io-design-doctrine.md) — how the research results support these design decisions.
- [`research/RESULTS.md`](research/RESULTS.md) — retained research evidence and conclusions.

## Architecture at a glance

<p align="center">
  <img src="docs/assets/sluice-architecture.svg" alt="Sluice architecture overview" width="100%">
</p>

The diagram summarizes the current implementation shape. Current code and build definitions describe what exists today; the mission and ADR define the research-backed design boundary.

## Research-backed guardrails

The retained research does not support a project-level assumption that more explicit information naturally produces more generic control, specialization, or performance.

The following distinctions remain fixed:

```text
resource identity != fixed-resource optimization authority
operation grouping != fused / atomic admission authority
backend capability != semantic authority
hint / information != authority
```

The Copy experiments showed that an explicit composed operation can form a legal transformation boundary, while a thin local branch was sufficient for the capability that was actually earned; a generic capability framework was not justified.

The Batch experiments showed that knowing operations belong to one Batch does not grant group-admission authority.

Performance research likewise keeps semantic contracts separate from execution policy: alignment, chunk size, queue depth, worker count, and backend mechanisms can matter materially, but benchmark results do not automatically promote those controls into public semantics.

## Current implementation

The current codebase contains a synchronous I/O core and an opt-in asynchronous runtime.

The synchronous side provides `Result<T>` / `IoError`, Reader/Writer-style I/O, file and positional I/O, copy helpers, durability operations, and related utilities.

The asynchronous side contains explicit operations, caller-owned completions, bounded request state, scheduler/runtime machinery, cancellation, synchronization facilities, and backend execution.

Future architecture audits classify concepts by their actual role:

```text
SEMANTIC_CONTRACT
CORRECTNESS_AUTHORITY
RESOURCE_BOUND
BACKEND_CAPABILITY
EXECUTION_POLICY
HINT / OBSERVATION
LEGACY / UNJUSTIFIED
```

An abstraction survives because it defines real semantics, correctness, resource bounds, or execution value—not because a larger framework would look more complete.

## Applications

- [`sluice-copy`](apps/sluice-copy/README.md)
- [`sluice-hash`](apps/sluice-hash/README.md)
- [`sluice-grep`](apps/sluice-grep/README.md)
- [`sluice-tail`](apps/sluice-tail/README.md)

## Build

Sluice uses [Xmake](https://xmake.io) and requires a C++20 compiler.

```bash
git clone https://github.com/jnhu76/Sluice.git
cd Sluice
xmake f -m release -y
xmake
```

`xmake.lua` and `xmake/` define the targets that currently exist.

## Documentation

- [`docs/mission.md`](docs/mission.md) — project mission.
- [`docs/adr/0001-explicit-io-design-doctrine.md`](docs/adr/0001-explicit-io-design-doctrine.md) — research-backed design decision.
- `docs/architecture.md` — architecture snapshot derived from current code.
- [`research/RESULTS.md`](research/RESULTS.md) — retained research conclusions.

## License

Sluice is licensed under the [MIT License](LICENSE).
