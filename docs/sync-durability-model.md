# Sync durability model

**Status: SYNC-IO-COMPLETE Phase 6 (sync doc reconciliation).** Defines the
blocking durability model: the three durability-related operations, what each
guarantees, and the benchmark **sync-policy names** used by the W1–W4 blocking
benchmark matrix. It extends (does not replace) `docs/design-flush-sync-
durability.md` and `docs/design-wal-durability.md`. Async overlapped durability
(W4) is out of scope here — it is blocked behind the sync-first gate; this doc
defines the blocking baseline async W4 must beat.

## 1. The three operations

```text
flush()
  Layer-specific contract. For FileWriter it is a documented NO-OP for user-space
  state (it does NOT call fsync/fdatasync). For buffered layers (BufferedWriter,
  WalWriter) it drains dirty user-space bytes to the inner writer then calls
  inner.flush(). flush is NEVER durability. It is "push user-space buffered state
  to the underlying sink."

sync_data()        // fdatasync — data integrity
  Requests the OS persist file DATA. Returns when the kernel has acknowledged the
  sync. Maps to fdatasync. Available via the SyncableWriter capability mixin
  (dynamic_cast detection), NOT as a Writer virtual.

sync_all()         // fsync — data + metadata integrity
  Requests the OS persist file DATA + METADATA. Returns when the kernel has
  acknowledged the sync. Maps to fsync. Same capability-mixin surface as
  sync_data.
```

## 2. What each guarantees (and does not)

```text
- A COMPLETED sync_data/sync_all Completion/result means the kernel acknowledged
  the sync request. It is NOT a physical-disk durability guarantee beyond what
  the OS gives fdatasync/fsync (which is filesystem/hardware dependent).
- sync is EXPLICIT and BLOCKING. The caller decides when.
- normal write helpers (write_some/write_all/write_vec/write_all_vec / positional
  write_at/write_at_all) DO NOT imply sync. Writes are buffered by the OS; without
  an explicit sync they may be lost on crash.
- errors propagate through Result<void> (or the existing equivalent). EINTR is
  retried internally; other errors map through from_errno_value.
```

## 3. WAL durability (existing, restated)

`WalWriter` maintains the invariant `durable_lsn <= flushed_lsn <= written_lsn`
(`docs/design-wal-durability.md`):

```text
  write_record(_vec)  advances written_lsn
  flush()             advances flushed_lsn to written_lsn
  sync()              flushes, then sync_data(), then advances durable_lsn
```

The WAL is a **single-writer barrier** today: no group commit (016B O5 keeps group
commit out of scope). `durable_lsn` reflects that fdatasync was *requested*, not a
stronger crash-safety guarantee than the OS provides.

## 4. Benchmark sync-policy names

These are **policy names for benchmark labeling**, not a large public policy API.
They name how a benchmark cell drives sync, so W4 (and any W1/W3 cell with a sync
component) is reproducible and comparable. The implementation may be a small enum
or just documented string labels in the bench harness — repository style decides;
this doc fixes the *names and meanings*.

| Policy name | Meaning |
|---|---|
| `none` | no explicit sync during the run (OS decides when to flush) |
| `sync_data_every_file` | `sync_data()` once per file at end of that file's writes |
| `sync_all_every_file` | `sync_all()` once per file at end of that file's writes |
| `sync_data_every_batch` | `sync_data()` after each batch of writes |
| `sync_all_every_batch` | `sync_all()` after each batch of writes |
| `manual` | caller-managed sync (the bench code controls timing explicitly) |

## 5. Blocking overlapped durability (W4 baseline)

For W4, "overlapped durability" on the blocking side is represented by **worker
threads** in `BlockingIoPool` (the execution model, not an I/O backend — see
`docs/sync-io-architecture.md` §3): one stream's fsync can overlap another
stream's writes because they run on different pool workers. This is the
**engineered blocking baseline**, not async overlap. The distinction is recorded
so that future async W4 (true event-driven overlapped sync) is compared against
this defined blocking baseline, not against a sequential fsync tail.

## 6. Out of scope

```text
- group commit / multi-stream atomic commit        (016B O5)
- async sync (AIO / io_sync / overlapped event-driven)  (async W4, gated)
- physical disk durability claims                  (filesystem/hardware dependent)
- a large public sync-policy API                   (these are bench labels)
```

## 7. Tests (behavioral, not physical-durability)

```text
- sync_data succeeds on a temporary file (returns Result<void> success)
- sync_all succeeds on a temporary file
- a closed/invalid writer surfaces the right error (permission_denied/invalid_state)
- normal write helpers do NOT imply sync (write_all then immediate close is not a
  sync — verified structurally/behaviorally, not by crash testing)
- sync stats (SyncStats: sync_data_calls/sync_all_calls + errors) update correctly
```

Avoid physical-disk durability claims. Test API behavior and error propagation.

## 8. Cross-links

- Architecture: `docs/sync-io-architecture.md`.
- Primitive contract: `docs/sync-io-model.md`.
- Existing durability design: `docs/design-flush-sync-durability.md`, `docs/design-wal-durability.md`.
- Bench methodology (uses these policy names): `docs/sync-bench-methodology.md`, `docs/sync-bench-matrix.md`.
- Job that baselines this: `docs/sync-io-next-jobs.md` (020S).
