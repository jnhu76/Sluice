// ScriptedAsyncBackend implementation.
//
// Thread-safety model:
//   - All backend submit_*/poll/wait_one/cancel/outstanding acquire the shared
//     state mutex (state_->mtx).
//   - submit_* are called from Runtime worker/driver threads.
//   - All controller control/inspection/wait methods are called from the test
//     thread and acquire the SAME mutex.
//   - The condition variable cv is notified when a pending op is added
//     (submit_*), a staged result is ready (complete_*), or the backend is
//     closed (destructor).
//
// Outstanding accounting (Phase 0 correctness): an op stays in size_ops/void_ops
// from submit until poll()/wait_one() applies its result. Its `stage` moves
// pending -> staged when a result is staged. outstanding() == size_ops.size() +
// void_ops.size() == pending + staged exactly. Statistics are updated on submit
// (peak of pending+staged) and never reduced on completion, so they reflect the
// true peak concurrency the backend observed.

#include "scripted_async_backend.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <utility>

namespace sluice::async {

// ---------------------------------------------------------------------------
// Helpers shared between backend and controller (operate on shared state)
// ---------------------------------------------------------------------------

namespace {

// Count reads among outstanding size ops (pending + staged).
std::size_t count_reads_locked(const ScriptedBackendSharedState& s) {
    std::size_t n = 0;
    for (const auto& [id, op] : s.size_ops)
        if (op.kind == OpKind::read) ++n;
    return n;
}

// Recompute peak statistics after a submit. Caller holds mtx.
void update_peak_locked(ScriptedBackendSharedState& s) {
    std::size_t total = s.size_ops.size() + s.void_ops.size();
    if (total > s.max_total) s.max_total = total;
    std::size_t reads = count_reads_locked(s);
    if (reads > s.max_reads) s.max_reads = reads;
}

// Apply all staged results to their Completion objects. Caller holds mtx.
// Returns the number applied.
std::size_t apply_staged_locked(ScriptedBackendSharedState& s) {
    std::size_t n = 0;

    for (auto& res : s.staged_size) {
        ScriptedAsyncBackend::publish_completion(*res.completion, std::move(res.result));
        ++n;
    }
    s.staged_size.clear();

    for (auto& res : s.staged_void) {
        ScriptedAsyncBackend::publish_completion(*res.completion, std::move(res.result));
        ++n;
    }
    s.staged_void.clear();

    // Now erase every op whose stage became staged (those whose results were
    // just applied). Pending ops (no result yet) remain.
    for (auto it = s.size_ops.begin(); it != s.size_ops.end();) {
        if (it->second.stage == OpStage::staged)
            it = s.size_ops.erase(it);
        else
            ++it;
    }
    for (auto it = s.void_ops.begin(); it != s.void_ops.end();) {
        if (it->second.stage == OpStage::staged)
            it = s.void_ops.erase(it);
        else
            ++it;
    }

    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// ScriptedAsyncBackend: construction / destruction
// ---------------------------------------------------------------------------

namespace {

// T3 named fail-fast authority (AGENTS.md §3.8,
// docs/architecture/failure-model.md §3/§5): constructing the backend without
// a shared state is a test-author contract violation with no recovery
// channel. Same contract as the production detail::fail_fast.hpp family
// ([[noreturn]] noexcept; no allocation, locking, I/O, parameters, or state
// recovery; terminates). Test-support code: deliberately NOT added to the
// production family header. Death-tested (Debug AND Release) by
// failure_model_high_risk_death_test SB-A via the real constructor path.
[[noreturn]] void scripted_backend_state_fail_fast() noexcept {
    std::terminate();
}

// T6 named fail-fast authority: destroying the backend while scripted
// operations are still outstanding (pending or staged) is a lifetime
// contract violation — the caller contract is to drain everything before
// teardown (the controller's expect_no_outstanding() is the descriptive,
// non-fatal check for the same condition). Same family contract as above;
// no hidden cleanup, no drain attempt. Death-tested (Debug AND Release) by
// failure_model_high_risk_death_test SB-B via the real destructor path.
[[noreturn]] void scripted_backend_non_quiescent_destruction_fail_fast()
    noexcept {
    std::terminate();
}

}  // namespace

ScriptedAsyncBackend::ScriptedAsyncBackend(
    std::shared_ptr<ScriptedBackendSharedState> state)
    : state_(std::move(state)) {
    // T3 caller-contract check (AGENTS.md §3.8): a null shared state is a
    // test-author bug with no recovery; the named fail-fast is active in
    // Debug AND Release (the previous bare assert left Release test binaries
    // a silent null dereference at first use).
    if (!state_) {
        scripted_backend_state_fail_fast();
    }
}

ScriptedAsyncBackend::~ScriptedAsyncBackend() {
    // Mark the shared state closed under its mutex and notify waiters. The
    // shared state itself is NOT destroyed (the controller may still hold a
    // reference). Final diagnostics snapshot is preserved for the test thread.
    std::lock_guard<std::mutex> lk(state_->mtx);
    state_->shutdown = true;
    state_->closed = true;
    state_->final_outstanding =
        state_->size_ops.size() + state_->void_ops.size() +
        state_->staged_size.size() + state_->staged_void.size();
    state_->cv.notify_all();
    // Split-wait control wake: any participant parked in the context-level
    // wait_one observe phase must re-evaluate after close (it re-polls and
    // terminates on the empty wait / shutdown boundary).
    state_->ready_wait.interrupt_all();

    // T6 lifetime-contract check (AGENTS.md §3.8,
    // docs/architecture/failure-model.md §6): destroying a backend with
    // accepted work still outstanding — pending or staged — is a lifetime
    // violation, not an ordinary caller-contract slip: the caller contract
    // is to drain everything before teardown (the controller's
    // expect_no_outstanding() gives the descriptive non-fatal check for the
    // same condition). The named fail-fast is active in Debug AND Release;
    // the previous bare assert let Release test binaries
    // destroy-with-outstanding silently.
    if (!(state_->size_ops.empty() && state_->void_ops.empty() &&
          state_->staged_size.empty() && state_->staged_void.empty())) {
        scripted_backend_non_quiescent_destruction_fail_fast();
    }
}

// ---------------------------------------------------------------------------
// ScriptedAsyncBackend: submit
// ---------------------------------------------------------------------------

namespace {

// Common submit-failure-injection check. Caller holds mtx. Returns the IoError
// to return (and consumes the injection) on failure; std::nullopt otherwise.
std::optional<IoError> check_submit_failure_locked(
    ScriptedBackendSharedState& s) {
    ++s.submit_count;
    if (s.fail_submit_num && s.submit_count == *s.fail_submit_num) {
        IoError err = s.fail_submit_num_error;
        s.fail_submit_num.reset();
        return err;
    }
    return std::nullopt;
}

// Per-kind submit-failure check (fail_next_kind consumes one matching submit).
std::optional<IoError> check_kind_failure_locked(
    ScriptedBackendSharedState& s, OpKind kind) {
    if (s.fail_next_kind && *s.fail_next_kind == kind) {
        IoError err = s.fail_next_kind_error;
        s.fail_next_kind.reset();
        return err;
    }
    return std::nullopt;
}

}  // namespace

Result<void> ScriptedAsyncBackend::submit_read(ReadOp op,
                                               Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(state_->mtx);
    if (auto e = check_submit_failure_locked(*state_))
        return make_unexpected<void>(*e);
    if (auto e = check_kind_failure_locked(*state_, OpKind::read))
        return make_unexpected<void>(*e);

    if (!c.idle())
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});

