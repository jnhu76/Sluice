# Flush / sync / durability semantics

**Status: contract documented in SLUICE-CORE-008A; implemented in 008B–008G.**
This separates three concepts that were previously conflated under a single
`flush()`:

```text
flush buffered bytes   — BufferedWriter drains dirty bytes into the inner writer
sync file data         — ask the OS to persist file *data* (fdatasync)
durable WAL commit     — flush + sync + advance the durable LSN
```

This is not a performance claim.

## 1. Scope

SLUICE-CORE-008 adds explicit sync/durability semantics on top of the existing
drain-style `flush()`. It does **not** add group commit, async, io_uring, or a
thread pool. It makes durability an explicit, opt-in, observable operation.

## 2. Terminology

- **flush** — move user-space buffered bytes to the next writer down the stack.
  For `BufferedWriter` this drains dirty bytes into the inner writer. It is a
  *byte movement* operation, not a durability operation.
- **sync** — ask the operating system to persist already-written file data
  and/or metadata to stable storage. Maps to `fdatasync` / `fsync`.
- **durable** — an application-level claim that data has survived a crash, which
  requires a sync (and, for WAL, a policy that says a record is committed only
  after sync).

## 3. `flush()` means drain buffered bytes

```text
flush != fsync
fsync != application-level commit unless the WAL policy says so
```

| Layer | `flush()` does | Implies durability? |
|---|---|---|
| `Writer::flush()` | generic virtual | no |
| `BufferedWriter::flush()` | drain dirty bytes into inner writer, then `inner.flush()` | no |
| `FileWriter::flush()` | **documented no-op this phase** — no `fsync`/`fdatasync` | no |

`FileWriter::flush()` deliberately does **not** call `fsync`. Conflating the two
would let callers believe data is durable when it is only in the OS page cache.
Durability is an explicit, separate operation (§4–§5).

## 4. `sync_data()` means request file data persistence

`SyncableWriter::sync_data()` maps to `fdatasync(fd)` on POSIX where available.
It requests persistence of the file's *data* (not necessarily metadata that
doesn't affect data retrieval). It is an explicit, opt-in capability interface
— a writer need not implement it, and `flush()` never calls it.

## 5. `sync_all()` means request file data + metadata persistence

`SyncableWriter::sync_all()` maps to `fsync(fd)`. It requests persistence of
both data and metadata. Like `sync_data()`, it is opt-in and never invoked by
`flush()`.

## 6. WAL durability model

`wal::WalWriter` (008E) tracks three LSNs with the invariant:

```text
durable_lsn <= flushed_lsn <= written_lsn
```

- `written_lsn` — logical bytes accepted by the WAL record methods.
- `flushed_lsn` — advances to `written_lsn` only after `flush()` succeeds.
- `durable_lsn` — advances to `flushed_lsn` only after `sync()` succeeds.

`sync()` flushes first, then (if a `SyncableWriter*` is attached) calls
`sync_data()`, then advances `durable_lsn`. With no `SyncableWriter*`, `sync()`
returns `invalid_state`. This is **not** group commit — each `sync()` is a
single-writer barrier.

## 7. Relationship to Zig `std.Io.Writer.flush`

Zig `std.Io.Writer.flush` is the drain operation that empties the interface-
owned buffer into the underlying sink — the same concept as sluice's
`BufferedWriter::flush()`. Zig does **not** overload `flush` to mean durability;
durability in Zig lives at the `std.fs.File` layer (`sync`, `updateTimes`)
separately. sluice mirrors that separation: `flush()` drains, `SyncableWriter`
syncs. See `docs/zig-std-io-parity-audit.md`.

## 8. What is deferred

- **Group commit** — batching multiple writers' syncs. Not implemented.
- **Async sync** — `io_sync`/AIO. Not implemented (no async backend).
- **Batch WAL commit** — multiple records per sync barrier are possible via
  repeated `write_record` + one `sync()`, but there is no group coordinator.
- **`fsync`-per-record policy** — left to the caller via `sync()` cadence.

## 9. No performance claim

`SyncStats` (008D) counts sync calls/errors — it is an observability hook, not a
latency/throughput number. Whether `sync_data()` vs `sync_all()` vs no-sync
matters for a given workload is unmeasured until SLUICE-CORE-010. No claim is
made here that any sync path is faster or that flush implies durability.

Zig `std.Io` remains a **design reference only**; not a dependency, no code
copied.
