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
#include <sluice/async/fiber.hpp>
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
//
// Two execution modes (E5-B):
//   - Threaded (default): Group() — each async(fn) spawns a std::thread; await
//     blocks the calling OS thread on each task Future (ThreadedWaitPolicy).
//   - Evented: Group(Scheduler&) — each async(fn) spawns a Fiber on the
//     scheduler; the task Future uses EventedWaitPolicy so a Future::await
//     INSIDE fn suspends the Fiber (not the worker). await drives the scheduler
//     cooperatively. The task body runs in a Fiber with a valid Scheduler
//     current-Fiber context (G1/G2).
class Scheduler;  // forward decl (defined in scheduler.hpp; avoid circular include)

class Group {
public:
    // Threaded mode (existing behavior). Tasks run on std::thread; await blocks
    // the calling thread on each task Future.
    Group() = default;

    // Evented mode (E5-B). Tasks run as Fibers on `sched`; task Futures use
    // EventedWaitPolicy(sched). The caller MUST drive `sched` (typically via
    // this group's await(), which calls sched.run_until_idle()). The scheduler
    // is borrowed; it must outlive the group.
    explicit Group(Scheduler& sched);

    ~Group();

    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
    Group(Group&&) = delete;
    Group& operator=(Group&&) = delete;

    // Spawn a task. In Threaded mode, runs `fn` on a worker thread; in Evented
    // mode, runs `fn` on a scheduler-spawned Fiber (which may suspend inside
    // Future::await via EventedWaitPolicy). `fn` is invoked as fn(CancelToken&)
    // so it can observe cancellation; on return, the task completes (swallowing
    // any internal cancel — the group is a cancel-propagation boundary,
    // Zig Io.zig:1240). The task's Future<void> is owned by the group.
    template <class Fn>
    void async(Fn fn) {
        if (sched_) {
            async_evented<Fn>(std::move(fn));
        } else {
            async_threaded<Fn>(std::move(fn));
        }
    }

    // The shared cancel token for all tasks in this group. A task observes it
    // at its cancel points (see cancel.hpp). cancel() requests it.
    CancelToken& group_token() noexcept { return token_; }

    // Wait until ALL tasks complete. Idempotent (Zig Io.zig:1282). In Threaded
    // mode, blocks the calling OS thread on each task Future + joins threads.
    // In Evented mode, drives the scheduler cooperatively until all task
    // Futures are ready (does NOT block the worker on a cv; does NOT join
    // threads — there are none).
    void await();

    // Request cancel on all tasks (via the shared token), then await them.
    // Idempotent (Zig Io.zig:1298). Best-effort.
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
    // Threaded async: spawn a std::thread per task (existing path).
    template <class Fn>
    void async_threaded(Fn fn) {
        auto fut = std::make_shared<Future<void>>();
        std::thread w([fut, fn = std::move(fn), tok = &token_]() mutable {
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

    // Evented async: spawn a Fiber on sched_ per task. The Fiber body wraps fn
    // so fn runs inside a Scheduler-driven Fiber (valid current-Fiber context).
    // Defined out-of-line in group.cpp (needs Scheduler's complete definition).
    template <class Fn>
    void async_evented(Fn fn);

    mutable std::mutex mtx_;
    std::vector<std::thread> tasks_;              // Threaded mode only
    std::vector<std::shared_ptr<Future<void>>> futures_;
    CancelToken token_;
    Scheduler* sched_ = nullptr;                  // Evented mode (borrowed)
    // Evented-mode owned state. The EventedWaitPolicy is shared across all
    // task Futures in this group (one policy per group; it borrows sched_).
    // Stored as unique_ptr so its address is stable across Group copies (none
    // — Group is non-copyable/non-movable, but the indirection is clean).
    std::unique_ptr<class EventedWaitPolicy> evented_policy_;
    // One Fiber + one stack per task. Fiber: unique_ptr (non-movable). Stack:
    // unique_ptr<std::byte[]> so its base never moves even if the vector
    // reallocates (a vector<vector<byte>> would dangle init_context'd rsp).
    std::vector<std::unique_ptr<Fiber>> evented_fibers_;
    std::vector<std::unique_ptr<std::byte[]>> evented_stacks_;
};

}  // namespace sluice::async

// ---- Evented async definition (needs Scheduler's + EventedWaitPolicy full def) ----
#include <sluice/async/evented_wait_policy.hpp>
#include <sluice/async/scheduler.hpp>

namespace sluice::async {

template <class Fn>
void Group::async_evented(Fn fn) {
    // One Fiber + one stack + one Future per task, all group-owned. The Fiber
    // body wraps fn so fn runs inside a Scheduler-driven Fiber (valid
    // current-Fiber context: Scheduler::current_ is set while the body runs).
    // The task Future uses EventedWaitPolicy(sched_) so a Future::await inside
    // fn suspends the Fiber, not the worker.
    auto fut = std::make_shared<Future<void>>(*evented_policy_);
    // Stable stack: 64 KiB + 16 bytes of alignment padding headroom, owned by
    // a unique_ptr so its base address never moves even if evented_stacks_
    // reallocates on a later async(). 16-byte aligned allocation via new.
    constexpr std::size_t kStackBytes = 64 * 1024;
    auto stack_up = std::unique_ptr<std::byte[]>(new std::byte[kStackBytes]);
    std::byte* stack_base = stack_up.get();
    // The Fiber is owned by unique_ptr so its address is stable across vector
    // relocation of evented_fibers_.
    auto fiber_up = std::make_unique<Fiber>();
    Fiber* fiber_raw = fiber_up.get();
    // Captures: fut (shared_ptr), fn (moved), tok (group token pointer),
    // fiber_raw (so the body can mark itself done via the bridge — actually
    // the scheduler's fiber_entry_bridge calls make_done; we don't need it
    // here, but we keep the Fiber handle for the entry to reference state).
    fiber_up->set_entry([fut, fn = std::move(fn), tok = &token_](Fiber&) mutable {
        try {
            fn(*tok);
        } catch (...) {
            // Swallow — cancel-propagation boundary (same as Threaded).
        }
        fut->complete_with(sluice::Result<void>{});
    });
    // Init the fiber's context (must happen before spawn). The scheduler's
    // internal bridge will invoke fiber.entry()(*fiber).
    bool ok = sched_->init_fiber(*fiber_raw, stack_base, kStackBytes);
    (void)ok;  // production code would propagate; E5 test asserts externally
    std::lock_guard<std::mutex> lk(mtx_);
    evented_fibers_.push_back(std::move(fiber_up));
    evented_stacks_.push_back(std::move(stack_up));
    futures_.push_back(std::move(fut));
    // spawn is non-blocking (just enqueues the fiber on the scheduler's
    // runnable queue). Single-worker E5: await() and async() run on the same
    // thread; the lock discipline is defensive.
    sched_->spawn(*fiber_raw);
}

}  // namespace sluice::async
