// sluice::bench::BlockingIoPool — bench adapter over the production pool
// (sluice-CORE-024S §6).
//
// This is NOT a duplicate implementation. It wraps the production
// sluice::BlockingIoPool (include/sluice/blocking_io_pool.hpp) and exposes the
// fire-and-forget shape the W1-W4 benches already use (void submit, wait_all,
// shutdown, thread_count). Per docs/io/sync-backend-taxonomy.md §2.6, benches
// do not re-implement the pool.
//
// The void submit() drops the returned Task (fire-and-forget); wait_all() joins
// via an in-flight atomic counter + a condition variable; exceptions are
// captured (first-wins) and rethrown at wait_all(), matching the pre-024S
// adapter's contract. The production pool's submit-after-shutdown rejection is
// mapped to a silent drop here (preserving the bench adapter's existing
// void-submit contract so bench call sites don't change).
#pragma once

#include <sluice/blocking_io_pool.hpp>

#include <cstddef>
#include <functional>
#include <memory>

namespace sluice::bench {

class BlockingIoPool {
  public:
    explicit BlockingIoPool(std::size_t threads, std::size_t max_queued = 64);
    ~BlockingIoPool();

    BlockingIoPool(const BlockingIoPool&) = delete;
    BlockingIoPool& operator=(const BlockingIoPool&) = delete;
    BlockingIoPool(BlockingIoPool&&) = delete;
    BlockingIoPool& operator=(BlockingIoPool&&) = delete;

    // Fire-and-forget. Blocks (backpressure) if the queue is full. After
    // shutdown() this is a silent drop (bench adapter contract).
    void submit(std::function<void()> job);

    // Block until every submitted job has completed. Rethrows the first captured
    // job exception (then clears it). Safe when nothing is pending.
    void wait_all();

    // Idempotent; destructor calls it.
    void shutdown();

    std::size_t thread_count() const;

  private:
    struct Adapter;
    std::unique_ptr<Adapter> adapter_;
};

} // namespace sluice::bench
