// NC3 — raw private-resolver negative compile probe (E10-CORRECTIVE-2 R1+R2).
//
// An ordinary downstream TU attempts the real authority bypass: calling the
// internal resolver equivalents (wake_one_locked, cancel_locked, unlink_locked)
// and reaching mtx_ directly against the NORMAL public header surface. This is
// the resolution-authority bypass — resolving a node + unlinking it WITHOUT
// Scheduler::wake_wait_one / Scheduler::cancel_wait (so the count is not
// decremented and the fiber is not routed).
//
// PROVENANCE:
//   dbabd21 (pre-corrective): COMPILES IF a friend grant exists (the test TU
//     defines WaitQueueTestHooks). Standalone (no friend) it is ALREADY
//     rejected on dbabd21 because the resolvers are private — UNLESS the TU
//     exploits the WaitQueueTestHooks friend grant (NC1). NC3 isolates the
//     private-resolver access itself: even without the test-hook grant, the
//     probe must remain rejected.
//   corrective (post):        COMPILATION REJECTED — the resolvers + mtx_
//     remain private, and the only friend is Scheduler. An unprivileged TU
//     cannot name them.
//
// This file is NOT built by any test target (standalone probe). The build
// script invokes the compiler on it directly and records diagnostics.
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <mutex>

using namespace sluice::async;

int main() {
    WaitQueue q;
    WaitNode n;
    // Direct private resolver access (the authority bypass). Must NOT compile.
    {
        std::lock_guard<std::mutex> lk(q.mtx_);          // private mtx_
        (void)q.wake_one_locked();                        // private resolver
        (void)q.cancel_locked(n);                         // private resolver
        q.unlink_locked(n);                               // private unlink
    }
    return 0;
}
