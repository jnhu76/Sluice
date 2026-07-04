# Sync-before-async readiness gate

**Status: sluice-CORE-016G (sync-first planning patch).** A gate, not an
implementation. It lists the preconditions that **must** be satisfied by the
sync-first phase (jobs 017S–023S, `docs/sync-io-next-jobs.md`) before any async
**implementation** (jobs 017+, `docs/async-next-jobs.md`/016F) may start.

It is the analogue of `docs/async-readiness-gate.md` (016E), but one level
earlier: 016E gates *async implementation* on *async design*; this gate gates
*async implementation* on a *fair, engineered blocking baseline*. The async
**design** (016A–016F) remains accepted and is not re-litigated here.

## Decision

**BLOCKED.** Async implementation is **blocked** until this gate is GREEN. The
async model decision (ADR 016D) is unchanged — only the *start of async coding*
is deferred. Rationale (see `docs/sync-io-model-gap-audit.md` §3):

```text
Async's value (016B W1-W4) = many outstanding ops on few threads.
Blocking today             = one op, one thread, sequential only.

Comparing async against sequential blocking conflates "concurrency vs no
concurrency" with "event-driven vs thread-per-op." The sync-first phase closes
gaps G1-G7 so async is later measured against a concurrent, positional,
durability-defined, benchmarked blocking baseline — isolating async's real delta.
```

## 1. Minimum required gate items

Each item maps to a gap in `docs/sync-io-model-gap-audit.md`, a job in
`docs/sync-io-next-jobs.md`, and a contract doc in the SYNC-IO-COMPLETE
architecture/contract layer.

| # | Gate item | Closes gap | Closed by | Contract doc | Status |
|---|---|---|---|---|---|
| 1 | Blocking primitive semantics audited | G3 | 017S + 019S | `sync-io-model.md` | **[IMPL]** |
| 2 | Positional I/O decision made (and, if "add", implemented) | G1, G2 | 017S decision + 018S | `sync-io-model.md` (Positional I/O) | **[IMPL]** |
| 3 | Durability model documented + baseline measured | G4 | 020S | `sync-durability-model.md` | **[IMPL]** |
| 4 | Blocking bounded pool baseline exists | G5 | 021S | `sync-io-architecture.md` §3 (`BlockingIoPool`) | **[IMPL]** |
| 5 | W1–W4 blocking benchmark matrix exists | G6, G7 | 022S | `sync-bench-matrix.md`, `sync-bench-methodology.md` | **[IMPL]** |
| 6 | Blocking baseline engineered (tuned) for W1–W4 | G7 | 023S | `sync-optimization-notes.md` | **[IMPL]** |
| 7 | Async will be compared against engineered blocking baselines, not only sequential | fairness | 022S + 023S produce the rows; async bench (016F job 022) consumes them | `sync-bench-methodology.md` §1 | **[POLICY]** |

All items are **[IMPL]** (proven by the sync-first jobs) except #7, which is a
**[POLICY]** commitment recorded here and enforced when async bench (016F job
022) is written.

## 2. Per-item detail

### Item 1 — Blocking primitive semantics audited

The blocking primitive surface (`read_some`/`write_some`/`read_vec`/`write_vec`/
`read_exact`/`write_all`/`read_vec_all`/`write_all_vec`, plus `sync_data`/
`sync_all`) is reviewed for completeness and untested edge cases (017S signs off
the gap audit; 019S closes derived-helper gaps). No semantic change to existing
methods — additions only.

### Item 2 — Positional I/O decision made

The audit (017S) records a decision: either (a) add `pread`/`pwrite` (+`preadv`/
`pwritev`) to the blocking backend as new opt-in methods (018S), so the blocking
side can express "many offsets on one fd" matching async P1; or (b) explicitly
document why blocking stays cursor-only and how the async-vs-blocking comparison
accounts for the asymmetry. Either resolves G1/G2; the decision must be recorded,
not left implicit.

### Item 3 — Durability model documented + baseline measured

A blocking durability policy exists (020S extends `docs/design-flush-sync-
durability.md`): when to sync, cadence options, and the accepted limits of the
single-writer-barrier WAL. A blocking durability baseline bench row exists
(single-stream per-batch cadence), recorded under `docs/results/`. This gives
async W4 (overlapped durability) a defined blocking baseline to compare against.
Group commit remains out of scope (016B O5).

### Item 4 — Blocking bounded pool baseline exists

A bounded blocking worker pool (021S, `std::thread` based, no new dependency)
exists so W1–W4 are expressible as *concurrent* blocking workloads. It is opt-in
and does NOT replace `BlockingIoContext` as the default. Proven correct under
TSan; buffer/handle lifetime stays caller-owned; no global scheduler.

### Item 5 — W1–W4 blocking benchmark matrix exists

Stats/bench extended for multi-stream cells (022S, G6: active-streams count,
per-cell concurrency) and four concurrent bench targets exist (W1 writes, W2
reads, W3 copy, W4 overlapped durability) parameterized by (streams, pool size,
payload), with at least one recorded run per workload under `docs/results/`.
Scoped per-workload/per-machine, never universal.

### Item 6 — Blocking baseline engineered for W1–W4

Using the 022S matrix, the blocking baseline is tuned (023S: buffer sizes, vector
chunking, copy-strategy selection, sync cadence) with an evidence-linked decision
matrix (extends `docs/bench-decision-matrix.md`). Single-stream tests and the 011
baseline do not regress beyond documented bounds.

### Item 7 — [POLICY] async compared against engineered blocking baselines

When async bench (016F job 022) is written, its blocking comparison rows MUST be
the engineered concurrent W1–W4 rows from 022S/023S, not the sequential
single-stream rows. Sequential rows may be shown for reference but must not be
the basis of the async-vs-blocking comparison. This is the fairness guarantee the
whole sync-first phase exists to provide.

## 3. What this gate is NOT

```text
- NOT a re-litigation of the async design. ADR 016D stays accepted.
- NOT a requirement that blocking-with-a-pool outperforms async. It may or may
  not (workload-dependently; no universal claim). It is a requirement that the
  comparison be DEFINED and run against an engineered baseline.
- NOT a blocker on async DESIGN work. More async design docs/ADRs may proceed;
  only async CODE (jobs 017+) is gated.
- NOT a removal of any async 016 doc.
```

## 4. Going GREEN

The gate flips to **GREEN** when 017S–023S acceptance criteria are met and the
seven items above are satisfied. At that point `docs/async-next-jobs.md` (016F)
unblocks and the async implementation ordering (017 → 019 → 018 → 018B → 020A →
021 → 020B → 022) proceeds, with async bench (016F 022) consuming the engineered
W1–W4 blocking rows per item 7.

## 5. Cross-links

- Gap audit: `docs/sync-io-model-gap-audit.md` (016G).
- Sync-first job cards: `docs/sync-io-next-jobs.md` (017S–023S).
- Async next jobs (blocked until this gate is green): `docs/async-next-jobs.md` (016F).
- Async readiness gate (the next gate, after this one): `docs/async-readiness-gate.md` (016E).
- Async ADR (accepted, unchanged): `docs/adr/ADR-async-io-model.md` (016D).
- Async problem statement (W1–W5): `docs/async-problem-statement.md` (016B).
- Existing durability design: `docs/design-flush-sync-durability.md`.
- Existing bench methodology: `docs/bench-methodology.md`, `docs/bench-decision-matrix.md`.
