// ScriptedAsyncBackend — deterministic, scriptable test backend for async I/O.
//
// Implements the AsyncBackend interface with:
//   - Monotonic, stable operation IDs
//   - ID-based (not FIFO) completion: any pending op can be completed in any order
//   - Short read/write completion
//   - Error completion for read/write/sync
//   - Submit failure injection (by number or by kind)
//   - Outstanding statistics (max reads, max total)
//   - Pending inspection (count, by-kind, by-offset)
//   - Thread-safe: submit/poll from Runtime workers, control from test thread
//   - Condition-variable based waiting (no sleeps)
//   - Drain verification (expect_no_pending)
//
// Completions are staged by the test thread (complete_*) and applied to the
// actual Completion objects inside poll()/wait_one(), matching the ADR A3/O1
// rule that completions are produced only inside poll/wait_one.
//
// Operation IDs are monotonic starting from 1. The ID is stable for the
// lifetime of the pending operation and is never reused within a single backend
// instance. This makes it safe for tests to capture IDs and use them later.
//
// Buffer lifecycle: the backend stores NON-OWNING pointers to caller-owned
// buffers. complete_read_with_data() copies data into the read buffer before
// completing. captured_write_bytes() returns a copy of the write buffer data
// (valid only while the operation is pending).
//
// --- Control-plane lifetime model (Phase 0) -------------------------------
//
// The backend object is owned by a Runtime/copy thread (a unique_ptr). The test
// thread must NEVER hold or dereference a backend raw pointer across threads:
// checking `destroyed` then calling a method is a TOCTOU window.
//
// Instead the test thread drives everything through a `ScriptedBackendController`
// that shares a `shared_ptr<ScriptedBackendSharedState>` with the backend. The
// shared state outlives the backend object. When the backend is destroyed by the
// Runtime thread it only marks the shared state `closed`; it does NOT destroy the
// shared state. Controller methods, after the backend is closed, return an
// explicit `closed` result (or throw a clearly-named test error) and never touch
// the destroyed backend object.
//
// Thread safety: all shared-state methods acquire the shared-state mutex. The
// condition variable is signaled when a pending operation appears, a staged
// result is available, or the backend transitions to closed. TSan must pass on
// all tests using this backend.

#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sluice::async {

// --- OpKind ------------------------------------------------------------------
enum class OpKind : std::uint8_t {
    read,
    write,
    sync_data,
    sync_all,
};

inline const char* to_string(OpKind k) {
    switch (k) {
    case OpKind::read:      return "read";
    case OpKind::write:     return "write";
    case OpKind::sync_data: return "sync_data";
    case OpKind::sync_all:  return "sync_all";
    }
    return "unknown";
}

// --- PendingOpView (read-only snapshot for test inspection) ------------------
struct PendingOpView {
    std::uint64_t id;
    OpKind kind;
    int fd;
    std::uint64_t offset;
    std::size_t length;
    void* buffer;               // non-owning; valid only while pending
    void* completion_identity;  // address of the Completion object
};

// Per-op lifecycle stage used for accurate outstanding accounting.
enum class OpStage : std::uint8_t {
    pending,   // submitted, awaiting test-thread completion control
    staged,    // test thread staged a result; awaiting poll()/wait_one()
};

// Result of a bounded wait for pending operations.
enum class WaitStatus {
    ready,    // pending count >= requested count
    timeout,  // bounded wait elapsed before the requested count arrived
    closed,   // the backend was destroyed (closed) while waiting
};

// Thrown by control methods when an op is not found / already completed / wrong
// kind. Test harnesses catch std::runtime_error.
struct ScriptedBackendError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Thrown when a control method is invoked after the backend has been closed
// (destroyed). Lets tests distinguish "used after close" from generic errors.
struct ScriptedBackendClosed : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// --- ScriptedBackendSharedState ---------------------------------------------
// Shared control block. Outlives the backend object via shared_ptr: the test
// thread holds a shared_ptr to this state; the backend holds another. When the
// backend is destroyed (Runtime thread) it only sets `closed` and notifies
// waiters — it never destroys the shared state.
//
// All access goes through a single mutex (mtx). The condition variable is
// notified on: pending add, staged result, backend close.
class ScriptedBackendSharedState {
public:
    ScriptedBackendSharedState() = default;

    ScriptedBackendSharedState(const ScriptedBackendSharedState&) = delete;
    ScriptedBackendSharedState& operator=(const ScriptedBackendSharedState&) = delete;

