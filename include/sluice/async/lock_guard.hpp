











#pragma once

#include <sluice/async/mutex.hpp>
#include <sluice/async/thread_annotations.hpp>

namespace sluice::async {

class SLUICE_SCOPED_CAPABILITY LockGuard {
public:
    explicit LockGuard(Mutex& mu) SLUICE_ACQUIRE(mu) : mu_(mu) { mu_.lock(); }
    ~LockGuard() SLUICE_RELEASE() { mu_.unlock(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
    LockGuard(LockGuard&&) = delete;
    LockGuard& operator=(LockGuard&&) = delete;

private:
    Mutex& mu_;
};

}