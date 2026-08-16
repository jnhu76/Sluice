// Implementation of the multi-worker Scheduler (sluice-CORE-E7-A).
//
// E7-A: worker-local execution state + multi-worker run skeleton.
// See docs/adr/ADR-execution-model.md §9.2.
//
// This commit localizes sched_ctx + current_ into per-Worker WorkerState and
// adds run(worker_count). E7-B adds pinned routing; E7-C adds MW coordination.
// Post-freeze R1 (docs/post-freeze/structural-audit.md §6): this TU owns
// Scheduler lifecycle, the run loop/workers/steal/routing, and AsyncTestAccess.
// Sibling scheduler_*.cpp TUs own park/wake + waits, timer/deadline, and the
// mutex/condition/semaphore/rwlock/event/queue implementations — pure
// relocation, no semantic change.
// For E7-A, the multi-worker path uses a simple model: all spawned Fibers go
// into pending_spawn_; workers pick from it round-robin; backend progress is
// done by worker 0 only (single-driver for E7-A; E7-C generalizes); the
// coordinated run returns when no runnable Fiber remains and no Completion
// is outstanding. This is sufficient to prove worker-local execution state
// (E7-T1/T2) and preserve single-worker regression (E4-E6).
#include <sluice/async/scheduler.hpp>

#include <sluice/async/async_rwlock.hpp>  // E12-F ExpireCtx (pump routing)
#include <sluice/async/select.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "scheduler_internal.hpp"  // Wake Control + worker TLS (shared across Scheduler TUs)

#include <utility>
#include <cstdio>   // Phase G park-window forensics dump (internal testing)
#include <cstdlib>  // std::abort (E12-F Category B fail-fast)

// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the internal-testing variant pulls in
// the non-installed test-control header so the phase call sites below resolve to
// the controller. In the production build this include is absent and the call
// sites compile to nothing.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {

namespace {

// g_worker moved to scheduler_internal.hpp (inline thread_local, shared TLS).

void fiber_entry_bridge(fiber_ctx::Switch* resumed_by, void* user_data) {
    (void)resumed_by;
    auto* fiber = static_cast<Fiber*>(user_data);
    if (fiber->entry()) {
        fiber->entry()(*fiber);
    }
    fiber->make_done();
    // Switch back to this worker's scheduler context forever.
    fiber_ctx::context_switch_final(fiber->ctx, g_worker->sched_ctx);
}

}  // namespace


// Phase F1: per-Scheduler identity for waiter tokens (0 reserved — the
// default WaiterToken is all-zeros and must never match a live Scheduler).
namespace {
std::uint64_t next_scheduler_identity() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return ++counter;
}
}  // namespace

Scheduler::Scheduler(AsyncIoContext& ctx, std::size_t wait_capacity)
    : ctx_(ctx),
      wait_capacity_(wait_capacity == 0 ? 1 : wait_capacity),
      scheduler_identity_(next_scheduler_identity()) {
    // E14 D-E14-2: construction-time fail-fast on unsupported targets.
    // Scheduler is the earliest Evented admission boundary; all downstream
    // primitives (E12 Event/Semaphore/Mutex/Condition/Queue/RwLock, Select,
    // Group(Scheduler&), EventedWaitPolicy) require a Scheduler reference.
    // Guarding here covers the complete Evented public admission surface.
    detail::require_evented_supported(detail::evented_admission_check());
    // E9: create the wake control block. Every issued SchedulerWakeHandle
    // holds a shared_ptr to it; the Scheduler holds a shared_ptr too so the
    // block outlives the Scheduler's stack locals. A handle's notify() detects
    // post-destruction via the Control::alive flag (protected by Control::mtx,
    // the callback lease) and returns false without any Scheduler dereference.
    // See Control below and docs/spec/e9_wake_handle_lifetime/.
    wake_control_ = std::make_shared<SchedulerWakeHandle::Control>();

    // Phase F1 P1-2: preallocate the WaitRecord pool to exactly
    // wait_capacity_ records. Once constructed, wait_records_.size() == N and
    // never grows. All N records start in the free list with generation 0.
    // The pool is address-stable (unique_ptr per element) and allocation-free
    // on the hot path (only free-list pops/pushes).
    //
    // Generation protocol: generation starts at 0 for every record. When a
    // record is reused from the free list, acquire_wait_record_locked bumps
    // generation BEFORE making the new occupant visible (I6). This ensures
    // that a stale WaiterToken from occupant K cannot match occupant K+1,
    // even though every record started with the same initial generation.
    // No pointer identity alone is authority (I6).
    wait_records_.reserve(wait_capacity_);
    for (std::size_t i = 0; i < wait_capacity_; ++i) {
        auto rec = std::make_unique<WaitRecord>();
        rec->index = static_cast<std::uint32_t>(i);
        // generation = 0 (default); next_free = nullptr (linked below)
        wait_records_.push_back(std::move(rec));
    }
    // Link all records into the free list in FIFO order (record 0 at head).
    // The free list is intrusive (WaitRecord::next_free), so the drain/route
    // path never allocates (I9). FIFO order ensures the first acquire returns
    // record 0, matching the old on-demand creation behavior where record 0
    // was always the first created.
    WaitRecord** tail = &wait_record_free_head_;
    for (std::size_t i = 0; i < wait_capacity_; ++i) {
        WaitRecord* r = wait_records_[i].get();
        *tail = r;
        tail = &r->next_free;
    }
    *tail = nullptr;

    // Phase F1 (issue #98): install the Scheduler-owned identity-bearing
    // ReadySink on the context so every backend reap delivers by-value
    // ReadyEvents here instead of to the no-op reference sink (ADR Decision
    // 9). Detached in ~Scheduler. No workers exist yet, so the attachment
    // cannot race a reap.
    ctx_.set_ready_sink(&ready_sink_);
}

Scheduler::~Scheduler() {
    // Phase F1: detach the routing sink BEFORE this Scheduler's address can
    // be referenced again. ApplicationRuntime destroys sched_ before io_ctx_
    // (close_resources: group.reset(); sched.reset(); io_ctx.reset();), and
    // workers are joined before ~Scheduler, so no reap can be in flight; a
    // later poll on the same context delivers to the no-op reference sink.
    ctx_.set_ready_sink(nullptr);

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
    [[maybe_unused]] bool any_active_select = false;
    for (auto& r : select_timer_pool_) {
        if (r.is_active()) { any_active_select = true; break; }
    }
    assert(!any_active_select &&
           "~Scheduler: an ACTIVE SelectTimerRegistration remains (live Select "
           "timer authority not closed — caller contract violation)");
    assert(active_deadline_count_ == 0 &&
           "~Scheduler: active_deadline_count_ != 0 (a timer registration was "
           "not retired/consumed before teardown)");
    // E13 P6-C1 (P1-2 corrective): the suspended-Select quiescence invariant.
    // An Event-only suspended Select (no active Timer, no ordinary WaitQueue,
    // no backend op) escapes BOTH checks above — active_deadline_count_==0 and
    // no ACTIVE SelectTimerRegistration — yet it leaves a live SelectGroup
    // Armed, an intrusive SelectArmSlot in the Event's SelectPort, and an
    // unfulfilled runnable obligation. waiting_select_count_ is the minimal
    // accounting that closes this gap: it was incremented at the no-ready
    // suspension commit and decremented exactly once at suspended publication,
    // so a non-zero value at teardown means a suspended caller was abandoned.
    // Read without global_mtx_ (single-threaded post-run-join, matching the
    // two asserts above). Debug-only; absent in release (NDEBUG).
    assert(waiting_select_count_ == 0 &&
           "~Scheduler: waiting_select_count_ != 0 (a suspended SelectGroup "
           "remains Armed — an Event-only Select with no active Timer escapes "
           "the timer teardown checks; caller contract violation)");
    // Phase F1: the wait registry must be quiescent — every registered waiter
    // was delivered (drained) or cancelled before destruction. A leftover
    // record means a suspended Fiber's wake obligation was abandoned.
    {
        LockGuard rlk(wait_registry_mtx_);
        assert(wait_record_live_count_ == 0 &&
               "~Scheduler: wait_record_live_count_ != 0 (a registered "
               "Completion waiter was neither delivered nor cancelled — "
               "abandoned wake obligation)");
        if (wait_record_live_count_ != 0) {
            detail::scheduler_wait_registry_nonempty_fail_fast();
        }
    }
    if (any_active_select || active_deadline_count_ != 0 ||
        waiting_select_count_ != 0) {
        // Destruction cannot safely recover live caller-frame Select authority,
        // and destructors must not throw. Preserve the debug diagnostics above
        // while enforcing the same fail-fast contract in NDEBUG builds.
        detail::select_invariant_fail_fast();
    }
}

