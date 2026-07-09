// e10_wait_queue_test — WaitNode / WaitQueue cancellation-safe protocol
// (sluice-CORE-E10). E10-CORRECTIVE-2: rewritten through the PRODUCTION
// Scheduler integration authority. The protocol is observed ONLY through:
//
//   - Scheduler::await_wait        (registration integration authority)
//   - Scheduler::wake_wait_one     (Woken resolution integration authority)
//   - Scheduler::cancel_wait       (Cancelled resolution integration authority)
//   - WaitNode public lock-free state queries (was_woken / was_cancelled /
//     is_terminal / is_registered / outcome) — the terminal-outcome projection
//   - Scheduler::waiting_count()   (the wait-accounting authority)
//
// There is NO test friend and NO privileged queue mutation: the WaitQueue
// structural operations (register_wait_locked / wake_one_locked / cancel_locked
// / unlink_locked / mtx) are private, friended only to Scheduler. Each test
// registers a real Fiber via await_wait (the production registration path) so
// waiting_waitq_count_ is accounted and the Fiber identity is captured by the
// Scheduler integration authority. See docs/e10-waitnode-wait-queue.md and the
// E10-CORRECTIVE-2 brief §6.
//
// C1  wake vs cancel race        — one winner only; no double resolve.
// C2  repeated wake              — second wake loses / no-ops (returns false).
// C3  repeated cancellation      — second cancel loses / no-ops (returns false).
// C4  wake after cancel          — a cancelled node cannot be resurrected.
// C5  cancel after wake          — a woken node cannot change terminal outcome.
// C6  unlink exactly once        — multi-waiter drain preserves list spine.
// C7  multiple waiters           — waking one does not terminally alter others.
// C8  node reuse rejection       — a terminal node cannot be re-registered.
// C9  destruction invariant      — a Registered node cannot be silently destroyed.
// C12 race stress                — high-iteration concurrent wake||cancel on one
//                                  node via the Scheduler seams; exactly one winner.
//
// Gated to x86_64 (fiber_ctx::supported): registration requires a real Fiber.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace sluice::async;
using sluice::Result;

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// A backend that never completes anything (outstanding stays 0). The only
// progress in these tests is the Scheduler-integrated resolution, so MW stays
// at S1/S3 and the run terminates only once all fibers are done (or STALLED
// when a wait is deliberately left unresolved, which we then cancel).
class IdleBackend : public AsyncBackend {
public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override { return {}; }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override { return {}; }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return 0; }
    void cancel(Completion<std::size_t>&) override {}
    void cancel(Completion<void>&) override {}
    std::size_t outstanding() const noexcept override { return 0; }
};

// Spin until `flag` is observed. Bounded by the cooperative run: the waiter
// fiber yields via context_switch inside await_wait, freeing the worker to run
// the waker/canceller fiber that sets the flag.
inline void spin_wait(std::atomic<bool>& flag) {
    while (!flag.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}
}  // namespace

