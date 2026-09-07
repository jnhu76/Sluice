





#include <sluice/async/detail/fail_fast.hpp>

#include <sluice/async/fiber_ctx.hpp>

#include <atomic>
#include <exception>

namespace sluice::async::detail {

[[noreturn]] void async_mutex_lock_fail_fast() noexcept {
    std::terminate();
}




[[noreturn]] void select_timer_pump_active_fail_fast() noexcept {
    std::terminate();
}






[[noreturn]] void select_multi_group_event_stage_fail_fast() noexcept {
    std::terminate();
}





[[noreturn]] void select_invariant_fail_fast() noexcept {
    std::terminate();
}


[[noreturn]] void group_lifetime_fail_fast() noexcept {
    std::terminate();
}


[[noreturn]] void async_mutex_lifetime_fail_fast() noexcept {
    std::terminate();
}
[[noreturn]] void async_rwlock_lifetime_fail_fast() noexcept {
    std::terminate();
}
[[noreturn]] void async_condition_lifetime_fail_fast() noexcept {
    std::terminate();
}
[[noreturn]] void wait_queue_lifetime_fail_fast() noexcept {
    std::terminate();
}


[[noreturn]] void async_rwlock_recursive_write_fail_fast() noexcept {
    std::terminate();
}
[[noreturn]] void async_rwlock_unlock_write_inactive_fail_fast() noexcept {
    std::terminate();
}
[[noreturn]] void async_rwlock_unlock_write_not_owner_fail_fast() noexcept {
    std::terminate();
}


[[noreturn]] void scheduler_deferred_publication_stranded_fail_fast() noexcept {
    std::terminate();
}


[[noreturn]] void evented_admission_fail_fast() noexcept {
    std::terminate();
}







[[noreturn]] void async_context_outstanding_fail_fast() noexcept {
    std::terminate();
}




[[noreturn]] void scheduler_invalid_runnable_ticket_fail_fast() noexcept {
    std::terminate();
}



[[noreturn]] void scheduler_invalid_suspend_transition_fail_fast() noexcept {
    std::terminate();
}

[[noreturn]] void scheduler_missing_fiber_owner_fail_fast() noexcept {
    std::terminate();
}




[[noreturn]] void scheduler_wait_registry_invariant_fail_fast() noexcept {
    std::terminate();
}



[[noreturn]] void scheduler_wait_registry_nonempty_fail_fast() noexcept {
    std::terminate();
}





[[noreturn]] void completion_authority_fail_fast() noexcept {
    std::terminate();
}









[[noreturn]] void completion_binding_destruction_fail_fast() noexcept {
    std::terminate();
}
[[noreturn]] void completion_binding_reset_fail_fast() noexcept {
    std::terminate();
}







[[noreturn]] void request_slot_release_invariant_fail_fast() noexcept {
    std::terminate();
}






[[noreturn]] void request_arena_enqueue_state_fail_fast() noexcept {
    std::terminate();
}






[[noreturn]] void request_arena_destruction_fail_fast() noexcept {
    std::terminate();
}





[[noreturn]] void request_arena_missing_binding_fail_fast() noexcept {
    std::terminate();
}





[[noreturn]] void request_arena_terminal_state_fail_fast() noexcept {
    std::terminate();
}





[[noreturn]] void request_arena_enqueue_stale_fail_fast() noexcept {
    std::terminate();
}






[[noreturn]] void request_arena_generation_exhausted_fail_fast() noexcept {
    std::terminate();
}




[[noreturn]] void request_arena_slot_index_out_of_range_fail_fast() noexcept {
    std::terminate();
}





[[noreturn]] void request_arena_invalid_terminal_fail_fast() noexcept {
    std::terminate();
}




[[noreturn]] void request_arena_dispatch_state_fail_fast() noexcept {
    std::terminate();
}





[[noreturn]] void request_arena_dispatch_stale_fail_fast() noexcept {
    std::terminate();
}






[[noreturn]] void request_arena_ready_ring_invariant_fail_fast() noexcept {
    std::terminate();
}




[[noreturn]] void threadpool_non_quiescent_destruction_fail_fast() noexcept {
    std::terminate();
}






[[noreturn]] void uring_non_quiescent_destruction_fail_fast() noexcept {
    std::terminate();
}




#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
namespace {

std::atomic<int> g_evented_admission_override{-1};
}

bool evented_admission_check() noexcept {
    int ovr = g_evented_admission_override.load(std::memory_order_acquire);
    if (ovr >= 0) return ovr != 0;
    return fiber_ctx::supported;
}



void set_evented_admission_override_impl(bool supported) noexcept {
    g_evented_admission_override.store(supported ? 1 : 0, std::memory_order_release);
}
void clear_evented_admission_override_impl() noexcept {
    g_evented_admission_override.store(-1, std::memory_order_release);
}
bool get_evented_admission_override_impl() noexcept {
    int ovr = g_evented_admission_override.load(std::memory_order_acquire);
    return ovr >= 0 ? (ovr != 0) : fiber_ctx::supported;
}
#else
bool evented_admission_check() noexcept {
    return fiber_ctx::supported;
}
#endif

}
