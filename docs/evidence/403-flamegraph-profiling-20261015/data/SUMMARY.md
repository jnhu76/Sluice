# Flame-graph self-cost summary

Cycle-weighted self cost per case, from the perf captures in `data/perf/`.
Each recorded repetition runs the workload three times (timed throughput
pass, correctness-oracle pass, instrumented latency pass), so the profile
covers all three.

## w01-read-4k-seq

total cycles in profile: 24,721,890,506

threads (root frame share):
- bench_w01_direc: 100.0%
- sh: 0.0%
- mkdir: 0.0%

top self cost (leaf frame share):
- rep_movs_alternative_[k]: 34.23%
- entry_SYSRETQ_unsafe_stack_[k]: 18.09%
- entry_SYSCALL_64_[k]: 5.22%
- syscall_return_via_sysret_[k]: 3.96%
- __memcmp_avx2_movbe: 3.04%
- shmem_file_read_iter_[k]: 2.36%
- __internal_syscall_cancel: 2.17%
- vfs_read_[k]: 1.58%
- entry_SYSCALL_64_safe_stack_[k]: 1.52%
- clear_highpages_kasan_tagged_[k]: 1.38%
- xas_load_[k]: 1.34%
- shmem_get_folio_gfp_[k]: 1.21%
- entry_SYSCALL_64_after_hwframe_[k]: 1.17%
- __memmove_avx_unaligned_erms: 1.16%
- filemap_get_entry_[k]: 1.14%
- do_syscall_64_[k]: 1.12%
- atime_needs_update_[k]: 0.93%
- operator: 0.75%
- _copy_to_iter_[k]: 0.74%
- copy_page_to_iter_[k]: 0.72%

## w01-sync-data-4k

total cycles in profile: 2,089,610,115

threads (root frame share):
- bench_w01_direc: 99.5%
- sh: 0.3%
- mkdir: 0.2%

top self cost (leaf frame share):
- clear_highpages_kasan_tagged_[k]: 17.03%
- __memmove_avx_unaligned_erms: 13.13%
- rep_movs_alternative_[k]: 12.67%
- try_charge_memcg_[k]: 3.31%
- __mod_memcg_lruvec_state_[k]: 1.76%
- get_mem_cgroup_from_mm_[k]: 1.28%
- mod_node_page_state_[k]: 1.28%
- __dquot_alloc_space_[k]: 1.11%
- xas_store_[k]: 1.10%
- lru_gen_del_folio.constprop.0_[k]: 1.08%
- __rcu_read_unlock_[k]: 1.03%
- rmqueue.isra.0_[k]: 1.02%
- _raw_spin_lock_[k]: 0.99%
- lru_gen_add_folio_[k]: 0.90%
- avc_lookup_[k]: 0.90%
- free_unref_folios_[k]: 0.81%
- xas_load_[k]: 0.79%
- lruvec_stat_mod_folio_[k]: 0.78%
- __rcu_read_lock_[k]: 0.76%
- __alloc_frozen_pages_noprof_[k]: 0.70%

## w01-write-4k-seq

total cycles in profile: 33,839,123,332

threads (root frame share):
- bench_w01_direc: 100.0%
- sh: 0.0%
- mkdir: 0.0%

top self cost (leaf frame share):
- rep_movs_alternative_[k]: 31.64%
- entry_SYSRETQ_unsafe_stack_[k]: 15.79%
- entry_SYSCALL_64_[k]: 4.76%
- syscall_return_via_sysret_[k]: 3.54%
- folio_unlock_[k]: 2.45%
- __memmove_avx_unaligned_erms: 2.28%
- __memcmp_avx2_movbe: 2.11%
- __internal_syscall_cancel: 2.04%
- xas_load_[k]: 1.80%
- vfs_write_[k]: 1.75%
- filemap_get_entry_[k]: 1.51%
- shmem_get_folio_gfp_[k]: 1.49%
- entry_SYSCALL_64_safe_stack_[k]: 1.34%
- generic_perform_write_[k]: 1.28%
- do_syscall_64_[k]: 1.26%
- clear_highpages_kasan_tagged_[k]: 1.19%
- entry_SYSCALL_64_after_hwframe_[k]: 1.19%
- rw_verify_area_[k]: 0.82%
- shmem_write_end_[k]: 0.82%
- shmem_write_begin_[k]: 0.64%

