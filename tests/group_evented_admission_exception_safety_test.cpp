// P2-01 (Group transactional admission seam, §13.5) regression —
// Group::async_evented must be exception-safe when bookkeeping allocation
// fails, making one Evented task admission a complete transaction:
//
//   prepare complete task resources
//     -> reserve all Group bookkeeping capacity
//     -> commit all ownership records
//     -> release Group mutex
//     -> Scheduler::spawn
//
// The implementation now performs ALL fallible capacity preparation (the three
// vector reserves) BEFORE the first push_back, in a single mtx_ critical
// section. The three push_backs are then guaranteed not to allocate (sufficient
// capacity), and their moved types are noexcept-movable, so the commit block
// cannot throw. Therefore either ALL three ownership records commit (and only
// then does Scheduler::spawn publish the Fiber) or NONE commit.
//
// Test seam: under SLUICE_ASYNC_INTERNAL_TESTING, Group exposes
// test_set_evented_admission_fail(EventedAdmissionFailPoint) that throws
// std::bad_alloc at a specific reserve boundary on the NEXT async_evented
// call (one-shot). This file links sluice_async_internal_testing; production
// builds compile the seam out.
//
// Coverage (§7.1–7.7):
//   - §7.1 each reserve fail point on the FIRST admission
//   - §7.2 second admission fails; first task stays valid
//   - §7.3 Group remains reusable after a failure
//   - §7.4 destructor is safe after a failed admission (no await)
//   - §7.5 init_fiber regression is covered by threaded_evented_internal_test
//   - §7.6 Threaded regression is covered by group_exception_safety_test
//   - §7.7 normal Evented path is covered by evented_group_test
//
// Deterministic causal tests: NO sleep_for is used as proof of ordering,
// liveness, or absence of a lost wake. The fail-point seam is one-shot and is
// consumed on use, so the test always observes the exact admission boundary.
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

#include <atomic>
#include <new>       // std::bad_alloc
#include <vector>

using namespace sluice::async;

namespace {
// Fail points to iterate over in the parameterized cases.
const std::vector<Group::EventedAdmissionFailPoint> kAllFailPoints = {
    Group::EventedAdmissionFailPoint::before_fiber_storage_reserve,
    Group::EventedAdmissionFailPoint::before_stack_storage_reserve,
    Group::EventedAdmissionFailPoint::before_future_storage_reserve,
};
}  // namespace

// ============================================================================
// §7.1 — each reserve fail point on the FIRST admission.
// A failed first admission means: the task body did NOT run, the Group has no
// tracked task (size == 0, storage snapshot {0,0,0}), the Scheduler received
// no new runnable Fiber, and the Group is safely destructible.
// ============================================================================

SLUICE_TEST_CASE(egroup_es_first_admission_fail_at_each_reserve_safe) {
    if constexpr (!fiber_ctx::supported) return;

    for (auto fp : kAllFailPoints) {
        AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
        Scheduler sched(ctx);
        Group g{sched};

        std::atomic<int> ran{0};

        // Preconditions.
        SLUICE_CHECK(g.size() == 0);
        auto snap0 = g.test_evented_storage_snapshot();
        SLUICE_CHECK(snap0.fibers == 0 && snap0.stacks == 0 && snap0.futures == 0);
        std::size_t runnable_before = sched.runnable_count();

        // Arm the one-shot failure at this reserve boundary.
        g.test_set_evented_admission_fail(fp);
        SLUICE_CHECK(g.test_evented_admission_fail() == fp);

        bool caught = false;
        try {
            g.async([&](CancelToken&) { ++ran; });
        } catch (const std::bad_alloc&) {
            caught = true;
        }

        // The seam is consumed (one-shot).
        SLUICE_CHECK(g.test_evented_admission_fail() ==
                     Group::EventedAdmissionFailPoint::none);

        SLUICE_CHECK_MSG(caught, "first admission must throw std::bad_alloc");
        // Task body did NOT execute.
        SLUICE_CHECK(ran.load() == 0);
        // No tracked task.
        SLUICE_CHECK(g.size() == 0);
        // No partial task record: all three storage sizes still 0.
        auto snap = g.test_evented_storage_snapshot();
        SLUICE_CHECK_MSG(snap.fibers == 0, "no partial fiber record");
        SLUICE_CHECK_MSG(snap.stacks == 0, "no partial stack record");
        SLUICE_CHECK_MSG(snap.futures == 0, "no partial future record");
        // Scheduler received no new runnable Fiber.
        SLUICE_CHECK(sched.runnable_count() == runnable_before);
        // Scope ends here; dtor must return normally (failed task was not
        // admitted, so the Evented destructor's fail-fast cannot fire).
    }
}

// ============================================================================
// §7.2 — the SECOND admission fails at each reserve; the first task stays
// valid and is the only task that runs. After await the Group is fully reaped.
// ============================================================================

