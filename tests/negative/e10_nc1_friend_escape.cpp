// NC1 — friend-escape negative compile probe (E10-CORRECTIVE-2 R1).
//
// An UNPRIVILEGED downstream TU, compiling against the NORMAL production
// headers, attempts to define `sluice::async::WaitQueueTestHooks` itself and
// reach the PRIVATE queue internals (mtx_, wake_one_locked, cancel_locked,
// unlink_locked) through the unconditional friendship the public header used
// to grant to any downstream-definable `WaitQueueTestHooks`.
//
// PROVENANCE:
//   dbabd21 (pre-corrective): COMPILES — the public wait_queue.hpp forward-
//     declares and friends `struct WaitQueueTestHooks`, so this TU's own
//     definition becomes the granted friend and reaches the resolvers + mtx_.
//   corrective (post):        COMPILATION REJECTED — the forward-declaration
//     and friend grant are removed from the public header, so this TU's
//     WaitQueueTestHooks is an ordinary unprivileged type with no access.
//
// This file is NOT built by any test target (it is a standalone probe). The
// build script invokes the compiler on it directly and records diagnostics.
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <mutex>

namespace sluice::async {

// The counterexample: a downstream-defined WaitQueueTestHooks reaches the
// private resolvers + mtx_ through the unconditional friend grant.
struct WaitQueueTestHooks {
    static WaitNode* bypass_wake(WaitQueue& q) {
        std::lock_guard<std::mutex> lk(q.mtx_);          // private mtx_
        return q.wake_one_locked();                       // private resolver
    }
    static bool bypass_cancel(WaitQueue& q, WaitNode& n) {
        std::lock_guard<std::mutex> lk(q.mtx_);
        return q.cancel_locked(n);                         // private resolver
    }
    static void bypass_unlink(WaitQueue& q, WaitNode& n) {
        std::lock_guard<std::mutex> lk(q.mtx_);
        q.unlink_locked(n);                               // private unlink
    }
};

}  // namespace sluice::async

// A reference so the TU is not empty if the friend grant were absent.
int main() { return 0; }
