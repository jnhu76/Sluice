#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>

#include <sluice/async/async_mutex.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

namespace sluice::async {

class AsyncCondition {
  public:
    explicit AsyncCondition(AsyncMutex& mutex) noexcept
        : mutex_(mutex), scheduler_(mutex.scheduler_) {}

    ~AsyncCondition() {
        if (active_waits_.load(std::memory_order::acquire) != 0) {
            assert(active_waits_.load(std::memory_order::acquire) == 0 &&
                   "AsyncCondition destroyed while a wait() call is in flight "
                   "(Condition epoch OR reacquire epoch)");
            detail::async_condition_lifetime_fail_fast();
        }
    }

    AsyncCondition(const AsyncCondition&) = delete;
    AsyncCondition& operator=(const AsyncCondition&) = delete;
    AsyncCondition(AsyncCondition&&) = delete;
    AsyncCondition& operator=(AsyncCondition&&) = delete;

    [[nodiscard]] WaitOutcome wait(WaitNode& condition_node);

    [[nodiscard]] WaitOutcome wait_until(WaitNode& condition_node, Scheduler::deadline_t deadline);

    [[nodiscard]] bool cancel(WaitNode& condition_node);

    void notify_one();

    void notify_all();

  private:
    friend class Scheduler;

    AsyncMutex& mutex_;
    Scheduler& scheduler_;
    WaitQueue waiters_;

    std::atomic<std::size_t> active_waits_{0};

    struct ActiveWaitGuard {
        std::atomic<std::size_t>& cnt;
        explicit ActiveWaitGuard(std::atomic<std::size_t>& c) noexcept : cnt(c) {
            cnt.fetch_add(1, std::memory_order::acq_rel);
        }
        ~ActiveWaitGuard() noexcept { cnt.fetch_sub(1, std::memory_order::acq_rel); }
        ActiveWaitGuard(const ActiveWaitGuard&) = delete;
        ActiveWaitGuard& operator=(const ActiveWaitGuard&) = delete;
    };
};

inline WaitOutcome AsyncCondition::wait(WaitNode& condition_node) {
    ActiveWaitGuard guard(active_waits_);

    bool released_mutex = false;
    WaitOutcome reason = scheduler_.condition_wait_prepare(
        waiters_, condition_node, mutex_.waiters_, mutex_.owner_, released_mutex);

    if (!released_mutex) {
        return reason;
    }

    WaitNode reacquire_node;
    mutex_.lock(reacquire_node);

    return reason;
}

inline WaitOutcome AsyncCondition::wait_until(WaitNode& condition_node,
                                              Scheduler::deadline_t deadline) {
    ActiveWaitGuard guard(active_waits_);
    bool released_mutex = false;
    WaitOutcome reason = scheduler_.condition_wait_prepare_until(
        waiters_, condition_node, mutex_.waiters_, mutex_.owner_, deadline, released_mutex);

    if (!released_mutex) {
        return reason;
    }
    WaitNode reacquire_node;
    mutex_.lock(reacquire_node);
    return reason;
}

inline bool AsyncCondition::cancel(WaitNode& condition_node) {
    return scheduler_.condition_cancel_wait(waiters_, condition_node);
}

inline void AsyncCondition::notify_one() {
    scheduler_.condition_notify_one(waiters_);
}

inline void AsyncCondition::notify_all() {
    scheduler_.condition_notify_all(waiters_);
}

} // namespace sluice::async
