// Scheduler park/wake domain (wake handle, external wake + interrupt bridge, park forensics, WaitRecord pool, awaits) — implementation TU split from scheduler.cpp in the
// post-freeze R1 structural pass (docs/post-freeze/structural-audit.md §6).
//
// Pure relocation: every definition below is byte-identical to its pre-split
// text at d9184de; the class declaration, lock domains, atomic orderings,
// and wake contracts remain in include/sluice/async/scheduler.hpp.
#include <sluice/async/scheduler.hpp>

#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/select.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "scheduler_internal.hpp"

#include <utility>
#include <cstdio>
#include <cstdlib>

// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the internal-testing variant pulls in
// the non-installed test-control header so the phase call sites below resolve to
// the controller. In the production build this include is absent and the call
// sites compile to nothing.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {
// ---- SchedulerWakeHandle::notify + bound ----

bool SchedulerWakeHandle::notify() noexcept {
    if (!control_) return false;
    // Hold Control::mtx (the callback lease) from
    // the validity check THROUGH the Scheduler wake callback. The
    // destructor acquires this same mutex before invalidating, so it
    // BLOCKS while a callback is in flight; invalidation + Scheduler
    // member destruction happen strictly after any validated callback
    // returns. This closes the snapshot-before-callback UAF window where
    // a previously-released lease let the destructor destroy members
    // between snapshot and callback. See spec/tla/e9_wake_handle_lifetime/.
    LockGuard lk(control_->mtx);
    if (!control_->alive || control_->scheduler == nullptr) {
        return false;  // post-destruction / unbound: no-op
    }
    // Deterministic lifetime seam (spec 13): pause at the
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

// Deterministic lifetime test seam (spec 13). TEST-ONLY.
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

SchedulerWakeHandle Scheduler::make_wake_handle() noexcept {
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
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // #196 trace: attribute the upcoming wake publication to the external
    // notify producer (consumed by signal_wake_locked's wake record).
    sluice_async_test::set_trace_wake_cause(
        *this, sluice_async_test::WakeCause::external_notify,
        static_cast<unsigned>(-1));
#endif
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
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        // #196 trace: the single chokepoint every wake publication passes.
        // Records wake_published with the pending producer attribution (or
        // cause=none when unattributed — the validator fail-closes on it).
        sluice_async_test::record_trace_wake(*this, wake_epoch_);
#endif
    }
    wake_cv_.notify_all();
    // Backend-progress bridge: a Scheduler wake-domain publication
    // (routing, flag/select/waitqueue resolution, deadline pump, external
    // wake handle, termination) must ALSO reach the MW-S2 progress
    // participant when it is parked in the BACKEND domain (ctx_.wait_one()).
    // interrupt_backend_waiters is the existing control seam: it bumps the
    // backend wait source's control epoch and wakes the park — no fabricated
    // readiness, no Completion publication, no I/O cancellation (AGENTS.md
    // §13.2: state first, then notify; the wake is advisory, persistent state
    // is authoritative). The atomic gate keeps the cost at one load when no
    // backend participant is parked; the commit-to-park window is closed by
    // the arm_committed_wait handshake (the arm runs in the same
    // critical section that sets the gate), and the invocation-level
    // control baseline keeps the interrupt one-shot per wait_one() (no
    // busy-spin). Lock order: wake_mtx_ is released before the wait-source
    // mutex is taken; the wait source is a leaf that never acquires Scheduler
    // locks (design docs/history/implementation-plans/phase-g-backend-progress-wake.md §4).
    if (backend_wait_active_.load(std::memory_order_acquire)) {
        ctx_.interrupt_backend_waiters();
    }
}

// TSA-SUPPRESS-001: park_on_wake_source uses std::unique_lock + cv.wait
// (release-wait-reacquire pattern).  Clang TSA cannot track unique_lock
// capability semantics through condition_variable::wait.  The production
// lock fact (wake_epoch_ is protected by wake_mtx_) is already independently
// accepted and proven in the park/wake domain.  Suppression is attached to the smallest exact
// function.
void Scheduler::park_on_wake_source(WorkerState* ws,
                                    bool bounded_backend_observation) SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    // Park the calling Worker on the SCHEDULER wake domain (ADR §9.4.5).
    // Record the observed epoch under wake_mtx_, then cv.wait with a bounded
    // timeout. The timeout is DEADLINE-DRIVEN (the earliest active
    // deadline, uncapped) so no periodic wake exists without an active
    // deadline. The 2ms bounded observation interval remains ONLY when
    // `bounded_backend_observation` is set — the MW-S2 MIXED-WAKE park for a
    // NON-split-wait (reference/legacy) backend, whose poll-driven readiness
    // is observed through that interval (it is their progress path; split-wait
    // backends park the progress participant in ctx_.wait_one() instead).
    //
    // Section 10 data-race fix: the predicate no longer
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

    // Deterministic pause seam B: pause at the commit boundary with NO wake lock
    // held, so the test can publish + notify (signal_wake_locked) during
    // the pause without deadlocking.
    // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: controller-driven (test variant).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_park_commit);
#endif

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Phase G park-window forensics: snapshot the classify-authority
    // persistent state BEFORE the wake-domain baseline is recorded.
    // Gated OFF by default (the snapshot locks shift park timing and made a
    // seam-driven regression flaky); only the forensics case arms it.
    // global_mtx_ must never be acquired while holding wake_mtx_ (one-way
    // order), so this snapshot runs first; ctx_.outstanding() takes
    // access_mtx_ (also one-way vs wake_mtx_) — both released before the
    // baseline below.
    ParkLedgerRecord forensics_rec{};
    if (park_forensics_enabled_.load(std::memory_order_acquire)) {
        {
            LockGuard glk(global_mtx_);
            forensics_rec.waiting_registered =
                waiting_size_.size() + waiting_void_.size() +
                waiting_ready_.size() +
                static_cast<std::size_t>(waiting_waitq_count_) +
                static_cast<std::size_t>(waiting_select_count_);
            forensics_rec.external_wake_possible =
                external_wake_possible_locked();
        }
        const BackendWaitToken forensics_tok = ctx_.backend_wait_token_for_test();
        forensics_rec.ready_generation = forensics_tok.progress_generation;
        forensics_rec.control_generation = forensics_tok.control_generation;
        forensics_rec.backend_outstanding = ctx_.outstanding();
    }
    forensics_rec.worker_id = ws->id;
    forensics_rec.idle_workers = idle_workers_.load(std::memory_order_acquire);
    forensics_rec.backend_wait_active =
        backend_wait_active_.load(std::memory_order_acquire);
    forensics_rec.ready_flag_bounded = bounded_backend_observation;
    forensics_rec.global_terminate =
        global_terminate_.load(std::memory_order_acquire);
    // Per-worker classify evidence (Phase G review P2a): the classification
    // THIS worker last trusted — its own classify pair, not a Scheduler-global
    // last-writer value that another worker's classify could have overwritten
    // between this worker's decision and its park commit.
    forensics_rec.last_classify =
        ws->last_classify.load(std::memory_order_relaxed);
    forensics_rec.classify_seq =
        ws->classify_seq.load(std::memory_order_relaxed);
