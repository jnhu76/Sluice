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
- [`docs/adr/0001-explicit-io-design-doctrine.md`](docs/adr/0001-explicit-io-design-doctrine.md) — research-backed explicit-I/O design doctrine.
- [`docs/adr/0002-explicit-file-api-architecture.md`](docs/adr/0002-explicit-file-api-architecture.md) — frozen File-centric semantics, API responsibility, and replaceable-execution architecture.
- [`docs/roadmap/explicit-file-conformance.md`](docs/roadmap/explicit-file-conformance.md) — current master conformance ledger and architecture-completion roadmap derived from ADR-0002.
- [`research/RESULTS.md`](research/RESULTS.md) — retained research evidence and conclusions.

## Architecture at a glance

<p align="center">
  <img src="docs/assets/sluice-architecture.svg" alt="Sluice architecture overview" width="100%">
</p>

The SVG is a compact implementation overview. [`docs/architecture.md`](docs/architecture.md) is the current-code snapshot; the mission and ADRs define normative boundaries; the conformance roadmap records which parts of current master already satisfy those boundaries and which gaps remain.

The `sluice_core` / `sluice_async` build split does not define two independent long-term I/O semantics. ADR-0002 defines one canonical `File` resource and shared operation semantics, with Blocking, ThreadPool, and io_uring as explicit, replaceable execution forms.

A `File` does **not** carry a blocking/async mode or backend choice. Execution is selected by the invocation/API boundary so caller-visible blocking, outstanding-request lifetime, cancellation, and bounded-resource costs remain explicit.

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

Current master has one canonical `sluice::File` resource with explicit open/close/access semantics. File-facing Blocking operations cover positional Read/Write, sequential Read/Write, SyncData/SyncAll, and the minimal observable state surface (`size` / `resize`). File-facing evented operations cover positional Read/Write and SyncData/SyncAll through the existing runtime seam, while explicit outstanding operations carry `NativeFileRef` resource references through `AsyncIoContext`. None of these paths gives the Scheduler or backend File-semantic authority.

The repository still contains the historical blocking `FileReader` / `FileWriter` world; its final disposition is intentionally tracked by legacy-surface audit #355 rather than treated as a second canonical File model. Application File-resource consumption is conforming: hash/grep/tail and copy source lifetime are owned by canonical `File`, while remaining raw/native-handle escapes are classified as required interop or out-of-scope namespace work in the app-consumer census. They are not unclassified conformance gaps.

The asynchronous side contains caller-owned completions, bounded request state, scheduler/runtime machinery, cancellation, synchronization facilities, and honest backend execution. Repository-provided synthetic AsyncBackend implementations have been removed; production execution is ThreadPool plus io_uring when available.

Current architecture work is intentionally split into phases:

```text
Phase A  make the ADR architecture true in code       CLOSED / CONFORMANT
Phase B  prove and compare execution quality
Phase C  optimize or add execution backends/capabilities
```

Phase A was closed by the final conformance audit (#348) and roadmap closure (#339). The conformance roadmap remains the source of truth for the frozen architecture and its verified implementation state; legacy-surface disposition continues separately in #355.

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
- [`docs/adr/0001-explicit-io-design-doctrine.md`](docs/adr/0001-explicit-io-design-doctrine.md) — research-backed explicit-I/O doctrine.
- [`docs/adr/0002-explicit-file-api-architecture.md`](docs/adr/0002-explicit-file-api-architecture.md) — normative File API and execution architecture.
- [`docs/roadmap/explicit-file-conformance.md`](docs/roadmap/explicit-file-conformance.md) — implementation conformance ledger and architecture-completion roadmap.
- [`docs/architecture.md`](docs/architecture.md) — current-code architecture snapshot.
- [`research/RESULTS.md`](research/RESULTS.md) — retained research conclusions.

## License

Sluice is licensed under the [MIT License](LICENSE).
