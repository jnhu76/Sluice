# B-01 Evidence: #506 BlockingIoPool Physical Retirement

Base: ecb2481c700e3c6710885ec3af9e903c941ab180 (origin/master)
Head: $(git rev-parse HEAD)
Host: E5 / Fedora 44 / x86_64
Toolchain: GCC 16.2.1 / xmake v3.0.6+20260117
Profile: release / no-liburing

## Removed product files

- include/sluice/blocking_io_pool.hpp
- include/sluice/detail/blocking_io_pool_impl.hpp
- src/blocking_io_pool.cpp

## Package delta

| Metric | Base | Head | Delta |
|--------|------|------|-------|
| Installed headers | 73 | 71 | -2 |
| libsluice_core.a members | 15 | 14 | -1 |
| libsluice_async.a members | 33 | 33 | 0 |

### Removed installed headers

- sluice/blocking_io_pool.hpp
- sluice/detail/blocking_io_pool_impl.hpp

### Removed archive member

- blocking_io_pool.cpp.o

## Consumer closure

No in-tree production, application, test, or template consumer remains.
External/public-API breakage requires F0 §1.7 owner approval before merge.

## Build/test matrix

- [x] Debug / no-liburing: PASS (53 tests)
- [x] Release / no-liburing: PASS (53 tests)
- [x] Debug / liburing: PASS (67 tests)
- [x] Release / liburing: PASS (67 tests)
- [ ] ASan+UBSan: UNAVAILABLE (libasan runtime missing on host)
- [ ] TSan: UNAVAILABLE (libtsan runtime missing on host)
