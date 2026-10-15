# Where the time goes (cycle-weighted self cost)

Evidence classes: every share is **directly sampled** leaf self cost (what the
CPU was executing when the sample fired). Kernel cycles are additionally
**charged by call path** to the syscall that entered the kernel; that charge is
an inference from the stack, not an observation, and the per-case confidence
line below says how often the wrapper was found directly above the kernel
frames and how often frames had to be skipped. Kernel cycles with no syscall
entering frame are reported as `kernel (interrupt/exception)` or
`kernel (syscall, wrapper unresolved)` and are never charged to a Sluice
function. Syscall **event counts** are a separate instrument and live in
`SYSCALLS.md`.

Shares are of all cycles captured for the case, across all threads.

## w01-read-4k-seq  (total 24.7 G cycles)

| bucket | share |
|---|---|
| kernel | 86.3% |
| libc mem* | 4.2% |
| other | 3.6% |
| sluice | 2.7% |
| bench harness | 1.6% |
| c++ runtime | 0.9% |
| libc wrapper self | 0.5% |
| kernel (interrupt/exception) | 0.1% |
| kernel (syscall, wrapper unresolved) | 0.1% |

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
| kernel | fdatasync | 0.37% |
| sluice | sluice::blocking::read_exact_at | 0.35% |
| other | decltype | 0.34% |

call-path confidence, as a share of the 21.4 G cycles with a kernel leaf (86.5% of the case): direct 99.83%, interrupt 0.08%, no-wrapper 0.08%, gap 0.00%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.00% — entering frame `bpf_prog_31efb7c56239148b_restrict_filesystems`, charged to `open`

## w01-sync-data-4k  (total 2.1 G cycles)

| bucket | share |
|---|---|
| kernel | 85.4% |
| libc mem* | 13.1% |
| kernel (interrupt/exception) | 0.5% |
| other | 0.5% |
| kernel (syscall, wrapper unresolved) | 0.3% |
| sluice | 0.1% |
| bench harness | 0.1% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | pwrite | 63.80% |
| kernel | open | 16.31% |
| libc mem* | __memmove_avx_unaligned_erms | 13.13% |
| kernel | fdatasync | 4.91% |
| kernel (interrupt/exception) | asm_exc_page_fault | 0.32% |
| kernel (syscall, wrapper unresolved) | [unknown] | 0.16% |
| other | __internal_syscall_cancel | 0.13% |
| kernel (interrupt/exception) | __irqentry_text_end | 0.11% |
| kernel | mmap | 0.10% |
| kernel | execve | 0.08% |
| kernel (interrupt/exception) | asm_sysvec_apic_timer_interrupt | 0.08% |
| other | __memset_avx2_unaligned_erms | 0.07% |
| kernel (syscall, wrapper unresolved) | __open64_nocancel | 0.07% |
| kernel | write | 0.05% |
| other | _dl_new_hash | 0.05% |
| other | do_lookup_x | 0.04% |

call-path confidence, as a share of the 1.8 G cycles with a kernel leaf (86.2% of the case): direct 99.01%, interrupt 0.59%, no-wrapper 0.40%

## w01-write-4k-seq  (total 33.8 G cycles)

| bucket | share |
|---|---|
| kernel | 87.1% |
| libc mem* | 4.4% |
| other | 3.2% |
| sluice | 2.7% |
| bench harness | 1.3% |
| c++ runtime | 0.7% |
| libc wrapper self | 0.4% |
| kernel (syscall, wrapper unresolved) | 0.1% |
| kernel (interrupt/exception) | 0.1% |

| bucket | detail | share |
|---|---|---|
| kernel | pwrite | 66.71% |
| kernel | pread | 18.87% |
| libc mem* | __memmove_avx_unaligned_erms | 2.28% |
| libc mem* | __memcmp_avx2_movbe | 2.11% |
| other | __internal_syscall_cancel | 2.04% |
| kernel | open | 0.75% |
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

call-path confidence, as a share of the 29.5 G cycles with a kernel leaf (87.2% of the case): direct 99.82%, no-wrapper 0.09%, interrupt 0.09%

## w02-tp-4k-d16-w1  (total 27.1 G cycles)

| bucket | share |
|---|---|
| kernel | 58.4% |
| libc pthread | 26.5% |
| sluice | 9.2% |
| libc mem* | 1.9% |
| other | 1.9% |
| c++ runtime | 0.8% |
| kernel (syscall, wrapper unresolved) | 0.7% |
| bench harness | 0.5% |
| libc wrapper self | 0.2% |
| kernel (interrupt/exception) | 0.1% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 20.46% |
| kernel | pread | 15.17% |
| kernel | futex_wait (mutex) | 13.68% |
| libc pthread | pthread_mutex_lock | 11.01% |
| libc pthread | __pthread_mutex_unlock_usercnt | 10.69% |
| kernel | eventfd write (completion notification) | 5.65% |
| libc pthread | __lll_lock_wait | 2.01% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.87% |
| libc mem* | __memcmp_avx2_movbe | 1.85% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 1.04% |
| kernel | poll | 1.00% |
| libc pthread | __lll_lock_wake | 0.73% |
| libc pthread | __gthread_mutex_lock | 0.65% |
| other | __syscall_cancel_arch_end | 0.57% |
| kernel | mutex_unlock (userspace) | 0.54% |
| kernel | clock_gettime | 0.54% |