    mutable std::mutex mtx;
    std::condition_variable cv;

    bool closed = false;  // set under mtx by the backend destructor; never cleared

    // --- Pending operations (caller holds mtx) ---
    struct PendingSizeOp {
        std::uint64_t id;
        OpKind kind;  // read or write
        OpStage stage = OpStage::pending;  // pending -> staged
        int fd;
        std::uint64_t offset;
        std::size_t length;
        std::byte* buffer;
        Completion<std::size_t>* completion;
    };

    struct PendingVoidOp {
        std::uint64_t id;
        OpKind kind;  // sync_data or sync_all
        OpStage stage = OpStage::pending;
        int fd;
        Completion<void>* completion;
    };

    // Pending AND staged operations, keyed by operation ID. An operation remains
    // in these maps (with stage=pending or stage=staged) until poll()/wait_one()
    // applies its result; only then is it erased. This keeps
    // outstanding() == pending + staged exactly and preserves diagnostics.
    std::map<std::uint64_t, PendingSizeOp> size_ops;
    std::map<std::uint64_t, PendingVoidOp> void_ops;

    // Staged results to be applied by poll(). A pointer to the matching op
    // stays in the size_ops/void_ops map (stage=staged) until poll applies it.
    struct StagedSizeResult {
        Completion<std::size_t>* completion;
        Result<std::size_t> result;
    };
    struct StagedVoidResult {
        Completion<void>* completion;
        Result<void> result;
    };
    std::vector<StagedSizeResult> staged_size;
    std::vector<StagedVoidResult> staged_void;

    // Monotonic operation ID counter.
    std::uint64_t next_id = 1;

    // Outstanding statistics (peak over pending+staged).
    std::size_t max_reads = 0;
    std::size_t max_total = 0;

    // Submit failure injection state.
    std::optional<std::uint64_t> fail_submit_num;
    IoError fail_submit_num_error{IoError::Code::backend_error};
    std::optional<OpKind> fail_next_kind;
    IoError fail_next_kind_error{IoError::Code::backend_error};

    // Submit counter (for fail_submit_number).
    std::uint64_t submit_count = 0;

    // Shutdown flag observed by wait_one().
    bool shutdown = false;

    // Snapshot of outstanding count at destruction (for diagnostics).
    std::size_t final_outstanding = 0;
};

// --- ScriptedAsyncBackend ----------------------------------------------------
// The AsyncBackend implementation. Owns a shared_ptr to SharedState; delegates
// submit/poll/wait_one/cancel/outstanding to the shared state. The destructor
// marks the shared state closed (under its mutex) and notifies waiters; it never
// destroys the shared state (the controller may still hold a reference).
class ScriptedAsyncBackend : public AsyncBackend {
public:
    explicit ScriptedAsyncBackend(std::shared_ptr<ScriptedBackendSharedState> state);
    ~ScriptedAsyncBackend() override;

    ScriptedAsyncBackend(const ScriptedAsyncBackend&) = delete;
    ScriptedAsyncBackend& operator=(const ScriptedAsyncBackend&) = delete;

    // The shared control state (test thread). The returned shared_ptr remains
    // valid after the backend is destroyed.
    std::shared_ptr<ScriptedBackendSharedState> shared_state() const noexcept {
        return state_;
    }

    // --- AsyncBackend interface (called by Runtime worker/driver) ---
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override;
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override;
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override;
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override;

    std::size_t poll() override;
    Result<std::size_t> wait_one() override;

    void cancel(Completion<std::size_t>& c) override;
    void cancel(Completion<void>& c) override;

    std::size_t outstanding() const noexcept override;

private:
    std::shared_ptr<ScriptedBackendSharedState> state_;

    // Internal poll (caller holds state_->mtx).
    static std::size_t poll_locked(ScriptedBackendSharedState& s);

    // Shared submit bookkeeping (caller holds mtx). Validates submit-failure
    // injection; returns the assigned op id and sets `injected` on failure.
    template <class PendingT>
    static void assign_outstanding_stats(ScriptedBackendSharedState& s);
};

