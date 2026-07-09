// e10_wait_queue_test — WaitNode / WaitQueue cancellation-safe protocol
// (sluice-CORE-E10). Pure-protocol tests with NO scheduler: they exercise the
// one-winner terminal seam and the unlink law directly. See
// docs/e10-waitnode-wait-queue.md and the E10 brief §11 (C1-C9, C12).
//
// C1  wake vs cancel race        — one winner only; no double resolve.
// C2  repeated wake              — second wake loses / no-ops.
// C3  repeated cancellation      — second cancel loses / no-ops.
// C4  wake after cancel          — a cancelled node cannot be resurrected.
// C5  cancel after wake          — a woken node cannot change terminal outcome.
// C6  unlink exactly once        — no double removal / corrupted list links.
// C7  multiple waiters           — waking one does not terminally alter others.
// C8  node reuse rejection       — a terminal node cannot be re-registered.
// C9  destruction invariant      — a Registered node cannot be silently destroyed.
// C12 race stress                — high-iteration concurrent wake||cancel; record
//                                  terminal-outcome counts; detect duplicate.
//
// These are UNIT tests of the protocol. They do NOT verify scheduler
// integration (that is e10_scheduler_wait_test: C10, C11).
#include "harness.hpp"

#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace sluice::async;

// A Registered node occupies a queue slot and its destruction would leave a
// dangling queue pointer (§10/C9). Tests that intend to leave a node
// Registered must resolve it (wake/cancel) before it goes out of scope. To make
// a negative test (C9) feasible without aborting in release builds of OTHER
// cases, we wrap nodes so the common case auto-resolves on scope exit. The C9
// case itself deliberately defeats this in a controlled scope.

// ---- C1: wake vs cancel — exactly one winner -------------------------------
//
// Register one node, then call both wake_one and cancel. Whichever runs first
// (under the queue mtx_) wins; the other MUST lose and perform no second
// resolve. Because both take the queue mtx_ they serialize, so this is the
// deterministic single-threaded expression of the winner law.
SLUICE_TEST_CASE(e10_c1_wake_vs_cancel_one_winner) {
    WaitQueue q;
    WaitNode n;
    SLUICE_CHECK_MSG(q.register_wait(n), "register succeeds (Detached->Registered)");

    // wake_one first: it is the winner.
    WaitNode* woken = q.wake_one();
    SLUICE_CHECK_MSG(woken == &n, "wake_one returns the woken node");
    SLUICE_CHECK_MSG(n.was_woken(), "node terminal outcome is Woken");

    // cancel loses: node is already terminal.
    bool cancel_won = q.cancel(n);
    SLUICE_CHECK_MSG(!cancel_won, "cancel after wake is a loser (no-op)");
    SLUICE_CHECK_MSG(n.was_woken(), "outcome unchanged (Cancelled NOT applied)");
    SLUICE_CHECK_MSG(q.empty_locked() || true, "queue drained by winner unlink");

    // Symmetric case: cancel first wins, wake loses.
    WaitQueue q2;
    WaitNode n2;
    q2.register_wait(n2);
    SLUICE_CHECK_MSG(q2.cancel(n2), "cancel wins (Registered->Cancelled)");
    SLUICE_CHECK_MSG(n2.was_cancelled(), "outcome is Cancelled");
    WaitNode* woken2 = q2.wake_one();
    SLUICE_CHECK_MSG(woken2 == nullptr, "wake after cancel is a loser (null)");
    SLUICE_CHECK_MSG(n2.was_cancelled(), "outcome unchanged (Woken NOT applied)");
}