call-path confidence, as a share of the 16.0 G cycles with a kernel leaf (59.1% of the case): direct 97.87%, no-wrapper 1.13%, gap 0.90%, interrupt 0.10%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.34% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `mutex_unlock (userspace)`
* 0.24% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `clone`
* 0.12% — entering frame `std::mutex::lock`, charged to `clone`
* 0.10% — entering frame `sluice::async::ThreadPoolBackend::worker_loop`, charged to `clone`
* 0.04% — entering frame `[unknown]`, charged to `eventfd write (completion notification)`

## w02-tp-4k-d16-w4  (total 74.4 G cycles)

| bucket | share |
|---|---|
| kernel | 71.8% |
| libc pthread | 17.2% |
| sluice | 6.5% |
| other | 1.9% |
| libc mem* | 1.1% |
| c++ runtime | 0.6% |
| bench harness | 0.4% |
| kernel (syscall, wrapper unresolved) | 0.4% |
| libc wrapper self | 0.2% |
| kernel (interrupt/exception) | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 21.52% |
| kernel | futex_wait (mutex) | 19.05% |
| kernel | pread | 11.79% |
| kernel | futex_wait (timed) | 7.97% |
| libc pthread | __pthread_mutex_unlock_usercnt | 6.17% |
| libc pthread | pthread_mutex_lock | 5.77% |
| kernel | eventfd write (completion notification) | 5.06% |
| kernel | futex_wake (condvar) | 4.78% |
| libc pthread | __lll_lock_wait | 2.05% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.13% |
| libc mem* | __memcmp_avx2_movbe | 1.02% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.81% |
| libc pthread | pthread_cond_wait | 0.79% |
| libc pthread | __lll_lock_wake | 0.67% |
| kernel | mutex_unlock (userspace) | 0.60% |
| other | __internal_syscall_cancel | 0.47% |

call-path confidence, as a share of the 53.7 G cycles with a kernel leaf (72.1% of the case): direct 99.12%, no-wrapper 0.50%, gap 0.34%, interrupt 0.04%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.09% — entering frame `sluice::async::ThreadPoolBackend::worker_loop`, charged to `clone`
* 0.08% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `mutex_unlock (userspace)`
* 0.05% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `clone`
* 0.04% — entering frame `sluice::async::detail::ProgressSource::wake_notification_nolock_`, charged to `clone`
* 0.03% — entering frame `std::mutex::lock`, charged to `clone`

## w02-tp-4k-d16-w8  (total 51.1 G cycles)

| bucket | share |
|---|---|
| kernel | 76.3% |
| libc pthread | 14.6% |
| sluice | 5.1% |
| other | 2.0% |
| libc mem* | 0.9% |
| c++ runtime | 0.4% |
| bench harness | 0.3% |
| kernel (syscall, wrapper unresolved) | 0.3% |
| libc wrapper self | 0.2% |
| kernel (interrupt/exception) | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 21.54% |
| kernel | futex_wait (mutex) | 19.03% |
| kernel | futex_wait (timed) | 13.02% |
| kernel | pread | 9.83% |
| kernel | futex_wake (condvar) | 7.01% |
| libc pthread | __pthread_mutex_unlock_usercnt | 4.79% |
| libc pthread | pthread_mutex_lock | 4.29% |
| kernel | eventfd write (completion notification) | 4.10% |
| libc pthread | __lll_lock_wait | 1.88% |
| libc pthread | pthread_cond_wait | 1.20% |
| sluice | sluice::async::detail::RequestCore::occupancy | 0.92% |
| libc mem* | __memcmp_avx2_movbe | 0.83% |
| libc pthread | __lll_lock_wake | 0.62% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.60% |
| kernel | mutex_unlock (userspace) | 0.59% |
| other | __internal_syscall_cancel | 0.57% |

call-path confidence, as a share of the 39.1 G cycles with a kernel leaf (76.5% of the case): direct 99.38%, no-wrapper 0.33%, gap 0.25%, interrupt 0.04%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.06% — entering frame `sluice::async::ThreadPoolBackend::worker_loop`, charged to `clone`
* 0.05% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `mutex_unlock (userspace)`
* 0.04% — entering frame `sluice::async::detail::ProgressSource::wake_notification_nolock_`, charged to `clone`
* 0.03% — entering frame `start_thread`, charged to `clone`
* 0.02% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `clone`

## w02-tp-64k-d16-w4  (total 7.7 G cycles)

| bucket | share |
|---|---|
| kernel | 80.0% |
| libc pthread | 7.9% |
| libc mem* | 6.7% |
| sluice | 3.2% |
| other | 1.1% |
| kernel (syscall, wrapper unresolved) | 0.4% |
| c++ runtime | 0.3% |
| bench harness | 0.2% |
| libc wrapper self | 0.1% |
| kernel (interrupt/exception) | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | pread | 54.11% |
| kernel | futex_wake (mutex) | 7.85% |
| kernel | futex_wait (mutex) | 6.66% |
| libc mem* | __memcmp_avx2_movbe | 6.20% |
| libc pthread | __pthread_mutex_unlock_usercnt | 3.15% |
| kernel | eventfd write (completion notification) | 3.15% |
| kernel | futex_wait (timed) | 2.64% |
| libc pthread | pthread_mutex_lock | 2.62% |
| kernel | futex_wake (condvar) | 1.57% |
| kernel | pwrite | 0.81% |
| kernel | poll | 0.79% |
| kernel | clone | 0.74% |
| libc pthread | __lll_lock_wait | 0.71% |
| sluice | sluice::async::detail::RequestCore::occupancy | 0.62% |
| kernel | eventfd read (notification drain) | 0.57% |
| libc mem* | __memmove_avx_unaligned_erms | 0.46% |

