# Applications — Workload-Driven Development

This directory is the developer entrypoint for Sluice's application-driven
development round: real programs built only on public Sluice headers, whose
friction decides what the foundation does next.

## Why applications exist

Sluice spent its first phase building the foundation (synchronous core, async
runtime, backends). The question that phase could not answer was whether that
foundation is *good enough to build real software on*. Applications answer it:

```text
Build a real application on public headers only
        ↓
Observe repeated friction (record it — do not pre-fix it)
        ↓
Measure it
        ↓
Locate the bottleneck: application algorithm OR public API OR runtime OR backend
        ↓
Optimize the correct layer
```

The reverse direction is explicitly rejected:

```text
Invent a foundation abstraction because it looks elegant
        ↓
Try to find a workload that justifies it
```

**Application asks; foundation responds.** Do not add foundation abstractions
because they look elegant. First build a real application, observe repeated
friction, measure it, and only then decide whether the fix belongs in:

- app-local code;
- shared app support;
- the public Sluice API; or
- a backend layer.

## Current real workloads

The first application round (file tools, 2026-08) shipped four CLI programs
under [`apps/`](../../apps/), all using only `include/sluice/*` public headers
— no test seams, no `src/` includes:

| Application | Workload | App README |
|-------------|----------|------------|
| `sluice-copy` | bounded asynchronous safe file copy | [`apps/sluice-copy/README.md`](../../apps/sluice-copy/README.md) |
| `sluice-hash` | bounded streaming SHA-256 | [`apps/sluice-hash/README.md`](../../apps/sluice-hash/README.md) |
| `sluice-grep` | bounded streaming literal search | [`apps/sluice-grep/README.md`](../../apps/sluice-grep/README.md) |
| `sluice-tail` | backward last-N scan + follow mode | [`apps/sluice-tail/README.md`](../../apps/sluice-tail/README.md) |

Documents in this directory:

- [`file-tools-plan.md`](file-tools-plan.md) — the track plan (Phase A audit,
  capability inventory, per-app scope).
- [`file-tools-findings.md`](file-tools-findings.md) — measured results:
  performance, memory bounds, sanitizer evidence, comparisons with system
  tools, and the API friction that was recorded.

The first application-driven foundation decision (the runtime I/O await API
horse race triggered by `sluice-copy`) is preserved as the decision record
[`../history/implementation-plans/m1-runtime-io-await-race.md`](../history/implementation-plans/m1-runtime-io-await-race.md).

## Future workload directions

Future workloads are **evidence generators, not promises of current public
capability**. Networking and external-memory data structures are not
implemented in Sluice today. Candidate directions for the next rounds:

- **Network server/client workloads** — the canonical second workload; the
  evidence source for any networking API discussion (Sluice currently has no
  socket or poll primitive).
- **External-memory data structures** — KV store, B+ tree, LSM tree, and
  WAL/storage-engine components exercising positional I/O, durability, and
  bounded buffers under sustained load.

Sluice will expand workloads first, then optimize the layers that real
workload evidence exposes.

## Promotion rule

A foundation change candidate recorded by an application becomes real work
only through:

1. an entry in the [application feedback ledger](../roadmap/app-feedback-ledger.md)
   (evidence candidate, not a foundation change);
2. a design under [`../design/`](../design/README.md) or an ADR when it
   changes a decided contract;
3. the applicable AGENTS.md compliance gates;
4. the application track re-measured after the change lands.

Friction worked around locally in one app stays local until a second workload
hits it.
