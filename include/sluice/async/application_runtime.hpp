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

namespace sluice::async {

// Forward declarations.
class Group;
class Fiber;
class ApplicationRuntime;

// ---------------------------------------------------------------------------
// RuntimeTaskContext (P1-04): restricted, non-owning task execution context.
// Valid only during one RuntimeTaskFn invocation; delegates I/O to the
// Runtime-owned AsyncIoContext. No spawn capability in E16 v1.
// ---------------------------------------------------------------------------
class RuntimeTaskContext {
public:
    CancelToken& cancel_token() noexcept;

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c);
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c);
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c);
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c);

    RuntimeTaskContext(const RuntimeTaskContext&) = delete;
    RuntimeTaskContext& operator=(const RuntimeTaskContext&) = delete;

private:
    friend class ApplicationRuntime;
    RuntimeTaskContext(AsyncIoContext& ctx, CancelToken& token) noexcept
        : ctx_(&ctx), token_(&token) {}

    AsyncIoContext* ctx_;
    CancelToken* token_;
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
// owned by this Runtime (detected via a thread-local execution tag; v1 seam —
// correct for E16 v1 Evented mode where each task body runs to completion
// within a single Fiber scheduling slice; a Fiber-local tag will replace this
// for full multiplexing safety).
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
    void publish_epoch_and_wake();
    void close_resources();
    bool is_runtime_task() const noexcept;

    // --- Owned components (destroyed at close) ---
    AsyncIoContext io_ctx_;
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
    // Tag value is `this`; stored as thread_local (v1 seam; see class comment).
    static thread_local ApplicationRuntime* current_runtime_tls_;
};

}  // namespace sluice::async
