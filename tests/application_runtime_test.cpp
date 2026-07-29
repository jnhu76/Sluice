// E16 ApplicationRuntime lifecycle tests.
// ADR: docs/adr/ADR-application-runtime.md (Accepted).
// Design: docs/design/e16-application-runtime.md.
//
// Tests the basic lifecycle: construct → start → submit → request_stop →
// drain → join → Stopped. Uses FakeAsyncBackend for deterministic I/O.
#include "harness.hpp"

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/fake_backend.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// ---- Builder validation -----------------------------------------------------

SLUICE_TEST_CASE(runtime_builder_no_backend_fails) {
    RuntimeBuilder builder;
    auto result = builder.build();
    SLUICE_CHECK(!result.has_value());
    SLUICE_CHECK(result.error().code == IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(runtime_builder_with_backend_succeeds) {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<FakeAsyncBackend>());
    auto result = builder.build();
    SLUICE_CHECK(result.has_value());
    // Runtime is in Constructed state; destructor is safe.
}

// ---- Basic lifecycle --------------------------------------------------------

SLUICE_TEST_CASE(runtime_start_from_constructed) {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<FakeAsyncBackend>());
    auto rt_result = builder.build();
    SLUICE_CHECK(rt_result.has_value());
    auto& rt = *rt_result.value();

    auto start_result = rt.start();
    SLUICE_CHECK(start_result.has_value());

    // Clean shutdown.
    auto shutdown_result = rt.shutdown();
    SLUICE_CHECK(shutdown_result.has_value());
}

SLUICE_TEST_CASE(runtime_double_start_fails) {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<FakeAsyncBackend>());
    auto rt_result = builder.build();
    SLUICE_CHECK(rt_result.has_value());
    auto& rt = *rt_result.value();

    SLUICE_CHECK(rt.start().has_value());
    // Second start should fail (not in Constructed).
    auto second = rt.start();
    SLUICE_CHECK(!second.has_value());
    SLUICE_CHECK(second.error().code == IoError::Code::invalid_state);

    SLUICE_CHECK(rt.shutdown().has_value());
}

// ---- Submit and drain -------------------------------------------------------

SLUICE_TEST_CASE(runtime_submit_and_drain) {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<FakeAsyncBackend>());
    auto rt_result = builder.build();
    SLUICE_CHECK(rt_result.has_value());
    auto& rt = *rt_result.value();

    SLUICE_CHECK(rt.start().has_value());

    // Submit a task that sets a flag.
    std::atomic<bool> task_ran{false};
    auto submit_result = rt.submit([&task_ran](RuntimeTaskContext& ctx) {
        (void)ctx;
        task_ran.store(true, std::memory_order::release);
    });
    SLUICE_CHECK(submit_result.has_value());

    // Request stop and drain.
    rt.request_stop();
    auto drain_result = rt.drain();
    SLUICE_CHECK(drain_result.has_value());

    // Task should have run.
    SLUICE_CHECK(task_ran.load(std::memory_order::acquire));

    // Join and verify Stopped.
    auto join_result = rt.join();
    SLUICE_CHECK(join_result.has_value());
}

SLUICE_TEST_CASE(runtime_submit_after_stop_fails) {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<FakeAsyncBackend>());
    auto rt_result = builder.build();
    SLUICE_CHECK(rt_result.has_value());
    auto& rt = *rt_result.value();

    SLUICE_CHECK(rt.start().has_value());
    rt.request_stop();

    // Submit after stop should fail (admission closed).
    auto submit_result = rt.submit([](RuntimeTaskContext&) {});
    SLUICE_CHECK(!submit_result.has_value());
    SLUICE_CHECK(submit_result.error().code == IoError::Code::invalid_state);

    SLUICE_CHECK(rt.shutdown().has_value());
}

// ---- Stop before start ------------------------------------------------------

SLUICE_TEST_CASE(runtime_stop_before_start) {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<FakeAsyncBackend>());
    auto rt_result = builder.build();
    SLUICE_CHECK(rt_result.has_value());
    auto& rt = *rt_result.value();

    // Request stop before start.
    rt.request_stop();

    // Start should return canceled.
    auto start_result = rt.start();
    SLUICE_CHECK(!start_result.has_value());
    SLUICE_CHECK(start_result.error().code == IoError::Code::canceled);
}

// ---- Shutdown in Constructed ------------------------------------------------

SLUICE_TEST_CASE(runtime_shutdown_constructed) {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<FakeAsyncBackend>());
    auto rt_result = builder.build();
    SLUICE_CHECK(rt_result.has_value());
    auto& rt = *rt_result.value();

    // Shutdown without start should succeed (direct close).
    auto shutdown_result = rt.shutdown();
    SLUICE_CHECK(shutdown_result.has_value());
}

// ---- Shutdown idempotent ----------------------------------------------------

SLUICE_TEST_CASE(runtime_shutdown_idempotent) {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<FakeAsyncBackend>());
    auto rt_result = builder.build();
    SLUICE_CHECK(rt_result.has_value());
    auto& rt = *rt_result.value();

    SLUICE_CHECK(rt.start().has_value());
    SLUICE_CHECK(rt.shutdown().has_value());
    // Second shutdown should be idempotent.
    SLUICE_CHECK(rt.shutdown().has_value());
}

// ---- Drain before stop fails ------------------------------------------------

SLUICE_TEST_CASE(runtime_drain_in_running_fails) {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<FakeAsyncBackend>());
    auto rt_result = builder.build();
    SLUICE_CHECK(rt_result.has_value());
    auto& rt = *rt_result.value();

    SLUICE_CHECK(rt.start().has_value());

    // Drain in Running should fail (must request_stop first).
    auto drain_result = rt.drain();
    SLUICE_CHECK(!drain_result.has_value());
    SLUICE_CHECK(drain_result.error().code == IoError::Code::invalid_state);

    SLUICE_CHECK(rt.shutdown().has_value());
}

// ---- Multiple tasks ---------------------------------------------------------

SLUICE_TEST_CASE(runtime_multiple_tasks) {
    RuntimeBuilder builder;
    builder.backend(std::make_unique<FakeAsyncBackend>());
    auto rt_result = builder.build();
    SLUICE_CHECK(rt_result.has_value());
    auto& rt = *rt_result.value();

    SLUICE_CHECK(rt.start().has_value());

    constexpr int N = 5;
    std::atomic<int> count{0};
    for (int i = 0; i < N; ++i) {
        auto sr = rt.submit([&count](RuntimeTaskContext&) {
            count.fetch_add(1, std::memory_order::relaxed);
        });
        SLUICE_CHECK(sr.has_value());
    }

    rt.request_stop();
    SLUICE_CHECK(rt.drain().has_value());
    SLUICE_CHECK(count.load(std::memory_order::acquire) == N);
    SLUICE_CHECK(rt.join().has_value());
}

SLUICE_MAIN()
