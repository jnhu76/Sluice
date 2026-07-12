// sluice::async::Semaphore — async counting Semaphore (sluice-CORE-E12-B).
//
// The second user-facing async synchronization primitive built on the closed
// E10/E11/E12-A wait substrate. A counting Semaphore composes:
//
//   - an std::atomic<permit_count_t> available_  (stored, unassigned permits;
//     atomic ONLY to support lock-free observation via available())
//   - a private E10 WaitQueue waiters_           (intrusive FIFO of waiters)
//   - a const permit_count_t max_permits_        (capacity bound)
//
// WITHOUT duplicating the Scheduler coordination domain, WITHOUT a Semaphore-
// private wake channel, WITHOUT permit-ownership tracking, WITHOUT grant-in-
// flight / refund state, and WITHOUT direct Fiber manipulation.
//
// Semantic model (docs/e12-semaphore.md, docs/spec/e12_semaphore/):
//
//   available_ in [0, max_permits_]
//   Permit conservation law (history counters exist only in the formal model):
//       available + acquiredCount == initialPermits + acceptedReleaseCount
//
//   A release() call creates ONE pending permit that is exactly-one-of:
//     - transferred to the FIFO head waiter (available_ unchanged)
//     - stored into available_               (available_++)
//     - rejected because available_ == max_permits_ AND queue is empty (no
//       mutation, release returns false)
//
// Synchronization domain: ALL authoritative decisions occur under
// Scheduler::global_mtx_ -> Semaphore waiters_.mtx() (the existing coordination
// domain; same lock order as E10/E11/E12-A). `available_` is atomic ONLY so
// available() can observe it lock-free; it does NOT authorize lock-free
// acquisition. No separate Semaphore state mutex is added.
//
// SEALED PUBLIC AUTHORITY (mirrors E12-A-EVENT-CORRECTIVE-1 F-EVENT-AUTH). The
// Semaphore's private WaitQueue is NOT publicly reachable: there is no
// wait_queue() accessor on the production-public surface, and no test friend
// grants access to it. The ONLY RESOURCE_WAKE authorities on a Semaphore queue
// are release() (FIFO head transfer) and admission observing an admissible
// permit (inline Woken). Cancellation routes through Scheduler::sem_cancel on
// this Semaphore's private WaitQueue WITHOUT exposing it. Ordinary downstream
// code CANNOT call wake_wait_one on a Semaphore (the required bypass
// `scheduler.wake_wait_one(sem.wait_queue())` does not compile). Deterministic
// test phase seams are reached ONLY through the internal-testing runtime
// variant (`sluice_async_internal_testing`); the production `sluice_async`
// target declares no test friend and exports no test phase symbol.
//
// Scheduler binding: Semaphore borrows Scheduler& for its lifetime. release()
// may be called from an external OS thread; wait resolution reaches that
// Scheduler's waiting_waitq_count_, runnable routing, wake source, and deadline
// service — exactly the E12-A Event external-thread path.
//
// Destruction contract: destroying a Semaphore while wait epochs remain
// registered is a CALLER CONTRACT VIOLATION. The destructor does NOT
// cancel/wake/synthesize RESOURCE_WAKE and does NOT release stored permits.
// Caller must ensure all waits are terminal before Semaphore lifetime ends. A
// debug assertion (~WaitQueue's existing assert) enforces this. No cancel-all,
// no wake-all.
//
// Constructor preconditions (caller contract):
//   max_permits > 0
//   initial_permits <= max_permits
// Violation is a debug assertion (no exception, no Result factory).
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>

#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

namespace sluice::async {

// An async counting Semaphore.
//
// Non-copyable AND non-movable: the WaitQueue is non-movable (intrusive list
// with pointer identity), and the Semaphore borrows Scheduler& (identity
// matters for wait resolution routing). A Semaphore is constructed once and
// lives at one address for its lifetime.
class Semaphore {
public:
    using permit_count_t = std::uint32_t;

    // Construct a Semaphore bound to `scheduler` with `initial_permits` stored
    // permits and capacity `max_permits`.
    //
    // Preconditions (caller contract):
    //   max_permits > 0
    //   initial_permits <= max_permits
    // Violation is a debug assertion; release is unchecked (no exception, no
    // Result factory). The Scheduler must outlive the Semaphore.
    Semaphore(Scheduler& scheduler, permit_count_t initial_permits,
              permit_count_t max_permits) noexcept
        : scheduler_(scheduler),
          available_(initial_permits),
          max_permits_(max_permits) {
        assert(max_permits > 0 && "Semaphore max_permits must be > 0");
        assert(initial_permits <= max_permits &&
               "Semaphore initial_permits must be <= max_permits");
    }

    // Destruction contract: all Semaphore waits must be terminal / unregistered
    // before the Semaphore is destroyed. The destructor does NOT cancel waiters,
    // does NOT wake waiters, does NOT release stored permits, and does NOT
    // synthesize RESOURCE_WAKE. The underlying ~WaitQueue asserts empty in
    // debug (caller must drain first). In release builds, no recovery/cancel-
    // all protocol is required.
    ~Semaphore() = default;

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;
    Semaphore(Semaphore&&) = delete;
    Semaphore& operator=(Semaphore&&) = delete;

