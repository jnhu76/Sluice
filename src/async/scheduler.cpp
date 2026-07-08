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

// ---- E9 SchedulerWakeHandle::Control (full definition; forward-declared in
// the header so the shared_ptr member is pimpl-friendly) ----
struct SchedulerWakeHandle::Control {
    // mutex protects alive; wake delivery itself uses the Scheduler's
    // wake_mtx_. A raw Scheduler* is safe here ONLY because the Scheduler
    // clears `scheduler` and flips `alive=false` in its destructor before
    // the control block can be freed (the Scheduler holds a shared_ptr;
    // handles hold a shared_ptr, so the block outlives both). A notify()
    // that races the destructor either sees alive=true (Scheduler still
    // fully constructed) or alive=false (no-op) — never a torn Scheduler.
    std::mutex mtx;
    Scheduler* scheduler{nullptr};
    bool alive{false};
};

Scheduler::Scheduler(AsyncIoContext& ctx) noexcept : ctx_(ctx) {
    // E9: create the wake control block. Every issued SchedulerWakeHandle
    // holds a shared_ptr to it; the Scheduler holds a shared_ptr too so the
    // block outlives the Scheduler's stack locals. The handle additionally
    // keeps a weak_ptr to the Scheduler itself so notify() can detect
    // post-destruction calls and no-op.
    wake_control_ = std::make_shared<SchedulerWakeHandle::Control>();
}

Scheduler::~Scheduler() {
    // E9-CORRECTIVE (Section 11 wake-handle lifetime audit): invalidate every
    // outstanding wake handle so a post-destruction notify() becomes a no-op.
    // Both `alive=false` AND `scheduler=nullptr` are set UNDER THE SAME LOCK
    // so notify()'s consistent snapshot (taken under control_->mtx) never
    // observes alive=true with a dangling scheduler pointer. This closes the
    // during-destruction race: a concurrent notify() either sees
    // {alive=true, scheduler=valid} (Scheduler fully constructed — the
    // callback touches only wake_mtx_/wake_epoch_/wake_cv_) or
    // {alive=false} (no-op). The destructor does not proceed to destroy
    // wake_mtx_ until after this block (member destruction order), by which
    // point no live notify() can hold a valid scheduler snapshot.
    if (wake_control_) {
        {
            std::lock_guard<std::mutex> lk(wake_control_->mtx);
            wake_control_->alive = false;
            wake_control_->scheduler = nullptr;
        }
        wake_control_.reset();
    }
    // Workers are joined in run().
}

// ---- E9 SchedulerWakeHandle::notify + bound ----

bool SchedulerWakeHandle::notify() noexcept {
    if (!control_) return false;
    Scheduler* sched = nullptr;
    bool alive = false;
    {
        std::lock_guard<std::mutex> lk(control_->mtx);
        sched = control_->scheduler;
        alive = control_->alive;
    }
    if (!alive || sched == nullptr) return false;  // post-destruction: no-op
    sched->notify_external_wake();
    return true;
}

bool SchedulerWakeHandle::bound() const noexcept {
    if (!control_) return false;
    std::lock_guard<std::mutex> lk(control_->mtx);
    return control_->alive && control_->scheduler != nullptr;
}

SchedulerWakeHandle Scheduler::make_wake_handle() {
    // The control block is shared with this Scheduler; it points back here.
    wake_control_->scheduler = this;
    wake_control_->alive = true;
    return SchedulerWakeHandle{wake_control_};
}

void Scheduler::notify_external_wake() noexcept {
    // External producer entry point. Publishes a wake obligation: advance
    // the wake epoch and notify wake_cv_. Safe to call from any thread.
    // Refinement map: TLA+ ExternalReadyPublish (signal half).
    signal_wake_locked();
}

void Scheduler::signal_wake_locked() {
    // Advance the wake epoch under wake_mtx_ and notify. Idempotent +
    // coalescing-safe. The epoch is the authority for the commit-to-sleep
    // window; persistent state is the lost-wake authority (ADR §9.4.5).
    // Safe to call with global_mtx_ held (we only acquire wake_mtx_).
    {
        std::lock_guard<std::mutex> lk(wake_mtx_);
        ++wake_epoch_;
    }
    wake_cv_.notify_all();
}

