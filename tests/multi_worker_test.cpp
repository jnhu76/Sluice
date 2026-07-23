// E7-A — worker-local execution state (sluice-CORE-E7-A).
//
// E7-T1: two workers run two Fibers concurrently; each registers a different
// wait. Assert no cross-registration. Proves worker-local current.
// E7-T2: coordinated two-worker suspend/resume regression. Asserts the
// two-waiter + setter topology drains, wakes, and completes cleanly.
//
// CONTRACT ROLE (reassessed under E8, ADR §9.3):
//   E7-T2 no longer claims to prove the current C-A contract directly. C-A is
//   "whenever Fiber F executes/suspends/resumes on Worker W, the context
//   switch uses W's own WorkerState::sched_ctx." That pairing is a structural
//   invariant of production (sched_ctx is reached only via the local ws /
//   g_worker at every switch site — see run_next_on / fiber_entry_bridge /
//   await_* in src/async/scheduler.cpp) and is exercised across a legal E8
//   W1-suspend -> W0-resume migration by e8_t3_steal_run_suspend_wake_resume_
//   on_thief, which asserts wid_pre == wid_post on the thief. E7-T2 is a
//   distinct coordinated suspend/resume regression, NOT a C-A proof.
//
//   The original E7-era same-thread oracle (`tid_pre == tid_post`) encoded the
//   superseded C-B wait-epoch worker-affinity contract ("suspend-worker ==
//   resume-worker"). E8 superseded C-B by permitting Runnable ownership
//   transfer + cross-worker resume between runnable epochs, so C-B may be
//   violated by a legal steal. The C-B oracle is dropped here (it would reject
//   legal E8 executions on this topology); the topology's liveness/state
//   assertions remain.
//
// PROVES: worker-local current (T1); coordinated suspend/resume liveness (T2).
// DOES NOT PROVE: worker-local sched_ctx pairing (C-A — proven structurally +
//   e8_t3); pinned routing (E7-B, superseded by E8); MW coordination (E7-C).
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

// ---- E7-T2: coordinated two-worker suspend/resume regression --------------
// Historical identity: this case was named "worker_local_scheduler_context" and
// asserted `tid_pre == tid_post` per waiter to encode the E7-era C-B contract
// ("suspend-worker == resume-worker" within a wait epoch). E8 (ADR §9.3)
// superseded C-B by permitting legal Runnable ownership transfer + cross-worker
// resume between runnable epochs, so a legal steal on this topology would
// legitimately make a waiter resume on the OTHER worker — the C-B oracle would
// reject that legal E8 execution (a flake source). The same-thread oracle is
// therefore dropped; it is NOT a valid C-A proxy (C-A is "the context switch
// uses the executing Worker's own sched_ctx," not "same worker pre/post").
//
// Current role: a coordinated suspend/resume regression. Three Fibers share one
// run(2): two waiters each suspend on an unready flag; a setter, after both
// waiters have suspended, makes both flags ready. The load-bearing assertions
// are liveness + state: both waiters reach their post-resume point and all
// three Fibers complete done, and the ready-flag registrations are erased on
// wake. This exercises the readiness drain + wake-route + worker-local suspend
// machinery on the two-worker topology.
//
// C-A (worker-local sched_ctx pairing) is NOT proven by this case. It is a
// structural invariant of production (sched_ctx is reached only via the local
// ws / g_worker at every switch site — src/async/scheduler.cpp run_next_on,
// fiber_entry_bridge, await_completion_*, await_ready_flag) and is exercised
// across a legal E8 W1-suspend -> W0-resume migration by
// e8_t3_steal_run_suspend_wake_resume_on_thief (asserts wid_pre == wid_post on
// the thief). The test name is preserved for historical identity; do not read
// it as a C-A proof claim.
SLUICE_TEST_CASE(e7_t2_worker_local_scheduler_context) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<bool> flag_a{false}, flag_b{false};
    std::atomic<int> waiters_suspended{0};
    std::atomic<bool> a_resumed{false};
    std::atomic<bool> b_resumed{false};

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        waiters_suspended.fetch_add(1, std::memory_order_acq_rel);
        sched.await_ready_flag(flag_a);
        a_resumed.store(true, std::memory_order_release);
    });
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        waiters_suspended.fetch_add(1, std::memory_order_acq_rel);
        sched.await_ready_flag(flag_b);
        b_resumed.store(true, std::memory_order_release);
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

    // Load-bearing: both waiters reached their post-resume point (the readiness
    // drain woke them and they resumed), and all three Fibers completed.
    SLUICE_CHECK(a_resumed.load());
    SLUICE_CHECK(b_resumed.load());
    SLUICE_CHECK(fa.state() == FiberState::done);
    SLUICE_CHECK(fb.state() == FiberState::done);
    SLUICE_CHECK(fset.state() == FiberState::done);
    SLUICE_CHECK(sched.waiting_ready_count() == 0);  // both registrations erased on wake
}

