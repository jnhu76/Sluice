# Sluice

Sluice is a C++20 explicit-I/O runtime and library. The current repository is intentionally small: production code, real applications, current architecture/ADR documents, and a compact record of past research results.

[中文说明](README.zh-CN.md)

## What is here

- `include/` — public C++ headers
- `src/` — production implementation
- `apps/` — real programs built on the public API
- `docs/architecture/` — current architecture
- `docs/adr/` — retained architectural decisions
- `research/RESULTS.md` — durable conclusions from the previous research phase
- `xmake.lua` and `xmake/` — build configuration

Historical tests, benchmarks, examples, formal models, scripts, research campaigns, and generated documentation were deliberately removed from the current tree. Git history remains the archive.

## Core shape

Sluice contains a synchronous I/O core and an opt-in asynchronous runtime.

The synchronous side provides `Result<T>` / `IoError`, Reader/Writer-style I/O, file and positional I/O, copy helpers, and durability operations.

The asynchronous side provides explicit operations, caller-owned completions, bounded request state, a scheduler/fiber runtime, synchronization primitives, cancellation, and runtime lifecycle management. `ThreadPoolBackend` is the ordinary blocking-I/O backend; Linux io_uring support exists behind the liburing build option.

The implementation under `include/` and `src/` is authoritative. Documentation describes the current code; it does not define a parallel historical model of the system.

## Applications

The retained applications are real consumers of the public API:

- [`sluice-copy`](apps/sluice-copy/README.md) — bounded file copy
- [`sluice-hash`](apps/sluice-hash/README.md) — streaming SHA-256
- [`sluice-grep`](apps/sluice-grep/README.md) — streaming literal search
- [`sluice-tail`](apps/sluice-tail/README.md) — last-N and follow-mode tailing

## Build

Sluice uses [Xmake](https://xmake.io) and requires a C++20 compiler.

The build description is being reduced together with the repository. `xmake.lua` and `xmake/` are the source of truth for currently available targets.

## Documentation

- [Architecture](docs/architecture/README.md)
- [Architecture Decision Records](docs/adr/README.md)
- [Historical research results](research/RESULTS.md)

## Project direction

The project is no longer driven by broad research campaigns. The near-term priorities are:

1. keep the retained C++ implementation usable;
2. reduce unnecessary modules and build machinery;
3. reconstruct tests from the current code;
4. rebuild formal verification and TLA+ models from the current C++ implementation;
5. optimize only measured, fine-grained hotspots after the architecture is stable.

Old campaign names, issue chronology, research scaffolding, and formal models are intentionally not carried forward as context.

## License

Sluice is licensed under the [MIT License](LICENSE).
