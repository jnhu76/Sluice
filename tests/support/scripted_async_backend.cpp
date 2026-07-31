// ScriptedAsyncBackend implementation.
//
// Thread-safety model:
//   - All public methods acquire mtx_ (except outstanding() which uses an
//     atomic snapshot approach via the lock for consistency).
//   - submit_* are called from Runtime worker threads (under
//     AsyncIoContext::access_mtx_).
//   - complete_* and inspection methods are called from the test thread.
//   - poll()/wait_one() are called from the Runtime driver thread.
//   - The condition variable cv_ is notified when a pending operation is
//     added (submit_*) or a staged result is ready (complete_*).

#include "scripted_async_backend.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace sluice::async {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ScriptedAsyncBackend::ScriptedAsyncBackend() = default;

ScriptedAsyncBackend::~ScriptedAsyncBackend() {
    // Debug-assert that there are no pending operations. In a test, this
    // catches the case where production code failed to drain pending ops.
    // We do NOT silently cancel them — that would mask bugs.
    //
    // Use a simple assert; the test harness can also call expect_no_pending()
    // before destruction for a more descriptive failure.
    std::lock_guard<std::mutex> lk(mtx_);
    shutdown_ = true;
    cv_.notify_all();
    // We intentionally do NOT complete pending ops here. If the test doesn't
    // drain, the assert fires. This is a debug-only check; in Release, the
    // test would typically call expect_no_pending() explicitly.
    assert(pending_size_.empty() && pending_void_.empty() &&
           "ScriptedAsyncBackend destroyed with pending operations");
}

// ---------------------------------------------------------------------------
// AsyncBackend: submit
// ---------------------------------------------------------------------------

Result<void> ScriptedAsyncBackend::submit_read(ReadOp op,
                                                Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(mtx_);

    // Check submit failure injection.
    ++submit_count_;
    if (fail_submit_num_ && submit_count_ == *fail_submit_num_) {
        IoError err = fail_submit_num_error_;
        fail_submit_num_.reset();
        return make_unexpected<void>(err);
    }
    if (fail_next_kind_ && *fail_next_kind_ == OpKind::read) {
        IoError err = fail_next_kind_error_;
        fail_next_kind_.reset();
        return make_unexpected<void>(err);
    }

    if (!c.idle())
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});

    c.mark_outstanding();
    std::uint64_t id = next_id_locked();
    pending_size_[id] = PendingSizeOp{id, OpKind::read, op.fd, op.offset,
                                       op.len, op.dst, &c};

    // Update max statistics.
    std::size_t total = pending_size_.size() + pending_void_.size();
    if (total > max_total_) max_total_ = total;
    std::size_t reads = pending_size_.size();
    if (reads > max_reads_) max_reads_ = reads;

    cv_.notify_all();
    return {};
}

Result<void> ScriptedAsyncBackend::submit_write(WriteOp op,
                                                 Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(mtx_);

    ++submit_count_;
    if (fail_submit_num_ && submit_count_ == *fail_submit_num_) {
        IoError err = fail_submit_num_error_;
        fail_submit_num_.reset();
        return make_unexpected<void>(err);
    }
    if (fail_next_kind_ && *fail_next_kind_ == OpKind::write) {
        IoError err = fail_next_kind_error_;
        fail_next_kind_.reset();
        return make_unexpected<void>(err);
    }

    if (!c.idle())
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});

    c.mark_outstanding();
    std::uint64_t id = next_id_locked();
    // Note: src is const std::byte*, but we store as std::byte* for
    // captured_write_bytes. We only read from it while pending.
    pending_size_[id] = PendingSizeOp{id, OpKind::write, op.fd, op.offset,
                                       op.len,
                                       const_cast<std::byte*>(op.src), &c};

    std::size_t total = pending_size_.size() + pending_void_.size();
    if (total > max_total_) max_total_ = total;

    cv_.notify_all();
    return {};
}

Result<void> ScriptedAsyncBackend::submit_sync_data(SyncDataOp op,
                                                     Completion<void>& c) {
    std::lock_guard<std::mutex> lk(mtx_);

    ++submit_count_;
    if (fail_submit_num_ && submit_count_ == *fail_submit_num_) {
        IoError err = fail_submit_num_error_;
        fail_submit_num_.reset();
        return make_unexpected<void>(err);
    }
    if (fail_next_kind_ && *fail_next_kind_ == OpKind::sync_data) {
        IoError err = fail_next_kind_error_;
        fail_next_kind_.reset();
        return make_unexpected<void>(err);
    }

    if (!c.idle())
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});

    c.mark_outstanding();
    std::uint64_t id = next_id_locked();
    pending_void_[id] = PendingVoidOp{id, OpKind::sync_data, op.fd, &c};

    std::size_t total = pending_size_.size() + pending_void_.size();
    if (total > max_total_) max_total_ = total;

    cv_.notify_all();
    return {};
}

