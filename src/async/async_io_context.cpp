// Implementation of AsyncIoContext (serialized backend access).
//
// The context routes submit_*/poll/wait_one/cancel to its owned backend and
// tallies AsyncStats. access_mtx_ serializes all backend calls
// (at most one concurrent caller). This satisfies the ADR §9.2.5 serialized
// backend access domain contract.
//
// Lifetime contract for outstanding Completions. Per
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

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include <thread>  // std::this_thread::yield (D4-RM13 pause gate spin)
#endif

namespace sluice::async {

AsyncIoContext::AsyncIoContext(std::unique_ptr<AsyncBackend> backend, AsyncStats* stats)
    : backend_(std::move(backend)), stats_(stats) {
    if (backend_) backend_->attach_stats(stats_);
}

AsyncIoContext::~AsyncIoContext() {
    if (backend_ && backend_->outstanding() != 0) {
        // ADR §5 L11: outstanding-when-destroyed.
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
        // The DESTINATION must not already own a backend with
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

// TAX-0D F01 seam consult — external linkage, defined only in the
// sluice_async_internal_testing target (tests/tax0_ablation_seams.cpp).
namespace detail {
bool tax0_f01_gate_outstanding_eval() noexcept;
}
using detail::tax0_f01_gate_outstanding_eval;

namespace {
// Tally one submit_* result into AsyncStats. This is the SINGLE counting
// authority for the reject-path counters on the L8 reject path:
//   - would_block (capacity pressure, ADR Decision 6/13) is the canonical
//     queue-full retry signal and counts queue_full_retries. A backend MAY
//     also bump queue_full_retries for a backend_error ring-full path (Uring
//     does; tally_submit cannot see it since it returns backend_error, not
//     would_block).
//   - invalid_state (non-idle Completion, admission closed, lifecycle misuse)
//     is a CALLER contract violation, NOT capacity pressure. It counts
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
        // ADR Decision 6/13: configured-capacity pressure is reported
        // as would_block by the reference backends; it is the canonical
        // queue-full retry signal (the arena-level capacity_rejections counter
        // tracks the same event at the arena).
        ++s->queue_full_retries;
    } else if (r.error().code == IoError::Code::invalid_state) {
        // Caller lifecycle violation (non-idle Completion, admission closed,
        // lifecycle misuse) — NOT capacity pressure. Counted in its own metric
        // so queue_full_retries never conflates the two.
        ++s->invalid_state_rejections;
    }
}
void update_max_outstanding(AsyncStats* s, std::size_t cur) {
    if (s && cur > s->max_outstanding) s->max_outstanding = cur;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
// TAX-0D F01 research ablation (#250/PR #260): R1 evaluates the
// backend_->outstanding() argument (one virtual call + arena leaf lock
// round-trip per submit) only when stats can observe it. Production
// behavior (R0, default) stays unconditional. Never compiled outside the
// sluice_async_internal_testing target.
void tax0_f01_update_max_outstanding(AsyncStats* s, AsyncBackend& b) {
    if (tax0_f01_gate_outstanding_eval() && s == nullptr) return;
    update_max_outstanding(s, b.outstanding());
}
#else
// Production path: the identical unconditional evaluation the submit_*
// methods always performed; the helper only names the shared tail.
void tax0_f01_update_max_outstanding(AsyncStats* s, AsyncBackend& b) {
    update_max_outstanding(s, b.outstanding());
}
#endif
}  // namespace

Result<void> AsyncIoContext::submit_read(ReadOp op, Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_read(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    return r;
}
Result<void> AsyncIoContext::submit_write(WriteOp op, Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_write(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    return r;
}
Result<void> AsyncIoContext::submit_sync_data(SyncDataOp op, Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_sync_data(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    return r;
}
Result<void> AsyncIoContext::submit_sync_all(SyncAllOp op, Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_sync_all(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    return r;
}

// ADR-public-request-handle: additive identity-returning submit.
// Capability is checked BEFORE accepting so a non-identity backend returns
// not_supported with no side effect (Decision 5). On rejection the submit error
// propagates and NO handle is produced (Decision 4). On success the handle is
// derived from the just-bound Completion under the same lock (no TOCTOU).
Result<RequestHandle> AsyncIoContext::submit_read_request(ReadOp op,
                                                          Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_->supports_request_identity())
        return make_unexpected<RequestHandle>(IoError{IoError::Code::not_supported});
    auto r = backend_->submit_read(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value()) return make_unexpected<RequestHandle>(r.error());
    return backend_->identity_of(c);
}
Result<RequestHandle> AsyncIoContext::submit_write_request(WriteOp op,
                                                           Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_->supports_request_identity())
        return make_unexpected<RequestHandle>(IoError{IoError::Code::not_supported});
    auto r = backend_->submit_write(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value()) return make_unexpected<RequestHandle>(r.error());
    return backend_->identity_of(c);
}
Result<RequestHandle> AsyncIoContext::submit_sync_data_request(SyncDataOp op,
                                                               Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_->supports_request_identity())
        return make_unexpected<RequestHandle>(IoError{IoError::Code::not_supported});
    auto r = backend_->submit_sync_data(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value()) return make_unexpected<RequestHandle>(r.error());
    return backend_->identity_of(c);
}
Result<RequestHandle> AsyncIoContext::submit_sync_all_request(SyncAllOp op,
                                                              Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_->supports_request_identity())
        return make_unexpected<RequestHandle>(IoError{IoError::Code::not_supported});
    auto r = backend_->submit_sync_all(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value()) return make_unexpected<RequestHandle>(r.error());
    return backend_->identity_of(c);
}

// Read-only identity consumer (ADR Decision 6).
Result<RequestHandleState> AsyncIoContext::request_state(const RequestHandle& h) const {
    std::lock_guard<std::mutex> lk(access_mtx_);
    return backend_->request_handle_state(h);
}

std::size_t AsyncIoContext::poll() {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (stats_) ++stats_->poll_calls;
    std::size_t n = backend_->poll();
    if (stats_) stats_->completed_ops += n;
    return n;
}

Result<std::size_t> AsyncIoContext::wait_one() {
    // The no-argument form is the unbounded entry; the bounded
    // variant carries the deadline-driven park cap (see wait_one(max_park)).
    return wait_one(std::chrono::nanoseconds::max());
}

Result<std::size_t> AsyncIoContext::wait_one(std::chrono::nanoseconds max_park) {
    // wait_calls counts the single external wait invocation.
    // The blocking phase NEVER runs under access_mtx_: with a
    // split-wait-capable backend the call repeatedly
    //   snapshot -> poll (serialized reap) -> park in the observe-only wait
    // so a second participant's poll/reap path stays reachable while this
    // caller waits.
    //
    // Stats accounting stays inside access_mtx_: AsyncStats fields are plain
    // std::uint64_t (caller-owned, never atomic — see measurement.hpp), and
    // access_mtx_ is the single serialized consuming/accounting domain for
    // this context (AGENTS.md §4.1/§13.1 leaf domain). The split-wait fix
    // moved the PARK out of the lock; it MUST NOT also move accounting out,
    // or two wait_one() callers (or a wait_one() and a poll()) race on
    // wait_calls / completed_ops.
    BackendWaitSource* ws = backend_ ? backend_->wait_source() : nullptr;
    if (ws == nullptr) {
        // Legacy backend without the split wait capability: keep the original
        // serialized contract — the whole call, including a backend-side
        // block, runs under access_mtx_. This is safe when the backend's
        // wait_one does not block (SyncBackend / FakeAsyncBackend) or when
        // the caller is the single documented driver (UringAsyncBackend).
        // ApplicationRuntime rejects such backends at build time so the
        // multi-participant production path never takes this fallback.
        // The bounded-park hint is not plumbed here: legacy backends
        // have no split wait source to bound (the Scheduler never sends a
        // bounded hint to them — non-split-wait backends keep the
        // Scheduler-domain observation park).
        (void)max_park;
        std::lock_guard<std::mutex> lk(access_mtx_);
        if (stats_) ++stats_->wait_calls;
        auto r = backend_->wait_one();
        if (r.has_value() && stats_) stats_->completed_ops += r.value();
        return r;
    }
    // A finite park cap is a REAL contract
    // only when the wait source implements the bounded transport. A wait
    // source that only provides the unbounded one-argument contract would
    // silently discard `max_park` in the base wait_for_change overload and
    // park indefinitely past the caller's deadline (the liveness hole).
    // Fail synchronously instead — no park, no accounting side effect, no
    // silent fallback; the caller (the Scheduler) routes deadline-bound
    // parks away from such backends via has_bounded_split_wait_capability()
    // BEFORE reaching this point.
    if (max_park != std::chrono::nanoseconds::max() &&
        !ws->supports_bounded_wait()) {
        return make_unexpected<std::size_t>(
            IoError{IoError::Code::not_supported});
    }
    // wait_calls counts ONE external invocation, not one loop iteration.
    // Account it once under access_mtx_ before entering the reap/park loop;
    // spurious-wake re-scans MUST NOT bump it again.
    {
        std::lock_guard<std::mutex> lk(access_mtx_);
        if (stats_) ++stats_->wait_calls;
    }
    // The CONTROL baseline belongs to the whole external
    // wait_one() invocation, not to one internal progress iteration. A
    // control-plane wake (close_admission / interrupt_backend_waiters) that
    // lands ANY time after this call began must be observed by THIS call —
    // including the window between wait_for_change() returning `progress` and
    // the next internal snapshot, where a fresh snapshot used to absorb the
    // control bump (rebaseline it into the observed token), drain its eventfd,
    // and repark forever. The PROGRESS baseline may refresh every internal
    // loop; the CONTROL baseline stays fixed for the invocation, so
    // wait_for_change reports interrupted on any control advance and this call
    // terminates with its final poll. A FUTURE wait_one() captures a fresh
    // baseline, so the interrupt stays one-shot — never a sticky shutdown
    // flag, never a busy-spin.
    //
    // Commit-to-park handshake: the invocation baseline comes
    // from consume_committed_wait() instead of a bare snapshot(). A Scheduler
    // MW-S2 participant that armed a committed-wait registration at its
    // Phase-B commit (under global_mtx_, before releasing the admission
    // authority) gets the ARMED control generation as its baseline — a stop
    // published after the commit is observed by THIS call even though it
    // landed before the call entered. With no registration the consume
    // degenerates to a fresh snapshot (unchanged behavior).
    const BackendWaitToken invocation_start = ws->consume_committed_wait();
    const std::uint64_t control_baseline = invocation_start.control_generation;
    // Bounded park: the deadline-driven cap bounds the WHOLE
    // invocation's physical parking — each internal iteration re-parks with
    // the REMAINING budget, so a spurious re-loop can never push the total
    // park past the deadline (the caller — the Scheduler's timer pump —
    // must re-drain in time). When the budget is exhausted the invocation
    // terminates with a final poll and returns 0, exactly like a control
    // interruption (the caller re-drains, pumps, and re-invokes with a fresh
    // budget). The unbounded sentinel (nanoseconds::max()) keeps the classic
    // infinite-park invocation.
    const bool bounded_park = max_park != std::chrono::nanoseconds::max();
    const auto park_deadline =
        bounded_park ? std::chrono::steady_clock::now() + max_park
                     : std::chrono::steady_clock::time_point{};
    for (;;) {
        BackendWaitToken token = ws->snapshot();
        // Refresh the progress baseline per internal loop; preserve the
        // invocation-level control baseline (invariant: one external
        // wait invocation cannot forget a control epoch that advanced during
        // that invocation).
        token.control_generation = control_baseline;
        std::size_t n = 0;
        std::size_t outstanding_now = 0;
        {
            // Reap AND account under the serialized backend domain:
            // every consuming backend access is serialized by access_mtx_.
            // completed_ops is tallied here, inside the lock, not after it —
            // a concurrent poll() / wait_one() updates the same field under
            // the same lock, and AsyncStats is not atomic.
            std::lock_guard<std::mutex> lk(access_mtx_);
            n = backend_->poll();
            outstanding_now = backend_->outstanding();
            if (n > 0 && stats_) stats_->completed_ops += n;
        }
        if (n > 0) {
            return Result<std::size_t>{n};
        }
        // Nothing reaped and nothing outstanding: nothing can ever become
        // ready — an empty wait is a no-progress boundary, not a park.
        if (outstanding_now == 0) {
            return Result<std::size_t>{0};
        }
        // Park WITHOUT access_mtx_. The snapshot-then-poll-then-wait
        // order closes both lost-wake windows: a signal before poll is seen
        // by poll; a signal between poll and park advances the epoch so the
        // predicate wait does not park. Spurious wakes only re-check the
        // predicate and re-loop.
        // With a finite deadline hint the park is bounded by the
        // invocation's REMAINING budget (see park_deadline above); when the
        // budget is exhausted the call terminates with a final poll and
        // returns 0 so the caller re-drains in time for the active deadline.
        BackendWakeReason reason;
        if (bounded_park) {
            auto remaining = park_deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::nanoseconds::zero()) {
                reason = BackendWakeReason::interrupted;
            } else {
                reason = ws->wait_for_change(token, remaining);
            }
        } else {
            reason = ws->wait_for_change(token);
        }
        if (reason == BackendWakeReason::progress) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            // D4-RM13 detector seam: pause AFTER wait_for_change reported
            // progress, BEFORE the next internal snapshot — the exact window
            // where a control wake used to be rebaselined away and the
            // participant reparked forever. Compiled out of production builds
            // (AGENTS.md §15); the layout cost in the internal-testing target
            // is accepted and documented.
            pause_after_wait_source_progress_();
#endif
            continue;
        }
        // Control-plane interruption: one final non-blocking poll closes the
        // interrupt-vs-final-ready race and its reaped count is returned — 0
        // only when that poll finds nothing (no fake completion, no
        // completed_ops inflation). The poll + accounting form one critical
        // section, same as the main reap.
        std::size_t final_n = 0;
        {
            std::lock_guard<std::mutex> lk(access_mtx_);
            final_n = backend_->poll();
            if (final_n > 0 && stats_) stats_->completed_ops += final_n;
        }
        if (final_n > 0) {
            return Result<std::size_t>{final_n};
        }
        return Result<std::size_t>{0};
    }
}