    // An OBSERVATIONAL atomic snapshot of stored, unassigned permits. The value
    // may become stale immediately; a positive value does NOT guarantee that a
    // later try_acquire succeeds (an eligible queued waiter may have FIFO
    // priority, or the permit may be consumed first). Lock-free acquire load.
    // NOT used as an internal admission oracle.
    [[nodiscard]] permit_count_t available() const noexcept {
        return available_.load(std::memory_order::acquire);
    }

    // Attempt to acquire one permit WITHOUT suspending. In one authoritative
    // critical section (under global_mtx_ + waiters_.mtx()):
    //   if available_ > 0 AND no eligible queued waiter has FIFO priority:
    //       available_--
    //       return true
    //   otherwise:
    //       return false  (no mutation)
    // No barging: a queued waiter with FIFO priority cannot be bypassed. NOT a
    // standalone atomic decrement. NOT a zero-duration timed acquire (no
    // deadline).
    [[nodiscard]] bool try_acquire() {
        return scheduler_.sem_try_acquire(waiters_, available_);
    }

    // Acquire one permit, suspending the calling Fiber if none is immediately
    // admissible. `node` must be Detached (fresh) and outlive this call's
    // suspend until it is resumed. The admission closure closes the lost-wake
    // window:
    //   - immediately admissible permit (available_ > 0 AND no eligible queued
    //     waiter): consume one stored permit, resolve the epoch Woken inline,
    //     do NOT suspend.
    //   - otherwise: register on this Semaphore's private FIFO WaitQueue,
    //     recheck resource admission under the authoritative locks, consume +
    //     resolve inline if a permit appeared, commit suspension only if still
    //     not admissible.
    // Outcome is read from `node` (Woken / Cancelled). Returns void.
    void acquire(WaitNode& node) {
        scheduler_.sem_acquire(waiters_, available_, node);
    }

    // Deadline-aware acquire. Composes acquire() with E11 TimerRegistration.
    // The wait resolves when EXACTLY ONE cause wins the resolve_ CAS:
    //   - release() transfer         -> Woken   (RESOURCE_WAKE)
    //   - cancel(node)               -> Cancelled (CANCEL)
    //   - the deadline elapsing       -> Expired  (TIMER_EXPIRE)
    //
    // Mandatory precedence (A4):
    //   1. authoritative permit admission
    //   2. already-due deadline
    //   3. timed registration and normal race
    // Exact results at admission (under the authoritative locks):
    //   permit admissible + deadline already due -> Woken
    //   no permit admissible + deadline already due -> Expired
    //   permit admissible + future deadline -> Woken
    //   no permit admissible + future deadline -> register and wait
    // For a registered timed wait, RESOURCE_WAKE / TIMER_EXPIRE / CANCEL compete
    // through the existing exactly-once WaitNode resolution authority. Reuses
    // E11 timer registration/retirement/unlinking/publication. No Semaphore-
    // local deadline mechanism. Outcome is read from `node`. Returns void.
    void acquire_until(WaitNode& node, Scheduler::deadline_t deadline) {
        scheduler_.sem_acquire_until(waiters_, available_, node, deadline);
    }

    // Narrow per-wait-epoch CANCELLATION authority (mirrors Event::cancel).
    // Resolves `node` with Cancelled through the Scheduler's cancellation path
    // on THIS Semaphore's private WaitQueue, WITHOUT exposing that queue.
    //
    // Contract (returns true ONLY if ALL hold):
    //   - node is currently Registered, AND
    //   - node is currently linked in THIS Semaphore's private WaitQueue, AND
    //   - CANCEL wins node.resolve_(Cancelled).
    // Otherwise returns false WITHOUT mutation. This includes:
    //   detached node                      -> false
    //   already Woken                      -> false
    //   already Cancelled                  -> false
    //   already Expired                    -> false
    //   live node in another Semaphore     -> false (same OR different Scheduler)
    //
    // Wrong-Semaphore cancellation is a safe false-return case (NOT UB, NOT a
    // debug assertion). The membership check scans THIS Semaphore's own queue
    // under this Scheduler's global_mtx_ + this Semaphore's waiters_.mtx(); it
    // never reads a foreign node's home_ and never locks a foreign
    // Semaphore/Scheduler, so cross-Scheduler wrong-Semaphore cancel is
    // synchronized and structurally safe.
    [[nodiscard]] bool cancel(WaitNode& node) {
        return scheduler_.sem_cancel(waiters_, node);
    }

    // Contribute one permit. One release call contributes exactly one pending
    // permit. Under the authoritative locks:
    //   if the queue is non-empty:
    //       wake exactly the FIFO head, transferring this release-created
    //       permit directly to that waiter. available_ unchanged. return true.
    //   otherwise:
    //       if available_ == max_permits_:
    //           return false  (overflow; no authoritative mutation)
    //       otherwise:
    //           available_++
    //           return true
    // A queued grant from available_ == 0 succeeds without decrement or integer
    // underflow (the permit is transferred, not withdrawn and re-deposited).
    // One release never both wakes a waiter AND stores. Safe to call from an
    // external OS thread (mirrors Event::set()).
    [[nodiscard]] bool release() {
        return scheduler_.sem_release(waiters_, available_, max_permits_);
    }

private:
    Scheduler& scheduler_;
    std::atomic<permit_count_t> available_;
    const permit_count_t max_permits_;
    WaitQueue waiters_;
};

}  // namespace sluice::async
