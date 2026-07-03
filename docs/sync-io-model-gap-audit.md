# Sync I/O model gap audit

**Status: sluice-CORE-016G (sync-first planning patch).** This is a planning
artifact, not code. It inventories the **blocking** I/O model as it stands at the
016 design baseline and identifies the gaps that must be closed *before* async
implementation (jobs 017+) may start. The reason: ADR 016D compares async against
a "blocking baseline," but the current blocking baseline is **sequential
single-stream only** — it is not an engineered baseline for the multi-stream W1–W4
workloads async targets (016B). Comparing async against a sequential baseline
would be unfair to blocking and would not isolate async's real value. This audit
feeds `docs/sync-before-async-readiness-gate.md` (the gate) and
`docs/sync-io-next-jobs.md` (the sync-first job cards).

The async **design** (016A–016F) remains accepted. This patch only **defers async
implementation** behind a sync-first completion phase. It changes no async
decision and removes no async doc.

## 1. What the blocking model has today (inventory)

All of the following live in the `sluice` namespace and are blocking, single-threaded.

| Capability | Where | Status | Notes |
|---|---|---|---|
| `Reader` / `Writer` | `reader.hpp`, `writer.hpp` | present | `read_some`/`write_some` primitives; derived `read_exact`/`write_all`/`stream_to`; vector `read_vec`/`write_vec`/`read_vec_all`/`write_all_vec`. |
| `FileReader` / `FileWriter` | `file.hpp`, `src/file.cpp` | present | Blocking POSIX backend; EINTR retry; errno preserved on open failure. |
| `IoContext` / `BlockingIoContext` | `io_context.hpp` | present | Backend factory boundary; default backend. |
| `MemoryIoContext` | `memory_io_context.hpp` | present | Deterministic in-memory fake for tests. |
| Vector I/O | `reader.hpp`, `writer.hpp`, `src/file.cpp` | present | `readv`/`writev` overrides with `IOV_MAX` chunking. |
| Buffered fast path | `buffered_readable.hpp`, `buffer.hpp` | present | `BufferedReadable` + `copy_all` fast path. |
| Copy strategy layer | `copy_strategy.hpp`, `src/copy_strategy.cpp` | present | Auto/Scratch/BufferedFirst + deferred slots. |
| Durability primitives | `sync.hpp`, `src/file.cpp` | **partial** | `SyncableWriter::sync_data` (fdatasync) / `sync_all` (fsync). No policy. |
| WAL durability | `wal.hpp`, `src/wal.cpp` | present | `written ≤ flushed ≤ durable` LSN invariant; single-writer barrier. |
| Measurement structs | `measurement.hpp` | present | `SyscallStats`/`BufferStats`/`CopyStats`/`SyncStats`/`VectorStats`/`UringStats` — raw counters, caller-owned. |
| Microbench harness | `bench/`, `docs/bench-methodology.md` | **partial** | 5 bench targets; **all single-stream sequential**. |

## 2. Gaps that block a fair async comparison

These are the things the blocking baseline must gain before async is measured
against it. Each maps to a sync-first job card in `docs/sync-io-next-jobs.md`.

### G1 — Positional I/O is absent (parity gap with async P1)

The blocking POSIX backend is uniformly **implicit-cursor**. Evidence:

```text
src/file.cpp:95   read (fd, dst.data(), dst.size())      — implicit position
src/file.cpp:208  write(fd, src.data(), src.size())      — implicit position
src/file.cpp:142  readv(fd, iov, count)                  — implicit position
src/file.cpp:258  writev(fd, iov, count)                 — implicit position
```

No `pread`/`pwrite`/`preadv`/`pwritev` is used anywhere in `src/`. No public API
on `FileReader`/`FileWriter`/`Reader`/`Writer` takes an explicit offset. The only
positional API in the whole tree is the **experimental** `UringWriteBatch::write_all(fd, bytes, file_offset)`.

This is a direct parity gap: ADR 016D §6 P1 makes async ops positional by default
so W1/W2 ("many files or many offsets") are expressible without cursor races.
The blocking baseline cannot express "many offsets on one fd" without caller-
managed `lseek`+`read` — and sluice has **no `lseek` wrapper** either. **Decision
required:** add positional blocking read/write (`pread`/`pwrite` and
`preadv`/`pwritev`), or explicitly document why blocking stays cursor-only and
how async-vs-blocking comparison accounts for the difference. See job 018S.

### G2 — Vector I/O exists but is non-positional

`read_vec`/`write_vec` are present and tested, but they use the no-offset POSIX
`readv`/`writev` (G1). If positional I/O is added (018S), the vector variants
must gain positional forms (`preadv`/`pwritev`) for symmetry, or the gap is
documented.

### G3 — Derived helper closeout

`read_exact`/`write_all`/`read_vec_all`/`write_all_vec` exist. Audit whether the
positional variants (if added) need matching derived helpers (`read_exact_at`,
`write_all_at`), and whether any existing helper has untested edge cases. This is
a closeout/audit job (019S), not new architecture.

### G4 — No durability POLICY, only primitives

`docs/design-flush-sync-durability.md` is explicit: the work "does not add group
commit, async, io_uring, or a thread pool." Only the two primitives
(`sync_data`/`sync_all`) and an observability counter exist. Open questions a
blocking durability model should answer before async durability (W4) is compared:

