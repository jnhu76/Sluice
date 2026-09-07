
























#pragma once

#include <sluice/async/application_runtime.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>

namespace sluice::async {

#ifdef SLUICE_ASYNC_INTERNAL_TESTING





namespace detail {
inline bool task_result_submit_throw_armed = false;

inline bool task_result_test_inject_next_submit_throw() noexcept {
    if (!task_result_submit_throw_armed) return false;
    task_result_submit_throw_armed = false;
    return true;
}
}
#endif













template <class T>
class TaskResultSlot {
  public:
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "TaskResultSlot<T>: T must be nothrow move constructible "
                  "(publish is noexcept; a throwing move would terminate the "
                  "publishing Runtime worker)");

    TaskResultSlot() = default;
    TaskResultSlot(const TaskResultSlot&) = delete;
    TaskResultSlot& operator=(const TaskResultSlot&) = delete;











    void publish(T r) noexcept {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (done_) return;
            out_.emplace(std::move(r));
            done_ = true;
        }
        cv_.notify_all();
    }







    T wait_and_take() {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return done_; });
        T v = std::move(out_.value());

        out_.reset();
        return v;
    }

  private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::optional<T> out_;
    bool done_ = false;
};












template <class T>
Result<T> translate_task_exception() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return make_unexpected<T>(IoError{IoError::Code::no_space});
    } catch (const std::system_error& e) {
        IoError err{IoError::Code::backend_error};
        if (e.code().value() > 0) err.os_errno = e.code().value();
        return make_unexpected<T>(err);
    } catch (...) {
        return make_unexpected<T>(IoError{IoError::Code::backend_error});
    }
}






































template <class T, class TaskFn>
Result<T> run_task_to_result(unsigned workers,
                             std::unique_ptr<AsyncBackend> backend,
                             TaskFn&& task) {
    static_assert(std::is_invocable_v<TaskFn&, RuntimeTaskContext&,
                                      TaskResultSlot<Result<T>>&>,
                  "task must be invocable as void(RuntimeTaskContext&, "
                  "TaskResultSlot<Result<T>>&)");

    if (workers == 0 || backend == nullptr) {
        return make_unexpected<T>(IoError{IoError::Code::invalid_state});
    }

    TaskResultSlot<Result<T>> slot;

    RuntimeBuilder builder;
    builder.backend(std::move(backend));
    builder.workers(workers);




    std::unique_ptr<ApplicationRuntime> rt;
    try {
        auto build_r = builder.build();
        if (!build_r.has_value()) {
            return make_unexpected<T>(build_r.error());
        }
        rt = std::move(build_r.value());

        auto start_r = rt->start();
        if (!start_r.has_value()) {
            return make_unexpected<T>(start_r.error());
        }
    } catch (...) {
        if (rt) (void)rt->shutdown();
        return translate_task_exception<T>();
    }



#ifdef SLUICE_ASYNC_INTERNAL_TESTING
    if (detail::task_result_test_inject_next_submit_throw()) {
        rt->test_inject_next_submit_throw();
    }
#endif














    try {
        auto sub_r = rt->submit([&task, &slot](RuntimeTaskContext& ctx) {
            try {
                task(ctx, slot);
            } catch (...) {
                slot.publish(translate_task_exception<T>());
            }
        });
        if (!sub_r.has_value()) {
            (void)rt->shutdown();
            return make_unexpected<T>(sub_r.error());
        }
    } catch (...) {
        (void)rt->shutdown();
        return translate_task_exception<T>();
    }





    Result<T> result = slot.wait_and_take();

    rt->request_stop();
    auto drain_r = rt->drain();
    if (!drain_r.has_value()) {
        (void)rt->shutdown();
        return make_unexpected<T>(drain_r.error());
    }
    auto join_r = rt->join();
    if (!join_r.has_value()) {
        (void)rt->shutdown();
        return make_unexpected<T>(join_r.error());
    }

    return result;
}

}
