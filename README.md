# Sluice

Sluice is a C++20 explicit-I/O library and runtime.

Its purpose is to expose only the I/O semantics and resource boundaries callers truly need, enforce those contracts with the smallest practical correctness machinery, and keep execution mechanisms replaceable without letting backend details redefine the public semantics.

[中文说明](README.zh-CN.md)

## Mission

Sluice is governed by six long-term principles:

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
- [`docs/adr/0001-explicit-io-design-doctrine.md`](docs/adr/0001-explicit-io-design-doctrine.md) — rationale, tradeoffs, consequences, and rejected alternatives.

The mission defines what Sluice is allowed to become. Current C++ and build files define what the repository does today. Descriptive architecture documentation is derived from the code; it does not override the code or expand the mission.

## Repository shape

```text
include/              C++ headers
src/                  production implementation
apps/                 real applications
xmake.lua, xmake/      build configuration
docs/                 frozen mission, ADRs, and code-derived architecture docs
research/RESULTS.md    retained research conclusions
```

Historical campaign scaffolding, obsolete tests, benchmarks, examples, CI workflows, old documentation, and old formal models are not design authority. Git history remains the archive.

## Current implementation

The retained codebase contains a synchronous I/O core and an opt-in asynchronous runtime.

The synchronous side provides `Result<T>` / `IoError`, Reader/Writer-style I/O, file and positional I/O, copy helpers, durability operations, and related utilities.

The asynchronous side contains explicit operations, caller-owned completions, bounded request state, scheduling/runtime machinery, cancellation, synchronization facilities, and backend execution.

These implementation details remain subject to subtraction. An abstraction, backend, helper, state, or public type survives only when it earns its cost through necessary semantics, correctness, boundedness, real execution needs, real callers, or indispensable verification value.

## Boundary rule

Applications should depend on the public semantic boundary, not implementation internals. Backend capability, observation, hints, or execution policy do not automatically become public semantic authority.

Explicit semantics also do not automatically imply generic optimization power or better performance. Correctness, performance, and semantic-authority claims are separate evidence lines.

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

## Correctness and verification

Correctness is mandatory, but verification machinery must remain proportional to the boundary it protects. Deterministic tests, property tests, fuzzing, sanitizers, and formal models should target real invariants rather than grow as parallel frameworks.

A formal model proves the model, not the C++ implementation by itself; important models require explicit implementation correspondence.

## Research record

Past research is reduced to [research/RESULTS.md](research/RESULTS.md). It keeps durable conclusions, not campaign process.

## License

Sluice is licensed under the [MIT License](LICENSE).
