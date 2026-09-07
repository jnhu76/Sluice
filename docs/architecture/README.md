# Sluice Architecture

This directory contains only current architectural descriptions of the implementation.

The source code under `include/` and `src/` is the implementation authority. These documents describe the current structure and contracts; they are not a project history or research log.

## Current documents

- [overview.md](overview.md) — system-level architecture overview
- [sync-io-architecture.md](sync-io-architecture.md) — synchronous I/O architecture
- [sync-backend-taxonomy.md](sync-backend-taxonomy.md) — synchronous backend roles
- [sync-durability-model.md](sync-durability-model.md) — durability semantics
- [async-io-foundation.md](async-io-foundation.md) — async I/O foundation
- [async-request-lifecycle.md](async-request-lifecycle.md) — request ownership and lifecycle
- [async-runtime.md](async-runtime.md) — runtime and scheduling structure
- [async-synchronization.md](async-synchronization.md) — async synchronization primitives
- [failure-model.md](failure-model.md) — failure behavior and boundaries

Historical architecture governance, divergence registries, freeze records, and conformance campaigns are intentionally not retained in the current tree.
