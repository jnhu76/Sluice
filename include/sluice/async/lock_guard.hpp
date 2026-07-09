// sluice::async::LockGuard — annotated RAII scoped-lock for Clang TSA.
//
// CPP-STATIC-1 substrate.  Wraps a Mutex reference with the TSA scoped
// capability annotation so the compiler understands acquire-on-construct /
// release-on-destruct.  Delegates to Mutex::lock/unlock; same exclusive
// non-recursive semantics; no additional runtime state.
//
// Non-copyable, non-movable: the lexical scope IS the lock lifetime.
//
// Use where the codebase currently uses std::lock_guard<std::mutex>.
// This is the simplest annotated lock form; TSA's unique_lock / cv patterns
// require a separate annotated type when needed.
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

}  // namespace sluice::async