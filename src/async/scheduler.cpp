// Implementation of the minimal single-worker Scheduler (sluice-CORE-E4).
//
// See include/sluice/async/scheduler.hpp for scope and ownership. This is the
// single-worker experiment proving ADR-execution-model §9.1 (scheduler
// liveness). It is NOT the final runtime.
#include <sluice/async/scheduler.hpp>

#include <sluice/async/fiber_ctx.hpp>

#include <utility>

namespace sluice::async {

namespace {

// Bridge: fiber_ctx's Entry is void(*)(Switch*, void*); Fiber's entry is
// std::function<void(Fiber&)> stored on the Fiber. This trampoline recovers
// the Fiber* (passed as user_data) and invokes its entry. On return, it marks
// the fiber done and switches back to the scheduler forever (the trampoline
// must not return — it has no return address).
//
// The scheduler context pointer is stashed per-fiber so a fiber that completes
// can switch back to the scheduler. We store it in a thread-local during
// run_until_idle (single-worker => one OS thread).
//
// Per-fiber "scheduler ctx to return to" lives in a side table keyed by Fiber*,
// set when run_next switches into the fiber. We use a thread-local pointer
// instead because there is exactly one scheduler per thread in E4.
thread_local fiber_ctx::Context* g_sched_ctx = nullptr;

void fiber_entry_bridge(fiber_ctx::Switch* resumed_by, void* user_data) {
    (void)resumed_by;  // E4 does not use the resume-message field
    auto* fiber = static_cast<Fiber*>(user_data);
    // Run the user entry. It may call Scheduler::await_completion_* which
    // switches back to the scheduler; control returns here when the scheduler
    // switches back into this fiber.
    if (fiber->entry()) {
        fiber->entry()(*fiber);
    }
    fiber->make_done();
    // Switch back to the scheduler forever. The fiber is done; the scheduler
    // will not re-select it (run_next skips done fibers).
    fiber_ctx::Switch s;
    s.old = &fiber->ctx;       // our own context (unused after done)
    s.new_ = g_sched_ctx;      // resume scheduler
    (void)fiber_ctx::context_switch(&s);
    __builtin_unreachable();   // scheduler never resumes a done fiber
}

}  // namespace

Scheduler::Scheduler(AsyncIoContext& ctx) noexcept : ctx_(ctx) {}

bool Scheduler::init_fiber(Fiber& fiber, std::byte* stack_base, std::size_t stack_size) {
    return fiber_ctx::init_context(fiber.ctx, &fiber_entry_bridge, &fiber,
                                   stack_base, stack_size);
}

void Scheduler::spawn(Fiber& fiber) noexcept {
    // Initialize the fiber's context if it hasn't been already (idempotent:
    // init_context overwrites). The fiber's stack is owned by the caller; we
    // require the caller to have called init_context OR we do it here. For E4
    // simplicity, the TEST calls init_context (it owns the stack). Here we
    // just enqueue.
    fiber.make_runnable();
    runnable_.push_back(&fiber);
}

void Scheduler::run_until_idle() {
    running_ = true;
    g_sched_ctx = &sched_ctx_;
    // Drive until neither runnable fibers nor waiting fibers remain. A waiting
    // fiber may be woken by a backend completion observed via poll(), so even
    // when the runnable queue is empty we must poll while fibers are waiting.
    // (If no fiber is runnable AND none is waiting, there is nothing to drive.)
    while (true) {
        wake_ready_completions();  // polls ctx_; wakes any fiber whose op fired
        if (runnable_.empty()) {
            // Nothing runnable right now. If fibers are still waiting, their
            // completions haven't fired yet — the test must release them. Stop
            // (do not busy-loop) and let the caller stage more completions and
            // call run_until_idle again.
            break;
        }
        run_next();
    }
    running_ = false;
    g_sched_ctx = nullptr;
}

void Scheduler::run_next() {
    Fiber* fiber = runnable_.front();
    runnable_.pop_front();
    current_ = fiber;
    // Switch into the fiber. Its context was either init'd (first run) or
    // saved when it last suspended. On return, the fiber has either completed
    // (done) or suspended via await_completion_* (switched back to sched_ctx_).
    fiber->make_running();
    fiber_ctx::Switch s;
    s.old = &sched_ctx_;   // save scheduler state here
    s.new_ = &fiber->ctx;  // resume the fiber
    (void)fiber_ctx::context_switch(&s);
    // Control resumes here when the fiber switches back to &sched_ctx_.
    current_ = nullptr;
}

std::size_t Scheduler::wake_ready_completions() {
    // Poll the backend. Each ready Completion wakes its associated fiber
    // (waiting -> runnable) and the association is removed (exactly-once).
    std::size_t woken = 0;
    // Take a snapshot of waiting fibers BEFORE polling: a completion that
    // fires inside poll will mark a Completion ready; we then match it.
    // Snapshot is by key (Completion*); pointers are stable.
    std::size_t reaped = ctx_.poll();
    if (reaped == 0) return 0;

    // Walk the waiting maps; any whose Completion is now ready gets woken.
    for (auto it = waiting_size_.begin(); it != waiting_size_.end();) {
        auto* c = static_cast<Completion<std::size_t>*>(it->first);
        if (c->ready()) {
            Fiber* f = it->second;
            it = waiting_size_.erase(it);
            f->make_runnable();
            runnable_.push_back(f);
            ++woken;
        } else {
            ++it;
        }
    }
    for (auto it = waiting_void_.begin(); it != waiting_void_.end();) {
        auto* c = static_cast<Completion<void>*>(it->first);
        if (c->ready()) {
            Fiber* f = it->second;
            it = waiting_void_.erase(it);
            f->make_runnable();
            runnable_.push_back(f);
            ++woken;
        } else {
            ++it;
        }
    }
    return woken;
}

void Scheduler::await_completion_size(Completion<std::size_t>& c) {
    // current_ is the running fiber. Mark it waiting, associate, switch back
    // to the scheduler.
    Fiber* me = current_;
    me->make_waiting();
    waiting_size_[static_cast<void*>(&c)] = me;
    fiber_ctx::Switch s;
    s.old = &me->ctx;       // save fiber state
    s.new_ = g_sched_ctx;   // resume scheduler
    (void)fiber_ctx::context_switch(&s);
    // Resumed: the scheduler observed the completion and switched back into
    // me->ctx. c is now ready.
}

void Scheduler::await_completion_void(Completion<void>& c) {
    Fiber* me = current_;
    me->make_waiting();
    waiting_void_[static_cast<void*>(&c)] = me;
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = g_sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

}  // namespace sluice::async
