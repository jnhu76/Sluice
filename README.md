# Sluice

Sluice is a C++20 I/O library and runtime currently being reduced to a smaller, stable engineering baseline.

[中文说明](README.zh-CN.md)

## Repository shape

The current tree is intentionally small:

```text
include/              public C++ headers
src/                  production implementation
apps/                 real applications using the public API
docs/adr/             retained architectural decisions
docs/architecture/    current architecture
research/RESULTS.md    conclusions from the previous research phase
xmake.lua, xmake/      build configuration
```

The implementation under `include/` and `src/` is the primary source of truth for current behavior. `apps/` matters because it provides real consumers of that API.

Historical tests, benchmarks, examples, scripts, CI workflows, formal models, and research campaign scaffolding were deliberately removed from the current tree. Git history remains the archive.

## What Sluice contains

The retained codebase includes a synchronous I/O core and an opt-in asynchronous runtime.

The synchronous side provides the `Result<T>` / `IoError` error model, Reader/Writer-style I/O, file and positional I/O, copy helpers, and durability operations.

The asynchronous side contains explicit operations, caller-owned completions, bounded request state, scheduling/runtime machinery, synchronization primitives, cancellation, and backend execution. Linux io_uring support remains an optional implementation path where enabled by the build.

The exact retained surface is defined by the current headers and build files, not by historical roadmaps.

## Applications

The repository keeps real applications built on the public API:

- [`sluice-copy`](apps/sluice-copy/README.md)
- [`sluice-hash`](apps/sluice-hash/README.md)
- [`sluice-grep`](apps/sluice-grep/README.md)
- [`sluice-tail`](apps/sluice-tail/README.md)

These applications are part of the current product surface and help determine which library capabilities still have real users.

## Build

Sluice uses [Xmake](https://xmake.io) and requires a C++20 compiler.

```bash
git clone https://github.com/jnhu76/Sluice.git
cd Sluice
xmake f -m release -y
xmake
```

`xmake.lua` and `xmake/` are the authority for the targets that currently exist. The build configuration is being simplified together with the repository.

## Documentation

Current documentation is intentionally narrow:

- [Architecture](docs/architecture/README.md)
- [Architecture Decision Records](docs/adr/README.md)
- [Research results](research/RESULTS.md)
- [Current project context](CONTEXT.md)
- [AI-agent guidance](AGENTS.md)

Documentation should describe the current implementation. It should not preserve old project chronology as a second architecture.

## Current direction

Sluice is no longer driven by broad research campaigns.

The current sequence is:

```text
keep the retained C++ usable
        -> remove unnecessary modules and abstractions
        -> freeze the smaller architecture
        -> rebuild tests from current behavior
        -> rebuild formal verification and TLA+ from current C++
        -> establish C++ <-> formal-model correspondence
        -> optimize measured local hotspots
```

Correctness remains mandatory, but correctness work is treated as engineering rather than as a separate research program.

Performance work comes later and should be fine-grained: profile first, isolate one cost, change one local mechanism, measure, and keep or revert.

## License

Sluice is licensed under the [MIT License](LICENSE).
