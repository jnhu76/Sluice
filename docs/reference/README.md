# Reference — The Public Contract

This directory answers: *what exactly does Sluice promise to its users?*

Repository-wide conflict resolution is governed by [`AGENTS.md` §3](../../AGENTS.md).
For day-to-day API lookup, the installed headers under `include/sluice/` and the
canonical narrative reference `api.md` describe the current public surface. If a
header, reference document, accepted ADR, or approved task disagrees, do **not**
silently pick one: resolve the conflict through the AGENTS authority chain and
fix the stale artifact.

> **Translation status (2026-08-18):** `api.zh-CN.md` predates several recent
> explicit-I/O request/runtime additions and is not yet fully synchronized with
> `api.md`. Until that synchronization pass is complete, use the installed
> headers plus `api.md` for canonical current facts.

## Documents

| Document | Scope |
|----------|-------|
| [`api.md`](api.md) | Canonical narrative public API reference — synchronous core and async runtime (English) |
| [`api.zh-CN.md`](api.zh-CN.md) | Chinese companion reference; currently has known translation lag |
| [`sync-io-model.md`](sync-io-model.md) | Synchronous I/O behavioral contract stated as testable propositions |
| [`sync-error-semantics.md`](sync-error-semantics.md) | Partial-I/O and error semantics table for the synchronous surface |

## Maintenance rules

- Public API changes require explicit approval and synchronized updates to
  headers, contract tests, the canonical reference, examples, and README text
  where affected (AGENTS.md §6).
- `api.zh-CN.md` should be updated with the same public facts whenever
  `api.md` changes. A translation mismatch is documentation debt, never an
  alternate contract.
- The point-in-time public-surface audit that preceded the async work is a
  historical record: [`../history/archive/api-audit.md`](../history/archive/api-audit.md).
