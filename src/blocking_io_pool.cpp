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
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace sluice {

namespace {
thread_local const void* current_blocking_io_pool = nullptr;

struct WorkerScope {
    const void* previous;
    explicit WorkerScope(const void* pool) : previous(current_blocking_io_pool) {
        current_blocking_io_pool = pool;
    }
    ~WorkerScope() { current_blocking_io_pool = previous; }
};

BlockingIoPoolOptions validate_options_or_throw(BlockingIoPoolOptions opts) {
    if (opts.worker_count == 0 || opts.max_queue_depth == 0) {
        throw std::invalid_argument(
            "BlockingIoPool requires non-zero worker_count and max_queue_depth");
    }
    return opts;
}
} // namespace

struct BlockingIoPool::Impl {
    BlockingIoPoolOptions opts;
    PoolStats* stats;

    std::mutex mtx;
    std::condition_variable not_full;  // signaled when a slot frees
    std::condition_variable not_empty; // signaled when a job is enqueued
    std::condition_variable idle;      // signaled when queue + active work drain
    std::deque<std::function<void()>> queue;
    std::vector<std::thread> workers;
    std::once_flag shutdown_once;
    std::size_t active = 0;
    bool accepting = true; // false once shutdown() begins

    Impl(BlockingIoPoolOptions o, PoolStats* s) : opts(o), stats(s) {
        if (stats) {
            stats->worker_count = opts.worker_count;
        }
    }

    void worker_loop() {
        WorkerScope scope(this);
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
                ++active;
                if (stats) {
                    ++stats->started;
                    --stats->queue_depth;
                }
                not_full.notify_one();
            }
            job();
            {
                std::scoped_lock lk(mtx);
                --active;
                if (queue.empty() && active == 0) {
                    idle.notify_all();
                }
            }
        }
    }

    bool is_current_worker() const noexcept {
        return current_blocking_io_pool == this;
    }

    void shutdown(bool detach_current_worker) {
        std::call_once(shutdown_once, [this, detach_current_worker] {
            {
                std::scoped_lock lk(mtx);
                accepting = false;
                not_empty.notify_all(); // wake all workers to see accepting==false
                not_full.notify_all();  // wake any blocked submitters
            }

            const std::thread::id self = std::this_thread::get_id();
            for (auto& w : workers) {
                if (!w.joinable()) {
                    continue;
                }
                if (w.get_id() == self) {
                    if (detach_current_worker) {
                        w.detach();
                    }
                    continue;
                }
                w.join();
            }
        });
    }
};

BlockingIoPool::BlockingIoPool(BlockingIoPoolOptions opts, PoolStats* stats)
    : impl_(std::make_unique<Impl>(validate_options_or_throw(opts), stats)) {
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
        impl_->shutdown(/*detach_current_worker=*/false);
        throw;
    }
}

BlockingIoPool::~BlockingIoPool() {
    impl_->shutdown(/*detach_current_worker=*/impl_->is_current_worker());
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

void BlockingIoPool::wait_idle() {
    if (impl_->is_current_worker()) {
        throw std::logic_error("BlockingIoPool::wait_idle() cannot be called from a worker task");
    }
    std::unique_lock<std::mutex> lk(impl_->mtx);
    impl_->idle.wait(lk, [&] { return impl_->queue.empty() && impl_->active == 0; });
}

void BlockingIoPool::shutdown() {
    if (impl_->is_current_worker()) {
        throw std::logic_error("BlockingIoPool::shutdown() cannot be called from a worker task");
    }
    impl_->shutdown(/*detach_current_worker=*/false);
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
