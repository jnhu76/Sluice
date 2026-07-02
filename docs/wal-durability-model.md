# WAL durability model

**Status: model note for CPPIO-CORE-008F.** This documents the invariant and
operations of `wal::WalWriter` (008E) as a lightweight model. It is not a full
TLA+ spec; the invariant is simple enough to state and test directly.

## Invariant

For a `WalWriter` at all times:

```text
durable_lsn <= flushed_lsn <= written_lsn
```

- `written_lsn` — logical bytes accepted by `write_record` / `write_record_vec`
  (the framed record size: 8 header + payload + 4 checksum). Advances only on a
  successful write.
- `flushed_lsn` — advances to `written_lsn` only after `flush()` succeeds.
- `durable_lsn` — advances to `flushed_lsn` only after `sync()` succeeds.

## Operations

| Operation | On success | On failure |
|---|---|---|
| `write_record(_vec)` | `written_lsn += framed_size` | nothing advances |
| `flush()` | `flushed_lsn = written_lsn` | nothing advances |
| `sync()` | flush, then `sync_data()`, then `durable_lsn = flushed_lsn` | nothing advances; partial flush success is reflected (flushed may advance) but durable does not |

`sync()` with no `SyncableWriter*` returns `invalid_state` (and still performs
the flush first, so `flushed_lsn` may advance while `durable_lsn` does not).

## Why no group commit

This is a single-writer barrier per `sync()` call. There is no coordinator that
batches multiple writers' syncs. Group commit is deferred (see
`docs/flush-sync-durability.md` §8).

## Verification

The invariant is checked by `tests/wal_writer_test.cpp` after every operation
(the `invariant(w)` helper), including mixed write/flush/sync sequences and all
three failure paths.

## Not a durability guarantee beyond the OS

`durable_lsn` reflects that `sync_data()` (fdatasync) was *requested*. Actual
persistence depends on OS/filesystem/disk behavior; this model does not claim
crash safety beyond what the OS sync provides. See
`docs/flush-sync-durability.md` §9.
