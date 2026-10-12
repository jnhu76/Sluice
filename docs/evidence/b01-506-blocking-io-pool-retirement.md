# B-01 Evidence: #506 BlockingIoPool Physical Retirement

Base: `f3521b45ead3963162e518758ac1820004adcfc2` (origin/master, post-#509)
Head: `f0647a705f8d522394fdb1fdd86a9e258d93d6ea` (codex/f4-506-pool, rebased)
Host: E5 / Fedora 44 / x86_64
Toolchain: GCC 16.2.1 / xmake v3.0.6+20260117
Evidence collected: 2026-10-12

## Removed product files

- `include/sluice/blocking_io_pool.hpp`
- `include/sluice/detail/blocking_io_pool_impl.hpp`
- `src/blocking_io_pool.cpp`

## Package delta (release, both profiles)

| Metric | Base (master) | Head (#508) | Delta |
|--------|---------------|-------------|-------|
| Installed headers | 73 | 71 | -2 |
| `libsluice_core.a` members | 13 | 12 | -1 |
| `libsluice_async.a` members | 33 | 33 | 0 |

### Removed installed headers

- `sluice/blocking_io_pool.hpp`
- `sluice/detail/blocking_io_pool_impl.hpp`

### Removed archive member

- `blocking_io_pool.cpp.o`

## Dual-profile archive evidence

### Release / no-liburing

Build commands:

```bash
xmake f -m release --liburing=false -y -c
xmake -r sluice_core
xmake -r sluice_async
xmake install -o /tmp/b01-evidence/pr-508/install-release-liburing-false sluice_core
xmake install -o /tmp/b01-evidence/pr-508/install-release-liburing-false sluice_async
```

Results:

| Artifact | Count / hash |
|----------|--------------|
| Installed headers | 71 |
| `libsluice_core.a` members | 12 |
| `libsluice_core.a` sha256 | `bb4d002938fce589fba1a4d51ec11f19b9932bb540123527d5d39b3d2ce80867` |
| `libsluice_async.a` members | 33 |
| `libsluice_async.a` sha256 | `ffbcf8d3f88dff0d7fae06103e0901faa9d04804a5fd8bf272052561c996cf45` |

### Release / liburing

Build commands:

```bash
xmake f -m release --liburing=true -y -c
xmake -r sluice_core
xmake -r sluice_async
xmake install -o /tmp/b01-evidence/pr-508/install-release-liburing-true sluice_core
xmake install -o /tmp/b01-evidence/pr-508/install-release-liburing-true sluice_async
```

Results:

| Artifact | Count / hash |
|----------|--------------|
| Installed headers | 71 |
| `libsluice_core.a` members | 12 |
| `libsluice_core.a` sha256 | `bb4d002938fce589fba1a4d51ec11f19b9932bb540123527d5d39b3d2ce80867` |
| `libsluice_async.a` members | 33 |
| `libsluice_async.a` sha256 | `634afb1bcdc4b7316562cd7b6190f66836262d0cb771da52ba69ed68cada6ffa` |

The `libsluice_core.a` archive is identical between the two profiles because it does not depend on `SLUICE_HAS_LIBURING`. The `libsluice_async.a` archive differs because the macro and liburing link are part of the async public usage requirement.

## Consumer closure

No in-tree production, application, test, or template consumer remains after this retirement.

## Compile/link probes

Negative probes (deleted interfaces no longer compile):

| Probe | Expected | Result |
|-------|----------|--------|
| `#include <sluice/blocking_io_pool.hpp>` | compile failure | `fatal error: sluice/blocking_io_pool.hpp: No such file or directory` |
| `#include <sluice/detail/blocking_io_pool_impl.hpp>` | compile failure | `fatal error: sluice/detail/blocking_io_pool_impl.hpp: No such file or directory` |

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
INSTALL=/tmp/b01-evidence/pr-508/install-release-liburing-false
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

This candidate deletes two public installed headers and one core archive member that were present in the v0.0.1 release. Per-interface risk adjudication is in `docs/evidence/f0-1-7-506-blocking-io-pool-risk.md`. Final F0 §1.7 owner approval is required before merge.
