// Implementation of AsyncIoContext (sluice-CORE-017). The context routes
// submit_*/poll/wait_one/cancel to its owned backend and tallies AsyncStats.
// The Completion lifecycle state machine (idle/outstanding/ready) lives in the
// Completion itself; the context just records outstanding count and enforces L11
// (destroy-with-outstanding is a contract violation).
#include <sluice/async/async_io_context.hpp>

#include <cassert>

namespace sluice::async {

AsyncIoContext::AsyncIoContext(std::unique_ptr<AsyncBackend> backend, AsyncStats* stats)
    : backend_(std::move(backend)), stats_(stats) {
    // Hand the caller-owned stats sink to the backend so it can tally per-
    // completion outcomes it knows directly (canceled_ops, completion_errors)
    // at completion time, rather than the context re-scanning results after
    // poll(). Job 021 (cancellation spike).
    if (backend_) backend_->attach_stats(stats_);
}

AsyncIoContext::~AsyncIoContext() {
    // ADR L11: destroying with outstanding Completions is a contract violation.
    // Debug asserts; release silently tears down (cancel/drain is NOT implicit —
    // silent teardown would hide bugs, but a throw from a destructor terminates).
    if (backend_) {
        assert(backend_->outstanding() == 0 &&
               "AsyncIoContext destroyed with outstanding Completions (L11)");
    }
}

AsyncIoContext::AsyncIoContext(AsyncIoContext&&) noexcept = default;
AsyncIoContext& AsyncIoContext::operator=(AsyncIoContext&&) noexcept = default;

namespace {
void tally_submit(AsyncStats* s, const Result<void>& r) {
    if (!s) return;
    ++s->submit_calls;
    if (r.has_value()) {
        ++s->submitted_ops;
    } else if (r.error().code == IoError::Code::invalid_state) {
        // L8 reject path — counts toward queue_full_retries (the "submit rejected
        // then retried" stat) when the caller retries.
        ++s->queue_full_retries;
    }
}
void update_max_outstanding(AsyncStats* s, std::size_t cur) {
    if (s && cur > s->max_outstanding) s->max_outstanding = cur;
}
}  // namespace

Result<void> AsyncIoContext::submit_read(ReadOp op, Completion<std::size_t>& c) {
    auto r = backend_->submit_read(op, c);
    tally_submit(stats_, r);
    update_max_outstanding(stats_, backend_->outstanding());
    return r;
}
Result<void> AsyncIoContext::submit_write(WriteOp op, Completion<std::size_t>& c) {
    auto r = backend_->submit_write(op, c);
    tally_submit(stats_, r);
    update_max_outstanding(stats_, backend_->outstanding());
    return r;
}
Result<void> AsyncIoContext::submit_sync_data(SyncDataOp op, Completion<void>& c) {
    auto r = backend_->submit_sync_data(op, c);
    tally_submit(stats_, r);
    update_max_outstanding(stats_, backend_->outstanding());
    return r;
}
Result<void> AsyncIoContext::submit_sync_all(SyncAllOp op, Completion<void>& c) {
    auto r = backend_->submit_sync_all(op, c);
    tally_submit(stats_, r);
    update_max_outstanding(stats_, backend_->outstanding());
    return r;
}

std::size_t AsyncIoContext::poll() {
    if (stats_) ++stats_->poll_calls;
    std::size_t before = backend_->outstanding();
    std::size_t n = backend_->poll();
    if (stats_) stats_->completed_ops += n;
    (void)before;
    return n;
}

Result<std::size_t> AsyncIoContext::wait_one() {
    if (stats_) ++stats_->wait_calls;
    auto r = backend_->wait_one();
    if (r.has_value() && stats_) stats_->completed_ops += r.value();
    return r;
}

void AsyncIoContext::cancel(Completion<std::size_t>& c) {
    backend_->cancel(c);
}
void AsyncIoContext::cancel(Completion<void>& c) {
    backend_->cancel(c);
}

std::size_t AsyncIoContext::outstanding() const noexcept {
    return backend_ ? backend_->outstanding() : 0;
}

}  // namespace sluice::async
