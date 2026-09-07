#pragma once

namespace sluice::async::detail {

[[noreturn]] void async_mutex_lock_fail_fast() noexcept;

[[noreturn]] void select_timer_pump_active_fail_fast() noexcept;

[[noreturn]] void select_multi_group_event_stage_fail_fast() noexcept;

[[noreturn]] void select_invariant_fail_fast() noexcept;

[[noreturn]] void group_lifetime_fail_fast() noexcept;

[[noreturn]] void evented_admission_fail_fast() noexcept;

[[noreturn]] void async_context_outstanding_fail_fast() noexcept;

inline void require_evented_supported(bool supported) noexcept {
    if (!supported) {
        evented_admission_fail_fast();
    }
}

bool evented_admission_check() noexcept;

[[noreturn]] void scheduler_invalid_runnable_ticket_fail_fast() noexcept;

[[noreturn]] void scheduler_invalid_suspend_transition_fail_fast() noexcept;

[[noreturn]] void scheduler_missing_fiber_owner_fail_fast() noexcept;

[[noreturn]] void scheduler_wait_registry_invariant_fail_fast() noexcept;

[[noreturn]] void scheduler_wait_registry_nonempty_fail_fast() noexcept;

[[noreturn]] void completion_authority_fail_fast() noexcept;

[[noreturn]] void completion_binding_destruction_fail_fast() noexcept;
[[noreturn]] void completion_binding_reset_fail_fast() noexcept;

[[noreturn]] void request_slot_release_invariant_fail_fast() noexcept;

[[noreturn]] void request_arena_enqueue_state_fail_fast() noexcept;

[[noreturn]] void request_arena_destruction_fail_fast() noexcept;

[[noreturn]] void request_arena_missing_binding_fail_fast() noexcept;

[[noreturn]] void request_arena_terminal_state_fail_fast() noexcept;

[[noreturn]] void request_arena_enqueue_stale_fail_fast() noexcept;

[[noreturn]] void request_arena_generation_exhausted_fail_fast() noexcept;

[[noreturn]] void request_arena_slot_index_out_of_range_fail_fast() noexcept;

[[noreturn]] void request_arena_invalid_terminal_fail_fast() noexcept;

[[noreturn]] void request_arena_dispatch_state_fail_fast() noexcept;

[[noreturn]] void request_arena_dispatch_stale_fail_fast() noexcept;

[[noreturn]] void request_arena_ready_ring_invariant_fail_fast() noexcept;

[[noreturn]] void threadpool_non_quiescent_destruction_fail_fast() noexcept;

[[noreturn]] void async_mutex_lifetime_fail_fast() noexcept;
[[noreturn]] void async_rwlock_lifetime_fail_fast() noexcept;
[[noreturn]] void async_condition_lifetime_fail_fast() noexcept;
[[noreturn]] void wait_queue_lifetime_fail_fast() noexcept;

[[noreturn]] void async_rwlock_recursive_write_fail_fast() noexcept;

[[noreturn]] void async_rwlock_unlock_write_inactive_fail_fast() noexcept;

[[noreturn]] void async_rwlock_unlock_write_not_owner_fail_fast() noexcept;

[[noreturn]] void scheduler_deferred_publication_stranded_fail_fast() noexcept;

[[noreturn]] void uring_non_quiescent_destruction_fail_fast() noexcept;

} // namespace sluice::async::detail