bool Scheduler::init_fiber(Fiber& fiber, std::byte* stack_base, std::size_t stack_size) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // E14 RT-F3: one-shot failure injection. If armed, consume the flag and
    // return false WITHOUT calling fiber_ctx::init_context. This simulates
    // an invalid stack or unsupported architecture deterministically.
    if (force_init_fiber_fail_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
#endif
    return fiber_ctx::init_context(fiber.ctx, &fiber_entry_bridge, &fiber,
                                   stack_base, stack_size);
}

void Scheduler::spawn(Fiber& fiber) noexcept {
    // E7-T2 exactly-once: publish a runnable ticket ONLY if the created->runnable
    // transition succeeded. (spawn's source state is always 'created', so this
    // only fails if spawn is called twice — defensive.)
    if (!fiber.make_runnable())
        return;
    // Round-robin assignment to worker local queues so that Fibers distribute
    // across this invocation's published participants (required for E7-T1/T2
    // concurrency tests). With no active run, use pending_spawn_; the next
    // run() assigns those tickets across its first-N snapshot.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
                                  sluice_async_test::PhaseTag::worker_topology_reader_attempt);
#endif
    LockGuard lk(global_mtx_);
    const unsigned participant_count = active_worker_count_.load(std::memory_order_acquire);
    if (participant_count != 0 &&
        !global_terminate_.load(std::memory_order_acquire)) {
        unsigned target = next_spawn_worker_++ % participant_count;
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
    if (!fiber.make_runnable())
        return;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
                                  sluice_async_test::PhaseTag::worker_topology_reader_attempt);
