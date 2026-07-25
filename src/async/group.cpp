// Implementation of Group (sluice-CORE-029 + E5-B). See group.hpp for the model.
//
// Two execution modes:
//   - Threaded (Group()): std::thread per task; await blocks + joins.
//   - Evented  (Group(Scheduler&)): Fiber per task on the scheduler; await
//     drives sched.run_live(1) until all task Futures are terminal (E14-F1).
#include <sluice/async/group.hpp>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/evented_wait_policy.hpp>
#include <sluice/async/scheduler.hpp>

namespace sluice::async {

// E14-F1: Group-scoped invocation stop predicate for run_live. Returns true
// when all of the Group's task Futures are terminal (ready). Called under
// Scheduler global_mtx_ at the MW-S3 boundary. Acquires Group::mtx_ (no
// inversion: Group never holds mtx_ while calling into Scheduler).
bool Group::group_stop_predicate(void* ctx) {
    auto* self = static_cast<Group*>(ctx);
    std::lock_guard<std::mutex> lk(self->mtx_);
    for (auto& f : self->futures_) {
        if (!f->ready()) return false;
    }
    return true;
}

Group::Group(Scheduler& sched) : sched_(&sched) {
    // E14 D-E14-2: construction-time fail-fast on unsupported targets.
    // The Scheduler constructor already guards the earliest Evented admission
    // boundary; this is defense-in-depth for the Group-specific surface.
    // Production passes fiber_ctx::supported (compile-time true on x86_64);
    // death tests exercise the false path deterministically.
    detail::require_evented_supported(detail::evented_admission_check());
    // Create the shared EventedWaitPolicy once per group. It borrows sched_,
    // which outlives the group by contract. All task Futures in this group
    // reference *evented_policy_.
    evented_policy_ = std::make_unique<EventedWaitPolicy>(sched);
}

void Group::await() {
    if (sched_) {
        // E14-F1/D-E14-1: Evented Group-scoped Live drive. Drive the scheduler
        // in LIVE mode with an invocation stop predicate that returns true when
        // all of THIS Group's task Futures are terminal. This prevents unrelated
        // Scheduler registrations (other Groups/Fibers) from permanently blocking
        // this Group's await(). The stop predicate is checked under global_mtx_
        // at the MW-S3 boundary; it acquires mtx_ (no inversion: Group never
        // holds mtx_ while calling into Scheduler).
        //
        // External producers completing a Future trigger
        // SchedulerWakeHandle::notify() → signal_wake → the parked worker
        // resumes → wake_ready_flags_locked() → task fiber resumes → completes.
        // run_live returns when: (a) all this Group's Futures are terminal
        // (stop predicate fires), or (b) QUIESCENT, or (c) MW-S3 without
        // effective wake source.
        while (true) {
            std::size_t pending = 0;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (auto& f : futures_) {
                    if (!f->ready()) ++pending;
                }
            }
            if (pending == 0) break;
            // Group-scoped Live drive: the stop predicate returns true when
            // all this Group's Futures are terminal, allowing run_live to
            // return even if unrelated Scheduler registrations remain.
            sched_->run_live(1, &group_stop_predicate, this);
            // run_live returned: the scheduler run terminated. Loop to verify
            // all futures are ready.
        }
        // E14-F4/D-E14-3: reap completed task Futures and release Fiber/stack
        // storage. After a successful await all admitted task Futures are
        // terminal; size() becomes 0 (parity with Threaded). Repeated await
        // is idempotent (vectors already empty). Fiber/stack address stability
        // is preserved until this point (terminal completion).
        {
            std::lock_guard<std::mutex> lk(mtx_);
            bool all_terminal = true;
            for (auto& f : futures_) {
                if (!f->ready()) { all_terminal = false; break; }
            }
            if (all_terminal) {
                futures_.clear();
                evented_fibers_.clear();
                evented_stacks_.clear();
            }
        }
        return;
    }

    // Threaded path (existing).
    std::vector<std::thread> local_tasks;
    std::vector<std::shared_ptr<Future<void>>> local_futures;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        local_tasks.swap(tasks_);
        local_futures.swap(futures_);
    }
    // A task publishes its Future immediately before its worker exits. Join
    // first so every Future is already ready before await() observes it. This
    // removes the only potentially-blocking wait from the scope that owns
    // joinable std::threads: even if an underlying wait primitive could throw,
    // no joinable thread can reach std::thread's terminating destructor.
    for (auto& t : local_tasks) if (t.joinable()) t.join();
    for (auto& f : local_futures) (void)f->await();
}

Group::~Group() {
    // E14 D-E14-F2a: Evented destructor fail-fast. If any Evented task Future
    // is still pending, the caller violated the contract (must await/cancel
    // before destroying). Calling Evented Future::await from a non-Fiber
    // context (ordinary caller thread) would dereference null g_worker in
    // Scheduler::await_ready_flag. Fail fast deterministically instead.
    if (sched_) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& f : futures_) {
            if (!f->ready()) {
                detail::group_lifetime_fail_fast();  // [[noreturn]]
            }
        }
        // All Evented task Futures are terminal. Clean up without blocking.
        // No Evented Future::await is needed (results are already materialized).
        futures_.clear();
        evented_fibers_.clear();
        evented_stacks_.clear();
        return;
    }

    // Threaded: drain if await was never called (no detached threads, CP.26).
    std::vector<std::thread> local_tasks;
    std::vector<std::shared_ptr<Future<void>>> local_futures;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        local_tasks.swap(tasks_);
        local_futures.swap(futures_);
    }
    // Same exception-safety ordering as await(): do not call a potentially
    // blocking Future wait while this noexcept destructor owns joinable
    // std::threads.
    for (auto& t : local_tasks) if (t.joinable()) t.join();
    for (auto& f : local_futures) (void)f->await();
}

}  // namespace sluice::async
