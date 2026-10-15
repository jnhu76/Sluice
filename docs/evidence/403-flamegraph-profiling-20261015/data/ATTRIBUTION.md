# Where the time goes (cycle-weighted self cost)

Kernel cost is charged to the operation that entered the kernel.
Shares are of all cycles captured for the case, across all threads.

## w01-read-4k-seq  (total 24.7 G cycles)

| bucket | share |
|---|---|
| kernel | 86.5% |
| libc mem* | 4.2% |
| other | 3.6% |
| sluice | 2.7% |
| bench harness | 1.6% |
| c++ runtime | 0.9% |
| libc wrapper self | 0.5% |

| bucket | detail | share |
|---|---|---|
| kernel | pread | 79.99% |
| kernel | pwrite | 5.27% |
| libc mem* | __memcmp_avx2_movbe | 3.04% |
| other | __internal_syscall_cancel | 2.17% |
| libc mem* | __memmove_avx_unaligned_erms | 1.16% |
| bench harness | operator | 0.75% |
| kernel | clock_gettime | 0.65% |
| c++ runtime | __gnu_cxx::__normal_iterator | 0.63% |
| sluice | sluice::detail::rejection_error | 0.56% |
| libc wrapper self | __libc_pread | 0.49% |
| other | __syscall_cancel | 0.49% |
| bench harness | precheck | 0.42% |
| sluice | sluice::blocking::read_at | 0.40% |
| kernel | syscall machinery via fdatasync | 0.37% |
| sluice | sluice::blocking::read_exact_at | 0.35% |
| other | decltype | 0.34% |

## w01-sync-data-4k  (total 2.1 G cycles)

| bucket | share |
|---|---|
| kernel | 86.2% |
| libc mem* | 13.1% |
| other | 0.5% |
| sluice | 0.1% |
| bench harness | 0.1% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | pwrite | 63.80% |
| kernel | syscall machinery via __GI___libc_open | 16.31% |
| libc mem* | __memmove_avx_unaligned_erms | 13.13% |
| kernel | syscall machinery via fdatasync | 4.91% |
| kernel | syscall machinery via __memset_avx2_unaligned_erms | 0.30% |
| kernel | syscall machinery via [unknown] | 0.16% |
| other | __internal_syscall_cancel | 0.13% |
| kernel | syscall machinery via __GI___execve | 0.08% |
| kernel | syscall machinery via __mmap64 | 0.08% |
| other | __memset_avx2_unaligned_erms | 0.07% |
| kernel | syscall machinery via __memmove_avx_unaligned_erms | 0.06% |
| kernel | syscall machinery via __GI___libc_write | 0.05% |
| kernel | syscall machinery via __open64_nocancel | 0.05% |
| other | _dl_new_hash | 0.05% |
| kernel | clone (thread create) | 0.04% |
| other | do_lookup_x | 0.04% |

## w01-write-4k-seq  (total 33.8 G cycles)

| bucket | share |
|---|---|
| kernel | 87.2% |
| libc mem* | 4.4% |
| other | 3.2% |
| sluice | 2.7% |
| bench harness | 1.3% |
| c++ runtime | 0.7% |
| libc wrapper self | 0.4% |

| bucket | detail | share |
|---|---|---|
| kernel | pwrite | 66.71% |
| kernel | pread | 18.87% |
| libc mem* | __memmove_avx_unaligned_erms | 2.28% |
| libc mem* | __memcmp_avx2_movbe | 2.11% |
| other | __internal_syscall_cancel | 2.04% |
| kernel | syscall machinery via __GI___libc_open | 0.75% |
| bench harness | operator | 0.58% |
| sluice | sluice::detail::rejection_error | 0.51% |
| other | __syscall_cancel | 0.46% |
| kernel | clock_gettime | 0.44% |
| c++ runtime | __gnu_cxx::__normal_iterator | 0.40% |
| bench harness | precheck | 0.39% |
| sluice | compose | 0.37% |
| sluice | sluice::blocking::write_at | 0.37% |
| sluice | sluice::detail::result_storage | 0.35% |
| libc wrapper self | __libc_pwrite | 0.31% |

## w02-tp-4k-d16-w1  (total 27.1 G cycles)

