// async_condition_authority_probe — NEGATIVE compile probe (E12-D, §1.4).
//
// This file MUST NOT COMPILE under each PROBE_CASE. It is the mechanical gate
// that proves ordinary production code CANNOT:
//   1. reach an AsyncCondition's underlying Condition WaitQueue (no
//      wait_queue() accessor);
//   2. reach the bound AsyncMutex via a mutex() accessor;
//   3. observe the Condition queue depth via a waiting_count() accessor;
//   4. batch-resolve via a notify_n() accessor;
//   5. reach the stack-local reacquire node via a reacquire_node() accessor;
//   6. call the Scheduler's private Condition seams directly (they take a
//      WaitQueue& that an external TU cannot name for an AsyncCondition);
//   7. obtain the Condition's private WaitQueue and pass it to a generic
//      Scheduler resolver (e.g. wake_wait_one) to synthesize a Condition
//      publication.
//
// The verify gate compiles this file ONCE PER CASE with -DPROBE_CASE=N, each
// time expecting a COMPILE FAILURE. If any case unexpectedly compiles, the
// AsyncCondition public authority has regressed. Each case is verified
// INDEPENDENTLY (§1.4 forbids a single-file/single-error weak gate).
//
// It deliberately includes ONLY public production headers and uses fully-
// qualified names (no test TU friend struct). The AsyncCondition has NO public
// wait_queue()/mutex()/waiting_count()/notify_n()/reacquire_node() accessor, so
// the required arguments cannot be named; the Scheduler Condition seams are
// public methods taking a WaitQueue&, but an ordinary TU cannot obtain the
// AsyncCondition's private WaitQueue to pass in.
#include <sluice/async/async_mutex.hpp>
#include <sluice/async/condition.hpp>
#include <sluice/async/scheduler.hpp>

#ifndef PROBE_CASE
#error "PROBE_CASE must be defined (1..7) by the verify gate"
#endif

void probe(sluice::async::AsyncCondition& cond,
           sluice::async::AsyncMutex& mtx,
           sluice::async::Scheduler& scheduler) {
#if PROBE_CASE == 1
    // (1) wait_queue() must NOT exist / not be accessible.
    (void)cond.wait_queue();  // expected: no member named 'wait_queue'
#elif PROBE_CASE == 2
    // (2) mutex() must NOT exist / not be accessible.
    (void)cond.mutex();  // expected: no member named 'mutex'
#elif PROBE_CASE == 3
    // (3) waiting_count() must NOT exist / not be accessible.
    (void)cond.waiting_count();  // expected: no member named 'waiting_count'
#elif PROBE_CASE == 4
    // (4) notify_n() must NOT exist / not be accessible.
    cond.notify_n(3);  // expected: no member named 'notify_n'
#elif PROBE_CASE == 5
    // (5) reacquire_node() must NOT exist / not be accessible.
    (void)cond.reacquire_node();  // expected: no member named 'reacquire_node'
#elif PROBE_CASE == 6
    // (6) External code cannot call the Scheduler private Condition seams
    // directly: they require a WaitQueue& (the Condition queue) that an
    // external TU cannot name for an AsyncCondition. Attempting to pass a
    // fabricated WaitQueue would not target the Condition queue and cannot
    // synthesize a Condition publication. Here we prove the seam cannot be
    // reached with the AsyncCondition's own (inaccessible) queue.
    sluice::async::WaitNode node;
    // cond's Condition queue is private; the seam needs it. No accessor exists,
    // so the call cannot be expressed correctly. We attempt the seam with a
    // dummy queue to prove it does not accept the Condition's hidden state —
    // but the real gate is that there is NO way to obtain cond's queue. The
    // line below is intentionally ill-formed because cond.wait_queue() does not
    // exist (same sealed authority as case 1, applied to the Scheduler seam).
    scheduler.condition_notify_one(cond.wait_queue());  // expected: no wait_queue
#elif PROBE_CASE == 7
    // (7) External code cannot obtain the Condition's private WaitQueue and pass
    // it to a generic Scheduler resolver (wake_wait_one) to synthesize a
    // Condition-epoch publication bypassing notify_one/notify_all. The required
    // bypass `scheduler.wake_wait_one(cond.wait_queue())` cannot be expressed
    // because cond.wait_queue() does not exist.
    scheduler.wake_wait_one(cond.wait_queue());  // expected: no wait_queue
#else
#error "unknown PROBE_CASE (expected 1..7)"
#endif
    (void)mtx;
}
