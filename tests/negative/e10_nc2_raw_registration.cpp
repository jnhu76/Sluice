// NC2 — raw-registration negative compile probe (E10-CORRECTIVE-2 R2).
//
// An ordinary downstream TU attempts the actual equivalent of:
//
//     WaitQueue q;
//     WaitNode n;
//     q.register_wait(n, arbitrary_fiber);
//
// against the NORMAL public header surface. This is the P1/P2/P5 bypass:
// raw registration of an arbitrary Fiber* into a queue that Scheduler
// resolution later trusts, WITHOUT going through Scheduler::await_wait (so
// waiting_waitq_count_ is never incremented and the Fiber identity is not
// captured by the Scheduler integration authority).
//
// PROVENANCE:
//   dbabd21 (pre-corrective): COMPILES — WaitQueue::register_wait is a PUBLIC
//     member taking (WaitNode&, Fiber* = nullptr).
//   corrective (post):        COMPILATION REJECTED — register_wait is moved
//     to private/internal (friend Scheduler only); an external TU cannot
//     express q.register_wait(node, fiber) against the public header.
//
// This file is NOT built by any test target (standalone probe). The build
// script invokes the compiler on it directly and records diagnostics.
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

namespace sluice::async {
class Fiber;  // any downstream Fiber identity
}

using namespace sluice::async;

int main() {
    WaitQueue q;
    WaitNode n;
    sluice::async::Fiber* arbitrary = nullptr;  // arbitrary Fiber identity
    // The bypass composition: register an arbitrary node + Fiber directly.
    bool ok = q.register_wait(n, arbitrary);    // must NOT compile post-corrective
    return ok ? 0 : 1;
}