// --- ScriptedBackendController ----------------------------------------------
// The test-thread control surface. Holds a shared_ptr to the same SharedState
// as the backend; all control operations go through the shared state's mutex.
// After the backend is closed (destroyed), inspection/statistics methods still
// return their last-observed values safely (the data lives in the shared state),
// while completion-control methods throw ScriptedBackendClosed so the test does
// not silently drive a dead backend.
//
// The controller is safe to construct BEFORE the backend exists and to use
// AFTER the backend is destroyed.
class ScriptedBackendController {
public:
    ScriptedBackendController() = default;
    explicit ScriptedBackendController(std::shared_ptr<ScriptedBackendSharedState> state)
        : state_(std::move(state)) {}

    bool valid() const noexcept { return state_ != nullptr; }
    bool closed() const;  // true once the backend has been destroyed

    // --- Pending inspection ---
    std::size_t pending_count();        // pending + staged (everything outstanding)
    std::size_t pending_read_count();   // read ops pending + staged
    std::size_t pending_write_count();  // write ops pending + staged
    std::size_t pending_sync_count();   // sync ops pending + staged
    std::size_t max_outstanding_reads();
    std::size_t max_outstanding_total();
    std::vector<PendingOpView> pending_operations();

    // Find a pending/staged read/write by offset. Returns the operation ID.
    std::optional<std::uint64_t> find_read_by_offset(std::uint64_t offset);
    std::optional<std::uint64_t> find_write_by_offset(std::uint64_t offset);

    // --- Completion control (test thread) ---
    //
    // These throw ScriptedBackendError for op-not-found / wrong-kind / double
    // completion / out-of-range bytes, and ScriptedBackendClosed if the backend
    // is already destroyed.

    // Complete a read/write operation with a byte count. n may be less than the
    // requested length (short completion). n must be <= requested length.
    void complete_bytes(std::uint64_t op_id, std::size_t n);

    // Complete a read operation with EOF (0 bytes). Only valid for reads.
    void complete_eof(std::uint64_t op_id);

    // Complete a read/write operation with an error.
    void complete_error(std::uint64_t op_id, IoError error);

    // Complete a sync operation with success. Only valid for sync ops.
    void complete_sync_success(std::uint64_t op_id);

    // Complete a sync operation with an error. Only valid for sync ops.
    void complete_sync_error(std::uint64_t op_id, IoError error);

    // Complete a read operation: copy `len` bytes of `data` into the read
    // buffer, then complete with `len` bytes. Validates read kind + length.
    void complete_read_with_data(std::uint64_t op_id, const std::byte* data,
                                 std::size_t len);

    // Capture the bytes currently in a pending write operation's buffer.
    // Returns a copy. The operation must be outstanding.
    std::vector<std::byte> captured_write_bytes(std::uint64_t op_id);

    // --- Submit failure injection (test thread) ---
    void fail_next_submit(OpKind kind, IoError error);
    void fail_submit_number(std::uint64_t n, IoError error);

    // --- Waiting (test thread) ---
    //
    // wait_until_pending: block until >= min_count ops are outstanding OR the
    // backend is closed. Closed is surfaced as WaitStatus::closed (NOT ready),
    // so a test never mistakes "backend gone" for "ops arrived".

    // Unbounded wait (no timeout). Returns ready or closed.
    WaitStatus wait_until_pending(std::size_t min_count);
    // Bounded wait. Returns ready/timeout/closed.
    WaitStatus wait_until_pending_for(std::size_t min_count,
                                      std::chrono::milliseconds timeout);

    // --- Drain verification (test thread) ---
    //
    // Assert that nothing is outstanding (pending + staged + unconsumed-staged
    // results all empty). Throws ScriptedBackendError with diagnostics if not.
    void expect_no_outstanding();

    // Convenience: best-effort complete-everything-pending (used by RAII
    // cleanup). Safe to call after close (no-op). Reads/syncs completed with
    // their requested length/EOF; writes with their length.
    void complete_all_for_cleanup();

private:
    std::shared_ptr<ScriptedBackendSharedState> state_;

    void require_open_locked(const char* fn) const;
};

// --- Factory -----------------------------------------------------------------
// Create a backend + controller pair sharing the same control state.
struct ScriptedBackendPair {
    std::unique_ptr<ScriptedAsyncBackend> backend;
    ScriptedBackendController controller;
};

inline ScriptedBackendPair make_scripted_backend() {
    auto state = std::make_shared<ScriptedBackendSharedState>();
    auto backend = std::make_unique<ScriptedAsyncBackend>(state);
    ScriptedBackendController controller(state);
    return {std::move(backend), std::move(controller)};
}

}  // namespace sluice::async
