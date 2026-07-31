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
// Thread safety: all public methods acquire a std::mutex. The condition variable
// is signaled when a pending operation appears or a staged result is available.
// TSan must pass on all tests using this backend.

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
#include <mutex>
#include <optional>
#include <string>
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

// --- ScriptedAsyncBackend ----------------------------------------------------
class ScriptedAsyncBackend : public AsyncBackend {
public:
    ScriptedAsyncBackend();
    ~ScriptedAsyncBackend() override;

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

    // --- Pending inspection (test thread) ---
    std::size_t pending_count();
    std::size_t pending_read_count();
    std::size_t pending_write_count();
    std::size_t pending_sync_count();
    std::size_t max_outstanding_reads();
    std::size_t max_outstanding_total();
    std::vector<PendingOpView> pending_operations();

    // Find a pending read/write by offset. Returns the operation ID if found.
    std::optional<std::uint64_t> find_read_by_offset(std::uint64_t offset);
    std::optional<std::uint64_t> find_write_by_offset(std::uint64_t offset);

    // --- Completion control (test thread) ---

    // Complete a read/write operation with a byte count. n may be less than
    // the requested length (short completion). n must be <= requested length.
    void complete_bytes(std::uint64_t op_id, std::size_t n);

    // Complete a read operation with EOF (0 bytes).
    void complete_eof(std::uint64_t op_id);

    // Complete a read/write operation with an error.
    void complete_error(std::uint64_t op_id, IoError error);

    // Complete a sync operation with success.
    void complete_sync_success(std::uint64_t op_id);

    // Complete a sync operation with an error.
    void complete_sync_error(std::uint64_t op_id, IoError error);

    // Complete a read operation: copy `len` bytes of `data` into the read
    // buffer, then complete with `len` bytes. Validates that the target is a
    // read op and that len <= requested length.
    void complete_read_with_data(std::uint64_t op_id, const std::byte* data,
                                 std::size_t len);

    // Capture the bytes currently in a pending write operation's buffer.
    // Returns a copy of the buffer contents. The operation must be pending.
    std::vector<std::byte> captured_write_bytes(std::uint64_t op_id);

    // --- Submit failure injection (test thread) ---

    // Make the next submit of the given kind fail with the given error.
    // Only affects one submit; resets after the failure is consumed.
    void fail_next_submit(OpKind kind, IoError error);

    // Make the Nth submit (overall, across all kinds) fail with the given
    // error. Only affects one submit; resets after the failure is consumed.
    void fail_submit_number(std::uint64_t n, IoError error);

    // --- Waiting (test thread) ---

    // Block until at least `min_count` operations are pending.
    void wait_until_pending(std::size_t min_count);

    // Block until at least `min_count` operations are pending, or `timeout`
    // elapses. Returns true if the condition was met, false on timeout.
    bool wait_until_pending_for(std::size_t min_count,
                                std::chrono::milliseconds timeout);

    // --- Drain verification (test thread) ---

    // Assert that no operations are pending. Records a test failure (via the
    // test harness) if any are pending. Does NOT throw or terminate.
    void expect_no_pending();

private:
    // --- Internal pending operation representation ---
    struct PendingSizeOp {
        std::uint64_t id;
        OpKind kind;  // read or write
        int fd;
        std::uint64_t offset;
        std::size_t length;
        std::byte* buffer;
        Completion<std::size_t>* completion;
    };

    struct PendingVoidOp {
        std::uint64_t id;
        OpKind kind;  // sync_data or sync_all
        int fd;
        Completion<void>* completion;
    };

    // Staged results to be applied by poll().
    struct StagedSizeResult {
        Completion<std::size_t>* completion;
        Result<std::size_t> result;
    };

    struct StagedVoidResult {
        Completion<void>* completion;
        Result<void> result;
    };

    // Internal helpers (caller must hold mtx_).
    std::size_t poll_locked();
    std::uint64_t next_id_locked();

    mutable std::mutex mtx_;
    std::condition_variable cv_;

    // Pending operations, keyed by operation ID.
    std::map<std::uint64_t, PendingSizeOp> pending_size_;
    std::map<std::uint64_t, PendingVoidOp> pending_void_;

    // Staged completions (populated by complete_*, consumed by poll).
    std::vector<StagedSizeResult> staged_size_;
    std::vector<StagedVoidResult> staged_void_;

    // Monotonic operation ID counter.
    std::uint64_t next_id_{1};

    // Outstanding statistics.
    std::size_t max_reads_{0};
    std::size_t max_total_{0};

    // Submit failure injection state.
    std::optional<std::uint64_t> fail_submit_num_;
    IoError fail_submit_num_error_{IoError::Code::backend_error};
    std::optional<OpKind> fail_next_kind_;
    IoError fail_next_kind_error_{IoError::Code::backend_error};

    // Submit counter (for fail_submit_number).
    std::uint64_t submit_count_{0};

    // Shutdown flag.
    bool shutdown_{false};

    // For expect_no_pending: stores the failure message if check fails.
    // We can't include the test harness here, so we use a callback approach.
    // Instead, expect_no_pending records failures via a simple mechanism.
    mutable std::vector<std::string> pending_failures_;
};

}  // namespace sluice::async