Result<void> ScriptedAsyncBackend::submit_sync_all(SyncAllOp op,
                                                    Completion<void>& c) {
    std::lock_guard<std::mutex> lk(mtx_);

    ++submit_count_;
    if (fail_submit_num_ && submit_count_ == *fail_submit_num_) {
        IoError err = fail_submit_num_error_;
        fail_submit_num_.reset();
        return make_unexpected<void>(err);
    }
    if (fail_next_kind_ && *fail_next_kind_ == OpKind::sync_all) {
        IoError err = fail_next_kind_error_;
        fail_next_kind_.reset();
        return make_unexpected<void>(err);
    }

    if (!c.idle())
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});

    c.mark_outstanding();
    std::uint64_t id = next_id_locked();
    pending_void_[id] = PendingVoidOp{id, OpKind::sync_all, op.fd, &c};

    std::size_t total = pending_size_.size() + pending_void_.size();
    if (total > max_total_) max_total_ = total;

    cv_.notify_all();
    return {};
}

// ---------------------------------------------------------------------------
// AsyncBackend: poll / wait_one
// ---------------------------------------------------------------------------

std::size_t ScriptedAsyncBackend::poll() {
    std::lock_guard<std::mutex> lk(mtx_);
    return poll_locked();
}

Result<std::size_t> ScriptedAsyncBackend::wait_one() {
    std::unique_lock<std::mutex> lk(mtx_);
    // If there are no pending ops and no staged results, return 0 immediately.
    // This prevents the Runtime's drain() from hanging forever when the test
    // has already completed all operations.
    if (pending_size_.empty() && pending_void_.empty() &&
        staged_size_.empty() && staged_void_.empty()) {
        return Result<std::size_t>{0};
    }
    cv_.wait(lk, [this] {
        return !staged_size_.empty() || !staged_void_.empty() || shutdown_;
    });
    if (shutdown_ && staged_size_.empty() && staged_void_.empty())
        return make_unexpected<std::size_t>(IoError{IoError::Code::canceled});
    return poll_locked();
}

std::size_t ScriptedAsyncBackend::poll_locked() {
    std::size_t n = 0;

    // Apply staged size results.
    for (auto& s : staged_size_) {
        s.completion->complete_with(std::move(s.result));
        ++n;
    }
    staged_size_.clear();

    // Apply staged void results.
    for (auto& s : staged_void_) {
        s.completion->complete_with(std::move(s.result));
        ++n;
    }
    staged_void_.clear();

    return n;
}

// ---------------------------------------------------------------------------
// AsyncBackend: cancel
// ---------------------------------------------------------------------------

void ScriptedAsyncBackend::cancel(Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(mtx_);

    // Find the pending operation by completion pointer.
    for (auto it = pending_size_.begin(); it != pending_size_.end(); ++it) {
        if (it->second.completion == &c) {
            // Stage a canceled result.
            staged_size_.push_back(
                StagedSizeResult{&c, make_unexpected<std::size_t>(
                                         IoError{IoError::Code::canceled})});
            pending_size_.erase(it);
            cv_.notify_all();
            return;
        }
    }
    // Not found: op was already completed or never submitted. No-op.
}

void ScriptedAsyncBackend::cancel(Completion<void>& c) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto it = pending_void_.begin(); it != pending_void_.end(); ++it) {
        if (it->second.completion == &c) {
            staged_void_.push_back(
                StagedVoidResult{&c, make_unexpected<void>(
                                         IoError{IoError::Code::canceled})});
            pending_void_.erase(it);
            cv_.notify_all();
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// AsyncBackend: outstanding
// ---------------------------------------------------------------------------

std::size_t ScriptedAsyncBackend::outstanding() const noexcept {
    // snapshot under lock for consistency
    std::lock_guard<std::mutex> lk(mtx_);
    return pending_size_.size() + pending_void_.size();
}

// ---------------------------------------------------------------------------
// Pending inspection
// ---------------------------------------------------------------------------

std::size_t ScriptedAsyncBackend::pending_count() {
    std::lock_guard<std::mutex> lk(mtx_);
    return pending_size_.size() + pending_void_.size();
}

std::size_t ScriptedAsyncBackend::pending_read_count() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::size_t n = 0;
    for (auto& [id, op] : pending_size_)
        if (op.kind == OpKind::read) ++n;
    return n;
}

std::size_t ScriptedAsyncBackend::pending_write_count() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::size_t n = 0;
    for (auto& [id, op] : pending_size_)
        if (op.kind == OpKind::write) ++n;
    return n;
}

