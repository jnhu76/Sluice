// Tests for BlockingIoPool (sluice-CORE-021S, Phase 7).
//
// Exercises the bounded blocking worker pool through its public interface:
//   - N independent jobs complete on M workers (basic correctness).
//   - Results collected from worker tasks back to the caller.
//   - Bounded: submit throttles when the queue is full (no unbounded growth).
//   - wait_all() blocks until every submitted job completes.
//   - shutdown() joins workers and is idempotent.
//   - submit() after shutdown() is a safe no-op.
//   - A single-worker pool runs jobs sequentially (deterministic config).
//   - Exception propagation: a throwing job is surfaced at wait_all().
//   - No global state: two pools do not interfere with each other.
//
// No sleeps in the synchronization paths; we use atomic counters, barriers, and
// wait_all() itself as the join. ASan/TSan must be clean.
#include "harness.hpp"

#include "support/blocking_io_pool.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace sluice::bench;

// ---- Slice 1: N jobs run to completion on M workers -------------------------

SLUICE_TEST_CASE(pool_runs_n_jobs_on_m_workers) {
    constexpr std::size_t kJobs = 1000;
    constexpr std::size_t kThreads = 4;

    BlockingIoPool pool(kThreads);
    SLUICE_CHECK(pool.thread_count() == kThreads);

    std::atomic<int> done{0};
    for (std::size_t i = 0; i < kJobs; ++i) {
        pool.submit([&done] { done.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.wait_all();

    SLUICE_CHECK(done.load() == static_cast<int>(kJobs));
}

// ---- Slice 2: results flow back to caller-owned buffers ---------------------

SLUICE_TEST_CASE(pool_collects_results_into_caller_buffer) {
    constexpr std::size_t kJobs = 200;
    constexpr std::size_t kThreads = 4;

    BlockingIoPool pool(kThreads);
    std::vector<int> results(kJobs, -1);

    for (std::size_t i = 0; i < kJobs; ++i) {
        // Caller-owned buffer; the lambda's capture keeps a pointer to it. The
        // buffer outlives the job (we wait_all below before reading/destroying).
        pool.submit([&results, i] { results[i] = static_cast<int>(i * i); });
    }
    pool.wait_all();

    bool all_ok = true;
    for (std::size_t i = 0; i < kJobs; ++i) {
        if (results[i] != static_cast<int>(i * i)) {
            all_ok = false;
            break;
        }
    }
    SLUICE_CHECK(all_ok);
}

// ---- Slice 3: a single-worker pool is sequential ----------------------------
// A single-worker pool is the deterministic config: jobs cannot overlap. We
// detect overlap by tracking max concurrency observed by the jobs themselves.
SLUICE_TEST_CASE(single_worker_pool_runs_jobs_sequentially) {
    BlockingIoPool pool(1);
    SLUICE_CHECK(pool.thread_count() == 1);

    std::atomic<int> running{0};
    std::atomic<int> max_concurrent{0};

    constexpr std::size_t kJobs = 50;
    for (std::size_t i = 0; i < kJobs; ++i) {
        pool.submit([&running, &max_concurrent] {
            int cur = running.fetch_add(1, std::memory_order_acq_rel) + 1;
            // Record the high-water mark of concurrent execution.
            int prev = max_concurrent.load(std::memory_order_relaxed);
            while (cur > prev &&
                   !max_concurrent.compare_exchange_weak(prev, cur, std::memory_order_relaxed)) {}
            // brief work so a race (if any existed) would be observable.
            std::this_thread::yield();
            running.fetch_sub(1, std::memory_order_acq_rel);
        });
    }
    pool.wait_all();

    SLUICE_CHECK(max_concurrent.load() == 1);
}

// ---- Slice 4: wait_all is safe when nothing is pending ----------------------

SLUICE_TEST_CASE(wait_all_with_no_pending_jobs_returns_immediately) {
    BlockingIoPool pool(2);
    pool.wait_all(); // no jobs submitted; must not deadlock
    pool.wait_all(); // idempotent
    SLUICE_CHECK(pool.thread_count() == 2);
}

// ---- Slice 5: bounded queue throttles submit -------------------------------

SLUICE_TEST_CASE(bounded_queue_throttles_submit) {
    // A tiny queue + a blocking gate job that none of the workers will complete
    // until we release it. submit() must block once the queue is full.
    constexpr std::size_t kThreads = 2;
    constexpr std::size_t kQueue = 4;
    BlockingIoPool pool(kThreads, kQueue);

    std::atomic<bool> release{false};
    // Gate jobs occupy the workers.
    for (std::size_t i = 0; i < kThreads; ++i) {
        pool.submit([&release] {
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    }

    // Fill the bounded queue exactly. The +kThreads accounts for the two gate
    // jobs already consumed by workers (queued = 0 after they dequeue).
    std::atomic<int> queued_ran{0};
    for (std::size_t i = 0; i < kQueue; ++i) {
        pool.submit([&queued_ran] { queued_ran.fetch_add(1, std::memory_order_relaxed); });
    }

    // One more submit must block because the queue is full and workers are
    // gated. We launch it on a side thread and check that it hasn't returned
    // after a grace period.
    std::atomic<bool> extra_submit_done{false};
    std::thread extra([&] {
        pool.submit([&queued_ran] { queued_ran.fetch_add(1, std::memory_order_relaxed); });
        extra_submit_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    SLUICE_CHECK(extra_submit_done.load() == false);

    // Release the gate; the throttled submit completes and wait_all drains.
    release.store(true, std::memory_order_release);
    pool.wait_all();
    extra.join();

    SLUICE_CHECK(queued_ran.load() == static_cast<int>(kQueue + 1));
    SLUICE_CHECK(extra_submit_done.load() == true);
}

// ---- Slice 6: shutdown joins workers and is idempotent ---------------------

SLUICE_TEST_CASE(shutdown_joins_workers_and_is_idempotent) {
    BlockingIoPool pool(3);
    std::atomic<int> ran{0};
    for (int i = 0; i < 100; ++i) {
        pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.shutdown(); // drains pending + joins
    SLUICE_CHECK(ran.load() == 100);

    pool.shutdown(); // idempotent
    pool.shutdown(); // still idempotent
}

// ---- Slice 7: submit after shutdown is a safe no-op ------------------------

SLUICE_TEST_CASE(submit_after_shutdown_is_noop) {
    BlockingIoPool pool(2);
    std::atomic<int> ran{0};
    pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    pool.wait_all();
    pool.shutdown();

    // These must not run.
    pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    pool.wait_all();

    SLUICE_CHECK(ran.load() == 1);
}

// ---- Slice 8: exception in a job surfaces at wait_all ----------------------

SLUICE_TEST_CASE(exception_in_job_surfaces_at_wait_all) {
    BlockingIoPool pool(2);
    std::atomic<int> ran{0};
    for (int i = 0; i < 10; ++i) {
        pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.submit([] { throw std::runtime_error("boom"); });
    for (int i = 0; i < 10; ++i) {
        pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }

    bool caught = false;
    try {
        pool.wait_all();
    } catch (const std::runtime_error& e) {
        caught = std::string(e.what()) == "boom";
    }
    SLUICE_CHECK(caught);

    // After the exception is drained, the pool is usable again and clear.
    pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    pool.wait_all(); // must not throw
    SLUICE_CHECK(ran.load() == 21);
}

// ---- Slice 9: two pools don't interfere (no global state) ------------------

SLUICE_TEST_CASE(two_pools_do_not_interfere) {
    BlockingIoPool a(2);
    BlockingIoPool b(3);
    SLUICE_CHECK(a.thread_count() == 2);
    SLUICE_CHECK(b.thread_count() == 3);

    std::atomic<int> a_count{0};
    std::atomic<int> b_count{0};

    for (int i = 0; i < 50; ++i) {
        a.submit([&a_count] { a_count.fetch_add(1, std::memory_order_relaxed); });
        b.submit([&b_count] { b_count.fetch_add(1, std::memory_order_relaxed); });
    }
    a.wait_all();
    b.wait_all();

    SLUICE_CHECK(a_count.load() == 50);
    SLUICE_CHECK(b_count.load() == 50);
}

// ---- Slice 10: destructor drains pending + joins (RAII) --------------------

SLUICE_TEST_CASE(destructor_drains_and_joins) {
    std::atomic<int> ran{0};
    {
        BlockingIoPool pool(4);
        for (int i = 0; i < 500; ++i) {
            pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
        }
        // No explicit shutdown — destructor must handle it.
    }
    SLUICE_CHECK(ran.load() == 500);
}

SLUICE_MAIN()
