// semaphore_authority_probe — NEGATIVE compile probe (E12-B, F-SEM-SEAM-1).
//
// This file MUST NOT COMPILE. It is the mechanical gate that proves ordinary
// production code CANNOT obtain a Semaphore's underlying WaitQueue and therefore
// CANNOT call Scheduler::wake_wait_one / wake_wait_one_locked on a Semaphore
// queue to synthesize a RESOURCE_WAKE, NOR call the private Scheduler Semaphore
// integration seams (sem_release / sem_acquire / sem_cancel) directly.
//
// The verify gate compiles this file expecting FAILURE. If any bypass compiles,
// the Semaphore public authority has regressed.
//
// It deliberately includes ONLY public production headers and uses fully-
// qualified names (no test TU friend struct). The Scheduler Semaphore seams are
// PUBLIC methods (so they can be reached by the inline Semaphore wrappers), but
// they require a WaitQueue& argument that an ordinary production TU cannot
// obtain (Semaphore has NO public wait_queue() accessor).
#include <sluice/async/scheduler.hpp>
#include <sluice/async/semaphore.hpp>

// Attempt the forbidden bypasses. These must fail to compile because Semaphore
// has NO public wait_queue() accessor and NO public access to its available_
// state, and the private Scheduler seams require a WaitQueue& the caller cannot
// name for a Semaphore.
void bypass_semaphore_authority_1(sluice::async::Semaphore& sem,
                                  sluice::async::Scheduler& scheduler) {
    // F-SEM-SEAM-1: synthesize a RESOURCE_WAKE on a Semaphore queue. MUST NOT
    // compile (no public wait_queue() accessor).
    scheduler.wake_wait_one(sem.wait_queue());  // expected: no member wait_queue
}

void bypass_semaphore_authority_2(sluice::async::Semaphore& sem,
                                  sluice::async::Scheduler& scheduler) {
    // The private Scheduler seam requires a WaitQueue&; an ordinary TU cannot
    // obtain one for a Semaphore. MUST NOT compile.
    scheduler.sem_release(sem.wait_queue(), sem.available_mut(),
                          1);  // expected: no such accessors
}
