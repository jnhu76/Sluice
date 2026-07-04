// sluice::BlockingIoPool — production bounded OS-thread pool (sluice-CORE-024S).
//
// A bounded pool of fixed std::thread workers that executes blocking callables.
// This is the production sync execution helper (see docs/adr/ADR-024S §G9 and
// docs/io/sync-backend-taxonomy.md). It is NOT an async runtime, NOT a fiber
// scheduler, NOT a P2300 executor, and NOT one-thread-per-operation.
//
// Concurrency model:
//   - N worker threads, fixed at construction (worker_count >= 1).
//   - A bounded FIFO queue (max_queue_depth >= 1). submit() applies backpressure
//     (blocks when full); try_submit() is non-blocking and rejects when full.
//   - Tasks surface their return value and any thrown exception via Task<T>.
//   - shutdown() drains already-submitted work (running syscalls NOT cancelled),
//     joins workers. Idempotent; destructor calls it (drain-and-join contract).
//   - submit/try_submit AFTER shutdown() is REJECTED (IoError::invalid_state).
//   - Progress guarantee boundary: tasks are ordinary user callables. A task
//     must not synchronously require queued work from the same saturated pool to
//     make forward progress. Same-pool recursive blocking submit() and
//     capacity-exhausting same-pool Task::get() dependency graphs are outside
//     the pool's progress guarantee.
//
// Pure C++17/20: std::thread/mutex/condition_variable/future. No C runtime
// mixing, no platform-specific threading primitives. State is instance-owned
// only (no globals) so the pool is ASan/TSan-clean and pools do not interfere.
#pragma once

#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace sluice {

class BlockingIoPool;

namespace detail {
// Non-template enqueue core (defined in src/blocking_io_pool.cpp; friend of
// BlockingIoPool). block==true -> backpressure submit; false -> try_submit.
Result<void> enqueue_job(BlockingIoPool& pool, std::function<void()> job, bool block);
} // namespace detail

// Construction options. worker_count==0 and max_queue_depth==0 are invalid and
// rejected by make_blocking_io_pool().
struct BlockingIoPoolOptions {
    std::size_t worker_count = 0;
    std::size_t max_queue_depth = 0;
};

// Pool observability. Caller-owned via the pool (a pointer is attached at
// construction); nullable. Never global. Counters are monotonic except
// queue_depth (instantaneous) and worker_count (fixed).
struct PoolStats {
    // All counters are atomic: written concurrently by submitters (submitted/
    // rejected/queue_depth) and workers (started/completed/failed). Reads in
    // tests use .load() after the pool is drained (joins provide the
    // happens-before for a stable snapshot).
    std::atomic<std::size_t> submitted{0};
    std::atomic<std::size_t> started{0};
    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> failed{0};
    std::atomic<std::size_t> rejected{0};
    std::atomic<std::size_t> queue_depth{0};
    std::atomic<std::size_t> worker_count{0};
};

// A handle to a submitted task. get() blocks until the task completes, then
// returns its value or rethrows its exception (exactly-once). Move-only.
template <class T> class Task {
  public:
    Task() = default;
    ~Task() = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;

    // Block until the task completes; return its value or rethrow its exception.
    // Calling get() more than once is undefined (the shared state is consumed).
    T get();
    bool valid() const noexcept { return static_cast<bool>(state_); }

  private:
    struct State;
    std::shared_ptr<State> state_;
    explicit Task(std::shared_ptr<State> s) : state_(std::move(s)) {}

    friend class BlockingIoPool;
    template <class U, class Fn>
    friend std::function<void()> make_bound_job(Fn fn, std::shared_ptr<typename Task<U>::State> st,
                                                PoolStats* stats);
};

class BlockingIoPool {
  public:
    // Construct via the factory make_blocking_io_pool (rejects invalid options).
    // Public for unique_ptr/make_unique; prefer the factory.
    explicit BlockingIoPool(BlockingIoPoolOptions opts, PoolStats* stats = nullptr);
    ~BlockingIoPool();

    BlockingIoPool(const BlockingIoPool&) = delete;
    BlockingIoPool& operator=(const BlockingIoPool&) = delete;
    BlockingIoPool(BlockingIoPool&&) = delete;
    BlockingIoPool& operator=(BlockingIoPool&&) = delete;

    // Non-blocking submit. Returns would_block when the queue is full,
    // invalid_state after shutdown(). Never blocks the caller.
    template <class F> Result<Task<std::invoke_result_t<F&&>>> try_submit(F&& f);

    // Blocking submit with backpressure: waits for queue space, then enqueues.
    // Returns invalid_state after shutdown() (no silent drop).
    template <class F> Result<Task<std::invoke_result_t<F&&>>> submit(F&& f);

    // Wait until the current queue and in-flight tasks are idle without
    // stopping the pool. New concurrent submissions after the idle observation
    // are outside this wait.
    void wait_idle();

    // Stop accepting, drain already-submitted work, join workers. Idempotent.
    void shutdown();

    // Fixed worker count and instantaneous queue depth.
    std::size_t worker_count() const noexcept;
    std::size_t queue_depth() const noexcept;

    // Observability (nullptr if no stats attached at construction).
    const PoolStats* stats() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend Result<void> detail::enqueue_job(BlockingIoPool&, std::function<void()>, bool);
    PoolStats* pool_stats() noexcept; // returns impl_->stats (for the submit templates)
};

// Factory: rejects worker_count==0 or max_queue_depth==0 with invalid_state.
// Returns an owning unique_ptr (BlockingIoPool is non-movable: PIMPL with an
// incomplete Impl, and address-stability matches the pool's worker-thread model).
Result<std::unique_ptr<BlockingIoPool>> make_blocking_io_pool(BlockingIoPoolOptions opts,
                                                              PoolStats* stats = nullptr);

} // namespace sluice

#include <sluice/detail/blocking_io_pool_impl.hpp> // template + Task definitions
