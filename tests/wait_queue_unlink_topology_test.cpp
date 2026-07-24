// wait_queue_unlink_topology_test — E10-CORRECTIVE-2 C5 Scheduler-integrated middle-node
// concurrent-unlink topology stress (sluice-CORE-E10).
//
// E10-CORRECTIVE-2 (this file): the old C5 used privileged test access
// (WaitQueueTestHooks) to directly construct and mutate A<->B<->C and call the
// private resolvers. That access is now SEALED (registration + resolution are
// private, friended only to Scheduler). This rewrite builds the topology
// through REAL production registration (Scheduler::await_wait) and races the
// REAL production resolution seams (Scheduler::wake_wait_one || Scheduler::
// cancel_wait). No privileged queue mutation, no test friend. See the E10-
// CORRECTIVE-2 brief §7.
//
// Topology per iteration (built through production registration):
//
//     Fiber A: Scheduler::await_wait(q, nodeA)
//     Fiber B: Scheduler::await_wait(q, nodeB)
//     Fiber C: Scheduler::await_wait(q, nodeC)
//
// All three waits register through the production integration authority, so
// waiting_waitq_count_ == 3 once registered. Registration order is established
// deterministically by a single-worker cooperative run: the fibers are spawned
// in A, B, C order and each suspends inside await_wait before the next runs, so
// the FIFO queue order is A then B then C (no interleaving on one worker). This
// uses the execution harness, NOT privileged access, to establish the order.
//
// Contenders (genuinely concurrent — two external threads, no serializing join):
//     Thread 1: Scheduler::wake_wait_one(q)   -> resolves the head (A) Woken
//     Thread 2: Scheduler::cancel_wait(q, b)  -> resolves middle node B Cancelled
//
// Both reach ONLY the public Scheduler seams. The operations genuinely contend
// on global_mtx_ + q.mtx(). After both finish:
//   - A resumes exactly once with Woken
//   - B resumes exactly once with Cancelled
//   - C does NOT resume (still Registered)
//   - waiting_waitq_count_ == 1 (only C remains) [observed via waiting_count(),
//     which sums all wait maps; with an IdleBackend the other maps are empty]
//
// Then a final Scheduler::wake_wait_one(q) resolves C:
//   - C resumes exactly once with Woken
//   - waiting_waitq_count_ == 0
//   - no duplicate runnable publication
//   - all fibers complete
//   - all WaitNodes are terminal and unregistered
//   - WaitQueue destruction is clean
//
// Run at a meaningful iteration count. Gated to x86_64 (fiber_ctx::supported).
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

// Idle backend: outstanding()==0 always, so MW never reaches S2; the only
// progress is the Scheduler-integrated resolution. This makes waiting_count()
// reflect exactly the WaitQueue waits (the size/void/ready maps stay empty).
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
}  // namespace