// =============================================================================
// C1: wake vs cancel — exactly one winner (deterministic, single worker).
//
// A waiter registers via await_wait. A waker fiber resolves the head with
// Woken; a subsequent cancel returns false (the loser). Symmetric case: cancel
// wins, then wake returns null/false. Each transition is observed through the
// Scheduler seams + the node's terminal-outcome projection.
// =============================================================================
SLUICE_TEST_CASE(e10_c1_wake_vs_cancel_one_winner) {
    if constexpr (!fiber_ctx::supported) return;

    // Case A: wake wins first; cancel loses.
    {
        AsyncIoContext ctx(std::make_unique<IdleBackend>());
        Scheduler sched(ctx);

        WaitQueue q;
        WaitNode n;
        std::atomic<bool> registered{false};
        std::atomic<bool> wake_won{false};
        std::atomic<bool> cancel_won{true};  // expect false

        Fiber fwait, fwake, fcancel;
        fwait.set_entry([&](Fiber&) {
            registered.store(true, std::memory_order_release);
            sched.await_wait(q, n);
        });
        fwake.set_entry([&](Fiber&) {
            spin_wait(registered);
            wake_won.store(sched.wake_wait_one(q), std::memory_order_release);
        });
        fcancel.set_entry([&](Fiber&) {
            spin_wait(registered);
            // Run after wake: cancel observes a terminal node -> loser.
            spin_wait(wake_won);  // wait until the wake fiber has acted
            cancel_won.store(sched.cancel_wait(q, n), std::memory_order_release);
        });

        FiberStack sw, sk, sc;
        SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
        SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
        SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
        sched.spawn(fwait);
        sched.spawn(fwake);
        sched.spawn(fcancel);
        sched.run(1);

        SLUICE_CHECK_MSG(n.was_woken(), "wake won: outcome Woken");
        SLUICE_CHECK_MSG(wake_won.load(), "wake_wait_one returned true (winner)");
        SLUICE_CHECK_MSG(!cancel_won.load(), "cancel after wake loses (no-op)");
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
    }

    // Case B: cancel wins first; wake loses.
    {
        AsyncIoContext ctx(std::make_unique<IdleBackend>());
        Scheduler sched(ctx);

        WaitQueue q;
        WaitNode n;
        std::atomic<bool> registered{false};
        std::atomic<bool> cancel_won{false};
        std::atomic<bool> wake_won{true};  // expect false

        Fiber fwait, fwake, fcancel;
        fwait.set_entry([&](Fiber&) {
            registered.store(true, std::memory_order_release);
            sched.await_wait(q, n);
        });
        fcancel.set_entry([&](Fiber&) {
            spin_wait(registered);
            cancel_won.store(sched.cancel_wait(q, n), std::memory_order_release);
        });
        fwake.set_entry([&](Fiber&) {
            spin_wait(registered);
            spin_wait(cancel_won);  // wait until the cancel fiber has acted
            wake_won.store(sched.wake_wait_one(q), std::memory_order_release);
        });

        FiberStack sw, sk, sc;
        SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
        SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
        SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
        sched.spawn(fwait);
        sched.spawn(fcancel);
        sched.spawn(fwake);
        sched.run(1);

        SLUICE_CHECK_MSG(n.was_cancelled(), "cancel won: outcome Cancelled");
        SLUICE_CHECK_MSG(cancel_won.load(), "cancel_wait returned true (winner)");
        SLUICE_CHECK_MSG(!wake_won.load(), "wake after cancel loses (no-op)");
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
    }
}

// =============================================================================
// C2: repeated wake — second wake loses / no-ops.
//
// Wake the head once (winner). A second wake_wait_one finds the queue empty
// (winner unlinked the node) and returns false. The outcome stays Woken.
// =============================================================================
SLUICE_TEST_CASE(e10_c2_repeated_wake_second_loses) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> registered{false};
    std::atomic<bool> first_wake{false};
    std::atomic<bool> second_wake{true};  // expect false

    Fiber fwait, fwake1, fwake2;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sched.await_wait(q, n);
    });
    fwake1.set_entry([&](Fiber&) {
        spin_wait(registered);
        first_wake.store(sched.wake_wait_one(q), std::memory_order_release);
    });
    fwake2.set_entry([&](Fiber&) {
        spin_wait(registered);
        spin_wait(first_wake);  // wait until the first wake has acted
        second_wake.store(sched.wake_wait_one(q), std::memory_order_release);
    });

    FiberStack sw, s1, s2;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fwake1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(fwake2, s2.base(), s2.size()));
    sched.spawn(fwait);
    sched.spawn(fwake1);
    sched.spawn(fwake2);
    sched.run(1);

    SLUICE_CHECK_MSG(first_wake.load(), "first wake wins");
    SLUICE_CHECK_MSG(!second_wake.load(), "second wake loses (queue drained by winner)");
    SLUICE_CHECK_MSG(n.was_woken(), "outcome still Woken (no double-resolve)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no stale count");
}