#endif

    // The arm→recheck closure (conditions 1+4 of the stranded-progress repair):
    // the park commit used to record its wake-epoch baseline AFTER an
    // unlocked classify, so a runnable publication landing in the
    // check-then-arm window was consumed by the baseline and the worker
    // slept trusting a stale "someone else will run it" (the deterministic
    // stranded-runnable stall). The commit is now an ARM-then-RECHECK
    // handshake under the state authority (the Tokio Notify::enable
    // discipline cited in design §8.3): while holding global_mtx_ — the
    // same domain every runnable/route publication serializes under —
    // (a) REFUSE to park when unguarded progress remains: a runnable
    //     ticket anywhere in the run domain (ALWAYS — never delegatable
    //     to a running Fiber: the owner may sit in an unbounded fiber
    //     execution and no one else is awake to steal; Issue #115), or
    //     accepted backend work with NO active observer (no backend-
    //     domain participant, no admission in flight). The refusing
    //     worker re-loops and BECOMES the observer (its loop-top steals
    //     the runnable — including one stranded on a terminated worker's
    //     queue — or elects as the MW-S2 participant);
    // (b) otherwise record the baseline under the NESTED wake_mtx_
    //     (global→wake is the accepted order): a publication after this
    //     point advances the wake epoch past the baseline and the cv
    //     predicate observes it; a publication before it was visible to
    //     the (a) recheck — no consumed-signal window remains.
    // The cv.wait below runs with BOTH locks released; the predicate
    // (epoch / terminate / own local_runnable) is unchanged from the original park protocol.
    //
    // R4 (persistent-state backstop, final form after the adversarial
    // review): the idle-dance condition is checked HERE, at the COMMIT
    // recheck, against the worker's OWN dance contribution — NOT in the cv
    // predicate and NOT as a bare count comparison. Two rejected drafts
    // show why: (1) a predicate term observes the dancer's own count, so
    // the dancer's park becomes a no-op and it re-dances immediately; (2)
    // a bare commit-time `idle_workers_ > 0` refusal self-triggers the
    // same way, and each re-dance erases the count (last-idle store(0)),
    // so a woken sleeper always starts its dance from zero, emits its own
    // not-last signal, and wakes its peer — the wake-park-wake chain never
    // damps (the Live-mw_s3 resident livelock). Exempting the worker's
    // own contribution restores the original damping: a counted dancer
    // SLEEPS holding its count, so the woken sleeper's fetch_add reaches
    // the last-idle threshold immediately and its cycle ends in either
    // termination or a SILENT park (no further signal). Meanwhile a
    // worker that has not danced (contribution 0) refuses behind ANY live
    // count: the recheck and the dance serialize under global_mtx_ (the
    // dancer's fetch_add and not-last signal both hold it), so the dance
    // is either visible here (refuse; re-loop and converge the dance) or
    // its signal advances the epoch past the baseline being recorded
    // (predicate wake) — the absorbed-baseline window stays
    // closed with persistent state.
    //
    // Issue #161 (contribution-identity law, third refusal term): the R4
    // idle>own comparison cannot distinguish a dancer's OWN stale count from
    // the eraser's fresh one — both read 1 == 1 after the unlocked erase
    // orphaned the dancer's contribution. The generation term closes that:
    // a counted dancer whose recorded contribution identity is no longer
    // current (an unlocked erase at the popped-ticket or MW-S1 site
    // advanced Scheduler::dance_epoch_ past it) refuses, signals, and
    // re-dances toward the last-idle threshold instead of arming a
    // baseline that absorbs the eraser's not-last signal (the permanent
    // all-work-complete stall; TLC M4, spec/tla/e12_rwlock_scheduler_
    // liveness).
    {
        LockGuard glk(global_mtx_);
        const unsigned own_dance =
            ws->idle_dance_contributed_.load(std::memory_order_acquire);
        // Issue #161 (contribution-identity law): the identity term is
        // evaluated LAST — its dance_epoch_ load is sequenced AFTER the
        // idle_workers_ load above it, but that ordering does NOT (and
        // cannot) make the epoch load see the eraser's bump whenever the
        // idle load sees the erased count: the two are distinct unlocked
        // atomics, and the eraser's exchange(0)-then-bump pair can split
        // around this commit. What the ordering does give is the honest
        // dichotomy (see the refinement argument on dance_epoch_): a
        // mismatch refuses; a MATCH together with an erased count pins
        // the split window, in which the eraser's protocol is incomplete
        // and every not-last signal it can later emit is G-serialized
        // AFTER this arming — a transient park, never the M4 stall. A
        // mismatch means this worker's recorded contribution was orphaned
        // by an unlocked erase: the run is one short of the last-idle
        // threshold while this 1-bit flag still claims a live
        // contribution — the exact M4 stall (both workers parked, work
        // complete, no producer). Refuse and re-dance instead of arming a
        // baseline that would absorb the eraser's already-emitted
        // not-last signal. Gated on own_dance != 0: a worker that has
        // not danced must not refuse (its observer delegation is
        // legitimate — R4; also prevents a self-sustaining refuse loop on
        // an unbounded generation).
        if (unguarded_progress_pending_locked() ||
            idle_workers_.load(std::memory_order_acquire) > own_dance ||
            (own_dance != 0 &&
             dance_epoch_.load(std::memory_order_acquire) !=
                 ws->dance_epoch_at_contribution_.load(
                     std::memory_order_acquire))) {
            // Persistent progress with no observer, or an unfinished idle
            // dance this worker has not contributed to: parking here is
            // the stranded-progress violation or the baseline-absorption
            // violation (a dance count this sleeper's baseline would
            // absorb, leaving the convergence one short forever). Do not
            // record a baseline; the caller's re-loop makes this worker
            // the observer (steal / elect) or the next dance contributor.
            // SIGNAL the wake domain first: the refusing worker may not
            // itself be able to act on the progress (e.g. it is not the
            // lowest-id electable worker for the MW-S2 participant) — the
            // signal wakes a parked sibling that can (invariant B: a
            // progress publication must wake or elect an observer; without
            // the signal a non-electable refuser would spin between
            // refuse and re-loop while the only electable worker sleeps
            // on a stale classification).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            // #196 trace: the refusal decision, then its bundled signal
            // (TLA+ AbandonParkCandidate's signaling branch is one fused
            // step — the two trace events compile to that single action).
            sluice_async_test::TraceEvent refuse_ev{};
            refuse_ev.kind = static_cast<unsigned char>(
                sluice_async_test::TraceEventKind::park_refused);
            refuse_ev.worker = static_cast<unsigned char>(ws->id);
            sluice_async_test::record_trace_event(*this, refuse_ev);
            sluice_async_test::set_trace_wake_cause(
                *this, sluice_async_test::WakeCause::park_refuse,
                static_cast<unsigned>(-1));
#endif
            signal_wake_locked();
            ws->park_domain = WorkerState::ParkDomain::None;
            return;
        }
        LockGuard wlk(wake_mtx_);
        ws->observed_epoch = wake_epoch_;  // arm UNDER the state authority
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        // #196 trace: the park admission commit — the baseline this park
        // will trust (TLA+ FinalParkRecheckAndCommit's observedEpoch capture).
        // The armed flag records whether the E5-A2/#185 entry-armed bounded
        // observation (2 ms reference park) applies to THIS park.
        {
            sluice_async_test::TraceEvent commit_ev{};
            commit_ev.kind = static_cast<unsigned char>(
                sluice_async_test::TraceEventKind::park_committed);
            commit_ev.worker = static_cast<unsigned char>(ws->id);
            commit_ev.armed = bounded_backend_observation ? 1 : 0;
            commit_ev.epoch = wake_epoch_;
            sluice_async_test::record_trace_event(*this, commit_ev);
        }
        // Baseline point: the wake epoch this park will trust. Publications
        // that advanced the epoch BEFORE this line are consumed by the
        // baseline (the predicate compares against it); the ledger preserves
        // what the worker believed so a stall can state the exact violation.
        forensics_rec.epoch_at_commit = wake_epoch_;
        if (park_forensics_enabled_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> plk(park_ledger_mtx_);
            // Monotonic sequence: total parks ever recorded + 1 (ring wraps).
            forensics_rec.park_seq = park_ledger_total_ + 1;
            ++park_ledger_total_;
            park_ledger_[park_ledger_next_] = forensics_rec;
            park_ledger_next_ = (park_ledger_next_ + 1) % kParkLedgerCapacity;
            if (park_ledger_count_ < kParkLedgerCapacity) ++park_ledger_count_;
        }
#endif
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Issue #115 causal seam: the baseline is committed and NO lock is held.
    // A publication issued while paused here is strictly post-commit — the
    // cv predicate is its only possible transport (an epoch advance fires it
    // at wait entry; anything else is the #115 strand). Complements seam B
    // (scheduler_park_commit), which pauses strictly PRE-baseline.
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_park_baseline_recorded);
#endif
    std::unique_lock<Mutex> lk(wake_mtx_);
    // The cv predicate (both wait forms below share it).
    auto park_pred = [&]() SLUICE_NO_THREAD_SAFETY_ANALYSIS {
        // Section 10 data-race fix: check local_runnable SAFELY. The deque
        // is protected by inbox_mtx (route_runnable_locked pushes under it);
        // reading it under wake_mtx_ alone was a data race. Acquire inbox_mtx
        // for the read (nested under wake_mtx_ — never the reverse order).
        // This closes the lost-wake window where a runnable publication
        // advanced the epoch BEFORE observed_epoch was recorded.
        // route_runnable_locked also signals the epoch, so the epoch clause
        // usually fires first; the inbox term is the authoritative backstop
        // for the routed-but-epoch-already-observed case.
        if (wake_epoch_ != ws->observed_epoch ||
            global_terminate_.load(std::memory_order_acquire)) {
            return true;
        }
        std::lock_guard<std::mutex> ilk(ws->inbox_mtx);
        return !ws->local_runnable.empty();
    };
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // #196 trace: record the physical-wait boundary and, at the return,
    // WHICH predicate terms held (evaluated under the same lock the wait
    // reacquired — the exact cause set the as-built park returned on).
    // park_entered + an immediate park_returned (predicate already true at
    // entry, so cv.wait would return without blocking) compile to the model
    // EnterPhysicalPark predicate-true branch; a blocking return compiles
    // to LeavePark. The timeout bit marks a wait_until expiry with the
    // predicate still false.
    const auto e9t_record_entered = [this, ws]() {
        sluice_async_test::TraceEvent ev{};
        ev.kind = static_cast<unsigned char>(
            sluice_async_test::TraceEventKind::park_entered);
        ev.worker = static_cast<unsigned char>(ws->id);
        sluice_async_test::record_trace_event(*this, ev);
    };
    const auto e9t_record_returned = [this, ws](bool immediate,
                                                bool timed_out)
                                     SLUICE_NO_THREAD_SAFETY_ANALYSIS {
        sluice_async_test::TraceEvent ev{};
        ev.kind = static_cast<unsigned char>(
            sluice_async_test::TraceEventKind::park_returned);
        ev.worker = static_cast<unsigned char>(ws->id);
        ev.immediate = immediate ? 1 : 0;
        if (timed_out) {
            ev.return_causes = sluice_async_test::kReturnCauseTimeout;
        } else {
            std::uint16_t causes = 0;
            if (wake_epoch_ != ws->observed_epoch) {
                causes |= sluice_async_test::kReturnCauseEpoch;
            }
            if (global_terminate_.load(std::memory_order_acquire)) {
                causes |= sluice_async_test::kReturnCauseTerminate;
            }
            {
                std::lock_guard<std::mutex> ilk(ws->inbox_mtx);
                if (!ws->local_runnable.empty()) {
                    causes |= sluice_async_test::kReturnCauseRunnable;
                }
            }
            ev.return_causes = causes;
        }
        sluice_async_test::record_trace_event(*this, ev);
    };
