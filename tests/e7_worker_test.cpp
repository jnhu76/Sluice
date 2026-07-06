// E7-A — worker-local execution state (sluice-CORE-E7-A).
//
// E7-T1: two workers run two Fibers concurrently; each registers a different
// wait. Assert no cross-registration. Proves worker-local current.
// E7-T2: two workers suspend/resume separate Fibers. Assert each Fiber returns
// through its own worker's scheduler continuation (distinct OS thread identity).
//
// PROVES: worker-local sched_ctx + current.
// DOES NOT PROVE: pinned routing (E7-B), MW coordination (E7-C).
//
// Single coordinated run(2) per test — no double-run (sched_ctx is OS-thread-
// stack-dependent; destroying/recreating threads between run() calls dangles
// saved continuations).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

using namespace sluice::async;

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};
}  // namespace

// ---- E7-T1: worker-local current — no cross-registration -----------------
// Two workers run two Fibers concurrently. Both Fibers reach a barrier (both
// spinning on different OS threads simultaneously), then each registers a wait
// on a different ready flag. Assert: both registered (waiting_ready_count==2),
// no cross-registration. If current_ were shared, the second worker would
// overwrite the first's current, causing one registration to be lost.
//
// The flags start UNREADY; both Fibers suspend. Worker 0 drives backend
// progress (Fake poll); since nothing is staged, run returns at MW-S3. The
// test asserts the registrations survived (both Fibers are waiting).
SLUICE_TEST_CASE(e7_t1_worker_local_current_no_cross_registration) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<bool> flag_a{false}, flag_b{false};
    std::atomic<int> at_barrier{0};
    std::atomic<bool> a_ran{false}, b_ran{false};

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        a_ran.store(true);
        at_barrier.fetch_add(1, std::memory_order_acq_rel);
        while (at_barrier.load(std::memory_order_acquire) < 2) {}
        sched.await_ready_flag(flag_a);  // registers fa under flag_a
    });
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        b_ran.store(true);
        at_barrier.fetch_add(1, std::memory_order_acq_rel);
        while (at_barrier.load(std::memory_order_acquire) < 2) {}
        sched.await_ready_flag(flag_b);  // registers fb under flag_b
    });
    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));
    sched.spawn(fa);
    sched.spawn(fb);
    // Single run(2): both Fibers reach the barrier on different OS threads,
    // both register their waits on different flags, both suspend (flags unready).
    // The run returns at MW-S3 (no runnable Fiber, no outstanding Completion).
    sched.run(2);

    SLUICE_CHECK(a_ran.load());
    SLUICE_CHECK(b_ran.load());
    SLUICE_CHECK(sched.waiting_ready_count() == 2);  // both registered, no overwrite
    SLUICE_CHECK(fa.state() == FiberState::waiting);
    SLUICE_CHECK(fb.state() == FiberState::waiting);
}

// ---- E7-T2: worker-local scheduler context — distinct continuations -------
// Two workers each run a Fiber that records its OS thread identity before
// suspend. A third Fiber (setter) sets both flags. The waiting Fibers resume.
// Assert: each Fiber's pre-suspend thread identity == post-resume identity
// (it resumed on the same OS worker thread). Also assert: the two waiting
// Fibers ran on different OS threads (proving two distinct workers).
//
// This requires all three Fibers to be in the same run(2): the setter, after
// both waiters have suspended, sets both flags, and the coordinated run's
// readiness drain routes the woken waiters back to their owning workers.
SLUICE_TEST_CASE(e7_t2_worker_local_scheduler_context) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<bool> flag_a{false}, flag_b{false};
    std::thread::id a_tid_pre{}, a_tid_post{};
    std::thread::id b_tid_pre{}, b_tid_post{};
    std::atomic<int> waiters_suspended{0};

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        a_tid_pre = std::this_thread::get_id();
        waiters_suspended.fetch_add(1, std::memory_order_acq_rel);
        sched.await_ready_flag(flag_a);
        a_tid_post = std::this_thread::get_id();
    });
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        b_tid_pre = std::this_thread::get_id();
        waiters_suspended.fetch_add(1, std::memory_order_acq_rel);
        sched.await_ready_flag(flag_b);
        b_tid_post = std::this_thread::get_id();
    });
    Fiber fset;
    fset.set_entry([&](Fiber&) {
        // Wait until both waiters have suspended.
        while (waiters_suspended.load(std::memory_order_acquire) < 2) {}
        flag_a.store(true, std::memory_order_release);
        flag_b.store(true, std::memory_order_release);
    });
    FiberStack sa, sb, sset;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(fset, sset.base(), sset.size()));
    sched.spawn(fa);
    sched.spawn(fb);
    sched.spawn(fset);
    sched.run(2);

    SLUICE_CHECK(a_tid_pre == a_tid_post);  // resumed on same OS thread
    SLUICE_CHECK(b_tid_pre == b_tid_post);
    SLUICE_CHECK(a_tid_pre != b_tid_pre);   // ran on different OS threads
    SLUICE_CHECK(fa.state() == FiberState::done);
    SLUICE_CHECK(fb.state() == FiberState::done);
    SLUICE_CHECK(fset.state() == FiberState::done);
}

SLUICE_MAIN()
