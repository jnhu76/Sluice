#pragma once

#include <sluice/async/cancel.hpp>
#include <sluice/async/wait_policy.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace sluice::async {

template <class T> class Future {
  public:
    Future() : policy_(&default_wait_policy()) {}
    explicit Future(WaitPolicy& policy) : policy_(&policy) {}

    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) = delete;
    Future& operator=(Future&&) = delete;

    void complete_with(Result<T> r) {
        bool first_publication = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (ready_)
                return;
            result_.emplace(std::move(r));
            ready_.store(true, std::memory_order::release);
            first_publication = true;
        }
        cv_.notify_all();

        if (first_publication) {
            policy_->notify_ready();
        }
    }

    CancelToken& cancel_token() noexcept { return token_; }

    Result<T> await() {
        if (!ready_.load(std::memory_order::acquire)) {
            policy_->wait_until_ready(ready_, mtx_, cv_);
        }
        return *result_;
    }

    Result<T> cancel() {
        token_.request();
        return await();
    }

    bool ready() const noexcept { return ready_.load(std::memory_order::acquire); }

  private:
    WaitPolicy* policy_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;

    std::optional<Result<T>> result_;
    std::atomic<bool> ready_{false};
    CancelToken token_;
};

} // namespace sluice::async