| bucket | share |
|---|---|
| kernel | 59.1% |
| libc pthread | 23.7% |
| sluice | 9.2% |
| other | 4.7% |
| libc mem* | 1.9% |
| c++ runtime | 0.8% |
| bench harness | 0.5% |
| libc wrapper self | 0.1% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 20.46% |
| kernel | pread | 15.17% |
| kernel | futex_wait (mutex) | 13.68% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 11.01% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 10.69% |
| kernel | clone (thread create) | 6.03% |
| other | __GI___lll_lock_wait | 2.01% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.87% |
| libc mem* | __memcmp_avx2_movbe | 1.85% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 1.04% |
| kernel | syscall machinery via __poll | 1.00% |
| other | __GI___lll_lock_wake | 0.73% |
| libc pthread | __gthread_mutex_lock | 0.65% |
| other | __syscall_cancel_arch_end | 0.57% |
| kernel | mutex_unlock (userspace) | 0.54% |
| kernel | clock_gettime | 0.54% |

## w02-tp-4k-d16-w4  (total 74.4 G cycles)

| bucket | share |
|---|---|
| kernel | 72.1% |
| libc pthread | 14.2% |
| sluice | 6.5% |
| other | 5.0% |
| libc mem* | 1.1% |
| c++ runtime | 0.6% |
| bench harness | 0.4% |
| libc wrapper self | 0.1% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 21.52% |
| kernel | futex_wait (mutex) | 19.05% |
| kernel | pread | 11.79% |
| kernel | futex_wait (timed) | 7.97% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 6.17% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 5.77% |
| kernel | clone (thread create) | 5.42% |
| kernel | futex_wake (condvar) | 4.78% |
| other | __GI___lll_lock_wait | 2.05% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.13% |
| libc mem* | __memcmp_avx2_movbe | 1.02% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.81% |
| libc pthread | pthread_cond_wait@@GLIBC_2.3.2 | 0.79% |
| other | __GI___lll_lock_wake | 0.67% |
| kernel | mutex_unlock (userspace) | 0.60% |
| other | __internal_syscall_cancel | 0.47% |

## w02-tp-4k-d16-w8  (total 51.1 G cycles)

| bucket | share |
|---|---|
| kernel | 76.5% |
| libc pthread | 11.6% |
| sluice | 5.1% |
| other | 5.0% |
| libc mem* | 0.9% |
| c++ runtime | 0.4% |
| bench harness | 0.3% |
| libc wrapper self | 0.1% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 21.54% |
| kernel | futex_wait (mutex) | 19.03% |
| kernel | futex_wait (timed) | 13.02% |
| kernel | pread | 9.83% |
| kernel | futex_wake (condvar) | 7.01% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 4.79% |
| kernel | clone (thread create) | 4.48% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 4.29% |
| other | __GI___lll_lock_wait | 1.88% |
| libc pthread | pthread_cond_wait@@GLIBC_2.3.2 | 1.20% |
| sluice | sluice::async::detail::RequestCore::occupancy | 0.92% |
| libc mem* | __memcmp_avx2_movbe | 0.83% |
| other | __GI___lll_lock_wake | 0.62% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.60% |
| kernel | mutex_unlock (userspace) | 0.59% |
| other | __internal_syscall_cancel | 0.57% |

## w02-tp-64k-d16-w4  (total 7.7 G cycles)

| bucket | share |
|---|---|
| kernel | 80.4% |
| libc pthread | 6.9% |
| libc mem* | 6.7% |
| sluice | 3.2% |
| other | 2.1% |
| c++ runtime | 0.3% |
| bench harness | 0.2% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | pread | 54.11% |
| kernel | futex_wake (mutex) | 7.85% |
| kernel | futex_wait (mutex) | 6.66% |
| libc mem* | __memcmp_avx2_movbe | 6.20% |
| kernel | clone (thread create) | 4.19% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 3.15% |
| kernel | futex_wait (timed) | 2.64% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 2.62% |
| kernel | futex_wake (condvar) | 1.57% |
| kernel | pwrite | 0.81% |
| kernel | syscall machinery via __poll | 0.79% |
| other | __GI___lll_lock_wait | 0.71% |
| sluice | sluice::async::detail::RequestCore::occupancy | 0.62% |
| kernel | syscall machinery via read | 0.57% |
| libc mem* | __memmove_avx_unaligned_erms | 0.46% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.43% |

## w02-uring-4k-d16  (total 37.3 G cycles)

