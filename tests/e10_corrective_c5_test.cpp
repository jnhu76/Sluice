// e10_corrective_c5_test — E10-CORRECTIVE C5 middle-node concurrent unlink
// topology stress (sluice-CORE-E10).
//
// Test-adequacy gap (C5): the E10 suite lacked a focused concurrent stress on
// UNLINKING THE MIDDLE NODE while a resolver races on the HEAD. The doubly-
// linked list is the load-bearing structural substrate; a concurrent splice of
// a middle node (B) while the head (A) is being resolved must leave the list
// (and the surviving tail C) structurally valid.
//
// Topology per iteration:
//
//     A <-> B <-> C
//
// Contenders (genuinely concurrent — no serializing joins before the operation):
//     Thread 1: wake one  -> resolve queue HEAD A (winner A->Woken, unlink A)
//     Thread 2: cancel    -> resolve MIDDLE node B (winner B->Cancelled, unlink B)
//
// Both resolvers run the resolve_ CAS under q.mtx_ (via the test hook), so they
// serialize. Exactly one outcome per node. After both finish we verify:
//   - A has exactly one terminal outcome (Woken or — if cancel raced the head
//     first — but B is the cancel target, so A is resolved only by wake; see
//     notes). A.is_terminal() && A.outcome() is a single terminal value.
//   - B has exactly one terminal outcome (Cancelled).
//   - C remains Registered/linked until explicitly resolved.
//   - head/tail are consistent (empty after C is also resolved).
//   - no double unlink (each node resolved exactly once).
//   - neighbor links (C.prev after B's unlink) are structurally valid.
//
// NOTE on this test's scope: this is a TEST_ADEQUACY_GAP (C5), NOT a claim of a
// pre-existing correctness defect. If this test reproduced list corruption on
// uncorrected E10 it would be reclassified; it does not — the winner-CAS +
// same-critical-section unlink already holds. The test LOCKS IN that property
// at a meaningful stress count so a future regression in the middle-node
// unlink path is caught.
//
// Pure protocol (no scheduler): it exercises WaitQueue::wake_one_locked /
// cancel_locked directly via WaitQueueTestHooks under mtx_.
#include "harness.hpp"

#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <atomic>
#include <thread>

using namespace sluice::async;

// E10-CORRECTIVE C2 test hook reaching the (now private) resolvers.
namespace sluice::async {
struct WaitQueueTestHooks {
    static WaitNode* wake_one_locked(WaitQueue& q) { return q.wake_one_locked(); }
    static bool cancel_locked(WaitQueue& q, WaitNode& n) { return q.cancel_locked(n); }
    static WaitNode* wake_one(WaitQueue& q) {
        std::lock_guard<std::mutex> lk(q.mtx_);
        return q.wake_one_locked();
    }
    static bool cancel(WaitQueue& q, WaitNode& n) {
        std::lock_guard<std::mutex> lk(q.mtx_);
        return q.cancel_locked(n);
    }
};
}  // namespace sluice::async

