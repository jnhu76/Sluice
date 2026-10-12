# B-01 Evidence: #505-B Legacy File/Reader/Writer Retirement

Base for #505-A: ecb2481c700e3c6710885ec3af9e903c941ab180 (origin/master)
Base for #505-B: 2107a4f3 (codex/f4-505-oracle, after canonical range migration)
Head: $(git rev-parse HEAD)
Host: E5 / Fedora 44 / x86_64
Toolchain: GCC 16.2.1 / xmake v3.0.6+20260117
Profile: release / no-liburing

## Removed files

### Installed headers (15)

- include/sluice/buffer.hpp
- include/sluice/buffered_readable.hpp
- include/sluice/copy.hpp
- include/sluice/copy_strategy.hpp
- include/sluice/fault.hpp
- include/sluice/file.hpp
- include/sluice/io_context.hpp
- include/sluice/iovec.hpp
- include/sluice/limit.hpp
- include/sluice/memory_io_context.hpp
- include/sluice/observed.hpp
- include/sluice/reader.hpp
- include/sluice/sync.hpp
- include/sluice/wal.hpp
- include/sluice/writer.hpp

### Sources (10)

- src/buffer.cpp
- src/copy.cpp
- src/copy_strategy.cpp
- src/fault.cpp
- src/file.cpp
- src/io_context.cpp
- src/observed.cpp
- src/reader.cpp
- src/wal.cpp
- src/writer.cpp

## Package delta (vs #505-A head)

| Metric | #505-A | #505-B | Delta |
|--------|--------|--------|-------|
| Installed headers | 73 | 58 | -15 |
| libsluice_core.a members | 15 | 3 | -12 |
| libsluice_async.a members | 33 | 33 | 0 |

### Remaining libsluice_core.a members

- blocking_file.cpp.o
- blocking_io_pool.cpp.o
- file_resource.cpp.o

## Shared helpers intentionally retained

- include/sluice/detail/posix_retry.hpp (used by blocking_file.cpp,
  threadpool_backend.cpp, apps/sluice-copy/safe_output.cpp)
- include/sluice/detail/file_semantics.hpp (canonical range oracle)

## Consumer closure

No in-tree production, application, or test consumer remains after
#505-A migrated tests/semantic_range_test.cpp to canonical
sluice::File + sluice::blocking::read_at/write_at.

External/public-API breakage requires per-surface F0 §1.7 owner approval
before merge; this candidate does not close #505.

## Build/test matrix

- [x] Debug / no-liburing: PASS (53 tests)
- [x] Release / no-liburing: PASS (53 tests)
- [x] Debug / liburing: PASS (67 tests)
- [x] Release / liburing: PASS (67 tests)
- [ ] ASan+UBSan: UNAVAILABLE (libasan runtime missing on host)
- [ ] TSan: UNAVAILABLE (libtsan runtime missing on host)
