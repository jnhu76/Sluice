# Directly observed synchronization and submission events (strace)

Events, not cycles. `strace` serialises the traced threads, so timing and
therefore contention differ from an uninstrumented run; the identities, the
operation mix and the submission batch sizes are what these traces establish.
Unperturbed per-operation counts are in `SYSCALLS.md`; cycle shares and their
call-path attribution are in `ATTRIBUTION.md`.

## w02-tp-4k-d16-w4

futex calls observed: 221,398

| operation | calls | share |
|---|---|---|
| FUTEX_WAKE_PRIVATE | 105,482 | 47.6% |
| FUTEX_WAIT_PRIVATE | 103,796 | 46.9% |
| FUTEX_WAIT_BITSET_PRIVATE | 12,117 | 5.5% |
| FUTEX_WAIT_BITSET | 3 | 0.0% |

distinct (operation, address) pairs: 21

busiest addresses (each address is one futex word, i.e. one lock or one condition-variable waiter word):

| operation | address | calls |
|---|---|---|
| FUTEX_WAIT_PRIVATE | `0x3c636660` | 63,542 |
| FUTEX_WAKE_PRIVATE | `0x3c636660` | 43,545 |
| FUTEX_WAIT_PRIVATE | `0x3c6366e0` | 31,730 |
| FUTEX_WAKE_PRIVATE | `0x3c6366e0` | 21,435 |
| FUTEX_WAKE_PRIVATE | `0x3c635cd8` | 20,638 |
| FUTEX_WAKE_PRIVATE | `0x3c635d24` | 6,310 |
| FUTEX_WAKE_PRIVATE | `0x3c635d20` | 6,247 |
| FUTEX_WAIT_BITSET_PRIVATE | `0x3c635d24` | 6,107 |
| FUTEX_WAIT_BITSET_PRIVATE | `0x3c635d20` | 6,010 |
| FUTEX_WAIT_PRIVATE | `0x3c635cd8` | 5,592 |

io_uring_enter calls observed: 1

| SQEs submitted per enter (`to_submit`) | calls | share |
|---|---|---|
| 1 | 1 | 100.0% |

## w02-uring-4k-d16

futex calls observed: 2

| operation | calls | share |
|---|---|---|
| FUTEX_WAKE_PRIVATE | 2 | 100.0% |

distinct (operation, address) pairs: 2

busiest addresses (each address is one futex word, i.e. one lock or one condition-variable waiter word):

| operation | address | calls |
|---|---|---|
| FUTEX_WAKE_PRIVATE | `0x7fb6272ae6bc` | 1 |
| FUTEX_WAKE_PRIVATE | `0x7ff8837bc6ec` | 1 |

io_uring_enter calls observed: 60,004

| SQEs submitted per enter (`to_submit`) | calls | share |
|---|---|---|
| 1 | 60,004 | 100.0% |