#endif
    LockGuard lk(global_mtx_);
    const unsigned participant_count = active_worker_count_.load(std::memory_order_acquire);
    if (participant_count == 0 ||
        global_terminate_.load(std::memory_order_acquire) ||
        worker_id >= participant_count) {
        // No active run, or the requested WorkerState is retained but does not
        // participate in this invocation. Defer owner assignment to the next
        // run's first-N topology.
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

void Scheduler::run_live(unsigned worker_count, bool (*stop_fn)(void*), void* stop_ctx) {
    // E14-F1: Group-scoped Live invocation. The stop predicate is checked at
    // the MW-S3+Live+external_wake boundary; if it returns true the run
    // terminates instead of parking. This prevents unrelated Scheduler
    // registrations from permanently blocking a Group::await() whose own
    // Futures are already terminal.
    invocation_stop_fn_ = stop_fn;
    invocation_stop_ctx_ = stop_ctx;
    run_impl(worker_count, RunMode::live);
    invocation_stop_fn_ = nullptr;
    invocation_stop_ctx_ = nullptr;
}

void Scheduler::run_impl(unsigned worker_count, RunMode mode) {
    if (worker_count == 0)
        worker_count = 1;
    run_mode_ = mode; // stable for the duration of this run invocation

    WorkerSnapshot run_workers;
    run_workers.reserve(worker_count);
    {
        LockGuard lk(global_mtx_);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(*this, sluice_async_test::PhaseTag::worker_topology_mutation);
#endif
        ensure_workers_locked(worker_count, run_workers);

        // Reset only this invocation's participants. A prior run() may have
        // left sched_ctx holding native-stack pointers valid only for that
        // invocation. Each worker re-saves sched_ctx before a Fiber resumes.
        for (WorkerState* worker : run_workers) {
            fiber_ctx::reset_context(worker->sched_ctx);
        }

        // Move the pre-run domain into the now-published first-N topology
        // without exposing a partially established WorkerState to spawn().
        unsigned target = 0;
        while (!pending_spawn_.empty()) {
            Fiber* fiber = pending_spawn_.front();
            pending_spawn_.pop_front();
            WorkerState* worker = run_workers[target % worker_count];
            std::lock_guard<std::mutex> wlk(worker->inbox_mtx);
            worker->local_runnable.push_back(fiber);
            fiber_owner_[fiber] = worker;
            worker->inbox_cv.notify_one();
            ++target;
        }
        next_spawn_worker_ = 0;
        admission_ = AdmissionState::none;
        admission_owner_ = static_cast<unsigned>(-1);
        running_fiber_count_.store(0, std::memory_order_release);
        idle_workers_.store(0, std::memory_order_release);
        global_terminate_.store(false, std::memory_order_release);
        in_coordinated_run_ = true;
        active_worker_count_.store(worker_count, std::memory_order_release);
        live_loop_workers_ = worker_count;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
                                  sluice_async_test::PhaseTag::worker_topology_ready_before_start);
#endif
    // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the test-controlled causal seam
    // state (E7 admission, E9 park, E12 event) no longer lives on Scheduler; it
    // is driven by the internal-testing controller and persists across the
    // run() boundary by construction (the controller registry is external).

    if (worker_count == 1) {
        // Single-worker fast path: run inline (no thread spawn). This preserves
        // the E4-E6 behavior exactly — run_until_idle on the caller's thread.
        WorkerState* worker = run_workers[0];
        g_worker = worker;
        worker->owner_scheduler = this; // E13 P5 caller-validation identity
        worker->active.store(true, std::memory_order_release);
        worker->idle_dance_contributed_.store(0, std::memory_order_release);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        // Fresh invocation: the death evidence describes THIS invocation's
        // thread only (a prior run's sticky loop_exited would misreport a
        // live re-entered worker as dead — the dump and the reproducer's
        // causal death observation both read it cross-thread).
        worker->loop_exit_reason.store(WorkerState::LoopExitReason::live,
                                       std::memory_order_relaxed);
        worker->loop_exited.store(false, std::memory_order_relaxed);
#endif
        worker_loop(worker, run_workers);
        worker->active.store(false, std::memory_order_release);
        g_worker = nullptr;
    } else {
        // Multi-worker: spawn OS threads from the immutable invocation snapshot.
        std::vector<std::thread> threads;
        threads.reserve(worker_count);
        for (WorkerState* worker : run_workers) {
            threads.emplace_back([this, worker, &run_workers] {
                g_worker = worker;
                worker->owner_scheduler = this; // E13 P5 caller-validation identity
                worker->active.store(true, std::memory_order_release);
                worker->idle_dance_contributed_.store(0, std::memory_order_release);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                worker->loop_exit_reason.store(
                    WorkerState::LoopExitReason::live, std::memory_order_relaxed);
                worker->loop_exited.store(false, std::memory_order_relaxed);
#endif
                worker_loop(worker, run_workers);
                worker->active.store(false, std::memory_order_release);
                g_worker = nullptr;
            });
        }
        for (auto& thread : threads) {
            if (thread.joinable())
                thread.join();
        }
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::worker_topology_joined_before_unpublish);
#endif
    {
        LockGuard lk(global_mtx_);
        in_coordinated_run_ = false;
        active_worker_count_.store(0, std::memory_order_release);
    }
}

void Scheduler::ensure_workers_locked(unsigned worker_count, WorkerSnapshot& run_workers) {
    // WorkerState is address-stable across run() calls (wait registrations may
    // hold WorkerState* pointers between calls — E7-ABORT-6 lifetime). Grow as
    // needed, but never shrink or destroy/recreate an existing pointee.
    while (workers_.size() < worker_count) {
        workers_.push_back(std::make_unique<WorkerState>());
        workers_.back()->id = static_cast<unsigned>(workers_.size() - 1);
    }
    for (unsigned i = 0; i < worker_count; ++i) {
        run_workers.push_back(workers_[i].get());
    }
}

void Scheduler::worker_loop(WorkerState* ws, const WorkerSnapshot& run_workers) {
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
    //   5. No work and not elected: park on the unified wake source.
    while (true) {
        // R4: conservative contribution reset (see the WorkerState field
        // contract). Clearing here — rather than at every count erase —
        // keeps the park-commit exemption sound in one place: an
        // under-clear (the count was erased externally while this worker
        // slept) only makes the next park commit treat the worker's own
        // stale count as another dancer's (refuse once more, re-dance,
        // converge); it can never suppress a needed refusal.
        ws->idle_dance_contributed_.store(0, std::memory_order_release);
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
                // Re-record the owner: a retire-seeded ticket carries its
                // DEAD original owner (the retire moves the ticket, not the
                // ownership record). Without this, the fiber's next wait
                // resolution routes it back onto the terminated worker's
                // inbox (route_runnable_locked), turning every retire-rescue
                // into a stranded ticket that only a later steal happens to
                // recover (adversarial-review finding: the route-to-dead-
                // worker class G1 repairs must not survive as a designed-in
                // extra hop).
                fiber_owner_[f] = ws;
            }
        }

        // E8: if this worker has no local work, try to steal a runnable
        // Fiber from another worker before falling through to the idle/
        // admission path. Steal is MOVE + OWNER TRANSFER; on success a
        // runnable ticket now sits on ws->local_runnable owned by ws, so
        // loop back and pop it. try_steal is a no-op if there is only one
        // worker or no other worker has runnable work.
        if (!f && run_workers.size() > 1) {
            if (try_steal(ws, run_workers)) {
                continue; // stolen ticket is on ws->local_runnable; pop next iteration
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
            (void)drain_routed_completion_waits_locked();
            (void)wake_ready_flags_locked();
            (void)pump_deadlines_locked();
            state = classify_locked(run_workers, ws);
        }

        // If drain produced routed work, the owning worker will pick it up
        // next iteration; for this worker, fall through to state handling.

        if (state == MwState::mw_s1) {
            // Runnable/running work exists somewhere (possibly routed by the
            // drain to another worker, or another worker is running a Fiber).
            // This worker has no local runnable work (f was null at top), so it
            // must NOT busy-spin — that starves other workers on a contended
            // core. Fall through to the unified wake-domain park below; the
            // owning worker's route signals the wake source (route_runnable
            // publishes under global_mtx_ + signal_wake_locked), so the parked
            // worker re-checks without polling.
            idle_workers_.store(0, std::memory_order_release);
            if (global_terminate_.load(std::memory_order_acquire)) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                ws->loop_exit_reason =
                    WorkerState::LoopExitReason::mw_s1_terminate_observed;
#endif
                break;
            }
            // Fall through to park (no continue).
        }

        if (state == MwState::mw_s2) {
            // MW-S2: backend progress pending, no Fiber can execute. At most
            // one participant may enter wait_one. Two-phase admission.
            //
            // Phase A: under global_mtx_, elect this worker as candidate if
            //          no admission is in progress and this is the lowest-id
            //          ALIVE worker (deterministic election). G1 repair
            //          (§8.3 "worker-0-only election"): the pre-fix
            //          `ws->id == 0` restriction left a run with NO possible
            //          participant once worker 0's thread had exited through
            //          the no-progress terminate — the surviving workers
            //          classify mw_s2, cannot elect, and park unguarded in
            //          the wake domain while the backend completion has no
            //          observer (the no-participant manifestation). The
            //          election is now transferable: the lowest-id worker
            //          whose thread is still in the loop claims it. With all
            //          workers alive this remains worker 0 (unchanged
            //          behavior for every existing test construction).
            bool elected = false;
            {
                LockGuard lk(global_mtx_);
                // Re-classify under the lock — state may have changed since
                // the unlocked classify above.
                if (classify_locked(run_workers, ws) == MwState::mw_s2 &&
                    admission_ == AdmissionState::none) {
                    unsigned lowest_alive = static_cast<unsigned>(-1);
                    for (WorkerState* w : run_workers) {
                        if (w->active.load(std::memory_order_acquire) &&
                            w->id < lowest_alive) {
                            lowest_alive = w->id;
                        }
                    }
                    if (ws->id == lowest_alive) {
                        admission_ = AdmissionState::candidate;
                        admission_owner_ = ws->id;
                        elected = true;
                    }
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
                                              sluice_async_test::PhaseTag::mw_admission_phase_b);
#endif

                bool phase_b_committed = false;
                // Phase G (E5-A2): level-triggered ready-flag waits are
                // poll-observed — a bare flag store is a legal producer and the
                // notify is only a promptness optimization (the park-time window
                // can absorb it). Captured under global_mtx_ at the commit so
                // the backend-domain park below stays bounded at the E9
                // observation interval while such waits are registered.
                bool ready_flag_observation = false;
                {
                    LockGuard lk(global_mtx_);
                    ready_flag_observation = !waiting_ready_.empty();
                    // Demoted by a concurrent route? Then abandon admission.
                    if (admission_ != AdmissionState::candidate || admission_owner_ != ws->id) {
                        // Another path cancelled us. Loop.
                        continue;
                    }
                    // Re-drain readiness + reclassify.
                    (void)drain_routed_completion_waits_locked();
                    (void)wake_ready_flags_locked();
                    (void)pump_deadlines_locked();
                    MwState s2 = classify_locked(run_workers, ws);
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
                    // Phase G (P5-CORRECTIVE): capture the park-domain decision
                    // NOW (under global_mtx_). A SPLIT-WAIT backend (ThreadPool /
                    // Uring — wait_source() != null) parks the participant in
                    // ctx_.wait_one() for BOTH backend-only and MIXED-WAKE: its
                    // own progress transport is prompt (ready epoch / ring fd),
                    // and external-ready publications reach the park through the
                    // interrupt bridge in signal_wake_locked (the E9 GAP-2
                    // closure, now on the backend domain). A NON-split-wait
                    // (reference/legacy) backend keeps the E9 rule: MIXED-WAKE
                    // parks on the SCHEDULER wake domain (bounded observation),
                    // backend-only parks in wait_one(). Refinement map: TLA+
                    // EnterPhysicalPark domain selection.
                    //
                    // Phase G review P1b (PR #108): a FINITE backend-domain
                    // park cap (an active deadline or a registered
                    // level-triggered ready-flag wait) is honor-able ONLY when
                    // the wait source implements the bounded transport
                    // (BackendWaitSource::supports_bounded_wait) — the base
                    // wait_for_change overload silently discards `max_park`.
                    // When a bound is needed but unsupported, do NOT park in
                    // the backend domain on the unbounded contract: fall back
                    // to the SCHEDULER wake domain, whose cv timeout the
                    // Scheduler itself bounds (the E9 observation interval for
                    // MIXED-WAKE / poll-driven backends). Deadline liveness
                    // (E11 I6) never depends on a capability the backend does
                    // not truthfully expose.
                    const bool split_wait = ctx_.has_split_wait_capability();
                    const bool bounded_park_needed =
                        ready_flag_observation ||
                        earliest_active_deadline_.load(std::memory_order::acquire) !=
                            kNoDeadline;
                    const bool backend_park_ok =
                        split_wait &&
                        (!bounded_park_needed ||
                         ctx_.has_bounded_split_wait_capability());
                    ws->park_domain =
                        backend_park_ok
                            ? WorkerState::ParkDomain::Backend
                            : (!split_wait && !external_wake_possible_locked()
                                   ? WorkerState::ParkDomain::Backend
                                   : WorkerState::ParkDomain::Scheduler);
                    // D4-RM14 (P0-1, commit-to-park handshake): register the
                    // mandatory control-observation baseline with the backend
                    // wait source NOW — under the admission authority, BEFORE
                    // the backend-park commitment is exposed and global_mtx_
                    // is released. A runtime stop (request_stop ->
                    // interrupt_backend_waiters) landing between this commit
                    // and the participant's ctx_.wait_one() entry would
                    // otherwise be rebaselined into the entry snapshot as a
                    // past event: the participant would park in the BACKEND
                    // domain (which the Scheduler wake domain cannot
                    // interrupt) and, with backend I/O that never completes,
                    // the run could never reach its stop-predicate boundary —
                    // drain_complete_ unreachable (shutdown liveness). The
                    // armed baseline makes the upcoming wait_one() observe the
                    // interrupt and return (register -> publish -> release ->
                    // wait with the registered baseline). Phase G: the arm now
                    // also covers the MIXED-WAKE case (split-wait backends park
                    // in the backend domain), and the bridge gate is set in the
                    // same critical section so Scheduler wake publications
                    // interrupt this park from the moment the commit is
                    // exposed.
                    if (ws->park_domain == WorkerState::ParkDomain::Backend) {
                        ctx_.arm_backend_wait_commit();
                        backend_wait_active_.store(true, std::memory_order_release);
                    }
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
                        ws->park_domain = WorkerState::ParkDomain::None; // reset before park
                        // Phase G: this is the MIXED-WAKE park for a
                        // NON-split-wait backend (reference/legacy) — its
                        // poll-driven readiness is observed through the 2ms
                        // bounded observation interval (the E9 behavior,
                        // retained only for these backends). Split-wait
                        // backends park in ctx_.wait_one() (the BACKEND
                        // branch) and never take this path.
                        park_on_wake_source(ws, /*bounded_backend_observation=*/true);
                        // Phase D: reacquire global_mtx_, clear admission, drain.
                        {
                            LockGuard lk(global_mtx_);
                            admission_ = AdmissionState::none;
                            admission_owner_ = static_cast<unsigned>(-1);
                            (void)drain_routed_completion_waits_locked();
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

                    // BACKEND domain: the unified progress park (backend-only
                    // AND MIXED-WAKE for split-wait backends; backend-only for
                    // non-split-wait backends). Backend progress is prompt via
                    // the backend's own wait transport; Scheduler wake
                    // publications reach this park through the bridge in
                    // signal_wake_locked (gated by backend_wait_active_, which
                    // was set at the commit above).
                    ws->park_domain = WorkerState::ParkDomain::Backend;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                    // D4-RM14 detector seam: the committed participant paused
                    // between its commit-to-park registration (the arm above,
                    // under global_mtx_) and entering ctx_.wait_one(). A test
                    // injects request_stop() here; on release the armed
                    // baseline must make the upcoming wait_one() observe the
                    // interrupt (return 0), the run terminate, and the driver
                    // re-enter — the pre-fix code rebaselines the stop into
                    // the entry snapshot and the participant parks forever.
                    // Runs OUTSIDE global_mtx_ (released at Phase B block
                    // end). Compiled out of production builds.
                    sluice_async_test::test_phase(
                        *this,
                        sluice_async_test::PhaseTag::mw_s2_committed_before_wait_one);
#endif
                    // Phase G (E11 deadline pump): bound the wait_one park by
                    // the earliest active deadline so pump_deadlines_locked
                    // re-drains in time — the MIXED-WAKE participant no longer
                    // observes deadlines through the wake-domain park timeout.
                    // No deadline -> unbounded park (the classic behavior).
                    // Test clock mode -> the short real-time poll that drives
                    // the logical-clock pump deterministically.
                    // Phase G (E5-A2): while level-triggered ready-flag waits
                    // are registered, cap the park at the E9 observation
                    // interval (their poll-observed resolution; see the
                    // capture at the Phase-B commit).
                    auto max_park = std::chrono::nanoseconds::max();
                    {
                        deadline_t earliest =
                            earliest_active_deadline_.load(std::memory_order::acquire);
                        if (earliest != kNoDeadline) {
                            if (test_clock_mode_.load(std::memory_order::acquire)) {
                                max_park = std::chrono::milliseconds(1);
                            } else {
                                deadline_t now_ticks = clock_now_unlocked();
                                if (earliest <= now_ticks) {
                                    max_park = std::chrono::nanoseconds::zero();
                                } else {
                                    max_park = std::chrono::milliseconds(
                                        earliest - now_ticks);
                                }
                            }
                        }
                        if (ready_flag_observation) {
                            const auto observation =
                                std::chrono::milliseconds(2);  // E9 interval
                            if (max_park == std::chrono::nanoseconds::max() ||
                                max_park > observation) {
                                max_park = observation;
                            }
                        }
                    }
                    // Phase G review P1b (defensive clamp): a deadline can
                    // become active between the Phase-B commit decision (which
                    // routed this park to the backend domain only when the
                    // wait source bounds its parks) and this max_park
                    // computation. Never hand a finite cap to an unbounded
                    // wait source — AsyncIoContext::wait_one rejects it
                    // (not_supported), which would surface as a spurious
                    // no-progress terminate. Park unbounded instead: the
                    // deadline registration itself signals the wake domain,
                    // the bridge (backend_wait_active_) interrupts this park,
                    // and the re-drain re-decides the domain with the
                    // deadline visible.
                    if (max_park != std::chrono::nanoseconds::max() &&
                        !ctx_.has_bounded_split_wait_capability()) {
                        max_park = std::chrono::nanoseconds::max();
                    }
                    auto wr = ctx_.wait_one(max_park);
                    backend_wait_active_.store(false, std::memory_order_release);
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
                        (void)drain_routed_completion_waits_locked();
                        (void)wake_ready_flags_locked();
                        (void)pump_deadlines_locked();
                    }

                    if (!made_progress) {
                        LockGuard lk(global_mtx_);
                        // A concurrent route may have published work after the
                        // Phase-D drain. Reclassify under the same authority
                        // used by spawn() before publishing termination.
                        if (classify_locked(run_workers, ws) == MwState::mw_s1) {
                            continue;
                        }
                        // Phase G: an interrupted wait_one() while
                        // external-wake-capable waits remain registered is a
                        // re-evaluation signal from the wake bridge (an
                        // external publication or a spurious wake), NOT a
                        // no-progress boundary — stay resident and re-park
                        // (the run must remain Live while external waits can
                        // resolve). Pure control interrupts with NO external
                        // waits (backend-only MW-S2: stop / close) keep the
                        // terminate semantics below: the runtime driver
                        // re-enters on the control epoch, and the stop
                        // predicate converges the run at MW-S3 once the
                        // outstanding I/O is reaped.
                        if (external_wake_possible_locked()) {
                            continue;
                        }
                        // No backend progress: terminate this coordinated run. The
                        // run may be re-entered by the caller after staging work
                        // (E4/E5 model). MW-S2 with outstanding-but-uncompletable
                        // ops is treated as a no-progress boundary, NOT busy-spin.
                        global_terminate_.store(true, std::memory_order_release);
                        for (WorkerState* worker : run_workers) {
                            std::lock_guard<std::mutex> wlk(worker->inbox_mtx);
                            worker->inbox_cv.notify_all();
                        }
                        // E9: wake any Worker parked on the wake source.
                        signal_wake_locked();
                        // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: release any paused
                        // admission seam via the controller (test variant only).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                        sluice_async_test::release_all_phases(*this);
                        ws->loop_exit_reason =
                            WorkerState::LoopExitReason::mw_s2_no_progress_terminate;
#endif
                        break;
                    }
                    idle_workers_.store(0, std::memory_order_release);
                    continue;
                }
            } // if (elected)

            // Not elected and not committed: another worker is the candidate/
            // committed participant. Fall through to idle parking.
        }

        // state is MW-S3 or QUIESCENT (or MW-S2 non-participant): contribute
        // to coordinated termination. The last worker to go idle does a FINAL
        // re-check under global_mtx_ before setting global_terminate_.
        bool ready_flag_observation = false;
        {
            LockGuard lk(global_mtx_);
            // Phase G (E5-A2 level-triggered contract): a ready-flag wait
            // (await_ready_flag) is POLL-OBSERVED — wake_ready_flags_locked
            // resolves it in the drain; a bare flag store is a legal producer
            // (the Scheduler "observes the persistent flag"), and the
            // SchedulerWakeHandle notify is only a promptness optimization
            // that the park-time window can absorb. While such waits are
            // registered, the wake-domain park below MUST stay bounded at the
            // E9 observation interval so the drain re-observes the flags.
            ready_flag_observation = !waiting_ready_.empty();
            // Cancel any stale admission if we're terminating — wait_one is
            // undefined past run end. (admission_ should already be none here
            // for non-elected workers.)
            MwState final_state = classify_locked(run_workers, ws);
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
                //   Live  + MW-S3 + stop predicate true   -> RETURN (E14-F1)
                if (final_state == MwState::mw_s3_unresolved && run_mode_ == RunMode::live &&
                    external_wake_possible_locked()) {
                    // E14-F1: check the invocation stop predicate BEFORE
                    // deciding to park. If the caller (e.g. Group::await)
                    // says its own Futures are all terminal, terminate the
                    // run instead of parking on unrelated registrations.
                    if (invocation_stop_fn_ != nullptr &&
                        invocation_stop_fn_(invocation_stop_ctx_)) {
                        // Stop requested: fall through to the idle/terminate
                        // path (same as Drain MW-S3). The run returns to the
                        // caller; unrelated registrations remain for later.
                        // G1 repair: converge against the LIVE loop count —
                        // early-exited workers can never join the dance.
                        unsigned prev = idle_workers_.fetch_add(1, std::memory_order_acq_rel);
                        ws->idle_dance_contributed_.store(1, std::memory_order_release);
                        if (prev + 1 >= live_loop_workers_) {
                            MwState still = classify_locked(run_workers, ws);
                            if (still == MwState::mw_s3_unresolved || still == MwState::quiescent) {
                                global_terminate_.store(true, std::memory_order_release);
                                for (WorkerState* worker : run_workers) {
                                    std::lock_guard<std::mutex> wlk(worker->inbox_mtx);
                                    worker->inbox_cv.notify_all();
                                }
                                signal_wake_locked();
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                            sluice_async_test::release_all_phases(*this);
                            ws->loop_exit_reason =
                                WorkerState::LoopExitReason::e14f1_last_idle_terminate;
#endif
                            break;
                            }
                            idle_workers_.store(0, std::memory_order_release);
                            // Phase G: the final re-check observed mw_s1/mw_s2 —
                            // runnable work or backend progress EXISTS. Re-loop
                            // (the top of the loop runs it / elects the MW-S2
                            // participant) instead of falling through to a silent
                            // park: with the Phase G unbounded park, a silent
                            // park here deadlocks the run — the backend's ready
                            // signal reaches only the wait source, and no worker
                            // is parked in wait_one (deterministic stall:
                            // final_backend_ready_request_drains_at_shutdown).
                            continue;
                        } else {
                            // Phase G (E9-LIFE-8 termination convergence): not
                            // the last idle worker — wake the domain so the
                            // dance converges (see the plain idle path below
                            // for the full race analysis).
                            signal_wake_locked();
                        }
                    } else {
                        // Live: keep the run resident. Do NOT contribute to the
                        // idle/terminate count; fall through to park_on_wake_source.
                        // The bounded wake_cv timeout re-drains; persistent state
                        // is the authority (E9-LIFE-8).
                        idle_workers_.store(0, std::memory_order_release);
                        // Fall through to park_on_wake_source below.
                    }
                } else {
                    // G1 repair: converge against the LIVE loop count — an
                    // early-exited worker (e.g. the MW-S2 no-progress
                    // terminate) can never join the dance; counting it would
                    // strand the survivors one short of last-idle forever.
                    unsigned prev = idle_workers_.fetch_add(1, std::memory_order_acq_rel);
                    ws->idle_dance_contributed_.store(1, std::memory_order_release);
                    if (prev + 1 >= live_loop_workers_) {
                        // All workers idle. Final re-check (global_mtx_ held).
                        MwState still = classify_locked(run_workers, ws);
                        if (still == MwState::mw_s3_unresolved || still == MwState::quiescent) {
                            // Physical run termination. MW-S3 retains wait
                            // registrations logically; only quiescent is true
                            // completion. Both may terminate the run. In Drain
                            // this is RETURN STALLED (the E7/E8 contract); in
                            // Live it is reached only for MW-S3 without an
                            // effective wake source, or true quiescence.
                            global_terminate_.store(true, std::memory_order_release);
                            for (WorkerState* worker : run_workers) {
                                std::lock_guard<std::mutex> wlk(worker->inbox_mtx);
                                worker->inbox_cv.notify_all();
                            }
                            // E9: wake any Worker parked on the wake source.
                            signal_wake_locked();
                            // Release any paused test phase so parked test
                            // workers can observe termination.
                            // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: via the
                            // controller (test variant only).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                            sluice_async_test::release_all_phases(*this);
                            ws->loop_exit_reason =
                                WorkerState::LoopExitReason::last_idle_terminate;
#endif
                            break;
                        }
                        idle_workers_.store(0, std::memory_order_release);
                        // Phase G: the final re-check observed mw_s1/mw_s2 —
                        // runnable work or backend progress EXISTS. Re-loop
                        // (the loop top runs it / elects the MW-S2
                        // participant) instead of falling through to a silent
                        // park: with the Phase G unbounded park, a silent
                        // park here deadlocks the run — the backend's ready
                        // signal reaches only the wait source, and no worker
                        // is parked in wait_one (deterministic stall:
                        // final_backend_ready_request_drains_at_shutdown).
                        continue;
                    } else {
                        // Phase G (E9-LIFE-8 termination convergence): this
                        // worker observed quiescence/mw_s3 but is NOT the last
                        // idle worker. Signal the wake source so the parked
                        // workers re-run the dance and converge — the last
                        // worker's re-check sets global_terminate_. Without
                        // this signal, a Live-mw_s3 RESIDENT park (which resets
                        // idle_workers_ below) can erase this count between
                        // this worker's classify and its fetch_add; with the
                        // Phase G unbounded park (no periodic timeout) nobody
                        // would re-check and the run would never terminate
                        // after the final work completed (deterministic hang:
                        // st16_multi_worker_owner_routing). Persistent state
                        // first, then notify (AGENTS.md §13.2); the dance
                        // re-checks under global_mtx_, so the signal is
                        // advisory only and coalesces safely (G-I3).
                        signal_wake_locked();
                    }
                }
            }
        }

        if (global_terminate_.load(std::memory_order_acquire)) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            ws->loop_exit_reason =
                WorkerState::LoopExitReason::final_park_terminate;