void Scheduler::park_on_wake_source(WorkerState* ws) {
    // Park the calling Worker on the SCHEDULER wake domain (ADR §9.4.5).
    // Record the observed epoch under wake_mtx_, then cv.wait_for with a
    // BOUNDED timeout. The timeout is LOAD-BEARING in MIXED-WAKE: it is the
    // observation-return path for backend readiness, which does NOT directly
    // signal the Scheduler wake source in the E9 baseline (E9-LIFE-8,
    // ADR §9.4.7.1). It is NOT "defense in depth only."
    //
    // E9-CORRECTIVE (Section 10 data-race fix): the predicate no longer
    // inspects ws->local_runnable (a std::deque protected by inbox_mtx_,
    // NOT wake_mtx_). Reading it under wake_mtx_ was a data race. Runnable
    // publication (route_runnable_locked) already calls signal_wake_locked()
    // — advancing wake_epoch_ — so the epoch advance IS the wake signal for
    // runnable publication. The Worker re-drains local_runnable under
    // inbox_mtx_ at loop top after the wake; it does not need to peek at
    // the deque in the wake predicate.
    //
    // Deterministic park seams (ADR §9.4.15): seam B pauses the Worker at
    // the commit-to-physical-wait boundary. The pause is done with wake_mtx_
    // RELEASED so the test's notify() (which acquires wake_mtx_ via
    // signal_wake_locked) can proceed; the observed_epoch is recorded AFTER
    // the pause so a pre-wait publication is caught by the epoch predicate.
    //
    // Lock order: called with global_mtx_ RELEASED.
    ws->park_domain = WorkerState::ParkDomain::Scheduler;

    // E9-CORRECTIVE seam B: pause at the commit boundary with NO wake lock
    // held, so the test can publish + notify (signal_wake_locked) during
    // the pause without deadlocking.
    if (park_commit_seam_armed_) {
        std::unique_lock<std::mutex> slk(park_seam_mtx_);
        park_seam_commit_paused_ = true;
        park_seam_cv_.notify_all();  // signal the test we are at the boundary
        park_seam_cv_.wait(slk, [this] { return !park_commit_seam_armed_; });
        park_seam_commit_paused_ = false;
    }

    std::unique_lock<std::mutex> lk(wake_mtx_);
    ws->observed_epoch = wake_epoch_;  // recorded AFTER any seam publication
    wake_cv_.wait_for(lk, std::chrono::milliseconds(2), [&] {
        if (wake_epoch_ != ws->observed_epoch ||
            global_terminate_.load(std::memory_order_acquire)) {
            return true;
        }
        // E9-CORRECTIVE (Section 10): check local_runnable SAFELY. The deque
        // is protected by inbox_mtx (route_runnable_locked pushes under it);
        // reading it under wake_mtx_ alone was a data race. Acquire inbox_mtx
        // for the read. This closes the lost-wake window where a runnable
        // publication advanced the epoch BEFORE observed_epoch was recorded
        // (the parked Worker would otherwise wait for the 2ms timeout, opening
        // a steal window). route_runnable_locked also signals the epoch, so
        // the epoch clause usually fires first; this is the authoritative
        // backstop for the routed-but-epoch-already-observed case.
        std::lock_guard<std::mutex> ilk(ws->inbox_mtx);
        return !ws->local_runnable.empty();
    });
    ws->park_domain = WorkerState::ParkDomain::None;
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
        // E8: record the initial runnable owner (ADR §9.3.5.1 ownerRecord;
        // production realization of the TLA+ ownerRecord[f]).
        fiber_owner_[&fiber] = workers_[target].get();
        workers_[target]->inbox_cv.notify_one();
    } else {
        pending_spawn_.push_back(&fiber);
        // Owner is assigned at run() distribute time; record a placeholder
        // (null) here. run() will set fiber_owner_ when it distributes.
    }
}

