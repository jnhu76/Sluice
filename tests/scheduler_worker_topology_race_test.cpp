// Issue #50 deterministic worker-topology authority regression.
#include "async_test_control.hpp"
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

using namespace sluice::async;

namespace {

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    std::vector<std::byte> bytes = std::vector<std::byte>(kBytes);

    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

struct FiberBatch {
    explicit FiberBatch(std::size_t count) : fibers(count), stacks(count), executions(count) {
        for (auto& execution : executions) {
            execution.store(0, std::memory_order_relaxed);
        }
    }

    std::vector<Fiber> fibers;
    std::vector<FiberStack> stacks;
    std::vector<std::atomic<unsigned>> executions;
};

bool run_batch(Scheduler& sched, unsigned worker_count, FiberBatch& batch) {
    for (std::size_t i = 0; i < batch.fibers.size(); ++i) {
        batch.fibers[i].set_entry(
            [&, i](Fiber&) { batch.executions[i].fetch_add(1, std::memory_order_acq_rel); });
        if (!sched.init_fiber(batch.fibers[i], batch.stacks[i].base(), batch.stacks[i].size())) {
            return false;
        }
        sched.spawn(batch.fibers[i]);
    }
    sched.run(worker_count);
    for (std::size_t i = 0; i < batch.fibers.size(); ++i) {
        if (batch.executions[i].load(std::memory_order_acquire) != 1 ||
            batch.fibers[i].state() != FiberState::done) {
            return false;
        }
    }
    return true;
}

bool init_batch(Scheduler& sched, FiberBatch& batch) {
    for (std::size_t i = 0; i < batch.fibers.size(); ++i) {
        batch.fibers[i].set_entry(
            [&, i](Fiber&) { batch.executions[i].fetch_add(1, std::memory_order_acq_rel); });
        if (!sched.init_fiber(batch.fibers[i], batch.stacks[i].base(), batch.stacks[i].size())) {
            return false;
        }
    }
    return true;
}

bool batch_finished_once(const FiberBatch& batch) {
    for (std::size_t i = 0; i < batch.fibers.size(); ++i) {
        if (batch.executions[i].load(std::memory_order_acquire) != 1 ||
            batch.fibers[i].state() != FiberState::done) {
            return false;
        }
    }
    return true;
}

bool run_concurrent_growth_case(unsigned worker_count, bool targeted) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard controller(sched);
    FiberBatch batch(worker_count * 3);
    if (!init_batch(sched, batch)) {
        return false;
    }

    sluice_async_test::WorkerTopologySeam::arm_mutation(sched);
    sluice_async_test::WorkerTopologySeam::arm_ready_before_start(sched);
    std::thread runner([&] { sched.run(worker_count); });
    sluice_async_test::WorkerTopologySeam::wait_mutation_paused(sched);

    if (targeted) {
        sluice_async_test::WorkerTopologySeam::release_mutation(sched);
        sluice_async_test::WorkerTopologySeam::wait_ready_before_start_paused(sched);
        for (std::size_t i = 0; i < batch.fibers.size(); ++i) {
            sched.spawn_on(batch.fibers[i], static_cast<unsigned>(i % worker_count));
        }
    } else {
        std::thread submitter([&] {
            for (Fiber& fiber : batch.fibers) {
                sched.spawn(fiber);
            }
        });
        sluice_async_test::WorkerTopologySeam::wait_reader_attempt(sched);
        sluice_async_test::WorkerTopologySeam::release_mutation(sched);
        sluice_async_test::WorkerTopologySeam::wait_ready_before_start_paused(sched);
        submitter.join();
    }

    sluice_async_test::WorkerTopologySeam::release_ready_before_start(sched);
    runner.join();
    return batch_finished_once(batch);
}

} // namespace

SLUICE_TEST_CASE(topology_mutation_excludes_concurrent_spawn_reader) {
    if constexpr (!fiber_ctx::supported)
        return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard controller(sched);

    Fiber fiber;
    FiberStack stack;
    std::atomic<unsigned> executions{0};
    fiber.set_entry([&](Fiber&) { executions.fetch_add(1, std::memory_order_acq_rel); });
    SLUICE_CHECK(sched.init_fiber(fiber, stack.base(), stack.size()));

    sluice_async_test::WorkerTopologySeam::arm_mutation(sched);
    sluice_async_test::WorkerTopologySeam::arm_ready_before_start(sched);

    std::thread runner([&] { sched.run(1); });
    sluice_async_test::WorkerTopologySeam::wait_mutation_paused(sched);

    const bool topology_lock_was_available =
        sluice_async_test::WorkerTopologySeam::topology_lock_available(sched);

    std::thread submitter([&] { sched.spawn(fiber); });
    sluice_async_test::WorkerTopologySeam::wait_reader_attempt(sched);

    sluice_async_test::WorkerTopologySeam::release_mutation(sched);
    sluice_async_test::WorkerTopologySeam::wait_ready_before_start_paused(sched);
    submitter.join();
    sluice_async_test::WorkerTopologySeam::release_ready_before_start(sched);
    runner.join();

    SLUICE_CHECK_MSG(!topology_lock_was_available,
                     "global_mtx_ must be held while run_impl mutates workers_ topology");
    SLUICE_CHECK_MSG(executions.load(std::memory_order_acquire) == 1,
                     "concurrently admitted Fiber must execute exactly once");
    SLUICE_CHECK(fiber.state() == FiberState::done);
}