#endif
            break;
        }

        // E9-CORRECTIVE seam A (ParkCandidate boundary, ADR §9.4.15): pause
        // the Worker right after it has decided to park (Live MW-S3 external
        // path) and before the physical wait setup. A test uses this to prove
        // a publication before ParkCandidate is drained (E9-T3). The seam
        // does NOT modify Scheduler state; it only pauses at the boundary.
        // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: controller-driven (test variant).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(*this,
            sluice_async_test::PhaseTag::scheduler_park_candidate);
#endif

        // E9: park on the unified wake source (wake_cv + wake epoch). This
        // replaces the E7 1ms inbox_cv timed park, which was a de-facto
        // periodic poll masking the external-wake gap (ADR §9.4). The wake
        // source's wake set now includes runnable publication (route_runnable
        // signals) and external-ready publication (SchedulerWakeHandle).
        // Phase G: no MIXED-WAKE backend observation is needed from the
        // wake-domain park — a split-wait backend parks its progress
        // participant in ctx_.wait_one(), and a non-split-wait MIXED-WAKE
        // park passes bounded_backend_observation=true at its own site. This
        // park is therefore deadline-driven only (unbounded without an active
        // deadline): no periodic wake, no 2ms CPU tax — EXCEPT while
        // level-triggered ready-flag waits are registered (ready_flag_
        // observation, captured under global_mtx_ above): those waits are
        // poll-observed by contract (E5-A2), so the park stays bounded at the
        // E9 observation interval until they resolve.
        park_on_wake_source(ws, /*bounded_backend_observation=*/ready_flag_observation);

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        // Phase G review P2b — G1 deterministic reproducer seam: the worker
        // just RETURNED from a wake-domain park and is about to re-enter the
        // loop top (pop own inbox -> try_steal -> global_mtx_ drain -> classify).
        // A test holding a worker HERE has the exact causal point the G1
        // park-window violation needs: everything the worker is about to
        // observe (a backend-ready publication, a route, a terminate flag) can
        // be published deterministically BEFORE the re-check. This tag is
        // deliberately EXCLUDED from release_all_phases (see async_test_control.cpp):
        // a terminating sibling worker's release_all_phases must not silently
        // destroy the reproducer's hold — the G1 stall scenarios are exactly
        // runs where a terminated worker leaves a survivor behind. A test that
        // arms this seam MUST release it (its own watchdog is the escape hatch).
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::worker_park_returned);
#endif
    }

    // G1 repair (§8.3 "terminate path strands queued runnables" / the
    // runnable-ownership invariant): a worker leaving the loop must not
    // strand tickets on its private queue. Move them to the pre-run domain
    // (pending_spawn_) so the LIVE workers of this invocation — woken by the
    // signal below, their loop tops drain pending_spawn_ — or, if none
    // remain, the NEXT invocation's setup (which distributes pending_spawn_
    // across its fresh participants and re-records owners) re-seeds them.
    // Lock order global_mtx_ -> inbox_mtx is the spawn/steal order.
    {
        LockGuard glk(global_mtx_);
        // G1 repair: this worker leaves the coordinated idle/termination
        // dance — drop it from the live count so the survivors' convergence
        // threshold shrinks (see the field contract in scheduler.hpp).
        --live_loop_workers_;
        // ...and retire the election-liveness fact in the SAME critical
        // section (the run_impl thread lambda repeats the store after
        // worker_loop returns — idempotent). Clearing `active` only there
        // left a window where a survivor's MW-S2 election scan (R2) still
        // saw this worker as the lowest-id ALIVE elector while it had
        // already left the loop: the survivor refused to elect, refused to
        // park, and signal-spun until the exiting thread finished its
        // straight-line epilogue (bounded, but the two liveness predicates
        // must move together).
        ws->active.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> ilk(ws->inbox_mtx);
            if (!ws->local_runnable.empty()) {
                for (Fiber* f : ws->local_runnable) {
                    pending_spawn_.push_back(f);
                }
                ws->local_runnable.clear();
            }
        }
        // Signal UNCONDITIONALLY (inbox lock RELEASED first — the
        // route_runnable_locked discipline; taking wake_mtx_ under inbox_mtx
        // would invert the park predicate's wake→inbox edge): the departure
        // itself is a wake-relevant publication — a survivor mid-idle-dance
        // parked after counting itself (or parked via a delegation
        // fall-through) must re-check the shrunk convergence threshold and
        // the moved tickets.
        signal_wake_locked();
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Causal worker-death evidence (G1 reproducer): after this store the
    // thread never touches WorkerState again (run_impl's thread lambda only
    // clears `active` and TLS). A test observing loop_exited knows the
    // worker's inbox residue is stranded unless a live participant re-seeds
    // or steals it.
    ws->loop_exited.store(true, std::memory_order_release);