| bucket | share |
|---|---|
| kernel | 45.1% |
| kernel (io-wq worker) | 34.6% |
| sluice | 10.6% |
| libc pthread | 3.9% |
| libc mem* | 2.4% |
| other | 2.1% |
| c++ runtime | 0.8% |
| bench harness | 0.6% |

| bucket | detail | share |
|---|---|---|
| kernel | io_uring_enter | 36.14% |
| kernel (io-wq worker) | rep_movs_alternative | 6.08% |
| kernel (io-wq worker) | _raw_spin_lock | 2.89% |
| libc mem* | __memcmp_avx2_movbe | 2.34% |
| kernel (io-wq worker) | shmem_file_read_iter | 2.18% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.82% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 1.76% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 1.45% |
| kernel (io-wq worker) | io_cq_unlock_post | 1.21% |
| kernel (io-wq worker) | native_queued_spin_lock_slowpath | 0.97% |
| kernel (io-wq worker) | __raw_spin_lock_irqsave | 0.91% |
| kernel | syscall machinery via __memcmp_avx2_movbe | 0.90% |
| kernel | clock_gettime | 0.76% |
| kernel (io-wq worker) | filemap_get_entry | 0.76% |
| kernel (io-wq worker) | io_worker_handle_work | 0.73% |
| sluice | sluice::async::UringAsyncBackend::service_pending_controls_locked_ | 0.73% |

## w02-uring-4k-d64  (total 20.6 G cycles)

| bucket | share |
|---|---|
| kernel | 44.5% |
| kernel (io-wq worker) | 30.6% |
| sluice | 16.0% |
| libc pthread | 3.3% |
| libc mem* | 2.2% |
| other | 1.9% |
| c++ runtime | 0.8% |
| bench harness | 0.6% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | io_uring_enter | 36.41% |
| kernel (io-wq worker) | rep_movs_alternative | 4.91% |
| sluice | sluice::async::detail::RequestCore::occupancy | 4.22% |
| kernel (io-wq worker) | _raw_spin_lock | 2.44% |
| libc mem* | __memcmp_avx2_movbe | 2.08% |
| kernel (io-wq worker) | shmem_file_read_iter | 1.94% |
| sluice | sluice::async::UringAsyncBackend::service_pending_controls_locked_ | 1.69% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 1.64% |
| sluice | std::vector | 1.26% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 1.11% |
| sluice | sluice::async::RequestScope::Slot* sluice::async::RequestScope::resolve_ | 1.09% |
| kernel (io-wq worker) | __raw_spin_lock_irqsave | 0.94% |
| kernel (io-wq worker) | io_cq_unlock_post | 0.94% |
| kernel | syscall machinery via sluice::async::detail::RequestCore::occupancy | 0.82% |
| kernel | syscall machinery via __memcmp_avx2_movbe | 0.70% |
| kernel (io-wq worker) | filemap_get_entry | 0.69% |

## w03-tp-4k-d16-w4  (total 52.2 G cycles)

| bucket | share |
|---|---|
| kernel | 74.4% |
| libc pthread | 14.7% |
| sluice | 4.7% |
| other | 4.5% |
| libc mem* | 0.9% |
| c++ runtime | 0.3% |
| bench harness | 0.2% |
| libc wrapper self | 0.1% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 20.97% |
| kernel | futex_wait (mutex) | 19.72% |
| kernel | pread | 10.74% |
| kernel | futex_wait (timed) | 8.81% |
| kernel | clone (thread create) | 6.76% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 6.73% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 5.53% |
| kernel | futex_wake (condvar) | 4.80% |
| other | __GI___lll_lock_wait | 1.91% |
| kernel | epoll_wait | 1.32% |
| libc pthread | pthread_cond_wait@@GLIBC_2.3.2 | 0.98% |
| libc mem* | __memcmp_avx2_movbe | 0.92% |
| sluice | sluice::async::detail::RequestCore::occupancy | 0.85% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.74% |
| other | __GI___lll_lock_wake | 0.55% |
| kernel | mutex_unlock (userspace) | 0.54% |

## w03-uring-4k-d16  (total 26.7 G cycles)