call-path confidence, as a share of the 6.2 G cycles with a kernel leaf (80.4% of the case): direct 99.25%, no-wrapper 0.44%, gap 0.24%, interrupt 0.06%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.07% — entering frame `start_thread`, charged to `clone`
* 0.04% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `mutex_unlock (userspace)`
* 0.03% — entering frame `sluice::async::detail::ProgressSource::wake_notification_nolock_`, charged to `clone`
* 0.03% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `clone`
* 0.02% — entering frame `__lll_lock_wake_private`, charged to `clone`

## w02-uring-4k-d16  (total 37.3 G cycles)

| bucket | share |
|---|---|
| kernel | 38.1% |
| kernel (io-wq worker) | 34.6% |
| sluice | 10.6% |
| kernel (interrupt/exception) | 6.5% |
| libc pthread | 3.9% |
| libc mem* | 2.4% |
| other | 2.1% |
| c++ runtime | 0.8% |
| bench harness | 0.6% |
| kernel (syscall, wrapper unresolved) | 0.5% |

| bucket | detail | share |
|---|---|---|
| kernel | io_uring_enter | 36.14% |
| kernel (io-wq worker) | rep_movs_alternative | 6.08% |
| kernel (interrupt/exception) | asm_sysvec_reschedule_ipi | 4.75% |
| kernel (io-wq worker) | _raw_spin_lock | 2.89% |
| libc mem* | __memcmp_avx2_movbe | 2.34% |
| kernel (io-wq worker) | shmem_file_read_iter | 2.18% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.82% |
| libc pthread | pthread_mutex_lock | 1.76% |
| kernel (interrupt/exception) | __irqentry_text_end | 1.55% |
| libc pthread | __pthread_mutex_unlock_usercnt | 1.45% |
| kernel (io-wq worker) | io_cq_unlock_post | 1.21% |
| kernel (io-wq worker) | native_queued_spin_lock_slowpath | 0.97% |
| kernel (io-wq worker) | __raw_spin_lock_irqsave | 0.91% |
| kernel | clock_gettime | 0.76% |
| kernel (io-wq worker) | filemap_get_entry | 0.76% |
| kernel (io-wq worker) | io_worker_handle_work | 0.73% |

call-path confidence, as a share of the 29.7 G cycles with a kernel leaf (79.7% of the case): direct 91.19%, interrupt 8.19%, no-wrapper 0.62%, gap 0.00%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.00% — entering frame `sluice::async::operator==`, charged to `clock_gettime`

## w02-uring-4k-d64  (total 20.6 G cycles)

| bucket | share |
|---|---|
| kernel | 38.1% |
| kernel (io-wq worker) | 30.6% |
| sluice | 16.0% |
| kernel (interrupt/exception) | 6.0% |
| libc pthread | 3.3% |
| libc mem* | 2.2% |
| other | 1.9% |
| c++ runtime | 0.8% |
| bench harness | 0.6% |
| kernel (syscall, wrapper unresolved) | 0.5% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | io_uring_enter | 36.41% |
| kernel (io-wq worker) | rep_movs_alternative | 4.91% |
| kernel (interrupt/exception) | asm_sysvec_reschedule_ipi | 4.36% |
| sluice | sluice::async::detail::RequestCore::occupancy | 4.22% |
| kernel (io-wq worker) | _raw_spin_lock | 2.44% |
| libc mem* | __memcmp_avx2_movbe | 2.08% |
| kernel (io-wq worker) | shmem_file_read_iter | 1.94% |
| sluice | sluice::async::UringAsyncBackend::service_pending_controls_locked_ | 1.69% |
| libc pthread | pthread_mutex_lock | 1.64% |
| kernel (interrupt/exception) | __irqentry_text_end | 1.44% |
| sluice | std::vector | 1.26% |
| libc pthread | __pthread_mutex_unlock_usercnt | 1.11% |
| sluice | sluice::async::RequestScope::Slot* sluice::async::RequestScope::resolve_ | 1.09% |
| kernel (io-wq worker) | __raw_spin_lock_irqsave | 0.94% |
| kernel (io-wq worker) | io_cq_unlock_post | 0.94% |
| kernel (io-wq worker) | filemap_get_entry | 0.69% |

call-path confidence, as a share of the 15.5 G cycles with a kernel leaf (75.2% of the case): direct 91.39%, interrupt 7.95%, no-wrapper 0.65%

## w03-tp-4k-d16-w4  (total 52.2 G cycles)

