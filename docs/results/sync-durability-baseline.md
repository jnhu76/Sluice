# Sync durability baseline (sluice-CORE-020S)

**Status: scoped baseline row, NOT a universal durability claim.** Records the
*cost* of `sync_data`/`sync_all` under a per-batch (here per-file) cadence on a
single stream, so async W4 (overlapped durability) has a defined blocking
baseline number to compare against. Per `docs/sync-durability-model.md` §5.

This complements the multi-stream W4 row in `docs/results/sync-w1-w4-baseline.md`
— that one sweeps `streams`; this one fixes `streams=1` and isolates the sync
cost as the variable.

## Scope

- **Machine:** WSL2 (linux 6.18 x86_64), single dev environment.
- **Build:** xmake debug. NOT timing-clean (no -O2); absolute numbers are
  pessimistic. The *ratio* (sync vs none) is the robust signal.
- **Filesystem:** Linux tmpfs-backed `/tmp`. **CRITICAL CAVEAT:** tmpfs has no
  physical disk, so fsync/fdatasync return near-instantly. **The sync cost
  recorded here is a LOWER BOUND; on a real disk it is expected to dominate.**
  Do NOT generalize the absolute sync overhead to physical storage.
- **Repro:** `xmake build w4_durability_bench && xmake run w4_durability_bench`.
  The single-stream (`streams=1`) rows are this baseline.

## The baseline row (per-file cadence, single stream, debug + tmpfs)

From `w4_durability_bench`, `streams=1`, `block_size=4096`, `blocks_per_stream=32`
(128 KiB total per stream):

| sync_policy | relative cost vs `none` | interpretation |
|---|---|---|
| `none` | 1.0x (reference) | OS buffers; no explicit sync. |
| `sync_data_every_file` | ~1.0–1.3x | fdatasync on tmpfs is near-free. **Understates real disk.** |
| `sync_all_every_file` | ~1.1–1.6x | fsync slightly above fdatasync (metadata). Same caveat. |

**These ratios are tmpfs-specific.** The robust conclusion is *not* "sync is
cheap" — it is "on this environment sync is cheap; re-measure on the target disk
before any durability decision (decision Q4)."

## What this baseline establishes

- The **policy names** (`none`, `sync_data_every_file`, `sync_all_every_file`)
  are wired through the bench and produce reproducible CSV rows
  (`docs/sync-durability-model.md` §4).
- The **single-stream** sync cost is *defined and measurable* — async W4 has a
  concrete blocking baseline row to beat, not an undefined one.
- The **per-file cadence** is the documented baseline; per-batch cadence
  (`sync_data_every_batch`) reduces to per-file when batch == file, and is
  strictly more expensive otherwise.

## What this baseline does NOT establish

- No physical-disk durability number (tmpfs caveat above).
- No multi-stream overlapped-sync number here — that is the W4 row in
  `sync-w1-w4-baseline.md` (and still tmpfs-bounded).
- No claim that `sync_data` < `sync_all` universally — on tmpfs the difference
  is small; on disk with metadata changes it can be large.

## Cadence policy (restated from sync-durability-model.md §4)

The blocking baseline ships with a **caller-driven** cadence by default: the
caller calls `sync_data()`/`sync_all()` explicitly. The bench policy names label
*how a bench drives sync*, not a public cadence API. No change to `SyncableWriter`
semantics or WAL behavior was made for this baseline (020S abort condition
honored).

## Cross-links

- Durability model: `docs/sync-durability-model.md`.
- W4 multi-stream baseline: `docs/results/sync-w1-w4-baseline.md`.
- Bench target: `bench/w4_durability_bench.cpp`.
- Job: `docs/sync-io-next-jobs.md` (020S).
