# F0 §1.7 Decision Table — #505-B Legacy File/Reader/Writer Retirement

| # | Interface | v0.0.1 public | In-tree consumer | Migration / equivalent | Breakage impact | Owner approves deletion |
|---|-----------|---------------|------------------|------------------------|-----------------|-------------------------|
| 1 | `sluice/buffer.hpp` | Yes | None | None | Compile + link failure | ☐ |
| 2 | `sluice/buffered_readable.hpp` | Yes | None | None; use `sluice::File` positional reads | Compile + link failure | ☐ |
| 3 | `sluice/copy.hpp` | Yes | None | None; implement loop over `sluice::File` | Compile + link failure | ☐ |
| 4 | `sluice/copy_strategy.hpp` | Yes | None | None | Compile failure | ☐ |
| 5 | `sluice/fault.hpp` | Yes | None | None | Compile failure | ☐ |
| 6 | `sluice/file.hpp` | Yes | None | `sluice/blocking/file.hpp` / `sluice/async/file.hpp`; not drop-in | Compile + link failure | ☐ |
| 7 | `sluice/io_context.hpp` | Yes | None | None; use `sluice::File` + backends | Compile failure | ☐ |
| 8 | `sluice/iovec.hpp` | Yes | None | None | Compile failure | ☐ |
| 9 | `sluice/limit.hpp` | Yes | None | None | Compile failure | ☐ |
| 10 | `sluice/memory_io_context.hpp` | Yes | None | None; testing via internal seams / oracle | Compile failure | ☐ |
| 11 | `sluice/observed.hpp` | Yes | None | None | Compile failure | ☐ |
| 12 | `sluice/reader.hpp` | Yes | None | `sluice::File` `read_at`; not drop-in | Compile + link failure | ☐ |
| 13 | `sluice/sync.hpp` | Yes | None | `sluice::File::sync_data` / `sync_all` | Compile failure | ☐ |
| 14 | `sluice/wal.hpp` | Yes | None | None (WAL not in v1) | Compile + link failure | ☐ |
| 15 | `sluice/writer.hpp` | Yes | None | `sluice::File` `write_at`; not drop-in | Compile + link failure | ☐ |

**Owner signature / date:** _________________________

**Conditions of approval:**
- [ ] Reviewed B-01 evidence at `docs/evidence/b01-505-legacy-file-retirement.md`.
- [ ] Reviewed per-interface risk adjudication at `docs/evidence/f0-1-7-505-legacy-file-risk.md`.
- [ ] Confirmed #505-A migrated the last in-tree consumer.
- [ ] Confirmed retained canonical W-01/W-02/W-03 surfaces are sufficient for v1.