SLUICE_TEST_CASE(reentry_growth_and_smaller_run_use_only_participating_workers) {
    if constexpr (!fiber_ctx::supported)
        return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    FiberBatch first_one(1);
    FiberBatch second_one(1);
    FiberBatch grow_two(2);
    FiberBatch grow_four(4);
    SLUICE_CHECK(run_batch(sched, 1, first_one));
    SLUICE_CHECK(run_batch(sched, 1, second_one));
    SLUICE_CHECK(run_batch(sched, 2, grow_two));
    SLUICE_CHECK(run_batch(sched, 4, grow_four));

    FiberBatch shrink_to_two(4);
    const bool all_ran_with_two = run_batch(sched, 2, shrink_to_two);
    if (!all_ran_with_two) {
        // Failure cleanup for the pre-fix code: worker 2/3 may retain tickets
        // after run(2). Re-enter with all allocated workers before objects die.
        sched.run(4);
    }
    SLUICE_CHECK_MSG(all_ran_with_two,
                     "pre-run spawn after topology growth must route through the next run's "
                     "first-N participants");
}

SLUICE_TEST_CASE(concurrent_spawn_during_initial_growth_runs_exactly_once) {
    if constexpr (!fiber_ctx::supported)
        return;

    SLUICE_CHECK(run_concurrent_growth_case(1, false));
    SLUICE_CHECK(run_concurrent_growth_case(2, false));
    SLUICE_CHECK(run_concurrent_growth_case(4, false));
}

SLUICE_TEST_CASE(spawn_on_uses_published_participating_topology) {
    if constexpr (!fiber_ctx::supported)
        return;

    SLUICE_CHECK(run_concurrent_growth_case(1, true));
    SLUICE_CHECK(run_concurrent_growth_case(2, true));
    SLUICE_CHECK(run_concurrent_growth_case(4, true));
}

SLUICE_TEST_CASE(shrinking_reentry_routes_waiter_from_inactive_owner) {
    if constexpr (!fiber_ctx::supported)
        return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard controller(sched);

    std::atomic<bool> release_blockers{false};
    std::atomic<bool> ready{false};
    std::atomic<unsigned> resumed{0};
    FiberBatch batch(4);
    for (unsigned i = 0; i < 3; ++i) {
        batch.fibers[i].set_entry([&, i](Fiber&) {
            while (!release_blockers.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            batch.executions[i].fetch_add(1, std::memory_order_acq_rel);
        });
    }
    batch.fibers[3].set_entry([&](Fiber&) {
        sched.await_ready_flag(ready);
        resumed.fetch_add(1, std::memory_order_acq_rel);
    });
    for (std::size_t i = 0; i < batch.fibers.size(); ++i) {
        SLUICE_CHECK(
            sched.init_fiber(batch.fibers[i], batch.stacks[i].base(), batch.stacks[i].size()));
    }

    sluice_async_test::WorkerTopologySeam::arm_ready_before_start(sched);
    sluice_async_test::SuspendSeam::arm(sched);
    std::thread first_run([&] { sched.run(4); });
    sluice_async_test::WorkerTopologySeam::wait_ready_before_start_paused(sched);
    for (unsigned i = 0; i < 4; ++i) {
        sched.spawn_on(batch.fibers[i], i);
    }
    sluice_async_test::WorkerTopologySeam::release_ready_before_start(sched);
    sluice_async_test::SuspendSeam::wait_paused(sched);
    sluice_async_test::SuspendSeam::release(sched);
    release_blockers.store(true, std::memory_order_release);
    first_run.join();

    SLUICE_CHECK(batch.fibers[3].state() == FiberState::waiting);
    SLUICE_CHECK(sched.waiting_ready_count() == 1);

    ready.store(true, std::memory_order_release);
    sched.run(2);
    const bool resumed_with_two =
        resumed.load(std::memory_order_acquire) == 1 && batch.fibers[3].state() == FiberState::done;
    if (!resumed_with_two) {
        // Failure cleanup for the pre-fix route: the runnable ticket remains
        // on retained Worker 3, outside the run(2) participant snapshot.
        sched.run(4);
    }

    SLUICE_CHECK_MSG(resumed_with_two,
                     "a waiter owned by retained Worker 3 must migrate into the next run(2)");
    SLUICE_CHECK(sched.waiting_ready_count() == 0);
}

SLUICE_TEST_CASE(spawn_after_workers_join_is_deferred_to_next_run) {
    if constexpr (!fiber_ctx::supported)
        return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    FiberBatch batch(2);
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard controller(sched);
    SLUICE_CHECK(init_batch(sched, batch));

    sluice_async_test::WorkerTopologySeam::arm_joined_before_unpublish(sched);
    std::thread runner([&] { sched.run(2); });
    sluice_async_test::WorkerTopologySeam::wait_joined_before_unpublish_paused(sched);

    sched.spawn(batch.fibers[0]);
    sched.spawn(batch.fibers[1]);

    sluice_async_test::WorkerTopologySeam::release_joined_before_unpublish(sched);
    runner.join();

    sched.run(1);
    const bool all_ran_with_one = batch_finished_once(batch);
    SLUICE_CHECK_MSG(all_ran_with_one, "post-join spawn must be deferred to the next run topology");
}

SLUICE_MAIN()
