# B-01 Evidence: #505-B Legacy File/Reader/Writer Retirement

Base: `f3521b45ead3963162e518758ac1820004adcfc2` (origin/master, post-#509)
Head: `5679f23c08b5be58f433bf9e534ac7618917ba95` (codex/f4-505-retirement, rebased)
Host: E5 / Fedora 44 / x86_64
Toolchain: GCC 16.2.1 / xmake v3.0.6+20260117
Evidence collected: 2026-10-12

## Removed files

### Installed headers (15)

- `include/sluice/buffer.hpp`
- `include/sluice/buffered_readable.hpp`
- `include/sluice/copy.hpp`
- `include/sluice/copy_strategy.hpp`
- `include/sluice/fault.hpp`
- `include/sluice/file.hpp`
- `include/sluice/io_context.hpp`
- `include/sluice/iovec.hpp`
- `include/sluice/limit.hpp`
- `include/sluice/memory_io_context.hpp`
- `include/sluice/observed.hpp`
- `include/sluice/reader.hpp`
- `include/sluice/sync.hpp`
- `include/sluice/wal.hpp`
- `include/sluice/writer.hpp`

### Sources (10)

- `src/buffer.cpp`
- `src/copy.cpp`
- `src/copy_strategy.cpp`
- `src/fault.cpp`
- `src/file.cpp`
- `src/io_context.cpp`
- `src/observed.cpp`
- `src/reader.cpp`
- `src/wal.cpp`
- `src/writer.cpp`

## Package delta (release, both profiles)

| Metric | Base (master) | Head (#510) | Delta |
|--------|---------------|-------------|-------|
| Installed headers | 73 | 58 | -15 |
| `libsluice_core.a` members | 13 | 3 | -10 |
| `libsluice_async.a` members | 33 | 33 | 0 |

### Remaining `libsluice_core.a` members

- `blocking_file.cpp.o`
- `blocking_io_pool.cpp.o`
- `file_resource.cpp.o`

### Shared helpers intentionally retained

- `include/sluice/detail/posix_retry.hpp` (used by `blocking_file.cpp`, `threadpool_backend.cpp`, `apps/sluice-copy/safe_output.cpp`)
- `include/sluice/detail/file_semantics.hpp` (canonical range oracle)
- `include/sluice/detail/uring_submit.hpp` (async uring backend)

## Dual-profile archive evidence

### Release / no-liburing

Build commands:

```bash
xmake f -m release --liburing=false -y -c
xmake -r sluice_core
xmake -r sluice_async
xmake install -o /tmp/b01-evidence/pr-510/install-release-liburing-false sluice_core
xmake install -o /tmp/b01-evidence/pr-510/install-release-liburing-false sluice_async
```

Results:

| Artifact | Count / hash |
|----------|--------------|
| Installed headers | 58 |
| `libsluice_core.a` members | 3 |
| `libsluice_core.a` sha256 | `c4b8bf521812e9f8a511559200f9be80910255246a776a0bd7f5e7e0d91c213c` |
| `libsluice_async.a` members | 33 |
| `libsluice_async.a` sha256 | `ffbcf8d3f88dff0d7fae06103e0901faa9d04804a5fd8bf272052561c996cf45` |

### Release / liburing

Build commands:

```bash
xmake f -m release --liburing=true -y -c
xmake -r sluice_core
xmake -r sluice_async
xmake install -o /tmp/b01-evidence/pr-510/install-release-liburing-true sluice_core
xmake install -o /tmp/b01-evidence/pr-510/install-release-liburing-true sluice_async
```

Results:

| Artifact | Count / hash |
|----------|--------------|
| Installed headers | 58 |
| `libsluice_core.a` members | 3 |
| `libsluice_core.a` sha256 | `c4b8bf521812e9f8a511559200f9be80910255246a776a0bd7f5e7e0d91c213c` |
| `libsluice_async.a` members | 33 |
| `libsluice_async.a` sha256 | `634afb1bcdc4b7316562cd7b6190f66836262d0cb771da52ba69ed68cada6ffa` |

The `libsluice_core.a` archive is identical between the two profiles. The `libsluice_async.a` archive differs because of the public `SLUICE_HAS_LIBURING` usage requirement.

## Consumer closure

No in-tree production, application, or test consumer remains after #505-A migrated `tests/semantic_range_test.cpp` to canonical `sluice::File` + `sluice::blocking::read_at/write_at`.

## Compile/link probes

Negative probes (deleted interfaces no longer compile):

| Probe | Expected | Result |
|-------|----------|--------|
| `#include <sluice/buffer.hpp>` | compile failure | `fatal error: sluice/buffer.hpp: No such file or directory` |
| `#include <sluice/buffered_readable.hpp>` | compile failure | `fatal error: sluice/buffered_readable.hpp: No such file or directory` |
| `#include <sluice/copy.hpp>` | compile failure | `fatal error: sluice/copy.hpp: No such file or directory` |
| `#include <sluice/copy_strategy.hpp>` | compile failure | `fatal error: sluice/copy_strategy.hpp: No such file or directory` |
| `#include <sluice/fault.hpp>` | compile failure | `fatal error: sluice/fault.hpp: No such file or directory` |
| `#include <sluice/file.hpp>` | compile failure | `fatal error: sluice/file.hpp: No such file or directory` |
| `#include <sluice/io_context.hpp>` | compile failure | `fatal error: sluice/io_context.hpp: No such file or directory` |
| `#include <sluice/iovec.hpp>` | compile failure | `fatal error: sluice/iovec.hpp: No such file or directory` |
| `#include <sluice/limit.hpp>` | compile failure | `fatal error: sluice/limit.hpp: No such file or directory` |
| `#include <sluice/memory_io_context.hpp>` | compile failure | `fatal error: sluice/memory_io_context.hpp: No such file or directory` |
| `#include <sluice/observed.hpp>` | compile failure | `fatal error: sluice/observed.hpp: No such file or directory` |
| `#include <sluice/reader.hpp>` | compile failure | `fatal error: sluice/reader.hpp: No such file or directory` |
| `#include <sluice/sync.hpp>` | compile failure | `fatal error: sluice/sync.hpp: No such file or directory` |
| `#include <sluice/wal.hpp>` | compile failure | `fatal error: sluice/wal.hpp: No such file or directory` |
| `#include <sluice/writer.hpp>` | compile failure | `fatal error: sluice/writer.hpp: No such file or directory` |

Positive probes (retained canonical interfaces still compile and link):

| Probe | Result |
|-------|--------|
| W-01: `#include <sluice/blocking/file.hpp>` + `#include <sluice/file_resource.hpp>` | compile + link OK |
| W-02: `#include <sluice/async/request_scope.hpp>` + `#include <sluice/async/future.hpp>` | compile + link OK |
| W-03: `#include <sluice/async/scheduler.hpp>` + `#include <sluice/async/stackful_io_host.hpp>` | compile + link OK |
| Host: `#include <sluice/async/stackful_io_host.hpp>` | compile + link OK |
| Future: `#include <sluice/async/future.hpp>` | compile + link OK |
| WaitPolicy: `#include <sluice/async/wait_policy.hpp>` | compile + link OK |

Probe commands (release / no-liburing):

```bash
INSTALL=/tmp/b01-evidence/pr-510/install-release-liburing-false
g++ -std=c++20 -I$INSTALL/include -c positive_w01_blocking_file.cpp
g++ -std=c++20 positive_w01_blocking_file.o $INSTALL/lib/libsluice_core.a $INSTALL/lib/libsluice_async.a -lpthread -o positive_w01_blocking_file
```

## Build/test matrix

| Profile | Result |
|---------|--------|
| Release / no-liburing | PASS (53 tests) |
| Release / liburing | PASS (67 tests) |
| ASan+UBSan | UNAVAILABLE (libasan runtime missing on host) |
| TSan | UNAVAILABLE (libtsan runtime missing on host) |

## External risk

This candidate deletes 15 public installed headers and 10 core archive members that were present in the v0.0.1 release. Per-interface risk adjudication is in `docs/evidence/f0-1-7-505-legacy-file-risk.md`. Final F0 §1.7 owner approval is required before merge. This candidate does not close #505.