#endif
    // Timeout policy: bound the wake wait by the earliest active
    // deadline so an active deadline cannot park a Worker indefinitely past
    // it, and by the bounded observation interval only when this
    // park must observe poll-driven backend readiness (non-split-wait
    // MIXED-WAKE). With no deadline and no backend observation the park is
    // unbounded — no periodic wake, no CPU tax. Read the deadline from the
    // LOCK-FREE atomic cache (earliest_active_deadline_) — NOT under
    // global_mtx_ — to avoid a wake_mtx_->global_mtx_ lock-order inversion
    // (signal_wake_locked takes them in the opposite order). A stale cache
    // read is safe: it only changes the park timeout slightly; the worker
    // loop's pump_deadlines_locked re-establishes the authoritative deadline
    // set under global_mtx_ on every iteration, so liveness holds even
    // if the cache briefly lags.
    static constexpr auto kParkBackstop = std::chrono::milliseconds(2);
    static constexpr auto kTestParkPoll = std::chrono::milliseconds(1);
    deadline_t earliest = earliest_active_deadline_.load(std::memory_order::acquire);
    // Termination-convergence note: this unbounded park is sound ONLY
    // because every quiescence-observation path wakes the domain — the
    // worker_loop idle dance signals the wake source when it is NOT the last
    // idle worker (the not-last signal). Without that signal, a Live-mw_s3
    // resident park's idle_workers_ reset can erase the last worker's
    // termination count between its classify and fetch_add, and with no
    // timeout nobody would re-check: the run would never terminate after the
    // final work completed (bounded-timeout convergence removed;
    // st16_multi_worker_owner_routing deterministic hang).
    if (earliest == kNoDeadline && !bounded_backend_observation) {
        // No deadline and no backend observation: unbounded park (epoch /
        // terminate / runnable predicate only). No periodic wake exists.
        // R4 note: the idle-dance backstop is enforced at the park COMMIT
        // (above), not here — see the R4 redesign comment there.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        // #196 trace: an entry-true predicate is an immediate return (the
        // model's EnterPhysicalPark predicate-true branch); the manual check
        // is behaviorally identical to cv.wait's own entry evaluation.
        e9t_record_entered();
        if (park_pred()) {
            e9t_record_returned(/*immediate=*/true, /*timed_out=*/false);
            ws->park_domain = WorkerState::ParkDomain::None;
            return;
        }
        wake_cv_.wait(lk, park_pred);
        e9t_record_returned(/*immediate=*/false, /*timed_out=*/false);
#else
        wake_cv_.wait(lk, park_pred);
#endif
        ws->park_domain = WorkerState::ParkDomain::None;
        return;
    }
    deadline_t now_ticks = clock_now_unlocked();
    auto wake_deadline = std::chrono::steady_clock::time_point::max();
    if (earliest != kNoDeadline) {
        if (earliest <= now_ticks) {
            wake_deadline = std::chrono::steady_clock::now();
        } else {
            deadline_t remaining = earliest - now_ticks;
            if (test_clock_mode_.load(std::memory_order_acquire)) {
                // Test mode: the clock is logical; cap at a short poll so
                // advance_clock()'s pump drives expiry deterministically.
                wake_deadline = std::chrono::steady_clock::now() + kTestParkPoll;
            } else if (bounded_backend_observation) {
                // MIXED-WAKE observation interval: cap the re-drain at the
                // 2ms interval so poll-driven backend readiness is observed
                // promptly (reference/legacy backends only — see the function
                // comment). Avoids a fixed 1ms poll (~1000 wakeups/s).
                auto delay = std::min(std::chrono::milliseconds(remaining),
                                      kParkBackstop);
                wake_deadline = std::chrono::steady_clock::now() + delay;
            } else {
                // Production, deadline-driven: park exactly until the earliest
                // deadline (uncapped) so the timer pump runs in time; no
                // periodic wake exists between now and the deadline.
                wake_deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(remaining);
            }
        }
    } else {
        // Bounded backend observation with no active deadline: the 2ms
        // observation interval (reference/legacy MIXED-WAKE only).
        wake_deadline = std::chrono::steady_clock::now() + kParkBackstop;
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // #196 trace: see the unbounded branch above (same entry/immediate/
    // blocking discipline; a wait_until expiry with the predicate still
    // false records the timeout bit — the E5-A2/#185 entry-armed
    // observation return).
    e9t_record_entered();
    if (park_pred()) {
        e9t_record_returned(/*immediate=*/true, /*timed_out=*/false);
        ws->park_domain = WorkerState::ParkDomain::None;
        return;
    }
    {
        const bool satisfied = wake_cv_.wait_until(lk, wake_deadline, park_pred);
        e9t_record_returned(/*immediate=*/false, /*timed_out=*/!satisfied);
    }
#else
    wake_cv_.wait_until(lk, wake_deadline, park_pred);
#endif
    ws->park_domain = WorkerState::ParkDomain::None;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
void Scheduler::dump_park_forensics_for_test(const char* tag) {
    // Phase G park-window forensics (G1 BLOCKED instrumentation). Called by
    // a forensics test's watchdog when a bounded wait for progress expired —
    // the run is presumed stalled with worker(s) parked. Each domain lock is
    // taken separately (no nesting): global_mtx_ (worker list, admission,
    // waiting sets) -> wake_mtx_ (epoch, per-worker baselines) ->
    // park_ledger_mtx_ (ring). Cross-thread diagnostic fields
    // (WorkerState::park_domain / current / loop_exit_reason / loop_exited /
    // last_classify) are atomics — Phase G review P2a: a "best-effort" racy
    // read is still UB; the forensics facility must itself be race-free or
    // its evidence is worthless under sanitizers.
    std::fprintf(stderr, "=== park-forensics[%s] begin ===\n", tag);

    // Domain 1: scheduler-global state.
    std::vector<WorkerState*> worker_ptrs;
    const char* admission = "none";
    std::size_t w_size = 0, w_void = 0, w_ready = 0, w_waitq = 0, w_select = 0;
    std::size_t pending_spawn = 0;
    // Issue #116 liveness forensics: the coordinated-run / convergence /
    // obligation counters a stalled-run classification argument needs. Same
    // domain (global_mtx_) as the fields above; printed with Domain 1.
    unsigned active_workers = 0, live_loop = 0, idle_now = 0;
    std::size_t running_fibers = 0;
    bool global_term = false, in_run = false;
    {
        LockGuard glk(global_mtx_);
        worker_ptrs.reserve(workers_.size());
        for (const std::unique_ptr<WorkerState>& w : workers_) {
            worker_ptrs.push_back(w.get());
        }
        if (admission_ == AdmissionState::candidate) {
            admission = "candidate";
        } else if (admission_ == AdmissionState::committed) {
            admission = "committed";
        }
        w_size = waiting_size_.size();
        w_void = waiting_void_.size();
        w_ready = waiting_ready_.size();
        w_waitq = waiting_waitq_count_;
        w_select = waiting_select_count_;
        pending_spawn = pending_spawn_.size();
        active_workers = active_worker_count_.load(std::memory_order_acquire);
        live_loop = static_cast<unsigned>(live_loop_workers_);
        idle_now = idle_workers_.load(std::memory_order_acquire);
        running_fibers = running_fiber_count_.load(std::memory_order_acquire);
        global_term = global_terminate_.load(std::memory_order_acquire);
        in_run = in_coordinated_run_;
    }
    // Wait-registry domain (leaf, taken separately — no nesting with G): the
    // identity-waiter obligation counts classify_locked cannot see directly.
    std::size_t wait_live = 0, wait_delivered = 0;
    {
        LockGuard rlk(wait_registry_mtx_);
        wait_live = wait_record_live_count_;
        for (WaitRecord* r = wait_delivered_head_; r != nullptr;
             r = r->next_delivered) {
            ++wait_delivered;
        }
    }
    std::fprintf(stderr,
                 "[park-forensics] run: active_workers=%u live_loop=%u "
                 "idle=%u running_fibers=%zu terminate=%d in_coordinated=%d "
                 "pending_spawn=%zu wait_live=%zu wait_delivered_pending=%zu\n",
                 active_workers, live_loop, idle_now, running_fibers,
                 global_term ? 1 : 0, in_run ? 1 : 0, pending_spawn, wait_live,
                 wait_delivered);
    std::fflush(stderr);

    // Domain 2: wake domain.
    std::uint64_t wake_epoch_now = 0;
    {
        LockGuard wlk(wake_mtx_);
        wake_epoch_now = wake_epoch_;
    }

    // Backend domain (wait source snapshot; no access_mtx_).
    const BackendWaitToken tok = ctx_.backend_wait_token_for_test();
    const std::size_t outstanding = ctx_.outstanding();

    std::fprintf(stderr,
                 "[park-forensics] wake_epoch=%llu token=(ready=%llu,ctrl=%llu) "
                 "outstanding=%zu admission=%s idle_workers=%u terminate=%d "
                 "backend_wait_active=%d\n",
                 static_cast<unsigned long long>(wake_epoch_now),
                 static_cast<unsigned long long>(tok.progress_generation),
                 static_cast<unsigned long long>(tok.control_generation),
                 outstanding, admission, idle_workers_.load(std::memory_order_acquire),
                 global_terminate_.load(std::memory_order_acquire) ? 1 : 0,
                 backend_wait_active_.load(std::memory_order_acquire) ? 1 : 0);
    std::fprintf(stderr,
                 "[park-forensics] waiting: size=%zu void=%zu ready=%zu waitq=%zu "
                 "select=%zu running_fibers=%ld pending_spawn=%zu\n",
                 w_size, w_void, w_ready, w_waitq, w_select,
                 static_cast<long>(running_fiber_count_.load(std::memory_order_acquire)),
                 pending_spawn);

    // Per-worker baselines (observed_epoch under wake_mtx_; park_domain /
    // current / loop_exit_reason / loop_exited / last_classify are atomic —
    // see the function comment).
    {
        LockGuard wlk(wake_mtx_);
        static const char* kExitName[] = {
            "(live)", "mw_s1_terminate_observed", "mw_s2_no_progress_terminate",
            "e14f1_last_idle_terminate", "last_idle_terminate",
            "final_park_terminate",
        };
        for (WorkerState* w : worker_ptrs) {
            const auto domain_raw =
                w->park_domain.load(std::memory_order_acquire);
            const char* domain = "None";
            if (domain_raw == WorkerState::ParkDomain::Scheduler) {
                domain = "SCHEDULER";
            } else if (domain_raw == WorkerState::ParkDomain::Backend) {
                domain = "Backend";
            }
            std::size_t local_runnable = 0;
            {
                std::lock_guard<std::mutex> ilk(w->inbox_mtx);
                local_runnable = w->local_runnable.size();
            }
            // If the worker is inside a Fiber, its identity + state says
            // WHERE it is blocked (await/waiting) — the parked-vs-running
            // question the domain field cannot answer alone.
            const Fiber* cur =
                w->current.load(std::memory_order_acquire);
            const char* fiber_state = "-";
            if (cur != nullptr) {
                switch (cur->state()) {
                    case FiberState::created: fiber_state = "created"; break;
                    case FiberState::runnable: fiber_state = "runnable"; break;
                    case FiberState::running: fiber_state = "running"; break;
                    case FiberState::waiting: fiber_state = "waiting"; break;
                    case FiberState::done: fiber_state = "done"; break;
                }
            }
            const auto exit_raw =
                w->loop_exit_reason.load(std::memory_order_acquire);
            const char* exit_name =
                (exit_raw >= WorkerState::LoopExitReason::live &&
                 exit_raw <= WorkerState::LoopExitReason::final_park_terminate)
                    ? kExitName[static_cast<std::size_t>(exit_raw)]
                    : "(unknown)";
            const int cls = w->last_classify.load(std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[park-forensics] worker id=%u domain=%s "
                         "observed_epoch=%llu local_runnable=%zu "
                         "current_fiber=%p fiber_state=%s exit_reason=%s "
                         "loop_exited=%d last_classify=%d\n",
                         w->id, domain,
                         static_cast<unsigned long long>(w->observed_epoch),
                         local_runnable,
                         static_cast<const void*>(cur), fiber_state, exit_name,
                         w->loop_exited.load(std::memory_order_acquire) ? 1 : 0,
                         cls);
        }
    }

    // Domain 3: the park ledger ring (oldest first when wrapped).
    {
        std::lock_guard<std::mutex> plk(park_ledger_mtx_);
        static const char* kMwName[] = {"mw_s1", "mw_s2", "mw_s3", "quiescent"};
        for (std::size_t i = 0; i < park_ledger_count_; ++i) {
            const std::size_t idx =
                (park_ledger_count_ < kParkLedgerCapacity)
                    ? i
                    : (park_ledger_next_ + i) % kParkLedgerCapacity;
            const ParkLedgerRecord& r = park_ledger_[idx];
            std::fprintf(stderr,
                         "[park-forensics] ledger seq=%llu worker=%u "
                         "epoch_at_commit=%llu ready=%llu ctrl=%llu "
                         "outstanding=%zu waiting=%zu classify=%s "
                         "classify_seq=%llu bounded=%d "
                         "idle=%u term=%d extwake=%d bwait=%d\n",
                         static_cast<unsigned long long>(r.park_seq), r.worker_id,
                         static_cast<unsigned long long>(r.epoch_at_commit),
                         static_cast<unsigned long long>(r.ready_generation),
                         static_cast<unsigned long long>(r.control_generation),
                         r.backend_outstanding, r.waiting_registered,
                         (r.last_classify >= 0 && r.last_classify <= 3)
                             ? kMwName[r.last_classify]
                             : "?",
                         static_cast<unsigned long long>(r.classify_seq),
                         r.ready_flag_bounded ? 1 : 0, r.idle_workers,
                         r.global_terminate ? 1 : 0,
                         r.external_wake_possible ? 1 : 0,
                         r.backend_wait_active ? 1 : 0);
        }
    }
    std::fprintf(stderr, "=== park-forensics[%s] end ===\n", tag);
}
#endif


Scheduler::WaitRecord* Scheduler::acquire_wait_record_locked(
    Fiber* fiber, WorkerState* owner, const void* completion,
    std::uint64_t& lease_id_out) {
    LockGuard rlk(wait_registry_mtx_);
    WaitRecord* r = wait_record_free_head_;
    if (r != nullptr) {
        wait_record_free_head_ = r->next_free;
        r->next_free = nullptr;
        // Reuse bumps the generation BEFORE the new occupant is visible:
        // a stale token can never match the reused record. First use from the
        // initial free list also bumps (generation 0 -> 1), so the first
        // occupant's token always differs from any future occupant's token.
        ++r->generation;
    } else {
        // Pool exhausted — synchronous no_space. The
        // configured capacity has been reached; all N records are live
        // (registered or delivered). No allocation is used as recovery.
        return nullptr;
    }
    r->state = WaitRecordState::registered;
    r->fiber = fiber;
    r->owner = owner;
    r->completion = completion;
    ++wait_record_live_count_;
    lease_id_out = wait_lease_serial_++;
    return r;
}

void Scheduler::retire_wait_record_locked(std::uint32_t index) {
    LockGuard rlk(wait_registry_mtx_);
    if (index >= wait_records_.size()) {
        detail::scheduler_wait_registry_invariant_fail_fast();
    }
    WaitRecord* r = wait_records_[index].get();
    if (r->state != WaitRecordState::registered) {
        detail::scheduler_wait_registry_invariant_fail_fast();
    }
    r->state = WaitRecordState::free;
    r->fiber = nullptr;
    r->owner = nullptr;
    r->completion = nullptr;
    r->next_free = wait_record_free_head_;
    wait_record_free_head_ = r;
    --wait_record_live_count_;
}

std::size_t Scheduler::wait_record_live_count_locked() const {
    LockGuard rlk(wait_registry_mtx_);
    return wait_record_live_count_;
}

// ---- Scheduler-owned identity-bearing ReadySink ----


Result<void> Scheduler::await_completion_size(Completion<std::size_t>& c) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    // The Completion wait is now a REAL arena waiter
    // registration (ADR Decision 10) + a Scheduler routing record, and the
    // wake arrives through the identity-bearing ReadySink — no Completion*-
    // keyed map entry, no O(N) ready() re-scan.
    //
    // Atomicity (Race A closure): record creation + arena register_waiter +
    // inline-ready recheck + commit_suspend_locked are ONE transition with
    // respect to the wake path. The drain (poll -> sink mark -> delivered-pop
    // -> route) runs under global_mtx_, so it cannot interleave this G-scope;
    // the arena leaf serializes registration against reap extraction (C2c
    // arena_register_waiter_vs_reap_race, now on the production path).
    //
    // Suspend protocol: commit_suspend_locked raises suspend_switch_pending BEFORE
    // make_waiting, both under global_mtx_, closing the suspend-before-switch
    // race exactly as before.
    {
        LockGuard lk(global_mtx_);
        std::uint64_t lease_id = 0;
        WaitRecord* rec = acquire_wait_record_locked(me, ws, &c, lease_id);
        if (rec == nullptr) {
            // Record creation failed (pre-wait caller path allocation).
            return make_unexpected<void>(IoError{IoError::Code::no_space});
        }
        const detail::WaiterToken token{scheduler_identity_, rec->index,
                                        rec->generation};
        detail::RoutingLease lease = detail::RoutingLease::pinning(
            lease_id, rec->index, rec->generation);
        // Transfer the lease into the request slot (arena leaf under G).
        auto reg = ctx_.register_waiter(c, token, std::move(lease));
        if (!reg.has_value()) {
            const IoError e = reg.error();
            if (e.code == IoError::Code::not_supported) {
                // Non-arena backend (custom AsyncBackend without the
                // RequestArena waiter machinery): legacy fallback path
                // (design §9) — Completion*-keyed map + ready() re-scan in
                // the drain. Disjoint from the identity registry: this
                // registration lives ONLY in the map.
                retire_wait_record_locked(rec->index);
                waiting_size_[static_cast<void*>(&c)] = {me, ws};
                if (c.ready()) {
                    waiting_size_.erase(static_cast<void*>(&c));
                    return Result<void>{};
                }
                commit_suspend_locked(ws, me);
            } else {
                // The registration lost: reap already closed it
                // (completion_ready) or the Completion is not bound to this
                // context's backend (cross-context / idle / stale —
                // provenance misuse). Retire the record; the candidate lease
                // was consumed at the by-value boundary by the arena.
                retire_wait_record_locked(rec->index);
                if (c.ready()) {
                    // Reap won (release-store): return WITHOUT suspending
                    // — no wake is lost, the Completion is ready.
                    return Result<void>{};
                }
                // Duplicate waiter or provenance misuse (Decision 6):
                // synchronous invalid_state.
                return make_unexpected<void>(IoError{IoError::Code::invalid_state});
            }
        } else {
            commit_suspend_locked(ws, me);
        }
    }
    // Generic suspension phase seam. AFTER wait registration committed
    // + suspend authority raised + Fiber Waiting + global_mtx_ released,
    // BEFORE the physical context_switch. A coordinator can resolve the wait
    // and prove the authority window is closed.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    // Read the frozen winner outcome instead of racy c.ready().
    // The winning path (drain=reap wins, cancel_waiter=cancel wins) wrote
    // this BEFORE make_runnable; we read it AFTER resume. This eliminates the
    // race where c.ready() could return true even though cancel won the arena
    // race (the I/O completed concurrently after cancel set the fiber runnable).
    if (me->completion_wait_outcome() == CompletionWaitOutcome::completed)
        return Result<void>{};
    return make_unexpected<void>(IoError{IoError::Code::canceled});
}

