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
    idle_workers_.store(0, std::memory_order_release);
    global_terminate_.store(false, std::memory_order_release);

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
    // Multi-worker coordinated loop. Each iteration:
    //   1. Try to get a runnable Fiber (local queue → pending_spawn_).
    //   2. If got one: run it, continue.
    //   3. No runnable: do readiness drain (wake ready Completions/flags, route
    //      to owner workers). If any woken, continue.
    //   4. Still nothing: check for Completion-backed progress.
    //      Worker 0 drives backend (serialized; E7-C generalizes).
    //   5. No progress anywhere: check global quiescence. ALL workers must agree
    //      there is no runnable/running work AND no Completion outstanding before
    //      any worker may exit. Non-progressing workers park on inbox_cv with a
    //      timeout; the run terminates when global_quiescent becomes true.
    while (true) {
        // 1. Get a runnable Fiber.
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
            // Found work: reset idle count (other workers' idle observations
            // are stale).
            idle_workers_.store(0, std::memory_order_release);
            run_next_on(ws, f);
            continue;
        }

        // 2. Readiness drain: wake any ready Completions/flags.
        {
            std::lock_guard<std::mutex> lk(global_mtx_);
            if (wake_ready_completions_locked() || wake_ready_flags_locked()) {
                idle_workers_.store(0, std::memory_order_release);
                continue;  // woken Fibers routed to their owner workers
            }
        }

        // 3. Check Completion-backed progress (single driver for E7-A/E7-B).
        bool completion_pending;
        {
            std::lock_guard<std::mutex> lk(global_mtx_);
            completion_pending = !waiting_size_.empty() || !waiting_void_.empty();
        }

        if (completion_pending && ws->id == 0) {
            // Worker 0 drives backend progress.
            auto wr = ctx_.wait_one();
            if (wr.has_value() && wr.value() > 0) {
                // Reap; next iteration's readiness drain will route woken Fibers.
                continue;
            }
            // wait_one returned 0 (Fake: nothing staged). Fall through to
            // quiescence check.
        }

        // 4. No progress. Attempt to go idle and check global quiescence.
        //    The quiescence decision is made atomically under global_mtx_ by
        //    the LAST worker to go idle: it re-checks ALL state (runnable,
        //    running, completion) under the lock before setting terminate.
        //    Any route_runnable_locked call that runs while global_mtx_ is held
        //    clears terminate + resets idle — so the quiescence check and the
        //    route are mutually exclusive.
        {
            std::lock_guard<std::mutex> lk(global_mtx_);
            bool any_runnable = !pending_spawn_.empty();
            for (auto& w : workers_) {
                std::lock_guard<std::mutex> wlk(w->inbox_mtx);
                if (!w->local_runnable.empty()) { any_runnable = true; break; }
            }
            bool any_completion = !waiting_size_.empty() || !waiting_void_.empty();
            bool any_running = running_fiber_count_.load(std::memory_order_acquire) > 0;

            if (any_runnable || any_running || any_completion) {
                // Work exists. Reset idle and loop (will find it on next iteration).
                idle_workers_.store(0, std::memory_order_release);
            } else {
                // No work visible. Increment idle.
                unsigned prev = idle_workers_.fetch_add(1, std::memory_order_acq_rel);
                if (prev + 1 >= workers_.size()) {
                    // ALL workers idle. FINAL re-check under the same lock —
                    // no route_runnable_locked can have run since our state
                    // check (it needs global_mtx_ which we hold).
                    bool still_no_work = !pending_spawn_.empty() == false;
                    for (auto& w : workers_) {
                        std::lock_guard<std::mutex> wlk(w->inbox_mtx);
                        if (!w->local_runnable.empty()) { still_no_work = false; break; }
                    }
                    if (still_no_work &&
                        running_fiber_count_.load(std::memory_order_acquire) == 0 &&
                        waiting_size_.empty() && waiting_void_.empty()) {
                        // Confirmed: global quiescence or MW-S3. Terminate.
                        global_terminate_.store(true, std::memory_order_release);
                        for (auto& w : workers_) {
                            std::lock_guard<std::mutex> wlk(w->inbox_mtx);
                            w->inbox_cv.notify_all();
                        }
                        break;
                    }
                    // State changed since first check. Reset and retry.
                    idle_workers_.store(0, std::memory_order_release);
                }
            }
        }

        // Check terminate signal. If set, exit (the quiescence check above
        // confirmed no work under global_mtx_).
        if (global_terminate_.load(std::memory_order_acquire)) break;

        // 5. Park briefly waiting for routed work or a state change.
        {
            std::unique_lock<std::mutex> ws_lk(ws->inbox_mtx);
            ws->inbox_cv.wait_for(ws_lk, std::chrono::milliseconds(1),
                                   [&] { return !ws->local_runnable.empty() ||
                                                global_terminate_.load(std::memory_order_acquire); });
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
    // Clear the terminate signal: new work was routed, so the run is NOT over.
    // A worker that was about to exit must re-check its inbox (late-drain).
    global_terminate_.store(false, std::memory_order_release);
    idle_workers_.store(0, std::memory_order_release);
    if (owner) {
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
