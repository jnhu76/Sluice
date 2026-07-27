# Async deferred until sync baseline

**Status: SYNC-IO-COMPLETE Phase 11 (sync doc reconciliation).** A one-page
pointer that consolidates the async-deferral statement. It does **not** duplicate
or replace the async design docs (which remain accepted on this branch); it
states, in one place, that async implementation is deferred behind the sync-first
baseline and why.

## Statement

```text
Async remains a future direction, but implementation must compare against
engineered blocking baselines rather than naive sequential blocking. Therefore
async implementation is BLOCKED until the sync-first baseline exists.
```

## What "sync-first baseline exists" means

The sync-first readiness gate (`docs/sync-before-async-readiness-gate.md`) is
GREEN. Concretely:

```text
- sync primitive semantics audited         (docs/sync-io-model.md; job 017S/019S)
- positional blocking I/O decision made     (docs/sync-io-model.md; job 018S)
- durability model documented               (docs/sync-durability-model.md; job 020S)
- blocking bounded pool baseline exists     (BlockingIoPool; job 021S)
- W1-W4 blocking benchmark matrix exists    (docs/sync-bench-matrix.md; job 022S)
```

## What is NOT changed by this deferral

```text
- The async DESIGN (ADR docs/adr/ADR-async-io-model.md, 016D) remains ACCEPTED.
  Only async IMPLEMENTATION (code) is blocked.
- No async doc is removed or weakened.
- async design work (further ADRs/docs) may continue; async CODE may not.
```

## Where the details live

- The gate: `docs/sync-before-async-readiness-gate.md` (status: BLOCKED).
- Why the gate exists: `docs/sync-io-model-gap-audit.md` (the fairness argument).
- The sync architecture being completed first: `docs/sync-io-architecture.md`.
- The accepted async design: `docs/adr/ADR-async-io-model.md` (016D).
- Async job cards (blocked): `docs/async-next-jobs.md` (016F).

## Cross-links

- Sync-first readiness gate: `docs/sync-before-async-readiness-gate.md`.
- Sync architecture: `docs/sync-io-architecture.md`.
- Async ADR (accepted): `docs/adr/ADR-async-io-model.md`.
- Async next jobs (blocked): `docs/async-next-jobs.md`.
- Gap audit (rationale): `docs/sync-io-model-gap-audit.md`.