std::size_t ScriptedAsyncBackend::pending_sync_count() {
    std::lock_guard<std::mutex> lk(mtx_);
    return pending_void_.size();
}

std::size_t ScriptedAsyncBackend::max_outstanding_reads() {
    std::lock_guard<std::mutex> lk(mtx_);
    return max_reads_;
}

std::size_t ScriptedAsyncBackend::max_outstanding_total() {
    std::lock_guard<std::mutex> lk(mtx_);
    return max_total_;
}

std::vector<PendingOpView> ScriptedAsyncBackend::pending_operations() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<PendingOpView> out;
    out.reserve(pending_size_.size() + pending_void_.size());
    for (auto& [id, op] : pending_size_) {
        out.push_back(PendingOpView{op.id, op.kind, op.fd, op.offset,
                                     op.length, op.buffer, op.completion});
    }
    for (auto& [id, op] : pending_void_) {
        out.push_back(PendingOpView{op.id, op.kind, op.fd, 0, 0, nullptr,
                                     op.completion});
    }
    return out;
}

std::optional<std::uint64_t> ScriptedAsyncBackend::find_read_by_offset(
    std::uint64_t offset) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [id, op] : pending_size_) {
        if (op.kind == OpKind::read && op.offset == offset) return op.id;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> ScriptedAsyncBackend::find_write_by_offset(
    std::uint64_t offset) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [id, op] : pending_size_) {
        if (op.kind == OpKind::write && op.offset == offset) return op.id;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Completion control
// ---------------------------------------------------------------------------

void ScriptedAsyncBackend::complete_bytes(std::uint64_t op_id, std::size_t n) {
    std::lock_guard<std::mutex> lk(mtx_);

    auto it = pending_size_.find(op_id);
    if (it == pending_size_.end()) {
        // Operation not found or already completed. This is a test error.
        // We throw to make the test fail loudly.
        throw std::runtime_error(
            "complete_bytes: op_id " + std::to_string(op_id) +
            " not found in pending_size_ (already completed or never submitted)");
    }

    PendingSizeOp& op = it->second;
    if (op.kind != OpKind::read && op.kind != OpKind::write) {
        throw std::runtime_error(
            "complete_bytes: op_id " + std::to_string(op_id) +
            " is not a read/write op");
    }

    if (n > op.length) {
        throw std::runtime_error(
            "complete_bytes: op_id " + std::to_string(op_id) +
            " bytes " + std::to_string(n) +
            " > requested " + std::to_string(op.length));
    }

    staged_size_.push_back(
        StagedSizeResult{op.completion, Result<std::size_t>{n}});
    pending_size_.erase(it);
    cv_.notify_all();
}

void ScriptedAsyncBackend::complete_eof(std::uint64_t op_id) {
    complete_bytes(op_id, 0);
}

void ScriptedAsyncBackend::complete_error(std::uint64_t op_id, IoError error) {
    std::lock_guard<std::mutex> lk(mtx_);

    // Check size ops first.
    auto it = pending_size_.find(op_id);
    if (it != pending_size_.end()) {
        staged_size_.push_back(StagedSizeResult{
            it->second.completion, make_unexpected<std::size_t>(error)});
        pending_size_.erase(it);
        cv_.notify_all();
        return;
    }

    // Check void ops.
    auto vit = pending_void_.find(op_id);
    if (vit != pending_void_.end()) {
        staged_void_.push_back(StagedVoidResult{
            vit->second.completion, make_unexpected<void>(error)});
        pending_void_.erase(vit);
        cv_.notify_all();
        return;
    }

    throw std::runtime_error(
        "complete_error: op_id " + std::to_string(op_id) + " not found");
}

void ScriptedAsyncBackend::complete_sync_success(std::uint64_t op_id) {
    std::lock_guard<std::mutex> lk(mtx_);

    auto it = pending_void_.find(op_id);
    if (it == pending_void_.end()) {
        throw std::runtime_error(
            "complete_sync_success: op_id " + std::to_string(op_id) +
            " not found in pending_void_");
    }

    if (it->second.kind != OpKind::sync_data &&
        it->second.kind != OpKind::sync_all) {
        throw std::runtime_error(
            "complete_sync_success: op_id " + std::to_string(op_id) +
            " is not a sync op");
    }

    staged_void_.push_back(
        StagedVoidResult{it->second.completion, Result<void>{}});
    pending_void_.erase(it);
    cv_.notify_all();
}

void ScriptedAsyncBackend::complete_sync_error(std::uint64_t op_id,
                                                IoError error) {
    complete_error(op_id, error);
}

void ScriptedAsyncBackend::complete_read_with_data(std::uint64_t op_id,
                                                    const std::byte* data,
                                                    std::size_t len) {
    std::lock_guard<std::mutex> lk(mtx_);

    auto it = pending_size_.find(op_id);
    if (it == pending_size_.end()) {
        throw std::runtime_error(
            "complete_read_with_data: op_id " + std::to_string(op_id) +
            " not found");
    }

    if (it->second.kind != OpKind::read) {
        throw std::runtime_error(
            "complete_read_with_data: op_id " + std::to_string(op_id) +
            " is not a read op");
    }

    if (len > it->second.length) {
        throw std::runtime_error(
            "complete_read_with_data: op_id " + std::to_string(op_id) +
            " data length " + std::to_string(len) +
            " > requested " + std::to_string(it->second.length));
    }

    // Copy data into the read buffer.
    std::memcpy(it->second.buffer, data, len);

    staged_size_.push_back(
        StagedSizeResult{it->second.completion, Result<std::size_t>{len}});
    pending_size_.erase(it);
    cv_.notify_all();
}

std::vector<std::byte> ScriptedAsyncBackend::captured_write_bytes(
    std::uint64_t op_id) {
    std::lock_guard<std::mutex> lk(mtx_);

    auto it = pending_size_.find(op_id);
    if (it == pending_size_.end()) {
        throw std::runtime_error(
            "captured_write_bytes: op_id " + std::to_string(op_id) +
            " not found (not pending or already completed)");
    }

    if (it->second.kind != OpKind::write) {
        throw std::runtime_error(
            "captured_write_bytes: op_id " + std::to_string(op_id) +
            " is not a write op");
    }

    return std::vector<std::byte>(it->second.buffer,
                                   it->second.buffer + it->second.length);
}

// ---------------------------------------------------------------------------
// Submit failure injection
// ---------------------------------------------------------------------------

void ScriptedAsyncBackend::fail_next_submit(OpKind kind, IoError error) {
    std::lock_guard<std::mutex> lk(mtx_);
    fail_next_kind_ = kind;
    fail_next_kind_error_ = error;
}

void ScriptedAsyncBackend::fail_submit_number(std::uint64_t n, IoError error) {
    std::lock_guard<std::mutex> lk(mtx_);
    fail_submit_num_ = n;
    fail_submit_num_error_ = error;
}

// ---------------------------------------------------------------------------
// Waiting
// ---------------------------------------------------------------------------

void ScriptedAsyncBackend::wait_until_pending(std::size_t min_count) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [this, min_count] {
        return (pending_size_.size() + pending_void_.size()) >= min_count ||
               shutdown_;
    });
}