## w02-tp-4k-d16-w1

total cycles in profile: 27,144,907,881

threads (root frame share):
- bench_w02_pipel: 100.0%
- sh: 0.0%
- mkdir: 0.0%

top self cost (leaf frame share):
- entry_SYSRETQ_unsafe_stack_[k]: 18.16%
- pthread_mutex_lock@@GLIBC_2.2.5: 11.01%
- __GI___pthread_mutex_unlock_usercnt: 10.69%
- entry_SYSCALL_64_[k]: 5.65%
- rep_movs_alternative_[k]: 5.40%
- syscall_return_via_sysret_[k]: 4.49%
- __GI___lll_lock_wait: 2.01%
- sluice::async::detail::RequestCore::occupancy: 1.87%
- __memcmp_avx2_movbe: 1.85%
- do_syscall_64_[k]: 1.81%
- entry_SYSCALL_64_safe_stack_[k]: 1.51%
- native_queued_spin_lock_slowpath_[k]: 1.40%
- entry_SYSCALL_64_after_hwframe_[k]: 1.27%
- shmem_file_read_iter_[k]: 1.12%
- sluice::async::ThreadPoolBackend::worker_loop: 1.04%
- __futex_hash_[k]: 0.92%
- futex_wake_[k]: 0.90%
- __GI___lll_lock_wake: 0.73%
- __gthread_mutex_lock: 0.65%
- _raw_spin_lock_[k]: 0.64%

## w02-tp-4k-d16-w4

total cycles in profile: 74,425,907,623

threads (root frame share):
- bench_w02_pipel: 100.0%
- sh: 0.0%
- mkdir: 0.0%

top self cost (leaf frame share):
- entry_SYSRETQ_unsafe_stack_[k]: 17.66%
- __GI___pthread_mutex_unlock_usercnt: 6.17%
- entry_SYSCALL_64_[k]: 5.79%
- pthread_mutex_lock@@GLIBC_2.2.5: 5.77%
- syscall_return_via_sysret_[k]: 4.66%
- rep_movs_alternative_[k]: 3.92%
- native_queued_spin_lock_slowpath_[k]: 2.47%
- __GI___lll_lock_wait: 2.05%
- do_syscall_64_[k]: 1.85%
- entry_SYSCALL_64_safe_stack_[k]: 1.48%
- entry_SYSCALL_64_after_hwframe_[k]: 1.30%
- futex_wake_[k]: 1.18%
- __futex_hash_[k]: 1.18%
- sluice::async::detail::RequestCore::occupancy: 1.13%
- __memcmp_avx2_movbe: 1.02%
- _raw_spin_lock_[k]: 1.00%
- shmem_file_read_iter_[k]: 0.94%
- dequeue_entity_[k]: 0.87%
- sluice::async::ThreadPoolBackend::worker_loop: 0.81%
- pthread_cond_wait@@GLIBC_2.3.2: 0.79%

## w02-tp-4k-d16-w8

total cycles in profile: 51,133,436,142

threads (root frame share):
- bench_w02_pipel: 100.0%
- sh: 0.0%
- mkdir: 0.0%