```text
D1. When should a blocking writer sync? (per-record / per-batch / never / caller-decides)
D2. Is there a documented sync cadence policy, or only "caller calls sync_*"?
D3. WAL group-commit: the WAL is a single-writer barrier today (no group commit).
    Is that an accepted limit or a gap to close before async W4 comparison?
D4. What is the blocking baseline for overlapped durability? (Today: none — sync
    is a blocking tail on each writer. There is no "next batch overlaps fsync"
    on the blocking side either.)
```

Job 020S documents the blocking durability model (policy + measured baseline). It
does NOT add group commit (016B O5 keeps that out of scope) — it records what the
blocking durability baseline *is*, so async W4 has something defined to beat.

### G5 — No blocking bounded pool / concurrency primitive

Grep of `src/` and `include/` for `std::thread|mutex|atomic|future|thread_pool`
returns **zero matches**. The blocking path is strictly single-threaded
sequential. There is:

```text
- no thread pool
- no bounded worker pool
- no concurrent-copy primitive
- no baseline for "N concurrent blocking copies on a pool" (W3 blocking equivalent)
```

This is the largest gap. Async's whole value proposition is "many outstanding ops
on few threads" (016B W1–W4). If the blocking baseline cannot express "N streams
on a pool of K threads" at all, then async-vs-blocking compares "async concurrency"
against "blocking sequential" — which is exactly the unfair comparison the
sync-first phase exists to prevent. Job 021S adds a **bounded blocking pool
baseline** (std::thread based, no new dependency) so W1–W4 have a *concurrent
blocking* row, not only a sequential one.

### G6 — Stats lack multi-stream / latency fields

The measurement structs are raw cumulative counters only. `BenchResult` carries
`elapsed_ns` + `bytes` (computed to throughput after the fact) but has:

```text
- no per-op latency field
- no concurrency / active-streams count
- no queue-depth / in-flight / tail-latency fields
- no histogram / percentile support
```

For a W1–W4 benchmark matrix where concurrency is the variable, the stats and
bench harness need at minimum an active-streams count and a way to record
per-cell concurrency. Job 022S extends stats/bench for multi-stream cells.

### G7 — Benchmark coverage is single-stream only

All five bench targets are single-stream sequential:

| Target | Measures | Streams |
|---|---|---|
| `small_writes_bench` | raw/buffered/observed/vector writes | 1 |
| `copy_strategy_bench` | Scratch/BufferedFirst/Auto copy | 1 (in-memory) |
| `wal_write_bench` | scalar/vector WAL, buffered variants | 1 |
| `sync_smoke_bench` | flush/sync_data/sync_all | 1 |
| `uring_write_bench` | blocking vs experimental uring | 1 |

`docs/bench-methodology.md` explicitly excludes multi-stream and async regimes.
**W1–W4 have zero blocking benchmark coverage.** Job 022S builds the blocking
W1–W4 matrix (concurrent independent writes/reads/copy/overlapped-durability)
against the pool baseline from 021S. Job 023S is an optimization pass that tunes
the blocking baseline (buffer sizes, vector chunking, sync cadence) using the
matrix evidence — so async is later compared against an *engineered* blocking
baseline, not a naive one.

## 3. Why this matters (the fairness argument)

```text
Async's value (016B)  = many outstanding ops on few threads (W1-W4).
Blocking today        = one op at a time, one thread, sequential.

Comparing async-W1..W4 against sequential-blocking answers the wrong question.
The right comparison is async-W1..W4 vs blocking-W1..W4-on-a-pool, which
isolates async's real delta (event-driven completion vs thread-per-op) instead
of conflating it with "concurrency vs no concurrency."

Therefore: before async implementation starts, close G1-G7 so the blocking
baseline is a fair, engineered, multi-stream baseline. This is the sync-first
readiness gate (docs/sync-before-async-readiness-gate.md).
```

This is **not** a claim that blocking-with-a-pool will match async — it may or
may not, workload-dependently (no universal claim, per 016B C9). It is a claim
that the comparison must be *defined* before it is run.

## 4. Out of scope for the sync-first phase

These remain excluded (consistent with 016B O1–O10):

```text
- networking, timers, process APIs, mmap, group commit
- async runtime / coroutine / io_uring (that is what the gate gates)
- universal performance claims
- changing BlockingIoContext as the default backend
- changing Reader/Writer semantics
- any new dependency (the pool uses std::thread only)
```

The sync-first phase is **blocking-only engineering**: positional I/O, derived
helper closeout, durability policy documentation, a bounded pool baseline, a
W1–W4 benchmark matrix, and an optimization pass. No async.

## 5. Cross-links

- Async design (accepted, frozen): `docs/adr/ADR-async-io-model.md` (016D).
- Async problem statement (W1–W5): `docs/async-problem-statement.md` (016B).
- Sync-first job cards: `docs/sync-io-next-jobs.md` (017S–023S).
- Sync-first readiness gate: `docs/sync-before-async-readiness-gate.md`.
- Async next jobs (now blocked behind the sync gate): `docs/async-next-jobs.md` (016F).
- Existing durability design: `docs/design-flush-sync-durability.md`, `docs/design-wal-durability.md`.
- Existing bench methodology: `docs/bench-methodology.md`, `docs/bench-decision-matrix.md`.
