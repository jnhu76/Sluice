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
// authority for the reject-path counters on the L8 reject path:
//   - would_block (capacity pressure, ADR Decision 6/13) is the canonical
//     queue-full retry signal and counts queue_full_retries. A backend MAY
//     also bump queue_full_retries for a backend_error ring-full path (Uring
//     does; tally_submit cannot see it since it returns backend_error, not
//     would_block).
//   - invalid_state (non-idle Completion, admission closed, lifecycle misuse)
//     is a CALLER contract violation, NOT capacity pressure (P1-05). It counts
//     invalid_state_rejections so queue_full_retries never conflates capacity
//     pressure with lifecycle violations.
// Every AsyncBackend returns invalid_state/would_block for those cases, and
// tally_submit counts them here once, uniformly across backends. Backends MUST
// NOT also bump these counters for the same event — that double-counts.
void tally_submit(AsyncStats* s, const Result<void>& r) {
    if (!s) return;
    ++s->submit_calls;
    if (r.has_value()) {
        ++s->submitted_ops;
    } else if (r.error().code == IoError::Code::would_block) {
        // Phase B (ADR Decision 6/13): configured-capacity pressure is reported
        // as would_block by the reference backends; it is the canonical
        // queue-full retry signal (the arena-level capacity_rejections counter
        // tracks the same event at the arena).
        ++s->queue_full_retries;
    } else if (r.error().code == IoError::Code::invalid_state) {
        // Caller lifecycle violation (non-idle Completion, admission closed,
        // lifecycle misuse) — NOT capacity pressure. Counted in its own metric
        // so queue_full_retries never conflates the two (P1-05).
        ++s->invalid_state_rejections;
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
    // Issue #67 / D4: wait_calls counts the single external wait invocation
    // (I9). The blocking phase NEVER runs under access_mtx_: with a
    // split-wait-capable backend the call repeatedly
    //   snapshot -> poll (serialized reap) -> park in the observe-only wait
    // so a second participant's poll/reap path stays reachable while this
    // caller waits (I7).
    if (stats_) ++stats_->wait_calls;
    BackendWaitSource* ws = backend_ ? backend_->wait_source() : nullptr;
    if (ws == nullptr) {
        // Legacy backend without the split wait capability: keep the original
        // serialized contract — the whole call, including a backend-side
        // block, runs under access_mtx_. This is safe when the backend's
        // wait_one does not block (SyncBackend / FakeAsyncBackend) or when
        // the caller is the single documented driver (UringAsyncBackend).
        // ApplicationRuntime rejects such backends at build time so the
        // multi-participant production path never takes this fallback (D3).
        std::lock_guard<std::mutex> lk(access_mtx_);
        auto r = backend_->wait_one();
        if (r.has_value() && stats_) stats_->completed_ops += r.value();
        return r;
    }
    for (;;) {
        BackendWaitToken token = ws->snapshot();
        std::size_t n = 0;
        std::size_t outstanding_now = 0;
        {
            // Reap under the serialized backend domain (E7-C preserved: every
            // consuming backend access is still serialized by access_mtx_).
            std::lock_guard<std::mutex> lk(access_mtx_);
            n = backend_->poll();
            outstanding_now = backend_->outstanding();
        }
        if (n > 0) {
            if (stats_) stats_->completed_ops += n;
            return Result<std::size_t>{n};
        }
        // Nothing reaped and nothing outstanding: nothing can ever become
        // ready — an empty wait is a no-progress boundary, not a park.
        if (outstanding_now == 0) {
            return Result<std::size_t>{0};
        }
        // Park WITHOUT access_mtx_ (I1). The snapshot-then-poll-then-wait
        // order closes both lost-wake windows: a signal before poll is seen
        // by poll; a signal between poll and park advances the epoch so the
        // predicate wait does not park (I5). Spurious wakes only re-check the
        // predicate and re-loop (I10).
        if (ws->wait_for_change(token) == BackendWakeReason::progress) {
            continue;
        }
        // Control-plane interruption: one final non-blocking poll closes the
        // interrupt-vs-final-ready race, then return 0 (interrupted, nothing
        // reaped — I8: no fake completion, no completed_ops inflation).
        {
            std::lock_guard<std::mutex> lk(access_mtx_);
            n = backend_->poll();
        }
        if (n > 0) {
            if (stats_) stats_->completed_ops += n;
            return Result<std::size_t>{n};
        }
        return Result<std::size_t>{0};
    }
}

void AsyncIoContext::interrupt_backend_waiters() noexcept {
    // Control-plane wake for parked split-phase waiters (issue #67 / I6):
    // unblocks every participant parked in wait_one()'s observe phase so
    // shutdown / admission close can re-evaluate. Pure control: no backend
    // state is consumed, so no access_mtx_ is taken; a backend without the
    // wait capability has no one to wake (no-op).
    if (backend_) {
        if (auto* ws = backend_->wait_source()) {
            ws->interrupt_all();
        }
    }
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
