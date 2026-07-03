# Optimization decision matrix

**Status: CPPIO-CORE-011D.** Records cautious, evidence-linked optimization
decisions. Per the runbook (`docs/optimization-runbook.md`), every entry is
scoped ("on this host, in this run, for this workload") and never a universal
claim.

```text
No rule without evidence.
No rule without scope.
No universal claims.
```

## Categories

- **Accepted (implemented already)** — already in the codebase; evidence points
  here for context.
- **Candidate (deferred)** — observed win, not implemented; link to evidence.
- **Rejected (deferred)** — observed no win or unstable; recorded to avoid retry.

## Decisions

### 1. `CopyStrategy::Auto == BufferedFirst`
- **Category:** Accepted (implemented in CPPIO-CORE-006, made explicit in 007).
- **Scope:** in-memory reader → in-memory writer, all sizes tested.
- **Evidence:** `docs/microbench-summary-sample.txt`, `copy_strategy` case:
  BufferedFirst matches or beats Scratch at every tested size on the sample
  host, and never loses. Auto resolves to BufferedFirst.
- **Caveat:** file-backed readers/writers (real syscalls) were not the focus of
  the in-memory copy bench; re-measure before generalizing to file I/O.

### 2. BufferedWriter collapses small-write syscalls
- **Category:** Accepted (pre-007 behavior; measured in 010C).
- **Scope:** 1B–512B writes to a temp file, sample host.
- **Evidence:** `small_writes` case: `raw_file_writer` at 1B/iter issues one
  write syscall per byte; `buffered_writer` at 1B/iter collapses to ~256 syscalls
  per MiB (one per buffer refill). On the sample host the elapsed time drops
  accordingly. This is the textbook case for buffering and is already the default
  behavior.
- **Caveat:** at 4KB chunk size the syscall advantage shrinks; the win is
  size-dependent.

### 3. WAL vector write vs scalar write — no universal claim
- **Category:** Candidate (deferred) / not yet a rule.
- **Scope:** temp file, payloads 16B–16KB, 256 records, no sync.
- **Evidence:** `wal_write` case: on the sample host, `write_record_vec` reduces
  write syscalls (1 per record vs 3 for scalar) but the elapsed time does not
  show a consistent win across payload sizes — at small payloads the scalar path
  was observed to be quicker in this single run; at large payloads the picture
  differs. The syscall reduction is real and measured; the wall-clock win is not
  stable.
- **Decision:** Do NOT switch WAL defaults. The syscall reduction is observable
  via `VectorStats` already (005), so callers who care can request the vector
  path. Re-measure with repeats before any default change.

### 4. `sync_all` vs `sync_data` vs flush-only — environment-dominated
- **Category:** Rejected (deferred) as a general rule.
- **Scope:** temp file, 50 iterations, 4KB writes, sample host.
- **Evidence:** `sync_smoke` case: the rank of flush_only / sync_data / sync_all
  is dominated by filesystem/disk and was not stable. `sync_data_calls` /
  `sync_all_calls` counters confirm the paths were exercised.
- **Decision:** No default policy change. Durability cadence stays caller-chosen
  via `wal::WalWriter::sync()`.

## How to update this matrix

Follow the runbook (011A): run in release, summarize, interpret with repeats,
then add/revise an entry with category + scope + evidence link + decision.
Never delete a Rejected entry without new evidence — that's the point of
recording it.
