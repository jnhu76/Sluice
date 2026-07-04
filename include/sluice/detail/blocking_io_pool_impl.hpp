// Template + Task<T> implementation for sluice::BlockingIoPool (sluice-CORE-024S).
// Included by blocking_io_pool.hpp; not meant to be included directly.
//
// Task<T> uses a shared state (mutex + condition_variable + optional<T> +
// exception_ptr). Pure C++17/20; no C runtime mixing.
#pragma once

// NOTE: this file is included at the END of blocking_io_pool.hpp, so the
// public declarations (BlockingIoPool, Task, PoolStats, etc.) are already
// visible. Do NOT re-include blocking_io_pool.hpp here (circular dependency).

#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace sluice {

class BlockingIoPool;

namespace detail {

// Non-template enqueue core: pushes a type-erased job into the pool's Impl.
//   block==true  -> backpressure submit (waits for space; invalid_state after
//                   shutdown).
//   block==false -> try_submit (would_block when full; invalid_state after
//                   shutdown).
// Defined in src/blocking_io_pool.cpp where Impl is complete. stats may be null.
Result<void> enqueue_job(BlockingIoPool& pool, std::function<void()> job, bool block);

} // namespace detail

// ---- Task<T>::State (non-void) ---------------------------------------------
template <class T> struct Task<T>::State {
    std::mutex mtx;
    std::condition_variable cv;
    bool ready = false;
    std::optional<T> value;
    std::exception_ptr ex;

    void set_value(T v) {
        {
            std::scoped_lock lk(mtx);
            value.emplace(std::move(v));
            ready = true;
        }
        cv.notify_all();
    }
    void set_exception(const std::exception_ptr& e) {
        {
            std::scoped_lock lk(mtx);
            ex = e;
            ready = true;
        }
        cv.notify_all();
    }
};

// Build a type-erased worker job: runs fn, sets value/exception on st, tallies
// completed/failed on stats. Returns a std::function<void()>.
template <class T, class Fn>
std::function<void()> make_bound_job(Fn fn, std::shared_ptr<typename Task<T>::State> st,
                                     PoolStats* stats) {
    return [fn = std::move(fn), st = std::move(st), stats]() {
        try {
            if constexpr (std::is_void_v<T>) {
                fn();
                if (stats) {
                    ++stats->completed;
                }
                st->set_value();
            } else {
                T v = fn();
                if (stats) {
                    ++stats->completed;
                }
                st->set_value(std::move(v));
            }
        } catch (...) {
            st->set_exception(std::current_exception());
            if (stats) {
                ++stats->completed;
                ++stats->failed;
            }
        }
    };
}

template <class T> T Task<T>::get() {
    if (!state_) {
        throw std::logic_error("BlockingIoPool::Task::get() on an empty Task");
    }
    std::unique_lock<std::mutex> lk(state_->mtx);
    state_->cv.wait(lk, [&] { return state_->ready; });
    if (state_->ex) {
        std::rethrow_exception(state_->ex);
    }
    return std::move(*state_->value);
}

// ---- Task<void> specialization ---------------------------------------------
template <> struct Task<void>::State {
    std::mutex mtx;
    std::condition_variable cv;
    bool ready = false;
    std::exception_ptr ex;

    void set_value() {
        {
            std::scoped_lock lk(mtx);
            ready = true;
        }
        cv.notify_all();
    }
    void set_exception(const std::exception_ptr& e) {
        {
            std::scoped_lock lk(mtx);
            ex = e;
            ready = true;
        }
        cv.notify_all();
    }
};

template <> inline void Task<void>::get() {
    if (!state_) {
        throw std::logic_error("BlockingIoPool::Task::get() on an empty Task");
    }
    std::unique_lock<std::mutex> lk(state_->mtx);
    state_->cv.wait(lk, [&] { return state_->ready; });
    if (state_->ex) {
        std::rethrow_exception(state_->ex);
    }
}

// ---- BlockingIoPool submit templates ---------------------------------------
template <class F>
inline Result<Task<std::invoke_result_t<F&&>>> BlockingIoPool::try_submit(F&& f) {
    using R = std::invoke_result_t<F&&>;
    auto st = std::make_shared<typename Task<R>::State>();
    auto job = make_bound_job<R>(std::forward<F>(f), st, pool_stats());
    auto r = detail::enqueue_job(*this, std::move(job), /*block=*/false);
    if (!r.has_value()) {
        return make_unexpected<Task<R>>(r.error());
    }
    return Task<R>(st);
}

template <class F> inline Result<Task<std::invoke_result_t<F&&>>> BlockingIoPool::submit(F&& f) {
    using R = std::invoke_result_t<F&&>;
    auto st = std::make_shared<typename Task<R>::State>();
    auto job = make_bound_job<R>(std::forward<F>(f), st, pool_stats());
    auto r = detail::enqueue_job(*this, std::move(job), /*block=*/true);
    if (!r.has_value()) {
        return make_unexpected<Task<R>>(r.error());
    }
    return Task<R>(st);
}

} // namespace sluice
