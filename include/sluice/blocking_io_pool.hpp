
























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


Result<void> enqueue_job(BlockingIoPool& pool, std::function<void()> job, bool block);
}



struct BlockingIoPoolOptions {
    std::size_t worker_count = 0;
    std::size_t max_queue_depth = 0;
};




struct PoolStats {




    std::atomic<std::size_t> submitted{0};
    std::atomic<std::size_t> started{0};
    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> failed{0};
    std::atomic<std::size_t> rejected{0};
    std::atomic<std::size_t> queue_depth{0};
    std::atomic<std::size_t> worker_count{0};
};



template <class T> class Task {
  public:
    Task() = default;
    ~Task() = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;



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


    explicit BlockingIoPool(BlockingIoPoolOptions opts, PoolStats* stats = nullptr);
    ~BlockingIoPool();

    BlockingIoPool(const BlockingIoPool&) = delete;
    BlockingIoPool& operator=(const BlockingIoPool&) = delete;
    BlockingIoPool(BlockingIoPool&&) = delete;
    BlockingIoPool& operator=(BlockingIoPool&&) = delete;



    template <class F> Result<Task<std::invoke_result_t<F&&>>> try_submit(F&& f);



    template <class F> Result<Task<std::invoke_result_t<F&&>>> submit(F&& f);




    void wait_idle();


    void shutdown();


    std::size_t worker_count() const noexcept;
    std::size_t queue_depth() const noexcept;


    const PoolStats* stats() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend Result<void> detail::enqueue_job(BlockingIoPool&, std::function<void()>, bool);
    PoolStats* pool_stats() noexcept;
};




Result<std::unique_ptr<BlockingIoPool>> make_blocking_io_pool(BlockingIoPoolOptions opts,
                                                              PoolStats* stats = nullptr);

}

#include <sluice/detail/blocking_io_pool_impl.hpp>
