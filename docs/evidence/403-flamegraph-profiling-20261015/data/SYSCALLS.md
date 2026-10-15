# Syscalls per operation (in-kernel counting, bpftrace)

Method:

* the harness runs every recorded repetition three times (timed throughput
  pass, correctness-oracle pass, instrumented latency pass), so a counted run
  executes `(warmup + measured) reps x 3 passes x ops` operations;
* the denominator is that full execution count, read from the run's own JSONL
  records rather than assumed from the requested repetition count;
* `init cost` is the fixed startup, fixture and prepare cost measured by
  `tools/measure_init_cost.sh`, which runs the setup path with no pipeline
  pass; it is subtracted in the all-thread line, while the per-role tables
  divide raw counts by the execution count and still contain their share;
* counts come from the bpftrace probe in `tools/count_syscalls.sh`, which
  filters on the benchmark comms, so helper processes are excluded;
* `capture verification` is the record written by `count_syscalls.sh`; where
  the capture predates that script the record is reconstructed from the
  committed artifacts and the bench exit code is marked as not recorded.

Version note: this is the corrected table. `SYSCALLS.superseded-v1.md` keeps
the first version, which divided by `measured ops x 3` only -- excluding the
warmup repetition -- and so reported every per-operation figure about 10% high.

## w02-tp-4k-d16-w4 — 660,000 pipeline executions

1 warmup + 10 measured recorded reps x 3 passes x 20,000 ops = 660,000 executions

capture verification (reconstructed): `OK` — case ran `W02-threadpool-4k-d16-w4`, 1 probe(s) attached, 5,798,326 events, bench rc NOT_RECORDED_IN_ORIGINAL_RUN

init cost (measured, setup path only): 382 syscalls

### worker — 4,381,635 syscalls (6.64/op)

| syscall | count | per op |
|---|---|---|
| futex | 3,060,827 | 4.6376 |
| pread64 | 660,000 | 1.0000 |
| write | 660,000 | 1.0000 |
| rt_sigprocmask | 264 | 0.0004 |
| set_robust_list | 132 | 0.0002 |
| exit | 132 | 0.0002 |
| madvise | 132 | 0.0002 |
| rseq | 132 | 0.0002 |
| munmap | 8 | 0.0000 |
| mmap | 4 | 0.0000 |
| mprotect | 4 | 0.0000 |

### main — 1,416,691 syscalls (2.15/op)

| syscall | count | per op |
|---|---|---|
| futex | 1,415,750 | 2.1451 |
| rt_sigprocmask | 271 | 0.0004 |
| clone3 | 133 | 0.0002 |
| read | 115 | 0.0002 |
| close | 85 | 0.0001 |
| pwrite64 | 64 | 0.0001 |
| openat | 52 | 0.0001 |
| eventfd2 | 33 | 0.0001 |
| mmap | 31 | 0.0000 |
| poll | 24 | 0.0000 |
| lseek | 23 | 0.0000 |
| clock_gettime | 22 | 0.0000 |

**all threads: 5,798,326 counted = 8.785/op; 5,797,944 after the measured setup path = 8.785/op**

consistency checks (residual = setup path and per-repetition prepare; a wrong denominator shows up as a residual of ~10% of 660,000):

* `pread64`: 660,002 counted = 1 x 660,000 + 2 residual (OK)
* `write`: 660,013 counted = 1 x 660,000 + 13 residual (OK)

## w02-tp-4k-d16-w1 — 660,000 pipeline executions

1 warmup + 10 measured recorded reps x 3 passes x 20,000 ops = 660,000 executions

capture verification (reconstructed): `OK` — case ran `W02-threadpool-4k-d16-w1`, 1 probe(s) attached, 4,803,537 events, bench rc NOT_RECORDED_IN_ORIGINAL_RUN

init cost (measured, setup path only): 382 syscalls

### worker — 2,730,919 syscalls (4.14/op)

| syscall | count | per op |
|---|---|---|
| futex | 1,410,717 | 2.1374 |
| pread64 | 660,000 | 1.0000 |
| write | 660,000 | 1.0000 |
| rt_sigprocmask | 66 | 0.0001 |
| rseq | 33 | 0.0001 |
| set_robust_list | 33 | 0.0001 |
| exit | 33 | 0.0001 |
| madvise | 33 | 0.0001 |
| munmap | 2 | 0.0000 |
| mmap | 1 | 0.0000 |
| mprotect | 1 | 0.0000 |

### main — 2,072,618 syscalls (3.14/op)

| syscall | count | per op |
|---|---|---|
| futex | 1,750,446 | 2.6522 |
| read | 217,577 | 0.3297 |
| poll | 104,096 | 0.1577 |
| close | 85 | 0.0001 |
| rt_sigprocmask | 73 | 0.0001 |
| pwrite64 | 64 | 0.0001 |
| openat | 52 | 0.0001 |
| clone3 | 34 | 0.0001 |
| eventfd2 | 33 | 0.0001 |
| mmap | 28 | 0.0000 |
| lseek | 23 | 0.0000 |
| clock_gettime | 22 | 0.0000 |

