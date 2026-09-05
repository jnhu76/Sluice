// Issue #223 / R-F1 deterministic startup-skew correspondence witness.
//
// The E9 TLA+ model (spec/tla/e9_park_wake, R-F1 startup refinement) models
// the per-worker startup publication (StartWorker / the Eligible authority)
// and proves across it that the MW-S2 election picks the lowest-id
// STARTED worker: with W0 configured-but-unstarted, W1 legitimately elects
// as the backend participant (the #223/#210 skew shape). This test pins the
// SAME C++ fact deterministically:
//
//   1. run_impl stores each worker's `active` INSIDE its own thread
//      (scheduler.cpp run_impl multi-worker lambda), so a configured worker
//      whose thread has not published is invisible to the election scan
//      (scheduler.cpp:706-717 reads `active`).
//   2. The WorkerStartupSeam holds worker 0 strictly BEFORE its publication
//      while worker 1 publishes and drives the MW-S2 admission: the elected
//      participant is worker 1 (observed via
//      AsyncTestAccess::elected_participant_id while worker 1 is paused at
//      the mw_s2_committed_before_wait_one seam — outside global_mtx_ — and
//      worker 0 is STILL held, so the election necessarily scanned
//      active[0] == false).
//   3. After both seams release, the invocation completes: worker 1's
//      no-progress terminate retires it, worker 0's (then-starting) thread
//      observes the published terminate and retires, and run_impl returns —
//      the join boundary (scheduler.cpp:472-475): the run cannot return
//      before EVERY configured thread has run, which is the C++ counterpart
//      of the model's Settled-gated terminal classifications.
//
// Deterministic (per-worker causal seams + role pinning); NO sleep-ordering.
// The #210 forensic reproduction of this shape was load-dependent; this test
// upgrades it to a schedule-pinned regression.
#include "async_test_control.hpp"
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/scheduler.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace sa = sluice::async;
namespace stest = sluice_async_test;

using Scheduler = sa::Scheduler;

namespace {

// Fail-safe runner guard (#210 discipline): if a SLUICE_CHECK fires while the
// runner is joinable, the destructor releases every armed seam, joins, and
// runs the post-join drain — an early return must never destroy a joinable
// std::thread (std::terminate) nor leak the held backend op.
struct RunnerCleanup {
    std::thread& runner;
    std::function<void()> pre_join;
    std::function<void()> post_join;
    explicit RunnerCleanup(std::thread& r,
                           std::function<void()> pre = nullptr,
                           std::function<void()> post = nullptr)
        : runner(r), pre_join(std::move(pre)), post_join(std::move(post)) {}
    ~RunnerCleanup() {
        if (!runner.joinable()) return;  // testcase joined on the normal path
        if (pre_join) pre_join();
        runner.join();
        if (post_join) post_join();
    }
    RunnerCleanup(const RunnerCleanup&) = delete;
    RunnerCleanup& operator=(const RunnerCleanup&) = delete;
};

// Bounded wait for a seam pause (polling observes the persistent `paused`
// state the controller cv wait would; the 1 ms granularity proves nothing
// about ordering — the seam holds do). A miss fail-closes instead of hang.
bool wait_paused_bounded(Scheduler& s, stest::PhaseTag tag, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!stest::is_paused(s, tag)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "issue223: timed out waiting for pause %s\n",
                         what);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace

SLUICE_TEST_CASE(issue223_startup_skew_election) {
    auto backend = std::make_unique<sa::FakeAsyncBackend>();
    sa::FakeAsyncBackend* backend_ptr = backend.get();
    sa::AsyncIoContext ctx(std::move(backend));
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);

    // MW-S2 shape: one accepted-but-never-completed op (outstanding == 1,
    // legacy backend whose wait_one returns 0 promptly — no progress).
    sa::Completion<std::size_t> held;
    std::byte buf[4]{};
    SLUICE_CHECK(
        ctx.submit_read(sa::ReadOp{-1, buf, sizeof(buf), 0}, held).has_value());

    // Pin the roles BEFORE the run: worker 0 held before its startup
    // publication; the MW-S2 participant held after its commit, before
    // wait_one (outside global_mtx_ — the observation below may lock).
    stest::WorkerStartupSeam::arm_for_worker(sched, /*worker_id=*/0);
    stest::arm(sched, stest::PhaseTag::mw_s2_committed_before_wait_one);

    std::thread runner([&] { sched.run(2); });  // Drain mode
    // Fail-safe cleanup: release every armed seam, join the runner, drain
    // the held op so the Completion/context destruction contracts hold
    // (#210 RunnerCleanup discipline).
    RunnerCleanup cleanup(
        runner,
        [&] {
            stest::release(sched,
                           stest::PhaseTag::mw_s2_committed_before_wait_one);
            stest::WorkerStartupSeam::release(sched);
        },
        [&] {
            backend_ptr->complete_oldest_with_bytes(0);
            (void)ctx.poll();
        });

    // Worker 1 publishes and elects while worker 0 is held pre-publication.
    if (!wait_paused_bounded(sched,
                             stest::PhaseTag::mw_s2_committed_before_wait_one,
                             "participant commit")) {
        SLUICE_FAIL("issue223: no worker reached the MW-S2 commit");
    }
    // Worker 0 is STILL held — it never published while the election ran,
    // so the scan (which reads `active`) necessarily skipped it.
    if (!stest::WorkerStartupSeam::is_paused(sched)) {
        SLUICE_FAIL("issue223: worker 0 published before the election");
    }
    // The committed participant is worker 1 — the #223/#210 skew shape,
    // deterministically: the lowest-id STARTED worker, not the lowest id.
    SLUICE_CHECK(Scheduler::AsyncTestAccess::elected_participant_id(sched) == 1);

    // Release the participant: its wait_one returns 0 (no progress), it
    // publishes the no-progress terminate and retires. Then release worker
    // 0: its thread publishes `active` NOW, its loop observes the published
    // terminate, and it retires — the run returns only after both configured
    // threads have run (the join boundary).
    stest::release(sched, stest::PhaseTag::mw_s2_committed_before_wait_one);
    stest::WorkerStartupSeam::release(sched);
    runner.join();

    // Drain the intentionally-held op so the context's outstanding==0
    // destruction contract holds.
    backend_ptr->complete_oldest_with_bytes(0);
    (void)ctx.poll();
}

SLUICE_MAIN()
