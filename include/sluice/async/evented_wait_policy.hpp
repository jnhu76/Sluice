// sluice::async::EventedWaitPolicy — Evented Future wait via the Scheduler's
// level-triggered ready-flag protocol (sluice-CORE-E5-A2).
//
// The E0 ADR §2/§9.1 contract: an Evented logical wait must suspend the current
// user task (Fiber) and return the worker to the scheduler, NOT block the OS
// thread. This policy implements that by delegating to
// Scheduler::await_ready_flag (E5-A1), which registers &ready as a waitable
// identity and suspends the current Fiber until the Scheduler observes the
// persistent flag true.
//
// This is the Evented counterpart to ThreadedWaitPolicy (which blocks the OS
// thread on a condition variable). Both implement the same WaitPolicy seam.
//
// What this commit does:
//   - adds EventedWaitPolicy, which ignores the mutex/cv (those are the
//     Threaded mechanism) and calls scheduler_.await_ready_flag(ready).
//
// What this commit does NOT do (M3 contract — no Future change):
//   - does NOT change Future state (Future is unchanged).
//   - does NOT change Future::complete_with (it already persists ready_).
//   - does NOT change the WaitPolicy virtual signature.
//   - does NOT add a Future waiter slot.
//   - does NOT add a wake callback.
//   - does NOT lock the Future's mutex during suspension.
//   - does NOT add TLS or global Scheduler state.
//
// Pre for wait_until_ready: the calling code is running inside a Fiber driven
// by `scheduler_` (i.e. Scheduler::current_ is set). Calling EventedWaitPolicy
// from outside a Scheduler-driven Fiber is a contract violation (use
// ThreadedWaitPolicy instead).
#pragma once

#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_policy.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace sluice::async {

class EventedWaitPolicy final : public WaitPolicy {
public:
    // Borrow the scheduler that drives Fibers using this policy. The scheduler
    // must outlive any Future that uses this policy. Acquires a
    // SchedulerWakeHandle at construction for external-producer notification
    // (E14 D-E14-1, T-WAKE-1: policy-level, not per-Future/per-registration).
    explicit EventedWaitPolicy(Scheduler& scheduler) noexcept
        : scheduler_(scheduler), wake_handle_(scheduler.make_wake_handle()) {}

    // The physical Evented wait: suspend the current Fiber on the persistent
    // `ready` flag via the Scheduler's level-triggered protocol. The mutex and
    // condition_variable are IGNORED (they are the Threaded mechanism; the
    // Evented model does not block the OS thread).
    void wait_until_ready(const std::atomic<bool>& ready,
                          std::mutex& /*mtx*/,
                          std::condition_variable& /*cv*/) override {
        scheduler_.await_ready_flag(ready);
    }

    // E14 D-E14-1 (T-WAKE-5): producer notification. Called by
    // Future::complete_with AFTER the first terminal publication, OUTSIDE the
    // Future's mutex. Wakes a parked Scheduler Worker so it re-drains and
    // routes the now-ready Fiber via wake_ready_flags_locked.
    // T-WAKE-7: safe across Scheduler destruction (the handle's control block
    // outlives the Scheduler; a late notify is a harmless no-op).
    void notify_ready() noexcept override {
        wake_handle_.notify();
    }

private:
    Scheduler& scheduler_;
    SchedulerWakeHandle wake_handle_;  // T-WAKE-1/2: policy-level, acquired at
                                       // construction, valid for policy lifetime.
};

}  // namespace sluice::async
