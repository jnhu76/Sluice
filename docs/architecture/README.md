# Architecture — Current Authority Index

Every document in this directory is **current architecture authority**: the
as-built design, an active invariant, an active contract, or an active
process/policy. Point-in-time compliance/audit/phase evidence lives in
[`docs/history/closeout/`](../history/closeout/) (see the KNOWN_MOVED
registry in `scripts/check-doc-links.py` for the #290 move map).

Classification baseline: S0-DOCS reclassification (#290, 2026-09-04).
Re-classify when documents change role, not on a schedule.

## Current documents

| Document | Purpose | Pinned by |
|----------|---------|-----------|
| [`architecture-constitution.md`](architecture-constitution.md) | AC-N engineering principles | `AGENTS.md`, `scripts/verify-architecture-docs.py` |
| [`overview.md`](overview.md) | Architecture entry point + authoritative implementation map | root READMEs, `scripts/verify-architecture-docs.py` |
| [`async-runtime.md`](async-runtime.md) | Scheduler/Fiber/execution architecture | — |
| [`async-synchronization.md`](async-synchronization.md) | WaitNode/WaitQueue + synchronization primitives | — |
| [`async-io-foundation.md`](async-io-foundation.md) | Completion/AsyncBackend/AsyncIoContext layer | — |
| [`async-request-lifecycle.md`](async-request-lifecycle.md) | Per-request lifecycle narrative (as-built) | — |
| [`sync-io-architecture.md`](sync-io-architecture.md) | Synchronous I/O model | — |
| [`sync-durability-model.md`](sync-durability-model.md) | Durability contracts (`sync_data`/`sync_all`) | — |
| [`sync-backend-taxonomy.md`](sync-backend-taxonomy.md) | Sync backend boundary decisions | — |
| [`failure-model.md`](failure-model.md) | T1–T7 failure taxonomy + assert policy | `AGENTS.md` §3.8, assert-hygiene gates, `scripts/gates/pre-push.sh`, failure-model tests |
| [`divergence-registry.md`](divergence-registry.md) | Live Zig-divergence registry | `AGENTS.md`, `scripts/verify-architecture-docs.py` |
| [`design-compliance-gate.md`](design-compliance-gate.md) | Generic Gate 0–4 compliance-gate template | `AGENTS.md` §4 |
| [`zig-io-conformance-map.md`](zig-io-conformance-map.md) | Zig std.Io conformance/divergence map | `scripts/verify-architecture-docs.py` |
| [`foundation-freeze.md`](foundation-freeze.md) | Active freeze policy: entry conditions for any foundation change | — |

## Evidence and history (not here)

Point-in-time records that used to live at this level are archived under
[`docs/history/closeout/`](../history/closeout/), including:

- as-built snapshot and audit records (`as-built-async-architecture.md`,
  `current-architecture-findings.md`, `remediation-roadmap.md`) — still
  required to exist by `scripts/verify-architecture-docs.py`;
- phase/issue compliance and conformance gates (`phase-*`, `issue-*`,
  `fe2-*`, `fe3-*`) — still pinned as evidence by
  `spec/tla/manifest.json` and test/script comments where noted.

These records are evidence, not authority for new decisions. Do not move a
pinned document without updating every mechanical consumer in the same change
(`AGENTS.md` §8).
