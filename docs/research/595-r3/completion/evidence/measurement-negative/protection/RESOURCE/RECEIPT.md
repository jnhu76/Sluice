# RESOURCE — blocked B; A0 baseline, static audit and observability limits

| field | value |
|---|---|
| phase status | **NOT_ESTABLISHED** (B/A1 resource comparison not run) |
| raw | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/RESOURCE-STATIC-AUDIT/`, `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/RESOURCE-SYSCALL-A0-D16/`, per-block `*-time/` dirs |

## Peak RSS (`/usr/bin/time -v`, KiB)

| shape | max RSS |
|---|---|
| W02-sat-d16 | [5212] |
| W03-4k-d16-cpu2 | [5352] |
| W03-4k-d16-cpu8 | [5308] |
| W03-4k-d16-smt | [5324] |
| W03-4k-d16-xphys | [5212] |
| W03-64k-d16 | [5948, 5840, 5592] |
| W03-threadpool-4k-d16 | [5352] |

These are A0 baselines. The prereg +2% RSS gate is B vs A0 in the same case,
depth and fixture; B is blocked, so no RSS gate verdict exists.

## Threads (`/proc/<pid>/status Threads`, max sample)

| shape | max user threads |
|---|---|
| W02-sat-d16 | 2 |
| W03-4k-d16 | 3 |
| W03-64k-d16 | 3 |
| W03-threadpool-4k-d16 | 6 |

## Syscall diagnostic (A0 W03 d16, one measured rep)

| syscall | count |
|---|---|
| clone3 | 4 |
| epoll_create1 | 3 |
| eventfd2 | 4 |
| timer_create | 0 |

No `timer_create` and no new resident timer/queue; clone3/epoll/eventfd are the
pre-existing driver/epoll/eventfd registration mechanisms. B was not run, so a
B-vs-A0 "0 new" comparison is not available.

## Static audit (`/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/RESOURCE-STATIC-AUDIT/audit.txt`)

- Added lines in `src/` + `include/` matching `make_unique|make_shared|new|malloc|
  calloc|realloc|std::thread|timer_create|timerfd|epoll_create|eventfd(|queue<|
  std::deque`: **no matches**.
- Batch report storage is stack-only:
  `std::array<detail::RequestKey, 8>` (`include/sluice/async/detail/uring_backend.hpp:110`)
  and `std::array<Request<std::size_t>, 8>` in the test access shim
  (`include/sluice/async/detail/test_access.hpp:42`); dispatch uses the
  pre-existing `dispatch_` queue (`src/async/uring_backend.cpp:556,574`).
- Bench-only `std::vector` accepted lists are pre-reserved
  (`bench/bench_w03_external_loop.cpp:352-353`), so they do not add per-request
  allocation.
- New runtime state in the library is the lifetime witness
  `std::atomic<bool> have_ring_` (was `bool`), plus a test-only settlement pause
  gate; neither is a timer, queue or per-request allocation mechanism.

## Not observable / not run

- Allocation slope (`valgrind` 512 vs 1,024 reads): **NOT_RUN**. The frozen bench
  has no request-count knob; the two-point slope cannot be formed. Valgrind is
  available but the gate is blocked by B anyway.
- `wait4` RSS cross-check: **INVALID METHOD** here. Python `os.posix_spawn`
  uses `clone3(CLONE_VM|CLONE_VFORK)`, so the child's `ru_maxrss` includes the
  Python parent's pages (20,620 KiB) while `/usr/bin/time -v` reports 5,352 KiB.
  Control: `/bin/true` gives 9,860-10,116 KiB via posix_spawn vs 1,312-1,324 KiB
  via `time -v`. The `/usr/bin/time -v` values above are authoritative.
- `heaptrack` is unavailable (prereg tool limitation).

## Boundary

- Resource gate for B is `NOT_ESTABLISHED`, not PASS.
