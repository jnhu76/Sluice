#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace sluice {

class BlockingIoPool;

namespace detail {

Result<void> enqueue_job(BlockingIoPool& pool, std::function<void()> job, bool block);

}

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

template <class T, class Fn>
std::function<void()> make_bound_job(Fn fn, std::shared_ptr<typename Task<T>::State> st,
                                     PoolStats* stats) {
    return [fn = std::move(fn), st = std::move(st), stats]() mutable {
        try {
            if constexpr (std::is_void_v<T>) {
                fn();
                {
                    std::scoped_lock lk(st->mtx);
                    st->ready = true;
                    if (stats) {
                        ++stats->completed;
                    }
                }
                st->cv.notify_all();
            } else {
                T v = fn();
                {
                    std::scoped_lock lk(st->mtx);
                    st->value.emplace(std::move(v));
                    st->ready = true;
                    if (stats) {
                        ++stats->completed;
                    }
                }
                st->cv.notify_all();
            }
        } catch (...) {
            {
                std::scoped_lock lk(st->mtx);
                st->ex = std::current_exception();
                st->ready = true;
                if (stats) {
                    ++stats->completed;
                    ++stats->failed;
                }
            }
            st->cv.notify_all();
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

template <class F>
inline Result<Task<std::invoke_result_t<F&&>>> BlockingIoPool::try_submit(F&& f) {
    using R = std::invoke_result_t<F&&>;
    auto st = std::make_shared<typename Task<R>::State>();
    auto job = make_bound_job<R>(std::forward<F>(f), st, pool_stats());
    auto r = detail::enqueue_job(*this, std::move(job), false);
    if (!r.has_value()) {
        return make_unexpected<Task<R>>(r.error());
    }
    return Task<R>(st);
}

template <class F> inline Result<Task<std::invoke_result_t<F&&>>> BlockingIoPool::submit(F&& f) {
    using R = std::invoke_result_t<F&&>;
    auto st = std::make_shared<typename Task<R>::State>();
    auto job = make_bound_job<R>(std::forward<F>(f), st, pool_stats());
    auto r = detail::enqueue_job(*this, std::move(job), true);
    if (!r.has_value()) {
        return make_unexpected<Task<R>>(r.error());
    }
    return Task<R>(st);
}

} // namespace sluice
