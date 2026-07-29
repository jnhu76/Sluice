// sluice::async::ApplicationRuntime — E16 application lifecycle layer.
//
// A builder-constructed, one-shot, injected-backend Application Runtime driven
// by a single dedicated driver thread. Owns AsyncIoContext, Scheduler, root
// Group, root cancellation, and the driver-thread lifecycle. Exposes a unified
// start / submit / request_stop / drain / join / shutdown contract.
//
// ADR: docs/adr/ADR-application-runtime.md (Accepted 2026-07-29).
// Design: docs/design/e16-application-runtime.md.
// Formal: spec/tla/e16_application_runtime/.
//
// Non-copyable, non-movable. Constructed on the heap via RuntimeBuilder::build()
// which returns Result<std::unique_ptr<ApplicationRuntime>> (P1-02: stable
// address anchors driver captures, Fiber-local tag, lifecycle mutex/CV).
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/cancel.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#ifdef SLUICE_ASYNC_INTERNAL_TESTING
#include <future>
#endif

namespace sluice::async {

// Forward declarations.
class Group;
class Fiber;
class ApplicationRuntime;

// ---------------------------------------------------------------------------
// RuntimeTaskContext (P1-04): restricted, non-owning task execution context.
// Valid only during one RuntimeTaskFn invocation; delegates I/O to the
// Runtime-owned AsyncIoContext. No spawn capability in E16 v1.
//
// M1-A (docs/design/m1-runtime-io-await-race.md): added a cooperative
// Completion wait (await_completion) so a task can suspend until a submitted,
// caller-owned Completion reaches a terminal result. This is the application-
// discovered Runtime I/O wait gap (M1-API-GAP-1). The capability is backed by
// the already-audited Scheduler::await_completion_* primitive (E6-T2/E10/E11
// regression-proven against ThreadPoolBackend); the Scheduler* is PRIVATE and
// set only by ApplicationRuntime (friend), never escaping to task code.
// ---------------------------------------------------------------------------
class RuntimeTaskContext {
public:
    CancelToken& cancel_token() noexcept;

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c);
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c);
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c);
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c);

    // M1-A: cooperatively await a submitted, outstanding Completion. Returns
    // inline (no suspend) if the Completion is already ready; otherwise
    // suspends the calling Fiber exactly once and resumes exactly once when
    // the Completion reaches a terminal result. The result remains in the
    // Completion — read it via c.result() after this returns, then c.reset()
    // before reuse (L7/L9 lifecycle).
    //
    // Preconditions:
    //   - `c` is outstanding against THIS Runtime's AsyncIoContext (a prior
    //     submit_* on this context marked it outstanding). Awaiting an idle
    //     Completion is a caller contract violation (Debug asserts; Release
    //     documents). Mirrors the underlying Scheduler primitive precondition
    //     and Completion::result() L9 policy.
    //   - called only from within a Runtime task (the RuntimeTaskContext&
    //     lifetime is the task invocation). The context is non-owning and
    //     valid only during that invocation.
    //
    // Authority: delegates to the private Scheduler*; the pointer never
    // escapes. submit-time errors stay synchronous (from submit_*); completion
    // errors stay terminal results in the Completion.
    void await_completion(Completion<std::size_t>& c);
    void await_completion(Completion<void>& c);

#ifdef SLUICE_ASYNC_INTERNAL_TESTING
    // Suspend the current Fiber until `flag` becomes true. Uses the Scheduler's
    // level-triggered ready-flag protocol (await_ready_flag). The flag must be
    // set from another Fiber or external thread to resume this Fiber.
    //
    // TEST-ONLY: this capability exists solely to prove Fiber-local identity
    // survives suspension (C2-T3). It is not part of the public E16 API and is
    // not available in installed (non-internal-testing) builds.
    void suspend(std::atomic<bool>& flag);
#endif

    RuntimeTaskContext(const RuntimeTaskContext&) = delete;
    RuntimeTaskContext& operator=(const RuntimeTaskContext&) = delete;

private:
    friend class ApplicationRuntime;

    // M1-A: the Scheduler* is now part of the PRODUCTION object layout so the
    // cooperative Completion wait can delegate to await_completion_*. Only
    // ApplicationRuntime (friend) constructs the context; the pointer never
    // escapes and task code cannot retrieve it. The internal-testing
    // constructor previously carried the Scheduler for the test-only suspend();
    // both builds now share the same layout (sched_ is no longer test-only).
    RuntimeTaskContext(AsyncIoContext& ctx, CancelToken& token,
                       Scheduler& sched) noexcept
        : ctx_(&ctx), token_(&token), sched_(&sched) {}

    AsyncIoContext* ctx_;
    CancelToken* token_;
    Scheduler* sched_;
};