| bucket | share |
|---|---|
| kernel | 74.1% |
| libc pthread | 17.5% |
| sluice | 4.7% |
| other | 1.7% |
| libc mem* | 0.9% |
| c++ runtime | 0.3% |
| kernel (syscall, wrapper unresolved) | 0.3% |
| bench harness | 0.2% |
| libc wrapper self | 0.1% |
| kernel (interrupt/exception) | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 20.97% |
| kernel | futex_wait (mutex) | 19.72% |
| kernel | pread | 10.74% |
| kernel | futex_wait (timed) | 8.81% |
| libc pthread | __pthread_mutex_unlock_usercnt | 6.73% |
| libc pthread | pthread_mutex_lock | 5.53% |
| kernel | eventfd write (completion notification) | 5.26% |
| kernel | futex_wake (condvar) | 4.80% |
| libc pthread | __lll_lock_wait | 1.91% |
| kernel | epoll_wait | 1.32% |
| kernel | eventfd read (notification drain) | 1.12% |
| libc pthread | pthread_cond_wait | 0.98% |
| libc mem* | __memcmp_avx2_movbe | 0.92% |
| sluice | sluice::async::detail::RequestCore::occupancy | 0.85% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.74% |
| libc pthread | __lll_lock_wake | 0.55% |

call-path confidence, as a share of the 38.9 G cycles with a kernel leaf (74.4% of the case): direct 99.23%, gap 0.36%, no-wrapper 0.36%, interrupt 0.05%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.08% — entering frame `std::mutex::lock`, charged to `clone`
* 0.07% — entering frame `sluice::async::ThreadPoolBackend::worker_loop`, charged to `clone`
* 0.05% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `mutex_unlock (userspace)`
* 0.05% — entering frame `sluice::async::detail::ProgressSource::drain_notification_nolock_`, charged to `clone`
* 0.03% — entering frame `sluice::async::detail::ProgressSource::wake_notification_nolock_`, charged to `clone`

## w03-uring-4k-d16  (total 26.7 G cycles)

| bucket | share |
|---|---|
| kernel | 50.2% |
| kernel (io-wq worker) | 24.9% |
| libc pthread | 9.4% |
| sluice | 8.7% |
| libc mem* | 2.1% |
| other | 1.9% |
| kernel (interrupt/exception) | 1.7% |
| c++ runtime | 0.6% |
| bench harness | 0.4% |
| kernel (syscall, wrapper unresolved) | 0.2% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | io_uring_enter | 21.12% |
| kernel | futex_wake (mutex) | 9.05% |
| kernel | futex_wait (mutex) | 7.78% |
| kernel | futex_wait (timed) | 5.13% |
| kernel (io-wq worker) | rep_movs_alternative | 4.31% |
| libc pthread | __pthread_mutex_unlock_usercnt | 3.94% |
| libc pthread | pthread_mutex_lock | 3.63% |
| kernel | epoll_wait | 2.14% |
| kernel | eventfd read (notification drain) | 2.10% |
| libc mem* | __memcmp_avx2_movbe | 2.00% |
| kernel (io-wq worker) | _raw_spin_lock | 1.91% |
| kernel (io-wq worker) | shmem_file_read_iter | 1.55% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.27% |
| kernel (interrupt/exception) | asm_sysvec_reschedule_ipi | 1.21% |
| kernel (io-wq worker) | __raw_spin_lock_irqsave | 1.06% |
| kernel (io-wq worker) | native_queued_spin_lock_slowpath | 1.06% |

call-path confidence, as a share of the 20.5 G cycles with a kernel leaf (76.9% of the case): direct 97.30%, interrupt 2.16%, no-wrapper 0.32%, gap 0.22%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.11% — entering frame `sluice::async::detail::ProgressSource::drain_notification_nolock_`, charged to `clone`
* 0.08% — entering frame `std::mutex::lock`, charged to `clone`
* 0.01% — entering frame `sluice::async::UringAsyncBackend::service_pending_controls_locked_`, charged to `mutex_unlock (userspace)`
* 0.00% — entering frame `start_thread`, charged to `clone`
* 0.00% — entering frame `epoll_ctl`, charged to `clone`

# Per-thread-role split

## w01-read-4k-seq  (total 24.7 G cycles)

| bucket | share |
|---|---|
| kernel | 86.3% |
| libc mem* | 4.2% |
| other | 3.6% |
| sluice | 2.7% |
| bench harness | 1.6% |
| c++ runtime | 0.9% |
| libc wrapper self | 0.5% |
| kernel (interrupt/exception) | 0.1% |
| kernel (syscall, wrapper unresolved) | 0.1% |

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
| kernel | fdatasync | 0.37% |
| sluice | sluice::blocking::read_exact_at | 0.35% |
| other | decltype | 0.34% |

call-path confidence, as a share of the 21.4 G cycles with a kernel leaf (86.5% of the case): direct 99.83%, interrupt 0.08%, no-wrapper 0.08%, gap 0.00%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.00% — entering frame `bpf_prog_31efb7c56239148b_restrict_filesystems`, charged to `open`

- **main (submit + consume)** — 100.0% of all cycles: kernel 86%, libc mem* 4%, other 4%, sluice 3%, bench harness 2%

## w01-sync-data-4k  (total 2.1 G cycles)

