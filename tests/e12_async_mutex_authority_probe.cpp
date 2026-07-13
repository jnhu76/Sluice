// e12_async_mutex_authority_probe — NEGATIVE compile probe (E12-C, F-MTX-SEAM-1).
//
// This file MUST NOT COMPILE. It is the mechanical gate that proves ordinary
// production code CANNOT obtain an AsyncMutex's underlying WaitQueue or owner
// state and therefore CANNOT call Scheduler::wake_wait_one /
// wake_wait_one_locked on an AsyncMutex queue to synthesize a RESOURCE_WAKE,
// NOR call the private Scheduler Mutex integration seams (mutex_unlock /
// mutex_lock / mutex_cancel / mutex_handoff_one_locked) directly, NOR reach a
// public owner()/is_locked()/wait_queue() accessor.
//
// The verify gate (scripts/verify-e12-async-mutex-formal.sh) compiles this file
// expecting FAILURE. If any bypass compiles, the AsyncMutex public authority
// has regressed.
//
// It deliberately includes ONLY public production headers and uses fully-
// qualified names (no test TU friend struct). The Scheduler Mutex seams are
// private methods; the AsyncMutex has NO public wait_queue()/owner()/is_locked()
// accessor, so the required argument cannot be named for an AsyncMutex.
#include <sluice/async/async_mutex.hpp>
#include <sluice/async/scheduler.hpp>

// Attempt the forbidden bypasses. These must fail to compile because AsyncMutex
// has NO public wait_queue() / owner() / is_locked() accessor and NO public
// mutation access to its owner_/waiters_ state.
void bypass_async_mutex_authority_1(sluice::async::AsyncMutex& mtx,
                                    sluice::async::Scheduler& scheduler) {
    // F-MTX-SEAM-1: synthesize a RESOURCE_WAKE on an AsyncMutex queue. MUST NOT
    // compile (no public wait_queue() accessor).
    scheduler.wake_wait_one(mtx.wait_queue());  // expected: no member wait_queue
}

void bypass_async_mutex_authority_2(sluice::async::AsyncMutex& mtx,
                                    sluice::async::Scheduler& scheduler) {
    // The private Scheduler seam requires a WaitQueue&; an ordinary TU cannot
    // obtain one for an AsyncMutex. MUST NOT compile.
    scheduler.mutex_unlock(mtx.wait_queue(), mtx.owner_mut());
    // expected: no such accessors
}

void bypass_async_mutex_owner(sluice::async::AsyncMutex& mtx) {
    // No public owner() / is_locked(). MUST NOT compile.
    (void)mtx.owner();
    (void)mtx.is_locked();
}
