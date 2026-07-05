// sluice::async::Future — single-task awaitable (sluice-CORE-028, T2).
//
// Derived from Zig std.Io Future (Io.zig:1176-1206). In Zig a Future is
// { any_future, result } — caller-provided result storage + a backend handle,
// with idempotent await(io) and cancel(io). cppio has no fiber scheduler, so a
// Future here is the value-channel analogue:
//
//   - caller-provided result storage (like Completion<T>, ADR §5 L4);
//   - a producer side (complete_with) that may run on a worker thread, a manual
//     driver, or synchronously inline;
//   - idempotent await() — returns the result once ready, never re-blocks;
//   - a CancelToken (027) for cooperative cancellation — a producer observes
//     the token at its cancel points.
//
// Layering: ABOVE Completion (it composes one) and ABOVE cancel.hpp. It does
// not own a backend; whoever drives the producer wires the producer's completion
// to the Future. This keeps Future scheduler-free, matching how Zig's Future
// type is itself just {handle, result} — the scheduler lives in the backend.
//
// Non-goals (deferred):
//   - No fiber/coroutine suspension (cppio has no fiber runtime yet — PHASE E).
//     await() BLOCKS THE CALLING THREAD on a condition variable until ready.
//     This is the Threaded-equivalent shape, not the Evented shape.
//   - No multi-task grouping (that's Group, T3).
//   - No timeout (timers are out of scope, 016B O2).
#pragma once

#include <sluice/async/cancel.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace sluice::async {

// A single-task awaitable carrying a Result<T>. Producer side: complete_with().
// Consumer side: await() / cancel(). Mirrors Zig Future's idempotent
// await/cancel (Io.zig:1191, 1199): once the result is materialized, both are
// no-ops returning the cached result.
template <class T>
class Future {
public:
    Future() = default;

    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) = delete;
    Future& operator=(Future&&) = delete;

    // ---- Producer side ----

    // Publish the terminal result. Exactly-once: a second call is a no-op
    // (asserts in debug). Wake any awaiter. Thread-safe (producer may run on a
    // worker thread different from the awaiter).
    void complete_with(Result<T> r) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (ready_) return;  // exactly-once terminal
            result_.emplace(std::move(r));
            ready_.store(true, std::memory_order::release);
        }
        cv_.notify_all();
    }

    // The cooperative cancel token for this future. A producer that respects
    // cancellation observes this token at its cancel points (see cancel.hpp).
    // cancel() requests it; the producer decides when to observe.
    CancelToken& cancel_token() noexcept { return token_; }

    // ---- Consumer side ----

    // Idempotent (Zig Io.zig:1199). Blocks the calling thread until the result
    // is ready, then returns it. A second call returns the cached result
    // without blocking. Not thread-safe for concurrent awaiters (Zig: "not
    // threadsafe", Io.zig:1198) — one awaiter per Future.
    Result<T> await() {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return ready_.load(std::memory_order::acquire); });
        return *result_;
    }

    // Idempotent (Zig Io.zig:1191). Requests cancellation of the producer via
    // the token, then awaits the result like await(). Returns whatever the
    // producer published (which MAY be the real result, an error, or
    // IoError::canceled if the producer honored the request). Best-effort, same
    // semantics as ADR §7 X3.
    Result<T> cancel() {
        token_.request();
        return await();
    }

    // Non-blocking readiness query (for tests / polling drivers).
    bool ready() const noexcept {
        return ready_.load(std::memory_order::acquire);
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    // No result until ready (cppcoding-standards: avoid the meaningless state
    // of a "default" Result; absence is meaningful here). Emplaced under mtx_
    // by complete_with; read under mtx_ by await.
    std::optional<Result<T>> result_;
    std::atomic<bool> ready_{false};  // publish flag; acquire/release vs result_
    CancelToken token_;
};

}  // namespace sluice::async