    if (!try_claim(c))
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    std::uint64_t id = state_->next_id++;
    state_->size_ops[id] = ScriptedBackendSharedState::PendingSizeOp{
        id, OpKind::read, OpStage::pending, op.fd, op.offset, op.len, op.dst, &c};
    update_peak_locked(*state_);
    state_->cv.notify_all();
    return {};
}

Result<void> ScriptedAsyncBackend::submit_write(WriteOp op,
                                                Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(state_->mtx);
    if (auto e = check_submit_failure_locked(*state_))
        return make_unexpected<void>(*e);
    if (auto e = check_kind_failure_locked(*state_, OpKind::write))
        return make_unexpected<void>(*e);

    if (!c.idle())
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});

    if (!try_claim(c))
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    std::uint64_t id = state_->next_id++;
    // src is const std::byte*; store as std::byte* for captured_write_bytes.
    // The backend only reads from it while the op is outstanding.
    state_->size_ops[id] = ScriptedBackendSharedState::PendingSizeOp{
        id, OpKind::write, OpStage::pending, op.fd, op.offset, op.len,
        const_cast<std::byte*>(op.src), &c};
    update_peak_locked(*state_);
    state_->cv.notify_all();
    return {};
}

Result<void> ScriptedAsyncBackend::submit_sync_data(SyncDataOp op,
                                                    Completion<void>& c) {
    std::lock_guard<std::mutex> lk(state_->mtx);
    if (auto e = check_submit_failure_locked(*state_))
        return make_unexpected<void>(*e);
    if (auto e = check_kind_failure_locked(*state_, OpKind::sync_data))
        return make_unexpected<void>(*e);

    if (!c.idle())
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});

    if (!try_claim(c))
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    std::uint64_t id = state_->next_id++;
    state_->void_ops[id] =
        ScriptedBackendSharedState::PendingVoidOp{id, OpKind::sync_data,
                                                   OpStage::pending, op.fd, &c};
    update_peak_locked(*state_);
    state_->cv.notify_all();
    return {};
}