**all threads: 4,803,537 counted = 7.278/op; 4,803,155 after the measured setup path = 7.278/op**

consistency checks (residual = setup path and per-repetition prepare; a wrong denominator shows up as a residual of ~10% of 660,000):

* `pread64`: 660,002 counted = 1 x 660,000 + 2 residual (OK)
* `write`: 660,013 counted = 1 x 660,000 + 13 residual (OK)

## w02-uring-4k-d16 — 660,000 pipeline executions

1 warmup + 10 measured recorded reps x 3 passes x 20,000 ops = 660,000 executions

capture verification (reconstructed): `OK` — case ran `W02-uring-4k-d16`, 1 probe(s) attached, 660,758 events, bench rc NOT_RECORDED_IN_ORIGINAL_RUN

init cost (measured, setup path only): 382 syscalls

### main — 660,758 syscalls (1.00/op)

| syscall | count | per op |
|---|---|---|
| io_uring_enter | 660,034 | 1.0001 |
| close | 118 | 0.0002 |
| io_uring_register | 100 | 0.0002 |
| mmap | 93 | 0.0001 |
| munmap | 71 | 0.0001 |
| pwrite64 | 64 | 0.0001 |
| openat | 52 | 0.0001 |
| io_uring_setup | 34 | 0.0001 |
| eventfd2 | 33 | 0.0001 |
| read | 26 | 0.0000 |
| lseek | 23 | 0.0000 |
| clock_gettime | 22 | 0.0000 |

**all threads: 660,758 counted = 1.001/op; 660,376 after the measured setup path = 1.001/op**

## w03-tp-4k-d16-w4 — 660,000 pipeline executions

1 warmup + 10 measured recorded reps x 3 passes x 20,000 ops = 660,000 executions

capture verification (reconstructed): `OK` — case ran `W03-threadpool-4k-d16-w4`, 1 probe(s) attached, 7,002,219 events, bench rc NOT_RECORDED_IN_ORIGINAL_RUN

init cost (measured, setup path only): 382 syscalls

### worker — 5,441,186 syscalls (8.24/op)

| syscall | count | per op |
|---|---|---|
| futex | 3,881,647 | 5.8813 |
| write | 660,000 | 1.0000 |
| pread64 | 660,000 | 1.0000 |
| read | 158,978 | 0.2409 |
| epoll_wait | 79,489 | 0.1204 |
| rt_sigprocmask | 330 | 0.0005 |
| rseq | 165 | 0.0003 |
| madvise | 165 | 0.0003 |
| set_robust_list | 165 | 0.0003 |
| exit | 165 | 0.0003 |
| close | 33 | 0.0001 |
| epoll_ctl | 33 | 0.0001 |

### main — 1,561,033 syscalls (2.37/op)

| syscall | count | per op |
|---|---|---|
| futex | 1,559,885 | 2.3635 |
| rt_sigprocmask | 337 | 0.0005 |
| clone3 | 166 | 0.0003 |
| close | 85 | 0.0001 |
| write | 79 | 0.0001 |
| pwrite64 | 64 | 0.0001 |
| mmap | 64 | 0.0001 |
| openat | 52 | 0.0001 |
| munmap | 38 | 0.0001 |
| madvise | 37 | 0.0001 |
| epoll_create1 | 33 | 0.0001 |
| epoll_ctl | 33 | 0.0001 |

**all threads: 7,002,219 counted = 10.609/op; 7,001,837 after the measured setup path = 10.609/op**

consistency checks (residual = setup path and per-repetition prepare; a wrong denominator shows up as a residual of ~10% of 660,000):

* `pread64`: 660,002 counted = 1 x 660,000 + 2 residual (OK)
* `write`: 660,079 counted = 1 x 660,000 + 79 residual (OK)

## w01-read-4k-seq — 3,300,000 pipeline executions

1 warmup + 10 measured recorded reps x 3 passes x 100,000 ops = 3,300,000 executions

capture verification (reconstructed): `OK` — case ran `W01-read_at-4k-seq`, 1 probe(s) attached, 3,302,519 events, bench rc NOT_RECORDED_IN_ORIGINAL_RUN

init cost (measured, setup path only): 2,048 syscalls

### main — 3,302,519 syscalls (1.00/op)

| syscall | count | per op |
|---|---|---|
| pread64 | 3,300,706 | 1.0002 |
| pwrite64 | 1,479 | 0.0004 |
| openat | 72 | 0.0000 |
| close | 71 | 0.0000 |
| mmap | 23 | 0.0000 |
| lseek | 23 | 0.0000 |
| clock_gettime | 22 | 0.0000 |
| fstat | 18 | 0.0000 |
| read | 15 | 0.0000 |
| write | 13 | 0.0000 |
| rt_sigaction | 12 | 0.0000 |
| rt_sigprocmask | 12 | 0.0000 |

**all threads: 3,302,519 counted = 1.001/op; 3,300,471 after the measured setup path = 1.000/op**

consistency checks (residual = setup path and per-repetition prepare; a wrong denominator shows up as a residual of ~10% of 3,300,000):

* `pread64`: 3,300,706 counted = 1 x 3,300,000 + 706 residual (OK)
