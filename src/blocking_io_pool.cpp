// Production BlockingIoPool non-template core (sluice-CORE-024S).
// Pure C++17/20: std primitives only. No C runtime mixing.
#include <sluice/blocking_io_pool.hpp>
#include <sluice/detail/blocking_io_pool_impl.hpp>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sluice {

struct BlockingIoPool::Impl {
    BlockingIoPoolOptions opts;
    PoolStats* stats;

    std::mutex mtx;
    std::condition_variable not_full;  // signaled when a slot frees
    std::condition_variable not_empty; // signaled when a job is enqueued
    std::deque<std::function<void()>> queue;
    std::vector<std::thread> workers;
    bool accepting = true; // false once shutdown() begins

    Impl(BlockingIoPoolOptions o, PoolStats* s) : opts(o), stats(s) {
        if (stats) {
            stats->worker_count = opts.worker_count;
        }
    }

    void worker_loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mtx);
                not_empty.wait(lk, [&] { return !accepting || !queue.empty(); });
                if (queue.empty()) {
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
                if (stats) {
                    ++stats->started;
                    --stats->queue_depth;
                }
                not_full.notify_one();
            }
            job();
        }
    }
};

BlockingIoPool::BlockingIoPool(BlockingIoPoolOptions opts, PoolStats* stats)
    : impl_(std::make_unique<Impl>(opts, stats)) {
    impl_->workers.reserve(impl_->opts.worker_count);
    // If a thread constructor throws, the already-created threads in
    // impl_->workers are joined by the Impl destructor (which runs as part of
    // unique_ptr cleanup), and accepting stays true so they drain. No leak.
    try {
        for (std::size_t i = 0; i < impl_->opts.worker_count; ++i) {
            impl_->workers.emplace_back([this] { impl_->worker_loop(); });
        }
    } catch (...) {
        // Roll back: signal shutdown so workers exit, then rethrow.
        shutdown();
        throw;
    }
}

BlockingIoPool::~BlockingIoPool() {
    shutdown(); // drain-and-join; idempotent.
}

namespace detail {

// Enqueue a type-erased job. Reaches into the pool's Impl (friend). stats is
// read from the Impl so the path matches the submit templates.
Result<void> enqueue_job(BlockingIoPool& pool, std::function<void()> job, bool block) {
    auto& impl = *pool.impl_;
    PoolStats* stats = impl.stats;
    {
        std::unique_lock<std::mutex> lk(impl.mtx);
        if (!impl.accepting) {
            if (stats) {
                ++stats->rejected;
            }
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (block) {
            impl.not_full.wait(lk, [&] {
                return impl.queue.size() < impl.opts.max_queue_depth || !impl.accepting;
            });
            if (!impl.accepting) {
                if (stats) {
                    ++stats->rejected;
                }
                return make_unexpected<void>(IoError{IoError::Code::invalid_state});
            }
        } else {
            if (impl.queue.size() >= impl.opts.max_queue_depth) {
                if (stats) {
                    ++stats->rejected;
                }
                return make_unexpected<void>(IoError{IoError::Code::would_block});
            }
        }
        impl.queue.push_back(std::move(job));
        if (stats) {
            ++stats->submitted;
            ++stats->queue_depth;
        }
        impl.not_empty.notify_one();
    }
    return {};
}

} // namespace detail

PoolStats* BlockingIoPool::pool_stats() noexcept {
    return impl_->stats;
}

void BlockingIoPool::shutdown() {
    {
        std::scoped_lock lk(impl_->mtx);
        if (!impl_->accepting) {
            return; // idempotent
        }
        impl_->accepting = false;
        impl_->not_empty.notify_all(); // wake all workers to see accepting==false
        impl_->not_full.notify_all();  // wake any blocked submitters
    }
    for (auto& w : impl_->workers) {
        if (w.joinable()) {
            w.join();
        }
    }
}

std::size_t BlockingIoPool::worker_count() const noexcept {
    return impl_->opts.worker_count;
}

std::size_t BlockingIoPool::queue_depth() const noexcept {
    std::scoped_lock lk(impl_->mtx);
    return impl_->queue.size();
}

const PoolStats* BlockingIoPool::stats() const noexcept {
    return impl_->stats;
}

Result<std::unique_ptr<BlockingIoPool>> make_blocking_io_pool(BlockingIoPoolOptions opts,
                                                              PoolStats* stats) {
    if (opts.worker_count == 0 || opts.max_queue_depth == 0) {
        return make_unexpected<std::unique_ptr<BlockingIoPool>>(
            IoError{IoError::Code::invalid_state});
    }
    return std::make_unique<BlockingIoPool>(opts, stats);
}

} // namespace sluice