// The task function signature. Receives a RuntimeTaskContext& for I/O and
// cancellation observation.
using RuntimeTaskFn = std::function<void(RuntimeTaskContext&)>;

// ---------------------------------------------------------------------------
// RuntimeBuilder: collects configuration; build() validates, constructs on the
// heap, and returns Result<std::unique_ptr<ApplicationRuntime>> (P1-02).
// ---------------------------------------------------------------------------
class RuntimeBuilder {
public:
    RuntimeBuilder() = default;

    // Inject the backend (required). The Runtime owns the AsyncIoContext which
    // owns this backend.
    RuntimeBuilder& backend(std::unique_ptr<AsyncBackend> b);

    // Set the worker count for Scheduler::run_live invocations. Default: 1.
    RuntimeBuilder& workers(unsigned n);

    // Validate configuration and construct the Runtime on the heap.
    // Returns invalid_state if no backend is provided.
    Result<std::unique_ptr<ApplicationRuntime>> build();

private:
    std::unique_ptr<AsyncBackend> backend_;
    unsigned workers_ = 1;
};

// ---------------------------------------------------------------------------
// ApplicationRuntime: the E16 lifecycle owner.
//
// Lifecycle: Constructed → Starting → Running → Stopping → Draining → Stopped.
// Failure: StartFailed, Fatal (std::terminate).
//
// Thread safety: all public methods are safe to call from any thread except
// drain()/join()/shutdown() which return invalid_state when called from a task
// owned by this Runtime (detected via a Fiber-local execution tag stored in the
// current Fiber's execution_tag_ field). Unlike thread_local, a Fiber-local tag
// survives Fiber suspend/resume and is correct under multiplexing (one OS
// worker runs many Fibers; a TLS guard does not follow Fiber context switches).
// ---------------------------------------------------------------------------
class ApplicationRuntime {
public:
    ~ApplicationRuntime();

    ApplicationRuntime(const ApplicationRuntime&) = delete;
    ApplicationRuntime& operator=(const ApplicationRuntime&) = delete;
    ApplicationRuntime(ApplicationRuntime&&) = delete;
    ApplicationRuntime& operator=(ApplicationRuntime&&) = delete;

    // Spawn the driver thread and transition to Running.
    // Returns canceled if stop was requested before commit.
    // Returns invalid_state if not in Constructed.
    Result<void> start();

    // Admit a task for execution. Admission-gated: returns invalid_state if
    // admission is closed (not Running, or stop_requested).
    Result<void> submit(RuntimeTaskFn task);

    // Request cooperative stop. noexcept, idempotent, worker-safe.
    // Publishes root cancellation under lifecycle_mutex in Running (P1-01).
    void request_stop() noexcept;

    // Wait for all admitted tasks to complete and all outstanding I/O to drain.
    // Legal only in Stopping or Draining. Returns invalid_state in Running
    // (caller must request_stop() first) or from a Runtime task.
    Result<void> drain();

    // Post-drain close: join driver, destroy resources, publish Stopped.
    // Legal only after drain_complete. Returns invalid_state from a Runtime task.
    Result<void> join();

    // State-dispatched lifecycle operation (P1-05). Correct in every state.
    // One close owner elected across all concurrent callers.
    Result<void> shutdown();

#ifdef SLUICE_ASYNC_INTERNAL_TESTING
    // Test-only seam: returns a future that becomes ready when the driver
    // thread reaches the startup barrier (barrier_wait). Used by startup-abort
    // tests to deterministically observe the Starting phase before injecting
    // stop/shutdown.
    std::future<void> test_driver_barrier_reached() {
        return barrier_promise_.get_future();
    }

    // Test-only seam: enable the commit checkpoint pause. When enabled, the
    // start owner parks at the commit checkpoint (after the barrier wait,
    // immediately before checking stop_requested_) until
    // test_release_start_owner_at_commit_checkpoint() is called. This lets a
    // test inject stop/shutdown between the barrier-wait wake and the
    // stop_requested_ check, deterministically forcing the startup-abort path
    // (start() == canceled). OFF by default so that tests that do not need the
    // pause (e.g. the suspend/resume identity test) are not blocked.
    void test_set_pause_at_commit_checkpoint(bool enable) {
        test_pause_at_commit_checkpoint_.store(enable, std::memory_order::release);
    }

