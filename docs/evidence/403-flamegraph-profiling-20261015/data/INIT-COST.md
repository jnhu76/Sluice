# Fixed per-process cost (setup path only, strace)

Each run executes the harness's startup and fixture creation with no
pipeline pass (`G0_WARMUP=0 G0_MEASURED=0`), so these syscalls do not scale
with the operation count. `SYSCALLS.md` subtracts the total in its
all-thread line. The counts include the harness's `mkdir -p` helper
processes, which the bpftrace census excludes, so this is a slight
over-estimate of the benchmark's own setup cost.

## w01-read-4k-seq — 2,048 syscalls

verification: `OK` (bench rc 0, repetition lines 0)

| syscall | calls |
|---|---|
| pwrite64 | 1,479 |
| mmap | 85 |
| rt_sigaction | 46 |
| close | 44 |
| fstat | 30 |
| mprotect | 26 |
| read | 20 |
| rt_sigprocmask | 20 |
| brk | 16 |
| fdatasync | 11 |

## w02-tp-4k-d16-w1 — 382 syscalls

verification: `OK` (bench rc 0, repetition lines 0)

| syscall | calls |
|---|---|
| pwrite64 | 64 |
| mmap | 58 |
| rt_sigaction | 23 |
| close | 22 |
| fstat | 19 |
| mprotect | 17 |
| read | 13 |
| brk | 10 |
| rt_sigprocmask | 10 |
| munmap | 7 |

## w02-tp-4k-d16-w4 — 382 syscalls

verification: `OK` (bench rc 0, repetition lines 0)

| syscall | calls |
|---|---|
| pwrite64 | 64 |
| mmap | 58 |
| rt_sigaction | 23 |
| close | 22 |
| fstat | 19 |
| mprotect | 17 |
| read | 13 |
| brk | 10 |
| rt_sigprocmask | 10 |
| munmap | 7 |

## w02-uring-4k-d16 — 382 syscalls

verification: `OK` (bench rc 0, repetition lines 0)

| syscall | calls |
|---|---|
| pwrite64 | 64 |
| mmap | 58 |
| rt_sigaction | 23 |
| close | 22 |
| fstat | 19 |
| mprotect | 17 |
| read | 13 |
| brk | 10 |
| rt_sigprocmask | 10 |
| munmap | 7 |

## w03-tp-4k-d16-w4 — 382 syscalls

verification: `OK` (bench rc 0, repetition lines 0)

| syscall | calls |
|---|---|
| pwrite64 | 64 |
| mmap | 58 |
| rt_sigaction | 23 |
| close | 22 |
| fstat | 19 |
| mprotect | 17 |
| read | 13 |
| brk | 10 |
| rt_sigprocmask | 10 |
| munmap | 7 |
