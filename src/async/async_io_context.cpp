// Implementation of AsyncIoContext (sluice-CORE-017 + E7-C serialized access).
//
// The context routes submit_*/poll/wait_one/cancel to its owned backend and
// tallies AsyncStats. E7-C adds access_mtx_ — all backend calls are serialized
// (at most one concurrent caller). This satisfies the E7 ADR §9.2.5 serialized
// backend access domain contract.
//
// E15-P1-03 / E15-P2-06: lifetime contract for outstanding Completions. Per
// ADR §5 L11 the context is the publication authority for every outstanding
// Completion it owns. Destroying the backend (either by destroying the context
// or by overwriting it via move-assignment) while Completions are still
// outstanding would strand them permanently: their backend pointer is gone,
// no poll()/wait_one() can ever mark them ready, and they have no Result
// channel of their own. The truthful deterministic contract is therefore
// fail-fast in BOTH Debug and Release via async_context_outstanding_fail_fast
// (no silent abandonment, no claimed-but-unreturnable invalid_state).
#include <sluice/async/async_io_context.hpp>

#include <sluice/async/detail/fail_fast.hpp>

#include <utility>

namespace sluice::async {

AsyncIoContext::AsyncIoContext(std::unique_ptr<AsyncBackend> backend, AsyncStats* stats)
    : backend_(std::move(backend)), stats_(stats) {
    if (backend_) backend_->attach_stats(stats_);
}

AsyncIoContext::~AsyncIoContext() {
    if (backend_ && backend_->outstanding() != 0) {
        // E15-P1-03 / E15-P2-06 / ADR §5 L11: outstanding-when-destroyed.
        // The fail-fast (std::terminate) is the truthful deterministic contract
        // in BOTH Debug and Release — we deliberately do NOT use assert() here,
        // because assert() aborts (SIGABRT) and would bypass a process-wide
        // std::terminate handler that death tests rely on for a stable exit
        // code. async_context_outstanding_fail_fast routes through
        // std::terminate so the handler sees a clean terminate, not a signal.
        detail::async_context_outstanding_fail_fast();
    }
}

AsyncIoContext::AsyncIoContext(AsyncIoContext&& other) noexcept
    : backend_(std::move(other.backend_)), stats_(other.stats_) {
    // Source-side outstanding state is SAFE to transfer: the backend instance
    // (now ours) retains every outstanding Completion pointer, so callers that
    // poll/wait_one on us still see them resolve. No contract check needed.
}

AsyncIoContext& AsyncIoContext::operator=(AsyncIoContext&& other) noexcept {
    if (this != &other) {
        // E15-P1-03: the DESTINATION must not already own a backend with
        // outstanding Completions — overwriting it would destroy the backend
        // (the publication authority for those Completions) and strand them
        // permanently outstanding with no path to ready. This is the same L11
        // invariant the destructor enforces; fail-fast deterministically in
        // Debug AND Release rather than silently abandoning in-flight ops.
        // (No assert(): abort() would bypass a std::terminate handler — see
        // ~AsyncIoContext above.)
        if (backend_ && backend_->outstanding() != 0) {
            detail::async_context_outstanding_fail_fast();
        }
        backend_ = std::move(other.backend_);
        stats_ = other.stats_;
        // The SOURCE's outstanding state transfers with the backend (see the
        // move ctor); the source is left with backend_ == nullptr and any
        // further use (poll/wait_one/submit) is the caller's responsibility.
    }
    return *this;
}

namespace {
// Tally one submit_* result into AsyncStats. This is the SINGLE counting
// authority for queue_full_retries on the L8 reject path (submit into a
// non-idle Completion -> invalid_state): every AsyncBackend returns
// invalid_state for that case, and tally_submit counts it here once,
// uniformly across backends (Uring/ThreadPool/Sync/Fake). Backends MUST NOT
// also bump queue_full_retries for invalid_state — that double-counts. A
// backend MAY still bump queue_full_retries for a backend_error ring-full
// path (Uring does; tally_submit cannot see it since it returns
// backend_error, not invalid_state).
void tally_submit(AsyncStats* s, const Result<void>& r) {
    if (!s) return;
    ++s->submit_calls;
    if (r.has_value()) {
        ++s->submitted_ops;
    } else if (r.error().code == IoError::Code::invalid_state) {
        ++s->queue_full_retries;
    }
}
void update_max_outstanding(AsyncStats* s, std::size_t cur) {
    if (s && cur > s->max_outstanding) s->max_outstanding = cur;
}
}  // namespace

Result<void> AsyncIoContext::submit_read(ReadOp op, Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_read(op, c);
    tally_submit(stats_, r);
    update_max_outstanding(stats_, backend_->outstanding());
    return r;
}
Result<void> AsyncIoContext::submit_write(WriteOp op, Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_write(op, c);
    tally_submit(stats_, r);
    update_max_outstanding(stats_, backend_->outstanding());
    return r;
}
Result<void> AsyncIoContext::submit_sync_data(SyncDataOp op, Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_sync_data(op, c);
    tally_submit(stats_, r);
    update_max_outstanding(stats_, backend_->outstanding());
    return r;
}
Result<void> AsyncIoContext::submit_sync_all(SyncAllOp op, Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_sync_all(op, c);
    tally_submit(stats_, r);
    update_max_outstanding(stats_, backend_->outstanding());
    return r;
}

std::size_t AsyncIoContext::poll() {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (stats_) ++stats_->poll_calls;
    std::size_t n = backend_->poll();
    if (stats_) stats_->completed_ops += n;
    return n;
}

Result<std::size_t> AsyncIoContext::wait_one() {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (stats_) ++stats_->wait_calls;
    auto r = backend_->wait_one();
    if (r.has_value() && stats_) stats_->completed_ops += r.value();
    return r;
}

void AsyncIoContext::cancel(Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    backend_->cancel(c);
}
void AsyncIoContext::cancel(Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    backend_->cancel(c);
}

std::size_t AsyncIoContext::outstanding() const noexcept {
    std::lock_guard<std::mutex> lk(access_mtx_);
    return backend_ ? backend_->outstanding() : 0;
}

}  // namespace sluice::async
