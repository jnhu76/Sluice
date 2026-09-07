# Sluice

Sluice is a C++20 I/O library and runtime currently being reduced to a smaller, stable engineering baseline.

[中文说明](README.zh-CN.md)

## Repository shape

```text
include/              public C++ headers
src/                  production implementation
apps/                 real applications using the public API
research/RESULTS.md    retained research conclusions
xmake.lua, xmake/      build configuration
```

The current C++ implementation is the primary source of truth. `apps/` matter because they are real consumers of the public API.

Historical tests, benchmarks, examples, scripts, CI workflows, documentation, formal models, and research campaign scaffolding were deliberately removed from the current tree. Git history remains the archive.

## What Sluice contains

The retained codebase includes a synchronous I/O core and an opt-in asynchronous runtime.

The synchronous side provides the `Result<T>` / `IoError` error model, Reader/Writer-style I/O, file and positional I/O, copy helpers, and durability operations.

The asynchronous side contains explicit operations, caller-owned completions, bounded request state, scheduling/runtime machinery, synchronization primitives, cancellation, and backend execution. Linux io_uring support remains optional where enabled by the build.

The exact retained surface is defined by the current headers and build files, not by historical documentation.

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

## Research record

Past research is reduced to [research/RESULTS.md](research/RESULTS.md). It keeps durable conclusions, not campaign process.

## Current direction

```text
keep the retained C++ usable
        -> remove unnecessary modules and abstractions
        -> freeze the smaller architecture
        -> rebuild tests from current behavior
        -> rewrite documentation from current code
        -> rebuild formal verification and TLA+ from current C++
        -> establish C++ <-> formal-model correspondence
        -> optimize measured local hotspots
```

Correctness remains mandatory. Performance work comes later and should be fine-grained and measurement-driven.

## License

Sluice is licensed under the [MIT License](LICENSE).