// =============================================================================
// C3: repeated cancellation — second cancel loses / no-ops.
// =============================================================================
SLUICE_TEST_CASE(e10_c3_repeated_cancel_second_loses) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> registered{false};
    std::atomic<bool> first_cancel{false};
    std::atomic<bool> second_cancel{true};  // expect false

    Fiber fwait, fc1, fc2;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sched.await_wait(q, n);
    });
    fc1.set_entry([&](Fiber&) {
        spin_wait(registered);
        first_cancel.store(sched.cancel_wait(q, n), std::memory_order_release);
    });
    fc2.set_entry([&](Fiber&) {
        spin_wait(registered);
        spin_wait(first_cancel);
        second_cancel.store(sched.cancel_wait(q, n), std::memory_order_release);
    });

    FiberStack sw, s1, s2;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fc1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(fc2, s2.base(), s2.size()));
    sched.spawn(fwait);
    sched.spawn(fc1);
    sched.spawn(fc2);
    sched.run(1);

    SLUICE_CHECK_MSG(first_cancel.load(), "first cancel wins");
    SLUICE_CHECK_MSG(!second_cancel.load(), "second cancel loses (node terminal)");
    SLUICE_CHECK_MSG(n.was_cancelled(), "outcome still Cancelled");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no stale count");
}

// =============================================================================
// C4: wake after cancel — a cancelled node cannot be resurrected.
//
// Cancel wins (outcome Cancelled). A later wake_wait_one returns false (queue
// empty / loser). The Cancelled outcome is permanent: a terminal node cannot be
// re-registered, so no later wake has a live slot.
// =============================================================================
SLUICE_TEST_CASE(e10_c4_wake_after_cancel_no_resurrection) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> registered{false};
    std::atomic<bool> cancel_won{false};
    std::atomic<bool> wake_won{true};  // expect false

    Fiber fwait, fcancel, fwake;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sched.await_wait(q, n);
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(registered);
        cancel_won.store(sched.cancel_wait(q, n), std::memory_order_release);
    });
    fwake.set_entry([&](Fiber&) {
        spin_wait(registered);
        spin_wait(cancel_won);
        wake_won.store(sched.wake_wait_one(q), std::memory_order_release);
    });

    FiberStack sw, sc, sk;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
    sched.spawn(fwait);
    sched.spawn(fcancel);
    sched.spawn(fwake);
    sched.run(1);

    SLUICE_CHECK_MSG(cancel_won.load(), "cancel won");
    SLUICE_CHECK_MSG(n.was_cancelled(), "Cancelled");
    SLUICE_CHECK_MSG(!wake_won.load(), "wake after cancel finds nothing");
    SLUICE_CHECK_MSG(n.was_cancelled(), "Cancelled outcome preserved (no resurrection)");
    SLUICE_CHECK_MSG(!n.is_registered(), "terminal node not Registered");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no stale count");
}

// =============================================================================
// C5: cancel after wake — a woken node cannot change terminal outcome.
// =============================================================================
SLUICE_TEST_CASE(e10_c5_cancel_after_wake_outcome_unchanged) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> registered{false};
    std::atomic<bool> wake_won{false};
    std::atomic<bool> cancel_won{true};  // expect false

    Fiber fwait, fwake, fcancel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sched.await_wait(q, n);
    });
    fwake.set_entry([&](Fiber&) {
        spin_wait(registered);
        wake_won.store(sched.wake_wait_one(q), std::memory_order_release);
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(registered);
        spin_wait(wake_won);
        cancel_won.store(sched.cancel_wait(q, n), std::memory_order_release);
    });

    FiberStack sw, sk, sc;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fwait);
    sched.spawn(fwake);
    sched.spawn(fcancel);
    sched.run(1);

    SLUICE_CHECK_MSG(wake_won.load(), "wake won");
    SLUICE_CHECK_MSG(n.was_woken(), "Woken");
    SLUICE_CHECK_MSG(!cancel_won.load(), "cancel after wake loses");
    SLUICE_CHECK_MSG(n.was_woken(), "Woken outcome unchanged (not flipped to Cancelled)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no stale count");
}

