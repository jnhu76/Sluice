// sluice::async task-result bridge for run-to-completion Runtime tasks
// (C7, #135).
//
// The four applications each duplicated the same lifecycle scaffolding around
// ApplicationRuntime: an app-owned result slot (mutex + condition_variable +
// optional + done flag) published exactly-once by the task, an app-level
// exception boundary translating escaping exceptions into results (the
// Runtime swallows task exceptions at the Group boundary — without the
// translation the caller's wait would hang forever), and the
// submit -> wait-for-publish -> request_stop -> drain -> join teardown
// sequence (audit #135 C7).
//
// TaskResultSlot<T> is that published-exactly-once result slot.
// run_task_to_result<T> runs ONE run-to-completion task on a freshly built
// Runtime and returns its published result.
//
// Explicit non-goals (no hidden authority — the helpers simplify protocol
// only):
//   - no implicit Runtime ownership: the Runtime is built, driven, and joined
//     entirely within run_task_to_result;
//   - no implicit cancellation: request_stop is issued only AFTER the task
//     published (a run-to-completion task is never aborted mid-work);
//   - no hidden destructor drain or background task;
//   - no fabricated outcome: the task MUST publish exactly once; a silent
//     task blocks wait_and_take forever (identical to the app slots this
//     replaces).
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

// ---------------------------------------------------------------------------
// TaskResultSlot<T> — the app-owned terminal-outcome slot (exactly-once
// publish, blocking take). T is the outcome type: Result<X> for
// run_task_to_result callers, or any app result type that embeds its own
// error channel. Lifetime must exceed the task's (the task holds a
// reference). The publishing thread is a Runtime worker; the taking thread is
// any NON-Runtime thread (blocking a Runtime worker on wait_and_take would
// starve the driver — same rule as the application slots).
// ---------------------------------------------------------------------------
template <class T>
class TaskResultSlot {
  public:
    TaskResultSlot() = default;
    TaskResultSlot(const TaskResultSlot&) = delete;
    TaskResultSlot& operator=(const TaskResultSlot&) = delete;

    // Publish the terminal outcome. Exactly-once from the task's perspective:
    // the FIRST publish wins and wakes the waiter; later publishes are
    // dropped (mirrors the application slots: a racing cleanup path can never
    // overwrite the real outcome). noexcept: never allocates, never blocks.
    void publish(T r) noexcept {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (done_) return;
            out_.emplace(std::move(r));
            done_ = true;
        }
        cv_.notify_all();
    }

    // Block the calling (non-Runtime) thread until a publish, then return the
    // outcome. Safe to call once per slot.
    T wait_and_take() {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return done_; });
        return std::move(out_.value());
    }

  private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::optional<T> out_;
    bool done_ = false;
};

// ---------------------------------------------------------------------------
// Task exception translation (the shared app-level boundary).
// ---------------------------------------------------------------------------
// Translate an exception escaping a task body into the typed failure channel.
// This is the boundary every application duplicated: bad_alloc -> no_space,
// system_error -> backend_error (with the OS errno preserved when positive),
// anything else -> backend_error. noexcept; never rethrows.
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

// ---------------------------------------------------------------------------
// run_task_to_result — run ONE run-to-completion task and return its
// published outcome.
// ---------------------------------------------------------------------------
// `task` is invoked as task(RuntimeTaskContext&, TaskResultSlot<Result<T>>&)
// and MUST
// publish exactly once on every path (use translate_task_exception<T>() for
// escaping exceptions; run_task_to_result additionally nets any exception the
// task still lets escape and publishes the translation, so the caller can
// never hang on a throwing task).
//
// Sequence (exactly the audited application pattern):
//   validate -> build -> start -> submit -> wait for publish ->
//   request_stop -> drain -> join -> return the published result.
// A drain/join failure is returned as the bridge result (after best-effort
// shutdown), NOT swallowed.
//
// workers MUST be >= 1; backend MUST be non-null (invalid_state otherwise).
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

    // RuntimeBuilder::build() allocates on the heap and start() spawns the
    // driver thread; either can throw. shutdown() is correct in every Runtime
    // state, so a partially-started Runtime is cleaned up before reporting.
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
        if (rt) (void)rt->shutdown();  // best-effort: correct in every state
        return translate_task_exception<T>();
    }

    // The task-level exception boundary: any exception the task lets escape
    // becomes its published outcome (the Runtime would otherwise swallow it
    // at the Group boundary and the wait below would hang).
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

    // Wait for the task's terminal outcome BEFORE requesting stop: a
    // run-to-completion task must never observe root cancellation while it
    // still has work to do. The calling thread is not a Runtime worker; the
    // driver keeps reaping I/O while we block.
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

}  // namespace sluice::async