Result<void> Scheduler::await_completion_void(Completion<void>& c) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    // See await_completion_size for the unified identity
    // registration + suspend protocol.
    {
        LockGuard lk(global_mtx_);
        std::uint64_t lease_id = 0;
        WaitRecord* rec = acquire_wait_record_locked(me, ws, &c, lease_id);
        if (rec == nullptr) {
            return make_unexpected<void>(IoError{IoError::Code::no_space});
        }
        const detail::WaiterToken token{scheduler_identity_, rec->index,
                                        rec->generation};
        detail::RoutingLease lease = detail::RoutingLease::pinning(
            lease_id, rec->index, rec->generation);
        auto reg = ctx_.register_waiter(c, token, std::move(lease));
        if (!reg.has_value()) {
            const IoError e = reg.error();
            if (e.code == IoError::Code::not_supported) {
                // Non-arena fallback (design §9) — see await_completion_size.
                retire_wait_record_locked(rec->index);
                waiting_void_[static_cast<void*>(&c)] = {me, ws};
                if (c.ready()) {
                    waiting_void_.erase(static_cast<void*>(&c));
                    return Result<void>{};
                }
                commit_suspend_locked(ws, me);
            } else {
                retire_wait_record_locked(rec->index);
                if (c.ready()) return Result<void>{};
                return make_unexpected<void>(IoError{IoError::Code::invalid_state});
            }
        } else {
            commit_suspend_locked(ws, me);
        }
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    // Read the frozen winner outcome instead of racy c.ready().
    if (me->completion_wait_outcome() == CompletionWaitOutcome::completed)
        return Result<void>{};
    return make_unexpected<void>(IoError{IoError::Code::canceled});
}

