#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace sluice::async {

class WaitPolicy {
  public:
    virtual ~WaitPolicy() = default;
    WaitPolicy(const WaitPolicy&) = delete;
    WaitPolicy& operator=(const WaitPolicy&) = delete;

    virtual void wait_until_ready(const std::atomic<bool>& ready, std::mutex& mtx,
                                  std::condition_variable& cv) = 0;

    virtual void notify_ready() noexcept {}

  protected:
    WaitPolicy() = default;
};

class ThreadedWaitPolicy : public WaitPolicy {
  public:
    void wait_until_ready(const std::atomic<bool>& ready, std::mutex& mtx,
                          std::condition_variable& cv) override {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [&] { return ready.load(std::memory_order::acquire); });
    }
};

WaitPolicy& default_wait_policy() noexcept;

} // namespace sluice::async
