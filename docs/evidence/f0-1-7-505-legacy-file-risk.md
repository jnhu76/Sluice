# F0 §1.7 External Risk Adjudication: #505-B Legacy File/Reader/Writer

Candidate: #505-B legacy File/Reader/Writer native authority retirement
PR branch: codex/f4-505-retirement
Head: `5679f23c08b5be58f433bf9e534ac7618917ba95`
Base: `f3521b45ead3963162e518758ac1820004adcfc2` (origin/master, post-#509)
Status: **CANDIDATE — pending owner approval**

## Scope

This document adjudicates each of the 15 deleted public installed headers. It does not approve the deletion; the final F0 §1.7 sign-off remains with the human owner.

## Per-interface assessment

| # | Deleted header | v0.0.1 public | In-tree consumer | Canonical equivalent / migration | Breakage impact | Recommended disposition |
|---|----------------|---------------|------------------|----------------------------------|-----------------|------------------------|
| 1 | `sluice/buffer.hpp` | Yes | None | None. Buffer abstraction not carried into v1. | Compile + link failure for consumers using the buffer API. | Delete with F0 §1.7 approval. |
| 2 | `sluice/buffered_readable.hpp` | Yes | None | None. Replaced by direct `sluice::File` positional reads. | Compile + link failure for `BufferedReadable` consumers. | Delete with F0 §1.7 approval. |
| 3 | `sluice/copy.hpp` | Yes | None | None in core. Async runtime has its own pipeline patterns. | Compile + link failure for `copy_all` consumers. | Delete with F0 §1.7 approval. |
| 4 | `sluice/copy_strategy.hpp` | Yes | None | None. Strategy types retired with copy.hpp. | Compile failure for strategy consumers. | Delete with F0 §1.7 approval. |
| 5 | `sluice/fault.hpp` | Yes | None | None. Fault injection now uses internal test seams. | Compile failure for fault-model consumers. | Delete with F0 §1.7 approval. |
| 6 | `sluice/file.hpp` | Yes | None | `sluice/file_resource.hpp` plus `sluice/blocking/file.hpp` for W-01 direct operations; for W-02/W-03 use canonical Request/RequestScope and AsyncIoContext with retained backend/progress interfaces. `sluice/async/file.hpp` was already physically removed in #502 and is **not** a migration target. These are not drop-in replacements. | Compile + link failure; significant migration required. | Delete with F0 §1.7 approval after documenting migration. |
| 7 | `sluice/io_context.hpp` | Yes | None | None. The IoContext/BlockingIoContext split is retired; v1 uses `sluice::File` + backends. | Compile failure for IoContext consumers. | Delete with F0 §1.7 approval. |
| 8 | `sluice/iovec.hpp` | Yes | None | None. Internal scatter/gather types removed. | Compile failure for direct iovec helpers. | Delete with F0 §1.7 approval. |
| 9 | `sluice/limit.hpp` | Yes | None | None. Limit types not carried forward. | Compile failure for limit consumers. | Delete with F0 §1.7 approval. |
| 10 | `sluice/memory_io_context.hpp` | Yes | None | None for production. In-memory deterministic testing now uses internal test seams and the canonical semantic oracle, not a public MemoryIoContext. | Compile failure for in-memory test consumers. | Delete with F0 §1.7 approval. |
| 11 | `sluice/observed.hpp` | Yes | None | None. Observer pattern retired. | Compile failure for Observed consumers. | Delete with F0 §1.7 approval. |
| 12 | `sluice/reader.hpp` | Yes | None | `sluice::File` as resource owner plus `sluice::blocking::read_at` / `read_exact_at` or canonical Request operations; not a class-for-class replacement. | Compile + link failure for Reader consumers. | Delete with F0 §1.7 approval. |
| 13 | `sluice/sync.hpp` | Yes | None | Durability operations are `sluice::blocking::sync_data(const File&)` / `sync_all(const File&)`, with canonical Request-path equivalents; they are not `File` member functions. | Compile failure for sync helper consumers. | Delete with F0 §1.7 approval. |
| 14 | `sluice/wal.hpp` | Yes | None | None. WAL record format not carried into v1. | Compile + link failure for WAL consumers. | Delete with F0 §1.7 approval. |
| 15 | `sluice/writer.hpp` | Yes | None | `sluice::File` as resource owner plus `sluice::blocking::write_at` / `write_all_at` or canonical Request operations; not a class-for-class replacement. | Compile + link failure for Writer consumers. | Delete with F0 §1.7 approval. |

## Detailed notes on high-impact surfaces

### `sluice/file.hpp`

- **Historical role**: The legacy native File/Reader/Writer owner. Documented in v0.0.1 README as part of `sluice_core`.
- **v1 replacement**: `sluice/file_resource.hpp` owns the canonical `File` resource, and `sluice/blocking/file.hpp` provides W-01 direct I/O; W-02/W-03 are expressed with Request/RequestScope, AsyncIoContext and its backends/progress. `sluice/async/file.hpp` is retired as of #502 and must not appear in migration instructions.
- **Migration cost**: High for consumers using the old class hierarchy. The new surface is descriptor-based and positional; it does not expose the same Reader/Writer split.
- **Risk mitigation**: #505-A migrated the last in-tree consumer (`tests/semantic_range_test.cpp`) to the canonical oracle. External consumers need a documented migration guide.

### `sluice/reader.hpp` and `sluice/writer.hpp`

- **Historical role**: Buffered read/write abstractions over IoContext.
- **v1 replacement**: `sluice::File` owns both read and write paths via explicit descriptors.
- **Migration cost**: Medium-to-high; consumers must switch from stream-style reads/writes to positional operations and manage their own buffers.

### `sluice/copy.hpp`

- **Historical role**: `copy_all` helper between Reader and Writer.
- **v1 replacement**: No public equivalent. Applications use `sluice-copy` as a reference implementation or implement the loop directly over `sluice::File`.

### `sluice/wal.hpp`

- **Historical role**: WAL record format helper.
- **v1 replacement**: None. This is a deliberate scope contraction: v1 does not include a WAL surface.

## Summary

All 15 deleted headers were public in v0.0.1 and have no in-tree consumers after #505-A. The canonical v1 surface (`sluice::File`, `sluice/blocking/file.hpp`, `Request`/`RequestScope`, `AsyncIoContext` and retained backend/progress interfaces) addresses required W-01/W-02/W-03 file semantics, but it is not a drop-in replacement for most legacy interfaces. F0 §1.7 owner approval is required for each deleted surface.

| Surface category | Count | Has direct equivalent | Needs migration guide |
|------------------|-------|----------------------|-----------------------|
| File/Reader/Writer | 4 (file, reader, writer, buffered_readable) | Partial (`sluice::File`) | Yes |
| Copy pipeline | 2 (copy, copy_strategy) | None | Yes |
| IoContext / MemoryIoContext | 2 | None | Yes |
| Buffer / iovec / limit / sync | 4 | None | Yes |
| Fault / Observed / WAL | 3 | None | Yes |

## Evidence references

- B-01 package evidence: `docs/evidence/b01-505-legacy-file-retirement.md`
- Raw evidence on host: `/tmp/b01-evidence/pr-510`
- Negative/positive probes: `/tmp/b01-evidence/pr-510-probes`, `/tmp/b01-evidence/pr-510-probes-uring`