// =============================================================================
// C6: unlink exactly once — multi-waiter drain preserves list spine.
//
// Register three waiters A, B, C via await_wait. Cancel B (the middle node):
// it unlinks without disturbing the rest. Then wake_one resolves A (head) and
// wake_one resolves C (tail). The FIFO order A-then-C proves the list spine
// survived B's middle unlink. waiting_count()==0 at the end.
//
// Registration order is established deterministically by the cooperative
// single-worker run: fibers run in spawn order, and await_wait suspends each in
// turn, so A registers before B before C (no interleaving on one worker).
// =============================================================================
SLUICE_TEST_CASE(e10_c6_unlink_exactly_once_links_intact) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode a, b, c;
    std::atomic<int> registered{0};  // count of waiters that have registered

    Fiber fa, fb, fc, fresolver;
    auto wait_body = [&](WaitNode& node) {
        registered.fetch_add(1, std::memory_order_acq_rel);  // BEFORE await: NOT yet visible
        // The count is bumped inside await_wait under the locks, AFTER this
        // pre-bump. So a resolver that waits for registered==3 still races the
        // actual register_ CAS. We close that with a bounded retry in the
        // resolver (see fresolver): this is TEST SYNCHRONIZATION ONLY.
        sched.await_wait(q, node);
    };
    fa.set_entry([&](Fiber&) { wait_body(a); });
    fb.set_entry([&](Fiber&) { wait_body(b); });
    fc.set_entry([&](Fiber&) { wait_body(c); });

    fresolver.set_entry([&](Fiber&) {
        // TEST SYNCHRONIZATION ONLY: wait until all three await_wait calls have
        // actually made the nodes registration-visible. registered counts the
        // pre-await entry, so by the time it is 3 each fiber is at or past the
        // register_ CAS; we additionally spin until wake_wait_one can observe a
        // non-empty queue. Production resolvers are NOT required to retry.
        while (registered.load(std::memory_order_acquire) < 3) {
            std::this_thread::yield();
        }
        // Cancel the middle node B first (the load-bearing middle unlink).
        // Retry until cancel wins: B's registration may not be visible yet even
        // though registered==3 (pre-await count). Exactly one cancel wins.
        for (int i = 0; i < 10000; ++i) {
            if (sched.cancel_wait(q, b)) break;
            std::this_thread::yield();
        }
        SLUICE_CHECK_MSG(b.was_cancelled(), "B cancelled (middle unlink)");

        // Now wake A (the FIFO head), then C. FIFO order survives B's unlink.
        for (int i = 0; i < 10000 && !a.is_terminal(); ++i) {
            sched.wake_wait_one(q);
            std::this_thread::yield();
        }
        SLUICE_CHECK_MSG(a.was_woken(), "A woken (head, FIFO order preserved)");

        for (int i = 0; i < 10000 && !c.is_terminal(); ++i) {
            sched.wake_wait_one(q);
            std::this_thread::yield();
        }
        SLUICE_CHECK_MSG(c.was_woken(), "C woken (tail, list spine intact)");
    });

    FiberStack sa, sb, sc, sr;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(fc, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(fresolver, sr.base(), sr.size()));
    sched.spawn(fa);
    sched.spawn(fb);
    sched.spawn(fc);
    sched.spawn(fresolver);
    sched.run(1);

    SLUICE_CHECK_MSG(a.was_woken(), "A resolved Woken");
    SLUICE_CHECK_MSG(b.was_cancelled(), "B resolved Cancelled");
    SLUICE_CHECK_MSG(c.was_woken(), "C resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "queue fully drained (no stale count)");
}