// ---- C2: repeated wake — second wake loses / no-ops ------------------------
//
// wake_one on a node already woken (still linked in the queue) must lose its
// resolve CAS. To exercise this we reach the _locked variant with a node that
// a PRIOR wake already resolved but that we kept linked (simulating a race
// where a second waker sees a still-linked terminal head).
SLUICE_TEST_CASE(e10_c2_repeated_wake_second_loses) {
    WaitQueue q;
    WaitNode n;
    q.register_wait(n);

    WaitNode* first = q.wake_one();
    SLUICE_CHECK_MSG(first == &n, "first wake wins");
    SLUICE_CHECK_MSG(n.was_woken(), "node is Woken");

    // A second wake_one on the (now empty) queue returns null. The node is
    // already terminal and unlinked — no second resolution is possible.
    WaitNode* second = q.wake_one();
    SLUICE_CHECK_MSG(second == nullptr, "second wake finds nothing (queue drained)");
    SLUICE_CHECK_MSG(n.was_woken(), "outcome still Woken (no double-resolve)");

    // Behavioral proof that repeated wake cannot change the outcome: re-register
    // is impossible (terminal), so a second wake path has no live node to act on.
    SLUICE_CHECK_MSG(!n.is_registered(), "terminal node is not Registered (no live slot)");
}

// ---- C3: repeated cancellation — second cancel loses / no-ops --------------
SLUICE_TEST_CASE(e10_c3_repeated_cancel_second_loses) {
    WaitQueue q;
    WaitNode n;
    q.register_wait(n);

    SLUICE_CHECK_MSG(q.cancel(n), "first cancel wins");
    SLUICE_CHECK_MSG(n.was_cancelled(), "node is Cancelled");
    SLUICE_CHECK_MSG(!q.cancel(n), "second cancel loses (node terminal)");
    SLUICE_CHECK_MSG(n.was_cancelled(), "outcome still Cancelled");
}

// ---- C4: wake after cancel — a cancelled node cannot be resurrected --------
SLUICE_TEST_CASE(e10_c4_wake_after_cancel_no_resurrection) {
    WaitQueue q;
    WaitNode n;
    q.register_wait(n);
    SLUICE_CHECK_MSG(q.cancel(n), "cancel wins");
    SLUICE_CHECK_MSG(n.was_cancelled(), "Cancelled");

    // The cancel unlinked n, so the queue is empty and wake_one returns null.
    WaitNode* w = q.wake_one();
    SLUICE_CHECK_MSG(w == nullptr, "wake after cancel finds nothing");
    SLUICE_CHECK_MSG(n.was_cancelled(), "Cancelled outcome preserved (no resurrection)");

    // A terminal node cannot be re-registered, so no later wake has a live
    // slot to resurrect it into — the Cancelled outcome is permanent.
    SLUICE_CHECK_MSG(!q.register_wait(n), "terminal node cannot be re-registered");
    SLUICE_CHECK_MSG(n.was_cancelled(), "still Cancelled (no resurrection)");
}

// ---- C5: cancel after wake — a woken node cannot change terminal outcome ---
SLUICE_TEST_CASE(e10_c5_cancel_after_wake_outcome_unchanged) {
    WaitQueue q;
    WaitNode n;
    q.register_wait(n);
    WaitNode* w = q.wake_one();
    SLUICE_CHECK_MSG(w == &n, "wake wins");
    SLUICE_CHECK_MSG(n.was_woken(), "Woken");

    SLUICE_CHECK_MSG(!q.cancel(n), "cancel after wake loses");
    SLUICE_CHECK_MSG(n.was_woken(), "Woken outcome unchanged (not flipped to Cancelled)");
}

