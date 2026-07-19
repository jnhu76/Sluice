// src/async/mutex_test_seam.cpp
//
// Implementation of the internal-testing failure-injection seam for
// sluice::async::Mutex. The ENTIRE translation unit is gated on
// SLUICE_ASYNC_INTERNAL_TESTING: under the production `sluice_async` target
// (the macro is undefined) this file compiles to an empty TU, so the
// production archive carries NO injection symbol. Only the
// `sluice_async_internal_testing` variant (which defines the macro, PUBLIC)
// compiles the real fault state.
//
// The fault state is process-global atomics with memory_order_relaxed. This is
// correct because (a) these counters exist ONLY to count injected test faults
// for a single death-test child, and (b) they carry NO production
// synchronization meaning — the production Mutex has no notion of a fault
// counter. Relaxed ordering is sufficient: the death-test harness forks a
// fresh child per case, and the child sets the countdown before the relevant
// acquisition on the same thread.
//
// The thrown exception is a dedicated empty final type (InjectedMutexFailure),
// not std::system_error: it has no error_code, no message, no allocation, and
// unambiguously exercises the production `catch (...)` boundary.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <sluice/async/detail/mutex_test_seam.hpp>

#include <atomic>

namespace sluice::async::detail {

// Dedicated test-fault exception. Thrown ONLY by the seam, caught ONLY by the
// production Mutex catch (...) boundary, which converts it to fail-fast.
struct InjectedMutexFailure final {};

namespace {

// Test-fault counters ONLY. memory_order_relaxed: see file header.
std::atomic<unsigned> g_lock_countdown{0};
std::atomic<bool>     g_fail_next_try_lock{false};

}  // namespace

void maybe_inject_mutex_failure(MutexTestOperation op) {
    if (op == MutexTestOperation::lock) {
        // Countdown semantics (see mutex_test_seam.hpp):
        //   0           : disabled, this lock succeeds.
        //   1           : this lock throws, countdown becomes 0.
        //   > 1         : this lock succeeds, countdown decrements.
        unsigned prev = g_lock_countdown.load(std::memory_order_relaxed);
        for (;;) {
            if (prev == 0) return;
            unsigned next = (prev == 1) ? 0 : (prev - 1);
            if (g_lock_countdown.compare_exchange_weak(
                    prev, next, std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                break;
            }
        }
        if (prev == 1) throw InjectedMutexFailure{};
        // prev > 1: this lock succeeds (countdown already decremented).
    } else {  // MutexTestOperation::try_lock
        if (g_fail_next_try_lock.exchange(false,
                                          std::memory_order_relaxed)) {
            throw InjectedMutexFailure{};
        }
    }
}

namespace test_hooks {

void arm_lock_countdown(unsigned n) noexcept {
    g_lock_countdown.store(n, std::memory_order_relaxed);
}

void arm_next_try_lock_fail() noexcept {
    g_fail_next_try_lock.store(true, std::memory_order_relaxed);
}

void disarm() noexcept {
    g_lock_countdown.store(0, std::memory_order_relaxed);
    g_fail_next_try_lock.store(false, std::memory_order_relaxed);
}

}  // namespace test_hooks

}  // namespace sluice::async::detail

#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)
