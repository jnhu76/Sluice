// Implementation of AsyncIoContext (sluice-CORE-017 + E7-C serialized access).
//
// The context routes submit_*/poll/wait_one/cancel to its owned backend and
// tallies AsyncStats. E7-C adds access_mtx_ — all backend calls are serialized
// (at most one concurrent caller). This satisfies the E7 ADR §9.2.5 serialized
// backend access domain contract.
#include <sluice/async/async_io_context.hpp>

#include <cassert>

namespace sluice::async {

AsyncIoContext::AsyncIoContext(std::unique_ptr<AsyncBackend> backend, AsyncStats* stats)
    : backend_(std::move(backend)), stats_(stats) {
    if (backend_) backend_->attach_stats(stats_);
}

AsyncIoContext::~AsyncIoContext() {
    if (backend_) {
        assert(backend_->outstanding() == 0 &&
               "AsyncIoContext destroyed with outstanding Completions (L11)");
    }
}

AsyncIoContext::AsyncIoContext(AsyncIoContext&& other) noexcept
    : backend_(std::move(other.backend_)), stats_(other.stats_) {}

AsyncIoContext& AsyncIoContext::operator=(AsyncIoContext&& other) noexcept {
    if (this != &other) {
        backend_ = std::move(other.backend_);
        stats_ = other.stats_;
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