top self cost (leaf frame share):
- entry_SYSRETQ_unsafe_stack_[k]: 16.85%
- entry_SYSCALL_64_[k]: 5.45%
- __GI___pthread_mutex_unlock_usercnt: 4.79%
- syscall_return_via_sysret_[k]: 4.65%
- native_queued_spin_lock_slowpath_[k]: 4.38%
- pthread_mutex_lock@@GLIBC_2.2.5: 4.29%
- rep_movs_alternative_[k]: 3.19%
- __GI___lll_lock_wait: 1.88%
- do_syscall_64_[k]: 1.85%
- entry_SYSCALL_64_safe_stack_[k]: 1.46%
- futex_wake_[k]: 1.26%
- pthread_cond_wait@@GLIBC_2.3.2: 1.20%
- entry_SYSCALL_64_after_hwframe_[k]: 1.20%
- _raw_spin_lock_[k]: 1.18%
- dequeue_entity_[k]: 1.11%
- __futex_hash_[k]: 1.11%
- psi_group_change_[k]: 0.96%
- sluice::async::detail::RequestCore::occupancy: 0.92%
- __memcmp_avx2_movbe: 0.83%
- __smp_call_single_queue_[k]: 0.75%

## w02-tp-64k-d16-w4

total cycles in profile: 7,651,867,405

threads (root frame share):
- bench_w02_pipel: 99.9%
- sh: 0.0%
- mkdir: 0.0%

top self cost (leaf frame share):
- rep_movs_alternative_[k]: 35.12%
- entry_SYSRETQ_unsafe_stack_[k]: 7.11%
- shmem_file_read_iter_[k]: 6.45%
- __memcmp_avx2_movbe: 6.20%
- filemap_get_entry_[k]: 3.95%
- __GI___pthread_mutex_unlock_usercnt: 3.15%
- pthread_mutex_lock@@GLIBC_2.2.5: 2.62%
- entry_SYSCALL_64_[k]: 2.13%
- shmem_get_folio_gfp_[k]: 1.89%
- syscall_return_via_sysret_[k]: 1.87%
- native_queued_spin_lock_slowpath_[k]: 1.47%
- xas_load_[k]: 1.13%
- do_syscall_64_[k]: 0.73%
- __GI___lll_lock_wait: 0.71%
- sluice::async::detail::RequestCore::occupancy: 0.62%
- copy_page_to_iter_[k]: 0.61%
- xas_start_[k]: 0.53%
- entry_SYSCALL_64_safe_stack_[k]: 0.53%
- __memmove_avx_unaligned_erms: 0.46%
- futex_wake_[k]: 0.46%

## w02-uring-4k-d16

total cycles in profile: 37,265,172,391

threads (root frame share):
- bench_w02_pipel: 65.3%
- iou-wrk-5908: 34.6%
- sh: 0.0%
- mkdir: 0.0%

top self cost (leaf frame share):
- rep_movs_alternative_[k]: 6.10%
- native_queued_spin_lock_slowpath_[k]: 5.21%
- _raw_spin_lock_[k]: 4.45%
- entry_SYSRETQ_unsafe_stack_[k]: 4.38%
- asm_sysvec_reschedule_ipi_[k]: 2.65%
- __memcmp_avx2_movbe: 2.34%
- shmem_file_read_iter_[k]: 2.18%
- tctx_task_work_run_[k]: 2.17%
- __irqentry_text_end_[k]: 1.83%
- sluice::async::detail::RequestCore::occupancy: 1.82%
- pthread_mutex_lock@@GLIBC_2.2.5: 1.76%
- __GI___pthread_mutex_unlock_usercnt: 1.45%
- io_eventfd_signal_[k]: 1.44%
- __raw_spin_lock_irqsave_[k]: 1.34%
- entry_SYSCALL_64_[k]: 1.31%
- try_to_wake_up_[k]: 1.22%
- io_cq_unlock_post_[k]: 1.21%
- io_prep_async_work_[k]: 1.08%
- syscall_return_via_sysret_[k]: 1.07%
- io_wq_enqueue_[k]: 1.02%

## w02-uring-4k-d64

total cycles in profile: 20,579,292,910

threads (root frame share):
- bench_w02_pipel: 69.3%
- iou-wrk-6526: 30.6%
- sh: 0.0%
- mkdir: 0.0%