Result<void> ScriptedAsyncBackend::submit_sync_all(SyncAllOp op,
                                                   Completion<void>& c) {
    std::lock_guard<std::mutex> lk(state_->mtx);
    if (auto e = check_submit_failure_locked(*state_))
        return make_unexpected<void>(*e);
    if (auto e = check_kind_failure_locked(*state_, OpKind::sync_all))
        return make_unexpected<void>(*e);

    if (!c.idle())
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});

    if (!try_claim(c))
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    std::uint64_t id = state_->next_id++;
    state_->void_ops[id] =
        ScriptedBackendSharedState::PendingVoidOp{id, OpKind::sync_all,
                                                   OpStage::pending, op.fd, &c};
    update_peak_locked(*state_);
    state_->cv.notify_all();
    return {};
}

// ---------------------------------------------------------------------------
// ScriptedAsyncBackend: poll / wait_one
// ---------------------------------------------------------------------------

std::size_t ScriptedAsyncBackend::poll() {
    std::lock_guard<std::mutex> lk(state_->mtx);
    return apply_staged_locked(*state_);
}

Result<std::size_t> ScriptedAsyncBackend::wait_one() {
    std::unique_lock<std::mutex> lk(state_->mtx);
    // If nothing is outstanding and nothing is staged, return 0 immediately so
    // the Runtime's drain() does not hang after the test completed everything.
    if (state_->size_ops.empty() && state_->void_ops.empty() &&
        state_->staged_size.empty() && state_->staged_void.empty()) {
        return Result<std::size_t>{0};
    }
    state_->cv.wait(lk, [this] {
        return !state_->staged_size.empty() || !state_->staged_void.empty() ||
               state_->shutdown;
    });
    if (state_->shutdown && state_->staged_size.empty() &&
        state_->staged_void.empty())
        return make_unexpected<std::size_t>(IoError{IoError::Code::canceled});
    return apply_staged_locked(*state_);
}

std::size_t ScriptedAsyncBackend::poll_locked(ScriptedBackendSharedState& s) {
    return apply_staged_locked(s);
}

// ---------------------------------------------------------------------------
// ScriptedAsyncBackend: cancel
// ---------------------------------------------------------------------------

void ScriptedAsyncBackend::cancel(Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(state_->mtx);
    for (auto& [id, op] : state_->size_ops) {
        if (op.completion == &c) {
            // Idempotent cancel: only a still-PENDING op may be staged with a
            // canceled result. If a result is already staged (stage == staged,
            // e.g. from an earlier cancel or a test completion), a second
            // cancel must be a no-op — staging twice would complete_with the
            // same Completion twice (a terminal-result contract violation).
            if (op.stage != OpStage::pending) return;
            op.stage = OpStage::staged;
            state_->staged_size.push_back(
                {&c, make_unexpected<std::size_t>(
                         IoError{IoError::Code::canceled})});
            state_->cv.notify_all();
            state_->ready_wait.signal_progress();  // split-wait: staged result
            return;
        }
    }
    // Not found: already completed or never submitted. No-op.
}