| bucket | share |
|---|---|
| kernel | 85.4% |
| libc mem* | 13.1% |
| kernel (interrupt/exception) | 0.5% |
| other | 0.5% |
| kernel (syscall, wrapper unresolved) | 0.3% |
| sluice | 0.1% |
| bench harness | 0.1% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | pwrite | 63.80% |
| kernel | open | 16.31% |
| libc mem* | __memmove_avx_unaligned_erms | 13.13% |
| kernel | fdatasync | 4.91% |
| kernel (interrupt/exception) | asm_exc_page_fault | 0.32% |
| kernel (syscall, wrapper unresolved) | [unknown] | 0.16% |
| other | __internal_syscall_cancel | 0.13% |
| kernel (interrupt/exception) | __irqentry_text_end | 0.11% |
| kernel | mmap | 0.10% |
| kernel | execve | 0.08% |
| kernel (interrupt/exception) | asm_sysvec_apic_timer_interrupt | 0.08% |
| other | __memset_avx2_unaligned_erms | 0.07% |
| kernel (syscall, wrapper unresolved) | __open64_nocancel | 0.07% |
| kernel | write | 0.05% |
| other | _dl_new_hash | 0.05% |
| other | do_lookup_x | 0.04% |

call-path confidence, as a share of the 1.8 G cycles with a kernel leaf (86.2% of the case): direct 99.01%, interrupt 0.59%, no-wrapper 0.40%

- **main (submit + consume)** — 100.0% of all cycles: kernel 85%, libc mem* 13%, kernel (interrupt/exception) 1%, other 0%, kernel (syscall, wrapper unresolved) 0%

## w01-write-4k-seq  (total 33.8 G cycles)

| bucket | share |
|---|---|
| kernel | 87.1% |
| libc mem* | 4.4% |
| other | 3.2% |
| sluice | 2.7% |
| bench harness | 1.3% |
| c++ runtime | 0.7% |
| libc wrapper self | 0.4% |
| kernel (syscall, wrapper unresolved) | 0.1% |
| kernel (interrupt/exception) | 0.1% |

| bucket | detail | share |
|---|---|---|
| kernel | pwrite | 66.71% |
| kernel | pread | 18.87% |
| libc mem* | __memmove_avx_unaligned_erms | 2.28% |
| libc mem* | __memcmp_avx2_movbe | 2.11% |
| other | __internal_syscall_cancel | 2.04% |
| kernel | open | 0.75% |
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

call-path confidence, as a share of the 29.5 G cycles with a kernel leaf (87.2% of the case): direct 99.82%, no-wrapper 0.09%, interrupt 0.09%

- **main (submit + consume)** — 100.0% of all cycles: kernel 87%, libc mem* 4%, other 3%, sluice 3%, bench harness 1%

## w02-tp-4k-d16-w1  (total 27.1 G cycles)

| bucket | share |
|---|---|
| kernel | 58.4% |
| libc pthread | 26.5% |
| sluice | 9.2% |
| libc mem* | 1.9% |
| other | 1.9% |
| c++ runtime | 0.8% |
| kernel (syscall, wrapper unresolved) | 0.7% |
| bench harness | 0.5% |
| libc wrapper self | 0.2% |
| kernel (interrupt/exception) | 0.1% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 20.46% |
| kernel | pread | 15.17% |
| kernel | futex_wait (mutex) | 13.68% |
| libc pthread | pthread_mutex_lock | 11.01% |
| libc pthread | __pthread_mutex_unlock_usercnt | 10.69% |
| kernel | eventfd write (completion notification) | 5.65% |
| libc pthread | __lll_lock_wait | 2.01% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.87% |
| libc mem* | __memcmp_avx2_movbe | 1.85% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 1.04% |
| kernel | poll | 1.00% |
| libc pthread | __lll_lock_wake | 0.73% |
| libc pthread | __gthread_mutex_lock | 0.65% |
| other | __syscall_cancel_arch_end | 0.57% |
| kernel | mutex_unlock (userspace) | 0.54% |
| kernel | clock_gettime | 0.54% |

call-path confidence, as a share of the 16.0 G cycles with a kernel leaf (59.1% of the case): direct 97.87%, no-wrapper 1.13%, gap 0.90%, interrupt 0.10%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.34% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `mutex_unlock (userspace)`
* 0.24% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `clone`
* 0.12% — entering frame `std::mutex::lock`, charged to `clone`
* 0.10% — entering frame `sluice::async::ThreadPoolBackend::worker_loop`, charged to `clone`
* 0.04% — entering frame `[unknown]`, charged to `eventfd write (completion notification)`

- **main (submit + consume)** — 50.7% of all cycles: kernel 43%, libc pthread 34%, sluice 14%, libc mem* 4%, c++ runtime 2%
- **worker thread** — 49.3% of all cycles: kernel 74%, libc pthread 19%, sluice 4%, other 2%, libc wrapper self 0%

## w02-tp-4k-d16-w4  (total 74.4 G cycles)

