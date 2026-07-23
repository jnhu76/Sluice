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

#include <sluice/async/select.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "queue_detail.hpp"  // QueueWaitCtx (shared with queue_port.cpp; non-installed)

#include <utility>

// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the internal-testing variant pulls in
// the non-installed test-control header so the phase call sites below resolve to
// the controller. In the production build this include is absent and the call
// sites compile to nothing.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

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
    // E9-LIFETIME-CORRECTIVE: Control::mtx is the CALLBACK LEASE.
    //
    // notify() holds this mutex from the validity check through the ENTIRE
    // Scheduler wake callback (notify_external_wake -> signal_wake_locked).
    // ~Scheduler acquires the SAME mutex before invalidating the control
    // block. Therefore destruction cannot invalidate or destroy Scheduler
    // wake members while a notify callback holding the lease is in flight:
    //   - Notify wins:  N holds the lease through the callback; D's mutex
    //                   acquisition BLOCKS until the callback returns.
    //   - D wins:       D invalidates + releases; N then observes dead/null
    //                   and returns false without any Scheduler dereference.
    //
    // This is a mutex-serialized callback lease, NOT shared ownership and
    // NOT reference counting. Control::mtx does not extend Scheduler object
    // ownership. A stale handle may survive Scheduler destruction; its later
    // notify() observes alive=false and returns false (a safe no-op). See
    // docs/spec/e9_wake_handle_lifetime/ for the TLA+ proof.
    Mutex mtx;
    Scheduler* scheduler SLUICE_GUARDED_BY(mtx){nullptr};
    bool alive SLUICE_GUARDED_BY(mtx){false};

    // E9-LIFETIME-CORRECTIVE deterministic test seam (spec 13). TEST-ONLY.
    // When armed, notify() pauses at the exact causal boundary - AFTER it
    // has validated alive under Control::mtx and BEFORE notify_external_wake
    // - while STILL HOLDING the lease. This forces the notifier-wins
    // interleaving deterministically: the destructor cannot complete
    // invalidation while the notifier is paused. It does NOT alter
    // production Scheduler state; it only blocks the notifier thread.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    bool lifetime_seam_armed{false};
    bool lifetime_seam_paused{false};
    std::mutex lifetime_seam_mtx;
    std::condition_variable lifetime_seam_cv;
#endif
};

Scheduler::Scheduler(AsyncIoContext& ctx) noexcept : ctx_(ctx) {
    // E9: create the wake control block. Every issued SchedulerWakeHandle
    // holds a shared_ptr to it; the Scheduler holds a shared_ptr too so the
    // block outlives the Scheduler's stack locals. A handle's notify() detects
    // post-destruction via the Control::alive flag (protected by Control::mtx,
    // the callback lease) and returns false without any Scheduler dereference.
    // See Control below and docs/spec/e9_wake_handle_lifetime/.
    wake_control_ = std::make_shared<SchedulerWakeHandle::Control>();
}

Scheduler::~Scheduler() {
    // E9-LIFETIME-CORRECTIVE: invalidate the control block under Control::mtx.
    //
    // Control::mtx is held from the validity check through the Scheduler
    // wake callback in notify() (the callback lease). Scheduler destruction
    // acquires the same mutex here before invalidating the control block.
    // Therefore destruction cannot invalidate or destroy Scheduler wake
    // members while a notify callback holding the lease is in flight:
    //   - Notify wins:  the callback runs to completion and releases the
    //                   lease; only then does this acquire proceed.
    //   - Destructor wins: this invalidates (alive=false, scheduler=nullptr)
    //                   and releases; any later notify observes dead/null
    //                   and returns false without a Scheduler dereference.
    //
    // This serializes the callback-duration lease against invalidation. It
    // is NOT shared ownership: a stale SchedulerWakeHandle may outlive the
    // Scheduler, but its later notify() is a safe no-op. After this block,
    // wake_control_.reset() drops the Scheduler's reference; the Control
    // block lives as long as any outstanding handle holds a shared_ptr to
    // it, but `alive` is now permanently false.
    if (wake_control_) {
        {
            LockGuard lk(wake_control_->mtx);
            wake_control_->alive = false;
            wake_control_->scheduler = nullptr;
        }
        wake_control_.reset();
    }
    // Workers are joined in run().
    //
    // E13 P3 Corrective (destruction contract): at destruction the Scheduler
    // must hold NO live Select timer AUTHORITY — no ACTIVE SelectTimerRegist-
    // ration may remain, and the shared active-deadline counter must be zero.
    // Terminal (RETIRED/CONSUMED) lazy blocks whose deadlines never elapsed are
    // PERMITTED here: lazy-at-deadline reclamation may leave such inert blocks
    // in the pool, and their callback authority was already closed via the
    // Scheduler accounting helper (which decremented active_deadline_count_).
    // The pool/heap members then destruct normally and free the inert blocks.
    //
    // This mirrors the ordinary timer_pool_ teardown contract, which imposes
    // no pool-empty assertion: a non-empty physical pool is legal as long as no
    // logical authority remains. The previous shape wrongly asserted
    // select_timer_pool_.empty(), rejecting the legal lazy-teardown state where
    // a Select with an Event arm + a far-future Timer arm resolved via the
    // Event, leaving a RETIRED Timer block whose deadline had not elapsed.
    //
    // active_deadline_count_ == 0 is the logical-authority count: it is
    // decremented exactly once per ACTIVE->terminal transition by the
    // retire/consume helper, so a terminal lazy block contributes 0 and the
    // assertion is consistent with permitting lazy blocks. Debug-only asserts;
    // absent in release (NDEBUG).
    bool any_active_select = false;
    for (auto& r : select_timer_pool_) {
        if (r.is_active()) { any_active_select = true; break; }
    }
    assert(!any_active_select &&
           "~Scheduler: an ACTIVE SelectTimerRegistration remains (live Select "
           "timer authority not closed — caller contract violation)");
    assert(active_deadline_count_ == 0 &&
           "~Scheduler: active_deadline_count_ != 0 (a timer registration was "
           "not retired/consumed before teardown)");
}

// ---- E9 SchedulerWakeHandle::notify + bound ----

bool SchedulerWakeHandle::notify() noexcept {
    if (!control_) return false;
    // E9-LIFETIME-CORRECTIVE: hold Control::mtx (the callback lease) from
    // the validity check THROUGH the Scheduler wake callback. The
    // destructor acquires this same mutex before invalidating, so it
    // BLOCKS while a callback is in flight; invalidation + Scheduler
    // member destruction happen strictly after any validated callback
    // returns. This closes the snapshot-before-callback UAF window where
    // a previously-released lease let the destructor destroy members
    // between snapshot and callback. See docs/spec/e9_wake_handle_lifetime/.
    LockGuard lk(control_->mtx);
    if (!control_->alive || control_->scheduler == nullptr) {
        return false;  // post-destruction / unbound: no-op
    }
    // E9-LIFETIME-CORRECTIVE deterministic seam (spec 13): pause at the
    // exact boundary - validated + lease held, just before the callback.
    // Lets T1 prove the destructor cannot progress while the notifier
    // owns the lease. The seam blocks on its OWN mtx/cv; Control::mtx
    // remains held for the duration, which is precisely the guarantee
    // under test.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (control_->lifetime_seam_armed) {
        std::unique_lock<std::mutex> slk(control_->lifetime_seam_mtx);
        control_->lifetime_seam_paused = true;
        control_->lifetime_seam_cv.notify_all();
        control_->lifetime_seam_cv.wait(slk,
                                        [this] { return !control_->lifetime_seam_armed; });
        control_->lifetime_seam_paused = false;
        // Re-validate: the test's release may have let the destructor run.
        if (!control_->alive || control_->scheduler == nullptr) {
            return false;
        }
    }
#endif
    control_->scheduler->notify_external_wake();
    return true;
}

bool SchedulerWakeHandle::bound() const noexcept {
    if (!control_) return false;
    LockGuard lk(control_->mtx);
    return control_->alive && control_->scheduler != nullptr;
}

// E9-LIFETIME-CORRECTIVE deterministic test seam (spec 13). TEST-ONLY.
// Defined here because Control is complete only in this TU. These wrap the
// Control seam state; they do NOT touch any Scheduler state.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
void SchedulerWakeHandle::lifetime_seam_arm() noexcept {
    if (!control_) return;
    std::lock_guard<std::mutex> lk(control_->lifetime_seam_mtx);
    control_->lifetime_seam_armed = true;
}

void SchedulerWakeHandle::lifetime_seam_wait_paused() noexcept {
    if (!control_) return;
    std::unique_lock<std::mutex> lk(control_->lifetime_seam_mtx);
    control_->lifetime_seam_cv.wait(lk, [this] { return control_->lifetime_seam_paused; });
}

bool SchedulerWakeHandle::lifetime_seam_is_paused() const noexcept {
    if (!control_) return false;
    std::lock_guard<std::mutex> lk(control_->lifetime_seam_mtx);
    return control_->lifetime_seam_paused;
}

void SchedulerWakeHandle::lifetime_seam_release() noexcept {
    if (!control_) return;
    std::lock_guard<std::mutex> lk(control_->lifetime_seam_mtx);
    control_->lifetime_seam_armed = false;
    control_->lifetime_seam_cv.notify_all();
}
#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)

SchedulerWakeHandle Scheduler::make_wake_handle() {
    // The control block is shared with this Scheduler; it points back here.
    // Mutate scheduler/alive under Control::mtx, matching ~Scheduler and every
    // reader (notify/bound). A concurrent notify() on a previously-issued
    // handle reads these under Control::mtx; an unlocked write here would race
    // it (and TSan flags it).
    {
        LockGuard lk(wake_control_->mtx);
        wake_control_->scheduler = this;
        wake_control_->alive = true;
    }
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
        LockGuard lk(wake_mtx_);
        ++wake_epoch_;
    }
    wake_cv_.notify_all();
}

// TSA-SUPPRESS-001: park_on_wake_source uses std::unique_lock + cv.wait
// (release-wait-reacquire pattern).  Clang TSA cannot track unique_lock
// capability semantics through condition_variable::wait.  The production
// lock fact (wake_epoch_ is protected by wake_mtx_) is already independently
// accepted and proven in E9.  Suppression is attached to the smallest exact
// function.
void Scheduler::park_on_wake_source(WorkerState* ws) SLUICE_NO_THREAD_SAFETY_ANALYSIS {
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
    // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: controller-driven (test variant).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::e9_park_commit);
#endif

    std::unique_lock<Mutex> lk(wake_mtx_);
    ws->observed_epoch = wake_epoch_;  // recorded AFTER any seam publication
    // E11 I6 (Deadline Park Liveness): bound the wake wait by the earliest
    // active deadline so an active deadline cannot park a Worker indefinitely
    // past it. Read the deadline from the LOCK-FREE atomic cache
    // (earliest_active_deadline_) — NOT under global_mtx_ — to avoid a
    // wake_mtx_->global_mtx_ lock-order inversion (signal_wake_locked takes them
    // in the opposite order). A stale cache read is safe: it only changes the
    // park timeout slightly; the worker loop's pump_deadlines_locked
    // re-establishes the authoritative deadline set under global_mtx_ on every
    // iteration, so liveness (I6) holds even if the cache briefly lags. If the
    // deadline is already due, use a near-zero timeout so the loop re-drains +
    // pumps the timer promptly. Production default is the E9 2ms backstop.
    static constexpr auto kParkBackstop = std::chrono::milliseconds(2);
    static constexpr auto kTestParkPoll = std::chrono::milliseconds(1);
    auto wake_deadline = std::chrono::steady_clock::now() + kParkBackstop;
    deadline_t earliest = earliest_active_deadline_.load(std::memory_order::acquire);
    if (earliest != kNoDeadline) {
        deadline_t now_ticks = clock_now_unlocked();
        if (earliest <= now_ticks) {
            wake_deadline = std::chrono::steady_clock::now();
        } else {
            deadline_t remaining = earliest - now_ticks;
            if (test_clock_mode_.load(std::memory_order::acquire)) {
                // Test mode: the clock is logical; cap at a short poll so
                // advance_clock()'s pump drives expiry deterministically.
                wake_deadline = std::chrono::steady_clock::now() + kTestParkPoll;
            } else {
                // Production: compute the actual remaining time to the earliest
                // deadline, capped at the E9 2ms backstop. Avoids a fixed 1ms
                // poll that would cause ~1000 wakeups/s per active deadline.
                auto delay = std::min(std::chrono::milliseconds(remaining),
                                      kParkBackstop);
                wake_deadline = std::chrono::steady_clock::now() + delay;
            }
        }
    }
    wake_cv_.wait_until(lk, wake_deadline, [&]() SLUICE_NO_THREAD_SAFETY_ANALYSIS {
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
    LockGuard lk(global_mtx_);
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
    LockGuard lk(global_mtx_);
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
        LockGuard lk(global_mtx_);
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
        next_spawn_worker_ = 0;
        admission_ = AdmissionState::none;
        admission_owner_ = static_cast<unsigned>(-1);
    }
    in_coordinated_run_ = true;
    active_worker_count_.store(worker_count, std::memory_order_release);
    running_fiber_count_.store(0, std::memory_order_release);
    idle_workers_.store(0, std::memory_order_release);
    global_terminate_.store(false, std::memory_order_release);
    // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the test-controlled causal seam
    // state (E7 admission, E9 park, E12 event) no longer lives on Scheduler; it
    // is driven by the internal-testing controller and persists across the
    // run() boundary by construction (the controller registry is external).

    if (worker_count == 1) {
        // Single-worker fast path: run inline (no thread spawn). This preserves
        // the E4-E6 behavior exactly — run_until_idle on the caller's thread.
        g_worker = workers_[0].get();
        workers_[0]->owner_scheduler = this;  // E13 P5 caller-validation identity
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
                workers_[i]->owner_scheduler = this;  // E13 P5 caller-validation identity
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
            LockGuard lk(global_mtx_);
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
        // E11: pump due timers here so an expired deadline resolves a
        // deadline-waiting fiber the same way a ready flag resolves a
        // flag-waiting fiber (same canonical route). In production the clock
        // runs on steady_clock; in test mode advance_clock() advances it. The
        // pump is inert when no deadline is due/active.
        MwState state;
        {
            LockGuard lk(global_mtx_);
            (void)wake_ready_completions_locked();
            (void)wake_ready_flags_locked();
            (void)pump_deadlines_locked();
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
                LockGuard lk(global_mtx_);
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
                // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the seam state no longer
                // lives on Scheduler; the internal-testing variant calls a phase
                // function that looks up controller state by Scheduler*.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                sluice_async_test::test_phase(*this,
                    sluice_async_test::PhaseTag::e7_admission_phase_b);
#endif

                bool phase_b_committed = false;
                {
                    LockGuard lk(global_mtx_);
                    // Demoted by a concurrent route? Then abandon admission.
                    if (admission_ != AdmissionState::candidate ||
                        admission_owner_ != ws->id) {
                        // Another path cancelled us. Loop.
                        continue;
                    }
                    // Re-drain readiness + reclassify.
                    (void)wake_ready_completions_locked();
                    (void)wake_ready_flags_locked();
                    (void)pump_deadlines_locked();
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
                    phase_b_committed = true;
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
                // phase_b_committed was captured under the lock; reading it
                // outside the lock is safe because only the committed worker can
                // change admission_ from committed (the state machine invariant).
                if (phase_b_committed) {
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
                        LockGuard lk(global_mtx_);
                        admission_ = AdmissionState::none;
                        admission_owner_ = static_cast<unsigned>(-1);
                        (void)wake_ready_completions_locked();
                        (void)wake_ready_flags_locked();
                        (void)pump_deadlines_locked();
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
                    LockGuard lk(global_mtx_);
                    admission_ = AdmissionState::none;
                    admission_owner_ = static_cast<unsigned>(-1);
                    (void)wake_ready_completions_locked();
                    (void)wake_ready_flags_locked();
                    (void)pump_deadlines_locked();
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
                    // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: release any paused
                    // admission seam via the controller (test variant only).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                    sluice_async_test::release_all_phases(*this);
#endif
                    break;
                }
                idle_workers_.store(0, std::memory_order_release);
                continue;
            }
        }  // if (elected)

            // Not elected and not committed: another worker is the candidate/
            // committed participant. Fall through to idle parking.
        }

        // state is MW-S3 or QUIESCENT (or MW-S2 non-participant): contribute
        // to coordinated termination. The last worker to go idle does a FINAL
        // re-check under global_mtx_ before setting global_terminate_.
        {
            LockGuard lk(global_mtx_);
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
                            // Release any paused test phase so parked test
                            // workers can observe termination.
                            // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: via the
                            // controller (test variant only).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                            sluice_async_test::release_all_phases(*this);
#endif
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
        // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: controller-driven (test variant).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(*this,
            sluice_async_test::PhaseTag::e9_park_candidate);
#endif

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
    (void)per_worker_runnable;  // accumulated for diagnostics; silence unused warning
    const bool any_running =
        running_fiber_count_.load(std::memory_order_acquire) > 0;
    if (any_runnable || any_running) return MwState::mw_s1;

    // No executable Fiber. Backend outstanding count is the source of truth
    // for MW-S2 vs MW-S3. ctx_.outstanding() acquires access_mtx_ internally;
    // global_mtx_→access_mtx_ is the accepted lock order.
    const bool any_outstanding = ctx_.outstanding() > 0;
    if (any_outstanding) return MwState::mw_s2;

    const bool any_wait =
        !waiting_size_.empty() || !waiting_void_.empty() || !waiting_ready_.empty() ||
        waiting_waitq_count_ > 0 || waiting_select_count_ > 0;
    if (any_wait) return MwState::mw_s3_unresolved;

    return MwState::quiescent;
}

