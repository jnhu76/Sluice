#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <sluice/async/detail/mutex_test_seam.hpp>

#include <atomic>

namespace sluice::async::detail {

struct InjectedMutexFailure final {};

namespace {

std::atomic<unsigned> g_lock_countdown{0};
std::atomic<bool> g_fail_next_try_lock{false};

} // namespace

void maybe_inject_mutex_failure(MutexTestOperation op) {
    if (op == MutexTestOperation::lock) {
        unsigned prev = g_lock_countdown.load(std::memory_order_relaxed);
        for (;;) {
            if (prev == 0)
                return;
            unsigned next = (prev == 1) ? 0 : (prev - 1);
            if (g_lock_countdown.compare_exchange_weak(prev, next, std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
                break;
            }
        }
        if (prev == 1)
            throw InjectedMutexFailure{};

    } else {
        if (g_fail_next_try_lock.exchange(false, std::memory_order_relaxed)) {
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

} // namespace test_hooks

} // namespace sluice::async::detail

#endif
