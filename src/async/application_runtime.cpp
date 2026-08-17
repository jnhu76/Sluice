// sluice::async::ApplicationRuntime — E16 implementation.
// ADR: docs/adr/ADR-application-runtime.md (Accepted).
// Design: docs/history/implementation-plans/e16-application-runtime.md.
#include <sluice/async/application_runtime.hpp>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/group.hpp>
#include <sluice/async/scheduler.hpp>

#include <cassert>
#include <cstdio>
#include <exception>

namespace sluice::async {

// ---------------------------------------------------------------------------
// Fiber-local execution identity (P1-04).
// The tag is stored IN Fiber state (Fiber::execution_tag_), not thread_local,
// so it survives Fiber suspend/resume and is correct under multiplexing.
// These helpers access the current Fiber's tag via the Scheduler's public
// introspection API (Scheduler::current_fiber_execution_tag()).
// ---------------------------------------------------------------------------
void ApplicationRuntime::set_current_fiber_tag(ApplicationRuntime* rt) noexcept {
    Scheduler::set_current_fiber_execution_tag(rt);
}

ApplicationRuntime* ApplicationRuntime::current_fiber_tag() noexcept {
    return static_cast<ApplicationRuntime*>(
        Scheduler::current_fiber_execution_tag());
}

// ---------------------------------------------------------------------------
// RuntimeTaskContext
// ---------------------------------------------------------------------------
CancelToken& RuntimeTaskContext::cancel_token() noexcept { return *token_; }

Result<void> RuntimeTaskContext::submit_read(ReadOp op, Completion<std::size_t>& c) {
    return ctx_->submit_read(op, c);
}
Result<void> RuntimeTaskContext::submit_write(WriteOp op, Completion<std::size_t>& c) {
    return ctx_->submit_write(op, c);
}
Result<void> RuntimeTaskContext::submit_sync_data(SyncDataOp op, Completion<void>& c) {
    return ctx_->submit_sync_data(op, c);
}
Result<void> RuntimeTaskContext::submit_sync_all(SyncAllOp op, Completion<void>& c) {
    return ctx_->submit_sync_all(op, c);
}

// Phase F3 (ADR-public-request-handle): identity-returning submit.
Result<RequestHandle> RuntimeTaskContext::submit_read_request(ReadOp op,
                                                              Completion<std::size_t>& c) {
    return ctx_->submit_read_request(op, c);
}
Result<RequestHandle> RuntimeTaskContext::submit_write_request(WriteOp op,
                                                               Completion<std::size_t>& c) {
    return ctx_->submit_write_request(op, c);
}
Result<RequestHandle> RuntimeTaskContext::submit_sync_data_request(SyncDataOp op,
                                                                   Completion<void>& c) {
    return ctx_->submit_sync_data_request(op, c);
}
Result<RequestHandle> RuntimeTaskContext::submit_sync_all_request(SyncAllOp op,
                                                                  Completion<void>& c) {
    return ctx_->submit_sync_all_request(op, c);
}

// M1-A: cooperative Completion wait. Delegates to the already-audited
// Scheduler::await_completion_* primitive (one suspend + one resume per
// unresolved await; already-ready returns inline under the Scheduler's
// registration critical section). The Scheduler* is private; task code never
// sees raw Scheduler authority. See docs/history/implementation-plans/m1-runtime-io-await-race.md.
//
// Precondition: `c` must be outstanding (a prior submit_* marked it so). The
// underlying Scheduler primitive has no idle check of its own: it registers
// the Fiber for wake and, if the Completion is idle, nothing will ever
// complete it and the Fiber parks permanently. Enforce the documented
// "Debug asserts; Release documents" contract here (M1-A Known limitations).
Result<void> RuntimeTaskContext::await_completion(Completion<std::size_t>& c) {
    assert(!c.idle() &&
           "await_completion requires a submitted or ready Completion "
           "(idle-await is a caller contract violation: M1-A)");
    // Phase F1: the Scheduler primitive now returns the wait outcome (success,
    // synchronous invalid_state for duplicate/provenance misuse, or canceled
    // when the wait was cancelled via cancel_waiter). Forward verbatim so task
    // code can distinguish "completion ready" from "wait cancelled".
    return sched_->await_completion_size(c);
}

Result<void> RuntimeTaskContext::await_completion(Completion<void>& c) {
    assert(!c.idle() &&
           "await_completion requires a submitted or ready Completion "
           "(idle-await is a caller contract violation: M1-A)");
    return sched_->await_completion_void(c);
}

Result<bool> RuntimeTaskContext::cancel_waiter(Completion<std::size_t>& c) {
    // Phase F1: production waiter-cancel caller (ADR Decision 10). Removes
    // ONLY the waiter — the I/O continues; the Completion stays outstanding.
    return sched_->cancel_waiter(c);
}

Result<bool> RuntimeTaskContext::cancel_waiter(Completion<void>& c) {
    return sched_->cancel_waiter(c);
}

#ifdef SLUICE_ASYNC_INTERNAL_TESTING
void RuntimeTaskContext::suspend(std::atomic<bool>& flag) {
    sched_->await_ready_flag(flag);
}
#endif

// ---------------------------------------------------------------------------
// RuntimeBuilder
// ---------------------------------------------------------------------------
RuntimeBuilder& RuntimeBuilder::backend(std::unique_ptr<AsyncBackend> b) {
    backend_ = std::move(b);
    return *this;
}

RuntimeBuilder& RuntimeBuilder::workers(unsigned n) {
    workers_ = n;
    return *this;
}

Result<std::unique_ptr<ApplicationRuntime>> RuntimeBuilder::build() {
    if (!backend_) {
        return make_unexpected<std::unique_ptr<ApplicationRuntime>>(
            IoError{IoError::Code::invalid_state});
    }
    if (workers_ == 0) {
        workers_ = 1;
    }
    // Issue #67 (D3): the multi-participant runtime path MUST NOT fall back to
    // a BLOCKING serialized wait_one — a participant parked while holding
    // access_mtx_ starves every other poll/reap path and deadlocks drain. A
    // backend is accepted only if it exposes the split wait capability
    // (ThreadPoolBackend) OR guarantees a non-blocking wait_one (the reference
    // backends SyncBackend / FakeAsyncBackend, whose readiness is produced
    // synchronously inside poll). Anything else (UringAsyncBackend, external
    // legacy backends) is rejected HERE with a reportable invalid_state
    // instead of silently taking the dangerous fallback.
    if (backend_->wait_source() == nullptr && !backend_->wait_one_is_nonblocking()) {
        return make_unexpected<std::unique_ptr<ApplicationRuntime>>(
            IoError{IoError::Code::invalid_state});
    }
    // Construct on the heap for stable address (P1-02).
    // Cannot use make_unique because the constructor is private.
    std::unique_ptr<ApplicationRuntime> rt(
        new ApplicationRuntime(std::move(backend_), workers_));
    return std::move(rt);
}

// ---------------------------------------------------------------------------
// ApplicationRuntime construction / destruction
// ---------------------------------------------------------------------------
ApplicationRuntime::ApplicationRuntime(std::unique_ptr<AsyncBackend> backend,
                                       unsigned workers)
    : io_ctx_(std::make_unique<AsyncIoContext>(std::move(backend)))
    , worker_count_(workers) {
    // Construct Scheduler borrowing io_ctx_.
    sched_ = std::make_unique<Scheduler>(*io_ctx_);
    // Construct root Group in Evented mode (borrows Scheduler).
    root_group_ = std::make_unique<Group>(*sched_);
    // Acquire a wake handle for external-wake capability.
    wake_handle_ = sched_->make_wake_handle();
}

ApplicationRuntime::~ApplicationRuntime() {
    // Per-state safety (ADR §8):
    // Constructed / StartFailed / Stopped: safe to destroy.
    // Any other state: fail-fast.
    State s;
    {
        std::lock_guard lk(lifecycle_mtx_);
        s = state_;
    }
    if (s != State::Constructed && s != State::StartFailed && s != State::Stopped) {
        detail::group_lifetime_fail_fast();  // reuse existing fail-fast entry
    }
    // If driver was spawned and joined, thread is already joinable-safe.
    if (driver_thread_.joinable()) {
        driver_thread_.join();
    }
    // Remaining members (root_group_, sched_, io_ctx_) are already null
    // if close_resources() was called; otherwise they destroy normally in
    // reverse declaration order via ~unique_ptr.
}

// ---------------------------------------------------------------------------
// start()
// ---------------------------------------------------------------------------
Result<void> ApplicationRuntime::start() {
    std::unique_lock lk(lifecycle_mtx_);

    // Reject if not Constructed OR if a close is in flight / already done. The
    // close_state_ guard closes the Constructed-race window: a concurrent
    // shutdown() that observed Constructed and elected itself close owner
    // (Open -> InProgress) releases lifecycle_mtx_ between the election and
    // close_resources()'s Stopped publication. During that window state_ is
    // still Constructed but close_state_ is InProgress — without this check,
    // start() would proceed to spawn the driver over already-moved resources
    // (UAF). This makes start()/shutdown() races on the Constructed->Starting
    // boundary converge instead of corrupting state.
    if (state_ != State::Constructed || close_state_ != CloseState::Open) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    // Remember stop_requested: if stop was called before start, the runtime
    // must still tear down its already-constructed components (Group/Scheduler/
    // AsyncIoContext/backend) before reporting canceled. The start owner is the
    // close owner here (no driver was spawned): elect close owner and funnel
    // through the UNIFIED close_resources() authority (C1) so component
    // destruction completes before Stopped is published. Without this, stop-
    // before-start would publish no Stopped at all and leave resources alive
    // until ~ApplicationRuntime.
    if (stop_requested_) {
        close_state_ = CloseState::InProgress;
        lk.unlock();
        close_resources();
        return make_unexpected_void(IoError{IoError::Code::canceled});
    }

    state_ = State::Starting;

    // Spawn the driver thread.
    try {
        driver_thread_ = std::thread([this] { driver_main(); });
        driver_spawned_ = true;
    } catch (const std::system_error&) {
        state_ = State::StartFailed;
        driver_state_ = DriverState::not_started;
        return make_unexpected_void(IoError{IoError::Code::backend_error});
    }

    // Wait for the driver to reach the startup barrier (or exit if abort won
    // the race before we observed barrier_wait — Warning #2 fix).
    runtime_cv_.wait(lk, [this] {
        return driver_state_ == DriverState::barrier_wait ||
               driver_state_ == DriverState::exited;
    });

    // If the driver already exited (abort won before barrier was observed),
    // complete the abort path. The start owner is the close owner: join the
    // driver, then funnel through the UNIFIED close_resources() authority (C1)
    // so component destruction completes before Stopped is published. A
    // concurrent shutdown() waiter that observed Starting observes the final
    // Closed publication from close_resources() (E16-CORR-ABORT-2: there is
    // exactly one close owner — the start owner here, shutdown() only waits).
    if (driver_state_ == DriverState::exited) {
        // Elect close owner so a concurrent shutdown() waiter does not also
        // attempt to close.
        close_state_ = CloseState::InProgress;
        runtime_cv_.notify_all();
        lk.unlock();
        if (driver_thread_.joinable()) driver_thread_.join();
        close_resources();
        return make_unexpected_void(IoError{IoError::Code::canceled});
    }

    // Startup barrier: the driver is at barrier_wait. Commit or abort.
#ifdef SLUICE_ASYNC_INTERNAL_TESTING
    // Test-only commit checkpoint: if enabled (off by default), signal that
    // the start owner is at the commit checkpoint (immediately before checking
    // stop_requested_), then park on commit_release_flag_ until the test
    // releases it. This lets a test inject stop/shutdown between the barrier-
    // wait wake and the stop_requested_ check, deterministically forcing the
    // startup-abort path (start() == canceled) instead of a racy either/or
    // outcome. OFF by default so that tests that do not need the pause (e.g.
    // the suspend/resume identity test) proceed straight through to commit.
    if (test_pause_at_commit_checkpoint_.load(std::memory_order::acquire)) {
        commit_checkpoint_promise_.set_value();
        commit_release_flag_.store(false, std::memory_order::release);
        runtime_cv_.notify_all();
        runtime_cv_.wait(lk, [this] {
            return commit_release_flag_.load(std::memory_order::acquire);
        });
    }
#endif
    if (stop_requested_) {
        // Stop won the race pre-commit (P1-03 abort path). The start owner is
        // the close owner: signal the driver to abort, wait for it to exit,
        // join, then close via the UNIFIED close_resources() authority (C1).
        startup_abort_requested_ = true;
        close_state_ = CloseState::InProgress;
        control_epoch_++;
        runtime_cv_.notify_all();
        // Wait for driver to exit.
        runtime_cv_.wait(lk, [this] {
            return driver_state_ == DriverState::exited;
        });
        lk.unlock();
        if (driver_thread_.joinable()) driver_thread_.join();
        close_resources();
        return make_unexpected_void(IoError{IoError::Code::canceled});
    }

    // Commit: open admission, transition to Running.
    state_ = State::Running;
    admission_open_ = true;
    control_epoch_++;
    runtime_cv_.notify_all();
    wake_handle_.notify();

    return {};
}

// ---------------------------------------------------------------------------
// submit()
// ---------------------------------------------------------------------------
Result<void> ApplicationRuntime::submit(RuntimeTaskFn task) {
    if (!task) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    std::unique_lock lk(lifecycle_mtx_);

    // Admission reservation: admission_open must be true.
    if (!admission_open_) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    // Reserve admission slot. Reset stale drain_complete_ (P0-1 fix: the
    // driver may have set it in a prior quiescent window before this admission;
    // a new task invalidates any prior drain observation).
    admitted_count_++;
    drain_complete_ = false;
    recompute_task_set_terminal_locked();

    // Release lock before calling into Group (Group acquires its own mtx).
    lk.unlock();

    // Submit to root group. Wrap the user task with terminal guard + context.
    try {
        root_group_->async([this, task = std::move(task)](CancelToken& token) mutable {
            // Set Fiber-local execution identity for worker-call detection.
            // Save/restore the previous tag for nested or interleaved Runtime
            // safety. The tag is stored in the Fiber (not thread_local), so it
            // survives Fiber suspend/resume and is correct under multiplexing.
            auto* prev_tag = current_fiber_tag();
            set_current_fiber_tag(this);

            // RuntimeTaskContext delegates I/O to io_ctx_ and the cooperative
            // Completion wait to sched_ (M1-A). The Scheduler* is private to
            // the context and never escapes to task code. Both production and
            // internal-testing builds share the same constructor signature.
            RuntimeTaskContext ctx(*io_ctx_, token, *sched_);

            // Run user task body; swallow exceptions at this boundary.
            try {
                task(ctx);
            } catch (...) {
                // User task threw. Swallow (Group boundary contract).
            }

            // Terminal guard: publish terminal_count++ exactly once.
            {
                std::lock_guard glk(lifecycle_mtx_);
                terminal_count_++;
                recompute_task_set_terminal_locked();
                control_epoch_++;
            }
            runtime_cv_.notify_all();
            wake_handle_.notify();

            // Restore previous execution identity.
            set_current_fiber_tag(prev_tag);
        });
    } catch (...) {
        // Group::async threw (e.g. bad_alloc from vector reserve).
        // Rollback admission reservation.
        lk.lock();
        admitted_count_--;
        recompute_task_set_terminal_locked();
        control_epoch_++;
        lk.unlock();
        runtime_cv_.notify_all();
        wake_handle_.notify();
        throw;  // Propagate bad_alloc (P2-02: NOT mapped to invalid_state).
    }

    // Successful submit: publish epoch++ + dual-wake (P1-07).
    {
        std::lock_guard slk(lifecycle_mtx_);
        control_epoch_++;
    }
    runtime_cv_.notify_all();
    wake_handle_.notify();

    return {};
}

// ---------------------------------------------------------------------------
// request_stop()
// ---------------------------------------------------------------------------
void ApplicationRuntime::request_stop() noexcept {
    std::lock_guard lk(lifecycle_mtx_);

    if (stop_requested_) return;  // Idempotent.
    stop_requested_ = true;

    if (state_ == State::Running) {
        // Close admission.
        admission_open_ = false;
        admission_closed_snapshot_.store(true, std::memory_order::release);
        // Publish root cancellation under lifecycle_mutex (P1-01).
        if (!root_cancel_published_ && root_group_) {
            root_group_->group_token().request();
            root_cancel_published_ = true;
        }
        // Transition to Stopping.
        state_ = State::Stopping;
        control_epoch_++;
    } else if (state_ == State::Constructed) {
        // Remember stop; start() will return canceled.
        // No state transition.
    } else if (state_ == State::Starting) {
        // Abort startup.
        startup_abort_requested_ = true;
        control_epoch_++;
    }
    // Stopping/Draining/Stopped/StartFailed/Fatal: no additional action.

    // Issue #67 (I6): wake every participant parked in the backend ready wait.
    // Without this, an MW-S2 participant parked in wait_one's observe phase
    // would never learn that the run must terminate, the coordinated run could
    // not reach its stop-predicate boundary, and drain_complete_ would never
    // be satisfied. Order: stopping state is published above, THEN the
    // backend waiters are interrupted (state first, then wake). Lock order:
    // lifecycle_mtx_ -> ready-wait leaf mutex (accepted; no path acquires
    // lifecycle_mtx_ while holding the ready-wait mutex). No-op for backends
    // without the split wait capability (rejected at build).
    if (io_ctx_) io_ctx_->interrupt_backend_waiters();

    runtime_cv_.notify_all();
    wake_handle_.notify();
}

// ---------------------------------------------------------------------------
// drain()
// ---------------------------------------------------------------------------
Result<void> ApplicationRuntime::drain() {
    if (is_runtime_task()) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    std::unique_lock lk(lifecycle_mtx_);

    if (state_ != State::Stopping && state_ != State::Draining) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    if (state_ == State::Stopping) {
        state_ = State::Draining;
    }

    // Wait for drain_complete.
    runtime_cv_.wait(lk, [this] {
        return drain_complete_ || state_ == State::Fatal;
    });

    if (state_ == State::Fatal) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    return {};
}

// ---------------------------------------------------------------------------
// join()
// ---------------------------------------------------------------------------
Result<void> ApplicationRuntime::join() {
    if (is_runtime_task()) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    std::unique_lock lk(lifecycle_mtx_);

    // Must have drain_complete.
    if (!drain_complete_) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    // Close owner election (P1-05).
    if (close_state_ == CloseState::Closed) {
        return {};  // Already closed (idempotent).
    }
    if (close_state_ == CloseState::InProgress) {
        // Wait for the other owner to finish.
        runtime_cv_.wait(lk, [this] { return close_state_ == CloseState::Closed; });
        return {};
    }

    // We are the close owner.
    close_state_ = CloseState::InProgress;
    driver_exit_requested_ = true;
    driver_exit_snapshot_.store(true, std::memory_order::release);
    control_epoch_++;
    runtime_cv_.notify_all();
    wake_handle_.notify();

    // Wait for driver to exit.
    runtime_cv_.wait(lk, [this] {
        return driver_state_ == DriverState::exited;
    });

    lk.unlock();

    // Join the driver thread.
    if (driver_thread_.joinable()) {
        driver_thread_.join();
    }

    // Destroy resources and publish Stopped.
    close_resources();

    return {};
}

// ---------------------------------------------------------------------------
// shutdown()
// ---------------------------------------------------------------------------
Result<void> ApplicationRuntime::shutdown() {
    if (is_runtime_task()) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    std::unique_lock lk(lifecycle_mtx_);

    // Idempotent: already closed.
    if (close_state_ == CloseState::Closed) {
        return {};
    }

    // State-dispatched close (P1-05).
    switch (state_) {
    case State::Constructed:
    case State::StartFailed: {
        // Direct close: no driver, no tasks. Elect this caller as the SOLE
        // close owner (Open -> InProgress), then funnel through the UNIFIED
        // close_resources() authority (C1): component destruction happens
        // before Stopped publication, exactly once, outside lifecycle_mtx_.
        // close_resources() publishes State::Stopped / CloseState::Closed as
        // the single terminal authority. A concurrent caller that lost the
        // election waits here for Closed. No driver exists in these states,
        // so no join is required.
        if (close_state_ == CloseState::InProgress) {
            runtime_cv_.wait(lk, [this] { return close_state_ == CloseState::Closed; });
            return {};
        }
        close_state_ = CloseState::InProgress;
        lk.unlock();
        close_resources();
        return {};
    }

    case State::Starting: {
        // Record startup abort; let start() owner handle rollback+close.
        startup_abort_requested_ = true;
        stop_requested_ = true;
        control_epoch_++;
        runtime_cv_.notify_all();
        wake_handle_.notify();
        // Wait for close to complete.
        runtime_cv_.wait(lk, [this] { return close_state_ == CloseState::Closed; });
        return {};
    }

    case State::Running: {
        // request_stop + drain + join.
        lk.unlock();
        request_stop();
        auto dr = drain();
        if (!dr.has_value()) {
            // Late caller: another shutdown owner may have completed.
            std::lock_guard rlk(lifecycle_mtx_);
            if (close_state_ == CloseState::Closed) return {};
            return dr;
        }
        return join();
    }

    case State::Stopping: {
        lk.unlock();
        auto dr = drain();
        if (!dr.has_value()) {
            std::lock_guard rlk(lifecycle_mtx_);
            if (close_state_ == CloseState::Closed) return {};
            return dr;
        }
        return join();
    }

    case State::Draining: {
        // Wait for drain_complete then join.
        runtime_cv_.wait(lk, [this] {
            return drain_complete_ || state_ == State::Fatal ||
                   close_state_ == CloseState::Closed;
        });
        if (close_state_ == CloseState::Closed) return {};
        lk.unlock();
        return join();
    }

    case State::Stopped: {
        return {};
    }

    case State::Fatal: {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }
    }

    return make_unexpected_void(IoError{IoError::Code::invalid_state});
}

// ---------------------------------------------------------------------------
// Driver thread main loop
// ---------------------------------------------------------------------------
void ApplicationRuntime::driver_main() {
    std::unique_lock lk(lifecycle_mtx_);

    // Signal that we've reached the startup barrier.
    driver_state_ = DriverState::barrier_wait;
#ifdef SLUICE_ASYNC_INTERNAL_TESTING
    barrier_promise_.set_value();
#endif
    runtime_cv_.notify_all();

    // Park at startup barrier until commit or abort (P1-03).
    runtime_cv_.wait(lk, [this] {
        return state_ != State::Starting || startup_abort_requested_;
    });

    if (startup_abort_requested_) {
        // Abort path: exit without entering run_live.
        driver_state_ = DriverState::exited;
        runtime_cv_.notify_all();
        return;
    }

    // Commit: enter the run_live loop.
    driver_state_ = DriverState::in_run_live;
    observed_epoch_ = control_epoch_;
    lk.unlock();

    // Driver re-entry loop (P1-07): run_live may return at QUIESCENT /
    // MW-S3-no-wake / stop-predicate-true boundaries without all work done.
    for (;;) {
        // Enter run_live with the stop predicate.
        sched_->run_live(worker_count_, &stop_predicate_trampoline, this);

        // Returned from run_live: park at invocation boundary.
        lk.lock();
        driver_state_ = DriverState::between_invocations;

        // Check exit conditions.
        if (driver_exit_requested_ || fatal_snapshot_.load(std::memory_order::acquire)) {
            driver_state_ = DriverState::exited;
            runtime_cv_.notify_all();
            return;
        }

        // Check drain_complete: all tasks terminal + no outstanding I/O.
        // P0-1 fix: only publish drain_complete_ in Stopping/Draining. In
        // Running, a quiescent window (zero admitted tasks or momentary gap
        // between task completions) must NOT set drain_complete_ because
        // future submissions are still legal and would see a stale true.
        if (task_set_terminal_snapshot_.load(std::memory_order::acquire) &&
            io_ctx_->outstanding() == 0 &&
            (state_ == State::Stopping || state_ == State::Draining)) {
            drain_complete_ = true;
            runtime_cv_.notify_all();

            // Post-drain park (P2-03): wait until exit requested or epoch change.
            // All admitted tasks are terminal, admission is closed, and no I/O
            // remains, so control changes observed during the completed run no
            // longer carry a Scheduler re-entry obligation.
            observed_epoch_ = control_epoch_;
            driver_state_ = DriverState::drained_wait;
            runtime_cv_.wait(lk, [this] {
                return driver_exit_requested_ ||
                       fatal_snapshot_.load(std::memory_order::acquire) ||
                       control_epoch_ != observed_epoch_;
            });

            if (driver_exit_requested_ ||
                fatal_snapshot_.load(std::memory_order::acquire)) {
                driver_state_ = DriverState::exited;
                runtime_cv_.notify_all();
                return;
            }

            // Epoch changed: re-enter run_live.
            observed_epoch_ = control_epoch_;
            driver_state_ = DriverState::in_run_live;
            lk.unlock();
            continue;
        }

        // A submit/stop/terminal publication may race with the tail of
        // run_live after Scheduler termination was published. Do not overwrite
        // the entry epoch before checking it: pending Scheduler work already
        // has its wake encoded in this difference and requires immediate
        // re-entry, not a wait for a second control event.
        if (control_epoch_ != observed_epoch_) {
            observed_epoch_ = control_epoch_;
            driver_state_ = DriverState::in_run_live;
            lk.unlock();
            continue;
        }

        // Issue #116 (invocation-boundary lost re-entry): run_live may return
        // at an interrupted MW-S2 no-progress boundary while accepted I/O is
        // still outstanding — the Scheduler's caller-owned re-entry contract
        // (phase-g closeout TP-G5) transfers the observation obligation to
        // THIS caller at that boundary. control_epoch_ cannot observe backend
        // progress, so parking on runtime_cv_ with outstanding > 0 strands
        // the obligation permanently: no participant remains to poll, the
        // backend's terminal is recorded into a ready-ring nobody reaps, the
        // suspended task never resumes, and drain_complete_ is unreachable.
        // Re-enter immediately instead. The next invocation re-elects the
        // MW-S2 participant and parks in ctx_.wait_one() — a true park whose
        // own wake protocol (ready/control epochs) is closed — so this is not
        // a poll loop: each re-entry consumes one control interrupt (D4-RM13
        // one-shot baseline), and between interrupts the driver is parked
        // inside wait_one, not spinning here.
        //
        // Park-handshake closure (INV-R3): the check runs under lifecycle_mtx_
        // — the same domain that guards the park below. While outstanding==0,
        // a NEW obligation can appear only via submit/start/stop/terminal
        // publication, every one of which bumps control_epoch_ under this
        // mutex and wakes the parked driver; backend-side progress cannot
        // create a Runtime-visible obligation from zero (it needs an accepted
        // op, and accepting is a submit). No check-to-park window remains.
        if (io_ctx_ && io_ctx_->outstanding() > 0) {
            driver_state_ = DriverState::in_run_live;
            lk.unlock();
            continue;
        }

        // Not done yet: park on persistent CV predicate (P1-07).
        runtime_cv_.wait(lk, [this] {
            return driver_exit_requested_ ||
                   fatal_snapshot_.load(std::memory_order::acquire) ||
                   control_epoch_ != observed_epoch_;
        });

        if (driver_exit_requested_ ||
            fatal_snapshot_.load(std::memory_order::acquire)) {
            driver_state_ = DriverState::exited;
            runtime_cv_.notify_all();
            return;
        }

        // Epoch changed: re-enter run_live.
        observed_epoch_ = control_epoch_;
        driver_state_ = DriverState::in_run_live;
        lk.unlock();
    }
}

// ---------------------------------------------------------------------------
// Stop predicate (called under Scheduler global_mtx_ at MW-S3 boundary).
// Reads ONLY lock-free atomic snapshots. Never acquires lifecycle_mtx_.
// ---------------------------------------------------------------------------
bool ApplicationRuntime::stop_predicate_fn() {
    // The stop predicate fires only when the Runtime is shutting down AND all
    // work is done. admission_closed_snapshot_ prevents premature run_live
    // exit during Running (P0-1): without it, a quiescent gap between task
    // completions would terminate the driver while admission is still open.
    return fatal_snapshot_.load(std::memory_order::acquire) ||
           driver_exit_snapshot_.load(std::memory_order::acquire) ||
           (admission_closed_snapshot_.load(std::memory_order::acquire) &&
            task_set_terminal_snapshot_.load(std::memory_order::acquire));
}

bool ApplicationRuntime::stop_predicate_trampoline(void* ctx) {
    return static_cast<ApplicationRuntime*>(ctx)->stop_predicate_fn();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
void ApplicationRuntime::recompute_task_set_terminal_locked() {
    // task_set_terminal = no admitted tasks OR all admitted tasks are terminal.
    // When admitted_count_ == 0, the task set is trivially terminal.
    bool terminal = (terminal_count_ >= admitted_count_);
    task_set_terminal_snapshot_.store(terminal, std::memory_order::release);
}

void ApplicationRuntime::close_resources() {
    // Transfer ownership of Group, Scheduler, and IoContext to local variables
    // under the lock, then destroy them OUTSIDE the lock. The Group and
    // Scheduler destructors may join workers or trigger completion paths that
    // acquire lifecycle_mtx_ — holding the mutex during destruction would
    // self-deadlock (review finding).
    std::unique_ptr<Group> group;
    std::unique_ptr<Scheduler> sched;
    std::unique_ptr<AsyncIoContext> io_ctx;
    {
        std::lock_guard lk(lifecycle_mtx_);
        // Close admission so no new tasks can be submitted post-close.
        admission_open_ = false;
        admission_closed_snapshot_.store(true, std::memory_order::release);
        group = std::move(root_group_);
        sched = std::move(sched_);
        io_ctx = std::move(io_ctx_);
    }
    // Destroy outside the lock (reverse construction order: Group → Scheduler
    // → IoContext). The backend destructor joins backend workers here.
    group.reset();
    sched.reset();
    io_ctx.reset();
    // Re-acquire to publish Stopped / Closed and wake waiters.
    {
        std::lock_guard lk(lifecycle_mtx_);
        state_ = State::Stopped;
        close_state_ = CloseState::Closed;
        runtime_cv_.notify_all();
    }
}

bool ApplicationRuntime::is_runtime_task() const noexcept {
    // Reads the current Fiber's execution tag. If called from outside a worker
    // thread (e.g. user thread calling drain()), current_worker() returns null
    // and current_fiber_tag() returns nullptr ≠ this, correctly returning false.
    return current_fiber_tag() == this;
}

#ifdef SLUICE_ASYNC_INTERNAL_TESTING
void ApplicationRuntime::test_dump_forensics(const char* tag) {
    // Issue #116 liveness forensics (investigation Phase 2). Called by a
    // forensics watchdog on its bounded-timeout path, where the run is
    // presumed permanently stalled: the driver is parked between invocations
    // (lifecycle_mtx_ uncontended) and Scheduler workers have exited (every
    // Scheduler lock uncontended). Each domain is still read under its own
    // lock so the dump is race-free even when that presumption is wrong.
    // Absent from the installed production build.
    static const char* kStateName[] = {"Constructed", "Starting", "Running",
                                       "Stopping",   "Draining", "Stopped",
                                       "StartFailed", "Fatal"};
    static const char* kDriverName[] = {"not_started", "barrier_wait",
                                        "in_run_live", "between_invocations",
                                        "drained_wait", "exited"};
    State s;
    DriverState ds;
    std::uint64_t ctrl, obs;
    std::size_t admitted, terminal;
    bool drain_done, stop;
    {
        std::lock_guard lk(lifecycle_mtx_);
        s = state_;
        ds = driver_state_;
        ctrl = control_epoch_;
        obs = observed_epoch_;
        admitted = admitted_count_;
        terminal = terminal_count_;
        drain_done = drain_complete_;
        stop = stop_requested_;
    }
    std::fprintf(stderr,
                 "[issue116-forensics] runtime %s: state=%s driver=%s "
                 "control_epoch=%lu observed_epoch=%lu admitted=%zu "
                 "terminal=%zu drain_complete=%d stop_requested=%d\n",
                 tag, kStateName[static_cast<int>(s)],
                 kDriverName[static_cast<int>(ds)],
                 static_cast<unsigned long>(ctrl),
                 static_cast<unsigned long>(obs), admitted, terminal,
                 (int)drain_done, (int)stop);
    if (io_ctx_) {
        std::fprintf(stderr,
                     "[issue116-forensics] io %s: accepted_outstanding=%zu\n",
                     tag, io_ctx_->outstanding());
    }
    if (sched_) {
        Scheduler::AsyncTestAccess::dump_park_forensics(*sched_, tag);
    }
}
#endif  // SLUICE_ASYNC_INTERNAL_TESTING

}  // namespace sluice::async
