// sluice::async::WaitPolicy — the physical-wait seam (sluice-CORE-E0A).
//
// Required by the E0 ADR (docs/adr/ADR-execution-model.md §3): the public task
// contract is LOGICAL control-flow waiting ("await = wait until terminal
// completion"). The PHYSICAL mechanism (block-the-thread vs suspend-a-fiber)
// is strategy-determined and must NOT be embedded in the task abstractions.
//
// This seam is where Future<T> (and via composition, Group/Batch) delegates the
// physical wait. The default Threaded policy blocks the calling thread on a
// condition variable — identical to the pre-E0A behavior, so all existing
// tests pass unchanged. A future Evented policy (job E5) will replace the wait
// with a fiber yield through the scheduler; the Future's state, result,
// idempotency, and cancel contracts are unchanged.
//
// Layering: ABOVE cancel.hpp / Future. No scheduler dependency in the seam
// itself — the Evented policy brings the scheduler; the seam is pure interface.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace sluice::async {

// The physical-wait seam. A Future delegates "wait until ready" here.
//
// The state passed in (ready flag + mutex + cv) is OWNED by the Future (the
// state is strategy-independent). The policy only decides HOW to wait for the
// ready flag to flip — blocking the thread (Threaded) or yielding the fiber
// (Evented, E5). The Threaded default uses the mutex+cv; an Evented policy may
// ignore them and yield, but it must still observe `ready` becoming true under
// the memory model the Future publishes (release on complete_with, acquire on
// ready check).
class WaitPolicy {
public:
    virtual ~WaitPolicy() = default;
    WaitPolicy(const WaitPolicy&) = delete;
    WaitPolicy& operator=(const WaitPolicy&) = delete;

    // Block the current execution context until `ready` is true, then return.
    // Called by Future::await. Implementations own the physical mechanism.
    //
    // Pre: the caller holds no lock on `mtx` (the Threaded policy locks it
    // internally; an Evented policy may not use it at all). Idempotent in the
    // sense that returning with `ready` already true is a no-op.
    virtual void wait_until_ready(const std::atomic<bool>& ready,
                                  std::mutex& mtx,
                                  std::condition_variable& cv) = 0;

protected:
    WaitPolicy() = default;
};

// The default Threaded policy: blocks the calling OS thread on a condition
// variable until `ready` is true. This is the portable baseline (E0 ADR §6).
// Identical to the pre-E0A Future::await behavior, so existing tests pass.
class ThreadedWaitPolicy : public WaitPolicy {
public:
    void wait_until_ready(const std::atomic<bool>& ready,
                          std::mutex& mtx,
                          std::condition_variable& cv) override {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [&] { return ready.load(std::memory_order::acquire); });
    }
};

// Process-wide default policy (Threaded). Future<T> uses this when no policy is
// injected. Returned by reference so it has a stable address.
WaitPolicy& default_wait_policy() noexcept;

}  // namespace sluice::async
