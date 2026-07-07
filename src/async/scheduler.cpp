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
    // E7-T2 exactly-once: publish a runnable ticket ONLY if the created->runnable
    // transition succeeded. (spawn's source state is always 'created', so this
    // only fails if spawn is called twice — defensive.)
    if (!fiber.make_runnable()) return;
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

    // Reset each worker's saved scheduler context (sched_ctx) to a pristine
    // state before the run. A prior run() may have left sched_ctx holding a
    // resume-pointer and rsp/rbp that were valid only on that run's thread —
    // e.g. run(1) runs inline on the caller's thread, so workers_[0]->sched_ctx
    // would point into the caller's (now-unwound) stack. Each worker re-saves
    // sched_ctx on its first run_next_on() before any Fiber can switch back to
    // it, so a stale value is never read in the well-formed path; zeroing here
    // is a defense-in-depth that also makes a misuse fail loudly (jump to 0)
    // instead of silently into recycled stack memory.
    for (auto& w : workers_) {
        w->sched_ctx = fiber_ctx::Context{};
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
    admission_ = AdmissionState::none;
    admission_owner_ = static_cast<unsigned>(-1);
    // NOTE: admission_seam_* is NOT reset here — it is test-controlled state
    // that must persist across the run() boundary (T11 arms it before run()).

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
    // E7-C fixup: coordinated loop with explicit MW state classification
    // (ADR §9.2.6) and two-phase MW-S2 admission.
    //
    // Each iteration:
    //   1. Try to get a runnable Fiber (local queue → pending_spawn_).
    //   2. If got one: run it, continue (MW-S1).
    //   3. No runnable: under global_mtx_, do readiness drain (route ready
    //      Completions/flags). If any woken, continue (MW-S1).
    //   4. Under global_mtx_, classify: MW-S1 / MW-S2 / MW-S3 / QUIESCENT.
    //      MW-S2: this worker may be elected candidate; two-phase admission
    //      before entering wait_one (Phase A elect, Phase B re-drain+reclassify
    //      before commit, Phase C release global_mtx_ + wait_one, Phase D
    //      reacquire + clear + reclassify).
    //      MW-S3 / QUIESCENT: contribute to coordinated termination.
    //   5. No work and not elected: park briefly on inbox_cv.
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
            idle_workers_.store(0, std::memory_order_release);
            run_next_on(ws, f);
            continue;
        }

        // 2. Readiness drain + classify under global_mtx_.
        MwState state;
        {
            std::lock_guard<std::mutex> lk(global_mtx_);
            (void)wake_ready_completions_locked();
            (void)wake_ready_flags_locked();
            state = classify_locked();
        }

        // If drain produced routed work, the owning worker will pick it up
        // next iteration; for this worker, fall through to state handling.

        if (state == MwState::mw_s1) {
            // Runnable/running work exists somewhere (possibly routed by the
            // drain to another worker, or another worker is running a Fiber).
            // This worker has no local runnable work (f was null at top), so it
            // must NOT busy-spin — that starves other workers on a contended
            // core. Fall through to park on inbox_cv; the owning worker will
            // notify when it routes work here, or the 1ms timeout re-checks.
            idle_workers_.store(0, std::memory_order_release);
            if (global_terminate_.load(std::memory_order_acquire)) break;
            // Fall through to park (no continue).
        }

        if (state == MwState::mw_s2) {
            // MW-S2: backend progress pending, no Fiber can execute. At most
            // one participant may enter wait_one. Two-phase admission.
            //
            // Phase A: under global_mtx_, elect this worker as candidate if
            //          no admission is in progress and this is the lowest-id
            //          idle worker (deterministic election: worker 0).
            bool elected = false;
            {
                std::lock_guard<std::mutex> lk(global_mtx_);
                // Re-classify under the lock — state may have changed since
                // the unlocked classify above.
                if (classify_locked() == MwState::mw_s2 &&
                    admission_ == AdmissionState::none &&
                    ws->id == 0) {
                    admission_ = AdmissionState::candidate;
                    admission_owner_ = ws->id;
                    elected = true;
                }
            }

            if (elected) {
                // Phase B: re-drain + reclassify before committing. The
                // election is a candidate, not a commit — route_runnable_locked
                // may have demoted it in the meantime.
                // Test seam (E7-T11): pause here to let another worker route.
                if (admission_seam_armed_) {
                    std::unique_lock<std::mutex> slk(admission_seam_mtx_);
                    admission_seam_paused_ = true;
                    admission_seam_cv_.notify_all();  // signal the test we paused
                    admission_seam_cv_.wait(slk, [this] {
                        return !admission_seam_armed_;
                    });
                    admission_seam_paused_ = false;
                }

                std::lock_guard<std::mutex> lk(global_mtx_);
                // Demoted by a concurrent route? Then abandon admission.
                if (admission_ != AdmissionState::candidate ||
                    admission_owner_ != ws->id) {
                    // Another path cancelled us. Loop.
                    continue;
                }
                // Re-drain readiness + reclassify.
                (void)wake_ready_completions_locked();
                (void)wake_ready_flags_locked();
                MwState s2 = classify_locked();
                if (s2 != MwState::mw_s2) {
                    // State changed (MW-S1 via routed work, or MW-S3 via
                    // outstanding drop). Cancel candidate; do NOT enter wait_one.
                    admission_ = AdmissionState::none;
                    admission_owner_ = static_cast<unsigned>(-1);
                    continue;
                }
                // Commit: this is the single MW-S2 progress participant.
                admission_ = AdmissionState::committed;
            }

            // Phase C: release global_mtx_ (held only inside the blocks above)
            // and enter wait_one. Only the committed participant reaches here.
            if (admission_ == AdmissionState::committed && admission_owner_ == ws->id) {
                auto wr = ctx_.wait_one();
                // E6/E7 reap semantics: wait_one()==0 means the backend made
                // no progress this call. For FakeAsyncBackend this happens
                // when nothing is staged; for real backends it means "no op
                // became ready". Per ADR §9.2.6 / E6, this is the legitimate
                // "no Scheduler-driven progress" boundary: the coordinated
                // run terminates (like MW-S3), preserving E4/E5 caller-driven
                // semantics. A reap>0 yields ready Completions which the next
                // loop-top drain will route.
                bool made_progress = wr.has_value() && wr.value() > 0;

                // Phase D: reacquire global_mtx_, clear admission, drain.
                {
                    std::lock_guard<std::mutex> lk(global_mtx_);
                    admission_ = AdmissionState::none;
                    admission_owner_ = static_cast<unsigned>(-1);
                    (void)wake_ready_completions_locked();
                    (void)wake_ready_flags_locked();
                }

                if (!made_progress) {
                    // No backend progress: terminate this coordinated run. The
                    // run may be re-entered by the caller after staging work
                    // (E4/E5 model). MW-S2 with outstanding-but-uncompletable
                    // ops is treated as a no-progress boundary, NOT busy-spin.
                    global_terminate_.store(true, std::memory_order_release);
                    for (auto& w : workers_) {
                        std::lock_guard<std::mutex> wlk(w->inbox_mtx);
                        w->inbox_cv.notify_all();
                    }
                    {
                        std::lock_guard<std::mutex> slk(admission_seam_mtx_);
                        admission_seam_armed_ = false;
                        admission_seam_cv_.notify_all();
                    }
                    break;
                }
                idle_workers_.store(0, std::memory_order_release);
                continue;
            }

            // Not elected and not committed: another worker is the candidate/
            // committed participant. Fall through to idle parking.
        }

        // state is MW-S3 or QUIESCENT (or MW-S2 non-participant): contribute
        // to coordinated termination. The last worker to go idle does a FINAL
        // re-check under global_mtx_ before setting global_terminate_.
        {
            std::lock_guard<std::mutex> lk(global_mtx_);
            // Cancel any stale admission if we're terminating — wait_one is
            // undefined past run end. (admission_ should already be none here
            // for non-elected workers.)
            MwState final_state = classify_locked();
            // If real work appeared (MW-S1) or backend still outstanding (S2),
            // do not terminate.
            if (final_state == MwState::mw_s1 || final_state == MwState::mw_s2) {
                idle_workers_.store(0, std::memory_order_release);
            } else {
                unsigned prev = idle_workers_.fetch_add(1, std::memory_order_acq_rel);
                if (prev + 1 >= workers_.size()) {
                    // All workers idle. Final re-check (global_mtx_ held).
                    MwState still = classify_locked();
                    if (still == MwState::mw_s3_unresolved || still == MwState::quiescent) {
                        // Physical run termination. MW-S3 retains wait
                        // registrations logically; only quiescent is true
                        // completion. Both may terminate the run.
                        global_terminate_.store(true, std::memory_order_release);
                        for (auto& w : workers_) {
                            std::lock_guard<std::mutex> wlk(w->inbox_mtx);
                            w->inbox_cv.notify_all();
                        }
                        // Release any admission-seam wait so parked test
                        // workers can observe termination.
                        {
                            std::lock_guard<std::mutex> slk(admission_seam_mtx_);
                            admission_seam_armed_ = false;
                            admission_seam_cv_.notify_all();
                        }
                        break;
                    }
                    idle_workers_.store(0, std::memory_order_release);
                }
            }
        }

        if (global_terminate_.load(std::memory_order_acquire)) break;

        // Park briefly waiting for routed work or termination.
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
            // E7-T2 exactly-once: only publish a ticket if the fiber actually
            // transitioned waiting->runnable. If the fiber was already runnable
            // (e.g. a concurrent wake raced), do NOT enqueue a second ticket.
            if (f->make_runnable()) {
                route_runnable_locked(f, owner);
                woken = true;
            }
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
            if (f->make_runnable()) {  // E7-T2 exactly-once
                route_runnable_locked(f, owner);
                woken = true;
            }
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
            if (f->make_runnable()) {  // E7-T2 exactly-once
                route_runnable_locked(f, owner);
                woken = true;
            }
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
    // E7-C fixup: new runnable work cancels any MW-S2 admission candidate/
    // committed. A committed participant in wait_one cannot be interrupted
    // (it has released global_mtx_), but a CANDIDATE that has not yet
    // committed will observe admission_ != candidate on Phase-B re-check and
    // abandon. route_runnable_locked demotes candidate→none so the candidate
    // does not commit.
    if (admission_ == AdmissionState::candidate) {
        admission_ = AdmissionState::none;
        admission_owner_ = static_cast<unsigned>(-1);
    }
    if (owner) {
        std::lock_guard<std::mutex> lk(owner->inbox_mtx);
        owner->local_runnable.push_back(f);
        owner->inbox_cv.notify_one();
    } else {
        pending_spawn_.push_back(f);
    }
}

Scheduler::MwState Scheduler::classify_locked() const {
    // Must be called with global_mtx_ held.
    bool any_runnable = !pending_spawn_.empty();
    std::size_t per_worker_runnable = 0;
    if (!any_runnable) {
        for (auto& w : workers_) {
            std::lock_guard<std::mutex> wlk(w->inbox_mtx);
            per_worker_runnable += w->local_runnable.size();
            if (!w->local_runnable.empty()) { any_runnable = true; break; }
        }
    }
    const bool any_running =
        running_fiber_count_.load(std::memory_order_acquire) > 0;
    if (any_runnable || any_running) return MwState::mw_s1;

    // No executable Fiber. Backend outstanding count is the source of truth
    // for MW-S2 vs MW-S3. ctx_.outstanding() acquires access_mtx_ internally;
    // global_mtx_→access_mtx_ is the accepted lock order.
    const bool any_outstanding = ctx_.outstanding() > 0;
    if (any_outstanding) return MwState::mw_s2;

    const bool any_wait =
        !waiting_size_.empty() || !waiting_void_.empty() || !waiting_ready_.empty();
    if (any_wait) return MwState::mw_s3_unresolved;

    return MwState::quiescent;
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
