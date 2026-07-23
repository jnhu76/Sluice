// E5-A2 — Evented Future await via EventedWaitPolicy (sluice-CORE-E5-A2).
//
// Proves the M3 model: Future<T> is awaited on the Evented Scheduler WITHOUT
// any Future state change, complete_with change, WaitPolicy signature change,
// waiter slot, or wake callback. The Scheduler's level-triggered ready-flag
// protocol (E5-A1) observes Future::ready_ and wakes the waiting Fiber.
//
// Single-worker, single OS thread, deterministic (no sleeps). Producer is a
// Fiber B running on the same scheduler (in-scheduler production), per the
// directive: do not use a cross-thread producer as the primary proof.
//
// PROVES: F1 suspend, F2 liveness (B progresses while A waits), F3 completion
//         resumes A, F4 resume fidelity, F5 idempotent repeated await, F6
//         Threaded regression.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/evented_wait_policy.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/future.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_policy.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
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

// ---- F1+F2+F3+F4: Evented Future await — suspend, liveness, resume, fidelity
// Fiber A awaits an unready Future (EventedWaitPolicy) and suspends. Fiber B
// runs (liveness: B progresses while A waits), then completes the Future; the
// Scheduler's next poll of &fut.ready_ wakes A; A resumes with the result and
// its pre-suspend local survives (resume fidelity).
SLUICE_TEST_CASE(e5_a2_evented_future_await_suspends_liveness_resumes_fidelity) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    EventedWaitPolicy policy(sched);
    Future<int> fut{policy};

    int a_resumed = 0;
    int a_result = -1;
    std::uint64_t a_pre = 0, a_post = 0;
    int b_ran_before_a_resume = 0;
    bool b_observed_a_waiting = false;

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        a_pre = 0xCAFE;
        // Evented await: suspends A on &fut.ready_ via the scheduler.
        auto r = fut.await();
        a_post = a_pre;                          // resume fidelity
        a_resumed = 1;
        if (r.has_value()) a_result = r.value();
    });
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        // Liveness: B runs while A is suspended inside Future::await.
        b_observed_a_waiting = (fa.state() == FiberState::waiting);
        b_ran_before_a_resume = (a_resumed == 0);
        // In-scheduler production: complete the Future. complete_with stores
        // fut.ready_=true (release); the Scheduler's next wake_ready_flags()
        // poll observes it and wakes A.
        fut.complete_with(sluice::Result<int>{77});
    });
    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));
    sched.spawn(fa);
    sched.spawn(fb);
    sched.run_until_idle();

    SLUICE_CHECK(b_ran_before_a_resume == 1);    // F2 liveness
    SLUICE_CHECK(b_observed_a_waiting);          // A was waiting when B ran
    SLUICE_CHECK(a_resumed == 1);                // F3 resume
    SLUICE_CHECK(a_result == 77);                // F3 result delivered
    SLUICE_CHECK(a_post == 0xCAFE);              // F4 resume fidelity
    SLUICE_CHECK(fa.state() == FiberState::done);
    SLUICE_CHECK(fb.state() == FiberState::done);
    SLUICE_CHECK(sched.waiting_count() == 0);    // registration cleared on wake
}

// ---- F5: idempotent repeated await after ready ----------------------------
// After A resumes, a second await returns the cached result without suspending.
// Future's documented idempotent-await contract is unchanged under Evented.
SLUICE_TEST_CASE(e5_a2_evented_future_repeated_await_idempotent) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    EventedWaitPolicy policy(sched);
    Future<int> fut{policy};

    int second_await_result = -1;
    int resume_count = 0;

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        (void)fut.await();            // first await — may suspend
        ++resume_count;
        auto r2 = fut.await();        // second await — must return cached, no suspend
        if (r2.has_value()) second_await_result = r2.value();
    });
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        fut.complete_with(sluice::Result<int>{123});
    });
    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));
    sched.spawn(fa);
    sched.spawn(fb);
    sched.run_until_idle();

    SLUICE_CHECK(resume_count == 1);              // first await returned once
    SLUICE_CHECK(second_await_result == 123);     // second await returned cached
}

// ---- F6: Threaded Future regression (Evented policy does not affect it) ----
// A Future with the default (Threaded) policy still works as before. This is
// the regression guard: the Evented policy is purely additive; Future's
// Threaded path is unchanged. Uses a real cross-thread producer (std::thread)
// because that's the Threaded model's contract — TSan applies here.
SLUICE_TEST_CASE(e5_a2_threaded_future_regression_still_works) {
    Future<int> fut;  // default = ThreadedWaitPolicy
    int result = -1;
    std::thread worker([&fut] {
        fut.complete_with(sluice::Result<int>{42});
    });
    auto r = fut.await();
    if (r.has_value()) result = r.value();
    worker.join();
    SLUICE_CHECK(result == 42);
}

SLUICE_MAIN()