void Scheduler::await_completion_size(Completion<std::size_t>& c) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    // E7/E8 SuspendFiber refinement obligation: register + readiness recheck +
    // make_waiting MUST be one atomic transition with respect to the wake path
    // (wake_ready_completions_locked runs under global_mtx_). Doing make_waiting
    // before registering (the old shape) left a window in which the wake path
    // could not see this fiber; doing the recheck outside the lock could miss a
    // wake that landed between register-release and recheck. Both admitted a
    // lost wake / permanent park. Mirror the attach_ready_wake idiom: register,
    // recheck, and make_waiting all under global_mtx_; only context_switch is
    // outside. If the Completion is already ready under the lock, undo the
    // speculative registration and continue running (no make_waiting).
    {
        LockGuard lk(global_mtx_);
        waiting_size_[static_cast<void*>(&c)] = {me, ws};
        if (c.ready()) {
            waiting_size_.erase(static_cast<void*>(&c));
            return;
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

void Scheduler::await_completion_void(Completion<void>& c) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    // See await_completion_size: register + recheck + make_waiting under the
    // wake-path lock; only context_switch is outside.
    {
        LockGuard lk(global_mtx_);
        waiting_void_[static_cast<void*>(&c)] = {me, ws};
        if (c.ready()) {
            waiting_void_.erase(static_cast<void*>(&c));
            return;
        }
        me->make_waiting();
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
    // See await_completion_size: register + recheck + make_waiting under
    // global_mtx_ (the wake_flags lock), only context_switch outside. The old
    // shape did the recheck and make_waiting outside the lock, racing a wake
    // that landed between register-release and recheck (lost wake / park).
    {
        LockGuard lk(global_mtx_);
        waiting_ready_[&ready] = {me, ws};
        if (ready.load(std::memory_order::acquire)) {
            waiting_ready_.erase(&ready);
            return;
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

void Scheduler::await_wait(WaitQueue& q, WaitNode& node) {
    // E10 WaitQueue suspension seam. Mirrors await_ready_flag's lost-wake-closed
    // idiom (commit 422036c): register + recheck + make_waiting are ONE atomic
    // transition w.r.t. the wake path (wake_wait_one / cancel_wait run under
    // global_mtx_); only context_switch is outside. The queue protocol itself
    // creates no wake-before-suspend loss — the register_ CAS (under q.mtx_,
    // taken inside global_mtx_) publishes membership before make_waiting.
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    // The fiber handle is recorded on the node so the winner resolver can route
    // the resumed fiber without a per-node scheduler map.
    {
        // Register into the queue AND record the unresolved-wait count under one
        // critical section. q.mtx() is the structural authority (§9); global_mtx_
        // is the scheduler coordination domain. Lock order: global_mtx_ then
        // q.mtx() (consistent with global_mtx_->inbox_mtx in route_runnable).
        LockGuard lk(global_mtx_);
        LockGuard qlk(q.mtx());
        if (!q.register_wait_locked(node, me)) {
            // Node was already registered or terminal (C8): a contract violation.
            // Do not suspend; return to the caller with the node untouched.
            return;
        }
        ++waiting_waitq_count_;
        // Recheck: if the node was already resolved concurrently (it cannot be,
        // since register_wait_locked just moved it to Registered under both
        // locks and every resolver takes global_mtx_), undo and do not suspend.
        // This is defense-in-depth mirroring await_ready_flag's recheck.
        if (node.is_terminal()) {
            q.unlink_locked(node);
            --waiting_waitq_count_;
            return;
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

WaitNode* Scheduler::wake_wait_one_locked(WaitQueue& q) {
    // E12-A: the wake_wait_one body with global_mtx_ already held. Resolves the
    // FIFO head with Woken (wake_one_locked), retires any bound timer
    // (retire_timer_for_node_locked, E11 I4), decrements waiting_waitq_count_,
    // and routes the winner runnable through the canonical wake seam. Returns
    // the winning node (nullptr if empty or head lost to a concurrent resolver).
    //
    // The caller MUST hold global_mtx_. q.mtx() is taken here (under global_mtx_,
    // consistent lock order). Used by the public wake_wait_one AND
    // event_set_broadcast's drain loop so the drain is atomic w.r.t. reset and
    // admission (all under global_mtx_).
    //
    // E11 Phase 5 (Timer Lifetime Closure): a RESOURCE_WAKE winner MUST retire
    // any active timer registration bound to the resolved node, in the SAME
    // global_mtx_ critical section as the resolve CAS and BEFORE runnable
    // publication (I4).
    LockGuard qlk(q.mtx());
    WaitNode* won = q.wake_one_locked();
    if (won == nullptr) return nullptr;  // empty, or head lost to a cancel
    retire_timer_for_node_locked(*won);
    Fiber* f = won->fiber();
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // E7-T2 exactly-once: publish a runnable ticket ONLY if waiting->runnable
    // succeeded. The node is terminal; make_runnable is the publication guard.
    if (f != nullptr && f->make_runnable()) {
        route_runnable_locked(f, g_worker);
    }
    return won;
}

bool Scheduler::wake_wait_one(WaitQueue& q) {
    // Resolve the FIFO head of `q` with Woken and route the winner's fiber
    // through the canonical wake seam (route_runnable_locked). The winner is
    // the unique resolver (§2/§7): the resolve CAS under q.mtx_ is the
    // authority, and unlink happens in the same critical section. The loser
    // (e.g. a concurrent cancel of the head) returns null and this is a no-op.
    //
    // make_runnable + route_runnable_locked run under global_mtx_ (the wake
    // path's coordination domain), exactly like wake_ready_flags_locked. This
    // is the single canonical runnable-enqueue seam (§8).
    LockGuard lk(global_mtx_);
    return wake_wait_one_locked(q) != nullptr;
}

bool Scheduler::cancel_wait(WaitQueue& q, WaitNode& node) {
    // Resolve `node` with Cancelled and route the winner's fiber. E10 cancel is
    // wait-cancellation ONLY (not task/fiber/I/O cancellation). The winner is
    // determined by the same resolve CAS authority as wake (§2/§7): a losing
    // cancel (node already Woken) returns false and does nothing.
    //
    // E11 Phase 5 (Timer Lifetime Closure): as in wake_wait_one, a CANCEL
    // winner MUST retire the bound active timer registration before runnable
    // publication (I4).
    LockGuard lk(global_mtx_);
    LockGuard qlk(q.mtx());
    if (!q.cancel_locked(node)) return false;  // already terminal (loser)
    retire_timer_for_node_locked(node);
    Fiber* f = node.fiber();
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    if (f != nullptr && f->make_runnable()) {
        route_runnable_locked(f, g_worker);
        return true;
    }
    return false;
}

// ---- E11 timer / deadline subsystem (sluice-CORE-E11) ----

Scheduler::deadline_t Scheduler::monotonic_now() const noexcept {
    // Production clock: steady_clock ticks since process start. Rebased to a
    // small origin so deadline_t values stay manageable. Test mode returns the
    // logical clock_ (advanced deterministically by advance_clock()).
    // Lock-free: clock_ + test_clock_mode_ are atomics (no GUARDED_BY), read
    // here exactly as in clock_now_unlocked().
    if (test_clock_mode_.load(std::memory_order::acquire)) {
        return clock_.load(std::memory_order::acquire);
    }
    auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch);
    return static_cast<deadline_t>(ms.count());
}

Scheduler::deadline_t Scheduler::clock_now_unlocked() const noexcept {
    // Lock-free read for the park-timeout computation. In production reads
    // steady_clock; in test mode reads the atomic clock_.
    if (test_clock_mode_) {
        return clock_.load(std::memory_order::acquire);
    }
    auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch);
    return static_cast<deadline_t>(ms.count());
}

void Scheduler::advance_clock(deadline_t t) {
    // TEST-ONLY deterministic timer driver (M7). Advance the logical clock and
    // pump any now-due timers through expire_wait. Requires test-clock mode
    // (set by E11TimerTestHooks::enable_test_clock). In production this is a
    // no-op: real time advances on its own and the worker loop's pump drives
    // expiry. NEVER used as causal proof with sleep_for.
    {
        LockGuard lk(global_mtx_);
        if (!test_clock_mode_) return;  // production mode: no-op
        deadline_t cur = clock_.load(std::memory_order::acquire);
        if (t > cur) clock_.store(t, std::memory_order::release);
        // Pump due timers under the lock (expire_wait re-acquires the per-queue
        // mtx inside this global_mtx_ CS — lock order preserved).
        (void)pump_deadlines_locked();
    }
    // A timer expiry published a runnable: signal the wake source so a parked
    // worker (if any) re-loops.
    signal_wake_locked();
}

void Scheduler::await_wait_deadline(WaitQueue& q, WaitNode& node, deadline_t deadline) {
    // E11 deadline wait admission. Mirrors await_wait's lost-wake-closed idiom
    // (commit 422036c) and extends it with: (1) a TimerRegistration control
    // block bound to this wait epoch, and (2) an already-due-deadline recheck
    // that resolves Expired immediately through the SAME resolve_ authority
    // (I5 admission closure — the fiber is never stranded by a due deadline).
    //
    // The admission critical section establishes, atomically w.r.t. every
    // resolver (wake_wait_one / cancel_wait / expire_wait / pump_deadlines all
    // run under global_mtx_):
    //   1. register node into q               (Detached -> Registered)
    //   2. ++waiting_waitq_count_             (MW-S3 accounting)
    //   3. create TimerRegistration R_E       (ACTIVE, bound {node,q,deadline})
    //   4. push R_E into the deadline heap
    //   5. recheck: if node already terminal -> undo + return (defense-in-depth)
    //   6. recheck: if deadline already due  -> resolve Expired + return (I5)
    //   7. make_waiting()
    // Only context_switch is outside the lock.
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(q.mtx());
        if (!q.register_wait_locked(node, me)) {
            // Node already registered or terminal (C8): contract violation.
            return;
        }
        ++waiting_waitq_count_;
        // Create the timer registration control block for this wait epoch.
        // It lives in timer_pool_ (pointer-stable std::list) so its identity
        // survives heap operations and it outlives the caller-owned WaitNode.
        timer_pool_.emplace_back(&node, &q, deadline);
        reg = &timer_pool_.back();
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();  // publish to the park-timeout cache

        // I5 admission closure: if the deadline is ALREADY due, resolve Expired
        // through the same resolve_ authority NOW — the fiber must NOT suspend
        // and wait for a future timer scan merely because registration happened
        // after the deadline was due. expire_locked is the winner CAS; on win,
        // perform the winner path (unlink already done by expire_locked,
        // retire the registration, dec count) and return WITHOUT suspending.
        if (clock_now_unlocked() >= deadline) {
            if (q.expire_locked(node)) {
                reg->try_claim_expiry();  // ACTIVE->CONSUMED (winner)
                --active_deadline_count_;
                recompute_earliest_deadline_locked();  // reg no longer Active
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                if (me != nullptr && me->make_runnable()) {
                    route_runnable_locked(me, g_worker);
                }
                // The consumed block stays in the heap/pool for lazy removal:
                // its deadline is already due, so the next pump_deadlines_locked
                // (worker-loop drain, or the test driver's advance_clock) pops
                // + erases it. No UAF: the block is not dereferenced while inert.
                return;  // resolved at admission; do NOT suspend
            }
            // If expire_locked lost, a concurrent resolver won; fall through to
            // the terminal recheck (the node is terminal -> undo + return).
        }

        // Recheck: if the node was resolved concurrently (it cannot be, since
        // register_wait_locked just moved it to Registered under both locks and
        // every resolver takes global_mtx_), undo and do not suspend.
        if (node.is_terminal()) {
            q.unlink_locked(node);
            --waiting_waitq_count_;
            if (reg->retire()) {  // ACTIVE->RETIRED (closes callback authority)
                --active_deadline_count_;
            }
            recompute_earliest_deadline_locked();
            return;
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

bool Scheduler::expire_wait(WaitQueue& q, WaitNode& node) {
    // The E11 third resolution cause: resolve `node` with Expired and route the
    // winner's fiber. Mirrors wake_wait_one / cancel_wait EXACTLY:
    // global_mtx_ + q.mtx() -> resolve_(Expired) -> unlink_locked ->
    // --waiting_waitq_count_ -> make_runnable + route_runnable_locked.
    //
    // Called by pump_deadlines_locked for a due, ACTIVE registration. The
    // registration's try_claim_expiry() (ACTIVE->CONSUMED) has ALREADY been
    // performed by the caller, so THIS call owns the timer authority. The
    // resolve_ CAS is still the publication guard: if a concurrent wake/cancel
    // won the node, resolve_ fails here and this returns false (loser) — but
    // the concurrent winner retired the registration in its own CS, so there is
    // no double-claim. NEVER a parallel timer-wake publication path.
    LockGuard lk(global_mtx_);
    LockGuard qlk(q.mtx());
    if (!q.expire_locked(node)) return false;  // already terminal (loser)
    Fiber* f = node.fiber();
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    if (f != nullptr && f->make_runnable()) {
        route_runnable_locked(f, g_worker);
        return true;
    }
    return false;
}

// ---- E12-A Event wait admission / broadcast (sluice-CORE-E12-A) ----

std::size_t Scheduler::event_set_broadcast(Event& event) {
    LockGuard lk(global_mtx_);
    bool previous = event.set_.exchange(true, std::memory_order::release);
    if (previous) {
        return 0;
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::e12_set_store_before_drain);
#endif
    std::size_t woken = 0;
    while (wake_wait_one_locked(event.waiters_) != nullptr) {
        ++woken;
    }
    // P6: the suspended-Event resolver. Replaces the P2 readiness-offer-only
    // select_event_scan_locked. select_resolve_event_locked walks this Event's
    // SelectPort, applies the single-group P6 gate (P8 multi-group DENIED ->
    // fail-fast before any CAS), marks eligible arms CandidateReady, chooses
    // the lowest INDEX ready arm, drives the P4 group processor exactly once,
    // and publishes exactly once. A zero-eligible return is a clean no-op (no
    // suspended Select arms on this Event).
    (void)select_resolve_event_locked(event);
    return woken;
}

void Scheduler::event_reset(std::atomic<bool>& set_flag) {
    // Transition `set_flag` to UNSET. Pure state flip: does NOT resolve, cancel,
    // expire, unlink, or publish any WaitNode. A waiter already registered
    // remains governed by future set(), deadline, or cancellation. Linearized
    // under global_mtx_ so it serializes with set()'s drain and wait admission
    // (the set/reset epoch isolation domain).
    LockGuard lk(global_mtx_);
    set_flag.store(false, std::memory_order::release);
}

// ---- E13 Select registry operations (private Scheduler authority) ----

void Scheduler::select_event_link_locked(Event& event,
                                         detail::SelectArmSlot& arm) {
    // Event must belong to this Scheduler.
    assert(&event.scheduler_ == this &&
           "select_event_link_locked: Event does not belong to this Scheduler");
    // Precondition: arm is not already linked.
    // The caller (future select() admission) is responsible for setting
    // arm.state to Prepared and arm.group to the owning SelectGroup.
    assert(arm.home_ == nullptr &&
           "select_event_link_locked: arm already linked");
    assert(arm.next_ == nullptr &&
           "select_event_link_locked: arm.next_ not null");
    assert(arm.prev_ == nullptr &&
           "select_event_link_locked: arm.prev_ not null");
    assert(arm.kind == detail::ArmKind::event &&
           "select_event_link_locked: arm kind must be event");
    assert(arm.event.event_ == &event &&
           "select_event_link_locked: arm.event does not point to this Event");
    assert((arm.state == detail::ArmState::detached ||
            arm.state == detail::ArmState::prepared) &&
           "select_event_link_locked: arm state must be Detached or Prepared");
    assert(arm.group != nullptr &&
           "select_event_link_locked: arm.group must be set");

    arm.home_ = &event.select_port_;
    arm.state = detail::ArmState::registered;

    // Insert at head of the doubly-linked list.
    detail::SelectPort& port = event.select_port_;
    arm.next_ = port.head_;
    if (port.head_ != nullptr) {
        port.head_->prev_ = &arm;
    }
    arm.prev_ = nullptr;
    port.head_ = &arm;
}

void Scheduler::select_event_unlink_locked(Event& event,
                                           detail::SelectArmSlot& arm) {
    assert(&event.scheduler_ == this &&
           "select_event_unlink_locked: Event does not belong to this Scheduler");
    // Validate that the arm belongs to this Event's port.
    assert(arm.home_ == &event.select_port_ &&
           "select_event_unlink_locked: arm does not belong to this Event");
    assert((arm.state == detail::ArmState::registered ||
            arm.state == detail::ArmState::candidate_ready ||
            arm.state == detail::ArmState::retired) &&
           "select_event_unlink_locked: unexpected arm state");

    detail::SelectPort& port = event.select_port_;

    // Repair predecessor link.
    if (arm.prev_ != nullptr) {
        arm.prev_->next_ = arm.next_;
    } else {
        // Arm is the head.
        port.head_ = arm.next_;
    }

    // Repair successor link.
    if (arm.next_ != nullptr) {
        arm.next_->prev_ = arm.prev_;
    }

    // Clear arm linkage.
    arm.next_ = nullptr;
    arm.prev_ = nullptr;
    arm.home_ = nullptr;
}

std::size_t Scheduler::select_event_scan_locked(Event& event) {
    assert(&event.scheduler_ == this &&
           "select_event_scan_locked: Event does not belong to this Scheduler");
    // Walk the Event's SelectPort, marking eligible Event Select arms
    // CandidateReady. P2: readiness-offer only — no claim, no finalization,
    // no publication, no unlink, no worklist construction.
    std::size_t marked = 0;
    detail::SelectArmSlot* arm = event.select_port_.head_;
    while (arm != nullptr) {
        detail::SelectArmSlot* next = arm->next_;
        if (arm->kind == detail::ArmKind::event &&
            arm->state == detail::ArmState::registered &&
            arm->group != nullptr &&
            arm->group->phase() == detail::GroupPhase::armed &&
            arm->home_ == &event.select_port_ &&
            arm->event.event_ == &event) {
            arm->state = detail::ArmState::candidate_ready;
            ++marked;
        }
        arm = next;
    }
    return marked;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
void Scheduler::AsyncTestAccess::set_arm_state(Scheduler& s,
                                                detail::SelectArmSlot& arm,
                                                detail::ArmState st) {
    LockGuard lk(s.global_mtx_);
    arm.state = st;
}

// E13 P4 detached-group winner-CAS test entry (out-of-line: needs SelectGroup's
// complete definition from select_port.hpp). Enforces the mechanical detached
// precondition (scheduler_ == nullptr, arms_ == nullptr, arm_count_ == 0)
// BEFORE the CAS, then reaches the private claim_winner_locked. A registered
// group cannot reach this path: it would fail the precondition assertions.
bool Scheduler::AsyncTestAccess::detached_claim_winner(
    detail::SelectGroup& group, std::uint32_t arm_index) noexcept {
    assert(group.scheduler_ == nullptr &&
           "detached winner-CAS accessor requires scheduler_ == nullptr");
    assert(group.arms_ == nullptr &&
           "detached winner-CAS accessor requires arms_ == nullptr");
    assert(group.arm_count_ == 0 &&
           "detached winner-CAS accessor requires arm_count_ == 0");
    return group.claim_winner_locked(arm_index);
}

// E13 P4 central claim + finalization test driver. Acquires global_mtx_ and
// dispatches to the Scheduler-locked core (select_process_group_locked). The
// test harness sets up the registered group exactly as a future admission
// would: group.scheduler_ = &s, arms_/arm_count_ set, Event arms linked via
// select_event_link, Timer arms registered via register_synthetic_select_timer.
bool Scheduler::AsyncTestAccess::select_process_group(
    Scheduler& s, detail::SelectGroup& group, std::uint32_t candidate_index) {
    LockGuard lk(s.global_mtx_);
    return s.select_process_group_locked(group, candidate_index);
}

// E13 P4 all-authority-closed invariant predicate (SN-10). Acquires global_mtx_
// and dispatches to the const locked predicate. Pure read; no mutation.
bool Scheduler::AsyncTestAccess::select_all_authority_closed(
    const Scheduler& s, const detail::SelectGroup& group) {
    LockGuard lk(s.global_mtx_);
    return s.select_all_authority_closed_locked(group);
}

// E13 P4 OA corrective: the all-authority-closed invariant as a fail-fast
// assert — the mechanical precondition a future P6 publication entry will gate
// on. Acquires global_mtx_ and asserts select_all_authority_closed_locked.
void Scheduler::AsyncTestAccess::assert_select_all_authority_closed(
    const Scheduler& s, const detail::SelectGroup& group) {
    LockGuard lk(s.global_mtx_);
    assert(s.select_all_authority_closed_locked(group) &&
           "Select publication requires all arm authority closed");
}

// E13 P4 EH corrective: forge a stale-but-equality-passing Event home_. PRE:
// `arm` is unlinked (home_/next_/prev_ null) and NOT present in `event`'s
// SelectPort intrusive list. Sets arm.home_ = &event.select_port_ so the
// preflight home_ equality check passes while the arm remains absent from the
// intrusive list — exactly the shape required for the mechanical membership
// scan in select_preflight_claim_locked to be load-bearing. Acquires
// global_mtx_ internally; verifies the preconditions under G.
void Scheduler::AsyncTestAccess::select_event_forge_stale_home(
    Scheduler& s, Event& event, detail::SelectArmSlot& arm) {
    LockGuard lk(s.global_mtx_);
    assert(&event.scheduler_ == &s &&
           "select_event_forge_stale_home: Event does not belong to this Scheduler");
    assert(arm.home_ == nullptr &&
           "select_event_forge_stale_home: arm must be unlinked (home_ != nullptr)");
    assert(arm.next_ == nullptr && arm.prev_ == nullptr &&
           "select_event_forge_stale_home: arm must be fully unlinked");
    // The arm must NOT be reachable from the port's intrusive list — otherwise
    // this would be forging a stale home_ for an arm that is genuinely linked.
    for (detail::SelectArmSlot* p = event.select_port_.head_; p != nullptr;
         p = p->next_) {
        assert(p != &arm && "select_event_forge_stale_home: arm is actually "
               "linked into the port intrusive list; cannot forge stale home_");
    }
    arm.home_ = &event.select_port_;
}
#endif

bool Scheduler::event_cancel_wait(WaitQueue& q, WaitNode& node) {
    // E12-A-EVENT-CORRECTIVE-2: the narrow Event cancellation authority with
    // EXACT queue-membership validation. Event::cancel passes its private
    // waiters_ here (NOT exposed to the caller). The contract (Corrective C):
    //   returns true ONLY if node is currently Registered AND currently linked
    //   in THIS Event's private WaitQueue AND CANCEL wins node.resolve_.
    //   Otherwise returns false WITHOUT mutation.
    //
    // The membership check scans THIS queue's own intrusive list for &node
    // while holding this Scheduler's global_mtx_ + this Event's q.mtx(). It
    // does NOT read a foreign node's home_, does NOT lock a foreign Event or
    // foreign Scheduler, and does NOT depend on cross-Scheduler
    // synchronization. Wrong-Event (same OR different Scheduler), detached,
    // Woken, Expired, and Cancelled nodes all return false safely.
    //
    // Generic Scheduler::cancel_wait is unchanged (its caller contract already
    // guarantees membership); this Event-specific path is the one reached from
    // untrusted Event::cancel callers. The resolve_ CAS remains the
    // terminal-winner authority; contains_locked is the membership gate, taken
    // BEFORE resolve_ so no mutation occurs on a non-member. This call CANNOT
    // synthesize a RESOURCE_WAKE and CANNOT change Event SET/UNSET.
    LockGuard lk(global_mtx_);
    LockGuard qlk(q.mtx());
    // Membership gate: a node not linked in THIS queue is not cancellable here.
    // Covers wrong-Event (same/different Scheduler), detached, and (because a
    // terminal winner is already unlinked) Woken/Expired/Cancelled nodes.
    if (!q.contains_locked(node)) return false;
    if (!q.cancel_locked(node)) return false;  // concurrent resolver won (loser)
    retire_timer_for_node_locked(node);
    Fiber* f = node.fiber();
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // The cancel CAS won: the node is terminal+unlinked and the count is closed.
    // Return true unconditionally — the winner identity is the resolve_ CAS, not
    // the runnable publication (mirrors wake_wait_one_locked). make_runnable is
    // the exactly-once publication guard; a false return (fiber already runnable
    // from a concurrent path, or null fiber) does NOT undo the cancel. Returning
    // false here would mislead the caller into retrying or thinking the wait is
    // still active (PR#6 review: gemini-code-assist + coderabbitai).
    if (f != nullptr && f->make_runnable()) {
        route_runnable_locked(f, g_worker);
    }
    return true;
}

void Scheduler::await_event_wait(WaitQueue& q, const std::atomic<bool>& set_flag,
                                 WaitNode& node) {
    // E12-A Event wait admission. The lost-set closure: register + check SET +
    // (if SET) resolve Woken inline, OR commit suspension — all under one
    // global_mtx_ + q.mtx() critical section (the same domain set()/reset() use).
    // Only context_switch is outside the lock. This mirrors await_wait_deadline's
    // I5 already-due path: always register, then check the admission condition.
    //
    // If set_ is observed at admission (after registration), the wait resolves
    // Woken inline via wake_node_locked (resolve_(Woken) + unlink), the timer
    // (if any — none for this non-deadline overload) is retired, the count is
    // decremented, and the fiber does NOT suspend. Because the current Fiber has
    // not yet committed `waiting`, a successful admission-time Woken resolution
    // may cause make_runnable() to return false for the RUNNING Fiber — that is
    // expected and harmless (the fiber continues running and returns from wait).
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    // E12-A-EVENT-CORRECTIVE-2 (T31): mark that an admission attempt has begun,
    // BEFORE acquiring global_mtx_. A causal test observes this marker is set
    // while a setter holds global_mtx_ mid-drain, proving the admission could
    // not have entered its critical section yet. Controller-driven (test variant).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::e12_admission_attempt_before_global_lock);
#endif
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(q.mtx());
        if (!q.register_wait_locked(node, me)) {
            // Node already registered or terminal (C8): contract violation.
            return;
        }
        ++waiting_waitq_count_;
        // E12-A-EVENT-CORRECTIVE-1 (Corrective D): deterministic admission-before-
        // final-set-check phase seam. When armed, pause the admission thread
        // AFTER registration and while it STILL HOLDS global_mtx_+q.mtx(), BEFORE
        // the final SET check. This lets a causal test mechanically prove:
        //   - admission-first: set()'s drain cannot complete until admission
        //     releases serialization (a competing setter blocks on global_mtx_).
        //   - set-first: if the setter stores SET first, admission (paused here
        //     or about to run) cannot complete its drain until the setter
        //     releases; admission then observes SET and resolves Woken inline.
        // The seam blocks on its OWN mtx/cv (the production locks remain held),
        // which is precisely the guarantee under test.
        // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: controller-driven (test variant).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(*this,
            sluice_async_test::PhaseTag::e12_admission_before_final_check);
#endif
        // Admission closure: if SET is observed after registration, resolve this
        // wait as Woken inline through the canonical resolve_ authority. The node
        // is unlinked in the same critical section (wake_node_locked). No suspend.
        if (set_flag.load(std::memory_order::acquire)) {
            if (q.wake_node_locked(node)) {
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                // The current Fiber is RUNNING; make_runnable may return false.
                // That is not a reason to publish it. Return from wait normally.
                if (me != nullptr) (void)me->make_runnable();
            }
            return;  // node.outcome() == woken; do NOT suspend
        }
        // Defense-in-depth: if the node was resolved concurrently (it cannot be,
        // since register_wait_locked just moved it to Registered under both
        // locks and every resolver takes global_mtx_), undo and do not suspend.
        if (node.is_terminal()) {
            q.unlink_locked(node);
            --waiting_waitq_count_;
            return;
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

void Scheduler::await_event_wait_deadline(WaitQueue& q,
                                          const std::atomic<bool>& set_flag,
                                          WaitNode& node, deadline_t deadline) {
    // E12-A deadline-aware Event wait. Composes await_event_wait's admission
    // closure with E11 TimerRegistration. The wait resolves when EXACTLY ONE
    // cause wins the resolve_ CAS:
    //   - set() broadcast (event_set_broadcast -> wake_wait_one_locked) -> Woken
    //   - cancel_wait(q, node)                                   -> Cancelled
    //   - the deadline elapsing (pump_deadlines_locked)           -> Expired
    //
    // Admission precedence (under global_mtx_ + q.mtx()):
    //   1. If set_ is observed SET after registration: resolve Woken inline
    //      (no suspend). Event readiness wins over a due deadline at admission
    //      (the resource is ready; the deadline is moot).
    //   2. Else if the deadline is already due: resolve Expired inline (E11 I5).
    //   3. Else: commit suspension.
    // A non-timer winner retires the registration in the same CS (E11 I4).
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(q.mtx());
        if (!q.register_wait_locked(node, me)) {
            return;  // C8 contract violation
        }
        ++waiting_waitq_count_;
        // Create the timer registration control block for this wait epoch.
        timer_pool_.emplace_back(&node, &q, deadline);
        reg = &timer_pool_.back();
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();

        // Admission closure — Event SET takes precedence: if the resource is
        // ready, the wait resolves Woken inline (the deadline is moot).
        if (set_flag.load(std::memory_order::acquire)) {
            if (q.wake_node_locked(node)) {
                if (reg->retire()) {  // ACTIVE->RETIRED (closes timer authority)
                    --active_deadline_count_;
                }
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                if (me != nullptr) (void)me->make_runnable();
            }
            return;  // node.outcome() == woken; do NOT suspend
        }

        // E11 I5 admission closure: if the deadline is ALREADY due (and the
        // resource is NOT set), resolve Expired inline. The fiber must NOT
        // suspend and wait for a future timer scan merely because registration
        // happened after the deadline was due.
        if (clock_now_unlocked() >= deadline) {
            if (q.expire_locked(node)) {
                reg->try_claim_expiry();  // ACTIVE->CONSUMED (winner)
                --active_deadline_count_;
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                // The current Fiber is RUNNING (has not called make_waiting());
                // make_runnable() returns false for a Running fiber (E7-T2).
                // Call it for state consistency but do NOT route - matches the
                // inline SET admission path above.
                if (me != nullptr) (void)me->make_runnable();
                return;  // resolved at admission; do NOT suspend
            }
            // If expire_locked lost, a concurrent resolver won; fall through.
        }

        // Defense-in-depth: if the node was resolved concurrently, undo + return.
        if (node.is_terminal()) {
            q.unlink_locked(node);
            --waiting_waitq_count_;
            if (reg->retire()) {
                --active_deadline_count_;
            }
            recompute_earliest_deadline_locked();
            return;
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

// ---- E12-B Semaphore admission / release (sluice-CORE-E12-B) ----

bool Scheduler::sem_try_acquire(WaitQueue& waiters,
                                std::atomic<std::uint32_t>& available) {
    // Lock-free try_acquire is FORBIDDEN: it would bypass the FIFO queue and
    // admit a newcomer ahead of an eligible head (barging). The authoritative
    // decision is made under global_mtx_ + waiters_.mtx(), the SAME domain every
    // release / cancel / expire / admission path takes. `available` is atomic
    // ONLY to support lock-free observation via Semaphore::available(); it does
    // NOT authorize lock-free acquisition.
    //
    // No barging: if a waiter is already queued (eligible FIFO head exists),
    // try_acquire MUST fail even when available_ > 0 — a newcomer may not bypass
    // the queued waiter's priority. This preserves the stable-state invariant
    // (EligibleQueuedWaiterExists => available_ == 0): an eligible waiter is
    // never left stranded while a stored permit exists.
    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());
    if (waiters.empty_locked()) {
        const std::uint32_t cur = available.load(std::memory_order::acquire);
        if (cur > 0) {
            available.store(cur - 1, std::memory_order::release);
            return true;
        }
    }
    return false;  // no stored permit, OR an eligible queued waiter has priority
}

void Scheduler::sem_acquire(WaitQueue& waiters,
                            std::atomic<std::uint32_t>& available,
                            WaitNode& node) {
    // E12-B acquire admission. The lost-wake closure: register + recheck
    // admission + commit suspension — all under one global_mtx_ + waiters_.mtx()
    // critical section (the same domain release / cancel / expire / admission
    // use). Only context_switch is outside the lock. This mirrors
    // await_event_wait's lost-set closure (the canonical lost-wake-closed idiom,
    // commit 422036c) but the admission predicate is "a stored permit is
    // admissible to THIS newly-registered FIFO head" instead of "SET observed".
    //
    // The admission window is closed: a permit observed during the admission
    // critical section leads to inline Woken, not a sleeping registered waiter
    // with stored supply. The trace
    //     initial check sees no permit
    //     waiter registers
    //     release occurs
    //     waiter attempts suspension
    // cannot strand the waiter because release() takes global_mtx_ to transfer
    // its permit, and this admission recheck runs under the same global_mtx_
    // after registration — release either completed before registration (its
    // transfer targeted the prior head / stored the permit, which the recheck
    // observes) or runs after this critical section (it sees this registered
    // node and transfers to it).
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (!waiters.register_wait_locked(node, me)) {
            // Node already registered or terminal (C8): contract violation.
            return;
        }
        ++waiting_waitq_count_;

        // Admission recheck: a stored permit is admissible to THIS node only if
        // it is the FIFO head (no earlier waiter has priority). register_wait_
        // locked links at the FIFO TAIL, so this node is the head iff it has no
        // predecessor (node.prev_ == nullptr). The link field prev_ is read here
        // while holding waiters_.mtx() (the structural authority). wake_node_
        // locked resolves THIS specific node with Woken, so we target it
        // directly — there is no head-identity ambiguity at the resolve CAS.
        //
        // If an earlier waiter is queued (node.prev_ != nullptr), this node MUST
        // NOT consume a stored permit even when available_ > 0: the earlier
        // waiter has FIFO priority. The stable-state invariant
        // (EligibleQueuedWaiterExists => available_ == 0) holds in production
        // because a release transfers to the head rather than storing when a
        // waiter is queued, so available_ > 0 with a queued earlier waiter is a
        // transient that this check correctly refuses to admit.
        if (node.prev_ == nullptr &&
            available.load(std::memory_order::acquire) > 0) {
            if (waiters.wake_node_locked(node)) {
                available.store(available.load(std::memory_order::acquire) - 1,
                                std::memory_order::release);
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                // The current Fiber is RUNNING (has not called make_waiting());
                // make_runnable may return false. That is not a reason to
                // publish it.
                if (me != nullptr) (void)me->make_runnable();
            }
            return;  // node.outcome() == woken; do NOT suspend
        }

        // Defense-in-depth: if the node was resolved concurrently (it cannot be,
        // since register_wait_locked just moved it to Registered under both
        // locks and every resolver takes global_mtx_), undo and do not suspend.
        if (node.is_terminal()) {
            waiters.unlink_locked(node);
            --waiting_waitq_count_;
            return;
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

void Scheduler::sem_acquire_until(WaitQueue& waiters,
                                  std::atomic<std::uint32_t>& available,
                                  WaitNode& node, deadline_t deadline) {
    // E12-B deadline-aware acquire. Composes sem_acquire's admission closure
    // with E11 TimerRegistration. The wait resolves when EXACTLY ONE cause wins
    // the resolve_ CAS:
    //   - release() transfer (sem_release -> wake_wait_one_locked) -> Woken
    //   - cancel(node) (sem_cancel)                              -> Cancelled
    //   - the deadline elapsing (pump_deadlines_locked)          -> Expired
    //
    // Admission precedence (A4, under global_mtx_ + waiters_.mtx()):
    //   1. If a permit is admissible (available > 0 AND node is FIFO head):
    //      resolve Woken inline (no suspend). Permit admission wins over a due
    //      deadline (the resource is ready; the deadline is moot).
    //   2. Else if the deadline is already due: resolve Expired inline (E11 I5).
    //   3. Else: commit suspension.
    // A non-timer winner retires the registration in the same CS (E11 I4).
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (!waiters.register_wait_locked(node, me)) {
            return;  // C8 contract violation
        }
        ++waiting_waitq_count_;
        // Create the timer registration control block for this wait epoch.
        timer_pool_.emplace_back(&node, &waiters, deadline);
        reg = &timer_pool_.back();
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();

        // Admission precedence 1: permit admission wins over a due deadline. If
        // a stored permit is available AND this node is the FIFO head (no earlier
        // waiter has priority — node.prev_ == nullptr, read under waiters_.mtx()),
        // resolve Woken inline and retire the timer (the deadline is moot).
        if (node.prev_ == nullptr &&
            available.load(std::memory_order::acquire) > 0) {
            if (waiters.wake_node_locked(node)) {
                available.store(
                    available.load(std::memory_order::acquire) - 1,
                    std::memory_order::release);
                if (reg->retire()) {  // ACTIVE->RETIRED (closes timer)
                    --active_deadline_count_;
                }
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                if (me != nullptr) (void)me->make_runnable();
            }
            return;  // node.outcome() == woken; do NOT suspend
        }

        // Admission precedence 2: E11 I5 — if the deadline is ALREADY due (and
        // no permit is admissible), resolve Expired inline. The fiber must NOT
        // suspend and wait for a future timer scan merely because registration
        // happened after the deadline was due.
        if (clock_now_unlocked() >= deadline) {
            if (waiters.expire_locked(node)) {
                reg->try_claim_expiry();  // ACTIVE->CONSUMED (winner)
                --active_deadline_count_;
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                if (me != nullptr) (void)me->make_runnable();
                return;  // resolved at admission; do NOT suspend
            }
            // If expire_locked lost, a concurrent resolver won; fall through.
        }

        // Defense-in-depth: if the node was resolved concurrently, undo + return.
        if (node.is_terminal()) {
            waiters.unlink_locked(node);
            --waiting_waitq_count_;
            if (reg->retire()) {
                --active_deadline_count_;
            }
            recompute_earliest_deadline_locked();
            return;
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

bool Scheduler::sem_cancel(WaitQueue& waiters, WaitNode& node) {
    // E12-B queue-identity-safe cancellation. Mirrors event_cancel_wait exactly.
    // Semaphore::cancel passes its private waiters_ here (NOT exposed to the
    // caller). The contract:
    //   returns true ONLY if node is currently Registered AND currently linked
    //   in THIS Semaphore's private WaitQueue AND CANCEL wins node.resolve_.
    //   Otherwise returns false WITHOUT mutation.
    //
    // The membership check scans THIS queue's own intrusive list for &node
    // while holding this Scheduler's global_mtx_ + this Semaphore's
    // waiters_.mtx(). It does NOT read a foreign node's home_, does NOT lock a
    // foreign Semaphore or foreign Scheduler. Wrong-Semaphore (same OR different
    // Scheduler), detached, Woken, Expired, and Cancelled nodes all return false
    // safely. This call CANNOT synthesize a RESOURCE_WAKE and CANNOT change
    // available_.
    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());
    // Membership gate: a node not linked in THIS queue is not cancellable here.
    if (!waiters.contains_locked(node)) return false;
    if (!waiters.cancel_locked(node)) return false;  // concurrent resolver won
    retire_timer_for_node_locked(node);
    Fiber* f = node.fiber();
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    if (f != nullptr && f->make_runnable()) {
        route_runnable_locked(f, g_worker);
    }
    return true;
}

bool Scheduler::sem_release(WaitQueue& waiters,
                            std::atomic<std::uint32_t>& available,
                            std::uint32_t max_permits) {
    // E12-B release disposition. Under global_mtx_, this call contributes
    // exactly ONE pending permit. The disposition is exactly one of:
    //   - transferred to the FIFO head waiter (available_ UNCHANGED)
    //   - stored into available_ (available_++)
    //   - rejected: queue empty AND available_ == max_permits_ (no mutation)
    //
    // Transfer branch: wake_wait_one_locked takes global_mtx_ (held here) and
    // waiters_.mtx() (acquired INSIDE it), resolves the FIFO head with Woken,
    // and routes the winner. By Conclusion A, a linked FIFO head observed under
    // global_mtx_ + waiters_.mtx() is Registered and eligible; its
    // resolve_(Woken) cannot lose, and wake_wait_one_locked returns nullptr ONLY
    // when the queue is empty. Therefore a non-empty queue produces exactly one
    // winner and the transfer branch returns true with available_ UNCHANGED.
    //
    // Empty-queue branch: wake_wait_one_locked returned nullptr (queue empty at
    // the moment of the check). Store the permit, or reject overflow. A later
    // acquire that registers after this store is handled by its admission
    // recheck (which observes the stored permit and resolves Woken inline), so
    // no permit is stranded. available_ is bounded by max_permits_; no mutation
    // on overflow.
    //
    // Forbidden shapes (NOT present): available_-- before waking a waiter;
    // refund after a lost wake; reserve-then-commit; a grant-in-flight field;
    // increment available_ AND wake a waiter in one release; retry after a null
    // wake; skip-after-null. A queued grant from available_ == 0 succeeds
    // without decrement or integer underflow (the permit is transferred, not
    // withdrawn and re-deposited).
    //
    // Safe to call from an external OS thread: g_worker is null on a non-worker
    // thread, so route_runnable_locked routes the winner through pending_spawn_
    // and signal_wake_locked wakes a parked Scheduler worker — exactly the
    // event_set_broadcast external-thread path.
    LockGuard lk(global_mtx_);
    // Transfer branch: wake_wait_one_locked acquires waiters_.mtx() inside
    // global_mtx_ (consistent lock order) and resolves the FIFO head. nullptr
    // means the queue is empty (Conclusion A). One release never both wakes a
    // waiter AND stores: a non-empty queue fully consumes this release.
    if (wake_wait_one_locked(waiters) != nullptr) {
        return true;  // permit transferred to the FIFO head; available_ unchanged
    }
    // Empty-queue branch: store the permit, or reject overflow.
    const std::uint32_t cur = available.load(std::memory_order::acquire);
    if (cur >= max_permits) {
        return false;  // overflow: no authoritative mutation
    }
    available.store(cur + 1, std::memory_order::release);
    return true;
}

// ---- E12-C AsyncMutex admission / handoff (sluice-CORE-E12-C) ----

bool Scheduler::mutex_try_lock(WaitQueue& waiters, Fiber*& owner) {
    // Authoritative try_lock under global_mtx_ + waiters_.mtx() (the SAME
    // domain every unlock / cancel / expire / admission path takes). owner is
    // passed by reference (the AsyncMutex's Fiber* owner_); nullptr == NoOwner.
    //
    // No barging: if a waiter is already queued (eligible FIFO head exists),
    // try_lock MUST fail even when owner_ == nullptr — a newcomer may not bypass
    // the queued waiter's priority.
    //
    // Recursive acquire (current Fiber == owner) returns false with no mutation
    // (recursive locking is forbidden, §7.1). A null current Fiber (external
    // thread / no g_worker) is a caller-precondition debug assert.
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncMutex::try_lock requires a running Fiber");
    Fiber* me = ws->current;
    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());
    if (owner == me) {
        // Recursive try_lock: forbidden. Return false, no mutation.
        return false;
    }
    if (owner == nullptr && waiters.empty_locked()) {
        owner = me;
        return true;
    }
    return false;  // owned, OR an eligible queued waiter has FIFO priority
}

void Scheduler::mutex_lock(WaitQueue& waiters, Fiber*& owner, WaitNode& node) {
    // E12-C lock admission. The lost-wake closure: register + recheck admission
    // + commit suspension — all under one global_mtx_ + waiters_.mtx() critical
    // section (the same domain unlock / cancel / expire / admission use). Only
    // context_switch is outside the lock. Mirrors sem_acquire's lost-set closure
    // but the admission predicate is "owner_ == nullptr AND this node is the
    // eligible FIFO head" instead of "a stored permit is admissible".
    //
    // The admission window is closed: an ownership-free observation during the
    // admission critical section leads to inline Woken, not a sleeping
    // registered waiter. The trace
    //     initial check sees owner_ != nullptr
    //     waiter registers
    //     owner unlocks (handoff or free)
    //     waiter attempts suspension
    // cannot strand the waiter because unlock() takes global_mtx_ to hand off
    // or free, and this admission recheck runs under the same global_mtx_ after
    // registration — unlock either completed before registration (its handoff
    // targeted the prior head / freed owner_, which the recheck observes) or
    // runs after this critical section (it sees this registered node and hands
    // off to it).
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncMutex::lock requires a running Fiber");
    Fiber* me = ws->current;
    assert(owner != me && "AsyncMutex::lock recursive acquisition is a caller "
                          "precondition violation (not a successful acquisition)");
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (!waiters.register_wait_locked(node, me)) {
            // Node already registered or terminal (C8): contract violation.
            return;
        }
        ++waiting_waitq_count_;

        // Admission recheck: ownership is admissible to THIS node only if owner_
        // is free (nullptr) AND it is the FIFO head (no earlier waiter has
        // priority). register_wait_locked links at the FIFO TAIL, so this node
        // is the head iff node.prev_ == nullptr (read under waiters_.mtx(), the
        // structural authority). wake_node_locked resolves THIS specific node
        // with Woken, so there is no head-identity ambiguity at the resolve CAS.
        //
        // If an earlier waiter is queued (node.prev_ != nullptr), this node MUST
        // NOT acquire even when owner_ == nullptr: the earlier waiter has FIFO
        // priority. A concurrent unlock hands off to the head (this node, if it
        // is the head) rather than freeing, so owner_ == nullptr with a queued
        // earlier waiter is a transient that this check correctly refuses to
        // admit.
        if (node.prev_ == nullptr && owner == nullptr) {
            if (waiters.wake_node_locked(node)) {
                owner = me;
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                // The current Fiber is RUNNING (has not called make_waiting());
                // make_runnable may return false. That is not a reason to
                // publish it.
                if (me != nullptr) (void)me->make_runnable();
            }
            return;  // node.outcome() == woken; do NOT suspend
        }

        // Defense-in-depth: if the node was resolved concurrently (it cannot be,
        // since register_wait_locked just moved it to Registered under both
        // locks and every resolver takes global_mtx_), undo and do not suspend.
        if (node.is_terminal()) {
            waiters.unlink_locked(node);
            --waiting_waitq_count_;
            return;
        }
        // E12-D-CLOSURE: this fiber's node is registered in the Mutex waiter
        // queue and the fiber will suspend (no immediate ownership). A test
        // observing this phase proves the node queued (T15a/T15b).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::e12_mutex_waiter_registered_before_grant);
#endif
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

void Scheduler::mutex_lock_until(WaitQueue& waiters, Fiber*& owner,
                                 WaitNode& node, deadline_t deadline) {
    // E12-C deadline-aware lock. Composes mutex_lock's admission closure with
    // E11 TimerRegistration. The wait resolves when EXACTLY ONE cause wins the
    // resolve_ CAS:
    //   - unlock() handoff (mutex_unlock -> mutex_handoff_one_locked) -> Woken
    //   - cancel(node) (mutex_cancel)                              -> Cancelled
    //   - the deadline elapsing (pump_deadlines_locked)            -> Expired
    //
    // Admission precedence (resource-first, under global_mtx_ + waiters_.mtx()):
    //   1. If ownership is admissible (owner == nullptr AND node is FIFO head):
    //      resolve Woken inline (no suspend). Ownership admission wins over a
    //      due deadline (the resource is ready; the deadline is moot).
    //   2. Else if the deadline is already due: resolve Expired inline (E11 I5).
    //   3. Else: commit suspension.
    // A non-timer winner retires the registration in the same CS (E11 I4).
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncMutex::lock_until requires a running Fiber");
    Fiber* me = ws->current;
    assert(owner != me && "AsyncMutex::lock_until recursive acquisition is a "
                          "caller precondition violation");
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (!waiters.register_wait_locked(node, me)) {
            return;  // C8 contract violation
        }
        ++waiting_waitq_count_;
        // Create the timer registration control block for this wait epoch.
        timer_pool_.emplace_back(&node, &waiters, deadline);
        reg = &timer_pool_.back();
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();

        // Admission precedence 1: ownership admission wins over a due deadline.
        // If owner_ is free AND this node is the FIFO head (no earlier waiter —
        // node.prev_ == nullptr, read under waiters_.mtx()), resolve Woken
        // inline, commit ownership, and retire the timer (the deadline is moot).
        if (node.prev_ == nullptr && owner == nullptr) {
            if (waiters.wake_node_locked(node)) {
                owner = me;
                if (reg->retire()) {  // ACTIVE->RETIRED (closes timer)
                    --active_deadline_count_;
                }
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                if (me != nullptr) (void)me->make_runnable();
            }
            return;  // node.outcome() == woken; do NOT suspend
        }

        // Admission precedence 2: E11 I5 — if the deadline is ALREADY due (and
        // ownership is not admissible), resolve Expired inline. The fiber must
        // NOT suspend and wait for a future timer scan merely because
        // registration happened after the deadline was due.
        if (clock_now_unlocked() >= deadline) {
            if (waiters.expire_locked(node)) {
                reg->try_claim_expiry();  // ACTIVE->CONSUMED (winner)
                --active_deadline_count_;
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                if (me != nullptr) (void)me->make_runnable();
                return;  // resolved at admission; do NOT suspend
            }
            // If expire_locked lost, a concurrent resolver won; fall through.
        }

        // Defense-in-depth: if the node was resolved concurrently, undo + return.
        if (node.is_terminal()) {
            waiters.unlink_locked(node);
            --waiting_waitq_count_;
            if (reg->retire()) {
                --active_deadline_count_;
            }
            recompute_earliest_deadline_locked();
            return;
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

bool Scheduler::mutex_cancel(WaitQueue& waiters, WaitNode& node) {
    // E12-C queue-identity-safe cancellation. Mirrors sem_cancel exactly.
    // AsyncMutex::cancel passes its private waiters_ here (NOT exposed to the
    // caller). The contract:
    //   returns true ONLY if node is currently Registered AND currently linked
    //   in THIS AsyncMutex's private WaitQueue AND CANCEL wins node.resolve_.
    //   Otherwise returns false WITHOUT mutation.
    //
    // May run from ANY OS thread (g_worker may be null): cancel does not acquire
    // Mutex ownership and does not require Fiber identity. The membership check
    // scans THIS queue's own intrusive list for &node while holding this
    // Scheduler's global_mtx_ + this AsyncMutex's waiters_.mtx(). It does NOT
    // read a foreign node's home_, does NOT lock a foreign Mutex or foreign
    // Scheduler. Wrong-Mutex (same OR different Scheduler), detached, Woken,
    // Expired, and Cancelled nodes all return false safely. This call CANNOT
    // synthesize a RESOURCE_WAKE and CANNOT change owner_.
    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());
    // Membership gate: a node not linked in THIS queue is not cancellable here.
    if (!waiters.contains_locked(node)) return false;
    if (!waiters.cancel_locked(node)) return false;  // concurrent resolver won
    retire_timer_for_node_locked(node);
    Fiber* f = node.fiber();
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    if (f != nullptr && f->make_runnable()) {
        route_runnable_locked(f, g_worker);
    }
    return true;
}

WaitNode* Scheduler::mutex_handoff_one_locked(WaitQueue& waiters, Fiber*& owner) {
    // MUTEX-HANDOFF-ONE (docs §10): the narrow private seam that resolves the
    // eligible FIFO head Woken, commits owner_ = winner Fiber, retires any
    // bound timer, and publishes the winner runnable — in THAT source order
    // (owner commit BEFORE make_runnable / route_runnable_locked). This is the
    // load-bearing owner-before-publication refinement obligation (§10.5/§15.4).
    //
    // Mirrors wake_wait_one_locked EXCEPT it writes the winner's fiber() into
    // the caller's `owner` reference between resolution and publication. The
    // Semaphore has no ownership to commit; the Mutex does.
    //
    // The caller MUST hold global_mtx_. waiters_.mtx() is taken here (under
    // global_mtx_, consistent lock order). Returns the winning node (nullptr if
    // the queue is empty or the head lost to a concurrent resolver). A winning
    // linked node with null Fiber is an internal-invariant debug assert, NOT an
    // empty-queue result (§10.6).
    LockGuard qlk(waiters.mtx());
    WaitNode* won = waiters.wake_one_locked();  // resolve FIFO head Woken + unlink
    if (won == nullptr) return nullptr;  // empty, or head lost to a cancel
    Fiber* f = won->fiber();
    assert(f != nullptr && "MUTEX-HANDOFF-ONE winner has null Fiber "
                           "(internal invariant failure, NOT empty queue)");
    // ---- owner commit BEFORE publication (§10.5 load-bearing order) ----
    owner = f;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic test phase: after owner commit, BEFORE runnable publication.
    // A test observing this phase can prove owner == winner Fiber, winner not
    // yet published, old owner cannot reacquire, newcomer try_lock cannot barge.
    // No allocation, no callback, no lock held beyond global_mtx_ (already held
    // by the caller); compiled ONLY for the internal-testing variant.
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::e12_mutex_handoff_before_publication);
#endif
    retire_timer_for_node_locked(*won);  // E11 I4 timer closure (same CS)
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // E7-T2 exactly-once: publish a runnable ticket ONLY if waiting->runnable
    // succeeded. The node is terminal; make_runnable is the publication guard.
    if (f != nullptr && f->make_runnable()) {
        route_runnable_locked(f, g_worker);
    }
    return won;
}

void Scheduler::mutex_unlock(WaitQueue& waiters, Fiber*& owner) {
    // E12-C unlock with direct ownership handoff. Under global_mtx_, the calling
    // (owner) Fiber releases ownership:
    //   - if the queue has an eligible FIFO head: MUTEX-HANDOFF-ONE resolves it
    //     Woken, commits owner_ = winner Fiber (BEFORE publication), retires the
    //     timer, and publishes the winner runnable exactly once. The ownership
    //     transition is Owned(F_old) -> Owned(F_new) with NO intermediate
    //     owner_ = nullptr.
    //   - otherwise (queue empty): owner_ = nullptr (UnlockNoWaiter).
    //
    // Non-owner unlock and unlock-while-unlocked are caller-precondition debug
    // asserts with no owner/queue mutation. Requires a running Fiber
    // (g_worker->current); the current Fiber must equal `owner`.
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncMutex::unlock requires a running Fiber");
    Fiber* me = ws->current;
    LockGuard lk(global_mtx_);
    assert(owner == me && "AsyncMutex::unlock by non-owner is a caller "
                          "precondition violation (no owner/queue mutation)");
    (void)me;  // debug-only precondition check; release path does not need me
    // Handoff branch: mutex_handoff_one_locked acquires waiters_.mtx() inside
    // global_mtx_ (consistent lock order) and resolves the FIFO head. nullptr
    // means the queue is empty (Conclusion A).
    if (mutex_handoff_one_locked(waiters, owner) != nullptr) {
        return;  // ownership transferred to the FIFO head; owner_ = winner
    }
    // Empty-queue branch: release ownership. No waiter to hand off to.
    owner = nullptr;
}

// ===========================================================================
// E12-E AsyncQueue private seams (sluice-CORE-E12-E).
//
// Blocking/timed wait admission + reconciliation. A QueuePort owns a producer
// and a consumer WaitQueue (waiters_[2]); the Scheduler is the authoritative
// resolution + publication executor, as for E12-A/B/C/D. Lock order:
// G (global_mtx_) -> S (QueuePort::state_mtx_) -> exactly one role mtx();
// the two role mutexes are NEVER held together.
//
// DESIGN (atomic reconciler commit, single suspend). The reconciler is the
// OTHER role's fast-path success: e.g. a producer's try_push that commits an
// item to the ring WAKES the consumer FIFO head via wake_wait_one_locked and,
// in the SAME G + S + role.mtx() critical section, moves the just-committed
// item into that specific consumer's out-lease (read via won->user()). The
// consumer's admit did a SINGLE register + suspend; on wake its out-lease is
// already non-empty (item granted) — no re-check loop, no per-node reuse
// problem. The producer direction is symmetric: a consumer's try_pop that
// opened a slot wakes the producer FIFO head and commits the producer's lease
// into the freed slot. close() wakes every parked producer (closed outcome —
// lease retained) and every parked consumer (pop remaining items, else
// closed).
//
// Per-operation context: the admit caller stashes a QueueWaitCtx* on the
// WaitNode (node.set_user) BEFORE registering. The ctx carries the producer
// control pointer (push) or the consumer out-lease address (pop). The
// reconciler reads won->user() after wake_one_locked returns the winner.
//
// Winner-before-publication: the resolve_(Woken) CAS + the resource commit +
// timer retire all happen in the SAME G + S + role critical section, BEFORE
// make_runnable/route_runnable_locked (publication). A woken Fiber observes
// the final state on resume.
//
// E12-E Queue timer bookkeeping (Corrective-2 §8 supersession). The
// non-template QueuePort owns TWO per-port counters that bracket a Queue
// timed wait:
//   - `active_wait_associations_` (incremented on every successful
//     registration; decremented on every resolution path — inline admit,
//     grant seam, queue_cancel, AND the pump-driven timer expiry).
//   - `active_queue_timers_` (incremented at timer registration; decremented
//     when the timer is consumed or retired).
// `active_wait_associations_` is decremented manually at each resolution
// site that can name the port (the four admit seams, the two grant seams,
// queue_cancel). The pump_deadlines_locked path cannot otherwise reach the
// port, so for a Queue-bound registration it uses the registration's
// `owner_ctx_` to perform the `--active_wait_associations_` decrement.
// `active_queue_timers_` is decremented via the on-resolve thunk installed
// on the TimerRegistration (fired by pump on consume and by
// retire_timer_for_node_locked on retire); this keeps the timer-counter
// bookkeeping localized to the timer's ACTIVE->terminal transition.
// Non-Queue waits leave the hook null and the Scheduler's default
// `--waiting_waitq_count_` accounting applies unchanged.
//
// The §8 PreparedQueueTimer/prepare/activate/discard substrate is
// SUPERSEDED by this minimal model (see docs/e12-queue-corrective-3.md).
//
// static
void Scheduler::queue_timer_on_resolve(void* owner_ctx,
                                       bool /*timer_won*/) noexcept {
    // The on-resolve thunk for a Queue-bound TimerRegistration. Decrements
    // `active_queue_timers_` exactly once. Idempotent under the caller's
    // global_mtx_ + the registration's single ACTIVE->terminal transition.
    // Static-member form so it can reach QueuePort's private counter via
    // Scheduler's friend grant.
    auto* port = static_cast<detail::QueuePort*>(owner_ctx);
    if (port == nullptr) return;
    if (port->active_queue_timers_ > 0) --port->active_queue_timers_;
}


void Scheduler::queue_push_admit(detail::QueuePort& port, WaitNode& node,
                                 detail::QueueItemLease& lease) {
    // P5 blocking push. Register on the producer FIFO under G + S +
    // producer.mtx(); admission recheck commits inline if admissible (Open +
    // space + FIFO head) — else suspend. The reconciler (a consumer's try_pop
    // that freed a slot, or close) commits the lease into a ring slot in the
    // same critical section as the resolve CAS. On resume the caller reads
    // lease.control_: null => committed (ring owns it); non-null => closed/
    // cancelled/expired (caller returns the lease).
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncQueue::push requires a running Fiber");
    Fiber* me = ws->current;
    detail::QueueItemControl* c = lease.control_;
    assert(c != nullptr && c->location_ ==
           detail::QueueItemControl::Location::producer_operation);
    QueueWaitCtx ctx{&port, detail::QueueRole::producer, c, &lease, nullptr};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard slk(port.state_mtx_);
        LockGuard qlk(port.waiters_[0].mtx());
        if (!port.waiters_[0].register_wait_locked(node, me)) {
            return;  // C8 contract violation
        }
        ++port.active_wait_associations_;
        ++waiting_waitq_count_;
        // Admission recheck: Open + space + FIFO head => commit inline (the
        // common no-contention case; the reconciler path handles the rest).
        if (!port.closed_ && !port.ring_full_locked() && node.prev_ == nullptr) {
            c->location_ = detail::QueueItemControl::Location::ring;
            const std::size_t tail =
                (port.ring_head_ + port.ring_count_) % port.capacity_;
            port.ring_[tail] = std::move(lease);  // caller lease now empty
            ++port.ring_count_;
            port.waiters_[0].wake_node_locked(node);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;  // committed inline
        }
        // Closed at admission: resolve Woken with the lease retained (P7).
        if (port.closed_) {
            port.waiters_[0].wake_node_locked(node);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;  // lease retained; caller returns closed
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    // F.2 corrective: this fiber has now RESUMED from a suspended-winner
    // publication. Decrement the per-port `granted_not_resumed_` counter under
    // G (the publication side incremented it under G in the grant seam).
    {
        LockGuard lk(global_mtx_);
        if (port.granted_not_resumed_ > 0) --port.granted_not_resumed_;
    }
    // On resume the reconciler already finalized: lease.control_==null means
    // committed, non-null means closed/expired/cancelled.
}

void Scheduler::queue_pop_admit(detail::QueuePort& port, WaitNode& node,
                                detail::QueueItemLease& out) {
    // P5 blocking pop. Symmetric. Admission recheck pops inline if the ring is
    // non-empty + FIFO head; else suspend. The reconciler (a producer's
    // try_push that added an item, or close) moves a ring item into `out`.
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncQueue::pop requires a running Fiber");
    Fiber* me = ws->current;
    QueueWaitCtx ctx{&port, detail::QueueRole::consumer, nullptr, nullptr, &out};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard slk(port.state_mtx_);
        LockGuard qlk(port.waiters_[1].mtx());
        if (!port.waiters_[1].register_wait_locked(node, me)) {
            return;  // C8
        }
        ++port.active_wait_associations_;
        ++waiting_waitq_count_;
        if (!port.ring_empty_locked() && node.prev_ == nullptr) {
            const std::size_t head = port.ring_head_;
            out = std::move(port.ring_[head]);
            port.ring_head_ = (port.ring_head_ + 1) % port.capacity_;
            --port.ring_count_;
            out.control_->location_ =
                detail::QueueItemControl::Location::consumer_operation;
            port.waiters_[1].wake_node_locked(node);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;  // item granted inline
        }
        if (port.ring_empty_locked() && port.closed_) {
            port.waiters_[1].wake_node_locked(node);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;  // closed+empty
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    // F.2 corrective: this fiber has RESUMED from a suspended-winner
    // publication. Decrement granted_not_resumed_ under G.
    {
        LockGuard lk(global_mtx_);
        if (port.granted_not_resumed_ > 0) --port.granted_not_resumed_;
    }
}

void Scheduler::queue_push_admit_until(detail::QueuePort& port, WaitNode& node,
                                       detail::QueueItemLease& lease,
                                       deadline_t deadline) {
    // P4 timed push. Composes queue_push_admit with E11 timer registration and
    // the already-due inline-Expired precedence (resource-first). On expired
    // the lease is retained (caller returns expired).
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncQueue::push_until requires a running Fiber");
    Fiber* me = ws->current;
    detail::QueueItemControl* c = lease.control_;
    QueueWaitCtx ctx{&port, detail::QueueRole::producer, c, &lease, nullptr};
    node.set_user(&ctx);
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard slk(port.state_mtx_);
        LockGuard qlk(port.waiters_[0].mtx());
        if (!port.waiters_[0].register_wait_locked(node, me)) {
            return;  // C8
        }
        ++port.active_wait_associations_;
        ++waiting_waitq_count_;
        timer_pool_.emplace_back(&node, &port.waiters_[0], deadline);
        reg = &timer_pool_.back();
        reg->on_resolve_ = &Scheduler::queue_timer_on_resolve;  // F.1/F.2 wiring
        reg->owner_ctx_ = &port;
        ++port.active_queue_timers_;
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();
        // Admission precedence 1: resource admissible => commit + resolve.
        if (!port.closed_ && !port.ring_full_locked() && node.prev_ == nullptr) {
            c->location_ = detail::QueueItemControl::Location::ring;
            const std::size_t tail =
                (port.ring_head_ + port.ring_count_) % port.capacity_;
            port.ring_[tail] = std::move(lease);
            ++port.ring_count_;
            port.waiters_[0].wake_node_locked(node);
            if (reg->retire()) { --active_deadline_count_; }
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/false);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;
        }
        // Closed => resolve (lease retained).
        if (port.closed_) {
            port.waiters_[0].wake_node_locked(node);
            if (reg->retire()) { --active_deadline_count_; }
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/false);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;
        }
        // Admission precedence 2: already-due => Expired inline (I5).
        if (clock_now_unlocked() >= deadline) {
            if (port.waiters_[0].expire_locked(node)) {
                reg->try_claim_expiry();
                --active_deadline_count_;
                recompute_earliest_deadline_locked();
                reg->fire_on_resolve_locked(/*timer_won=*/true);
                if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                return;  // expired; lease retained
            }
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    // F.2 corrective: this fiber has RESUMED from a suspended-winner
    // publication (or a timer-expiry publication). Decrement
    // granted_not_resumed_ under G.
    {
        LockGuard lk(global_mtx_);
        if (port.granted_not_resumed_ > 0) --port.granted_not_resumed_;
    }
}

void Scheduler::queue_pop_admit_until(detail::QueuePort& port, WaitNode& node,
                                      detail::QueueItemLease& out,
                                      deadline_t deadline) {
    // P4 timed pop. Symmetric.
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncQueue::pop_until requires a running Fiber");
    Fiber* me = ws->current;
    QueueWaitCtx ctx{&port, detail::QueueRole::consumer, nullptr, nullptr, &out};
    node.set_user(&ctx);
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard slk(port.state_mtx_);
        LockGuard qlk(port.waiters_[1].mtx());
        if (!port.waiters_[1].register_wait_locked(node, me)) {
            return;  // C8
        }
        ++port.active_wait_associations_;
        ++waiting_waitq_count_;
        timer_pool_.emplace_back(&node, &port.waiters_[1], deadline);
        reg = &timer_pool_.back();
        reg->on_resolve_ = &Scheduler::queue_timer_on_resolve;  // F.1/F.2 wiring
        reg->owner_ctx_ = &port;
        ++port.active_queue_timers_;
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();
        if (!port.ring_empty_locked() && node.prev_ == nullptr) {
            const std::size_t head = port.ring_head_;
            out = std::move(port.ring_[head]);
            port.ring_head_ = (port.ring_head_ + 1) % port.capacity_;
            --port.ring_count_;
            out.control_->location_ =
                detail::QueueItemControl::Location::consumer_operation;
            port.waiters_[1].wake_node_locked(node);
            if (reg->retire()) { --active_deadline_count_; }
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/false);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;
        }
        if (port.ring_empty_locked() && port.closed_) {
            port.waiters_[1].wake_node_locked(node);
            if (reg->retire()) { --active_deadline_count_; }
            recompute_earliest_deadline_locked();
            reg->fire_on_resolve_locked(/*timer_won=*/false);
            if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
            if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
            return;
        }
        if (clock_now_unlocked() >= deadline) {
            if (port.waiters_[1].expire_locked(node)) {
                reg->try_claim_expiry();
                --active_deadline_count_;
                recompute_earliest_deadline_locked();
                reg->fire_on_resolve_locked(/*timer_won=*/true);
                if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                return;  // expired; out stays empty
            }
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    // F.2 corrective: this fiber has RESUMED from a suspended-winner
    // publication (or a timer-expiry publication). Decrement
    // granted_not_resumed_ under G.
    {
        LockGuard lk(global_mtx_);
        if (port.granted_not_resumed_ > 0) --port.granted_not_resumed_;
    }
}

bool Scheduler::queue_cancel(detail::QueuePort& port, detail::QueueRole role,
                             WaitNode& node) {
    // Queue-identity-safe cancellation. Mirrors mutex_cancel. Resolves the
    // node Cancelled ONLY if Registered + linked in this port's role FIFO +
    // CANCEL CAS wins. Safe from any OS thread; no ring/lease mutation (the
    // caller retains its lease / empty out on cancel).
    LockGuard lk(global_mtx_);
    const std::size_t roleIdx = static_cast<std::size_t>(role);
    LockGuard qlk(port.waiters_[roleIdx].mtx());
    if (!port.waiters_[roleIdx].contains_locked(node)) return false;
    if (!port.waiters_[roleIdx].cancel_locked(node)) return false;
    retire_timer_for_node_locked(node);
    Fiber* f = node.fiber();
    if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    if (f != nullptr && f->make_runnable()) {
        route_runnable_locked(f, g_worker);
    }
    return true;
}

WaitNode* Scheduler::queue_grant_consumer_locked(detail::QueuePort& port)
    SLUICE_REQUIRES(global_mtx_) {
    // Reconciler (producer-arrived or close-draining): grant the consumer FIFO
    // head the OLDEST ring item, winner-before-publication. Mirrors
    // mutex_handoff_one_locked's resolve -> commit -> retire -> publish order.
    // Caller holds G + S; we take consumer.mtx() here (under G).
    LockGuard qlk(port.waiters_[1].mtx());
    WaitNode* won = port.waiters_[1].wake_one_locked();  // resolve FIFO head Woken + unlink
    if (won == nullptr) return nullptr;  // no consumer parked / head lost
    auto* ctx = static_cast<QueueWaitCtx*>(won->user());
    // ---- retire BEFORE commit (§12 verbatim order; F.6 corrective) ----
    retire_timer_for_node_locked(*won);  // E11 I4 (fires F.2 thunk if Queue timer)
    // ---- resource commit BEFORE publication ----
    if (!port.ring_empty_locked()) {
        const std::size_t head = port.ring_head_;
        *ctx->cons_out = std::move(port.ring_[head]);
        port.ring_head_ = (port.ring_head_ + 1) % port.capacity_;
        --port.ring_count_;
        ctx->cons_out->control_->location_ =
            detail::QueueItemControl::Location::consumer_operation;
    }  // else: ring empty (close race) => leave out empty; caller returns closed
    if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    Fiber* f = won->fiber();
    if (f != nullptr && f->make_runnable()) {  // publication LAST
        ++port.granted_not_resumed_;  // F.2: published suspended-winner ticket
        route_runnable_locked(f, g_worker);
    }
    return won;
}

WaitNode* Scheduler::queue_grant_producer_locked(detail::QueuePort& port)
    SLUICE_REQUIRES(global_mtx_) {
    // Reconciler (consumer-freed-a-slot or close-draining): commit the producer
    // FIFO head's lease into a freed ring slot (or, if Closed, resolve it Woken
    // with the lease retained). Caller holds G + S; we take producer.mtx().
    // The producer admit stashed BOTH the control pointer AND a pointer to its
    // stack lease (QueueWaitCtx::prod_lease) so the grant can move the lease
    // whole into the slot in this critical section.
    LockGuard qlk(port.waiters_[0].mtx());
    WaitNode* won = port.waiters_[0].wake_one_locked();
    if (won == nullptr) return nullptr;
    auto* ctx = static_cast<QueueWaitCtx*>(won->user());
    // ---- retire BEFORE commit (§12 verbatim order; F.6 corrective) ----
    retire_timer_for_node_locked(*won);
    // ---- resource commit BEFORE publication ----
    if (!port.closed_ && !port.ring_full_locked()) {
        detail::QueueItemControl* c = ctx->prod_control;
        c->location_ = detail::QueueItemControl::Location::ring;
        const std::size_t tail =
            (port.ring_head_ + port.ring_count_) % port.capacity_;
        port.ring_[tail] = std::move(*ctx->prod_lease);  // winner lease -> slot
        ++port.ring_count_;
    }  // else: Closed (or race-full) => leave producer's lease retained; the
       // producer resume returns it as closed.
    if (port.active_wait_associations_ > 0) --port.active_wait_associations_;
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    Fiber* f = won->fiber();
    if (f != nullptr && f->make_runnable()) {
        ++port.granted_not_resumed_;  // F.2: published suspended-winner ticket
        route_runnable_locked(f, g_worker);
    }
    return won;
}

bool Scheduler::queue_role_waiters_empty_locked(detail::QueuePort& port)
    SLUICE_REQUIRES(global_mtx_) {
    // P7 teardown precondition query. Caller holds global_mtx_; we take each
    // role mtx() SEQUENTIALLY under G (the canonical G -> exactly-one-role
    // lock order — the two role mutexes are NEVER held together). The
    // QueuePort is not a friend of WaitQueue (only the Scheduler is), so this
    // is the sole authority for "no Queue wait epoch is registered". Both
    // FIFOs must be empty for begin_teardown to perform the irreversible
    // operational -> tearing_down transition.
    {
        LockGuard qlk(port.waiters_[0].mtx());  // producer FIFO
        if (!port.waiters_[0].empty_locked()) return false;
    }
    {
        LockGuard qlk(port.waiters_[1].mtx());  // consumer FIFO
        if (!port.waiters_[1].empty_locked()) return false;
    }
    return true;
}

// ===========================================================================
//
// CONDITION-WAIT-PREPARE combined step + Condition notify/cancel. Mirrors the
// E12-A/B/C seam discipline: the AsyncCondition passes its private Condition
// queue + the bound Mutex's (waiters, owner) BY REFERENCE (it friends the
// AsyncMutex solely for that). The Scheduler is the authoritative Mutex
// state-machine executor: the prepare seam releases the bound Mutex via the ONE
// accepted mutex_handoff_one_locked (no second handoff), and notify/cancel
// touch ONLY Condition-queue state.

WaitOutcome Scheduler::condition_wait_prepare(WaitQueue& cond_waiters,
                                              WaitNode& cond_node,
                                              WaitQueue& mutex_waiters,
                                              Fiber*& owner,
                                              bool& released_mutex) {
    // CONDITION-WAIT-PREPARE (docs §7). One global_mtx_ critical section makes
    // register-Condition-node + release-Mutex + make_waiting ATOMIC w.r.t. every
    // Condition notify/cancel/expire path (which also need global_mtx_). This is
    // the lost-notify closure (docs §6): a notify CANNOT interleave between
    // Condition registration and Mutex release.
    //
    // `released_mutex` mirrors condition_wait_prepare_until: false on the C8
    // registration-failure path (the Mutex is NOT released — the caller retains
    // ownership and runs NO reacquire epoch), true after the Mutex has been
    // released/handed off (the caller MUST run the reacquire epoch). The untimed
    // path has no inline-Expired-at-admission branch, so every other path
    // releases the Mutex.
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncCondition::wait requires a running Fiber");
    Fiber* me = ws->current;
    assert(owner == me && "AsyncCondition::wait by a non-owner Fiber is a "
                          "caller precondition violation");
    {
        LockGuard lk(global_mtx_);
        // Step 1: register the Condition node into the Condition queue. This is
        // a DIFFERENT queue from the Mutex queue (InvNoDualQueueMembership).
        {
            LockGuard qlk(cond_waiters.mtx());
            if (!cond_waiters.register_wait_locked(cond_node, me)) {
                // C8 contract violation (node already registered/terminal). Do
                // NOT release the Mutex; the caller retains ownership. Return
                // the node's (terminal) outcome.
                released_mutex = false;
                return cond_node.outcome();
            }
            ++waiting_waitq_count_;
        }
        // E12-D deterministic phase seam (test variant only): the Condition node
        // is now Registered AND linked in the Condition queue, while the bound
        // Mutex is STILL owned by `me`. A test observing this phase can prove
        // the register-before-release ordering (InvNoLostNotifyWindow / NEG-C8)
        // and that a concurrent notify sees the registered node.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::e12_condition_register_before_handoff);
#endif
        // INTERNALLY (under global_mtx_); the Condition queue mtx was already
        // released above, so the two queue mtxes are NEVER held simultaneously
        // (docs §6.3 — sequential lock topology, no self-deadlock). A nullptr
        // return means the Mutex queue is empty -> owner = nullptr.
        if (mutex_handoff_one_locked(mutex_waiters, owner) == nullptr) {
            owner = nullptr;  // UnlockNoWaiter: no Mutex waiter to hand off to
        }
        // The Mutex has been released/handed off; the caller MUST run the
        // reacquire epoch regardless of the outcome below.
        released_mutex = true;
        // Defense-in-depth: if the Condition node was resolved concurrently
        // (notify/cancel/expire all need global_mtx_, so this cannot happen
        // while this CS holds it, but guard anyway), undo the registration and
        // do NOT suspend. The Mutex has already been released/handed off; the
        // caller will run the reacquire epoch regardless.
        if (cond_node.is_terminal()) {
            return cond_node.outcome();
        }
        // Step 3: commit the calling Fiber to Waiting (inside global_mtx_, so a
        // concurrent resolver's make_runnable is the publication guard).
        me->make_waiting();
    }
    // ONLY context_switch is outside global_mtx_ (mirrors await_wait /
    // mutex_lock). The switch-back target is the calling fiber's ctx; it resumes
    // here after the Condition node resolves (Woken/Expired/Cancelled) and the
    // winner's make_runnable+route publishes it.
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    // The Condition node is terminal+unlinked. The caller (AsyncCondition::wait)
    // latches this outcome and then runs the mandatory reacquire epoch.
    return cond_node.outcome();
}

WaitOutcome Scheduler::condition_wait_prepare_until(WaitQueue& cond_waiters,
                                                    WaitNode& cond_node,
                                                    WaitQueue& mutex_waiters,
                                                    Fiber*& owner,
                                                    deadline_t deadline,
                                                    bool& released_mutex) {
    // Deadline-aware CONDITION-WAIT-PREPARE (docs §10). The deadline governs
    // ONLY the Condition epoch (C-H4). Admission precedence (under global_mtx_):
    //   1. deadline ALREADY due -> resolve Expired INLINE (WaitDueInline): do
    //      NOT release the Mutex, do NOT suspend, do NOT create a reacquire
    //      epoch. The caller RETAINS ownership (InvDueInlineRetainsOwnership).
    //      released_mutex = false.
    //   2. else -> register node + timer, release/handoff Mutex, make_waiting,
    //      context_switch; return the latched outcome. released_mutex = true
    //      (the caller MUST run the reacquire epoch).
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncCondition::wait_until requires a running Fiber");
    Fiber* me = ws->current;
    assert(owner == me && "AsyncCondition::wait_until by a non-owner Fiber is a "
                          "caller precondition violation");
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        {
            LockGuard qlk(cond_waiters.mtx());
            if (!cond_waiters.register_wait_locked(cond_node, me)) {
                // C8 contract violation: do NOT release the Mutex.
                released_mutex = false;
                return cond_node.outcome();
            }
            ++waiting_waitq_count_;
            // Install the E11 timer for the Condition epoch ONLY (C-H4). The
            // registration binds {cond_node, cond_waiters} so a later expiry
            // resolves the Condition node Expired through pump_deadlines_locked.
            timer_pool_.emplace_back(&cond_node, &cond_waiters, deadline);
            reg = &timer_pool_.back();
            ++active_deadline_count_;
            heap_push_ordinary_locked(reg);
            recompute_earliest_deadline_locked();
        }
        // Admission precedence 1: E11 I5 — if the deadline is ALREADY due, the
        // Condition node resolves Expired INLINE. The Mutex is NOT released
        // (the caller retains ownership), the Fiber does NOT suspend, and no
        // reacquire epoch is created. This is WaitDueInline /
        // InvDueInlineRetainsOwnership.
        if (clock_now_unlocked() >= deadline) {
            LockGuard qlk(cond_waiters.mtx());
            if (cond_waiters.expire_locked(cond_node)) {
                reg->try_claim_expiry();  // ACTIVE->CONSUMED (timer winner)
                --active_deadline_count_;
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                if (me != nullptr) (void)me->make_runnable();
                released_mutex = false;  // Mutex NOT released; no reacquire
                return WaitOutcome::expired;  // resolved at admission; do NOT
                                             // release Mutex or suspend
            }
            // If expire_locked lost, a concurrent resolver won; fall through to
            // the terminal-recheck guard (the node is no longer Registered).
        }
        // Step 2: register-before-handoff phase seam (same as the untimed seam).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::e12_condition_register_before_handoff);
#endif
        // Step 3: release the bound Mutex via the ONE accepted handoff.
        if (mutex_handoff_one_locked(mutex_waiters, owner) == nullptr) {
            owner = nullptr;
        }
        // Defense-in-depth: concurrent resolution guard. The Mutex has been
        // released; the caller MUST run the reacquire epoch regardless.
        released_mutex = true;
        if (cond_node.is_terminal()) {
            return cond_node.outcome();
        }
        me->make_waiting();
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    return cond_node.outcome();
}

void Scheduler::condition_notify_one(WaitQueue& cond_waiters) {
    // Resolve the eligible FIFO head of the Condition queue with Woken and
    // publish the winner runnable (Condition-epoch publication). Mirrors
    // wake_wait_one EXACTLY but operates on the Condition queue. The winner
    // subsequently performs its OWN reacquire epoch on resume; this seam does
    // NOT mutate Mutex state. Safe from any OS thread (g_worker may be null:
    // route_runnable_locked handles external-thread routing via pending_spawn_).
    LockGuard lk(global_mtx_);
    (void)wake_wait_one_locked(cond_waiters);
}

std::size_t Scheduler::condition_notify_all(WaitQueue& cond_waiters) {
    // Atomic snapshot-and-drain (C-H10): under one continuous global_mtx_ CS,
    // loop wake_wait_one_locked(cond_waiters) until nullptr. Mirrors
    // event_set_broadcast's drain loop EXACTLY. Each winner resolves Woken
    // exactly once, retires its timer, decrements waiting_waitq_count_, and is
    // published runnable. Waiters registered after the snapshot linearization
    // point are excluded (admission needs global_mtx_, which this holds). The
    // continuous global_mtx_ hold IS the atomic snapshot; no separate snapshot
    // container is needed. Does NOT mutate Mutex state.
    std::size_t woken = 0;
    LockGuard lk(global_mtx_);
    // E12-D deterministic phase seam: global authority acquired, before the
    // drain begins. A test observing this phase can prove late registration /
    // cancel / expiry serialize AFTER the snapshot (they need global_mtx_).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::e12_condition_notify_before_drain);
#endif
    while (wake_wait_one_locked(cond_waiters) != nullptr) {
        ++woken;
    }
    return woken;
}

bool Scheduler::condition_cancel_wait(WaitQueue& cond_waiters, WaitNode& cond_node) {
    // Queue-identity-safe Condition-node cancellation. Mirrors event_cancel_wait
    // / mutex_cancel EXACTLY: the membership gate (contains_locked) is taken
    // BEFORE the resolve CAS so no mutation occurs on a non-member. AsyncCondition
    // passes its private cond_waiters here (NOT exposed to the caller). The
    // contract: returns true ONLY if cond_node is Registered AND linked in
    // cond_waiters AND CANCEL wins. Otherwise returns false WITHOUT mutation.
    // Does NOT change Mutex `owner`. Safe from any OS thread. Safe against
    // wrong-Condition (same/different Scheduler), detached, Woken, Expired, and
    // Cancelled nodes.
    LockGuard lk(global_mtx_);
    LockGuard qlk(cond_waiters.mtx());
    if (!cond_waiters.contains_locked(cond_node)) return false;
    if (!cond_waiters.cancel_locked(cond_node)) return false;  // loser
    retire_timer_for_node_locked(cond_node);
    Fiber* f = cond_node.fiber();
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    if (f != nullptr && f->make_runnable()) {
        route_runnable_locked(f, g_worker);
    }
    return true;
}

std::size_t Scheduler::pump_deadlines_locked() {
    // Drive due timers: for every heap-min whose deadline <= now and whose
    // registration is still ACTIVE, claim it (try_claim_expiry) and resolve its
    // bound node via expire_wait. Lazy-skips retired/consumed entries (removes
    // them from the heap without dereferencing the node). Returns the number of
    // expiries that won the resolve_ CAS.
    //
    // I4 (Timer Lifetime Closure) is the load-bearing property here: a retired
    // registration is observed via its atomic state BEFORE its node pointer is
    // touched. The heap may retain a stale physical entry for a retired timer
    // whose WaitNode has since been destroyed; pump skips it inertly.
    std::size_t won = 0;
    const deadline_t now = clock_now_unlocked();
    while (!deadline_heap_.empty()) {
        const detail::DeadlineHeapEntry front = deadline_heap_.front();
        if (front.deadline > now) break;  // earliest not yet due
        // E13 P3: the deadline heap holds tagged entries (Ordinary | Select).
        // Pop the min regardless of kind (lazy removal: inert entries leave
        // the heap here without their target ever being dereferenced for a
        // non-ACTIVE state). Copy `front` because pop invalidates the ref.
        heap_pop_min_locked();
        if (front.kind == detail::DeadlineHeapEntry::Kind::select) {
            // Select timer branch (Addendum D/E). State-before-arm: the branch
            // body loads state first; non-ACTIVE skips (PumpSkip), ACTIVE
            // fails fast (a due ACTIVE Select entry is unreachable in valid
            // P3 — no admission path). Physical reclamation only here: the
            // retire/consume helper already decremented active_deadline_count_
            // exactly once; the stale-pop path MUST NOT decrement again.
            select_timer_pump_entry_locked(*front.target.select);
            erase_popped_select_registration_locked(front.target.select);
            continue;
        }
        TimerRegistration* top = front.target.ordinary;
        // I4 gate: claim the timer authority BEFORE dereferencing the node. If
        // the registration is RETIRED (non-timer winner closed it) or already
        // CONSUMED (an earlier expiry won), skip — do NOT touch node/queue.
        // The active count was already decremented when the registration was
        // retired/consumed by the non-timer winner path.
        if (!top->try_claim_expiry()) {
            // Inert stale entry, now dropped from the heap. Erase its pool block
            // too so the pool never accumulates dead registrations (the block's
            // node may already be destroyed; erase_popped_registration_locked
            // matches by ADDRESS without reading node/queue — I4-safe).
            erase_popped_registration_locked(top);
            continue;
        }
        --active_deadline_count_;  // ACTIVE->CONSUMED
        // ACTIVE->CONSUMED won: this expiry owns the timer authority. Resolve
        // the bound node through the canonical seam. The {node, queue} pointers
        // are valid: retirement happens only in the non-timer winner's
        // global_mtx_ CS, and we hold global_mtx_ here, so a concurrent retire
        // cannot have flipped the state after our winning claim.
        WaitNode* n = top->node();
        WaitQueue* q = top->queue();
        // Resolve the wait and, regardless of whether the CAS won, erase the
        // consumed block: it is no longer active, and the resolve_ CAS is the
        // publication guard. Erasing keeps the pool bounded by live deadline
        // waits (no accumulation across epochs).
        if (n != nullptr && q != nullptr) {
            // expire_wait re-acquires global_mtx_ + q.mtx(); but we ALREADY hold
            // global_mtx_. Call the resolve path inline (no re-acquire) to avoid
            // a self-deadlock. The registration is already consumed; the resolve
            // CAS is the publication guard.
            LockGuard qlk(q->mtx());
            if (q->expire_locked(*n)) {
                Fiber* f = n->fiber();
                // E12-E Queue (F.1 + F.2): for a Queue-bound registration the
                // pump performs the per-port `--active_wait_associations_`
                // (via owner_ctx) and `--active_queue_timers_` (via the
                // on-resolve thunk) decrements. Non-Queue registrations have
                // no thunk and no owner_ctx; the `--waiting_waitq_count_`
                // Scheduler-wide accounting below applies unchanged.
                if (top->has_on_resolve()) {
                    auto* port = static_cast<detail::QueuePort*>(top->owner_ctx_);
                    if (port != nullptr &&
                        port->active_wait_associations_ > 0) {
                        --port->active_wait_associations_;
                    }
                    top->fire_on_resolve_locked(/*timer_won=*/true);
                }
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                if (f != nullptr && f->make_runnable()) {
                    route_runnable_locked(f, g_worker);
                    ++won;
                }
            }
        }
        erase_popped_registration_locked(top);
    }
    // The pump may have retired/consumed/erased entries; refresh the park-
    // timeout cache so park_on_wake_source sees the new earliest obligation.
    recompute_earliest_deadline_locked();
    return won;
}

void Scheduler::retire_timer_for_node_locked(WaitNode& node) {
    // E11 Phase 5 (Timer Lifetime Closure). Called by the non-timer winner
    // (wake_wait_one / cancel_wait) in the SAME global_mtx_ CS as the resolve
    // CAS, BEFORE runnable publication. Performs ACTIVE->RETIRED on the bound
    // registration's independently-stable state. A later stale expiry then
    // observes RETIRED in pump_deadlines_locked / try_claim_expiry and MUST NOT
    // dereference the node (which may have been destroyed after the fiber
    // resumed).
    //
    // I4-safe scan: only ACTIVE registrations are inspected for a node match.
    // An ACTIVE registration is provably bound to a LIVE, still-Registered node
    // (a node is destroyed only after its wait epoch resolves, and resolving
    // retires/consumes the registration in the SAME CS — so while a block is
    // ACTIVE its bound node has not been destroyed). Inert blocks (bound to a
    // possibly-destroyed node) are skipped WITHOUT reading node(). This is the
    // load-bearing difference: we never read the node() of a block whose node
    // may be gone. The ACTIVE block matching the live `node` is retired.
    for (auto& r : timer_pool_) {
        if (!r.is_active()) continue;  // inert: node may be destroyed; skip (I4)
        if (r.node() == &node) {
            if (r.retire()) {  // ACTIVE->RETIRED
                --active_deadline_count_;
                // E12-E Queue (F.2): a Queue-bound timer's retire decrements
                // the per-port active_queue_timers_ counter via the on-resolve
                // thunk. timer_won=false (the timer LOST — a non-timer winner
                // retired it). Idempotent: the CAS above is the single
                // ACTIVE->terminal transition.
                r.fire_on_resolve_locked(/*timer_won=*/false);
            }
            recompute_earliest_deadline_locked();  // refresh park-timeout cache
            return;
        }
    }
}

bool Scheduler::any_active_deadline_locked() const {
    // True if any registration is still ACTIVE (an unresolved deadline wait).
    // Uses the O(1) active_deadline_count_ maintained across all state
    // transitions. Called by external_wake_possible_locked so a Live run with
    // an active deadline parks (I6) and MW classification treats the deadline
    // as an external-wake source.
    return active_deadline_count_ > 0;
}

bool Scheduler::earliest_active_deadline_locked(deadline_t& out) const {
    // Return the earliest ACTIVE deadline (min-heap front, skipping inert
    // entries). Used to bound park_on_wake_source (I6). The heap is lazily
    // cleaned by pump_deadlines_locked; here we just scan for the min ACTIVE.
    // E13 P3: Select ACTIVE deadlines participate exactly like ordinary ones
    // (docs/e13-select-timer-adapter.md §4.2), so both pools are scanned.
    bool found = false;
    deadline_t best = 0;
    for (const auto& r : timer_pool_) {
        if (!r.is_active()) continue;
        if (!found || r.deadline() < best) {
            best = r.deadline();
            found = true;
        }
    }
    for (const auto& r : select_timer_pool_) {
        if (!r.is_active()) continue;
        if (!found || r.deadline() < best) {
            best = r.deadline();
            found = true;
        }
    }
    if (found) out = best;
    return found;
}

void Scheduler::recompute_earliest_deadline_locked() {
    // Recompute the earliest ACTIVE deadline from the pool and publish it to the
    // atomic cache. Called under global_mtx_ after every heap/pool mutation so
    // park_on_wake_source can read it LOCK-FREE (avoiding a wake_mtx_ ->
    // global_mtx_ lock-order inversion). O(pool); the pool holds at most one
    // entry per concurrent deadline wait.
    // E13 P3: Select ACTIVE deadlines participate exactly like ordinary ones,
    // so both pools are scanned.
    deadline_t best = kNoDeadline;
    bool found = false;
    for (const auto& r : timer_pool_) {
        if (!r.is_active()) continue;
        if (!found || r.deadline() < best) {
            best = r.deadline();
            found = true;
        }
    }
    for (const auto& r : select_timer_pool_) {
        if (!r.is_active()) continue;
        if (!found || r.deadline() < best) {
            best = r.deadline();
            found = true;
        }
    }
    earliest_active_deadline_.store(found ? best : kNoDeadline,
                                    std::memory_order::release);
}

void Scheduler::erase_popped_registration_locked(TimerRegistration* r) {
    // Erase a registration's pool block. SAFE only because the caller
    // (pump_deadlines_locked) has ALREADY popped the block from the deadline
    // heap, so no live heap slot still holds its pointer. The block is
    // non-ACTIVE (retired/consumed) and is erased by ADDRESS match (no
    // node()/queue() read) — I4-safe. O(pool size): the pool holds at most one
    // entry per concurrent deadline wait, so this scan is small.
    if (r == nullptr) return;
    for (auto it = timer_pool_.begin(); it != timer_pool_.end(); ++it) {
        if (&*it == r) {
            timer_pool_.erase(it);
            return;  // r is now dangling; caller must not touch it
        }
    }
}

TimerRegistration* Scheduler::register_test_deadline_locked(WaitNode* node,
                                                            WaitQueue* q,
                                                            deadline_t deadline) {
    // E11-T17 (F2) narrow test hook. Creates an ACTIVE TimerRegistration for
    // {node, q, deadline}, pushes it into the deadline heap, refreshes the
    // earliest-deadline park cache, AND registers `node` into `q` (Detached ->
    // Registered) so the pump's expire path can resolve it. This mirrors the
    // full await_wait_deadline admission MINUS the fiber-suspend path: it
    // increments waiting_waitq_count_ (so the pump's decrement on win balances)
    // and registers the node, but does NOT suspend a fiber, so the coordinator
    // can install a NEW deadline from a NON-worker thread while the worker is
    // held at the park-commit seam (global_mtx_ is released at that seam).
    // Called by the test coordinator. See tests/e11_timer_wait_test.cpp T17.
    // TEST-ONLY; no production caller.
    if (clock_now_unlocked() >= deadline) return nullptr;  // already due: skip
    if (q != nullptr) {
        LockGuard qlk(q->mtx());
        if (!q->register_wait_locked(*node, nullptr)) return nullptr;  // not Detached
    }
    ++waiting_waitq_count_;  // mirror admission accounting (pump decrements on win)
    timer_pool_.emplace_back(node, q, deadline);
    TimerRegistration* reg = &timer_pool_.back();
    ++active_deadline_count_;
    heap_push_ordinary_locked(reg);
    recompute_earliest_deadline_locked();  // publish to the park-timeout cache
    return reg;
}

// ---- deadline heap helpers (min-heap on deadline) ----
// E13 P3: the heap stores unified DeadlineHeapEntry values (Ordinary | Select).
// The comparator (detail::heap_less_entry) compares cached deadlines only;
// equal-deadline order is unspecified. sift/pop operate on vector entries and
// no longer touch any registration's heap_index (the entry's vector position
// is the sole position authority — Addendum G).

void Scheduler::heap_push_entry_locked(const detail::DeadlineHeapEntry& e) {
    deadline_heap_.push_back(e);
    heap_sift_up_locked(deadline_heap_.size() - 1);
}

void Scheduler::heap_push_ordinary_locked(TimerRegistration* r) {
    heap_push_entry_locked(detail::DeadlineHeapEntry::for_ordinary(*r));
}

void Scheduler::heap_pop_min_locked() {
    if (deadline_heap_.empty()) return;
    detail::DeadlineHeapEntry last = deadline_heap_.back();
    deadline_heap_.pop_back();
    if (!deadline_heap_.empty()) {
        deadline_heap_[0] = last;
        heap_sift_down_locked(0);
    }
}

void Scheduler::heap_sift_up_locked(std::size_t i) {
    while (i > 0) {
        std::size_t parent = (i - 1) / 2;
        if (!detail::heap_less_entry(deadline_heap_[i], deadline_heap_[parent])) break;
        std::swap(deadline_heap_[i], deadline_heap_[parent]);
        i = parent;
    }
}

void Scheduler::heap_sift_down_locked(std::size_t i) {
    const std::size_t n = deadline_heap_.size();
    while (true) {
        std::size_t l = 2 * i + 1;
        std::size_t r = 2 * i + 2;
        std::size_t best = i;
        if (l < n && detail::heap_less_entry(deadline_heap_[l], deadline_heap_[best])) best = l;
        if (r < n && detail::heap_less_entry(deadline_heap_[r], deadline_heap_[best])) best = r;
        if (best == i) break;
        std::swap(deadline_heap_[i], deadline_heap_[best]);
        i = best;
    }
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
        LockGuard lk(global_mtx_);
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
        LockGuard lk(global_mtx_);
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

    LockGuard lk(global_mtx_);
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

// ----------------------------------------------------------------------------
// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: AsyncTestAccess definitions.
// Compiled ONLY in the internal-testing variant. These are thin pass-throughs to
// the dual-use production timer state; they exist so the non-installed test
// controller can drive the clock/observe the pool WITHOUT a forgeable friend.
// ----------------------------------------------------------------------------
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
TimerRegistration* Scheduler::AsyncTestAccess::register_test_deadline(
    Scheduler& s, WaitNode* node, WaitQueue* q, deadline_t deadline) {
    LockGuard lk(s.global_mtx_);
    return s.register_test_deadline_locked(node, q, deadline);
}

// Test-coordinator diagnostic observation. Reads GUARDED_BY fields without the
// lock; the sizes are not load-bearing for correctness (test diagnostics only).
std::size_t Scheduler::AsyncTestAccess::timer_pool_size(
    const Scheduler& s) noexcept SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    return s.timer_pool_.size();
}

std::size_t Scheduler::AsyncTestAccess::deadline_heap_size(
    const Scheduler& s) noexcept SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    return s.deadline_heap_.size();
}

std::size_t Scheduler::AsyncTestAccess::active_deadline_count(
    const Scheduler& s) noexcept SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    return s.active_deadline_count_;
}

std::size_t Scheduler::AsyncTestAccess::timer_pool_count_in_state(
    const Scheduler& s, TimerRegistration::State st) noexcept
    SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    std::size_t n = 0;
    for (const auto& r : s.timer_pool_) {
        if (r.state() == st) ++n;
    }
    return n;
}

bool Scheduler::AsyncTestAccess::earliest_active_deadline(
    Scheduler& s, deadline_t& out) {
    LockGuard lk(s.global_mtx_);
    return s.earliest_active_deadline_locked(out);
}

// ---- E13 P3 Select timer test accessors ----

void Scheduler::AsyncTestAccess::advance_clock(Scheduler& s, deadline_t t) {
    s.advance_clock(t);
}

std::size_t Scheduler::AsyncTestAccess::select_timer_pool_size(
    const Scheduler& s) noexcept SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    return s.select_timer_pool_.size();
}

std::size_t Scheduler::AsyncTestAccess::select_timer_count_in_state(
    const Scheduler& s,
    detail::SelectTimerRegistration::State st) noexcept
    SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    std::size_t n = 0;
    for (const auto& r : s.select_timer_pool_) {
        if (r.state() == st) ++n;
    }
    return n;
}

std::array<std::size_t, 2>
Scheduler::AsyncTestAccess::tagged_heap_counts_by_kind(
    const Scheduler& s) noexcept SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    std::array<std::size_t, 2> counts{0, 0};
    for (const auto& e : s.deadline_heap_) {
        if (e.kind == detail::DeadlineHeapEntry::Kind::ordinary) {
            ++counts[0];
        } else {
            ++counts[1];
        }
    }
    return counts;
}

// E13 P3 Corrective (closure 4): prove the heap stores the spliced block's
// address as its stable Select target. Diagnostic only (reads GUARDED_BY
// fields from the test coordinator).
bool Scheduler::AsyncTestAccess::deadline_heap_has_select_target(
    const Scheduler& s,
    const detail::SelectTimerRegistration* target) noexcept
    SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    for (const auto& e : s.deadline_heap_) {
        if (e.kind == detail::DeadlineHeapEntry::Kind::select &&
            e.target.select == target) {
            return true;
        }
    }
    return false;
}
#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)

}  // namespace sluice::async
