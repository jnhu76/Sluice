#pragma once

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/mutex_test_seam.hpp>
#include <sluice/async/thread_annotations.hpp>

#include <mutex>

namespace sluice::async {

class SLUICE_CAPABILITY("mutex") Mutex {
  public:
    Mutex() noexcept = default;
    ~Mutex() = default;

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    void lock() noexcept SLUICE_ACQUIRE() {
        try {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

            detail::maybe_inject_mutex_failure(detail::MutexTestOperation::lock);
#endif
            impl_.lock();
        } catch (...) {
            detail::async_mutex_lock_fail_fast();
        }
    }
    bool try_lock() noexcept SLUICE_TRY_ACQUIRE(true) {
        try {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            detail::maybe_inject_mutex_failure(detail::MutexTestOperation::try_lock);
#endif
            return impl_.try_lock();
        } catch (...) {
            detail::async_mutex_lock_fail_fast();
        }
    }
    void unlock() noexcept SLUICE_RELEASE() { impl_.unlock(); }

  private:
    std::mutex impl_;
};

} // namespace sluice::async