void AsyncIoContext::interrupt_backend_waiters() noexcept {
    // Control-plane wake for parked split-phase waiters:
    // unblocks every participant parked in wait_one()'s observe phase so
    // shutdown / admission close can re-evaluate. Pure control: no backend
    // state is consumed, so no access_mtx_ is taken; a backend without the
    // wait capability has no one to wake (no-op).
    //
    // This is ALSO the transport that carries
    // Scheduler wake-domain publications (routing / flag / select / waitqueue
    // / deadline / external wake handle) into a MW-S2 participant parked in
    // wait_one() — Scheduler::signal_wake_locked calls this for every wake
    // publication. The one-shot control epoch + invocation baseline
    // keep the interrupt one-shot per wait_one() invocation (no busy-spin),
    // and the armed commit baseline closes the commit-to-park window.
    if (backend_) {
        if (auto* ws = backend_->wait_source()) {
            ws->interrupt_all();
        }
    }
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
BackendWaitToken AsyncIoContext::backend_wait_token_for_test() const noexcept {
    // Phase G park-window forensics: snapshot() is the wait source's own
    // observe-only, lock-guarded read (documented contract). No access_mtx_
    // is taken — this runs from the forensics watchdog while the run is
    // stalled.
    if (backend_ != nullptr) {
        if (BackendWaitSource* ws = backend_->wait_source()) {
            return ws->snapshot();
        }
    }
    return BackendWaitToken{};
}
#endif

bool AsyncIoContext::has_split_wait_capability() const noexcept {
    // Split-wait backends expose the observe-only readiness wait
    // (BackendWaitSource). The Scheduler selects the MW-S2 park domain with
    // this — see the header comment. Const and lock-free (the backend pointer
    // and its wait_source are construction-stable).
    return backend_ && backend_->wait_source() != nullptr;
}

bool AsyncIoContext::has_bounded_split_wait_capability() const noexcept {
    // Split wait AND a bounded physical park. The
    // Scheduler may commit an MW-S2 participant to a FINITE-cap backend
    // domain park (active deadline / ready-flag observation) only when this
    // holds — see the header contract. Lock-free and construction-stable.
    return backend_ && backend_->wait_source() != nullptr &&
           backend_->wait_source()->supports_bounded_wait();
}

void AsyncIoContext::arm_backend_wait_commit() noexcept {
    // Commit-to-park handshake: register the mandatory
    // control-observation baseline for the NEXT wait_one() invocation. Called
    // by the Scheduler's MW-S2 participant under global_mtx_ at its Phase-B
    // commit — BEFORE the backend-park commitment is exposed and the
    // admission lock is released — so a control wake (request_stop ->
    // interrupt_backend_waiters) landing in the commit-to-wait_one window is
    // observed by that invocation (its control baseline is the armed
    // generation, invocation-begin semantics). One-shot: consumed by
    // the next wait_one(); a FUTURE invocation captures a fresh baseline, so
    // the interrupt stays one-shot. No-op for backends without the split wait
    // capability. Pure registration: no backend state is consumed, no
    // access_mtx_ is taken, never blocks.
    if (backend_) {
        if (auto* ws = backend_->wait_source()) {
            (void)ws->arm_committed_wait();
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

void AsyncIoContext::set_ready_sink(detail::SynchronousReadySink* sink) {
    // Serialized against an in-flight poll/reap (the sink is read by the
    // backend's reap call sites under access_mtx_).
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (backend_) backend_->attach_ready_sink(sink);
}

Result<void> AsyncIoContext::register_waiter(Completion<std::size_t>& c,
                                             detail::WaiterToken token,
                                             detail::RoutingLease lease) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return backend_->register_waiter(c, token, std::move(lease));
}
Result<void> AsyncIoContext::register_waiter(Completion<void>& c,
                                             detail::WaiterToken token,
                                             detail::RoutingLease lease) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return backend_->register_waiter(c, token, std::move(lease));
}
Result<detail::RoutingLease> AsyncIoContext::cancel_waiter(Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_) {
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::invalid_state});
    }
    return backend_->cancel_waiter(c);
}
Result<detail::RoutingLease> AsyncIoContext::cancel_waiter(Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_) {
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::invalid_state});
    }
    return backend_->cancel_waiter(c);
}

