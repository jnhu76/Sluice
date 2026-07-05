// sluice::async::Group — unordered task set (sluice-CORE-029, T3).
//
// Derived from Zig std.Io Group (Io.zig:1218-1303). An unordered set of tasks
// awaitable/cancelable only as a whole. Group is a CANCEL-PROPAGATION BOUNDARY:
// tasks swallow IoError::canceled (Zig Io.zig:1240-1261 — error.Canceled is
// swallowed inside group tasks). Per-task resources are freed when each task
// returns, not at group completion (Zig Io.zig:1211 — resource guarantee).
//
// cppio shape (no fiber runtime): a Group owns a set of Future<void> (one per
// async() task) and a shared CancelToken. async(fn) spawns a worker thread that
// runs fn (which may observe the shared token at cancel points) and completes
// the Future on return. await() blocks until ALL tasks complete; cancel()
// requests cancel on all tasks then awaits.
//
// Layering: composes Future<T> (T2) and CancelToken (T1). No scheduler.
//
// Non-goals (deferred):
//   - No result aggregation (Zig Group is fire-and-forget: tasks return void
//     after swallowing Cancel; results flow through separate Futures). This
//     matches Zig's Group.Task (Io.zig:484) which carries no result.
//   - No concurrent() variant (Zig requires a concurrency unit; cppio has no
//     notion yet — every async() spawns a thread). Add when PHASE E lands.
//   - await() blocks the calling thread (Threaded-equivalent; not fiber yield).
#pragma once

#include <sluice/async/cancel.hpp>
#include <sluice/async/future.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sluice::async {

// An unordered set of fire-and-forget tasks, awaitable/cancelable as a whole.
// Mirrors Zig Io.Group (Io.zig:1218). Tasks are added via async(); the group
// is awaited as a whole (await waits for ALL tasks) or canceled as a whole
// (cancel requests cancel on every task then awaits).
class Group {
public:
    Group() = default;
    ~Group();

    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
    Group(Group&&) = delete;
    Group& operator=(Group&&) = delete;

    // Spawn a task: runs `fn` on a worker thread. `fn` may observe the group's
    // shared cancel token (group_token()) at cancel points; on return, the
    // task completes (swallowing any internal cancel — the group is a cancel-
    // propagation boundary, Zig Io.zig:1240). The task's Future<void> is owned
    // by the group; await()/cancel() reap it. Mirrors Zig Group.async.
    // `fn` is invoked as fn(CancelToken&) so it can observe cancellation.
    template <class Fn>
    void async(Fn fn) {
        auto fut = std::make_shared<Future<void>>();
        // The shared token: every task sees the SAME token so cancel() hits all.
        std::thread w([fut, fn = std::move(fn), tok = &token_]() mutable {
            // Run; swallow IoError::canceled (cancel-propagation boundary).
            try {
                fn(*tok);
            } catch (...) {
                // Swallow — group tasks do not propagate exceptions across the
                // boundary (mirrors Zig swallowing error.Canceled).
            }
            fut->complete_with(sluice::Result<void>{});
        });
        std::lock_guard<std::mutex> lk(mtx_);
        tasks_.push_back(std::move(w));
        futures_.push_back(std::move(fut));
    }

    // The shared cancel token for all tasks in this group. A task observes it
    // at its cancel points (see cancel.hpp). cancel() requests it.
    CancelToken& group_token() noexcept { return token_; }

    // Block until ALL tasks complete. Idempotent (Zig Io.zig:1282). Returns void
    // — the group is fire-and-forget; per-task results are not aggregated.
    void await();

    // Request cancel on all tasks (via the shared token), then await them.
    // Idempotent (Zig Io.zig:1298). Tasks that observe the request should
    // return promptly; those that do not will complete normally (best-effort).
    void cancel() {
        token_.request();
        await();
    }

    // Number of tasks currently in the group (live + completed, before await
    // reaps). For tests/inspection.
    std::size_t size() const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        return futures_.size();
    }

private:
    mutable std::mutex mtx_;
    std::vector<std::thread> tasks_;              // joined in destructor / await
    std::vector<std::shared_ptr<Future<void>>> futures_;
    CancelToken token_;
};

}  // namespace sluice::async