void ScriptedAsyncBackend::cancel(Completion<void>& c) {
    std::lock_guard<std::mutex> lk(state_->mtx);
    for (auto& [id, op] : state_->void_ops) {
        if (op.completion == &c) {
            // Idempotent: see the size-op cancel above (exactly-once terminal
            // result per Completion).
            if (op.stage != OpStage::pending) return;
            op.stage = OpStage::staged;
            state_->staged_void.push_back(
                {&c, make_unexpected<void>(
                         IoError{IoError::Code::canceled})});
            state_->cv.notify_all();
            state_->ready_wait.signal_progress();  // split-wait: staged result
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// ScriptedAsyncBackend: outstanding
// ---------------------------------------------------------------------------

std::size_t ScriptedAsyncBackend::outstanding() const noexcept {
    // Snapshot under lock. Outstanding = every op still in the maps (pending or
    // staged), because a Completion is outstanding until poll()/wait_one()
    // applies its result (complete_with).
    std::lock_guard<std::mutex> lk(state_->mtx);
    return state_->size_ops.size() + state_->void_ops.size();
}

// ===========================================================================
// ScriptedBackendController (test-thread control surface)
// ===========================================================================

void ScriptedBackendController::require_open_locked(const char* fn) const {
    if (!state_)
        throw ScriptedBackendError(std::string(fn) + ": controller has no state");
    if (state_->closed)
        throw ScriptedBackendClosed(std::string(fn) + ": backend is closed");
}

bool ScriptedBackendController::closed() const {
    if (!state_) return false;
    std::lock_guard<std::mutex> lk(state_->mtx);
    return state_->closed;
}

// --- Pending inspection ---

std::size_t ScriptedBackendController::pending_count() {
    if (!state_) return 0;
    std::lock_guard<std::mutex> lk(state_->mtx);
    // Outstanding = pending + staged (ops still in the maps).
    return state_->size_ops.size() + state_->void_ops.size();
}

std::size_t ScriptedBackendController::pending_read_count() {
    if (!state_) return 0;
    std::lock_guard<std::mutex> lk(state_->mtx);
    return count_reads_locked(*state_);
}

std::size_t ScriptedBackendController::pending_write_count() {
    if (!state_) return 0;
    std::lock_guard<std::mutex> lk(state_->mtx);
    std::size_t n = 0;
    for (auto& [id, op] : state_->size_ops)
        if (op.kind == OpKind::write) ++n;
    return n;
}

std::size_t ScriptedBackendController::pending_sync_count() {
    if (!state_) return 0;
    std::lock_guard<std::mutex> lk(state_->mtx);
    return state_->void_ops.size();
}

std::size_t ScriptedBackendController::max_outstanding_reads() {
    if (!state_) return 0;
    std::lock_guard<std::mutex> lk(state_->mtx);
    return state_->max_reads;
}

std::size_t ScriptedBackendController::max_outstanding_total() {
    if (!state_) return 0;
    std::lock_guard<std::mutex> lk(state_->mtx);
    return state_->max_total;
}

std::vector<PendingOpView> ScriptedBackendController::pending_operations() {
    if (!state_) return {};
    std::lock_guard<std::mutex> lk(state_->mtx);
    std::vector<PendingOpView> out;
    out.reserve(state_->size_ops.size() + state_->void_ops.size());
    for (auto& [id, op] : state_->size_ops)
        out.push_back(PendingOpView{op.id, op.kind, op.fd, op.offset, op.length,
                                    op.buffer, op.completion, op.stage});
    for (auto& [id, op] : state_->void_ops)
        out.push_back(PendingOpView{op.id, op.kind, op.fd, 0, 0, nullptr,
                                    op.completion, op.stage});
    return out;
}

std::optional<std::uint64_t> ScriptedBackendController::find_read_by_offset(
    std::uint64_t offset) {
    if (!state_) return std::nullopt;
    std::lock_guard<std::mutex> lk(state_->mtx);
    for (auto& [id, op] : state_->size_ops)
        if (op.kind == OpKind::read && op.offset == offset) return op.id;
    return std::nullopt;
}

std::optional<std::uint64_t> ScriptedBackendController::find_write_by_offset(
    std::uint64_t offset) {
    if (!state_) return std::nullopt;
    std::lock_guard<std::mutex> lk(state_->mtx);
    for (auto& [id, op] : state_->size_ops)
        if (op.kind == OpKind::write && op.offset == offset) return op.id;
    return std::nullopt;
}

// --- Completion control ---

void ScriptedBackendController::complete_bytes(std::uint64_t op_id,
                                               std::size_t n) {
    std::lock_guard<std::mutex> lk(state_->mtx);
    require_open_locked("complete_bytes");
    auto it = state_->size_ops.find(op_id);
    if (it == state_->size_ops.end())
        throw ScriptedBackendError(
            "complete_bytes: op_id " + std::to_string(op_id) +
            " not found (already completed or never submitted)");
    if (it->second.stage != OpStage::pending)
        throw ScriptedBackendError(
            "complete_bytes: op_id " + std::to_string(op_id) +
            " already staged for completion");
    if (it->second.kind != OpKind::read && it->second.kind != OpKind::write)
        throw ScriptedBackendError(
            "complete_bytes: op_id " + std::to_string(op_id) +
            " is not a read/write op");
    if (n > it->second.length)
        throw ScriptedBackendError(
            "complete_bytes: op_id " + std::to_string(op_id) + " bytes " +
            std::to_string(n) + " > requested " +
            std::to_string(it->second.length));

    it->second.stage = OpStage::staged;
    state_->staged_size.push_back({it->second.completion, Result<std::size_t>{n}});
    state_->cv.notify_all();
    state_->ready_wait.signal_progress();  // split-wait: staged result
}

void ScriptedBackendController::complete_eof(std::uint64_t op_id) {
    std::lock_guard<std::mutex> lk(state_->mtx);
    require_open_locked("complete_eof");
    auto it = state_->size_ops.find(op_id);
    if (it == state_->size_ops.end())
        throw ScriptedBackendError(
            "complete_eof: op_id " + std::to_string(op_id) + " not found");
    if (it->second.stage != OpStage::pending)
        throw ScriptedBackendError(
            "complete_eof: op_id " + std::to_string(op_id) + " already staged");
    if (it->second.kind != OpKind::read)
        throw ScriptedBackendError(
            "complete_eof: op_id " + std::to_string(op_id) +
            " is not a read op (EOF is read-only)");

    it->second.stage = OpStage::staged;
    state_->staged_size.push_back({it->second.completion, Result<std::size_t>{0}});
    state_->cv.notify_all();
    state_->ready_wait.signal_progress();  // split-wait: staged result
}

void ScriptedBackendController::complete_error(std::uint64_t op_id,
                                               IoError error) {
    std::lock_guard<std::mutex> lk(state_->mtx);
    require_open_locked("complete_error");
    // Size ops (read/write).
    auto it = state_->size_ops.find(op_id);
    if (it != state_->size_ops.end()) {
        if (it->second.stage != OpStage::pending)
            throw ScriptedBackendError(
                "complete_error: op_id " + std::to_string(op_id) +
                " already staged");
        it->second.stage = OpStage::staged;
        state_->staged_size.push_back(
            {it->second.completion, make_unexpected<std::size_t>(error)});
        state_->cv.notify_all();
        state_->ready_wait.signal_progress();  // split-wait: staged result
        return;
    }
    // Void ops (sync).
    auto vit = state_->void_ops.find(op_id);
    if (vit != state_->void_ops.end()) {
        if (vit->second.stage != OpStage::pending)
            throw ScriptedBackendError(
                "complete_error: op_id " + std::to_string(op_id) +
                " already staged");
        vit->second.stage = OpStage::staged;
        state_->staged_void.push_back(
            {vit->second.completion, make_unexpected<void>(error)});
        state_->cv.notify_all();
        state_->ready_wait.signal_progress();  // split-wait: staged result
        return;
    }
    throw ScriptedBackendError(
        "complete_error: op_id " + std::to_string(op_id) + " not found");
}

void ScriptedBackendController::complete_sync_success(std::uint64_t op_id) {
    std::lock_guard<std::mutex> lk(state_->mtx);
    require_open_locked("complete_sync_success");
    auto it = state_->void_ops.find(op_id);
    if (it == state_->void_ops.end())
        throw ScriptedBackendError(
            "complete_sync_success: op_id " + std::to_string(op_id) +
            " not found in void ops");
    if (it->second.stage != OpStage::pending)
        throw ScriptedBackendError(
            "complete_sync_success: op_id " + std::to_string(op_id) +
            " already staged");
    if (it->second.kind != OpKind::sync_data &&
        it->second.kind != OpKind::sync_all)
        throw ScriptedBackendError(
            "complete_sync_success: op_id " + std::to_string(op_id) +
            " is not a sync op");
    it->second.stage = OpStage::staged;
    state_->staged_void.push_back({it->second.completion, Result<void>{}});
    state_->cv.notify_all();
    state_->ready_wait.signal_progress();  // split-wait: staged result
}

void ScriptedBackendController::complete_sync_error(std::uint64_t op_id,
                                                    IoError error) {
    std::lock_guard<std::mutex> lk(state_->mtx);
    require_open_locked("complete_sync_error");
    auto it = state_->void_ops.find(op_id);
    if (it == state_->void_ops.end())
        throw ScriptedBackendError(
            "complete_sync_error: op_id " + std::to_string(op_id) +
            " not found in void ops");
    if (it->second.stage != OpStage::pending)
        throw ScriptedBackendError(
            "complete_sync_error: op_id " + std::to_string(op_id) +
            " already staged");
    if (it->second.kind != OpKind::sync_data &&
        it->second.kind != OpKind::sync_all)
        throw ScriptedBackendError(
            "complete_sync_error: op_id " + std::to_string(op_id) +
            " is not a sync op");
    it->second.stage = OpStage::staged;
    state_->staged_void.push_back(
        {it->second.completion, make_unexpected<void>(error)});
    state_->cv.notify_all();
    state_->ready_wait.signal_progress();  // split-wait: staged result
}

void ScriptedBackendController::complete_read_with_data(std::uint64_t op_id,
                                                        const std::byte* data,
                                                        std::size_t len) {
    std::lock_guard<std::mutex> lk(state_->mtx);
    require_open_locked("complete_read_with_data");
    auto it = state_->size_ops.find(op_id);
    if (it == state_->size_ops.end())
        throw ScriptedBackendError(
            "complete_read_with_data: op_id " + std::to_string(op_id) +
            " not found");
    if (it->second.stage != OpStage::pending)
        throw ScriptedBackendError(
            "complete_read_with_data: op_id " + std::to_string(op_id) +
            " already staged");
    if (it->second.kind != OpKind::read)
        throw ScriptedBackendError(
            "complete_read_with_data: op_id " + std::to_string(op_id) +
            " is not a read op");
    if (len > it->second.length)
        throw ScriptedBackendError(
            "complete_read_with_data: op_id " + std::to_string(op_id) +
            " data length " + std::to_string(len) + " > requested " +
            std::to_string(it->second.length));

    std::memcpy(it->second.buffer, data, len);
    it->second.stage = OpStage::staged;
    state_->staged_size.push_back(
        {it->second.completion, Result<std::size_t>{len}});
    state_->cv.notify_all();
    state_->ready_wait.signal_progress();  // split-wait: staged result
}

std::vector<std::byte> ScriptedBackendController::captured_write_bytes(
    std::uint64_t op_id) {
    std::lock_guard<std::mutex> lk(state_->mtx);
    require_open_locked("captured_write_bytes");
    auto it = state_->size_ops.find(op_id);
    if (it == state_->size_ops.end())
        throw ScriptedBackendError(
            "captured_write_bytes: op_id " + std::to_string(op_id) +
            " not found (not outstanding or already completed)");
    if (it->second.kind != OpKind::write)
        throw ScriptedBackendError(
            "captured_write_bytes: op_id " + std::to_string(op_id) +
            " is not a write op");
    return std::vector<std::byte>(it->second.buffer,
                                  it->second.buffer + it->second.length);
}

// --- Submit failure injection ---

void ScriptedBackendController::fail_next_submit(OpKind kind, IoError error) {
    if (!state_) return;
    std::lock_guard<std::mutex> lk(state_->mtx);
    if (state_->closed)
        throw ScriptedBackendClosed("fail_next_submit: backend is closed");
    state_->fail_next_kind = kind;
    state_->fail_next_kind_error = error;
}

void ScriptedBackendController::fail_submit_number(std::uint64_t n,
                                                   IoError error) {
    if (!state_) return;
    std::lock_guard<std::mutex> lk(state_->mtx);
    if (state_->closed)
        throw ScriptedBackendClosed("fail_submit_number: backend is closed");
    state_->fail_submit_num = n;
    state_->fail_submit_num_error = error;
}

// --- Waiting ---

WaitStatus ScriptedBackendController::wait_until_pending(std::size_t min_count) {
    if (!state_) return WaitStatus::closed;
    std::unique_lock<std::mutex> lk(state_->mtx);
    state_->cv.wait(lk, [this, min_count] {
        return (state_->size_ops.size() + state_->void_ops.size()) >=
                   min_count ||
               state_->closed;
    });
    if (state_->closed) return WaitStatus::closed;
    return WaitStatus::ready;
}

WaitStatus ScriptedBackendController::wait_until_pending_for(
    std::size_t min_count, std::chrono::milliseconds timeout) {
    if (!state_) return WaitStatus::closed;
    std::unique_lock<std::mutex> lk(state_->mtx);
    bool met = state_->cv.wait_for(lk, timeout, [this, min_count] {
        return (state_->size_ops.size() + state_->void_ops.size()) >=
                   min_count ||
               state_->closed;
    });
    if (state_->closed) return WaitStatus::closed;
    return met ? WaitStatus::ready : WaitStatus::timeout;
}

// --- Drain verification ---

void ScriptedBackendController::expect_no_outstanding() {
    if (!state_) return;
    std::lock_guard<std::mutex> lk(state_->mtx);
    if (state_->size_ops.empty() && state_->void_ops.empty() &&
        state_->staged_size.empty() && state_->staged_void.empty())
        return;

    std::string msg = "ScriptedAsyncBackend has outstanding operations:";
    for (auto& [id, op] : state_->size_ops) {
        msg += "\n  size op " + std::to_string(id) + " " +
               to_string(op.kind) + " stage=" +
               (op.stage == OpStage::staged ? "staged" : "pending") +
               " fd=" + std::to_string(op.fd) +
               " offset=" + std::to_string(op.offset) +
               " len=" + std::to_string(op.length);
    }
    for (auto& [id, op] : state_->void_ops) {
        msg += "\n  void op " + std::to_string(id) + " " + to_string(op.kind) +
               " stage=" +
               (op.stage == OpStage::staged ? "staged" : "pending") +
               " fd=" + std::to_string(op.fd);
    }
    if (!state_->staged_size.empty())
        msg += "\n  staged_size results: " +
               std::to_string(state_->staged_size.size());
    if (!state_->staged_void.empty())
        msg += "\n  staged_void results: " +
               std::to_string(state_->staged_void.size());
    throw ScriptedBackendError(msg);
}

// --- Cleanup convenience ---

void ScriptedBackendController::complete_all_for_cleanup() {
    if (!state_) return;
    std::lock_guard<std::mutex> lk(state_->mtx);
    if (state_->closed) return;  // backend gone: nothing to drive
    // Stage every pending op with a benign result. Errors are acceptable here:
    // this is best-effort drain to let a test unblock the copy thread.
    for (auto& [id, op] : state_->size_ops) {
        if (op.stage != OpStage::pending) continue;
        if (op.kind == OpKind::read) {
            op.stage = OpStage::staged;
            state_->staged_size.push_back(
                {op.completion, Result<std::size_t>{0}});  // EOF
        } else {  // write
            op.stage = OpStage::staged;
            state_->staged_size.push_back(
                {op.completion, Result<std::size_t>{op.length}});
        }
    }
    for (auto& [id, op] : state_->void_ops) {
        if (op.stage != OpStage::pending) continue;
        op.stage = OpStage::staged;
        state_->staged_void.push_back({op.completion, Result<void>{}});
    }
    state_->cv.notify_all();
    state_->ready_wait.signal_progress();  // split-wait: staged result
}

}  // namespace sluice::async