| bucket | share |
|---|---|
| kernel | 71.8% |
| libc pthread | 17.2% |
| sluice | 6.5% |
| other | 1.9% |
| libc mem* | 1.1% |
| c++ runtime | 0.6% |
| bench harness | 0.4% |
| kernel (syscall, wrapper unresolved) | 0.4% |
| libc wrapper self | 0.2% |
| kernel (interrupt/exception) | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 21.52% |
| kernel | futex_wait (mutex) | 19.05% |
| kernel | pread | 11.79% |
| kernel | futex_wait (timed) | 7.97% |
| libc pthread | __pthread_mutex_unlock_usercnt | 6.17% |
| libc pthread | pthread_mutex_lock | 5.77% |
| kernel | eventfd write (completion notification) | 5.06% |
| kernel | futex_wake (condvar) | 4.78% |
| libc pthread | __lll_lock_wait | 2.05% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.13% |
| libc mem* | __memcmp_avx2_movbe | 1.02% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.81% |
| libc pthread | pthread_cond_wait | 0.79% |
| libc pthread | __lll_lock_wake | 0.67% |
| kernel | mutex_unlock (userspace) | 0.60% |
| other | __internal_syscall_cancel | 0.47% |

call-path confidence, as a share of the 53.7 G cycles with a kernel leaf (72.1% of the case): direct 99.12%, no-wrapper 0.50%, gap 0.34%, interrupt 0.04%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.09% — entering frame `sluice::async::ThreadPoolBackend::worker_loop`, charged to `clone`
* 0.08% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `mutex_unlock (userspace)`
* 0.05% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `clone`
* 0.04% — entering frame `sluice::async::detail::ProgressSource::wake_notification_nolock_`, charged to `clone`
* 0.03% — entering frame `std::mutex::lock`, charged to `clone`

- **worker thread** — 67.6% of all cycles: kernel 81%, libc pthread 14%, sluice 3%, other 2%, libc wrapper self 0%
- **main (submit + consume)** — 32.4% of all cycles: kernel 53%, libc pthread 24%, sluice 14%, libc mem* 3%, c++ runtime 2%

## w02-tp-4k-d16-w8  (total 51.1 G cycles)

| bucket | share |
|---|---|
| kernel | 76.3% |
| libc pthread | 14.6% |
| sluice | 5.1% |
| other | 2.0% |
| libc mem* | 0.9% |
| c++ runtime | 0.4% |
| bench harness | 0.3% |
| kernel (syscall, wrapper unresolved) | 0.3% |
| libc wrapper self | 0.2% |
| kernel (interrupt/exception) | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 21.54% |
| kernel | futex_wait (mutex) | 19.03% |
| kernel | futex_wait (timed) | 13.02% |
| kernel | pread | 9.83% |
| kernel | futex_wake (condvar) | 7.01% |
| libc pthread | __pthread_mutex_unlock_usercnt | 4.79% |
| libc pthread | pthread_mutex_lock | 4.29% |
| kernel | eventfd write (completion notification) | 4.10% |
| libc pthread | __lll_lock_wait | 1.88% |
| libc pthread | pthread_cond_wait | 1.20% |
| sluice | sluice::async::detail::RequestCore::occupancy | 0.92% |
| libc mem* | __memcmp_avx2_movbe | 0.83% |
| libc pthread | __lll_lock_wake | 0.62% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.60% |
| kernel | mutex_unlock (userspace) | 0.59% |
| other | __internal_syscall_cancel | 0.57% |

call-path confidence, as a share of the 39.1 G cycles with a kernel leaf (76.5% of the case): direct 99.38%, no-wrapper 0.33%, gap 0.25%, interrupt 0.04%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.06% — entering frame `sluice::async::ThreadPoolBackend::worker_loop`, charged to `clone`
* 0.05% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `mutex_unlock (userspace)`
* 0.04% — entering frame `sluice::async::detail::ProgressSource::wake_notification_nolock_`, charged to `clone`
* 0.03% — entering frame `start_thread`, charged to `clone`
* 0.02% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `clone`

- **worker thread** — 71.4% of all cycles: kernel 83%, libc pthread 12%, sluice 2%, other 2%, libc wrapper self 0%
- **main (submit + consume)** — 28.6% of all cycles: kernel 60%, libc pthread 20%, sluice 12%, libc mem* 3%, other 1%

## w02-tp-64k-d16-w4  (total 7.7 G cycles)

| bucket | share |
|---|---|
| kernel | 80.0% |
| libc pthread | 7.9% |
| libc mem* | 6.7% |
| sluice | 3.2% |
| other | 1.1% |
| kernel (syscall, wrapper unresolved) | 0.4% |
| c++ runtime | 0.3% |
| bench harness | 0.2% |
| libc wrapper self | 0.1% |
| kernel (interrupt/exception) | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | pread | 54.11% |
| kernel | futex_wake (mutex) | 7.85% |
| kernel | futex_wait (mutex) | 6.66% |
| libc mem* | __memcmp_avx2_movbe | 6.20% |
| libc pthread | __pthread_mutex_unlock_usercnt | 3.15% |
| kernel | eventfd write (completion notification) | 3.15% |
| kernel | futex_wait (timed) | 2.64% |
| libc pthread | pthread_mutex_lock | 2.62% |
| kernel | futex_wake (condvar) | 1.57% |
| kernel | pwrite | 0.81% |
| kernel | poll | 0.79% |
| kernel | clone | 0.74% |
| libc pthread | __lll_lock_wait | 0.71% |
| sluice | sluice::async::detail::RequestCore::occupancy | 0.62% |
| kernel | eventfd read (notification drain) | 0.57% |
| libc mem* | __memmove_avx_unaligned_erms | 0.46% |

