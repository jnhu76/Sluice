// sluice::async::Mutex — annotated std::mutex capability for Clang TSA.
//
// CPP-STATIC-1 substrate.  Wraps std::mutex with the Clang TSA capability
// annotation so the compiler can verify GUARDED_BY / REQUIRES contracts.
// Delegates to std::mutex; same exclusive non-recursive semantics; no
// additional runtime state; no lock-order policy; no debug ownership tracking.
//
// Composition (not inheritance): the underlying std::mutex is private.
//
// ---------------------------------------------------------------------------
// Failure policy (ASYNC-MUTEX-NOTHROW-AUTHORITY-1 §2/§3):
//
// lock() / try_lock() / unlock() are noexcept. An internal Mutex acquisition
// that participates in an authoritative Scheduler transition (or, by the
// Queue design, a winner CommitGap) must never let an underlying
// std::mutex::lock() / try_lock() failure (std::system_error) escape the
// boundary as a recoverable exception: the runtime cannot resume user
// execution after such a failure while preserving winner / ownership /
// queue-membership / publication invariants. Such a failure is therefore
// process-fatal (fail-fast via std::terminate).
//
// The explicit `catch (...) -> async_mutex_lock_fail_fast()` records the
// lock-boundary policy at the call site. `noexcept` is the language-level
// backstop that guarantees no exception can ever escape the boundary even if
// the catch were removed. Both are required by the authority; do not remove
// one without re-opening ASYNC-MUTEX-NOTHROW-AUTHORITY-1 — the catch documents
// intent and centralizes the fail-fast call, while noexcept is the
// non-negotiable runtime contract.
//
// A violated std::mutex::unlock() ownership precondition remains a program
// invariant failure (UB), not a recoverable error; the noexcept on unlock()
// documents that no recovery path exists there either.
// ---------------------------------------------------------------------------
#pragma once

#include <sluice/async/detail/fail_fast.hpp>
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
            impl_.lock();
        } catch (...) {
            detail::async_mutex_lock_fail_fast();
        }
    }
    bool try_lock() noexcept SLUICE_TRY_ACQUIRE(true) {
        try {
            return impl_.try_lock();
        } catch (...) {
            detail::async_mutex_lock_fail_fast();
        }
    }
    void unlock() noexcept SLUICE_RELEASE() { impl_.unlock(); }

private:
    std::mutex impl_;
};

}  // namespace sluice::async