    // Test-only seam: returns a future that becomes ready when the start owner
    // reaches the commit checkpoint (after the barrier wait, immediately before
    // checking stop_requested_). The test can then call shutdown()/request_stop()
    // to set stop_requested_ BEFORE the start owner checks it, deterministically
    // forcing the startup-abort path (start() == canceled). The start owner
    // blocks on commit_release_flag_ until test_commit_release() is called,
    // giving the test time to inject stop.
    std::future<void> test_start_owner_at_commit_checkpoint() {
        return commit_checkpoint_promise_.get_future();
    }

    // Test-only seam: releases the start owner that is parked at the commit
    // checkpoint. After this call, the start owner proceeds to check
    // stop_requested_ and (if stop was injected) takes the abort path.
    void test_release_start_owner_at_commit_checkpoint() {
        commit_release_flag_.store(true, std::memory_order::release);
        runtime_cv_.notify_all();
    }

    // Issue #50 deterministic Scheduler topology regression. The returned
    // reference remains owned by this Runtime and is valid only before terminal
    // close. Absent from the installed production build.
    Scheduler& test_scheduler_for_worker_topology() noexcept { return *sched_; }
#endif

private:
    friend class RuntimeBuilder;
    friend class RuntimeTaskContext;

    // Construction via builder only.
    ApplicationRuntime(std::unique_ptr<AsyncBackend> backend, unsigned workers);

    // --- Lifecycle state ---
    enum class State : std::uint8_t {
        Constructed,
        Starting,
        Running,
        Stopping,
        Draining,
        Stopped,
        StartFailed,
        Fatal,
    };

    // --- Close ownership (P1-05) ---
    enum class CloseState : std::uint8_t {
        Open,
        InProgress,
        Closed,
    };

    // --- Driver state machine (P1-07) ---
    enum class DriverState : std::uint8_t {
        not_started,
        barrier_wait,
        in_run_live,
        between_invocations,
        drained_wait,
        exiting,
        exited,
    };

    // --- Internal helpers ---
    void driver_main();
    bool stop_predicate_fn();
    static bool stop_predicate_trampoline(void* ctx);
    void recompute_task_set_terminal_locked();
    void close_resources();
    bool is_runtime_task() const noexcept;

    // --- Owned components (destroyed at close) ---
    std::unique_ptr<AsyncIoContext> io_ctx_;
    std::unique_ptr<Scheduler> sched_;
    std::unique_ptr<Group> root_group_;
    SchedulerWakeHandle wake_handle_;

    // --- Configuration ---
    unsigned worker_count_;

    // --- Lifecycle state (under lifecycle_mtx_) ---
    mutable std::mutex lifecycle_mtx_;
    std::condition_variable runtime_cv_;
    State state_{State::Constructed};
    CloseState close_state_{CloseState::Open};
    DriverState driver_state_{DriverState::not_started};
    bool admission_open_{false};
    bool stop_requested_{false};
    bool startup_abort_requested_{false};
    bool root_cancel_published_{false};
    bool drain_complete_{false};
    bool driver_exit_requested_{false};
    std::uint64_t control_epoch_{0};
    std::uint64_t observed_epoch_{0};
    std::size_t admitted_count_{0};
    std::size_t terminal_count_{0};

    // --- Atomic snapshots for stop predicate (lock-free) ---
    std::atomic<bool> fatal_snapshot_{false};
    std::atomic<bool> driver_exit_snapshot_{false};
    std::atomic<bool> task_set_terminal_snapshot_{true};  // Initially true (no tasks).
    std::atomic<bool> admission_closed_snapshot_{false};  // true when admission closed.

    // --- Driver thread ---
    std::thread driver_thread_;
    bool driver_spawned_{false};

    // --- Fiber-local execution identity (P1-04 worker-call detection) ---
    // Tag value is `this`; stored in the current Fiber's execution_tag_ field
    // so it survives Fiber suspend/resume and is correct under multiplexing.
    // Unlike thread_local, a Fiber-local tag follows the Fiber across context
    // switches, not the OS thread.
    //
    // Access helpers:
    static void set_current_fiber_tag(ApplicationRuntime* rt) noexcept;
    static ApplicationRuntime* current_fiber_tag() noexcept;

private:
    // ... (existing fields below)

#ifdef SLUICE_ASYNC_INTERNAL_TESTING
    std::promise<void> barrier_promise_;
    std::promise<void> commit_checkpoint_promise_;
    std::atomic<bool> commit_release_flag_{false};
    std::atomic<bool> test_pause_at_commit_checkpoint_{false};
#endif
};

}  // namespace sluice::async