top self cost (leaf frame share):
- native_queued_spin_lock_slowpath_[k]: 6.04%
- rep_movs_alternative_[k]: 4.95%
- sluice::async::detail::RequestCore::occupancy: 4.22%
- entry_SYSRETQ_unsafe_stack_[k]: 4.14%
- _raw_spin_lock_[k]: 3.95%
- asm_sysvec_reschedule_ipi_[k]: 2.56%
- __memcmp_avx2_movbe: 2.08%
- shmem_file_read_iter_[k]: 1.94%
- tctx_task_work_run_[k]: 1.89%
- sluice::async::UringAsyncBackend::service_pending_controls_locked_: 1.69%
- pthread_mutex_lock@@GLIBC_2.2.5: 1.64%
- __irqentry_text_end_[k]: 1.64%
- try_to_wake_up_[k]: 1.38%
- io_eventfd_signal_[k]: 1.36%
- __raw_spin_lock_irqsave_[k]: 1.33%
- entry_SYSCALL_64_[k]: 1.22%
- io_wq_enqueue_[k]: 1.17%
- io_prep_async_work_[k]: 1.14%
- __GI___pthread_mutex_unlock_usercnt: 1.11%
- sluice::async::RequestScope::Slot* sluice::async::RequestScope::resolve_<unsigned long>: 1.09%

## w03-tp-4k-d16-w4

total cycles in profile: 52,224,058,785

threads (root frame share):
- bench_w03_exter: 100.0%
- sh: 0.0%
- mkdir: 0.0%

top self cost (leaf frame share):
- entry_SYSRETQ_unsafe_stack_[k]: 16.37%
- __GI___pthread_mutex_unlock_usercnt: 6.73%
- pthread_mutex_lock@@GLIBC_2.2.5: 5.53%
- entry_SYSCALL_64_[k]: 5.15%
- syscall_return_via_sysret_[k]: 4.11%
- rep_movs_alternative_[k]: 4.04%
- native_queued_spin_lock_slowpath_[k]: 2.60%
- __GI___lll_lock_wait: 1.91%
- do_syscall_64_[k]: 1.64%
- futex_wake_[k]: 1.49%
- entry_SYSCALL_64_safe_stack_[k]: 1.31%
- _raw_spin_lock_[k]: 1.27%
- entry_SYSCALL_64_after_hwframe_[k]: 1.06%
- pthread_cond_wait@@GLIBC_2.3.2: 0.98%
- __futex_hash_[k]: 0.97%
- dequeue_entity_[k]: 0.96%
- __memcmp_avx2_movbe: 0.92%
- __smp_call_single_queue_[k]: 0.90%
- shmem_file_read_iter_[k]: 0.88%
- sluice::async::detail::RequestCore::occupancy: 0.85%

## w03-uring-4k-d16

total cycles in profile: 26,708,551,068

threads (root frame share):
- bench_w03_exter: 75.1%
- iou-wrk-7142: 24.9%
- sh: 0.0%
- mkdir: 0.0%

top self cost (leaf frame share):
- entry_SYSRETQ_unsafe_stack_[k]: 7.80%
- rep_movs_alternative_[k]: 4.34%
- native_queued_spin_lock_slowpath_[k]: 4.34%
- __GI___pthread_mutex_unlock_usercnt: 3.94%
- pthread_mutex_lock@@GLIBC_2.2.5: 3.63%
- _raw_spin_lock_[k]: 3.41%
- entry_SYSCALL_64_[k]: 2.64%
- syscall_return_via_sysret_[k]: 2.12%
- __memcmp_avx2_movbe: 2.00%
- __raw_spin_lock_irqsave_[k]: 1.55%
- shmem_file_read_iter_[k]: 1.55%
- tctx_task_work_run_[k]: 1.39%
- asm_sysvec_reschedule_ipi_[k]: 1.28%
- sluice::async::detail::RequestCore::occupancy: 1.27%
- __irqentry_text_end_[k]: 1.09%
- try_to_wake_up_[k]: 1.03%
- do_syscall_64_[k]: 1.00%
- io_eventfd_signal_[k]: 0.95%
- io_cq_unlock_post_[k]: 0.84%
- sluice::async::UringAsyncBackend::service_pending_controls_locked_: 0.81%