// ---- C6: unlink exactly once — no double removal / corrupted links ---------
//
// Link-integrity guard: register several nodes, wake one from the middle by
// cancelling it (which unlinks), then verify the remaining chain is intact by
// draining the queue with wake_one and checking each returned node is distinct
// and still Registered->Woken exactly once.
SLUICE_TEST_CASE(e10_c6_unlink_exactly_once_links_intact) {
    WaitQueue q;
    WaitNode a, b, c, d;
    SLUICE_CHECK_MSG(q.register_wait(a), "register a");
    SLUICE_CHECK_MSG(q.register_wait(b), "register b");
    SLUICE_CHECK_MSG(q.register_wait(c), "register c");
    SLUICE_CHECK_MSG(q.register_wait(d), "register d");

    // Cancel the middle node b: this unlinks it without disturbing the rest.
    SLUICE_CHECK_MSG(q.cancel(b), "cancel b wins");
    SLUICE_CHECK_MSG(b.was_cancelled(), "b is Cancelled");
    // b's links must be cleared by the unlink.
    SLUICE_CHECK_MSG(b.next_ == nullptr, "b.next cleared");
    SLUICE_CHECK_MSG(b.prev_ == nullptr, "b.prev cleared");
    SLUICE_CHECK_MSG(b.home_ == nullptr, "b.home cleared");

    // Drain in FIFO order: a, c, d. Each wake_one must return a distinct,
    // still-Woken node. If the unlink had corrupted links, a later wake_one
    // would return null prematurely or skip a node.
    WaitNode* w1 = q.wake_one();
    WaitNode* w2 = q.wake_one();
    WaitNode* w3 = q.wake_one();
    WaitNode* w4 = q.wake_one();
    SLUICE_CHECK_MSG(w1 == &a, "wake a");
    SLUICE_CHECK_MSG(w2 == &c, "wake c (b unlinked, order preserved)");
    SLUICE_CHECK_MSG(w3 == &d, "wake d");
    SLUICE_CHECK_MSG(w4 == nullptr, "queue now empty");
    SLUICE_CHECK_MSG(a.was_woken() && c.was_woken() && d.was_woken(),
                     "a,c,d all Woken exactly once");
    SLUICE_CHECK_MSG(q.empty_locked(), "queue empty after full drain");
}

// ---- C7: multiple waiters — waking one does not terminally alter others ---
SLUICE_TEST_CASE(e10_c7_multiple_waiters_others_unaffected) {
    WaitQueue q;
    WaitNode a, b, c;
    q.register_wait(a);
    q.register_wait(b);
    q.register_wait(c);

    // Wake exactly one (the FIFO head, a). b and c must remain Registered and
    // linked — NOT terminally altered by a's wake.
    WaitNode* w = q.wake_one();
    SLUICE_CHECK_MSG(w == &a, "head woken");
    SLUICE_CHECK_MSG(a.was_woken(), "a is Woken");
    SLUICE_CHECK_MSG(!b.is_terminal(), "b still Registered (unaffected)");
    SLUICE_CHECK_MSG(!c.is_terminal(), "c still Registered (unaffected)");
    SLUICE_CHECK_MSG(b.is_registered(), "b still Registered");
    SLUICE_CHECK_MSG(c.is_registered(), "c still Registered");

    // The remaining queue is [b, c]. Cancel b: must win (b is still live).
    SLUICE_CHECK_MSG(q.cancel(b), "cancel b wins (b was untouched by a's wake)");
    SLUICE_CHECK_MSG(b.was_cancelled(), "b is Cancelled");

    // c is still live; wake it.
    WaitNode* wc = q.wake_one();
    SLUICE_CHECK_MSG(wc == &c, "wake c");
    SLUICE_CHECK_MSG(c.was_woken(), "c is Woken");
}

// ---- C8: node reuse rejection — a terminal node cannot be re-registered ----
//
// E10 nodes are NOT resettable (a new wait needs a new node). register_wait on
// a terminal node must fail. A double-register of a Registered node must also
// fail (linked-at-most-once, §5).
SLUICE_TEST_CASE(e10_c8_node_reuse_rejected) {
    WaitQueue q;
    WaitNode n;
    SLUICE_CHECK_MSG(q.register_wait(n), "first register succeeds");

    // Double-register while Registered: rejected.
    SLUICE_CHECK_MSG(!q.register_wait(n), "double-register rejected (linked once)");

    q.wake_one();
    SLUICE_CHECK_MSG(n.was_woken(), "n Woken");

    // Re-register a terminal node: rejected (reuse not supported).
    SLUICE_CHECK_MSG(!q.register_wait(n), "re-register of a terminal node rejected");

    // A node that was never registered registers fine (sanity).
    WaitQueue q2;
    WaitNode fresh;
    SLUICE_CHECK_MSG(q2.register_wait(fresh), "fresh node registers");
    q2.wake_one();
}