#endif
}

void Scheduler::run_next_on(WorkerState* ws, Fiber* fiber) {
    // I47-F3: invalid runnable-ticket guard. A ticket whose Fiber is NOT
    // Runnable means the suspend-switch authority protocol was breached (a
    // thief stole a ticket before the owner saved the Fiber CPU context, or
    // a duplicate ticket was published). Fail-fast BEFORE entering the Fiber
    // context — a silent return would discard work and could hang the run.
    if (!fiber->make_running()) {
        detail::scheduler_invalid_runnable_ticket_fail_fast();
    }
    ws->current = fiber;
    running_fiber_count_.fetch_add(1, std::memory_order_acq_rel);
    fiber_ctx::Switch s;
    s.old = &ws->sched_ctx;
    s.new_ = &fiber->ctx;
    (void)fiber_ctx::context_switch(&s);
    // Control resumes here when the fiber switches back to ws->sched_ctx.
    //
    // I47-F2: clear suspend-switch authority on the SCHEDULER continuation.
    // At this moment the Fiber CPU context has been saved (the Fiber-side
    // context_switch stored rsp/rbp/rip into fiber->ctx). The routed runnable
    // ticket is now safe for migration/steal. This is the correct clear point
    // — NOT on the resumed Fiber continuation (the old Select path cleared it
    // there, leaving a window where a thief could steal before the save).
    //
    // Use store(false) unconditionally: if the Fiber completed instead of
    // suspending, suspend_switch_pending is already false (a Fiber that never
    // suspended never raised it). If it DID suspend, this clears the authority
    // raised by commit_suspend_locked. An exchange(false) + assert would be
    // stricter but store(false) is sufficient and cheaper.
    ws->suspend_switch_pending.store(false, std::memory_order_release);
    ws->current = nullptr;
    running_fiber_count_.fetch_sub(1, std::memory_order_acq_rel);
}

