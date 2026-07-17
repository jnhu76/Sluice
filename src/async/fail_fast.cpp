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

}  // namespace sluice::async::detail
