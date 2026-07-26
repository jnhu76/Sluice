// Implementation of the Mutex acquisition fail-fast entry.
//
// Kept deliberately trivial to satisfy ASYNC-MUTEX-NOTHROW-AUTHORITY-1 §D2:
// no allocation, no locking, no I/O, no virtual / function-pointer call, no
// dynamic string, no Scheduler-state recovery. A single std::terminate() is
// the language-standard process terminator and is the most auditable shape.
#include <sluice/async/detail/fail_fast.hpp>

#include <sluice/async/fiber_ctx.hpp>

#include <atomic>
#include <exception>  // std::terminate

namespace sluice::async::detail {

[[noreturn]] void async_mutex_lock_fail_fast() noexcept {
    std::terminate();
}

// E13 P3 stage-boundary fail-fast: a due ACTIVE SelectTimerRegistration is
// unreachable in valid P3 production state. Terminates the process; never
// returns. See include/sluice/async/detail/fail_fast.hpp.
[[noreturn]] void select_timer_pump_active_fail_fast() noexcept {
    std::terminate();
}

// E13 P6 stage-boundary fail-fast: a single Event::set() broadcast observed
// arms belonging to more than one distinct eligible SelectGroup. Multi-group
// shared Event (P8) is DENIED at the P6 boundary; fail fast before any group
// winner CAS. Terminates; never returns. See include/sluice/async/detail/
// fail_fast.hpp.
[[noreturn]] void select_multi_group_event_stage_fail_fast() noexcept {
    std::terminate();
}

// E13 P5 CORRECTIVE: general-purpose Select invariant fail-fast. Called when
// the admission core receives a structurally invalid descriptor/count argument
// or encounters an unknown descriptor kind. Provides defense-in-depth against
// Release-mode memory safety violations. Terminates; never returns.
[[noreturn]] void select_invariant_fail_fast() noexcept {
    std::terminate();
}

// E14 D-E14-F2a: Group lifetime fail-fast. ~Group with pending Evented task.
[[noreturn]] void group_lifetime_fail_fast() noexcept {
    std::terminate();
}

// E14 D-E14-2: Evented admission fail-fast. Unsupported target.
[[noreturn]] void evented_admission_fail_fast() noexcept {
    std::terminate();
}

// E15-P1-03 / E15-P2-06: AsyncIoContext destroyed or move-assigned over while
// it still owns a backend with outstanding Completions. Per ADR §5 L11 this is
// a contract violation; the truthful deterministic behavior is fail-fast in
// BOTH Debug and Release (a destructor / move-assign has no Result channel for
// invalid_state, and silent abandonment would strand caller-owned Completions
// permanently outstanding). Mirrors group_lifetime_fail_fast.
[[noreturn]] void async_context_outstanding_fail_fast() noexcept {
    std::terminate();
}

// E14 D-E14-2: Evented admission check. Returns the effective fiber support
// status. Production: fiber_ctx::supported (compile-time constant). Internal-
// testing: may be overridden to simulate unsupported targets on x86_64.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
namespace {
// -1 = no override (use fiber_ctx::supported); 0 = force unsupported; 1 = force supported.
std::atomic<int> g_evented_admission_override{-1};
}  // namespace

bool evented_admission_check() noexcept {
    int ovr = g_evented_admission_override.load(std::memory_order_acquire);
    if (ovr >= 0) return ovr != 0;
    return fiber_ctx::supported;
}

// AsyncTestAccess entry points (declared in scheduler.hpp under the macro).
// Defined here because the override state lives in this TU.
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

}  // namespace sluice::async::detail