// ---- E7-T3: pinned resume — Fiber resumes on its owning Worker -----------
// Fiber A starts on Worker 0 (round-robin: first spawned → worker 0). A
// suspends on a ready flag. A different Worker (Worker 1) observes the flag
// becomes ready (via the readiness drain) and routes A back to Worker 0.
// Assert: A resumes on the same OS thread as Worker 0 (its owner).
SLUICE_TEST_CASE(e7_t3_pinned_resume_on_owning_worker) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<bool> flag_a{false};
    std::thread::id a_tid_pre{}, a_tid_post{};
    std::atomic<bool> a_suspended{false};

    // Fiber A: starts on Worker 0 (first spawned, round-robin → worker 0).
    Fiber fa;
    fa.set_entry([&](Fiber&) {
        a_tid_pre = std::this_thread::get_id();
        a_suspended.store(true);
        sched.await_ready_flag(flag_a);
        a_tid_post = std::this_thread::get_id();
    });
    // Setter Fiber: makes flag_a ready (starts on Worker 1 — second spawned).
    Fiber fset;
    fset.set_entry([&](Fiber&) {
        while (!a_suspended.load()) {}
        flag_a.store(true, std::memory_order_release);
    });
    FiberStack sa, sset;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fset, sset.base(), sset.size()));
    sched.spawn(fa);   // → Worker 0
    sched.spawn(fset); // → Worker 1
    sched.run(2);

    SLUICE_CHECK(a_tid_pre == a_tid_post);  // resumed on same OS thread (Worker 0)
    SLUICE_CHECK(fa.state() == FiberState::done);
    SLUICE_CHECK(fset.state() == FiberState::done);
}

// ---- E7-T7: internal Worker notification — cross-worker routed work -------
// Worker 1 (setter) makes a flag ready for Fiber A owned by Worker 0. Worker 0
// must observe the routed runnable work and resume A. Assert: A resumes on
// its owner (Worker 0); the setter runs to completion.
//
// E8 NOTE: E7-T7 originally asserted `a_tid != setter_tid` ("ran on different
// Workers") as a proxy for "both fibers ran." E8 introduces work stealing
// (ADR §9.3), so the setter — initially assigned to Worker 1 — may be STOLEN
// by Worker 0 (which is idle while A waits). Under E8 this is correct:
// ownership transfers to the thief. The load-bearing E7-T7 claim (cross-
// worker routed wake: W0's A is woken by a flag and resumes on W0) is
// unchanged and still asserted. The "different worker" proxy is dropped as
// E8-invalid. The E7-pinned variant is preserved in e7_dup_publication_test
// semantics; E8 stealing is proven in e8_steal_test.
SLUICE_TEST_CASE(e7_t7_internal_worker_notification) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<bool> flag_a{false};
    std::thread::id a_tid{}, a_tid_post{};
    std::atomic<bool> a_suspended{false};
    std::atomic<bool> setter_ran{false};

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        a_tid = std::this_thread::get_id();
        a_suspended.store(true);
        sched.await_ready_flag(flag_a);
        a_tid_post = std::this_thread::get_id();
    });
    Fiber fset;
    fset.set_entry([&](Fiber&) {
        while (!a_suspended.load()) {}
        flag_a.store(true, std::memory_order_release);
        setter_ran.store(true);
    });
    FiberStack sa, sset;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fset, sset.base(), sset.size()));
    sched.spawn(fa);   // → Worker 0
    sched.spawn(fset); // → Worker 1
    sched.run(2);

    // Load-bearing E7-T7 claim: A resumed on its owning Worker 0 (pinned
    // wake routing preserved even under E8 stealing — a stolen Fiber's
    // wake routes to its CURRENT owner, and A was never stolen: it
    // suspended on W0 and woke on W0).
    SLUICE_CHECK(a_tid == a_tid_post);
    SLUICE_CHECK(setter_ran.load());
    SLUICE_CHECK(fa.state() == FiberState::done);
    SLUICE_CHECK(fset.state() == FiberState::done);
}

// ---- E7-T10A: persistent ready flag observed before idle admission --------
// Fiber A on Worker 0 waits on flag X; Fiber B on Worker 1 makes X ready.
// Before MW-S3/quiescence admission, X readiness is observed; A is routed to
// Worker 0; A resumes. Assert: A resumes; no stalled state.
SLUICE_TEST_CASE(e7_t10a_persistent_flag_before_idle_admission) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<bool> flag_x{false};
    std::atomic<bool> a_suspended{false};
    int a_resumed = 0;

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        a_suspended.store(true);
        sched.await_ready_flag(flag_x);
        a_resumed = 1;
    });
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        while (!a_suspended.load()) {}
        flag_x.store(true, std::memory_order_release);
    });
    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));
    sched.spawn(fa);   // → Worker 0
    sched.spawn(fb);   // → Worker 1
    sched.run(2);

    SLUICE_CHECK(a_resumed == 1);
    SLUICE_CHECK(fa.state() == FiberState::done);
    SLUICE_CHECK(fb.state() == FiberState::done);
    SLUICE_CHECK(sched.waiting_ready_count() == 0);  // registration erased on wake
}

SLUICE_MAIN()