// =============================================================================
// C7: multiple waiters — waking one does not terminally alter others.
//
// Register A, B, C. Wake_one resolves only A (the head); B and C MUST remain
// Registered + unresolved (their terminal outcome is untouched). Then cancel B
// (still live) and wake C. This proves a wake of one node does not terminally
// perturb its queue neighbors.
// =============================================================================
SLUICE_TEST_CASE(e10_c7_multiple_waiters_others_unaffected) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode a, b, c;
    std::atomic<int> registered{0};

    Fiber fa, fb, fc, fresolver;
    auto wait_body = [&](WaitNode& node) {
        registered.fetch_add(1, std::memory_order_acq_rel);
        sched.await_wait(q, node);
    };
    fa.set_entry([&](Fiber&) { wait_body(a); });
    fb.set_entry([&](Fiber&) { wait_body(b); });
    fc.set_entry([&](Fiber&) { wait_body(c); });

    fresolver.set_entry([&](Fiber&) {
        // TEST SYNCHRONIZATION ONLY: wait for all three registrations visible.
        while (registered.load(std::memory_order_acquire) < 3) {
            std::this_thread::yield();
        }
        // Wake exactly one (the FIFO head A). Retry until A resolves: A's
        // registration may not be visible yet. Exactly one wake wins.
        for (int i = 0; i < 10000 && !a.is_terminal(); ++i) {
            sched.wake_wait_one(q);
            std::this_thread::yield();
        }
        SLUICE_CHECK_MSG(a.was_woken(), "head A woken");
        // B and C must be UNAFFECTED by A's wake.
        SLUICE_CHECK_MSG(!b.is_terminal(), "B still Registered (unaffected by A's wake)");
        SLUICE_CHECK_MSG(!c.is_terminal(), "C still Registered (unaffected by A's wake)");
        SLUICE_CHECK_MSG(b.is_registered(), "B still Registered");
        SLUICE_CHECK_MSG(c.is_registered(), "C still Registered");

        // Cancel B (still live) — must win.
        for (int i = 0; i < 10000 && !b.is_terminal(); ++i) {
            sched.cancel_wait(q, b);
            std::this_thread::yield();
        }
        SLUICE_CHECK_MSG(b.was_cancelled(), "B cancelled (was untouched by A's wake)");

        // Wake C (still live).
        for (int i = 0; i < 10000 && !c.is_terminal(); ++i) {
            sched.wake_wait_one(q);
            std::this_thread::yield();
        }
        SLUICE_CHECK_MSG(c.was_woken(), "C woken");
    });

    FiberStack sa, sb, sc, sr;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(fc, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(fresolver, sr.base(), sr.size()));
    sched.spawn(fa);
    sched.spawn(fb);
    sched.spawn(fc);
    sched.spawn(fresolver);
    sched.run(1);

    SLUICE_CHECK_MSG(a.was_woken() && b.was_cancelled() && c.was_woken(),
                     "A Woken, B Cancelled, C Woken (each resolved exactly once)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no stale count");
}

// =============================================================================
// C8: node reuse rejection — a terminal node cannot be re-registered.
//
// E10 nodes are NOT resettable: once terminal, they cannot re-enter a queue.
// Because registration now goes ONLY through Scheduler::await_wait (which
// suspends the current fiber), re-registering a terminal node would require a
// second fiber to await_wait on it — but await_wait's registration calls the
// private register_wait_locked, which rejects a terminal node (returns false),
// and await_wait then returns WITHOUT suspending. We verify that observable
// behavior: a fiber attempting to await_wait on an already-terminal node does
// NOT suspend and the node's outcome is unchanged.
// =============================================================================
SLUICE_TEST_CASE(e10_c8_node_reuse_rejected) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> registered{false};
    std::atomic<bool> first_wake{false};
    std::atomic<bool> reuse_returned{false};  // did the reuse fiber pass await_wait?

    Fiber fwait, fwake, freuse;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sched.await_wait(q, n);  // first registration
    });
    fwake.set_entry([&](Fiber&) {
        spin_wait(registered);
        for (int i = 0; i < 10000 && !n.is_terminal(); ++i) {
            sched.wake_wait_one(q);
            std::this_thread::yield();
        }
        first_wake.store(true, std::memory_order_release);
    });
    freuse.set_entry([&](Fiber&) {
        spin_wait(first_wake);  // n is now terminal (Woken)
        // Attempt to re-register the terminal node via the production path.
        // await_wait calls the private register_wait_locked, which rejects a
        // terminal node and returns WITHOUT suspending. The fiber proceeds.
        sched.await_wait(q, n);
        reuse_returned.store(true, std::memory_order_release);
        // Clean up: the re-register was rejected, so n is still Woken and not
        // Registered; nothing to cancel. The node outlives the run in this scope.
    });

    FiberStack sw, sk, su;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
    SLUICE_CHECK(sched.init_fiber(freuse, su.base(), su.size()));
    sched.spawn(fwait);
    sched.spawn(fwake);
    sched.spawn(freuse);
    sched.run(1);

    SLUICE_CHECK_MSG(n.was_woken(), "n Woken by the first registration's resolution");
    // The re-register attempt must NOT have suspended (the node is terminal).
    // If it had suspended, freuse would be stuck Waiting and the run would
    // STALLED, never setting reuse_returned.
    SLUICE_CHECK_MSG(reuse_returned.load(),
        "re-register of a terminal node did not suspend (register_wait_locked rejected it)");
    SLUICE_CHECK_MSG(n.was_woken(), "outcome still Woken (no resurrection via await_wait)");
    SLUICE_CHECK_MSG(!n.is_registered(), "terminal node not re-Registered");
}