Result<bool> Scheduler::cancel_waiter(Completion<std::size_t>& c) {
    // Waiter cancellation (ADR Decision 10): removes ONLY the waiter
    // registration — never the I/O, never the borrow, never a terminal result
    // The arena leaf races cancel_waiter against reap extraction exactly
    // once (C2c arena_cancel_waiter_vs_reap_race); the record mirrors the
    // winner.
    LockGuard lk(global_mtx_);
    auto rl = ctx_.cancel_waiter(c);  // G -> access_mtx_ -> arena leaf
    if (!rl.has_value()) {
        const IoError e = rl.error();
        if (e.code == IoError::Code::not_found) {
            // The delivery already won (reap closed the registration): the
            // fiber is or will be resumed by the identity route with the
            // Completion ready. Nothing to remove.
            return Result<bool>{false};
        }
        // Non-arena backend (not_supported) — fall back to the
        // legacy Completion*-keyed map. The map holds the fiber/owner; erase
        // it, freeze the canceled outcome, and route the fiber runnable.
        if (e.code == IoError::Code::not_supported) {
            auto it = waiting_size_.find(static_cast<void*>(&c));
            if (it != waiting_size_.end()) {
                Fiber* f = it->second.fiber;
                WorkerState* owner = it->second.owner;
                waiting_size_.erase(it);
                if (f != nullptr) {
                    f->set_completion_wait_outcome(
                        CompletionWaitOutcome::canceled);
                    if (f->make_runnable()) {
                        route_runnable_locked(f, owner);
                    }
                }
                return Result<bool>{true};
            }
            return Result<bool>{false};
        }
        return make_unexpected<bool>(e);
    }
    // Cancel won at the arena: the lease is ours. Retire the pinned record and
    // wake the suspended fiber exactly once with the wait-cancelled outcome.
    detail::RoutingLease lease = std::move(rl.value());
    Fiber* f = nullptr;
    WorkerState* owner = nullptr;
    {
        LockGuard rlk(wait_registry_mtx_);
        const std::uint32_t idx = lease.record_index();
        const std::uint32_t gen = lease.record_generation();
        if (idx < wait_records_.size()) {
            WaitRecord* r = wait_records_[idx].get();
            if (r->generation == gen && r->state == WaitRecordState::registered) {
                // Retire the record (cancelled is terminal; no drain visit).
                r->state = WaitRecordState::cancelled;
                f = r->fiber;
                owner = r->owner;
                r->state = WaitRecordState::free;
                r->fiber = nullptr;
                r->owner = nullptr;
                r->completion = nullptr;
                r->next_free = wait_record_free_head_;
                wait_record_free_head_ = r;
                --wait_record_live_count_;
            }
            // A generation mismatch or non-registered state is unreachable
            // (the lease pins the record; delivery is single-owner) — the
            // lease is dropped either way.
        }
    }
    if (f != nullptr) {
        // Freeze the cancel outcome BEFORE make_runnable.
        // The fiber reads this AFTER resume instead of racy c.ready().
        f->set_completion_wait_outcome(CompletionWaitOutcome::canceled);
        if (f->make_runnable()) {  // exactly-once
            route_runnable_locked(f, owner);
        }
    }
    return Result<bool>{true};
}

