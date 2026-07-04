// Implementation of BlockingIoPool (sluice-CORE-021S). See header for the
// concurrency model. State is fully instance-owned (no globals).
#include "blocking_io_pool.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sluice::bench {

struct BlockingIoPool::Impl {
    const std::size_t thread_count;
    const std::size_t max_queued;

    std::mutex mtx;
    std::condition_variable cv_queue_not_full;  // signaled when a job dequeues
    std::condition_variable cv_queue_not_empty; // signaled when a job enqueues
    std::condition_variable cv_idle;            // signaled when in_flight hits 0

    std::deque<std::function<void()>> queue;
    std::size_t queued = 0;        // jobs in queue (not yet dequeued)
    std::size_t in_flight = 0;     // dequeued + not yet completed
    bool accepting = true;         // false once shutdown() begins
    bool stopped = false;          // workers exit when true && queue empty
    std::exception_ptr pending_ex; // first thrown exception, if any
    std::vector<std::thread> workers;

    explicit Impl(std::size_t threads, std::size_t max_q)
        : thread_count(threads), max_queued(max_q) {}

    void worker_loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv_queue_not_empty.wait(lk, [&] { return stopped || !queue.empty(); });
                if (queue.empty()) {
                    // stopped && drained -> exit.
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
                --queued;
                ++in_flight;
                // A slot freed up: unblock a throttled submit() if any.
                cv_queue_not_full.notify_one();
            }
            run_job(job);
            {
                std::unique_lock<std::mutex> lk(mtx);
                --in_flight;
                if (in_flight == 0) {
                    cv_idle.notify_all();
                }
            }
        }
    }

    void run_job(const std::function<void()>& job) {
        if (!job) {
            return;
        }
        try {
            job();
        } catch (...) {
            std::scoped_lock lk(mtx);
            if (!pending_ex) {
                pending_ex = std::current_exception();
            }
        }
    }
};

BlockingIoPool::BlockingIoPool(std::size_t threads, std::size_t max_queued)
    : impl_(std::make_unique<Impl>(threads == 0 ? 1 : threads, max_queued == 0 ? 1 : max_queued)) {
    impl_->workers.reserve(impl_->thread_count);
    for (std::size_t i = 0; i < impl_->thread_count; ++i) {
        impl_->workers.emplace_back([this] { impl_->worker_loop(); });
    }
}

BlockingIoPool::~BlockingIoPool() {
    // shutdown() drains the queue and joins all workers. The unique_ptr then
    // destroys Impl (no manual delete; leak-safe even if a worker constructor
    // had thrown during construction). Idempotent: a user-called shutdown()
    // makes this a no-op.
    shutdown();
}

void BlockingIoPool::submit(std::function<void()> job) {
    if (!job) {
        return;
    }
    {
        std::unique_lock<std::mutex> lk(impl_->mtx);
        if (!impl_->accepting) {
            // shutdown() in progress or completed: drop silently.
            return;
        }
        impl_->cv_queue_not_full.wait(
            lk, [&] { return impl_->queued < impl_->max_queued || !impl_->accepting; });
        if (!impl_->accepting) {
            return;
        }
        impl_->queue.push_back(std::move(job));
        ++impl_->queued;
        impl_->cv_queue_not_empty.notify_one();
    }
}

void BlockingIoPool::wait_all() {
    std::exception_ptr to_throw;
    {
        std::unique_lock<std::mutex> lk(impl_->mtx);
        impl_->cv_idle.wait(lk, [&] { return impl_->queued == 0 && impl_->in_flight == 0; });
        if (impl_->pending_ex) {
            to_throw = impl_->pending_ex;
            impl_->pending_ex = nullptr;
        }
    }
    if (to_throw) {
        std::rethrow_exception(to_throw);
    }
}

void BlockingIoPool::shutdown() {
    {
        std::scoped_lock lk(impl_->mtx);
        if (!impl_->accepting) {
            return; // idempotent
        }
        impl_->accepting = false;
        impl_->stopped = true;
        // Wake everyone who might be waiting.
        impl_->cv_queue_not_empty.notify_all();
        impl_->cv_queue_not_full.notify_all();
    }
    for (auto& w : impl_->workers) {
        if (w.joinable()) {
            w.join();
        }
    }
}

std::size_t BlockingIoPool::thread_count() const {
    return impl_->thread_count;
}

} // namespace sluice::bench