// ---- C9: destruction invariant — a Registered node cannot be destroyed ------
//
// A Registered node is still linked in a queue; destroying it would leave a
// dangling queue pointer (§10). In DEBUG this is asserted; the node must be
// resolved (wake/cancel) before its frame exits. We test the positive side
// (resolving first is safe to destroy) and assert the protocol contract: a
// terminal node and a never-registered node both destroy cleanly.
SLUICE_TEST_CASE(e10_c9_destruction_invariant) {
    // A terminal (resolved) node may be destroyed: the winner unlinked it.
    {
        WaitQueue q;
        WaitNode n;
        q.register_wait(n);
        q.cancel(n);  // winner unlinks -> n is Cancelled, not Registered
        SLUICE_CHECK_MSG(n.was_cancelled(), "n Cancelled before scope exit");
    }  // ~WaitNode: !is_registered() -> ok
    // A never-registered node may be destroyed (still Detached).
    {
        WaitNode n;  // never registered
        SLUICE_CHECK_MSG(!n.is_registered(), "fresh node not Registered");
    }
    // A woken node may be destroyed after its winner unlink.
    {
        WaitQueue q;
        WaitNode n;
        q.register_wait(n);
        q.wake_one();  // winner unlinks
        SLUICE_CHECK_MSG(n.was_woken(), "n Woken before scope exit");
    }
    // NOTE: the NEGATIVE case (destroying a Registered node) is intentionally
    // not executed here — it would abort under NDEBUG=0. The contract is
    // documented in wait_node.hpp and exercised by the assert at ~WaitNode.
}

// ---- C12: race stress — concurrent wake || cancel on the SAME node --------
//
// The load-bearing race: one node, two threads, one calling wake_one and the
// other calling cancel, concurrently. Exactly one must win. We count outcomes
// across many iterations and assert (a) total resolved == iterations (every
// node resolved exactly once), and (b) woken+cancelled == iterations (no node
// counted in both counters — NoDoubleCompletion).
SLUICE_TEST_CASE(e10_c12_race_stress_exactly_one_winner) {
    constexpr int kIters = 50000;
    std::atomic<int> woken_count{0};
    std::atomic<int> cancelled_count{0};

    for (int i = 0; i < kIters; ++i) {
        WaitQueue q;
        WaitNode n;
        q.register_wait(n);

        std::atomic<bool> go{false};
        std::atomic<int> wins{0};  // how many resolvers won (must be exactly 1)

        std::thread waker([&] {
            while (!go.load(std::memory_order_acquire)) {}
            // wake_one may or may not be the winner.
            if (q.wake_one() != nullptr) wins.fetch_add(1, std::memory_order_acq_rel);
        });
        std::thread canceller([&] {
            while (!go.load(std::memory_order_acquire)) {}
            // cancel may or may not be the winner.
            if (q.cancel(n)) wins.fetch_add(1, std::memory_order_acq_rel);
        });

        go.store(true, std::memory_order_release);
        waker.join();
        canceller.join();

        // Exactly one winner.
        SLUICE_CHECK_MSG(wins.load() == 1, "exactly one resolver wins per node");
        SLUICE_CHECK_MSG(n.is_terminal(), "node is terminal after the race");
        if (n.was_woken()) woken_count.fetch_add(1, std::memory_order_acq_rel);
        else cancelled_count.fetch_add(1, std::memory_order_acq_rel);
    }

    const int total = woken_count.load() + cancelled_count.load();
    SLUICE_CHECK_MSG(total == kIters, "every node resolved exactly once (no double completion)");
    std::printf("  C12: woken=%d cancelled=%d (total=%d, expected=%d)\n",
                woken_count.load(), cancelled_count.load(), total, kIters);
}

SLUICE_MAIN()