| bucket | share |
|---|---|
| kernel | 52.1% |
| kernel (io-wq worker) | 24.9% |
| sluice | 8.7% |
| libc pthread | 8.7% |
| other | 2.6% |
| libc mem* | 2.1% |
| c++ runtime | 0.6% |
| bench harness | 0.4% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | io_uring_enter | 21.12% |
| kernel | futex_wake (mutex) | 9.05% |
| kernel | futex_wait (mutex) | 7.78% |
| kernel | futex_wait (timed) | 5.13% |
| kernel (io-wq worker) | rep_movs_alternative | 4.31% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 3.94% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 3.63% |
| kernel | clone (thread create) | 2.42% |
| kernel | epoll_wait | 2.14% |
| libc mem* | __memcmp_avx2_movbe | 2.00% |
| kernel (io-wq worker) | _raw_spin_lock | 1.91% |
| kernel (io-wq worker) | shmem_file_read_iter | 1.55% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.27% |
| kernel (io-wq worker) | __raw_spin_lock_irqsave | 1.06% |
| kernel (io-wq worker) | native_queued_spin_lock_slowpath | 1.06% |
| kernel (io-wq worker) | io_cq_unlock_post | 0.84% |

# Per-thread-role split

## w01-read-4k-seq  (total 24.7 G cycles)

| bucket | share |
|---|---|
| kernel | 86.5% |
| libc mem* | 4.2% |
| other | 3.6% |
| sluice | 2.7% |
| bench harness | 1.6% |
| c++ runtime | 0.9% |
| libc wrapper self | 0.5% |

| bucket | detail | share |
|---|---|---|
| kernel | pread | 79.99% |
| kernel | pwrite | 5.27% |
| libc mem* | __memcmp_avx2_movbe | 3.04% |
| other | __internal_syscall_cancel | 2.17% |
| libc mem* | __memmove_avx_unaligned_erms | 1.16% |
| bench harness | operator | 0.75% |
| kernel | clock_gettime | 0.65% |
| c++ runtime | __gnu_cxx::__normal_iterator | 0.63% |
| sluice | sluice::detail::rejection_error | 0.56% |
| libc wrapper self | __libc_pread | 0.49% |
| other | __syscall_cancel | 0.49% |
| bench harness | precheck | 0.42% |
| sluice | sluice::blocking::read_at | 0.40% |
| kernel | syscall machinery via fdatasync | 0.37% |
| sluice | sluice::blocking::read_exact_at | 0.35% |
| other | decltype | 0.34% |

- **main (submit + consume)** — 100.0% of all cycles: kernel 86%, libc mem* 4%, other 4%, sluice 3%, bench harness 2%

## w01-sync-data-4k  (total 2.1 G cycles)

| bucket | share |
|---|---|
| kernel | 86.2% |
| libc mem* | 13.1% |
| other | 0.5% |
| sluice | 0.1% |
| bench harness | 0.1% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | pwrite | 63.80% |
| kernel | syscall machinery via __GI___libc_open | 16.31% |
| libc mem* | __memmove_avx_unaligned_erms | 13.13% |
| kernel | syscall machinery via fdatasync | 4.91% |
| kernel | syscall machinery via __memset_avx2_unaligned_erms | 0.30% |
| kernel | syscall machinery via [unknown] | 0.16% |
| other | __internal_syscall_cancel | 0.13% |
| kernel | syscall machinery via __GI___execve | 0.08% |
| kernel | syscall machinery via __mmap64 | 0.08% |
| other | __memset_avx2_unaligned_erms | 0.07% |
| kernel | syscall machinery via __memmove_avx_unaligned_erms | 0.06% |
| kernel | syscall machinery via __GI___libc_write | 0.05% |
| kernel | syscall machinery via __open64_nocancel | 0.05% |
| other | _dl_new_hash | 0.05% |
| kernel | clone (thread create) | 0.04% |
| other | do_lookup_x | 0.04% |

- **main (submit + consume)** — 100.0% of all cycles: kernel 86%, libc mem* 13%, other 0%, sluice 0%, bench harness 0%

## w01-write-4k-seq  (total 33.8 G cycles)

| bucket | share |
|---|---|
| kernel | 87.2% |
| libc mem* | 4.4% |
| other | 3.2% |
| sluice | 2.7% |
| bench harness | 1.3% |
| c++ runtime | 0.7% |
| libc wrapper self | 0.4% |