SLUICE_TEST_CASE(e10_corrective_c5_middle_node_concurrent_unlink_topology) {
    constexpr int kIters = 20000;
    std::atomic<int> a_woken{0}, a_cancelled{0};  // A should always be Woken
    std::atomic<int> b_cancelled{0};              // B should always be Cancelled
    std::atomic<int> c_left_registered{0};        // C survives each iter
    std::atomic<int> a_double{0}, b_double{0};    // structural corruption flags

    for (int i = 0; i < kIters; ++i) {
        WaitQueue q;
        WaitNode a, b, c;
        q.register_wait(a);
        q.register_wait(b);
        q.register_wait(c);
        // List is now A <-> B <-> C.

        std::atomic<bool> go{false};
        std::atomic<int> a_resolves{0}, b_resolves{0};

        // Thread 1: resolve HEAD A via wake_one.
        std::thread t1([&] {
            while (!go.load(std::memory_order_acquire)) {}
            if (WaitQueueTestHooks::wake_one(q) != nullptr) a_resolves.fetch_add(1);
        });
        // Thread 2: cancel MIDDLE B.
        std::thread t2([&] {
            while (!go.load(std::memory_order_acquire)) {}
            if (WaitQueueTestHooks::cancel(q, b)) b_resolves.fetch_add(1);
        });

        go.store(true, std::memory_order_release);
        t1.join();
        t2.join();

        // ---- Per-iteration topology + winner invariants ----
        // A: resolved exactly once (by the wake; B's cancel targets B, not A).
        SLUICE_CHECK_MSG(a_resolves.load() == 1, "A resolved exactly once (wake winner)");
        SLUICE_CHECK_MSG(b_resolves.load() == 1, "B resolved exactly once (cancel winner)");
        if (a_resolves.load() != 1) a_double.fetch_add(1);
        if (b_resolves.load() != 1) b_double.fetch_add(1);

        SLUICE_CHECK_MSG(a.is_terminal(), "A is terminal");
        SLUICE_CHECK_MSG(a.was_woken(), "A resolved Woken (wake_one target)");
        if (a.was_woken()) a_woken.fetch_add(1);
        else if (a.was_cancelled()) a_cancelled.fetch_add(1);

        SLUICE_CHECK_MSG(b.is_terminal(), "B is terminal");
        SLUICE_CHECK_MSG(b.was_cancelled(), "B resolved Cancelled (cancel target)");
        if (b.was_cancelled()) b_cancelled.fetch_add(1);

        // C must survive: still Registered + linked (unresolved) until we
        // explicitly resolve it. The middle-node unlink (B) must NOT have
        // corrupted C's membership.
        SLUICE_CHECK_MSG(c.is_registered(), "C remains Registered (unaffected by A/B)");
        SLUICE_CHECK_MSG(c.home_ == &q, "C still linked in q (home_ intact)");
        if (c.is_registered()) c_left_registered.fetch_add(1);

        // Drain-verify: resolve C via wake_one; the queue must then be empty
        // and C's links cleared by the winner unlink. This proves the list
        // spine (after B's middle unlink) is intact — a corrupted prev_/next_
        // would either skip C or leave head_/tail_ dangling.
        WaitNode* wc = WaitQueueTestHooks::wake_one(q);
        SLUICE_CHECK_MSG(wc == &c, "wake_one finds C (list spine intact after B unlink)");
        SLUICE_CHECK_MSG(c.was_woken(), "C resolved Woken");
        SLUICE_CHECK_MSG(q.empty_locked(), "queue empty after draining A,B,C");
        SLUICE_CHECK_MSG(a.next_ == nullptr && a.prev_ == nullptr && a.home_ == nullptr,
            "A links cleared by winner unlink");
        SLUICE_CHECK_MSG(b.next_ == nullptr && b.prev_ == nullptr && b.home_ == nullptr,
            "B links cleared by winner unlink");
        SLUICE_CHECK_MSG(c.next_ == nullptr && c.prev_ == nullptr && c.home_ == nullptr,
            "C links cleared by winner unlink");
    }

    std::printf("  C5: iters=%d A_woken=%d A_cancelled=%d B_cancelled=%d "
                "C_survived=%d A_double=%d B_double=%d\n",
                kIters, a_woken.load(), a_cancelled.load(), b_cancelled.load(),
                c_left_registered.load(), a_double.load(), b_double.load());

    // Aggregate guarantees.
    SLUICE_CHECK_MSG(a_woken.load() == kIters, "A Woken in every iteration");
    SLUICE_CHECK_MSG(a_cancelled.load() == 0, "A never wrongly Cancelled");
    SLUICE_CHECK_MSG(b_cancelled.load() == kIters, "B Cancelled in every iteration");
    SLUICE_CHECK_MSG(c_left_registered.load() == kIters, "C survived every iteration");
    SLUICE_CHECK_MSG(a_double.load() == 0, "no double resolve of A");
    SLUICE_CHECK_MSG(b_double.load() == 0, "no double resolve of B");
}

SLUICE_MAIN()
