// sluice::async::Mutex — annotated std::mutex capability for Clang TSA.
//
// CPP-STATIC-1 substrate.  Wraps std::mutex with the Clang TSA capability
// annotation so the compiler can verify GUARDED_BY / REQUIRES contracts.
// Delegates to std::mutex; same exclusive non-recursive semantics; no
// additional runtime state; no lock-order policy; no debug ownership tracking.
//
// Composition (not inheritance): the underlying std::mutex is private.
#pragma once

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

    void lock() SLUICE_ACQUIRE() { impl_.lock(); }
    bool try_lock() SLUICE_TRY_ACQUIRE(true) { return impl_.try_lock(); }
    void unlock() SLUICE_RELEASE() { impl_.unlock(); }

private:
    std::mutex impl_;
};

}  // namespace sluice::async