| bucket | detail | share |
|---|---|---|
| kernel | pwrite | 66.71% |
| kernel | pread | 18.87% |
| libc mem* | __memmove_avx_unaligned_erms | 2.28% |
| libc mem* | __memcmp_avx2_movbe | 2.11% |
| other | __internal_syscall_cancel | 2.04% |
| kernel | syscall machinery via __GI___libc_open | 0.75% |
| bench harness | operator | 0.58% |
| sluice | sluice::detail::rejection_error | 0.51% |
| other | __syscall_cancel | 0.46% |
| kernel | clock_gettime | 0.44% |
| c++ runtime | __gnu_cxx::__normal_iterator | 0.40% |
| bench harness | precheck | 0.39% |
| sluice | compose | 0.37% |
| sluice | sluice::blocking::write_at | 0.37% |
| sluice | sluice::detail::result_storage | 0.35% |
| libc wrapper self | __libc_pwrite | 0.31% |

- **main (submit + consume)** — 100.0% of all cycles: kernel 87%, libc mem* 4%, other 3%, sluice 3%, bench harness 1%

## w02-tp-4k-d16-w1  (total 27.1 G cycles)

| bucket | share |
|---|---|
| kernel | 59.1% |
| libc pthread | 23.7% |
| sluice | 9.2% |
| other | 4.7% |
| libc mem* | 1.9% |
| c++ runtime | 0.8% |
| bench harness | 0.5% |
| libc wrapper self | 0.1% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 20.46% |
| kernel | pread | 15.17% |
| kernel | futex_wait (mutex) | 13.68% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 11.01% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 10.69% |
| kernel | clone (thread create) | 6.03% |
| other | __GI___lll_lock_wait | 2.01% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.87% |
| libc mem* | __memcmp_avx2_movbe | 1.85% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 1.04% |
| kernel | syscall machinery via __poll | 1.00% |
| other | __GI___lll_lock_wake | 0.73% |
| libc pthread | __gthread_mutex_lock | 0.65% |
| other | __syscall_cancel_arch_end | 0.57% |
| kernel | mutex_unlock (userspace) | 0.54% |
| kernel | clock_gettime | 0.54% |

- **main (submit + consume)** — 50.7% of all cycles: kernel 45%, libc pthread 31%, sluice 14%, other 4%, libc mem* 4%
- **worker thread** — 49.3% of all cycles: kernel 74%, libc pthread 17%, other 5%, sluice 4%, libc wrapper self 0%

## w02-tp-4k-d16-w4  (total 74.4 G cycles)

| bucket | share |
|---|---|
| kernel | 72.1% |
| libc pthread | 14.2% |
| sluice | 6.5% |
| other | 5.0% |
| libc mem* | 1.1% |
| c++ runtime | 0.6% |
| bench harness | 0.4% |
| libc wrapper self | 0.1% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 21.52% |
| kernel | futex_wait (mutex) | 19.05% |
| kernel | pread | 11.79% |
| kernel | futex_wait (timed) | 7.97% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 6.17% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 5.77% |
| kernel | clone (thread create) | 5.42% |
| kernel | futex_wake (condvar) | 4.78% |
| other | __GI___lll_lock_wait | 2.05% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.13% |
| libc mem* | __memcmp_avx2_movbe | 1.02% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.81% |
| libc pthread | pthread_cond_wait@@GLIBC_2.3.2 | 0.79% |
| other | __GI___lll_lock_wake | 0.67% |
| kernel | mutex_unlock (userspace) | 0.60% |
| other | __internal_syscall_cancel | 0.47% |

- **worker thread** — 67.6% of all cycles: kernel 81%, libc pthread 11%, other 5%, sluice 3%, bench harness 0%
- **main (submit + consume)** — 32.4% of all cycles: kernel 54%, libc pthread 21%, sluice 14%, other 4%, libc mem* 3%

## w02-tp-4k-d16-w8  (total 51.1 G cycles)

| bucket | share |
|---|---|
| kernel | 76.5% |
| libc pthread | 11.6% |
| sluice | 5.1% |
| other | 5.0% |
| libc mem* | 0.9% |
| c++ runtime | 0.4% |
| bench harness | 0.3% |
| libc wrapper self | 0.1% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 21.54% |
| kernel | futex_wait (mutex) | 19.03% |
| kernel | futex_wait (timed) | 13.02% |
| kernel | pread | 9.83% |
| kernel | futex_wake (condvar) | 7.01% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 4.79% |
| kernel | clone (thread create) | 4.48% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 4.29% |
| other | __GI___lll_lock_wait | 1.88% |
| libc pthread | pthread_cond_wait@@GLIBC_2.3.2 | 1.20% |
| sluice | sluice::async::detail::RequestCore::occupancy | 0.92% |
| libc mem* | __memcmp_avx2_movbe | 0.83% |
| other | __GI___lll_lock_wake | 0.62% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.60% |
| kernel | mutex_unlock (userspace) | 0.59% |
| other | __internal_syscall_cancel | 0.57% |