bool ScriptedAsyncBackend::wait_until_pending_for(
    std::size_t min_count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mtx_);
    return cv_.wait_for(lk, timeout, [this, min_count] {
        return (pending_size_.size() + pending_void_.size()) >= min_count ||
               shutdown_;
    });
}

// ---------------------------------------------------------------------------
// Drain verification
// ---------------------------------------------------------------------------

void ScriptedAsyncBackend::expect_no_pending() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (pending_size_.empty() && pending_void_.empty()) return;

    // Build a descriptive failure message.
    std::string msg = "ScriptedAsyncBackend has pending operations:";
    for (auto& [id, op] : pending_size_) {
        msg += "\n  size op " + std::to_string(id) + " " +
               std::string(to_string(op.kind)) + " fd=" +
               std::to_string(op.fd) + " offset=" +
               std::to_string(op.offset) + " len=" +
               std::to_string(op.length);
    }
    for (auto& [id, op] : pending_void_) {
        msg += "\n  void op " + std::to_string(id) + " " +
               std::string(to_string(op.kind)) + " fd=" +
               std::to_string(op.fd);
    }

    // Throw to make the test fail. The test harness will catch this.
    throw std::runtime_error(msg);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

std::uint64_t ScriptedAsyncBackend::next_id_locked() {
    return next_id_++;
}

}  // namespace sluice::async