void Scheduler::spawn_on(Fiber& fiber, unsigned worker_id) noexcept {
    // E8 test seam: place `fiber` directly on worker `worker_id`'s
    // local_runnable. No-op if the make_runnable PUBLISH fails (created->
    // runnable didn't happen). Records the owner as worker_id. Narrow
    // deterministic-test hook (see header).
    if (!fiber.make_runnable()) return;
    std::lock_guard<std::mutex> lk(global_mtx_);
    if (worker_id >= workers_.size()) {
        // Workers not created yet — fall back to pending_spawn_; the run()
        // distribute will assign round-robin (test must call spawn_on after
        // workers exist, i.e. from inside a Fiber body during a run).
        pending_spawn_.push_back(&fiber);
        return;
    }
    WorkerState* tgt = workers_[worker_id].get();
    std::lock_guard<std::mutex> wlk(tgt->inbox_mtx);
    tgt->local_runnable.push_back(&fiber);
    fiber_owner_[&fiber] = tgt;
    tgt->inbox_cv.notify_one();
}

WorkerState* Scheduler::current_worker() {
    return g_worker;
}

void Scheduler::run(unsigned worker_count) {
    // E9-CORRECTIVE: existing run() remains DRAIN-compatible (ADR §9.4.0).
    // Existing E7/E8 callers and tests use Drain: MW-S3 returns STALLED.
    run_impl(worker_count, RunMode::drain);
}

void Scheduler::run_live(unsigned worker_count) {
    // E9-CORRECTIVE: explicit LIVE entry. The run may remain resident while
    // an unresolved wait has an effective Scheduler wake source. Used by the
    // E9-T1..T14 no-re-entry external-wake proofs.
    run_impl(worker_count, RunMode::live);
}

