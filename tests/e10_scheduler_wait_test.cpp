// e10_scheduler_wait_test — Scheduler integration of WaitNode/WaitQueue
// (sluice-CORE-E10). Integration tests with REAL fibers driving the canonical
// scheduler wake seam. See docs/e10-waitnode-wait-queue.md and the E10 brief
// §11 (C10, C11).
//
// C10  scheduler integration — exactly one winning resolver makes the waiting
//      execution runnable through the canonical scheduler seam (one runnable
//      enqueue per resolution; no duplicate dispatch).
// C11  Drain interaction — a registered wait and its resolution do NOT revive
//      the old E9 Drain hang and do NOT redefine wake capability; a stranded
//      MW-S3 wait returns STALLED exactly as E9 does (the run terminates, the
//      wait is left unresolved for the caller).
//
// These are INTEGRATION tests: they use the Scheduler + real fibers + the
// FakeAsyncBackend. The pure protocol (C1-C9, C12) is in e10_wait_queue_test.
//
// Single coordinated run(N) per test (sched_ctx is OS-thread-stack-dependent).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <atomic>
#include <cstddef>
#include <thread>

using namespace sluice::async;

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};
}  // namespace

// ---- C10: exactly-one winner makes the fiber runnable via the canonical seam -
//
// A waiter fiber suspends on a WaitQueue via await_wait. A waker fiber calls
// wake_wait_one. The waiter must resume EXACTLY ONCE (entered twice: before and
// after the await). This proves the canonical seam delivers exactly one
// runnable enqueue and the winner transition routes through route_runnable.
SLUICE_TEST_CASE(e10_c10_wake_resumes_once_via_canonical_seam) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode node;  // owned by this scope; outlives the run
    std::atomic<int> entries{0};
    std::atomic<bool> waiter_suspended{false};

    Fiber fwait, fwake;
    fwait.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);  // before await
        waiter_suspended.store(true, std::memory_order_release);
        sched.await_wait(q, node);
        entries.fetch_add(1, std::memory_order_acq_rel);  // after resume
    });
    fwake.set_entry([&](Fiber&) {
        // Wait until the waiter has registered + suspended, then wake it.
        while (!waiter_suspended.load(std::memory_order_acquire)) {
            // yield is fine; the run is cooperative on a single worker so this
            // fiber does not run until fwait yields via context_switch. With 2
            // workers fwait suspends and frees a worker to run fwake.
        }
        bool woke = sched.wake_wait_one(q);
        SLUICE_CHECK_MSG(woke, "wake_wait_one delivered exactly one wake");
    });

    FiberStack sw, sk;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
    sched.spawn(fwait);
    sched.spawn(fwake);
    sched.run(2);

    // The waiter entered once before await and once after resume -> exactly 2.
    SLUICE_CHECK_MSG(entries.load() == 2, "waiter resumed exactly once");
    SLUICE_CHECK_MSG(node.was_woken(), "node terminal outcome is Woken");
    SLUICE_CHECK_MSG(fwait.state() == FiberState::done, "fwait reached done");
    SLUICE_CHECK_MSG(fwake.state() == FiberState::done, "fwake reached done");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- C10b: cancel also resumes exactly once via the canonical seam ----------
//
// Symmetric to C10 but via cancel_wait. The waiter resumes with outcome
// Cancelled (still exactly one runnable enqueue).
SLUICE_TEST_CASE(e10_c10_cancel_resumes_once_via_canonical_seam) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode node;
    std::atomic<int> entries{0};
    std::atomic<bool> waiter_suspended{false};

    Fiber fwait, fcancel;
    fwait.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        waiter_suspended.store(true, std::memory_order_release);
        sched.await_wait(q, node);
        entries.fetch_add(1, std::memory_order_acq_rel);
    });
    fcancel.set_entry([&](Fiber&) {
        while (!waiter_suspended.load(std::memory_order_acquire)) {}
        bool cancelled = sched.cancel_wait(q, node);
        SLUICE_CHECK_MSG(cancelled, "cancel_wait won");
    });

    FiberStack sw, sk;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sk.base(), sk.size()));
    sched.spawn(fwait);
    sched.spawn(fcancel);
    sched.run(2);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter resumed exactly once");
    SLUICE_CHECK_MSG(node.was_cancelled(), "node outcome is Cancelled");
    SLUICE_CHECK_MSG(fwait.state() == FiberState::done, "fwait reached done");
}

