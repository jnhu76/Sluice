#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>

#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

namespace sluice::async {

class Semaphore {
  public:
    using permit_count_t = std::uint32_t;

    Semaphore(Scheduler& scheduler, permit_count_t initial_permits,
              permit_count_t max_permits) noexcept
        : scheduler_(scheduler), available_(initial_permits), max_permits_(max_permits) {
        assert(max_permits > 0 && "Semaphore max_permits must be > 0");
        assert(initial_permits <= max_permits &&
               "Semaphore initial_permits must be <= max_permits");
    }

    ~Semaphore() = default;

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;
    Semaphore(Semaphore&&) = delete;
    Semaphore& operator=(Semaphore&&) = delete;

    [[nodiscard]] permit_count_t available() const noexcept {
        return available_.load(std::memory_order::acquire);
    }

    [[nodiscard]] bool try_acquire() { return scheduler_.sem_try_acquire(waiters_, available_); }

    void acquire(WaitNode& node) { scheduler_.sem_acquire(waiters_, available_, node); }

    void acquire_until(WaitNode& node, Scheduler::deadline_t deadline) {
        scheduler_.sem_acquire_until(waiters_, available_, node, deadline);
    }

    [[nodiscard]] bool cancel(WaitNode& node) { return scheduler_.sem_cancel(waiters_, node); }

    [[nodiscard]] bool release() {
        return scheduler_.sem_release(waiters_, available_, max_permits_);
    }

  private:
    Scheduler& scheduler_;
    std::atomic<permit_count_t> available_;
    const permit_count_t max_permits_;
    WaitQueue waiters_;
};

} // namespace sluice::async
