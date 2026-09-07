#pragma once

#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_policy.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace sluice::async {

class EventedWaitPolicy final : public WaitPolicy {
  public:
    explicit EventedWaitPolicy(Scheduler& scheduler) noexcept
        : scheduler_(scheduler), wake_handle_(scheduler.make_wake_handle()) {}

    void wait_until_ready(const std::atomic<bool>& ready, std::mutex&,
                          std::condition_variable&) override {
        scheduler_.await_ready_flag(ready);
    }

    void notify_ready() noexcept override { wake_handle_.notify(); }

  private:
    Scheduler& scheduler_;
    SchedulerWakeHandle wake_handle_;
};

} // namespace sluice::async