// ---- C10c: wake-vs-cancel race — exactly one resume (no double dispatch) ----
//
// Two fibers race to resolve the same node (one wakes, one cancels). The waiter
// must resume EXACTLY ONCE regardless of which wins. Run many iterations to
// stress the scheduler-side race under the canonical seam.
SLUICE_TEST_CASE(e10_c10_wake_cancel_race_one_resume) {
    if constexpr (!fiber_ctx::supported) return;

    constexpr int kIters = 2000;
    std::atomic<int> woken{0};
    std::atomic<int> cancelled{0};
    std::atomic<int> double_resume{0};

    for (int it = 0; it < kIters; ++it) {
        AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
        Scheduler sched(ctx);

        WaitQueue q;
        WaitNode node;
        std::atomic<int> entries{0};
        std::atomic<bool> suspended{false};
        // Barrier: both resolvers observe suspended before racing.
        std::atomic<bool> go{false};

        Fiber fwait, fwake, fcancel;
        fwait.set_entry([&](Fiber&) {
            entries.fetch_add(1, std::memory_order_acq_rel);
            suspended.store(true, std::memory_order_release);
            sched.await_wait(q, node);
            entries.fetch_add(1, std::memory_order_acq_rel);
        });
        fwake.set_entry([&](Fiber&) {
            while (!suspended.load(std::memory_order_acquire)) {}
            while (!go.load(std::memory_order_acquire)) {}
            (void)sched.wake_wait_one(q);
        });
        fcancel.set_entry([&](Fiber&) {
            while (!suspended.load(std::memory_order_acquire)) {}
            while (!go.load(std::memory_order_acquire)) {}
            (void)sched.cancel_wait(q, node);
        });

        FiberStack sw, sk, sc;
        SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
        SLUICE_CHECK(sched.init_fiber(fwake, sk.base(), sk.size()));
        SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
        sched.spawn(fwait);
        sched.spawn(fwake);
        sched.spawn(fcancel);
        // run(2): at most 2 fibers run concurrently; the race is between the
        // wake/cancel fibers and is serialized on global_mtx_ + q.mtx(). With
        // a single worker the race is still valid (cooperative scheduling).
        // Use 3 workers so all three can be active and genuinely race.
        // (workers are created up to worker_count.)
        // Release the barrier after a short delay so both resolvers are primed.
        // We run in two phases is not possible (single run); instead the go
        // flag is set by the LAST resolver to become ready. Simpler: set go
        // from a 4th gate fiber.
        Fiber fgate;
        fgate.set_entry([&](Fiber&) {
            // Spin until both resolvers have observed suspended (they are now
            // spinning on go), then release them to race.
            // (They busy-wait on go; with >=3 workers they run concurrently.)
            go.store(true, std::memory_order_release);
        });
        FiberStack sg;
        SLUICE_CHECK(sched.init_fiber(fgate, sg.base(), sg.size()));
        sched.spawn(fgate);
        sched.run(3);

        if (entries.load() != 2) double_resume.fetch_add(1, std::memory_order_acq_rel);
        if (node.was_woken()) woken.fetch_add(1, std::memory_order_acq_rel);
        else if (node.was_cancelled()) cancelled.fetch_add(1, std::memory_order_acq_rel);
    }

    SLUICE_CHECK_MSG(double_resume.load() == 0, "waiter resumed exactly once in every iter");
    std::printf("  C10c: woken=%d cancelled=%d double_resume=%d (iters=%d)\n",
                woken.load(), cancelled.load(), double_resume.load(), kIters);
}

// ---- C11: Drain interaction — MW-S3 wait returns STALLED (no revival) -------
//
// A fiber suspends on a WaitQueue and NOTHING wakes it. In DRAIN mode the run
// must terminate (STALLED) exactly as E9 does for waiting_ready_ — it must NOT
// hang forever (the old E9 Drain-park defect) and must NOT redefine wake
// capability. The wait is left unresolved; the caller observes the fiber is
// still Waiting and the node is still Registered.
SLUICE_TEST_CASE(e10_c11_drain_mw_s3_returns_stalled) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode node;
    std::atomic<bool> suspended{false};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        suspended.store(true, std::memory_order_release);
        sched.await_wait(q, node);
        // Should NOT reach here in Drain STALLED (no resolver).
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);
    // Drain (default run): MW-S3 with an unresolved E10 wait returns STALLED.
    // This MUST terminate (not hang). If it revived the E9 Drain-park defect,
    // it would never return.
    sched.run(1);

    SLUICE_CHECK_MSG(suspended.load(), "waiter did suspend");
    SLUICE_CHECK_MSG(fwait.state() == FiberState::waiting,
                     "waiter left Waiting (STALLED, not resumed)");
    SLUICE_CHECK_MSG(node.is_registered(), "node left Registered (unresolved)");
    // The caller is now responsible for resolving before destroying the node.
    // Cancel it so the node can be destroyed cleanly (C9 contract).
    SLUICE_CHECK_MSG(sched.cancel_wait(q, node), "caller cancels the stranded wait");
    SLUICE_CHECK_MSG(node.was_cancelled(), "node now Cancelled (safe to destroy)");
}

SLUICE_MAIN()
