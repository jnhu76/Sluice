// sluice-CORE-021S: a bounded blocking worker pool for concurrent bench
// workloads (the W1-W4 matrix, job 022S).
//
// IMPORTANT: BlockingIoPool is an EXECUTION MODEL for benchmarks, NOT an I/O
// backend. It does not implement IoContext, is not selectable as a backend, and
// lives in bench/support/ (see docs/sync-io-architecture.md §3). It runs
// ordinary blocking operations on fixed std::thread workers. It is NOT async.
//
// Concurrency model:
//   - N worker threads, fixed at construction.
//   - A bounded FIFO queue (max_queued). submit() blocks when the queue is full,
//     so submitted-but-unrun jobs never grow without limit.
//   - wait_all() blocks until every submitted job has completed.
//   - shutdown() stops accepting, drains the queue, joins workers. Idempotent.
//   - submit() after shutdown() is a safe no-op.
//
// State is instance-owned only (no globals) so the pool is ASan/TSan-clean and
// multiple independent pools do not interfere.
#pragma once

#include <cstddef>
#include <functional>

namespace sluice::bench {

class BlockingIoPool {
public:
    // Construct a pool with `threads` worker threads and a bounded queue of
    // `max_queued` pending jobs. `threads` must be >= 1. `max_queued` default is
    // modest; callers (e.g. benches) may raise it to avoid submit throttling.
    explicit BlockingIoPool(std::size_t threads, std::size_t max_queued = 64);
    ~BlockingIoPool();

    BlockingIoPool(const BlockingIoPool&) = delete;
    BlockingIoPool& operator=(const BlockingIoPool&) = delete;
    BlockingIoPool(BlockingIoPool&&) = delete;
    BlockingIoPool& operator=(BlockingIoPool&&) = delete;

    // Submit a job. Blocks if the queue is at capacity. After shutdown() this is
    // a no-op (returns without enqueuing). A null job is ignored.
    void submit(std::function<void()> job);

    // Block until every submitted job has completed. If any job threw, the first
    // captured exception is rethrown here (and then cleared). Workers stay alive
    // and the pool keeps accepting jobs. Safe to call when nothing is pending.
    void wait_all();

    // Stop accepting submissions, finish all pending jobs, then join workers.
    // Idempotent; also invoked by the destructor if not called first.
    void shutdown();

    // Number of worker threads (fixed at construction).
    std::size_t thread_count() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace sluice::bench