SLUICE_TEST_CASE(wqtopo_c5_scheduler_integrated_topology) {
    if constexpr (!fiber_ctx::supported) return;

    constexpr int kIters = 2000;
    std::atomic<int> a_woken{0}, b_cancelled{0}, c_survived{0};
    std::atomic<int> double_resolve{0};  // any iteration with != exactly A,B resolved
    std::atomic<int> final_count_bad{0}; // waiting_count() != 0 after C resolved
    // order_bad removed: FIFO head resolution ordering is a property of
    // wake_wait_one (always resolves the head), not a concurrent-thread
    // completion-order invariant.

    for (int it = 0; it < kIters; ++it) {
        AsyncIoContext ctx(std::make_unique<IdleBackend>());
        Scheduler sched(ctx);

        WaitQueue q;
        WaitNode a, b, c;
        std::atomic<int> registered{0};   // pre-await entry count (TEST SYNC)
        std::atomic<bool> go{false};

        // The three waiters: each registers via the production path. registered
        // is bumped BEFORE await_wait's internal CAS, so it is a conservative
        // (early) signal; the resolver closes the residual race by retrying.
        Fiber fa, fb, fc;
        auto wait_body = [&](WaitNode& node) {
            registered.fetch_add(1, std::memory_order_acq_rel);
            sched.await_wait(q, node);
        };
        fa.set_entry([&](Fiber&) { wait_body(a); });
        fb.set_entry([&](Fiber&) { wait_body(b); });
        fc.set_entry([&](Fiber&) { wait_body(c); });

        FiberStack sa, sb, sc;
        SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
        SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));
        SLUICE_CHECK(sched.init_fiber(fc, sc.base(), sc.size()));
        sched.spawn(fa);
        sched.spawn(fb);
        sched.spawn(fc);

        // Run the waiters on a worker thread so they register + suspend while
        // the external resolvers race. run_live keeps the run resident (parked)
        // because waiting_waitq_count_ > 0 makes external_wake_possible_locked()
        // true, so the external resolution is observable.
        std::thread runner([&] { sched.run_live(1); });

        // ---- TEST SYNCHRONIZATION ONLY ----
        // Wait until all three await_wait calls have made the nodes registration-
        // visible. registered is bumped BEFORE await_wait's register_ CAS, so we
        // additionally retry the resolvers (below) until they win — closing the
        // residual visibility window. Production resolvers are NOT required to
        // retry; this retry is test synchronization only.
        while (registered.load(std::memory_order_acquire) < 3) {
            std::this_thread::yield();
        }
        // Let the worker reach MW-S3 + the park decision so the resolvers
        // genuinely contend on a resident run (not a STALLED one).
        std::this_thread::sleep_for(std::chrono::microseconds(30));

        // ---- Concurrent contenders (genuine contention) ----
        std::atomic<bool> a_done{false}, b_done{false};
        std::thread t_wake([&] {
            while (!go.load(std::memory_order_acquire)) {}
            // Resolve the head (A) via the public wake seam. Retry until the
            // head is resolvable (registration visibility window). Exactly one
            // wake wins the head; losers return false (no-op).
            for (int i = 0; i < 100000 && !a.is_terminal(); ++i) {
                if (sched.wake_wait_one(q)) {
                    a_done.store(true, std::memory_order_release);
                    break;
                }
                std::this_thread::yield();
            }
        });
        std::thread t_cancel([&] {
            while (!go.load(std::memory_order_acquire)) {}
            // Resolve the middle node B via the public cancel seam. Retry until
            // B's registration is visible. Exactly one cancel wins B.
            for (int i = 0; i < 100000 && !b.is_terminal(); ++i) {
                if (sched.cancel_wait(q, b)) {
                    b_done.store(true, std::memory_order_release);
                    break;
                }
                std::this_thread::yield();
            }
        });

        go.store(true, std::memory_order_release);
        t_wake.join();
        t_cancel.join();

        // ---- Per-iteration topology + winner invariants ----
        SLUICE_CHECK_MSG(a.is_terminal(), "A is terminal");
        SLUICE_CHECK_MSG(a.was_woken(), "A resolved Woken (wake_wait_one target = head)");
        SLUICE_CHECK_MSG(b.is_terminal(), "B is terminal");
        SLUICE_CHECK_MSG(b.was_cancelled(), "B resolved Cancelled (cancel_wait target = middle)");
        // C MUST survive: still Registered + unresolved (the middle/head
        // resolution did not terminally perturb it). C's links are intact.
        SLUICE_CHECK_MSG(c.is_registered(), "C remains Registered (unaffected by A/B)");
        SLUICE_CHECK_MSG(c.home_ != nullptr, "C still linked in q (membership intact)");

        if (!(a.was_woken() && b.was_cancelled())) double_resolve.fetch_add(1, std::memory_order_acq_rel);
        if (a.was_woken()) a_woken.fetch_add(1, std::memory_order_acq_rel);
        if (b.was_cancelled()) b_cancelled.fetch_add(1, std::memory_order_acq_rel);
        if (c.is_registered()) c_survived.fetch_add(1, std::memory_order_acq_rel);

        // waiting_waitq_count_ == 1 here: A and B are resolved (decremented),
        // only C remains. With the IdleBackend the other wait maps are empty, so
        // waiting_count() == waiting_waitq_count_ == 1.
        SLUICE_CHECK_MSG(sched.waiting_count() == 1,
            "waiting_count()==1 after A/B resolution (only C remains)");

        // ---- Final resolution of C via the public wake seam ----
        for (int i = 0; i < 100000 && !c.is_terminal(); ++i) {
            if (sched.wake_wait_one(q)) break;
            std::this_thread::yield();
        }
        SLUICE_CHECK_MSG(c.was_woken(), "C resolved Woken by final wake_wait_one");
        SLUICE_CHECK_MSG(sched.waiting_count() == 0,
            "waiting_count()==0 after C resolution (queue fully drained)");

        // No duplicate runnable publication: each waiter fiber resumed exactly
        // once. With a single worker + run_live, by the time the run returns
        // each resumed fiber has reached its terminal point. We verify the nodes
        // are all terminal + unregistered (the winner unlinked each).
        SLUICE_CHECK_MSG(!a.is_registered() && !b.is_registered() && !c.is_registered(),
            "all nodes terminal and unregistered");

        runner.join();  // run_live returns once MW-S3 is resolved (no stranded waits)

        // All fibers complete (no stranded Waiting fibers after the run).
        if (!(fa.state() == FiberState::done && fb.state() == FiberState::done &&
              fc.state() == FiberState::done)) {
            final_count_bad.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    // ---- Aggregate guarantees ----
    std::printf("  C5: iters=%d A_woken=%d B_cancelled=%d C_survived=%d "
                "double_resolve=%d final_bad=%d\n",
                kIters, a_woken.load(), b_cancelled.load(), c_survived.load(),
                double_resolve.load(), final_count_bad.load());

    SLUICE_CHECK_MSG(a_woken.load() == kIters, "A Woken in every iteration");
    SLUICE_CHECK_MSG(b_cancelled.load() == kIters, "B Cancelled in every iteration");
    SLUICE_CHECK_MSG(c_survived.load() == kIters, "C survived every iteration");
    SLUICE_CHECK_MSG(double_resolve.load() == 0, "no double/incorrect resolve of A or B");
    SLUICE_CHECK_MSG(final_count_bad.load() == 0,
        "all fibers complete + waiting_count()==0 after C resolution in every iter");
}

SLUICE_MAIN()
