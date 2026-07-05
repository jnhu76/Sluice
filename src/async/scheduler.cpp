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
    // Hybrid poll/wait progress (E6, P3 from the E6 audit):
    //   S1 (runnable non-empty): non-blocking wake passes (poll); run a Fiber.
    //       Backend progress observation is non-blocking here — calling
    //       wait_one() would strand a runnable Fiber (E4 liveness violation).
    //   S2 (runnable empty, ≥1 Completion-backed wait pending): the Scheduler
    //       worker is idle (no Fiber to run) and a backend op is in flight that
    //       it can drive. Block on ctx_.wait_one(); a backend completion
    //       (ThreadPool worker / Uring CQE / Fake staged) wakes it, the next
    //       loop top's wake_ready_completions enqueues the now-ready Fiber.
    //       This is the legitimate scheduler-idle wait — distinct from blocking
    //       while a Fiber is runnable. E4 liveness permits it (no runnable task
    //       to starve).
    //   S3 (runnable empty, only ready-flag/Future waits remain): no progress
    //       source the Scheduler can drive — return (caller re-enters; an
    //       external-thread producer would need a wake primitive = E7+).
    //
    // The wait_one() call is GATED on Completion-backed waits pending
    // (waiting_size_/waiting_void_ non-empty) to avoid the zero-outstanding
    // hazard (wait_one with nothing outstanding blocks forever).
    while (true) {
        wake_ready_completions();
        wake_ready_flags();
        if (!runnable_.empty()) {
            run_next();
            continue;
        }
        // runnable empty: S2 or S3?
        const bool completion_wait_pending =
            !waiting_size_.empty() || !waiting_void_.empty();
        if (completion_wait_pending) {
            // S2: ask the backend for progress. For real backends (ThreadPool,
            // Uring), wait_one() BLOCKS until a completion is ready — the
            // legitimate scheduler-idle wait. For Fake, wait_one() == poll()
            // and returns 0 when nothing is staged (non-blocking) — in that
            // case there is no progress the Scheduler can drive, so we break
            // and let the caller stage work and re-enter (preserving E4/E5's
            // test-driven model). Gating on the reaped count avoids a busy
            // spin on a non-blocking wait_one (the [BUSY-SPIN-RISK] from the
            // E6 audit).
            auto wr = ctx_.wait_one();
            if (!wr.has_value() || wr.value() == 0) {
                break;  // no progress; external producer must stage + re-enter
            }
            continue;  // wait_one reaped ≥1; next loop top maps it to runnable
        }
        break;  // S3 or truly idle
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
    // NOTE: do NOT early-return when poll reaps 0. A Completion may have been
    // made ready by a prior wait_one()'s internal poll (E6 T2 path: wait_one
    // drains the backend's ready queue and completes the Completion, then
    // returns; the next loop-top poll reaps 0 because the queue is already
    // drained, but the Completion is now ready and its waiting Fiber must still
    // be woken). Always scan the waiting maps.
    (void)ctx_.poll();

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

std::size_t Scheduler::wake_ready_flags() {
    // Poll registered level-triggered readiness flags. A flag that loads true
    // (acquire) wakes its single waiting Fiber (waiting -> runnable) and the
    // registration is erased (exactly-once: a later poll cannot re-enqueue).
    std::size_t woken = 0;
    for (auto it = waiting_ready_.begin(); it != waiting_ready_.end();) {
        if (it->first->load(std::memory_order::acquire)) {
            Fiber* f = it->second;
            it = waiting_ready_.erase(it);
            f->make_runnable();
            runnable_.push_back(f);
            ++woken;
        } else {
            ++it;
        }
    }
    return woken;
}

void Scheduler::await_ready_flag(const std::atomic<bool>& ready) {
    Fiber* me = current_;
    // 1. Fast path: already ready.
    if (ready.load(std::memory_order::acquire)) return;
    // 2-3. Register &ready -> current Fiber.
    waiting_ready_[&ready] = me;
    // 4-5. Re-check after registering. If ready flipped true in the window,
    // erase and return without suspending (R2).
    if (ready.load(std::memory_order::acquire)) {
        waiting_ready_.erase(&ready);
        return;
    }
    // 6. Transition running -> waiting. Set BEFORE the switch so the scheduler
    //    poll (which only accepts waiting->runnable) sees the right state if
    //    ready flips true between here and the switch (R3).
    me->make_waiting();
    // 7. Switch back to the scheduler.
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = g_sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    // 9. Resumed: the scheduler observed ready==true, erased the registration,
    //    and switched back into me->ctx. Return.
}

}  // namespace sluice::async