SLUICE_TEST_CASE(egroup_es_second_admission_fail_keeps_first_valid) {
    if constexpr (!fiber_ctx::supported) return;

    for (auto fp : kAllFailPoints) {
        AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
        Scheduler sched(ctx);
        Group g{sched};

        std::atomic<int> first_ran{0};
        std::atomic<int> second_ran{0};

        // First admission succeeds normally.
        g.async([&](CancelToken&) { ++first_ran; });
        SLUICE_CHECK(g.size() == 1);
        auto snap1 = g.test_evented_storage_snapshot();
        SLUICE_CHECK_MSG(snap1.fibers == 1, "first fiber committed");
        SLUICE_CHECK_MSG(snap1.stacks == 1, "first stack committed");
        SLUICE_CHECK_MSG(snap1.futures == 1, "first future committed");

        std::size_t runnable_after_first = sched.runnable_count();

        // Arm failure for the second admission at this reserve boundary.
        g.test_set_evented_admission_fail(fp);

        bool caught = false;
        try {
            g.async([&](CancelToken&) { ++second_ran; });
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        SLUICE_CHECK(caught);
        SLUICE_CHECK(g.test_evented_admission_fail() ==
                     Group::EventedAdmissionFailPoint::none);

        // First task remains the only tracked task; second task body never ran.
        SLUICE_CHECK(g.size() == 1);
        SLUICE_CHECK(second_ran.load() == 0);
        auto snap2 = g.test_evented_storage_snapshot();
        SLUICE_CHECK_MSG(snap2.fibers == 1, "failed admission left no extra fiber");
        SLUICE_CHECK_MSG(snap2.stacks == 1, "failed admission left no extra stack");
        SLUICE_CHECK_MSG(snap2.futures == 1, "failed admission left no extra future");
        // No second Fiber was published to the Scheduler.
        SLUICE_CHECK(sched.runnable_count() == runnable_after_first);

        // Drain the (only) first task.
        g.await();

        SLUICE_CHECK(first_ran.load() == 1);
        SLUICE_CHECK(second_ran.load() == 0);
        // Reaped after a successful await.
        SLUICE_CHECK(g.size() == 0);
        auto snap3 = g.test_evented_storage_snapshot();
        SLUICE_CHECK(snap3.fibers == 0 && snap3.stacks == 0 && snap3.futures == 0);
    }
}

// ============================================================================
// §7.3 — the Group remains reusable after a failed admission: clear the seam,
// admit a task normally, await, and prove it ran exactly once.
// ============================================================================

SLUICE_TEST_CASE(egroup_es_reusable_after_admission_failure) {
    if constexpr (!fiber_ctx::supported) return;

    for (auto fp : kAllFailPoints) {
        AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
        Scheduler sched(ctx);
        Group g{sched};

        std::atomic<int> ran{0};

        // Failed admission.
        g.test_set_evented_admission_fail(fp);
        bool caught = false;
        try {
            g.async([&](CancelToken&) { ++ran; });
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        SLUICE_CHECK(caught);
        SLUICE_CHECK(g.size() == 0);
        SLUICE_CHECK(ran.load() == 0);

        // Clear/consume the seam (already one-shot-consumed) and admit normally.
        g.test_set_evented_admission_fail(Group::EventedAdmissionFailPoint::none);
        g.async([&](CancelToken&) { ++ran; });
        SLUICE_CHECK(g.size() == 1);

        g.await();

        SLUICE_CHECK(ran.load() == 1);  // exactly once
        SLUICE_CHECK(g.size() == 0);
    }
}

// ============================================================================
// §7.4 — destructor is safe after a failed FIRST admission when await() is
// never called. Because the task was not admitted, the Evented destructor's
// fail-fast (pending Future) cannot fire, and there are no Fibers to reap.
// ============================================================================

SLUICE_TEST_CASE(egroup_es_destructor_safe_after_failed_first_admission) {
    if constexpr (!fiber_ctx::supported) return;

    for (auto fp : kAllFailPoints) {
        std::atomic<int> ran{0};
        AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
        Scheduler sched(ctx);
        {
            Group g{sched};
            g.test_set_evented_admission_fail(fp);
            bool caught = false;
            try {
                g.async([&](CancelToken&) { ++ran; });
            } catch (const std::bad_alloc&) {
                caught = true;
            }
            SLUICE_CHECK(caught);
            SLUICE_CHECK(g.size() == 0);
            // Leave scope WITHOUT calling await(). Dtor runs here.
        }
        SLUICE_CHECK(ran.load() == 0);
    }
}

// ============================================================================
// §7.7 supplement — the normal Evented admission contract still holds after
// the reserve-before-commit restructure: a task runs exactly once, its Future
// is terminal after await, repeated await is idempotent, and a task body
// exception is swallowed at the Group boundary (cancel-propagation boundary).
// (evented_group_test.cpp already covers suspend/resume; this pins the few
// admission-specific post-conditions the restructure touches.)
// ============================================================================

SLUICE_TEST_CASE(egroup_es_normal_admission_runs_once_and_awaits_cleanly) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    Group g{sched};

    std::atomic<int> ran{0};
    g.async([&](CancelToken&) { ++ran; });
    SLUICE_CHECK(g.size() == 1);

    g.await();
    SLUICE_CHECK(ran.load() == 1);
    SLUICE_CHECK(g.size() == 0);
    auto snap = g.test_evented_storage_snapshot();
    SLUICE_CHECK(snap.fibers == 0 && snap.stacks == 0 && snap.futures == 0);

    // Repeated await is idempotent (no tasks, no-op).
    g.await();
    SLUICE_CHECK(ran.load() == 1);
    SLUICE_CHECK(g.size() == 0);
}

SLUICE_TEST_CASE(egroup_es_task_body_exception_swallowed_at_boundary) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    Group g{sched};

    std::atomic<int> ran{0};
    g.async([&](CancelToken&) {
        ++ran;
        throw std::runtime_error("intentional");  // swallowed by the body wrapper
    });
    g.await();  // returns cleanly
    SLUICE_CHECK(ran.load() == 1);
    SLUICE_CHECK(g.size() == 0);
}

SLUICE_MAIN()
