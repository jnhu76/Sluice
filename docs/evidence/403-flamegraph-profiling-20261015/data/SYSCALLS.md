# Syscalls per operation (in-kernel counting, bpftrace)

Counts cover the whole process run (fixture setup included) and are
normalised to one pipeline execution (measured ops x 3 passes).

## w02-tp-4k-d16-w4 — 600,000 pipeline executions (measured ops x 3 passes)

### worker — 4,381,635 syscalls (7.30/op)

| syscall | count | per op |
|---|---|---|
| futex | 3,060,827 | 5.1014 |
| pread64 | 660,000 | 1.1000 |
| write | 660,000 | 1.1000 |
| rt_sigprocmask | 264 | 0.0004 |
| set_robust_list | 132 | 0.0002 |
| exit | 132 | 0.0002 |
| madvise | 132 | 0.0002 |
| rseq | 132 | 0.0002 |
| munmap | 8 | 0.0000 |
| mmap | 4 | 0.0000 |
| mprotect | 4 | 0.0000 |

### main — 1,416,691 syscalls (2.36/op)

| syscall | count | per op |
|---|---|---|
| futex | 1,415,750 | 2.3596 |
| rt_sigprocmask | 271 | 0.0005 |
| clone3 | 133 | 0.0002 |
| read | 115 | 0.0002 |
| close | 85 | 0.0001 |
| pwrite64 | 64 | 0.0001 |
| openat | 52 | 0.0001 |
| eventfd2 | 33 | 0.0001 |
| mmap | 31 | 0.0001 |
| poll | 24 | 0.0000 |
| lseek | 23 | 0.0000 |
| clock_gettime | 22 | 0.0000 |

**all threads: 5,798,326 syscalls = 9.66/op**

## w02-tp-4k-d16-w1 — 600,000 pipeline executions (measured ops x 3 passes)

### worker — 2,730,919 syscalls (4.55/op)

| syscall | count | per op |
|---|---|---|
| futex | 1,410,717 | 2.3512 |
| pread64 | 660,000 | 1.1000 |
| write | 660,000 | 1.1000 |
| rt_sigprocmask | 66 | 0.0001 |
| rseq | 33 | 0.0001 |
| set_robust_list | 33 | 0.0001 |
| exit | 33 | 0.0001 |
| madvise | 33 | 0.0001 |
| munmap | 2 | 0.0000 |
| mmap | 1 | 0.0000 |
| mprotect | 1 | 0.0000 |

### main — 2,072,618 syscalls (3.45/op)

| syscall | count | per op |
|---|---|---|
| futex | 1,750,446 | 2.9174 |
| read | 217,577 | 0.3626 |
| poll | 104,096 | 0.1735 |
| close | 85 | 0.0001 |
| rt_sigprocmask | 73 | 0.0001 |
| pwrite64 | 64 | 0.0001 |
| openat | 52 | 0.0001 |
| clone3 | 34 | 0.0001 |
| eventfd2 | 33 | 0.0001 |
| mmap | 28 | 0.0000 |
| lseek | 23 | 0.0000 |
| clock_gettime | 22 | 0.0000 |

**all threads: 4,803,537 syscalls = 8.01/op**

## w02-uring-4k-d16 — 600,000 pipeline executions (measured ops x 3 passes)

### main — 660,758 syscalls (1.10/op)

| syscall | count | per op |
|---|---|---|
| io_uring_enter | 660,034 | 1.1001 |
| close | 118 | 0.0002 |
| io_uring_register | 100 | 0.0002 |
| mmap | 93 | 0.0002 |
| munmap | 71 | 0.0001 |
| pwrite64 | 64 | 0.0001 |
| openat | 52 | 0.0001 |
| io_uring_setup | 34 | 0.0001 |
| eventfd2 | 33 | 0.0001 |
| read | 26 | 0.0000 |
| lseek | 23 | 0.0000 |
| clock_gettime | 22 | 0.0000 |

**all threads: 660,758 syscalls = 1.10/op**

## w03-tp-4k-d16-w4 — 600,000 pipeline executions (measured ops x 3 passes)

### worker — 5,441,186 syscalls (9.07/op)

| syscall | count | per op |
|---|---|---|
| futex | 3,881,647 | 6.4694 |
| write | 660,000 | 1.1000 |
| pread64 | 660,000 | 1.1000 |
| read | 158,978 | 0.2650 |
| epoll_wait | 79,489 | 0.1325 |
| rt_sigprocmask | 330 | 0.0006 |
| rseq | 165 | 0.0003 |
| madvise | 165 | 0.0003 |
| set_robust_list | 165 | 0.0003 |
| exit | 165 | 0.0003 |
| close | 33 | 0.0001 |
| epoll_ctl | 33 | 0.0001 |

### main — 1,561,033 syscalls (2.60/op)

| syscall | count | per op |
|---|---|---|
| futex | 1,559,885 | 2.5998 |
| rt_sigprocmask | 337 | 0.0006 |
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

**all threads: 7,002,219 syscalls = 11.67/op**

## w01-read-4k-seq — 3,000,000 pipeline executions (measured ops x 3 passes)

### main — 3,302,519 syscalls (1.10/op)

| syscall | count | per op |
|---|---|---|
| pread64 | 3,300,706 | 1.1002 |
| pwrite64 | 1,479 | 0.0005 |
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

**all threads: 3,302,519 syscalls = 1.10/op**
