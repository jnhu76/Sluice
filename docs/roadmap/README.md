# Roadmap — Thin Pointer

Repository Markdown is **not** an execution authority. Changing status — what
we are doing now, in what order, and where work stops — lives in GitHub Issues:

| Question | Authority |
|----------|-----------|
| What do we do next? (sole execution-order roadmap) | [#227](https://github.com/jnhu76/Sluice/issues/227) |
| Boundary / Safety research program | [#289](https://github.com/jnhu76/Sluice/issues/289) |
| Performance / data-movement research program | [#259](https://github.com/jnhu76/Sluice/issues/259) |
| Architecture constitution (responsibilities, not order) | [#225](https://github.com/jnhu76/Sluice/issues/225) |

## Navigation

- Completed-phase plans and closeouts: [`docs/history/`](../history/README.md)
  (the old repo-local roadmap was archived to
  [`docs/history/archive/Sluice-roadmap.md`](../history/archive/Sluice-roadmap.md)).
- Empirical feedback ledgers: [`docs/applications/app-feedback-ledger.md`](../applications/app-feedback-ledger.md)
  and [`docs/applications/performance-feedback-ledger.md`](../applications/performance-feedback-ledger.md).
- Performance methodology: [`docs/verification/performance-engineering.md`](../verification/performance-engineering.md).

## Standing non-goals

These are durable scope boundaries, not moving execution state:

- No `async`/`await` or coroutine abstraction as a public programming model.
- No P2300 sender/receiver model.
- No networking, OS-level timers, mmap, or group commit.
- No universal performance claims.
- io_uring stays experimental unless real liburing validation supports
  promotion.
