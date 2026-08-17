# Reference — The Public Contract

This directory answers: *what exactly does Sluice promise to its users?*

The installed headers under `include/sluice/` are the ultimate contract. The
documents here describe that contract precisely; when a document and a header
disagree, the header wins and the document must be fixed.

## Documents

| Document | Scope |
|----------|-------|
| [`api.md`](api.md) | Public API reference — synchronous core and async runtime (English) |
| [`api.zh-CN.md`](api.zh-CN.md) | Public API reference (Chinese; same facts as `api.md`) |
| [`sync-io-model.md`](sync-io-model.md) | Synchronous I/O behavioral contract stated as testable propositions |
| [`sync-error-semantics.md`](sync-error-semantics.md) | Partial-I/O and error semantics table for the synchronous surface |

## Maintenance rules

- `api.md` and `api.zh-CN.md` carry the same facts, status, and feature
  claims; they are updated together (AGENTS.md §16.1).
- Public API changes require explicit approval and synchronized updates to
  headers, contract tests, this reference, examples, and README text
  (AGENTS.md §16.1).
- The point-in-time public-surface audit that preceded the async work is a
  historical record: [`../history/archive/api-audit.md`](../history/archive/api-audit.md).
