// Implementation of the multi-worker Scheduler (sluice-CORE-E7-A).
//
// E7-A: worker-local execution state + multi-worker run skeleton.
// See docs/adr/ADR-execution-model.md §9.2.
//
// This commit localizes sched_ctx + current_ into per-Worker WorkerState and
// adds run(worker_count). E7-B adds pinned routing; E7-C adds MW coordination.
// For E7-A, the multi-worker path uses a simple model: all spawned Fibers go
// into pending_spawn_; workers pick from it round-robin; backend progress is
// done by worker 0 only (single-driver for E7-A; E7-C generalizes); the
// coordinated run returns when no runnable Fiber remains and no Completion
// is outstanding. This is sufficient to prove worker-local execution state
// (E7-T1/T2) and preserve single-worker regression (E4-E6).
#include <sluice/async/scheduler.hpp>

#include <sluice/async/fiber_ctx.hpp>

#include <utility>

namespace sluice::async {

namespace {

// TLS: the current Worker's WorkerState. Set by worker_loop before any Fiber
// runs; used by await_completion_*/await_ready_flag to find the current
// Fiber and scheduler context. This is genuine Worker-local state (one Worker
// per OS thread) — NOT a process-global slot shared across Workers.
thread_local WorkerState* g_worker = nullptr;

void fiber_entry_bridge(fiber_ctx::Switch* resumed_by, void* user_data) {
    (void)resumed_by;
    auto* fiber = static_cast<Fiber*>(user_data);
    if (fiber->entry()) {
        fiber->entry()(*fiber);
    }
    fiber->make_done();
    // Switch back to this worker's scheduler context forever.
    fiber_ctx::Switch s;
    s.old = &fiber->ctx;
    s.new_ = &g_worker->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    __builtin_unreachable();
}

}  // namespace

Scheduler::Scheduler(AsyncIoContext& ctx) noexcept : ctx_(ctx) {}

Scheduler::~Scheduler() {
    // Workers are joined in run(). Nothing to drain here for E7-A.
}

bool Scheduler::init_fiber(Fiber& fiber, std::byte* stack_base, std::size_t stack_size) {
    return fiber_ctx::init_context(fiber.ctx, &fiber_entry_bridge, &fiber,
                                   stack_base, stack_size);
}

void Scheduler::spawn(Fiber& fiber) noexcept {
    fiber.make_runnable();
    // Round-robin assignment to worker local queues so that Fibers distribute
    // across workers (required for E7-T1/T2 concurrency tests). If no workers
    // exist yet (pre-run), use pending_spawn_ which will be distributed when
    // run() creates workers.
    std::lock_guard<std::mutex> lk(global_mtx_);
    if (!workers_.empty()) {
        unsigned target = next_spawn_worker_++ % static_cast<unsigned>(workers_.size());
        std::lock_guard<std::mutex> wlk(workers_[target]->inbox_mtx);
        workers_[target]->local_runnable.push_back(&fiber);
        workers_[target]->inbox_cv.notify_one();
    } else {
        pending_spawn_.push_back(&fiber);
    }
}

WorkerState* Scheduler::current_worker() {
    return g_worker;
}

void Scheduler::run(unsigned worker_count) {
    if (worker_count == 0) worker_count = 1;

    // WorkerState is address-stable across run() calls (wait registrations may
    // hold WorkerState* pointers between calls — E7-ABORT-6 lifetime). Grow or
    // shrink as needed, but never destroy/recreate existing workers within
    // the Scheduler's lifetime.
    while (workers_.size() < worker_count) {
        workers_.push_back(std::make_unique<WorkerState>());
        workers_.back()->id = static_cast<unsigned>(workers_.size() - 1);
    }
    // Ensure worker IDs are correct.
    for (unsigned i = 0; i < workers_.size(); ++i) {
        workers_[i]->id = i;
    }

    // Distribute any pending_spawn_ across workers round-robin.
    {
        std::lock_guard<std::mutex> lk(global_mtx_);
        unsigned w = 0;
        while (!pending_spawn_.empty()) {
            auto* f = pending_spawn_.front();
            pending_spawn_.pop_front();
            std::lock_guard<std::mutex> wlk(workers_[w % worker_count]->inbox_mtx);
            workers_[w % worker_count]->local_runnable.push_back(f);
            workers_[w % worker_count]->inbox_cv.notify_one();
            ++w;
        }
    }
    next_spawn_worker_ = 0;

    in_coordinated_run_ = true;
    active_worker_count_.store(worker_count, std::memory_order_release);
    running_fiber_count_.store(0, std::memory_order_release);

    if (worker_count == 1) {
        // Single-worker fast path: run inline (no thread spawn). This preserves
        // the E4-E6 behavior exactly — run_until_idle on the caller's thread.
        g_worker = workers_[0].get();
        workers_[0]->active.store(true, std::memory_order_release);
        worker_loop(workers_[0].get());
        workers_[0]->active.store(false, std::memory_order_release);
        g_worker = nullptr;
    } else {
        // Multi-worker: spawn OS threads, each running worker_loop.
        std::vector<std::thread> threads;
        threads.reserve(worker_count);
        for (unsigned i = 0; i < worker_count; ++i) {
            threads.emplace_back([this, i] {
                g_worker = workers_[i].get();
                workers_[i]->active.store(true, std::memory_order_release);
                worker_loop(workers_[i].get());
                workers_[i]->active.store(false, std::memory_order_release);
                g_worker = nullptr;
            });
        }
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
    }

    in_coordinated_run_ = false;
    active_worker_count_.store(0, std::memory_order_release);
}

void Scheduler::worker_loop(WorkerState* ws) {
    // E7-A simplified loop: each worker tries to get a runnable Fiber, run it,
    // and repeat. When no runnable Fiber exists anywhere, the worker checks
    // for Completion-backed progress. The first worker (id==0) drives backend
    // progress (single backend driver for E7-A; E7-C generalizes to serialized
    // access). The coordinated run ends when no runnable Fiber + no
    // outstanding Completion + (for multi-worker) a quiescence check.
    //
    // This is a simplified E7-A loop; E7-C refines it into the full
    // MW-S1/MW-S2/MW-S3/QUIESCENT protocol.
    while (true) {
        // Try to get a runnable Fiber: local queue first, then pending_spawn_.
        Fiber* f = nullptr;
        {
            std::lock_guard<std::mutex> lk(ws->inbox_mtx);
            if (!ws->local_runnable.empty()) {
                f = ws->local_runnable.front();
                ws->local_runnable.pop_front();
            }
        }
        if (!f) {
            std::lock_guard<std::mutex> lk(global_mtx_);
            if (!pending_spawn_.empty()) {
                f = pending_spawn_.front();
                pending_spawn_.pop_front();
            }
        }

        if (f) {
            run_next_on(ws, f);
            continue;
        }

        // No runnable Fiber. Check backend progress (single driver for E7-A).
        bool made_progress = false;
        {
            std::lock_guard<std::mutex> lk(global_mtx_);
            // Pre-admission readiness drain (E7-C6): observe ready waits before
            // deciding to block or return.
            made_progress = wake_ready_completions_locked() || wake_ready_flags_locked();
        }
        if (made_progress) {
            // Route woken Fibers to workers and continue.
            continue;
        }

        // Check if there are Completion-backed waits pending (S2).
        bool completion_pending;
        {
            std::lock_guard<std::mutex> lk(global_mtx_);
            completion_pending = !waiting_size_.empty() || !waiting_void_.empty();
        }

        if (completion_pending && ws->id == 0) {
            // S2: only worker 0 drives backend progress (E7-A simplification).
            auto wr = ctx_.wait_one();
            if (!wr.has_value() || wr.value() == 0) {
                // No progress; check global state.
                std::lock_guard<std::mutex> lk(global_mtx_);
                bool any_runnable = !pending_spawn_.empty();
                for (auto& w : workers_) {
                    std::lock_guard<std::mutex> wlk(w->inbox_mtx);
                    if (!w->local_runnable.empty()) { any_runnable = true; break; }
                }
                if (!any_runnable) break;  // MW-S3 or quiescent: return
            }
            continue;
        }

        // Multi-worker coordination (E7-A simplified): if we're not worker 0
        // and have nothing to do, check if the run is done.
        if (ws->id != 0) {
            std::lock_guard<std::mutex> lk(global_mtx_);
            bool any_runnable = !pending_spawn_.empty();
            for (auto& w : workers_) {
                std::lock_guard<std::mutex> wlk(w->inbox_mtx);
                if (!w->local_runnable.empty()) { any_runnable = true; break; }
            }
            bool any_completion = !waiting_size_.empty() || !waiting_void_.empty();
            if (!any_runnable && !any_completion) break;  // done
            // Wait briefly for routed work or state change.
            std::unique_lock<std::mutex> ws_lk(ws->inbox_mtx);
            ws->inbox_cv.wait_for(ws_lk, std::chrono::milliseconds(1),
                                   [&] { return !ws->local_runnable.empty() || !ws->inbox.empty(); });
        } else {
            // Worker 0 with no progress and no completion: check done.
            std::lock_guard<std::mutex> lk(global_mtx_);
            bool any_runnable = !pending_spawn_.empty();
            for (auto& w : workers_) {
                std::lock_guard<std::mutex> wlk(w->inbox_mtx);
                if (!w->local_runnable.empty()) { any_runnable = true; break; }
            }
            if (!any_runnable) break;  // MW-S3 or quiescent: return
        }
    }
}

void Scheduler::run_next_on(WorkerState* ws, Fiber* fiber) {
    ws->current = fiber;
    running_fiber_count_.fetch_add(1, std::memory_order_acq_rel);
    fiber->make_running();
    fiber_ctx::Switch s;
    s.old = &ws->sched_ctx;
    s.new_ = &fiber->ctx;
    (void)fiber_ctx::context_switch(&s);
    // Control resumes here when the fiber switches back to ws->sched_ctx.
    ws->current = nullptr;
    running_fiber_count_.fetch_sub(1, std::memory_order_acq_rel);
}

void Scheduler::route_runnable(Fiber* f, WorkerState* owner) {
    // E7-A: route to the owner's local queue, or to pending_spawn_ if no owner.
    if (owner) {
        std::lock_guard<std::mutex> lk(owner->inbox_mtx);
        owner->local_runnable.push_back(f);
        owner->inbox_cv.notify_one();
    } else {
        pending_spawn_.push_back(f);
    }
}

bool Scheduler::wake_ready_completions_locked() {
    // Called with global_mtx_ held. Polls backend + scans Completion waits.
    bool woken = false;
    (void)ctx_.poll();
    for (auto it = waiting_size_.begin(); it != waiting_size_.end();) {
        auto* c = static_cast<Completion<std::size_t>*>(it->first);
        if (c->ready()) {
            Fiber* f = it->second.fiber;
            WorkerState* owner = it->second.owner;
            it = waiting_size_.erase(it);
            f->make_runnable();
            route_runnable_locked(f, owner);
            woken = true;
        } else {
            ++it;
        }
    }
    for (auto it = waiting_void_.begin(); it != waiting_void_.end();) {
        auto* c = static_cast<Completion<void>*>(it->first);
        if (c->ready()) {
            Fiber* f = it->second.fiber;
            WorkerState* owner = it->second.owner;
            it = waiting_void_.erase(it);
            f->make_runnable();
            route_runnable_locked(f, owner);
            woken = true;
        } else {
            ++it;
        }
    }
    return woken;
}

bool Scheduler::wake_ready_flags_locked() {
    bool woken = false;
    for (auto it = waiting_ready_.begin(); it != waiting_ready_.end();) {
        if (it->first->load(std::memory_order::acquire)) {
            Fiber* f = it->second.fiber;
            WorkerState* owner = it->second.owner;
            it = waiting_ready_.erase(it);
            f->make_runnable();
            route_runnable_locked(f, owner);
            woken = true;
        } else {
            ++it;
        }
    }
    return woken;
}

void Scheduler::route_runnable_locked(Fiber* f, WorkerState* owner) {
    // Must be called with global_mtx_ held.
    if (owner) {
        // Route to owner's local queue. Need owner->inbox_mtx (different from
        // global_mtx_). Safe: global_mtx_ protects waiting maps; inbox_mtx
        // protects the queue. No deadlock because we never acquire them in
        // reverse order.
        std::lock_guard<std::mutex> lk(owner->inbox_mtx);
        owner->local_runnable.push_back(f);
        owner->inbox_cv.notify_one();
    } else {
        pending_spawn_.push_back(f);
    }
}

void Scheduler::await_completion_size(Completion<std::size_t>& c) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    me->make_waiting();
    {
        std::lock_guard<std::mutex> lk(global_mtx_);
        waiting_size_[static_cast<void*>(&c)] = {me, ws};
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

void Scheduler::await_completion_void(Completion<void>& c) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    me->make_waiting();
    {
        std::lock_guard<std::mutex> lk(global_mtx_);
        waiting_void_[static_cast<void*>(&c)] = {me, ws};
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

void Scheduler::await_ready_flag(const std::atomic<bool>& ready) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    if (ready.load(std::memory_order::acquire)) return;
    {
        std::lock_guard<std::mutex> lk(global_mtx_);
        waiting_ready_[&ready] = {me, ws};
    }
    if (ready.load(std::memory_order::acquire)) {
        std::lock_guard<std::mutex> lk(global_mtx_);
        waiting_ready_.erase(&ready);
        return;
    }
    me->make_waiting();
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

std::size_t Scheduler::runnable_count() const {
    std::size_t total = 0;
    {
        std::lock_guard<std::mutex> lk(global_mtx_);
        total += pending_spawn_.size();
    }
    for (auto& w : workers_) {
        std::lock_guard<std::mutex> wlk(w->inbox_mtx);
        total += w->local_runnable.size();
    }
    return total;
}

}  // namespace sluice::async