call-path confidence, as a share of the 6.2 G cycles with a kernel leaf (80.4% of the case): direct 99.25%, no-wrapper 0.44%, gap 0.24%, interrupt 0.06%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.07% — entering frame `start_thread`, charged to `clone`
* 0.04% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `mutex_unlock (userspace)`
* 0.03% — entering frame `sluice::async::detail::ProgressSource::wake_notification_nolock_`, charged to `clone`
* 0.03% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `clone`
* 0.02% — entering frame `__lll_lock_wake_private`, charged to `clone`

- **worker thread** — 75.1% of all cycles: kernel 93%, libc pthread 5%, sluice 1%, other 1%, libc wrapper self 0%
- **main (submit + consume)** — 24.9% of all cycles: kernel 42%, libc mem* 27%, libc pthread 17%, sluice 9%, other 2%

## w02-uring-4k-d16  (total 37.3 G cycles)

| bucket | share |
|---|---|
| kernel | 38.1% |
| kernel (io-wq worker) | 34.6% |
| sluice | 10.6% |
| kernel (interrupt/exception) | 6.5% |
| libc pthread | 3.9% |
| libc mem* | 2.4% |
| other | 2.1% |
| c++ runtime | 0.8% |
| bench harness | 0.6% |
| kernel (syscall, wrapper unresolved) | 0.5% |

| bucket | detail | share |
|---|---|---|
| kernel | io_uring_enter | 36.14% |
| kernel (io-wq worker) | rep_movs_alternative | 6.08% |
| kernel (interrupt/exception) | asm_sysvec_reschedule_ipi | 4.75% |
| kernel (io-wq worker) | _raw_spin_lock | 2.89% |
| libc mem* | __memcmp_avx2_movbe | 2.34% |
| kernel (io-wq worker) | shmem_file_read_iter | 2.18% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.82% |
| libc pthread | pthread_mutex_lock | 1.76% |
| kernel (interrupt/exception) | __irqentry_text_end | 1.55% |
| libc pthread | __pthread_mutex_unlock_usercnt | 1.45% |
| kernel (io-wq worker) | io_cq_unlock_post | 1.21% |
| kernel (io-wq worker) | native_queued_spin_lock_slowpath | 0.97% |
| kernel (io-wq worker) | __raw_spin_lock_irqsave | 0.91% |
| kernel | clock_gettime | 0.76% |
| kernel (io-wq worker) | filemap_get_entry | 0.76% |
| kernel (io-wq worker) | io_worker_handle_work | 0.73% |

call-path confidence, as a share of the 29.7 G cycles with a kernel leaf (79.7% of the case): direct 91.19%, interrupt 8.19%, no-wrapper 0.62%, gap 0.00%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.00% — entering frame `sluice::async::operator==`, charged to `clock_gettime`

- **main (submit + consume)** — 65.4% of all cycles: kernel 58%, sluice 16%, kernel (interrupt/exception) 10%, libc pthread 6%, libc mem* 4%
- **io-wq kernel worker** — 34.6% of all cycles: kernel (io-wq worker) 100%

## w02-uring-4k-d64  (total 20.6 G cycles)

| bucket | share |
|---|---|
| kernel | 38.1% |
| kernel (io-wq worker) | 30.6% |
| sluice | 16.0% |
| kernel (interrupt/exception) | 6.0% |
| libc pthread | 3.3% |
| libc mem* | 2.2% |
| other | 1.9% |
| c++ runtime | 0.8% |
| bench harness | 0.6% |
| kernel (syscall, wrapper unresolved) | 0.5% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | io_uring_enter | 36.41% |
| kernel (io-wq worker) | rep_movs_alternative | 4.91% |
| kernel (interrupt/exception) | asm_sysvec_reschedule_ipi | 4.36% |
| sluice | sluice::async::detail::RequestCore::occupancy | 4.22% |
| kernel (io-wq worker) | _raw_spin_lock | 2.44% |
| libc mem* | __memcmp_avx2_movbe | 2.08% |
| kernel (io-wq worker) | shmem_file_read_iter | 1.94% |
| sluice | sluice::async::UringAsyncBackend::service_pending_controls_locked_ | 1.69% |
| libc pthread | pthread_mutex_lock | 1.64% |
| kernel (interrupt/exception) | __irqentry_text_end | 1.44% |
| sluice | std::vector | 1.26% |
| libc pthread | __pthread_mutex_unlock_usercnt | 1.11% |
| sluice | sluice::async::RequestScope::Slot* sluice::async::RequestScope::resolve_ | 1.09% |
| kernel (io-wq worker) | __raw_spin_lock_irqsave | 0.94% |
| kernel (io-wq worker) | io_cq_unlock_post | 0.94% |
| kernel (io-wq worker) | filemap_get_entry | 0.69% |

call-path confidence, as a share of the 15.5 G cycles with a kernel leaf (75.2% of the case): direct 91.39%, interrupt 7.95%, no-wrapper 0.65%

- **main (submit + consume)** — 69.4% of all cycles: kernel 55%, sluice 23%, kernel (interrupt/exception) 9%, libc pthread 5%, libc mem* 3%
- **io-wq kernel worker** — 30.6% of all cycles: kernel (io-wq worker) 100%

## w03-tp-4k-d16-w4  (total 52.2 G cycles)

