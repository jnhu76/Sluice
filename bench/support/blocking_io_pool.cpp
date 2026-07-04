// Bench adapter implementation (sluice-CORE-024S §6). Wraps the production
// sluice::BlockingIoPool; does NOT re-implement the pool. Pure C++17/20.
#include "blocking_io_pool.hpp"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <utility>

namespace sluice::bench {

struct BlockingIoPool::Adapter {
    std::unique_ptr<sluice::BlockingIoPool> pool;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<std::size_t> in_flight{0};
    std::exception_ptr pending_ex;
    bool accepting = true;

    explicit Adapter(std::size_t threads, std::size_t max_queued) {
        auto r = sluice::make_blocking_io_pool(sluice::BlockingIoPoolOptions{threads, max_queued});
        // The bench adapter coerces 0 to 1 (pre-024S behavior) for robustness;
        // make_blocking_io_pool rejects 0, so coerce before calling.
        if (!r.has_value()) {
            r = sluice::make_blocking_io_pool(sluice::BlockingIoPoolOptions{
                threads == 0 ? 1 : threads, max_queued == 0 ? 1 : max_queued});
        }
        if (!r.has_value()) {
            throw std::runtime_error("bench::BlockingIoPool: production pool construction failed");
        }
        pool = std::move(r.value());
    }
};

BlockingIoPool::BlockingIoPool(std::size_t threads, std::size_t max_queued)
    : adapter_(std::make_unique<Adapter>(threads == 0 ? 1 : threads,
                                         max_queued == 0 ? 1 : max_queued)) {}

BlockingIoPool::~BlockingIoPool() {
    shutdown();
}

void BlockingIoPool::submit(std::function<void()> job) {
    if (!job) {
        return;
    }
    if (!adapter_->accepting) {
        return; // bench adapter: silent drop after shutdown (preserves contract)
    }
    adapter_->in_flight.fetch_add(1, std::memory_order_acq_rel);
    // Wrap the job: decrement in_flight on completion, capture exception.
    std::function<void()> wrapped = [this, j = std::move(job)]() {
        try {
            j();
        } catch (...) {
            std::scoped_lock lk(adapter_->mtx);
            if (!adapter_->pending_ex) {
                adapter_->pending_ex = std::current_exception();
            }
        }
        if (adapter_->in_flight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::scoped_lock lk(adapter_->mtx);
            adapter_->cv.notify_all();
        }
    };
    auto r = adapter_->pool->submit(std::move(wrapped));
    if (!r.has_value()) {
        // Rejected (e.g. shutdown raced): undo the in-flight bump, drop silently.
        if (adapter_->in_flight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::scoped_lock lk(adapter_->mtx);
            adapter_->cv.notify_all();
        }
    }
}

void BlockingIoPool::wait_all() {
    std::exception_ptr to_throw;
    {
        std::unique_lock<std::mutex> lk(adapter_->mtx);
        adapter_->cv.wait(lk, [&] { return adapter_->in_flight.load() == 0; });
        if (adapter_->pending_ex) {
            to_throw = adapter_->pending_ex;
            adapter_->pending_ex = nullptr;
        }
    }
    if (to_throw) {
        std::rethrow_exception(to_throw);
    }
}

void BlockingIoPool::shutdown() {
    if (!adapter_->accepting) {
        adapter_->pool->shutdown(); // idempotent on production pool too
        return;
    }
    adapter_->accepting = false;
    adapter_->pool->shutdown();
}

std::size_t BlockingIoPool::thread_count() const {
    return adapter_->pool->worker_count();
}

} // namespace sluice::bench
