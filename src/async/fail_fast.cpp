// Implementation of the Mutex acquisition fail-fast entry.
//
// Kept deliberately trivial to satisfy ASYNC-MUTEX-NOTHROW-AUTHORITY-1 §D2:
// no allocation, no locking, no I/O, no virtual / function-pointer call, no
// dynamic string, no Scheduler-state recovery. A single std::terminate() is
// the language-standard process terminator and is the most auditable shape.
#include <sluice/async/detail/fail_fast.hpp>

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

// E13 P5 stage-boundary fail-fast: an inline-only Select admission reached the
// no-ready snapshot. Suspended completion is P6 (denied at this boundary), so
// the inline path fails fast rather than returning a no-winner result or
// unwinding a frame with live Event/Timer authority. Terminates; never returns.
// See include/sluice/async/detail/fail_fast.hpp.
[[noreturn]] void select_admission_no_ready_fail_fast() noexcept {
    std::terminate();
}

}  // namespace sluice::async::detail
