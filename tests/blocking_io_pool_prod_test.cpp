// Production BlockingIoPool contract tests (sluice-CORE-024S §8.1).
// TDD vertical slices — each test pins one contract row. The pool is the
// production sluice::BlockingIoPool (NOT the bench adapter). Pure C++17/20; no
// C runtime mixing in the test either.
#include "harness.hpp"

#include <sluice/blocking_io_pool.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace sluice;

SLUICE_TEST_CASE(make_pool_rejects_zero_worker_count) {
    auto r = make_blocking_io_pool(BlockingIoPoolOptions{0, 4});
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(make_pool_rejects_zero_queue_depth) {
    auto r = make_blocking_io_pool(BlockingIoPoolOptions{2, 0});
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(make_pool_constructs_valid_pool) {
    auto r = make_blocking_io_pool(BlockingIoPoolOptions{2, 8});
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value()->worker_count() == 2);
}

SLUICE_TEST_CASE(submit_surfaces_return_value) {
    auto pr = make_blocking_io_pool(BlockingIoPoolOptions{2, 8});
    SLUICE_CHECK(pr.has_value());
    auto& pool = *pr.value();
    auto t = pool.submit([] { return 42; });
    SLUICE_CHECK(t.has_value());
    SLUICE_CHECK(t.value().get() == 42);
}

SLUICE_TEST_CASE(submit_surfaces_exception) {
    auto pr = make_blocking_io_pool(BlockingIoPoolOptions{1, 4});
    SLUICE_CHECK(pr.has_value());
    auto& pool = *pr.value();
    auto t = pool.submit([]() -> int { throw std::runtime_error("boom"); });
    SLUICE_CHECK(t.has_value());
    bool caught = false;
    try {
        (void)t.value().get();
    } catch (const std::runtime_error& e) {
        caught = std::string(e.what()) == "boom";
    }
    SLUICE_CHECK(caught);
}

SLUICE_TEST_CASE(many_tasks_complete) {
    auto pr = make_blocking_io_pool(BlockingIoPoolOptions{4, 64});
    SLUICE_CHECK(pr.has_value());
    auto& pool = *pr.value();
    std::atomic<int> done{0};
    std::vector<Task<void>> tasks;
    for (int i = 0; i < 100; ++i) {
        auto t = pool.submit([&done] { done.fetch_add(1, std::memory_order_relaxed); });
        SLUICE_CHECK(t.has_value());
        tasks.emplace_back(std::move(t.value()));
    }
    for (auto& t : tasks) {
        t.get();
    }
    SLUICE_CHECK(done.load() == 100);
}

SLUICE_TEST_CASE(submit_after_shutdown_is_rejected) {
    auto pr = make_blocking_io_pool(BlockingIoPoolOptions{1, 4});
    SLUICE_CHECK(pr.has_value());
    auto& pool = *pr.value();
    pool.shutdown();
    auto t = pool.submit([] { return 1; });
    SLUICE_CHECK(!t.has_value());
    SLUICE_CHECK(t.error().code == IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(try_submit_rejects_when_full) {
    auto pr = make_blocking_io_pool(BlockingIoPoolOptions{1, 1});
    SLUICE_CHECK(pr.has_value());
    auto& pool = *pr.value();
    std::atomic<bool> release{false};
    std::atomic<bool> gate_started{false};
    // gate: occupy the single worker. Set gate_started so the test knows the
    // worker has DEQUEUED gate (otherwise try_submit below would race the
    // enqueue of gate and wrongly see the queue full).
    auto gate = pool.submit([&release, &gate_started] {
        gate_started.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    SLUICE_CHECK(gate.has_value());
    // Wait until the worker has started gate (queue slot freed).
    while (!gate_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // Now the queue is empty; try_submit fills it (depth 1).
    auto fill = pool.try_submit([] { return 1; });
    SLUICE_CHECK(fill.has_value());
    // Second try_submit: queue full -> would_block.
    auto rej = pool.try_submit([] { return 2; });
    SLUICE_CHECK(!rej.has_value());
    SLUICE_CHECK(rej.error().code == IoError::Code::would_block);
    release.store(true, std::memory_order_release);
    gate.value().get();
    fill.value().get();
}

SLUICE_TEST_CASE(shutdown_is_idempotent) {
    auto pr = make_blocking_io_pool(BlockingIoPoolOptions{2, 4});
    SLUICE_CHECK(pr.has_value());
    auto& pool = *pr.value();
    pool.shutdown();
    pool.shutdown();
    pool.shutdown();
}

SLUICE_TEST_CASE(stats_are_consistent) {
    PoolStats s;
    auto pr = make_blocking_io_pool(BlockingIoPoolOptions{2, 8}, &s);
    SLUICE_CHECK(pr.has_value());
    auto& pool = *pr.value();
    SLUICE_CHECK(s.worker_count.load() == 2);
    std::vector<Task<int>> tasks;
    for (int i = 0; i < 10; ++i) {
        auto t = pool.submit([i] { return i; });
        tasks.emplace_back(std::move(t.value()));
    }
    for (auto& t : tasks) {
        (void)t.get();
    }
    SLUICE_CHECK(s.submitted.load() == 10);
    SLUICE_CHECK(s.started.load() == 10);
    SLUICE_CHECK(s.completed.load() == 10);
    SLUICE_CHECK(s.failed.load() == 0);
    SLUICE_CHECK(s.rejected.load() == 0);
    SLUICE_CHECK(s.queue_depth.load() == 0);
}

SLUICE_TEST_CASE(stats_track_failed_and_rejected) {
    PoolStats s;
    auto pr = make_blocking_io_pool(BlockingIoPoolOptions{1, 2}, &s);
    SLUICE_CHECK(pr.has_value());
    auto& pool = *pr.value();
    auto t = pool.submit([]() -> int { throw std::runtime_error("x"); });
    try {
        (void)t.value().get();
    } catch (const std::runtime_error&) {}
    pool.shutdown();
    auto rej = pool.try_submit([] { return 1; });
    SLUICE_CHECK(!rej.has_value());
    SLUICE_CHECK(s.failed.load() == 1);
    SLUICE_CHECK(s.rejected.load() == 1);
}

SLUICE_TEST_CASE(destructor_drains_and_joins) {
    std::atomic<int> done{0};
    {
        auto pr = make_blocking_io_pool(BlockingIoPoolOptions{4, 64});
        SLUICE_CHECK(pr.has_value());
        auto& pool = *pr.value();
        for (int i = 0; i < 50; ++i) {
            auto t = pool.submit([&done] { done.fetch_add(1, std::memory_order_relaxed); });
            (void)t;
        }
    }
    SLUICE_CHECK(done.load() == 50);
}

SLUICE_MAIN()