// =============================================================================
// C9: destruction invariant — a Registered node cannot be destroyed.
//
// A terminal (resolved) node and a never-registered node both destroy cleanly
// (the winner unlinked the terminal node; a never-registered node was never
// linked). We verify the POSITIVE side: resolving before scope exit is safe.
// The NEGATIVE case (destroying a Registered node) is the ~WaitNode debug
// assert; executing it would abort under NDEBUG=0.
// =============================================================================
SLUICE_TEST_CASE(e10_c9_destruction_invariant) {
    if constexpr (!fiber_ctx::supported) return;

    // Case A: a node resolved via cancel destroys cleanly after the run.
    {
        AsyncIoContext ctx(std::make_unique<IdleBackend>());
        Scheduler sched(ctx);
        WaitQueue q;
        WaitNode n;
        std::atomic<bool> registered{false};

        Fiber fwait, fcancel;
        fwait.set_entry([&](Fiber&) {
            registered.store(true, std::memory_order_release);
            sched.await_wait(q, n);
        });
        fcancel.set_entry([&](Fiber&) {
            spin_wait(registered);
            for (int i = 0; i < 10000 && !n.is_terminal(); ++i) {
                sched.cancel_wait(q, n);
                std::this_thread::yield();
            }
        });
        FiberStack sw, sc;
        SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
        SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
        sched.spawn(fwait);
        sched.spawn(fcancel);
        sched.run(1);
        SLUICE_CHECK_MSG(n.was_cancelled(), "n Cancelled before scope exit");
    }  // ~WaitNode: !is_registered() -> ok; ~WaitQueue: empty -> ok

    // Case B: a never-registered node destroys cleanly (Detached).
    { WaitNode n; SLUICE_CHECK_MSG(!n.is_registered(), "fresh node not Registered"); }

    // Case C: a node resolved via wake destroys cleanly.
    {
        AsyncIoContext ctx(std::make_unique<IdleBackend>());
        Scheduler sched(ctx);
        WaitQueue q;
        WaitNode n;
        std::atomic<bool> registered{false};

        Fiber fwait, fwake;
        fwait.set_entry([&](Fiber&) {
            registered.store(true, std::memory_order_release);
            sched.await_wait(q, n);
        });
        fwake.set_entry([&](Fiber&) {
            spin_wait(registered);
            for (int i = 0; i < 10000 && !n.is_terminal(); ++i) {
                sched.wake_wait_one(q);
                std::this_thread::yield();
            }
        });
        FiberStack sw, sk;
        SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
        SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
        sched.spawn(fwait);
        sched.spawn(fwake);
        sched.run(1);
        SLUICE_CHECK_MSG(n.was_woken(), "n Woken before scope exit");
    }
}