std::size_t AsyncIoContext::outstanding() const noexcept {
    std::lock_guard<std::mutex> lk(access_mtx_);
    return backend_ ? backend_->outstanding() : 0;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
void AsyncIoContext::pause_after_wait_source_progress_() noexcept {
    // D4-RM13 detector seam (see WaitSourceProgressPauseGate in the header):
    // holds NO lock (the context is between wait_for_change's return and the
    // next internal snapshot — no access_mtx_ is held on this path). No-op
    // when no gate is installed. Compiled out of production builds.
    //
    // Bidirectional like the ThreadPoolBackend pause gates (issue #92): the
    // paused store is paired with a notify for zero-CPU atomic::wait
    // consumers, and the resume side BLOCKS on resume.wait(false) instead of
    // yield-spinning. atomic::wait is woken ONLY by a notifying atomic
    // operation — a plain store of the value does NOT wake an already-parked
    // consumer (a store racing the park is the lost-wake the gate exists to
    // eliminate), so the gate's resume field is private and the only
    // publisher is resume_wait_source_progress_gate_for_test (store +
    // notify_all). This is a test-only seam; production builds carry no
    // branch.
    if (auto* g = wait_source_progress_gate_.load(std::memory_order_acquire)) {
        g->exited.store(false, std::memory_order_release);
        g->paused.store(true, std::memory_order_release);
        g->paused.notify_all();
        g->resume.wait(false, std::memory_order_acquire);
        g->exited.store(true, std::memory_order_release);
    }
}
#endif

}  // namespace sluice::async
