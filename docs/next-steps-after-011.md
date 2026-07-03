# Next steps after CPPIO-CORE-011

**Status: CPPIO-CORE-011E.** With 004–011 landed, the correctness + measurement
foundation is in place. This records what the matrix points at, without starting
any new work.

## Where the project stands

- 004: measurement hooks everywhere (optional stats, no globals).
- 005: vector I/O (`read_vec`/`write_vec`/`write_all_vec`) + POSIX overrides.
- 006: buffered fast path (`BufferedReadable` + copy_all strategy probe).
- 007: explicit copy strategy layer (`CopyStrategy`/`CopyDecision`).
- 008: flush/sync/durability separation (`SyncableWriter`, `WalWriter`).
- 009: backend boundary (`IoContext`/`BlockingIoContext`).
- 010: core microbench harness + CSV.
- 011: optimization runbook + summarizer + decision matrix.

## What the matrix points at (candidates, not commitments)

1. **Re-measure the WAL vector path with repeats.** 011D decision #3: the
   syscall reduction is real and measured, but the wall-clock win is unstable.
   A repeated-measurement run could graduate it from Candidate to Accepted or
   Rejected.
2. **File-backed copy strategy measurement.** 011D decision #1 caveat: the
   in-memory copy bench didn't exercise real syscalls on both ends. A
   file→file copy bench would let the Auto/Scratch/BufferedFirst decision be
   evaluated where syscalls dominate.
3. **A `MemoryIoContext`** if/when a real test need appears (009D deferred it;
   the seam exists).

## Hard boundaries still in force

- No io_uring (012, requires async preconditions that don't exist yet).
- No async/evented backend.
- No thread pool.
- No group commit coordinator.
- No universal performance claims — every rule stays scoped + evidence-linked.

## Not started here

This document is descriptive, not prescriptive. Starting any candidate requires
a new task that re-measures first (per the runbook) before implementing.
