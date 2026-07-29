// sluice::async::ApplicationRuntime — E16 implementation.
// ADR: docs/adr/ADR-application-runtime.md (Accepted).
// Design: docs/design/e16-application-runtime.md.
#include <sluice/async/application_runtime.hpp>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/group.hpp>
#include <sluice/async/scheduler.hpp>

#include <cassert>
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

    if (state_ != State::Constructed) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    // Remember stop_requested: if stop was called before start, return canceled.
    if (stop_requested_) {
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
    // complete the abort path directly.
    if (driver_state_ == DriverState::exited) {
        state_ = State::Stopped;
        close_state_ = CloseState::Closed;
        runtime_cv_.notify_all();
        lk.unlock();
        if (driver_thread_.joinable()) driver_thread_.join();
        return make_unexpected_void(IoError{IoError::Code::canceled});
    }

    // Startup barrier: the driver is at barrier_wait. Commit or abort.
    if (stop_requested_) {
        // Stop won the race pre-commit (P1-03 abort path).
        startup_abort_requested_ = true;
        control_epoch_++;
        runtime_cv_.notify_all();
        // Wait for driver to exit.
        runtime_cv_.wait(lk, [this] {
            return driver_state_ == DriverState::exited;
        });
        state_ = State::Stopped;
        close_state_ = CloseState::Closed;
        runtime_cv_.notify_all();
        lk.unlock();
        if (driver_thread_.joinable()) driver_thread_.join();
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

            // RuntimeTaskContext delegates I/O to io_ctx_.
            RuntimeTaskContext ctx(*io_ctx_, token);

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
        // Direct close: no driver, no tasks.
        close_state_ = CloseState::InProgress;
        state_ = State::Stopped;
        close_state_ = CloseState::Closed;
        runtime_cv_.notify_all();
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
        observed_epoch_ = control_epoch_;

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

}  // namespace sluice::async