void Scheduler::commit_suspend_locked(WorkerState* ws, Fiber* fiber) {
    // I47-F2: unified suspend-switch authority protocol.
    //
    // Precondition: global_mtx_ held; fiber is ws->current (Running); wait
    // registration committed; inline-ready/terminal recheck ruled out return.
    //
    // Ordering (closes the suspend-before-switch race for ALL wait paths):
    //   1. Raise suspend authority BEFORE the Fiber becomes observably Waiting.
    //   2. Transition Running -> Waiting.
    //
    // Because every resolver (wake_ready_completions_locked, wake_wait_one,
    // cancel_wait, expire_wait, event_set_broadcast, select_publish_locked)
    // requires global_mtx_ to publish a Runnable ticket, and this function
    // holds global_mtx_ while raising authority AND transitioning Waiting,
    // there is NO window in which a resolver can publish before authority is
    // active. A thief under global_mtx_ sees suspend_switch_pending==true and
    // refuses the steal until run_next_on clears it (scheduler-side, after the
    // physical context switch saves the Fiber CPU context).
    ws->suspend_switch_pending.store(true, std::memory_order_release);
    if (!fiber->make_waiting()) {
        detail::scheduler_invalid_suspend_transition_fail_fast();
    }
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

bool Scheduler::drain_routed_completion_waits_locked() {
    // Phase F1 (issue #98): identity-bearing completion drain. Polls the
    // backend — the Scheduler-owned ReadyRoutingSink marks delivered records
    // during reap under the registry leaf (no G, no deadlock: the sink never
    // acquires a Scheduler lock, design §5.3) — then pops the delivered list
    // and routes each waiter exactly once under G. Replaces the O(N)
    // Completion::ready() re-scan of the Completion*-keyed maps (the maps are
    // gone; the sink carries the identity). Work per drain is O(delivered).
    (void)ctx_.poll();
    bool woken = false;
    WaitRecord* head = nullptr;
    {
        LockGuard rlk(wait_registry_mtx_);
        head = wait_delivered_head_;
        wait_delivered_head_ = nullptr;
    }
    while (head != nullptr) {
        WaitRecord* r = head;
        head = r->next_delivered;
        r->next_delivered = nullptr;
        // The record is pinned (delivered) — only the drain may consume it.
        Fiber* f = r->fiber;
        WorkerState* owner = r->owner;
        {
            LockGuard rlk(wait_registry_mtx_);
            r->state = WaitRecordState::free;
            r->fiber = nullptr;
            r->owner = nullptr;
            r->completion = nullptr;
            r->next_free = wait_record_free_head_;
            wait_record_free_head_ = r;
            --wait_record_live_count_;
        }
        // E7-T2 exactly-once: only publish a ticket if the fiber actually
        // transitioned waiting->runnable. If the fiber was already runnable
        // (a concurrent wake raced), do NOT enqueue a second ticket.
        //
        // Phase F1 P1-1: freeze the winner outcome BEFORE make_runnable.
        // The fiber reads this AFTER resume instead of racy c.ready().
        f->set_completion_wait_outcome(CompletionWaitOutcome::completed);
        if (f->make_runnable()) {
            route_runnable_locked(f, owner);
            woken = true;
        }
    }
    // Non-arena fallback (design §9): scan the legacy Completion*-keyed
    // maps. These entries exist only for backends whose register_waiter
    // returned not_supported (no RequestArena waiter machinery — custom/test
    // backends); they are never touched by the identity sink above, and the
    // identity records are never touched by this scan — each registration
    // lives in exactly one registry (non-dual authority).
    for (auto it = waiting_size_.begin(); it != waiting_size_.end();) {
        auto* c = static_cast<Completion<std::size_t>*>(it->first);
        if (c->ready()) {
            Fiber* f = it->second.fiber;
            WorkerState* owner = it->second.owner;
            it = waiting_size_.erase(it);
            // Phase F1 P1-1: freeze outcome for legacy-path fibers too.
            f->set_completion_wait_outcome(CompletionWaitOutcome::completed);
            if (f->make_runnable()) {  // E7-T2 exactly-once
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
            // Phase F1 P1-1: freeze outcome for legacy-path fibers too.
            f->set_completion_wait_outcome(CompletionWaitOutcome::completed);
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

// ---- Phase F1 registry helpers (leaf domain, callers hold G) ----

void Scheduler::ReadyRoutingSink::on_ready(detail::ReadyEvent event) noexcept {
    Scheduler* s = scheduler_;
    if (s == nullptr) return;  // detached (destruction)
    if (!event.waiter.has_waiter) {
        // No registered waiter for this request (or a waiter-cancel already
        // removed it) — nothing to route; the I/O/Completion lifecycle is
        // unaffected.
        return;
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    ++deliveries_;
#endif
    const detail::WaiterToken& t = event.waiter.token;
    LockGuard rlk(s->wait_registry_mtx_);
    // Identity validation (Race C, defense-in-depth): the arena only extracts
    // the current generation's token, but a stale/cross-Scheduler token must
    // never route — drop the lease and return.
    if (t.scheduler_identity != s->scheduler_identity_) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        ++stale_dropped_;
#endif
        return;
    }
    if (t.registration_slot >= s->wait_records_.size()) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        ++stale_dropped_;
#endif
        return;
    }
    WaitRecord* r = s->wait_records_[t.registration_slot].get();
    if (r->generation != t.registration_generation) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        ++stale_dropped_;
#endif
        return;
    }
    if (r->state != WaitRecordState::registered) {
        // The waiter-cancel path already retired this record (Race B loser).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        ++cancel_lost_;
#endif
        return;  // cancel won first
    }
    r->state = WaitRecordState::delivered;
    r->next_delivered = s->wait_delivered_head_;
    s->wait_delivered_head_ = r;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    ++routed_;
#endif
    // The routing lease is destroyed at scope exit = acknowledged (ADR
    // Decision 10). The record stays pinned (delivered) until the drain
    // consumes it, so the cancel path cannot retire it in between.
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
    const unsigned participant_count = active_worker_count_.load(std::memory_order_acquire);
    if (participant_count == 0) {
        // No worker can accept this ticket. Preserve it for the next
        // invocation, whose setup will assign a participating owner.
        pending_spawn_.push_back(f);
        signal_wake_locked();
        return;
    }

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

    WorkerState* target = owner;
    if (target == nullptr || target->id >= participant_count ||
        workers_[target->id].get() != target) {
        const unsigned target_id = next_spawn_worker_++ % participant_count;
        target = workers_[target_id].get();
        fiber_owner_[f] = target;
    }
    {
        std::lock_guard<std::mutex> lk(target->inbox_mtx);
        target->local_runnable.push_back(f);
        target->inbox_cv.notify_one();
    }

    // E9: signal the wake source so a Worker parked on the SCHEDULER domain
    // (park_on_wake_source) resumes. Refinement map: TLA+ PublishRunnable.
    // The inbox lock must already be released: the park predicate holds
    // wake_mtx_ before it inspects the inbox, so holding inbox_mtx here would
    // invert that order.
    signal_wake_locked();
}

WorkerState* Scheduler::owner_for_fiber_locked(Fiber* fiber) {
    // I47-F1: authoritative owner lookup for a previously-running Fiber.
    // A Fiber that has run and entered Waiting MUST have a recorded owner.
    // A missing entry is a fatal Scheduler invariant violation.
    auto it = fiber_owner_.find(fiber);
    if (it == fiber_owner_.end() || it->second == nullptr) {
        detail::scheduler_missing_fiber_owner_fail_fast();
    }
    return it->second;
}

bool Scheduler::publish_waiting_fiber_runnable_locked(Fiber* fiber) {
    // I47-F1: canonical waiting-Fiber publication. Routes the runnable ticket
    // to the Fiber's recorded owner Worker (NOT the resolver's g_worker).
    // The owner lookup is authoritative: a resolver must not change ownership.
    WorkerState* owner = owner_for_fiber_locked(fiber);
    if (!fiber->make_runnable()) {
        return false; // already runnable/running/done (exactly-once guard)
    }
    route_runnable_locked(fiber, owner);
    return true;
}

Scheduler::MwState Scheduler::classify_locked(const WorkerSnapshot& run_workers,
                                              WorkerState* classify_ws) const {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Park-window forensics (Phase G review P2a): record the classification
    // on the CALLING worker's own pair (value + monotonic sequence) so a park
    // ledger record attributes the state the PARKING worker trusted. A
    // Scheduler-global last-writer field mis-attributes when another worker
    // classifies between this worker's classify and its park commit.
    const MwState traced = classify_locked_impl(run_workers);
    if (classify_ws != nullptr) {
        classify_ws->last_classify.store(static_cast<int>(traced),
                                         std::memory_order_relaxed);
        classify_ws->classify_seq.fetch_add(1, std::memory_order_relaxed);
    }
    return traced;
#else
    (void)classify_ws;
    return classify_locked_impl(run_workers);
#endif
}

Scheduler::MwState Scheduler::classify_locked_impl(const WorkerSnapshot& run_workers) const {
    // Must be called with global_mtx_ held.
    bool any_runnable = !pending_spawn_.empty();
    if (!any_runnable) {
        for (WorkerState* worker : run_workers) {
            std::lock_guard<std::mutex> wlk(worker->inbox_mtx);
            if (!worker->local_runnable.empty()) {
                any_runnable = true;
                break;
            }
        }
    }
    const bool any_running = running_fiber_count_.load(std::memory_order_acquire) > 0;
    if (any_runnable || any_running)
        return MwState::mw_s1;

    // No executable Fiber. Backend outstanding count is the source of truth
    // for MW-S2 vs MW-S3. ctx_.outstanding() acquires access_mtx_ internally;
    // global_mtx_→access_mtx_ is the accepted lock order.
    const bool any_outstanding = ctx_.outstanding() > 0;
    if (any_outstanding)
        return MwState::mw_s2;

    // Phase F1: identity Completion waits live in the wait registry; a
    // registered record implies an accepted, unreaped backend op
    // (outstanding >= 1), so those classify as MW-S2 above and never reach
    // this check. Legacy fallback Completion waits (non-arena backends) and
    // flag/E10/Select waits can be unresolved with zero backend work.
    const bool any_wait = !waiting_size_.empty() || !waiting_void_.empty() ||
                          !waiting_ready_.empty() || waiting_waitq_count_ > 0 ||
                          waiting_select_count_ > 0;
    if (any_wait)
        return MwState::mw_s3_unresolved;

    return MwState::quiescent;
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
    const bool closed = s.select_all_authority_closed_locked(group);
    assert(closed &&
           "Select publication requires all arm authority closed");
    if (!closed) detail::select_invariant_fail_fast();
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

void Scheduler::AsyncTestAccess::select_event_forge_wrong_home(
    Scheduler& s, Event& event_a, Event& event_b,
    detail::SelectArmSlot& arm) {
    LockGuard lk(s.global_mtx_);
    assert(&event_a.scheduler_ == &s && &event_b.scheduler_ == &s &&
           "select_event_forge_wrong_home: Events must belong to this Scheduler");
    assert(arm.kind == detail::ArmKind::event &&
           arm.event.event_ == &event_a &&
           "select_event_forge_wrong_home: arm must be an Event arm bound to event_a");
    // Point home_ at event_b's port (the wrong Event). The rollback membership
    // check compares home_ against arm.event.event_->select_port_ (event_a's),
    // so it fails -> fail fast before any unlink.
    arm.home_ = &event_b.select_port_;
}

// ---- E12-F AsyncRwLock Category B death-test accessors ----
//
// Each constructs a deliberately-corrupted linked-node topology under G + W
// and then invokes the SAME production grant path so the fail-fast is the
// real production boundary (assert(false) + std::abort in Debug AND Release).
//
// The forged nodes are NOT caller-owned Fibers — they are stack-local to the
// accessor and the grant terminates before the accessor returns, so the
// forged linkage is never observed by a normal resolver.
//
// We access rw.waiters_ / rw.active_readers_ / rw.writer_active_ /
// rw.writer_owner_ through the Scheduler friend grant (AsyncRwLock declares
// Scheduler friend). This is the ONLY non-production path that forges user_
// on a linked node; ordinary tests cannot reach these symbols.
namespace {
// A RwWaitCtx-compatible layout with a valid mode value (read=0). Matches the
// production RwWaitCtx layout so the grant's static_cast<RwWaitCtx*> reads a
// well-formed mode byte. Production RwWaitCtx is in an anonymous namespace
// inside scheduler_rwlock.cpp; this test-local mirror keeps the seam header-stable.
struct ForgedRwWaitCtx {
    enum class Mode : std::uint8_t { read, write };
    Mode mode;
};
}  // namespace
void Scheduler::AsyncTestAccess::rwlock_death_forge_invalid_head_mode(
    Scheduler& s, AsyncRwLock& rw) {
    // Forged context: mode value 99 (neither read=0 nor write=1).
    struct BadCtx { std::uint8_t mode{99}; } bad;
    WaitNode forged_head;  // detached
    forged_head.set_user(&bad);  // install the bad-mode context BEFORE register
    {
        LockGuard lk(s.global_mtx_);
        LockGuard qlk(rw.waiters_.mtx());
        (void)rw.waiters_.register_wait_locked(forged_head, nullptr);
        ++s.waiting_waitq_count_;
        // user_ remains pointing at `bad` (register_wait_locked does not
        // touch user_). The grant's switch on mode MUST hit the default and
        // abort.
    }  // W released; G released.
    // Re-acquire G and call the production grant. grant_from_head_locked
    // requires the caller to hold G; it acquires W internally.
    LockGuard glk(s.global_mtx_);
    s.rwlock_grant_from_head_locked(rw.waiters_, rw.active_readers_,
                                    rw.writer_active_, rw.writer_owner_);
    // Unreachable on the intended path: the grant MUST have aborted above.
}

void Scheduler::AsyncTestAccess::rwlock_death_forge_null_head_user(
    Scheduler& s, AsyncRwLock& rw) {
    WaitNode forged_head;  // detached; user_ defaults to null
    // Do NOT call set_user — leave user_ null.
    {
        LockGuard lk(s.global_mtx_);
        LockGuard qlk(rw.waiters_.mtx());
        (void)rw.waiters_.register_wait_locked(forged_head, nullptr);
        ++s.waiting_waitq_count_;
    }
    LockGuard lk(s.global_mtx_);
    s.rwlock_grant_from_head_locked(rw.waiters_, rw.active_readers_,
                                    rw.writer_active_, rw.writer_owner_);
    // Unreachable: the null-user_ check MUST have aborted.
}

void Scheduler::AsyncTestAccess::rwlock_death_forge_invalid_batch_member(
    Scheduler& s, AsyncRwLock& rw) {
    // Head: a valid read-mode context. Second: an invalid-mode context.
    // The grant will claim the head reader, then encounter the second node's
    // invalid mode and abort (proving the per-node batch check is load-bearing).
    ForgedRwWaitCtx good_read{ForgedRwWaitCtx::Mode::read};
    struct BadCtx { std::uint8_t mode{99}; } bad;
    WaitNode forged_head;     // will be a valid reader
    WaitNode forged_second;   // will have invalid mode
    forged_head.set_user(&good_read);
    forged_second.set_user(&bad);
    {
        LockGuard lk(s.global_mtx_);
        LockGuard qlk(rw.waiters_.mtx());
        (void)rw.waiters_.register_wait_locked(forged_head, nullptr);
        (void)rw.waiters_.register_wait_locked(forged_second, nullptr);
        s.waiting_waitq_count_ += 2;
    }
    LockGuard lk(s.global_mtx_);
    s.rwlock_grant_from_head_locked(rw.waiters_, rw.active_readers_,
                                    rw.writer_active_, rw.writer_owner_);
    // Unreachable: the per-node mode check MUST have aborted on the 2nd node.
}
#endif

std::size_t Scheduler::runnable_count() const {
    std::size_t total = 0;
    LockGuard lk(global_mtx_);
    total += pending_spawn_.size();
    for (auto& w : workers_) {
        std::lock_guard<std::mutex> wlk(w->inbox_mtx);
        total += w->local_runnable.size();
    }
    return total;
}

bool Scheduler::unguarded_progress_pending_locked() const {
    // G1 repair: see the header contract. Observer checks first (cheap, the
    // common parked-delegation case): while a fiber executes somewhere or
    // the backend-domain participant exists, parked workers are delegating
    // to a live observer and may sleep.
    if (running_fiber_count_.load(std::memory_order_acquire) > 0 ||
        backend_wait_active_.load(std::memory_order_acquire) ||
        admission_ != AdmissionState::none) {
        return false;
    }
    if (!pending_spawn_.empty()) {
        return true;
    }
    const unsigned active_count =
        active_worker_count_.load(std::memory_order_acquire);
    // Queue-nonemptiness is the progress test, not per-fiber state: a ticket
    // whose Fiber is not Runnable would be a drift/invariant breach
    // (run_next_on fail-fasts at a LIVE owner; a TERMINATED owner never
    // reaches that check, so a drifted ticket there would refuse-park the
    // survivors indefinitely). Drift implies an upstream protocol breach;
    // documented rather than defended here.
    for (unsigned i = 0; i < active_count && i < workers_.size(); ++i) {
        WorkerState* w = workers_[i].get();
        std::lock_guard<std::mutex> ilk(w->inbox_mtx);
        if (!w->local_runnable.empty()) {
            return true;
        }
    }
    // ctx_.outstanding() takes access_mtx_ — the accepted global→access
    // order (classify_locked uses it under the same lock).
    return ctx_.outstanding() > 0;
}

bool Scheduler::try_steal(WorkerState* thief, const WorkerSnapshot& run_workers) {
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
    if (run_workers.size() <= 1)
        return false; // nothing to steal from

    LockGuard lk(global_mtx_);
    // Round-robin victim starting point keyed on thief id to spread load.
    unsigned n = static_cast<unsigned>(run_workers.size());
    for (unsigned k = 1; k < n; ++k) {
        unsigned vidx = (thief->id + k) % n;
        WorkerState* victim = run_workers[vidx];
        // P1-1 corrective (P6-C1 §9.2): refuse to steal ANY ticket from a victim
        // that is mid-suspend-context-switch. A resolver may have routed a
        // Runnable ticket onto this victim's local_runnable AFTER the victim
        // released global_mtx_ but BEFORE its physical context_switch saved the
        // fiber's CPU context. Resuming that ticket here would re-enter a stale
        // ctx. Skip the whole victim; the next suspend cycle re-arms the flag
        // only around its own switch. acquire load pairs with the release
        // stores in select.cpp's suspend path.
        if (victim->suspend_switch_pending.load(std::memory_order_acquire)) {
            continue;
        }
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

// E14 RT-F5: Evented admission override. The impl functions live in
// fail_fast.cpp (same TU as the override state). Extern-declare and forward.
namespace detail {
void set_evented_admission_override_impl(bool supported) noexcept;
void clear_evented_admission_override_impl() noexcept;
bool get_evented_admission_override_impl() noexcept;
}  // namespace detail

void Scheduler::AsyncTestAccess::set_evented_admission_override(bool supported) noexcept {
    detail::set_evented_admission_override_impl(supported);
}

bool Scheduler::AsyncTestAccess::evented_admission_override() noexcept {
    return detail::get_evented_admission_override_impl();
}
#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)

}  // namespace sluice::async