void Scheduler::run_impl(unsigned worker_count, RunMode mode) {
    if (worker_count == 0) worker_count = 1;
    run_mode_ = mode;  // stable for the duration of this run invocation

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
            auto* tgt = workers_[w % worker_count].get();
            std::lock_guard<std::mutex> wlk(tgt->inbox_mtx);
            tgt->local_runnable.push_back(f);
            // E8: record the initial runnable owner for pre-run spawns.
            fiber_owner_[f] = tgt;
            tgt->inbox_cv.notify_one();
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

        // E8: if this worker has no local work, try to steal a runnable
        // Fiber from another worker before falling through to the idle/
        // admission path. Steal is MOVE + OWNER TRANSFER; on success a
        // runnable ticket now sits on ws->local_runnable owned by ws, so
        // loop back and pop it. try_steal is a no-op if there is only one
        // worker or no other worker has runnable work.
        if (!f && workers_.size() > 1) {
            if (try_steal(ws)) {
                continue;  // stolen ticket is on ws->local_runnable; pop next iteration
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
                // E9 MIXED-WAKE: capture the park-domain decision NOW (under
                // global_mtx_). If an external-wake-capable wait is registered,
                // the participant must NOT enter a backend-only ctx_.wait_one()
                // (external-ready cannot interrupt it). It parks on the
                // SCHEDULER wake domain instead, so external-ready can wake it.
                // Refinement map: TLA+ EnterPhysicalPark domain selection.
                ws->park_domain = external_wake_possible_locked()
                                      ? WorkerState::ParkDomain::Scheduler
                                      : WorkerState::ParkDomain::Backend;
            }

            // Phase C: release global_mtx_ (held only inside the blocks above)
            // and enter wait_one. Only the committed participant reaches here.
            if (admission_ == AdmissionState::committed && admission_owner_ == ws->id) {
                // E9 MIXED-WAKE domain split.
                if (ws->park_domain == WorkerState::ParkDomain::Scheduler) {
                    // External-wake-capable wait registered: park on the wake
                    // source (NOT backend wait_one). The wake set includes
                    // external-ready publication. A wake here means a producer
                    // signaled (external ready or routed work) — treat it as
                    // progress and re-drain.
                    ws->park_domain = WorkerState::ParkDomain::None;  // reset before park
                    park_on_wake_source(ws);
                    // Phase D: reacquire global_mtx_, clear admission, drain.
                    {
                        std::lock_guard<std::mutex> lk(global_mtx_);
                        admission_ = AdmissionState::none;
                        admission_owner_ = static_cast<unsigned>(-1);
                        (void)wake_ready_completions_locked();
                        (void)wake_ready_flags_locked();
                    }
                    // A wake on the Scheduler domain means a wake-relevant
                    // publication happened; treat as progress (re-loop). We do
                    // NOT terminate on a wake-source return (unlike the backend
                    // no-progress path), because the whole point is that
                    // external wake must be observable independent of backend
                    // timing.
                    idle_workers_.store(0, std::memory_order_release);
                    continue;
                }

                // BACKEND domain: the E7 path. Backend progress only.
                ws->park_domain = WorkerState::ParkDomain::Backend;
                auto wr = ctx_.wait_one();
                ws->park_domain = WorkerState::ParkDomain::None;
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
                    // E9: wake any Worker parked on the wake source.
                    signal_wake_locked();
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
                // E9-CORRECTIVE: SelectIdleAction at the MW-S3 boundary (ADR
                // §9.4.0). Park admission for MW-S3 + external-wake-capable is
                // governed by the EXPLICIT RunMode, NOT by wake capability
                // alone. This is the shipped defect repaired: the original E9
                // used external_wake_possible_locked() as the run-lifetime
                // decision (the semantic conflation), which made a Drain run
                // park forever on MW-S3 (the deterministic hang, e7_t9).
                //
                //   Live  + MW-S3 + external-wake-capable -> PARK (resident)
                //   Drain + MW-S3                         -> RETURN STALLED
                //   Live  + MW-S3 without external wake   -> RETURN STALLED
                if (final_state == MwState::mw_s3_unresolved &&
                    run_mode_ == RunMode::live &&
                    external_wake_possible_locked()) {
                    // Live: keep the run resident. Do NOT contribute to the
                    // idle/terminate count; fall through to park_on_wake_source.
                    // The bounded wake_cv timeout re-drains; persistent state
                    // is the authority (E9-LIFE-8).
                    idle_workers_.store(0, std::memory_order_release);
                    // Fall through to park_on_wake_source below.
                } else {
                    unsigned prev = idle_workers_.fetch_add(1, std::memory_order_acq_rel);
                    if (prev + 1 >= workers_.size()) {
                        // All workers idle. Final re-check (global_mtx_ held).
                        MwState still = classify_locked();
                        if (still == MwState::mw_s3_unresolved || still == MwState::quiescent) {
                            // Physical run termination. MW-S3 retains wait
                            // registrations logically; only quiescent is true
                            // completion. Both may terminate the run. In Drain
                            // this is RETURN STALLED (the E7/E8 contract); in
                            // Live it is reached only for MW-S3 without an
                            // effective wake source, or true quiescence.
                            global_terminate_.store(true, std::memory_order_release);
                            for (auto& w : workers_) {
                                std::lock_guard<std::mutex> wlk(w->inbox_mtx);
                                w->inbox_cv.notify_all();
                            }
                            // E9: wake any Worker parked on the wake source.
                            signal_wake_locked();
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
        }

        if (global_terminate_.load(std::memory_order_acquire)) break;

        // E9-CORRECTIVE seam A (ParkCandidate boundary, ADR §9.4.15): pause
        // the Worker right after it has decided to park (Live MW-S3 external
        // path) and before the physical wait setup. A test uses this to prove
        // a publication before ParkCandidate is drained (E9-T3). The seam
        // does NOT modify Scheduler state; it only pauses at the boundary.
        if (park_candidate_seam_armed_) {
            std::unique_lock<std::mutex> slk(park_seam_mtx_);
            park_seam_candidate_paused_ = true;
            park_seam_cv_.notify_all();
            park_seam_cv_.wait(slk, [this] { return !park_candidate_seam_armed_; });
            park_seam_candidate_paused_ = false;
        }

        // E9: park on the unified wake source (wake_cv + wake epoch). This
        // replaces the E7 1ms inbox_cv timed park, which was a de-facto
        // periodic poll masking the external-wake gap (ADR §9.4). The wake
        // source's wake set now includes runnable publication (route_runnable
        // signals) and external-ready publication (SchedulerWakeHandle).
        // The 2ms bounded timeout is LOAD-BEARING for MIXED-WAKE backend
        // observation (E9-LIFE-8); it is the observation-return path.
        park_on_wake_source(ws);
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
    // E9: signal the wake source so a Worker parked on the SCHEDULER domain
    // (park_on_wake_source) resumes. Refinement map: TLA+ PublishRunnable.
    signal_wake_locked();
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

void Scheduler::attach_ready_wake(const std::atomic<bool>& ready,
                                  SchedulerWakeHandle& wh) {
    // Validate that `ready` is currently registered as an external-wake wait.
    // The handle is bound to THIS Scheduler (make_wake_handle), so an external
    // producer's notify() already routes to signal_wake_locked. This method
    // exists to make the per-wait attachment explicit and to future-proof a
    // per-wait wake-map (E10). It is a contract assertion + a pre-emptive
    // signal: if the flag became ready between the registration and this
    // attach, signal now so a Worker about to park is woken immediately.
    bool need_signal = false;
    {
        std::lock_guard<std::mutex> lk(global_mtx_);
        auto it = waiting_ready_.find(&ready);
        if (it == waiting_ready_.end()) {
            // Not registered (already drained, or wrong caller). No-op.
            return;
        }
        // Re-check the flag under the lock: if ready, the Worker that is
        // about to park must be woken — signal the wake source.
        if (ready.load(std::memory_order::acquire)) {
            need_signal = true;
        }
    }
    // wh must be bound to this Scheduler. (Contract; not enforced here to
    // avoid coupling — a foreign handle's notify no-ops harmlessly if the
    // Scheduler differs, but that is a caller bug.)
    if (need_signal) {
        signal_wake_locked();
    }
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

bool Scheduler::try_steal(WorkerState* thief) {
    // E8 StealRunnable (ADR §9.3.4): MOVE one runnable ticket from a victim's
    // local_runnable to thief->local_runnable AND TRANSFER owner victim->thief,
    // as one atomic transition under global_mtx_. NEVER calls make_runnable
    // (the fiber is already Runnable; steal is transport, not publication).
    //
    // Victim selection (non-normative, ADR §9.3.8): round-robin over the
    // other workers, steal from the first that has runnable work. Deterministic
    // enough to test; no NUMA/priority/affinity.
    //
    // Linearization (ADR §9.3.5): the entire remove-from-victim / set-owner /
    // push-to-thief sequence happens under global_mtx_, which is the same
    // domain that reads owner/ticket state (E8-0 audit O8). No IN_TRANSIT
    // state is observable.
    if (workers_.size() <= 1) return false;  // nothing to steal from

    std::lock_guard<std::mutex> lk(global_mtx_);
    // Round-robin victim starting point keyed on thief id to spread load.
    unsigned n = static_cast<unsigned>(workers_.size());
    for (unsigned k = 1; k < n; ++k) {
        unsigned vidx = (thief->id + k) % n;
        WorkerState* victim = workers_[vidx].get();
        Fiber* stolen = nullptr;
        {
            std::lock_guard<std::mutex> vlk(victim->inbox_mtx);
            // Find a stealable ticket: a Fiber on victim->local_runnable that
            // is Runnable and currently owned by victim. local_runnable holds
            // only runnable tickets in the well-formed path, but a defensive
            // state check guards against any drift.
            for (auto it = victim->local_runnable.begin();
                 it != victim->local_runnable.end(); ++it) {
                Fiber* f = *it;
                if (f->state() != FiberState::runnable) continue;
                auto oit = fiber_owner_.find(f);
                if (oit == fiber_owner_.end() || oit->second != victim) continue;
                stolen = f;
                victim->local_runnable.erase(it);
                break;
            }
        }
        if (stolen) {
            // Transfer owner (the E8 mutation) and push to thief.
            fiber_owner_[stolen] = thief;
            {
                std::lock_guard<std::mutex> tlk(thief->inbox_mtx);
                thief->local_runnable.push_back(stolen);
            }
            thief->inbox_cv.notify_one();
            // Stealable work was MW-S1; a successful steal keeps it MW-S1
            // (ticket count unchanged — see E8-0 audit O7). No admission
            // demotion needed: route_runnable_locked's admission-cancel is
            // for NEW publications; steal moves an existing ticket.
            return true;
        }
    }
    return false;
}

}  // namespace sluice::async
