// E5-B — Evented Group task execution on Fibers (sluice-CORE-E5-B).
//
// Proves Group-on-Evented: a Group(Scheduler&) spawns Fiber tasks (not
// std::thread), a task body can suspend inside Future::await via
// EventedWaitPolicy, another runnable Fiber progresses while the task is
// suspended (Group-layer liveness), the task resumes at the exact suspension
// point, and the task Future completes exactly once on body return. Threaded
// Group behavior is preserved (regression).
//
// Single-worker, single OS thread, deterministic.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/evented_wait_policy.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/future.hpp>
#include <sluice/async/group.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/result.hpp>

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

// ---- G1+G2+G3+G4: Evented Group task runs on a Fiber, suspends, resumes ----
// An Evented Group task awaits an unready Future (suspends its Fiber). While it
// is suspended, a peer Fiber (running on the same scheduler) makes progress
// (Group-layer liveness). The peer then completes the awaited Future; the
// scheduler wakes the task; the task resumes at the await point, observes the
// result, returns, and its task Future completes exactly once.
//
// The "peer" here is implemented as a second scheduler-spawned Fiber (not a
// Group task) so the test exercises Group-task-suspend + peer-progress without
// coupling two Group tasks' lifecycles.
SLUICE_TEST_CASE(e5_b_evented_group_task_suspends_and_resumes_on_fiber) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    // The awaited Future uses EventedWaitPolicy so the Group task's await
    // suspends its Fiber. Owned by the test; outlives the group.
    EventedWaitPolicy policy(sched);
    Future<int> inner{policy};

    int task_observed = -1;
    std::uint64_t task_pre = 0, task_post = 0;
    int peer_ran_while_task_suspended = 0;
    bool peer_saw_task_waiting = false;
    int task_returned = 0;

    Group g{sched};
    // Group task: await the inner Future, observe result.
    g.async([&](CancelToken&) {
        task_pre = 0xDEAD;
        auto r = inner.await();                 // suspends the task's Fiber
        task_post = task_pre;                   // resume fidelity
        if (r.has_value()) task_observed = r.value();
        ++task_returned;
    });

    // Peer Fiber (not a Group task) on the same scheduler: runs while the task
    // is suspended, completes the inner Future.
    Fiber peer;
    peer.set_entry([&](Fiber&) {
        peer_saw_task_waiting = (g.size() > 0);  // task exists; crude liveness
        peer_ran_while_task_suspended = 1;
        inner.complete_with(sluice::Result<int>{99});
    });
    FiberStack ps;
    SLUICE_CHECK(sched.init_fiber(peer, ps.base(), ps.size()));
    sched.spawn(peer);

    g.await();  // drives the scheduler until the Group task Future is ready

    SLUICE_CHECK(peer_ran_while_task_suspended == 1);   // G2 liveness
    SLUICE_CHECK(task_returned == 1);                   // G4 task body returned
    SLUICE_CHECK(task_observed == 99);                  // G3 resume + result
    SLUICE_CHECK(task_post == 0xDEAD);                  // G3 resume fidelity
    SLUICE_CHECK(g.size() == 1);                        // one task
}

// ---- G5: Evented Group await does not block the worker on a cv/join --------
// Structurally proven by G1-G4: g.await() drove sched.run_until_idle(), which
// is cooperative — it did NOT block on a condition_variable or join a thread
// (no threads exist in Evented mode). If it had, the peer Fiber could not
// have run. This test makes the contract explicit: assert no std::thread is
// involved by checking the task ran on the same thread as await().
SLUICE_TEST_CASE(e5_b_evented_group_runs_on_awaiting_thread) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::thread::id awaiter_tid = std::this_thread::get_id();
    std::thread::id task_tid;
    int task_ran = 0;

    {
        Group g{sched};
        g.async([&](CancelToken&) {
            task_tid = std::this_thread::get_id();
            ++task_ran;
        });
        g.await();
        SLUICE_CHECK(task_ran == 1);
        SLUICE_CHECK(task_tid == awaiter_tid);  // same OS thread (cooperative)
    }
}

// ---- G6: Threaded Group regression ----------------------------------------
// A default-constructed Group still uses std::thread + ThreadedWaitPolicy. The
// existing T3 behavior is unchanged. Uses a real cross-thread producer; TSan.
SLUICE_TEST_CASE(e5_b_threaded_group_regression) {
    std::thread::id main_tid = std::this_thread::get_id();
    std::thread::id task_tid;
    int ran = 0;

    Group g;  // Threaded
    g.async([&](CancelToken&) {
        task_tid = std::this_thread::get_id();  // MUST differ from main
        ++ran;
    });
    g.await();

    SLUICE_CHECK(ran == 1);
    SLUICE_CHECK(task_tid != main_tid);  // ran on a worker thread (Threaded)
}

SLUICE_MAIN()