- **worker thread** — 71.4% of all cycles: kernel 83%, libc pthread 9%, other 5%, sluice 2%, bench harness 0%
- **main (submit + consume)** — 28.6% of all cycles: kernel 61%, libc pthread 17%, sluice 12%, other 4%, libc mem* 3%

## w02-tp-64k-d16-w4  (total 7.7 G cycles)

| bucket | share |
|---|---|
| kernel | 80.4% |
| libc pthread | 6.9% |
| libc mem* | 6.7% |
| sluice | 3.2% |
| other | 2.1% |
| c++ runtime | 0.3% |
| bench harness | 0.2% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | pread | 54.11% |
| kernel | futex_wake (mutex) | 7.85% |
| kernel | futex_wait (mutex) | 6.66% |
| libc mem* | __memcmp_avx2_movbe | 6.20% |
| kernel | clone (thread create) | 4.19% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 3.15% |
| kernel | futex_wait (timed) | 2.64% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 2.62% |
| kernel | futex_wake (condvar) | 1.57% |
| kernel | pwrite | 0.81% |
| kernel | syscall machinery via __poll | 0.79% |
| other | __GI___lll_lock_wait | 0.71% |
| sluice | sluice::async::detail::RequestCore::occupancy | 0.62% |
| kernel | syscall machinery via read | 0.57% |
| libc mem* | __memmove_avx_unaligned_erms | 0.46% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.43% |

- **worker thread** — 75.1% of all cycles: kernel 93%, libc pthread 4%, other 2%, sluice 1%, bench harness 0%
- **main (submit + consume)** — 24.9% of all cycles: kernel 44%, libc mem* 27%, libc pthread 16%, sluice 9%, other 3%

## w02-uring-4k-d16  (total 37.3 G cycles)

| bucket | share |
|---|---|
| kernel | 45.1% |
| kernel (io-wq worker) | 34.6% |
| sluice | 10.6% |
| libc pthread | 3.9% |
| libc mem* | 2.4% |
| other | 2.1% |
| c++ runtime | 0.8% |
| bench harness | 0.6% |

| bucket | detail | share |
|---|---|---|
| kernel | io_uring_enter | 36.14% |
| kernel (io-wq worker) | rep_movs_alternative | 6.08% |
| kernel (io-wq worker) | _raw_spin_lock | 2.89% |
| libc mem* | __memcmp_avx2_movbe | 2.34% |
| kernel (io-wq worker) | shmem_file_read_iter | 2.18% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.82% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 1.76% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 1.45% |
| kernel (io-wq worker) | io_cq_unlock_post | 1.21% |
| kernel (io-wq worker) | native_queued_spin_lock_slowpath | 0.97% |
| kernel (io-wq worker) | __raw_spin_lock_irqsave | 0.91% |
| kernel | syscall machinery via __memcmp_avx2_movbe | 0.90% |
| kernel | clock_gettime | 0.76% |
| kernel (io-wq worker) | filemap_get_entry | 0.76% |
| kernel (io-wq worker) | io_worker_handle_work | 0.73% |
| sluice | sluice::async::UringAsyncBackend::service_pending_controls_locked_ | 0.73% |

- **main (submit + consume)** — 65.4% of all cycles: kernel 69%, sluice 16%, libc pthread 6%, libc mem* 4%, other 3%
- **io-wq kernel worker** — 34.6% of all cycles: kernel (io-wq worker) 100%

## w02-uring-4k-d64  (total 20.6 G cycles)

