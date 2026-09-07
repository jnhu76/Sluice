






































































#pragma once

#include <cassert>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

namespace sluice::async {







class AsyncMutex {
public:



    explicit AsyncMutex(Scheduler& scheduler) noexcept
        : scheduler_(scheduler), owner_(nullptr) {}







    ~AsyncMutex() {



        if (owner_ != nullptr) {
            assert(owner_ == nullptr &&
                   "AsyncMutex destroyed while locked (owner != NoOwner)");
            detail::async_mutex_lifetime_fail_fast();
        }
    }

    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;
    AsyncMutex(AsyncMutex&&) = delete;
    AsyncMutex& operator=(AsyncMutex&&) = delete;













    [[nodiscard]] bool try_lock() {
        return scheduler_.mutex_try_lock(waiters_, owner_);
    }














    void lock(WaitNode& node) {
        scheduler_.mutex_lock(waiters_, owner_, node);
    }




















    void lock_until(WaitNode& node, Scheduler::deadline_t deadline) {
        scheduler_.mutex_lock_until(waiters_, owner_, node, deadline);
    }























    [[nodiscard]] bool cancel(WaitNode& node) {
        return scheduler_.mutex_cancel(waiters_, node);
    }












    void unlock() {
        scheduler_.mutex_unlock(waiters_, owner_);
    }

private:











    friend class AsyncCondition;

    Scheduler& scheduler_;
    Fiber* owner_;
    WaitQueue waiters_;
};

}
