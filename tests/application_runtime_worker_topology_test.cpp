// Issue #50 ApplicationRuntime drain-hang regression.
#include "async_test_control.hpp"
#include "harness.hpp"

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/fake_backend.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>

using namespace sluice::async;

namespace {

bool run_runtime_topology_case(unsigned worker_count, unsigned task_count) {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<FakeAsyncBackend>()).workers(worker_count);
    auto built = builder.build();
    if (!built.has_value()) {
        return false;
    }
    auto& runtime = *built.value();
    Scheduler& sched = runtime.test_scheduler_for_worker_topology();

    std::atomic<unsigned> executions{0};
    bool start_ok = false;
    bool submissions_ok = true;
    bool drain_ok = false;
    {
        sluice_async_test::ControllerGuard controller(sched);
        sluice_async_test::WorkerTopologySeam::arm_mutation(sched);
        sluice_async_test::WorkerTopologySeam::arm_ready_before_start(sched);

        start_ok = runtime.start().has_value();
        if (!start_ok) {
            return false;
        }
        sluice_async_test::WorkerTopologySeam::wait_mutation_paused(sched);

        std::thread submitter([&] {
            for (unsigned i = 0; i < task_count; ++i) {
                auto submitted = runtime.submit([&](RuntimeTaskContext&) {
                    executions.fetch_add(1, std::memory_order_acq_rel);
                });
                if (!submitted.has_value()) {
                    submissions_ok = false;
                    return;
                }
            }
        });
        sluice_async_test::WorkerTopologySeam::wait_reader_attempt(sched);
        sluice_async_test::WorkerTopologySeam::release_mutation(sched);
        sluice_async_test::WorkerTopologySeam::wait_ready_before_start_paused(sched);
        submitter.join();
        sluice_async_test::WorkerTopologySeam::release_ready_before_start(sched);

        runtime.request_stop();
        drain_ok = runtime.drain().has_value();
    }

    const bool join_ok = runtime.join().has_value();
    return start_ok && submissions_ok && drain_ok && join_ok &&
           executions.load(std::memory_order_acquire) == task_count;
}

bool run_runtime_post_join_submit_case() {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<FakeAsyncBackend>()).workers(2);
    auto built = builder.build();
    if (!built.has_value()) {
        return false;
    }
    auto& runtime = *built.value();
    Scheduler& sched = runtime.test_scheduler_for_worker_topology();

    std::atomic<unsigned> executions{0};
    bool submit_ok = false;
    bool drain_ok = false;
    {
        sluice_async_test::ControllerGuard controller(sched);
        sluice_async_test::WorkerTopologySeam::arm_joined_before_unpublish(sched);
        if (!runtime.start().has_value()) {
            return false;
        }
        sluice_async_test::WorkerTopologySeam::wait_joined_before_unpublish_paused(sched);

        submit_ok = runtime
                        .submit([&](RuntimeTaskContext&) {
                            executions.fetch_add(1, std::memory_order_acq_rel);
                        })
                        .has_value();

        sluice_async_test::WorkerTopologySeam::release_joined_before_unpublish(sched);
        runtime.request_stop();
        drain_ok = runtime.drain().has_value();
    }

    const bool join_ok = runtime.join().has_value();
    return submit_ok && drain_ok && join_ok && executions.load(std::memory_order_acquire) == 1;
}

} // namespace

SLUICE_TEST_CASE(runtime_initial_topology_growth_concurrent_submit_drains) {
    SLUICE_CHECK(run_runtime_topology_case(1, 1));
    SLUICE_CHECK(run_runtime_topology_case(1, 8));
    SLUICE_CHECK(run_runtime_topology_case(2, 8));
    SLUICE_CHECK(run_runtime_topology_case(4, 12));
}

SLUICE_TEST_CASE(runtime_post_join_submit_reenters_without_losing_epoch) {
    SLUICE_CHECK(run_runtime_post_join_submit_case());
}

SLUICE_MAIN()