| bucket | share |
|---|---|
| kernel | 44.5% |
| kernel (io-wq worker) | 30.6% |
| sluice | 16.0% |
| libc pthread | 3.3% |
| libc mem* | 2.2% |
| other | 1.9% |
| c++ runtime | 0.8% |
| bench harness | 0.6% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | io_uring_enter | 36.41% |
| kernel (io-wq worker) | rep_movs_alternative | 4.91% |
| sluice | sluice::async::detail::RequestCore::occupancy | 4.22% |
| kernel (io-wq worker) | _raw_spin_lock | 2.44% |
| libc mem* | __memcmp_avx2_movbe | 2.08% |
| kernel (io-wq worker) | shmem_file_read_iter | 1.94% |
| sluice | sluice::async::UringAsyncBackend::service_pending_controls_locked_ | 1.69% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 1.64% |
| sluice | std::vector | 1.26% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 1.11% |
| sluice | sluice::async::RequestScope::Slot* sluice::async::RequestScope::resolve_ | 1.09% |
| kernel (io-wq worker) | __raw_spin_lock_irqsave | 0.94% |
| kernel (io-wq worker) | io_cq_unlock_post | 0.94% |
| kernel | syscall machinery via sluice::async::detail::RequestCore::occupancy | 0.82% |
| kernel | syscall machinery via __memcmp_avx2_movbe | 0.70% |
| kernel (io-wq worker) | filemap_get_entry | 0.69% |

- **main (submit + consume)** — 69.4% of all cycles: kernel 64%, sluice 23%, libc pthread 5%, libc mem* 3%, other 3%
- **io-wq kernel worker** — 30.6% of all cycles: kernel (io-wq worker) 100%

## w03-tp-4k-d16-w4  (total 52.2 G cycles)

| bucket | share |
|---|---|
| kernel | 74.4% |
| libc pthread | 14.7% |
| sluice | 4.7% |
| other | 4.5% |
| libc mem* | 0.9% |
| c++ runtime | 0.3% |
| bench harness | 0.2% |
| libc wrapper self | 0.1% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 20.97% |
| kernel | futex_wait (mutex) | 19.72% |
| kernel | pread | 10.74% |
| kernel | futex_wait (timed) | 8.81% |
| kernel | clone (thread create) | 6.76% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 6.73% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 5.53% |
| kernel | futex_wake (condvar) | 4.80% |
| other | __GI___lll_lock_wait | 1.91% |
| kernel | epoll_wait | 1.32% |
| libc pthread | pthread_cond_wait@@GLIBC_2.3.2 | 0.98% |
| libc mem* | __memcmp_avx2_movbe | 0.92% |
| sluice | sluice::async::detail::RequestCore::occupancy | 0.85% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.74% |
| other | __GI___lll_lock_wake | 0.55% |
| kernel | mutex_unlock (userspace) | 0.54% |

- **worker thread** — 76.0% of all cycles: kernel 78%, libc pthread 13%, other 5%, sluice 3%, bench harness 0%
- **main (submit + consume)** — 24.0% of all cycles: kernel 63%, libc pthread 19%, sluice 9%, libc mem* 4%, other 3%

## w03-uring-4k-d16  (total 26.7 G cycles)

| bucket | share |
|---|---|
| kernel | 52.1% |
| kernel (io-wq worker) | 24.9% |
| sluice | 8.7% |
| libc pthread | 8.7% |
| other | 2.6% |
| libc mem* | 2.1% |
| c++ runtime | 0.6% |
| bench harness | 0.4% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | io_uring_enter | 21.12% |
| kernel | futex_wake (mutex) | 9.05% |
| kernel | futex_wait (mutex) | 7.78% |
| kernel | futex_wait (timed) | 5.13% |
| kernel (io-wq worker) | rep_movs_alternative | 4.31% |
| libc pthread | __GI___pthread_mutex_unlock_usercnt | 3.94% |
| libc pthread | pthread_mutex_lock@@GLIBC_2.2.5 | 3.63% |
| kernel | clone (thread create) | 2.42% |
| kernel | epoll_wait | 2.14% |
| libc mem* | __memcmp_avx2_movbe | 2.00% |
| kernel (io-wq worker) | _raw_spin_lock | 1.91% |
| kernel (io-wq worker) | shmem_file_read_iter | 1.55% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.27% |
| kernel (io-wq worker) | __raw_spin_lock_irqsave | 1.06% |
| kernel (io-wq worker) | native_queued_spin_lock_slowpath | 1.06% |
| kernel (io-wq worker) | io_cq_unlock_post | 0.84% |

- **main (submit + consume)** — 54.0% of all cycles: kernel 73%, sluice 10%, libc pthread 8%, libc mem* 4%, other 3%
- **io-wq kernel worker** — 24.9% of all cycles: kernel (io-wq worker) 100%
- **worker thread** — 21.1% of all cycles: kernel 61%, libc pthread 20%, sluice 15%, other 4%, c++ runtime 0%