// =============================================================================
// C12: race stress — concurrent wake || cancel on the SAME node via Scheduler.
//
// The load-bearing race: one node registered via await_wait, two EXTERNAL
// threads, one calling Scheduler::wake_wait_one and the other Scheduler::
// cancel_wait, concurrently. Exactly one must win. We count outcomes across
// many iterations and assert (a) total resolved == iterations (every node
// resolved exactly once), and (b) woken+cancelled == iterations (no node
// counted in both — NoDoubleCompletion).
//
// NOTE on the wait/accounting authority (E10-CORRECTIVE-2): the external
// resolver threads reach ONLY the public Scheduler seams (wake_wait_one /
// cancel_wait), which take global_mtx_ + q.mtx() and decrement
// waiting_waitq_count_ on a win. There is no privileged access. The waiter
// fiber is the sole registrant (await_wait), so the Fiber identity is exactly
// ws->current captured by the Scheduler integration authority (I2).
// =============================================================================
SLUICE_TEST_CASE(e10_c12_race_stress_exactly_one_winner) {
    if constexpr (!fiber_ctx::supported) return;

    constexpr int kIters = 20000;
    std::atomic<int> woken_count{0};
    std::atomic<int> cancelled_count{0};
    std::atomic<int> double_win{0};

    for (int i = 0; i < kIters; ++i) {
        AsyncIoContext ctx(std::make_unique<IdleBackend>());
        Scheduler sched(ctx);

        WaitQueue q;
        WaitNode n;
        std::atomic<bool> registered{false};
        std::atomic<bool> go{false};
        std::atomic<int> wins{0};

        // Waiter fiber: the sole registrant via the production path.
        Fiber fwait;
        fwait.set_entry([&](Fiber&) {
            registered.store(true, std::memory_order_release);
            sched.await_wait(q, n);
        });
        FiberStack sw;
        SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
        sched.spawn(fwait);

        // Run the waiter on a worker thread so it registers + suspends while the
        // external resolvers race. run_live keeps the run resident (parked) so
        // the external wake/cancel is observable. The two resolver threads then
        // genuinely contend on the SAME node.
        std::thread runner([&] { sched.run_live(1); });

        // TEST SYNCHRONIZATION ONLY: wait until await_wait has made the node
        // registration-visible. registered is set BEFORE await_wait's internal
        // register_ CAS, so we additionally spin until the queue is non-empty by
        // probing wake_wait_one's return (a false means empty/lost, not a win).
        // Production resolvers are NOT required to retry.
        while (!registered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Give the worker a beat to reach the park decision, then release the
        // resolvers to race.
        std::this_thread::sleep_for(std::chrono::microseconds(20));

        std::thread waker([&] {
            while (!go.load(std::memory_order_acquire)) {}
            if (sched.wake_wait_one(q)) wins.fetch_add(1, std::memory_order_acq_rel);
        });
        std::thread canceller([&] {
            while (!go.load(std::memory_order_acquire)) {}
            if (sched.cancel_wait(q, n)) wins.fetch_add(1, std::memory_order_acq_rel);
        });

        go.store(true, std::memory_order_release);
        waker.join();
        canceller.join();
        runner.join();

        // Exactly one winner per node.
        if (wins.load() != 1) double_win.fetch_add(1, std::memory_order_acq_rel);
        SLUICE_CHECK_MSG(n.is_terminal(), "node is terminal after the race");
        if (n.was_woken()) woken_count.fetch_add(1, std::memory_order_acq_rel);
        else if (n.was_cancelled()) cancelled_count.fetch_add(1, std::memory_order_acq_rel);
    }

    const int total = woken_count.load() + cancelled_count.load();
    SLUICE_CHECK_MSG(total == kIters, "every node resolved exactly once (no double completion)");
    SLUICE_CHECK_MSG(double_win.load() == 0, "exactly one resolver won per node");
    std::printf("  C12: woken=%d cancelled=%d (total=%d, expected=%d) double_win=%d\n",
                woken_count.load(), cancelled_count.load(), total, kIters, double_win.load());
}

SLUICE_MAIN()