| bucket | share |
|---|---|
| kernel | 74.1% |
| libc pthread | 17.5% |
| sluice | 4.7% |
| other | 1.7% |
| libc mem* | 0.9% |
| c++ runtime | 0.3% |
| kernel (syscall, wrapper unresolved) | 0.3% |
| bench harness | 0.2% |
| libc wrapper self | 0.1% |
| kernel (interrupt/exception) | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | futex_wake (mutex) | 20.97% |
| kernel | futex_wait (mutex) | 19.72% |
| kernel | pread | 10.74% |
| kernel | futex_wait (timed) | 8.81% |
| libc pthread | __pthread_mutex_unlock_usercnt | 6.73% |
| libc pthread | pthread_mutex_lock | 5.53% |
| kernel | eventfd write (completion notification) | 5.26% |
| kernel | futex_wake (condvar) | 4.80% |
| libc pthread | __lll_lock_wait | 1.91% |
| kernel | epoll_wait | 1.32% |
| kernel | eventfd read (notification drain) | 1.12% |
| libc pthread | pthread_cond_wait | 0.98% |
| libc mem* | __memcmp_avx2_movbe | 0.92% |
| sluice | sluice::async::detail::RequestCore::occupancy | 0.85% |
| sluice | sluice::async::ThreadPoolBackend::worker_loop | 0.74% |
| libc pthread | __lll_lock_wake | 0.55% |

call-path confidence, as a share of the 38.9 G cycles with a kernel leaf (74.4% of the case): direct 99.23%, gap 0.36%, no-wrapper 0.36%, interrupt 0.05%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.08% — entering frame `std::mutex::lock`, charged to `clone`
* 0.07% — entering frame `sluice::async::ThreadPoolBackend::worker_loop`, charged to `clone`
* 0.05% — entering frame `sluice::async::detail::RequestCore::claim_execution`, charged to `mutex_unlock (userspace)`
* 0.05% — entering frame `sluice::async::detail::ProgressSource::drain_notification_nolock_`, charged to `clone`
* 0.03% — entering frame `sluice::async::detail::ProgressSource::wake_notification_nolock_`, charged to `clone`

- **worker thread** — 76.0% of all cycles: kernel 78%, libc pthread 16%, sluice 3%, other 2%, libc wrapper self 0%
- **main (submit + consume)** — 24.0% of all cycles: kernel 61%, libc pthread 22%, sluice 9%, libc mem* 4%, other 1%

## w03-uring-4k-d16  (total 26.7 G cycles)

| bucket | share |
|---|---|
| kernel | 50.2% |
| kernel (io-wq worker) | 24.9% |
| libc pthread | 9.4% |
| sluice | 8.7% |
| libc mem* | 2.1% |
| other | 1.9% |
| kernel (interrupt/exception) | 1.7% |
| c++ runtime | 0.6% |
| bench harness | 0.4% |
| kernel (syscall, wrapper unresolved) | 0.2% |
| libc wrapper self | 0.0% |

| bucket | detail | share |
|---|---|---|
| kernel | io_uring_enter | 21.12% |
| kernel | futex_wake (mutex) | 9.05% |
| kernel | futex_wait (mutex) | 7.78% |
| kernel | futex_wait (timed) | 5.13% |
| kernel (io-wq worker) | rep_movs_alternative | 4.31% |
| libc pthread | __pthread_mutex_unlock_usercnt | 3.94% |
| libc pthread | pthread_mutex_lock | 3.63% |
| kernel | epoll_wait | 2.14% |
| kernel | eventfd read (notification drain) | 2.10% |
| libc mem* | __memcmp_avx2_movbe | 2.00% |
| kernel (io-wq worker) | _raw_spin_lock | 1.91% |
| kernel (io-wq worker) | shmem_file_read_iter | 1.55% |
| sluice | sluice::async::detail::RequestCore::occupancy | 1.27% |
| kernel (interrupt/exception) | asm_sysvec_reschedule_ipi | 1.21% |
| kernel (io-wq worker) | __raw_spin_lock_irqsave | 1.06% |
| kernel (io-wq worker) | native_queued_spin_lock_slowpath | 1.06% |

call-path confidence, as a share of the 20.5 G cycles with a kernel leaf (76.9% of the case): direct 97.30%, interrupt 2.16%, no-wrapper 0.32%, gap 0.22%

kernel cycles charged only after skipping an unrecognised frame (share of kernel-leaf cycles):

* 0.11% — entering frame `sluice::async::detail::ProgressSource::drain_notification_nolock_`, charged to `clone`
* 0.08% — entering frame `std::mutex::lock`, charged to `clone`
* 0.01% — entering frame `sluice::async::UringAsyncBackend::service_pending_controls_locked_`, charged to `mutex_unlock (userspace)`
* 0.00% — entering frame `start_thread`, charged to `clone`
* 0.00% — entering frame `epoll_ctl`, charged to `clone`

- **main (submit + consume)** — 54.0% of all cycles: kernel 69%, sluice 10%, libc pthread 9%, libc mem* 4%, kernel (interrupt/exception) 3%
- **io-wq kernel worker** — 24.9% of all cycles: kernel (io-wq worker) 100%
- **worker thread** — 21.1% of all cycles: kernel 61%, libc pthread 22%, sluice 15%, other 3%, c++ runtime 0%
