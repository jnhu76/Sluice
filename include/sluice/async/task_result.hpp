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

#ifdef SLUICE_ASYNC_INTERNAL_TESTING
// P1 regression injection (#135 C7): when set, the NEXT run_task_to_result
// call arms its runtime's root group so the bridge's submit() takes the
// documented rollback-and-rethrow path (std::bad_alloc) BEFORE the task can
// be admitted. One-shot by construction (the call that observes it clears
// it); internal-testing builds only — production compiles the site out.
namespace detail {
inline bool task_result_submit_throw_armed = false;

inline bool task_result_test_inject_next_submit_throw() noexcept {
    if (!task_result_submit_throw_armed) return false;
    task_result_submit_throw_armed = false;
    return true;
}
}  // namespace detail
#endif

// ---------------------------------------------------------------------------
// TaskResultSlot<T> — the app-owned terminal-outcome slot (exactly-once
// publish, blocking take). T is the outcome type: Result<X> for
// run_task_to_result callers, or any app result type that embeds its own
// error channel. Lifetime must exceed the task's (the task holds a
// reference). The publishing thread is a Runtime worker; the taking thread is
// any NON-Runtime thread (blocking a Runtime worker on wait_and_take would
// starve the driver — same rule as the application slots).
//
// T must be nothrow move constructible: publish is noexcept and moving the
// outcome into the slot must not be able to terminate the publishing worker.
// ---------------------------------------------------------------------------
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

    // Publish the terminal outcome. Exactly-once effective publish: the FIRST
    // publish wins and wakes the waiter; later publishes are dropped
    // (mirrors the application slots: a racing cleanup path can never
    // overwrite the real outcome).
    //
    // Thread-safety/overhead contract: mutex-protected (publishing while a
    // taking thread contends may briefly block on mtx_ — this is NOT a
    // lock-free path and does not claim "never blocks"); performs no
    // unbounded wait or work; wakes all waiters via the condition variable.
    // Allocation-free given the T nothrow-move requirement above.
    void publish(T r) noexcept {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (done_) return;
            out_.emplace(std::move(r));
            done_ = true;
        }
        cv_.notify_all();
    }

    // Block the calling (non-Runtime) thread until a publish, then take the
    // outcome exactly once. The slot's logical state is
    //   Idle -> Published (first publish) -> Consumed (first take).
    // A SECOND take is a caller contract violation surfaced DETERMINISTICALLY
    // as std::bad_optional_access (the stored outcome was already consumed) —
    // never as a silent moved-from value.
    T wait_and_take() {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return done_; });
        T v = std::move(out_.value());  // throws bad_optional_access on a
                                        // second take (stored value consumed)
        out_.reset();
        return v;
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
//
// PRECONDITION: must be called ONLY from inside an active exception handler
// (a catch block) — the body rethrows the in-flight exception via a bare
// `throw;`. Calling it with no active exception calls std::terminate.
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
//
// Exception boundaries (every leg that can throw is netted into a typed
// result, so an escaping exception can never unwind into ~ApplicationRuntime
// in a non-quiescent state — the destructor fail-fasts there, which would
// turn a bad_alloc into process termination):
//   - build/start throw        -> best-effort shutdown, translate_task_exception;
//   - submit throws            -> same (see the P2-02 contract below);
//   - task body throws         -> published as the translated outcome by the
//                                 wrapper before the Group boundary swallows it;
//   - drain/join report errors -> returned as the bridge result (below).
//
// drain/join precedence (deliberate, preserves the only sound audited
// application semantics — sluice-copy's): a drain/join failure AFTER the task
// published is returned as the bridge result after best-effort shutdown; it
// is NOT swallowed. Swallowing is structurally impossible in this
// architecture: ~ApplicationRuntime fail-fasts in any state other than
// Constructed/StartFailed/Stopped, so a caller that ignored a drain/join
// failure and dropped the Runtime would terminate at scope exit anyway. The
// other three audited apps' `(void)drain(); (void)join();` pattern only ever
// reached its "return the task result" line on the drain/join SUCCESS path —
// where this bridge also returns exactly the task result.
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

    // Deterministic submit-time-throw injection (P1 regression hook,
    // #135 C7): one-shot, internal-testing builds only. Compiled out of the
    // production library entirely.
#ifdef SLUICE_ASYNC_INTERNAL_TESTING
    if (detail::task_result_test_inject_next_submit_throw()) {
        rt->test_inject_next_submit_throw();
    }
#endif

    // The task-level exception boundary: any exception the task lets escape
    // becomes its published outcome (the Runtime would otherwise swallow it
    // at the Group boundary and the wait below would hang).
    //
    // submit() is itself an exception boundary (P1, #135): ApplicationRuntime
    // ::submit rolls back its admission reservation and RETHROWS what
    // Group::async threw (P2-02 — e.g. bad_alloc from a bookkeeping reserve),
    // and constructing the std::function task wrapper may allocate. The
    // Runtime is already Running here, so an exception escaping this
    // statement would unwind into ~ApplicationRuntime in a non-quiescent
    // state and fail fast (group_lifetime_fail_fast) — process termination
    // instead of a typed result. Net it: shutdown first (correct in every
    // state, drives the Runtime destructor-safe), then translate.
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
        (void)rt->shutdown();  // best-effort: correct in every state
        return translate_task_exception<T>();
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