Result<bool> Scheduler::cancel_waiter(Completion<void>& c) {
    // See the size-twin for semantics; identical protocol.
    LockGuard lk(global_mtx_);
    auto rl = ctx_.cancel_waiter(c);
    if (!rl.has_value()) {
        const IoError e = rl.error();
        if (e.code == IoError::Code::not_found) {
            return Result<bool>{false};
        }
        // Non-arena backend (not_supported) — fall back to the
        // legacy Completion*-keyed map. The map holds the fiber/owner; erase
        // it, freeze the canceled outcome, and route the fiber runnable.
        if (e.code == IoError::Code::not_supported) {
            auto it = waiting_void_.find(static_cast<void*>(&c));
            if (it != waiting_void_.end()) {
                Fiber* f = it->second.fiber;
                WorkerState* owner = it->second.owner;
                waiting_void_.erase(it);
                if (f != nullptr) {
                    f->set_completion_wait_outcome(
                        CompletionWaitOutcome::canceled);
                    if (f->make_runnable()) {
                        route_runnable_locked(f, owner);
                    }
                }
                return Result<bool>{true};
            }
            return Result<bool>{false};
        }
        return make_unexpected<bool>(e);
    }
    detail::RoutingLease lease = std::move(rl.value());
    Fiber* f = nullptr;
    WorkerState* owner = nullptr;
    {
        LockGuard rlk(wait_registry_mtx_);
        const std::uint32_t idx = lease.record_index();
        const std::uint32_t gen = lease.record_generation();
        if (idx < wait_records_.size()) {
            WaitRecord* r = wait_records_[idx].get();
            if (r->generation == gen && r->state == WaitRecordState::registered) {
                r->state = WaitRecordState::cancelled;
                f = r->fiber;
                owner = r->owner;
                r->state = WaitRecordState::free;
                r->fiber = nullptr;
                r->owner = nullptr;
                r->completion = nullptr;
                r->next_free = wait_record_free_head_;
                wait_record_free_head_ = r;
                --wait_record_live_count_;
            }
        }
    }
    if (f != nullptr) {
        f->set_completion_wait_outcome(CompletionWaitOutcome::canceled);
        if (f->make_runnable()) {
            route_runnable_locked(f, owner);
        }
    }
    return Result<bool>{true};
}

void Scheduler::await_ready_flag(const std::atomic<bool>& ready) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    if (ready.load(std::memory_order::acquire)) return;
    // See await_completion_size for the unified suspend protocol.
    // register + recheck + commit_suspend_locked under global_mtx_; only
    // context_switch is outside.
    {
        LockGuard lk(global_mtx_);
        waiting_ready_[&ready] = {me, ws};
        if (ready.load(std::memory_order::acquire)) {
            waiting_ready_.erase(&ready);
            return;
        }
        commit_suspend_locked(ws, me);
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

void Scheduler::await_wait(WaitQueue& q, WaitNode& node) {
    // WaitQueue suspension seam. Unified suspend protocol.
    // register + recheck + commit_suspend_locked are ONE atomic transition
    // w.r.t. the wake path (wake_wait_one / cancel_wait run under global_mtx_);
    // only context_switch is outside. The queue protocol itself creates no
    // wake-before-suspend loss — the register_ CAS (under q.mtx_, taken inside
    // global_mtx_) publishes membership before commit_suspend_locked.
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
        if (!q.register_wait_locked(node, WaitResume::fiber(me))) {
            // Node was already registered or terminal: a contract violation.
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
        commit_suspend_locked(ws, me);
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

WaitNode* Scheduler::wake_wait_one_locked(WaitQueue& q) {
    // The wake_wait_one body with global_mtx_ already held. Resolves the
    // FIFO head with Woken (wake_one_locked), retires any bound timer
    // (retire_timer_for_node_locked), decrements waiting_waitq_count_,
    // and routes the winner runnable through the canonical wake seam. Returns
    // the winning node (nullptr if empty or head lost to a concurrent resolver).
    //
    // The caller MUST hold global_mtx_. q.mtx() is taken here (under global_mtx_,
    // consistent lock order). Used by the public wake_wait_one AND
    // event_set_broadcast's drain loop so the drain is atomic w.r.t. reset and
    // admission (all under global_mtx_).
    //
    // Timer-lifetime closure: a RESOURCE_WAKE winner MUST retire
    // any active timer registration bound to the resolved node, in the SAME
    // global_mtx_ critical section as the resolve CAS and BEFORE runnable
    // publication.
    LockGuard qlk(q.mtx());
    WaitNode* won = q.wake_one_locked();
    if (won == nullptr) return nullptr;  // empty, or head lost to a cancel
    retire_timer_for_node_locked(*won);
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // Exactly-once: publish a runnable ticket ONLY if waiting->runnable
    // succeeded. The node is terminal; the publication edge switches on the
    // winner's ResumeTarget kind (fiber route / deferred obligation / none).
    // Route to the Fiber's recorded owner (NOT g_worker).
    publish_wait_winner_locked(*won);
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

bool Scheduler::cancel_primitive_wait_locked(WaitQueue& waiters,
                                             WaitNode& node) {
    // Caller holds G + this exact W. Primitive-facing cancel APIs accept an
    // arbitrary WaitNode&, so target-queue membership must be proven before
    // the terminal CAS. The winning WaitQueue operation owns unlink in this
    // same W critical section; AC-2b owns ordinary timer retirement. The
    // closure stops here: waiting_waitq_count_ retirement stays at each
    // caller because that counter is current stackful-frontend bookkeeping
    // (MW classification), not frontend-neutral authority — the exactly-once
    // retirement obligation is semantic, the concrete counter is not.
    if (!waiters.contains_locked(node)) return false;
    if (!waiters.cancel_locked(node)) return false;
    retire_timer_for_node_locked(node);
    return true;
}

bool Scheduler::cancel_wait(WaitQueue& q, WaitNode& node) {
    // Resolve `node` with Cancelled and route the winner's fiber. Cancel is
    // wait-cancellation ONLY (not task/fiber/I/O cancellation). The winner is
    // determined by the same resolve CAS authority as wake (§2/§7): a losing
    // cancel (node already Woken) returns false and does nothing.
    //
    // Timer-lifetime closure: as in wake_wait_one, a CANCEL
    // winner MUST retire the bound active timer registration before runnable
    // publication.
    LockGuard lk(global_mtx_);
    LockGuard qlk(q.mtx());
    if (!q.cancel_locked(node)) return false;  // already terminal (loser)
    retire_timer_for_node_locked(node);
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // Route to the Fiber's recorded owner (NOT g_worker). The publication
    // edge switches on the ResumeTarget kind: a fiber returns the
    // exactly-once publication result; a deferred winner's delivery
    // obligation is committed (published) — the epoch is delivered.
    const WaitResume& r = node.resume();
    if (r.kind() == WaitResume::Kind::fiber) {
        Fiber* f = r.as_fiber();
        if (f != nullptr) {
            if (publish_waiting_fiber_runnable_locked(f)) {
                return true;
            }
        }
        return false;
    }
    if (r.kind() == WaitResume::Kind::deferred) {
        defer_publication_locked(r.as_deferred());
        return true;
    }
    return false;
}



void Scheduler::attach_ready_wake(const std::atomic<bool>& ready,
                                  SchedulerWakeHandle& wh) {
    // Validate that `ready` is currently registered as an external-wake wait.
    // The handle is bound to THIS Scheduler (make_wake_handle), so an external
    // producer's notify() already routes to signal_wake_locked. This method
    // exists to make the per-wait attachment explicit and to future-proof a
    // per-wait wake-map. It is a contract assertion + a pre-emptive
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
    (void)wh;  // contract-assertion parameter, see header (ADR §9.4.10)
    if (need_signal) {
        signal_wake_locked();
    }
}

}  // namespace sluice::async
