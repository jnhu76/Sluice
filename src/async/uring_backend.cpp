// Implementation of UringAsyncBackend (sluice-CORE-020B).
//
// Phase D1: migrated onto the bounded RequestArena / RequestSlot lifecycle
// with a PRIVATE io_uring ring per backend instance (ADR Decision 18 — Uring
// execution-ownership amendment). See
// docs/architecture/phase-d1-uring-frozen-design.md.
//
//   RequestArena            = logical request lifecycle / generation / terminal
//   one private io_uring    = execution ownership domain
//   io_uring_submit()       = transport progress only (NO RequestState change)
//   original operation CQE  = execution retirement / terminal candidate
//   RequestArena::reap()    = sole Completion-ready publication authority
//
// Two modes via SLUICE_HAS_LIBURING:
//   * defined: real io_uring via liburing.
//   * undefined: UNSUPPORTED STUB. submit_* returns backend_error
//     synchronously so the project builds/links with no liburing dependency.
//
// Cancel (ADR Decision 11): pending/enqueued cancel may win directly (its SQE
// was NEVER installed). running/ring-owned cancel records intent only; an
// appended IORING_OP_ASYNC_CANCEL produces a CONTROL CQE that is informational
// only — it never publishes a terminal. The original operation CQE decides.
#include <sluice/async/uring_backend.hpp>

#include <sluice/detail/io_validation.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <cstdio>
#include <limits>
#include <utility>

#if defined(SLUICE_HAS_LIBURING)
#include <liburing.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#endif

namespace sluice::async {

#if !defined(SLUICE_HAS_LIBURING)
// ---------------------------------------------------------------------------
// Stub mode: no liburing dependency. submit_* rejects synchronously; nothing
// is recorded outstanding so outstanding() stays 0 and the L11 check passes.
// ---------------------------------------------------------------------------

UringAsyncBackend::UringAsyncBackend(unsigned queue_depth) : available_(false) {
    (void)queue_depth; // stub: no ring to size
}

UringAsyncBackend::~UringAsyncBackend() = default;

namespace {
Result<void> unsupported_stub() {
    return make_unexpected<void>(IoError{IoError::Code::backend_error});
}
} // namespace

Result<void> UringAsyncBackend::submit_read(ReadOp, Completion<std::size_t>&) {
    return unsupported_stub();
}
Result<void> UringAsyncBackend::submit_write(WriteOp, Completion<std::size_t>&) {
    return unsupported_stub();
}
Result<void> UringAsyncBackend::submit_sync_data(SyncDataOp, Completion<void>&) {
    return unsupported_stub();
}
Result<void> UringAsyncBackend::submit_sync_all(SyncAllOp, Completion<void>&) {
    return unsupported_stub();
}

std::size_t UringAsyncBackend::poll() {
    return 0;
}
Result<std::size_t> UringAsyncBackend::wait_one() {
    return std::size_t{0};
}
void UringAsyncBackend::cancel(Completion<std::size_t>&) {}
void UringAsyncBackend::cancel(Completion<void>&) {}
std::size_t UringAsyncBackend::outstanding() const noexcept {
    return 0;
}
bool UringAsyncBackend::available() const noexcept {
    return available_;
}

#else // SLUICE_HAS_LIBURING --------------------------------------------------

// ---------------------------------------------------------------------------
// Real io_uring path — Phase D1 private-ring / ring-owned RequestArena model.
//
// Identity (P0-B): SQE.user_data = a 64-bit op_cookie (operation) or the
// reserved CONTROL_CANCEL value (cancel). The op_cookie is allocated from a
// no-wrap 64-bit counter and is NEVER reused within backend lifetime. The
// CqeRouter is keyed by cookie VALUE: a CQE carries a cookie, we scan the
// fixed router for the LIVE entry whose cookie matches, recover the full
// SlotHandle{slot, full uint64 generation}, and the arena re-validates the
// generation. Because the cookie is never reused, a stale CQE (whose cookie
// was retired) matches no live entry and is dropped — the ABA window that
// existed when user_data carried router_slot+1 (which recycled) is closed.
//
// Dispatch (P0-A + P0-C): under ONE dispatch_mtx_ critical section, peek the
// dispatch-queue front, obtain an SQE, fill it, set_data64(cookie), install
// the router entry, mark_running(h) (MUST succeed — the lock discipline means
// no cancel can have terminalized h first), and remove_exact(h) (MUST
// succeed). After io_uring_get_sqe() returns non-NULL there is NO recoverable
// rollback: io_uring_get_sqe() advances liburing's application-side sqe_tail,
// so the obtained SQE WILL be flushed by the next io_uring_submit() and the
// kernel WILL see it. Abandoning it would let it execute with a stale/wrong
// user_data. Therefore any failure after get_sqe is an invariant violation
// (fail-fast), not a recoverable error.
//
// Submit: io_uring_submit() is TRANSPORT ONLY. It never mutates RequestState.
// The submit return count is NOT lifecycle authority (a scalar count cannot
// classify which SQEs the kernel consumed vs. not).
//
// CQE: the handler calls ONLY arena.record_terminal() (+ signal progress). It
// never calls AsyncBackend::publish() directly — reap is the sole publisher.
//
// Threading: AsyncBackend is single-driver-thread (poll/wait_one/submit/cancel
// are serialized by AsyncIoContext::access_mtx_ at the context layer). The
// dispatch_mtx_ below guards the local dispatch ring + router/scratch mutation
// for the future multi-producer seam; D1 is single-driver but the lock makes
// the coordination domain explicit and TSan-honest.
// ---------------------------------------------------------------------------

// Opaque pimpl owning the private io_uring instance + the internal-testing
// submit hook. Defined here (in the sluice::async namespace, matching the
// header's forward declaration) so the public header never includes
// <liburing.h>.
struct UringRingState {
    ::io_uring ring{};
#if defined(SLUICE_URING_INTERNAL_TESTING)
    UringBackendSubmitTestHooks test_hooks{};
#endif
};

namespace {

// Reserved control user_data for an IORING_OP_ASYNC_CANCEL SQE. Its CQE is
// informational only (res ∈ {0, -ENOENT, -EALREADY}) and never owns a
// RequestKey. Modeled on liburing's LIBURING_UDATA_TIMEOUT = (__u64)-1.
constexpr std::uint64_t CONTROL_CANCEL = static_cast<std::uint64_t>(-1);

// Toggle a stat counter if a sink is attached.
inline void bump(sluice::AsyncStats* s, std::uint64_t sluice::AsyncStats::* field) {
    if (s)
        ++(s->*field);
}

Result<void> no_ring() {
    return make_unexpected<void>(IoError{IoError::Code::backend_error});
}

} // namespace

// ---------------------------------------------------------------------------
// Bounded local dispatch ring (capacity == request_capacity). Mirrors the
// ThreadPoolBackend::BoundedDispatchQueue discipline: noexcept push/pop/
// remove_exact, never allocates after construction, never stores Completion*.
// ---------------------------------------------------------------------------

class UringAsyncBackend::BoundedDispatchQueue {
  public:
    explicit BoundedDispatchQueue(std::size_t capacity) : storage_(capacity), capacity_(capacity) {}
    bool empty() const noexcept { return size_ == 0; }
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }

    // Peek the front entry WITHOUT removing it. The dispatch ownership model
    // (frozen design §4.2 peek protocol) is "queue membership == local
    // execution ownership": a request stays in the queue while the dispatcher
    // attempts to transfer it to ring ownership, and is removed only by the
    // successful transfer (dispatch_one_locked's remove_exact) or by cancel's
    // remove_exact. Peeking — instead of pop_front→dispatch→(re-)push_back —
    // removes the contradiction where remove_exact(h) would fail-fast because
    // h was already popped. Caller MUST check empty() first; front() on an
    // empty queue is an invariant violation.
    detail::SlotHandle front() const noexcept {
        if (size_ == 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: dispatch ring "
                                 "front() on empty queue (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        return storage_[head_];
    }

    // noexcept push. Caller guarantees room (dispatch capacity == request
    // capacity, and a committed request holds its slot); a full push is an
    // invariant fail-fast, not would_block (AGENTS.md §12).
    void push_back(detail::SlotHandle h) noexcept {
        if (size_ >= capacity_) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: dispatch ring overflow "
                                 "(invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        storage_[(head_ + size_) % capacity_] = h;
        ++size_;
    }
    bool pop_front(detail::SlotHandle& out) noexcept {
        if (size_ == 0)
            return false;
        out = storage_[head_];
        head_ = (head_ + 1) % capacity_;
        --size_;
        return true;
    }
    // noexcept bounded compaction: remove the first entry whose slot+gen
    // match h exactly. O(capacity). Returns true if removed.
    bool remove_exact(detail::SlotHandle h) noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            std::size_t idx = (head_ + i) % capacity_;
            if (storage_[idx].slot.value == h.slot.value &&
                storage_[idx].generation.value == h.generation.value) {
                // shift the remainder forward
                for (std::size_t j = i; j + 1 < size_; ++j) {
                    std::size_t a = (head_ + j) % capacity_;
                    std::size_t b = (head_ + j + 1) % capacity_;
                    storage_[a] = storage_[b];
                }
                --size_;
                return true;
            }
        }
        return false;
    }

  private:
    std::vector<detail::SlotHandle> storage_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t capacity_;
};

// ---------------------------------------------------------------------------
// Descriptor validation (ADR Decision 6; AGENTS.md §9.1). A REAL syscall
// backend validates representationally malformed descriptors before commit.
// No fcntl(F_GETFD) preflight (TOCTOU): a non-negative but closed fd may be
// accepted and later complete with the real EBADF terminal.
// ---------------------------------------------------------------------------

Result<void> UringAsyncBackend::validate_read(ReadOp op) {
    if (op.fd < 0)
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    if (op.len > 0 && op.dst == nullptr) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    auto off = sluice::detail::checked_posix_offset(op.offset);
    if (!off.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    if (op.len > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    return {};
}
Result<void> UringAsyncBackend::validate_write(WriteOp op) {
    if (op.fd < 0)
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    if (op.len > 0 && op.src == nullptr) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    auto off = sluice::detail::checked_posix_offset(op.offset);
    if (!off.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    if (op.len > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    return {};
}
Result<void> UringAsyncBackend::validate_sync(SyncDataOp op) {
    if (op.fd < 0)
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    return {};
}
Result<void> UringAsyncBackend::validate_sync(SyncAllOp op) {
    if (op.fd < 0)
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    return {};
}

template <class Op> Result<void> UringAsyncBackend::validate_op(const Op& op) noexcept {
    if constexpr (std::is_same_v<Op, ReadOp>) {
        return validate_read(op);
    } else if constexpr (std::is_same_v<Op, WriteOp>) {
        return validate_write(op);
    } else {
        return validate_sync(op);
    }
}

// ---------------------------------------------------------------------------
// Publication thunks + terminal conversion (ADR Decision 9). The arena calls
// these inside the leaf domain at reap; they convert TerminalResult -> Result<T>
// and publish Completion-ready through the protected helper. Identical in
// shape to ThreadPoolBackend's thunks.
// ---------------------------------------------------------------------------

void UringAsyncBackend::publish_size_ready(void* completion,
                                           const detail::TerminalResult& t) noexcept {
    AsyncBackend::publish(*static_cast<Completion<std::size_t>*>(completion), terminal_to_size(t));
}

void UringAsyncBackend::publish_void_ready(void* completion,
                                           const detail::TerminalResult& t) noexcept {
    AsyncBackend::publish(*static_cast<Completion<void>*>(completion), terminal_to_void(t));
}

Result<std::size_t> UringAsyncBackend::terminal_to_size(const detail::TerminalResult& t) noexcept {
    if (t.stored && t.is_error)
        return make_unexpected<std::size_t>(t.error);
    return Result<std::size_t>{static_cast<std::size_t>(t.bytes)};
}

Result<void> UringAsyncBackend::terminal_to_void(const detail::TerminalResult& t) noexcept {
    if (t.stored && t.is_error)
        return make_unexpected<void>(t.error);
    return {};
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UringAsyncBackend::UringAsyncBackend(unsigned queue_depth)
    : UringAsyncBackend(UringConfig{static_cast<std::size_t>(queue_depth > 0 ? queue_depth : 64),
                                    queue_depth > 0 ? queue_depth : 64}) {}

UringAsyncBackend::UringAsyncBackend(UringConfig config)
    : arena_(detail::ContextIdentity::for_testing(next_backend_id()), config.request_capacity),
      prepared_ops_(config.request_capacity), router_(config.request_capacity),
      cancel_scratch_(config.request_capacity), cookie_free_list_(config.request_capacity),
      queue_depth_(config.queue_depth), ring_state_(std::make_unique<UringRingState>()) {
    if (config.request_capacity == 0 || config.queue_depth == 0) {
        throw std::invalid_argument("UringConfig fields must be > 0");
    }
    // Seed the router ARRAY-slot free-list: every router slot is initially
    // free. The free-list recycles router ARRAY slots only (the bounded
    // router); it NEVER recycles cookie VALUES. Cookie uniqueness comes from
    // allocate_cookie_()'s no-wrap counter, which fail-fasts before ever
    // reaching the reserved CONTROL_CANCEL range (mirrors RequestArena
    // generation no-wrap discipline). next_cookie_ starts at 1 (0 is unused;
    // CONTROL_CANCEL == UINT64_MAX).
    for (std::uint32_t i = 0; i < config.request_capacity; ++i) {
        cookie_free_list_[i] = detail::SlotIndex{i};
    }
    dispatch_ = std::make_unique<BoundedDispatchQueue>(config.request_capacity);
    if (::io_uring_queue_init(config.queue_depth, &ring_state_->ring, /*flags=*/0) == 0) {
        have_ring_ = true;
        available_ = true;
    }
    // Construction is allowed to fail (e.g. kernel without io_uring); the
    // instance is then constructible-but-unavailable, mirroring the stub's
    // available()==false contract. submit_* will reject synchronously.
}

#if defined(SLUICE_URING_INTERNAL_TESTING)
UringAsyncBackend::UringAsyncBackend(UringConfig config, UringBackendSubmitTestHooks hooks)
    : UringAsyncBackend(config) {
    ring_state_->test_hooks = hooks;
}
#endif

UringAsyncBackend::~UringAsyncBackend() {
    if (have_ring_) {
        ::io_uring_queue_exit(&ring_state_->ring);
        have_ring_ = false;
    }
}

// ---------------------------------------------------------------------------
// Five-stage admission (ADR Decision 5; mirrors ThreadPoolBackend).
// Stages 1–3c run under the dispatch admission path; the Completion
// `binding -> outstanding` release-store is the commit/accept linearization
// point. Capacity full -> would_block; admission closed -> invalid_state;
// malformed descriptor -> invalid_argument (Stage 1.5, after reserve).
// ---------------------------------------------------------------------------

template <class Op>
Result<void> UringAsyncBackend::submit_size(Op op, Completion<std::size_t>& c,
                                            detail::OperationKind kind, std::size_t len) {
    if (!have_ring_)
        return no_ring();
    if (fatal_error_.has_value())
        return make_unexpected<void>(*fatal_error_);
    if (admission_closed_) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }

    detail::SlotHandle h{};
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        // Stage 1: reserve. Arena full -> would_block.
        auto rh = arena_.reserve();
        if (!rh.has_value())
            return make_unexpected<void>(rh.error());
        h = rh.value();

        // Stage 1.5: descriptor validation INSIDE the admission transaction,
        // AFTER reserve — admission/capacity take precedence over malformed
        // descriptor (ADR Decision 5 stage order). Roll back on failure.
        auto v = validate_op(op);
        if (!v.has_value()) {
            (void)arena_.rollback_reserved_or_prepared(h);
            return v;
        }

        // Stage 2: prepare (op kind + fd/buffer borrow metadata).
        auto ph = arena_.prepare(h, kind, borrow_of(op));
        if (!ph.has_value()) {
            (void)arena_.rollback_reserved_or_prepared(h);
            return make_unexpected<void>(ph.error());
        }
        // Record the fixed prepared op into per-slot scratch. Dispatch reads
        // this only after mark_running(h) succeeds (current-generation enqueued).
        prepared_ops_[h.slot.value] = PreparedUringOp{
            kind, op.fd, static_cast<const std::byte*>(borrow_of(op).address), op.len, op.offset};

        // Stage 2.5: install the slot's Completion publication binding.
        auto bh = arena_.install_publication_binding(h, &c, len, &publish_size_ready);
        if (!bh.has_value()) {
            (void)arena_.rollback_reserved_or_prepared(h);
            return make_unexpected<void>(bh.error());
        }

        // Stage 3a: Completion CAS idle -> binding elects ONE submitter.
        if (!begin_binding(c)) {
            (void)arena_.rollback_reserved_or_prepared(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        // Stage 3b: commit (pending + pin + accepted++ + borrow begins).
        auto ch = arena_.commit(h);
        if (!ch.has_value()) {
            rollback_binding_before_accept(c);
            (void)arena_.rollback_reserved_or_prepared(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        // Stage 3c: install slot-release capability, then publish outstanding.
        // AFTER commit_binding nothing may throw (I9).
        install_binding(c, &arena_, h);
        commit_binding(c);
    } // dispatch_mtx_ released BEFORE enqueue (no-fail, needs no serialization).

    // Stage 4: enqueue + dispatch attempt under one dispatch_mtx_ critical
    // section (frozen design §4.2).
    enqueue_after_commit(h);
    return {};
}

template <class Op>
Result<void> UringAsyncBackend::submit_void(Op op, Completion<void>& c,
                                            detail::OperationKind kind) {
    if (!have_ring_)
        return no_ring();
    if (fatal_error_.has_value())
        return make_unexpected<void>(*fatal_error_);
    if (admission_closed_) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }

    detail::SlotHandle h{};
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        auto rh = arena_.reserve();
        if (!rh.has_value())
            return make_unexpected<void>(rh.error());
        h = rh.value();

        auto v = validate_op(op);
        if (!v.has_value()) {
            (void)arena_.rollback_reserved_or_prepared(h);
            return v;
        }
        auto ph = arena_.prepare(h, kind, detail::BorrowMetadata{op.fd, nullptr, 0});
        if (!ph.has_value()) {
            (void)arena_.rollback_reserved_or_prepared(h);
            return make_unexpected<void>(ph.error());
        }
        prepared_ops_[h.slot.value] =
            PreparedUringOp{kind, op.fd, nullptr, std::size_t{0}, std::uint64_t{0}};

        auto bh = arena_.install_publication_binding(h, &c, 0, &publish_void_ready);
        if (!bh.has_value()) {
            (void)arena_.rollback_reserved_or_prepared(h);
            return make_unexpected<void>(bh.error());
        }
        if (!begin_binding(c)) {
            (void)arena_.rollback_reserved_or_prepared(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        auto ch = arena_.commit(h);
        if (!ch.has_value()) {
            rollback_binding_before_accept(c);
            (void)arena_.rollback_reserved_or_prepared(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        install_binding(c, &arena_, h);
        commit_binding(c);
    }

    enqueue_after_commit(h);
    return {};
}

Result<void> UringAsyncBackend::submit_read(ReadOp op, Completion<std::size_t>& c) {
    return submit_size(op, c, detail::OperationKind::read, op.len);
}
Result<void> UringAsyncBackend::submit_write(WriteOp op, Completion<std::size_t>& c) {
    return submit_size(op, c, detail::OperationKind::write, op.len);
}
Result<void> UringAsyncBackend::submit_sync_data(SyncDataOp op, Completion<void>& c) {
    return submit_void(op, c, detail::OperationKind::sync_data);
}
Result<void> UringAsyncBackend::submit_sync_all(SyncAllOp op, Completion<void>& c) {
    return submit_void(op, c, detail::OperationKind::sync_all);
}

// ---------------------------------------------------------------------------
// Enqueue + dispatch (frozen design §4). One dispatch_mtx_ critical section.
// ---------------------------------------------------------------------------

void UringAsyncBackend::enqueue_after_commit(detail::SlotHandle h) noexcept {
    detail::EnqueueOutcome outcome;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        outcome = arena_.enqueue(h); // pending -> enqueued OR terminal_noop
        if (outcome == detail::EnqueueOutcome::enqueued) {
            dispatch_->push_back(h);
            // P0-A peek protocol: enqueue (push_back) and the best-effort
            // ownership transfer (dispatch_one_locked) run under ONE
            // dispatch_mtx_ critical section. This is load-bearing: cancel()
            // acquires the SAME lock, so by holding it across push_back →
            // mark_running there is no window where a cancel could terminalize
            // h between enqueue and dispatch. Therefore mark_running == false
            // inside dispatch_one_locked can ONLY be an invariant violation
            // (handled there), never a legitimate cancel-won race. If the SQ is
            // full, h stays at the tail of the dispatch queue and a later
            // poll()/wait_one() retries via the peek drain loop. We do NOT
            // pop_front→dispatch→push_back: that pattern contradicts the
            // remove_exact(h) the successful transfer performs (it would
            // fail-fast because h was already popped).
            (void)dispatch_one_locked(h);
        }
        // terminal_noop: a pending cancel won first (Scheme B). No dispatch
        // linkage; that winner owns readiness.
    }
    if (outcome != detail::EnqueueOutcome::enqueued) {
        // terminal_noop: re-arm the ready condition so the wake is not lost
        // (ADR Decision 4 / I19). Done OUTSIDE the dispatch lock — it is a
        // no-op seam in D1's single-driver model.
        signal_ready_progress();
    }
}

// Public dispatch entry (acquires dispatch_mtx_). Used only by the post-commit
// fast path's neighbors; the peek drains in poll()/wait_one() call
// dispatch_one_locked directly under a held lock.
bool UringAsyncBackend::dispatch_one(detail::SlotHandle h) noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    return dispatch_one_locked(h);
}

// The ownership-transfer transaction (frozen design §4 + P0-A/P0-B/P0-C).
// Assumes dispatch_mtx_ is held AND h == dispatch_->front() (peek protocol).
// Returns false if the request could not be dispatched this pass (SQ full /
// fatal) — in that case h stays at the front of the dispatch queue and a later
// poll()/wait_one() retries. Returns true if the request became ring-owned
// (the queue entry is retired by remove_exact in that case).
bool UringAsyncBackend::dispatch_one_locked(detail::SlotHandle h) noexcept {
    if (fatal_error_.has_value())
        return false;

    // ---- pre-get_sqe region: all operations here are recoverable -----------
    // Reserve a router ARRAY slot up front. Exhaustion is an invariant
    // violation (router capacity == request_capacity == max live requests, and
    // a live request holds its slot). Reserve BEFORE get_sqe so a NULL SQE
    // path can simply push the slot back onto the free-list and return — no
    // SQE has been obtained yet, so rollback is still legal.
    if (cookie_free_list_.empty()) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: router exhaustion "
                             "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    detail::SlotIndex router_slot = cookie_free_list_.back();
    cookie_free_list_.pop_back();

    // Obtain an SQE. If the SQ is full, flush transport progress and retry
    // once; if still full, push the router slot back and return false (h stays
    // enqueued). NO SQE has been consumed by the retry-flush that matters
    // here because get_sqe() had not yet succeeded.
    io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_state_->ring);
    if (sqe == nullptr) {
        (void)submit_transport(); // transport only; no RequestState change
        if (fatal_error_.has_value()) {
            cookie_free_list_.push_back(router_slot);
            return false;
        }
        sqe = ::io_uring_get_sqe(&ring_state_->ring);
        if (sqe == nullptr) {
            // SQ still full: legal back-off. Restore the reserved router slot
            // and leave h at the front of the dispatch queue. No SQE was
            // obtained, so this rollback is sound.
            cookie_free_list_.push_back(router_slot);
            return false;
        }
    }

    // ---- NO-FAIL REGION: io_uring_get_sqe() advanced liburing's sqe_tail, --
    // so the obtained SQE WILL be flushed by the next io_uring_submit() and the
    // kernel WILL see whatever user_data we install here. There is NO
    // recoverable rollback from this point: abandoning the SQE would let it
    // execute with a stale/wrong identity. Every step below either completes
    // the transaction or fail-fasts.
    const std::uint64_t op_cookie = allocate_cookie_(); // no-wrap; fail-fast
    const PreparedUringOp& prep = prepared_ops_[h.slot.value];
    switch (prep.kind) {
    case detail::OperationKind::read:
        ::io_uring_prep_read(sqe, prep.fd, const_cast<std::byte*>(prep.buffer), prep.length,
                             static_cast<off_t>(static_cast<std::int64_t>(prep.offset)));
        break;
    case detail::OperationKind::write:
        ::io_uring_prep_write(sqe, prep.fd, prep.buffer, prep.length,
                              static_cast<off_t>(static_cast<int64_t>(prep.offset)));
        break;
    case detail::OperationKind::sync_data:
        ::io_uring_prep_fsync(sqe, prep.fd, IORING_FSYNC_DATASYNC);
        break;
    case detail::OperationKind::sync_all:
        ::io_uring_prep_fsync(sqe, prep.fd, 0); // 0 => full fsync (sync_all)
        break;
    }
    // Kernel-visible identity = the unique op_cookie (integer user_data API).
    // The cookie is never reused, so a stale CQE cannot resolve through a
    // recycled router ARRAY slot (P0-B ABA fix).
    ::io_uring_sqe_set_data64(sqe, op_cookie);
    router_[router_slot.value] = RouterEntry{op_cookie, h, /*in_use=*/true};
    live_cookies_.fetch_add(1, std::memory_order_relaxed);

    // mark_running: enqueued -> running (the ownership transfer). Under the
    // P0-A discipline (enqueue + dispatch share ONE dispatch_mtx_ critical
    // section; cancel takes the same lock), no cancel can terminalize h
    // between enqueue and this point. Therefore mark_running == false is an
    // invariant violation, not a legitimate cancel-won race — fail-fast. (We
    // cannot "drop the SQE": get_sqe already committed it to the SQ flush.)
    if (!arena_.mark_running(h)) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: mark_running false "
                             "after get_sqe (invariant violation — cancel cannot "
                             "have won under the dispatch_mtx_ discipline)\n");
        std::fflush(stderr);
        std::terminate();
    }
    // Unlink from the local dispatch ring. P0-A peek protocol: h was the front
    // entry under the held lock, so a successful transfer MUST retire it; a
    // miss is an invariant violation (fail-fast) rather than a silent skip.
    if (!dispatch_->remove_exact(h)) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: dispatch_one_locked "
                             "remove_exact miss after mark_running (invariant "
                             "violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    // From here until the original operation CQE, the request cannot be
    // locally released. io_uring_submit() may happen now or later — it is
    // transport progress and does not change the request's running state.
    return true;
}

// ---------------------------------------------------------------------------
// Transport progress: io_uring_submit(). DOES NOT mutate RequestState.
// ---------------------------------------------------------------------------

int UringAsyncBackend::submit_transport() noexcept {
    submit_flushes_.fetch_add(1, std::memory_order_relaxed);
#if defined(SLUICE_URING_INTERNAL_TESTING)
    if (ring_state_->test_hooks.submit != nullptr) {
        return ring_state_->test_hooks.submit(ring_state_->test_hooks.context, &ring_state_->ring);
    }
#endif
    return ::io_uring_submit(&ring_state_->ring);
}

// ---------------------------------------------------------------------------
// Cookie allocator (P0-B). The operation-cookie domain is [1, UINT64_MAX-1];
// 0 is unused and UINT64_MAX (CONTROL_CANCEL) is reserved. The counter never
// wraps — reaching CONTROL_CANCEL fail-fasts, mirroring RequestArena
// generation no-wrap discipline. Because the cookie is never reused, a stale
// CQE's cookie cannot match a later LIVE router entry.
// ---------------------------------------------------------------------------
std::uint64_t UringAsyncBackend::allocate_cookie_() noexcept {
    // The counter is mutated only under dispatch_mtx_ (dispatch_one_locked /
    // the test seam). If next_cookie_ has reached the reserved control value,
    // the operation-cookie domain is exhausted — fail-fast rather than wrap to
    // 0 (which would reuse cookie 1's value on the first occupant and reopen
    // the ABA window the cookie-keyed router exists to close).
    if (next_cookie_ == 0 || next_cookie_ == CONTROL_CANCEL) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: operation-cookie "
                             "domain exhausted (would reach CONTROL_CANCEL / "
                             "wrap; invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    return next_cookie_++;
}

// Find the router ARRAY index of the LIVE entry whose SlotHandle exactly
// matches h. Returns router_.size() when no live entry matches (h is not
// currently ring-owned). Bounded O(request_capacity), allocation-free.
std::size_t UringAsyncBackend::find_live_router_index_(detail::SlotHandle h) const noexcept {
    for (std::size_t i = 0; i < router_.size(); ++i) {
        const RouterEntry& e = router_[i];
        if (e.in_use && e.handle.slot.value == h.slot.value &&
            e.handle.generation.value == h.generation.value) {
            return i;
        }
    }
    return router_.size();
}

// ---------------------------------------------------------------------------
// CQE reap. Route op cookies to full SlotHandles, validate generation,
// record_terminal ONLY (never publish). Control cancel CQEs update only fixed
// cancel bookkeeping. Returns the count of operation CQEs whose terminal was
// recorded.
// ---------------------------------------------------------------------------

void UringAsyncBackend::handle_one_cqe(std::uint64_t user_data, int res) noexcept {
    // CONTROL_CANCEL is the informational cancel-SQE CQE; 0 is never emitted by
    // us (allocate_cookie_ starts at 1).
    if (user_data == CONTROL_CANCEL || user_data == 0) {
        // Control cancel CQE (res ∈ {0, -ENOENT, -EALREADY}): informational
        // only. It MUST NOT own a RequestKey, publish a terminal, release the
        // slot, or overwrite the operation result (frozen design §8.2). The
        // cancel_queued bit was already set when the cancel SQE was appended;
        // nothing to update here.
        return;
    }
    // Operation CQE: route by COOKIE VALUE (P0-B). A bounded O(request_capacity)
    // scan of the fixed router finds the LIVE entry whose cookie matches. If no
    // live entry matches, this is a STALE cookie (its entry was retired and the
    // array slot may have been reused by a different cookie) — drop it. This is
    // the central ABA fix: the old router_slot+1 encoding resolved a stale CQE
    // to whatever NEW SlotHandle now occupied the recycled array slot.
    std::size_t router_index = router_.size();
    for (std::size_t i = 0; i < router_.size(); ++i) {
        if (router_[i].in_use && router_[i].cookie == user_data) {
            router_index = i;
            break;
        }
    }
    if (router_index == router_.size())
        return; // stale/unknown cookie; no live execution reference matched
    RouterEntry& entry = router_[router_index];

    const detail::SlotHandle h = entry.handle;
    // Convert the CQE result to a TerminalResult. res < 0 maps to IoError via
    // from_errno_value(-res) (ADR E3); -ECANCELED here is the kernel honoring
    // our cancel. res >= 0 (size op) is bytes (may be short); (void op) success.
    // Capture whether this is a byte op and the requested length BEFORE building
    // the terminal, so the short-completion tally below does not depend on a
    // TerminalResult field (the struct does not carry an is_bytes discriminator).
    const PreparedUringOp& prep = prepared_ops_[h.slot.value];
    const bool is_byte_op =
        (prep.kind == detail::OperationKind::read || prep.kind == detail::OperationKind::write);
    const std::size_t requested = prep.length;
    detail::TerminalResult terminal;
    if (res < 0) {
        terminal = detail::TerminalResult::err(sluice::from_errno_value(-res));
    } else if (is_byte_op) {
        terminal = detail::TerminalResult::ok_bytes(static_cast<std::uint64_t>(res));
    } else {
        terminal = detail::TerminalResult::ok_void();
    }
    // record_terminal takes the arena leaf lock ALONE (no dispatch_mtx_ held);
    // first caller wins, losers no-op (ADR Decision 12). The full SlotHandle is
    // re-validated by the arena (context/slot/generation).
    const bool won = arena_.record_terminal(h, terminal);
    if (won) {
        // Retire the transport routing/execution reference. The slot remains
        // bound until reap publishes and the caller resets/releases.
        entry.in_use = false;
        entry.cookie = 0;
        cookie_free_list_.push_back(detail::SlotIndex{static_cast<std::uint32_t>(router_index)});
        live_cookies_.fetch_sub(1, std::memory_order_relaxed);
        // Cancel bookkeeping cleanup (the slot is terminal now).
        cancel_scratch_[h.slot.value].cancel_queued = false;
        if (terminal.stored && terminal.is_error &&
            terminal.error.code == IoError::Code::canceled) {
            bump(stats_, &AsyncStats::canceled_ops);
        } else if (terminal.stored && terminal.is_error) {
            bump(stats_, &AsyncStats::completion_errors);
        } else if (is_byte_op && static_cast<std::size_t>(res) < requested) {
            bump(stats_, &AsyncStats::short_completions);
        }
        return;
    }
    // record_terminal returned false despite a LIVE cookie match. The only
    // legitimate cause is a duplicate/late CQE for a slot that ALREADY has a
    // stored terminal (e.g. a cancel-won-before-dispatch slot whose original
    // SQE was never installed is impossible here since the cookie is LIVE —
    // so this is a genuine duplicate original CQE, or a real CQE racing a
    // cancel CQE that the kernel resolved as -ECANCELED first). In that case
    // retire the transport reference so it does not leak. Any OTHER state
    // (slot released/reused, or terminal_state fail-fast territory) is, per
    // frozen design §4, an invariant violation under a LIVE cookie — fail-fast
    // rather than silently leaking the router entry. Inspect the arena: if the
    // slot is still current-generation and already terminal, retire cleanly;
    // otherwise fail-fast.
    const bool already_terminal = arena_.terminal_stored(h.slot);
    if (already_terminal) {
        entry.in_use = false;
        entry.cookie = 0;
        cookie_free_list_.push_back(detail::SlotIndex{static_cast<std::uint32_t>(router_index)});
        live_cookies_.fetch_sub(1, std::memory_order_relaxed);
        return;
    }
    // LIVE cookie matched but the slot is not terminal and record_terminal
    // rejected — the generation moved (slot released/reused) while a LIVE
    // cookie pointed at it. That cannot happen under the dispatch/cancel lock
    // discipline (a LIVE cookie pins the slot until this CQE retires it).
    std::fprintf(stderr, "sluice::async::UringAsyncBackend: LIVE cookie matched "
                         "but record_terminal rejected a non-terminal slot "
                         "(invariant violation)\n");
    std::fflush(stderr);
    std::terminate();
}

std::size_t UringAsyncBackend::reap_cqes() noexcept {
    std::size_t recorded = 0;
    constexpr unsigned BATCH = 32;
    io_uring_cqe* cqes[BATCH];
    unsigned got = 0;
    while ((got = ::io_uring_peek_batch_cqe(&ring_state_->ring, cqes, BATCH)) > 0) {
        for (unsigned i = 0; i < got; ++i) {
            io_uring_cqe* cqe = cqes[i];
            const std::uint64_t user_data = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(io_uring_cqe_get_data(cqe)));
            const int res = cqe->res;
            ::io_uring_cqe_seen(&ring_state_->ring, cqe);
            // Snapshot whether this CQE is an operation (vs control) BEFORE
            // handle_one_cqe potentially retires the router entry.
            const bool is_op = (user_data != CONTROL_CANCEL && user_data != 0);
            handle_one_cqe(user_data, res);
            if (is_op)
                ++recorded;
        }
        if (got < BATCH)
            break;
    }
    return recorded;
}

// ---------------------------------------------------------------------------
// poll / wait_one — reap is the SOLE Completion-ready publication authority
// ---------------------------------------------------------------------------

std::size_t UringAsyncBackend::poll() {
    if (!have_ring_)
        return 0;
    // Re-dispatch enqueued requests toward ring ownership (best-effort; an
    // SQ-full request stays enqueued and retries next poll). Done under
    // dispatch_mtx_; dispatch_one_locked assumes the lock is held.
    if (!fatal_error_.has_value()) {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        // P0-A peek protocol: the dispatch queue owns local execution. We PEEK
        // the front, attempt the ownership transfer, and let the successful
        // transfer's remove_exact(h) retire the entry. On NULL SQE we BREAK
        // (ring-wide pressure: the next entry will not get an SQE either) and
        // leave h at the front for the next poll. We never pop_front→dispatch→
        // push_back: that would contradict the remove_exact(h) the successful
        // transfer performs. cancel()'s remove_exact + arena.cancel() shares
        // this same lock, so while we hold it the front cannot be canceled out
        // from under us.
        while (!dispatch_->empty()) {
            detail::SlotHandle h = dispatch_->front();
            if (!dispatch_one_locked(h))
                break; // SQ full (or fatal); h remains at front for next poll
        }
    }
    // Flush transport (SQEs prepared but not yet submitted) so the kernel can
    // complete them. TRANSPORT PROGRESS — does not change RequestState.
    if (!fatal_error_.has_value())
        (void)submit_transport();
    // Reap CQEs -> record_terminal ONLY. Then reap publishes Completion-ready
    // through the slot binding inside the leaf domain (ADR Decision 9 / I11).
    (void)reap_cqes();
    return arena_.reap(sink_);
}

Result<std::size_t> UringAsyncBackend::wait_one() {
    if (!have_ring_)
        return std::size_t{0};
    // First a non-blocking pass: flush transport, reap any already-ready CQEs,
    // and publish whatever is backend_ready. This resolves the common case
    // (work already complete) without a kernel wait.
    if (!fatal_error_.has_value()) {
        // Re-dispatch enqueued work toward ring ownership before flushing.
        // P0-A peek protocol (see poll() for the full rationale): peek the
        // front, attempt the transfer, break on NULL SQE. Never pop→dispatch→
        // push_back.
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        while (!dispatch_->empty()) {
            detail::SlotHandle h = dispatch_->front();
            if (!dispatch_one_locked(h))
                break; // SQ full (or fatal); h remains at front for next call
        }
    }
    if (!fatal_error_.has_value())
        (void)submit_transport();
    (void)reap_cqes();
    std::size_t n = arena_.reap(sink_);
    if (n > 0)
        return n;
    if (fatal_error_.has_value()) {
        // Do not block indefinitely after a permanent transport failure:
        // surface the stored backend error.
        return make_unexpected<std::size_t>(*fatal_error_);
    }
    if (arena_.accepted_outstanding() == 0)
        return std::size_t{0};

    // Nothing ready yet but work is outstanding: block in the KERNEL until at
    // least one CQE arrives (single-driver model — there is no separate worker
    // thread to signal a ReadyWaitSource). io_uring_submit_and_wait both
    // flushes pending SQEs and blocks for min_complete=1 CQE. On wake, reap.
    const int rc = ::io_uring_submit_and_wait(&ring_state_->ring, 1);
    if (rc < 0 && rc != -EINTR && rc != -EAGAIN) {
        // A genuine wait error (not a transient interrupt). Do not fabricate a
        // terminal; surface it so the caller can stop waiting.
        return make_unexpected<std::size_t>(sluice::from_errno_value(-rc));
    }
    (void)reap_cqes();
    return arena_.reap(sink_);
}

// ---------------------------------------------------------------------------
// cancel — Completion-keyed, drives the shared state machine (ADR Decision 11)
// ---------------------------------------------------------------------------

void UringAsyncBackend::issue_running_cancel(detail::SlotHandle h) noexcept {
    // The arena already recorded cancel intent (cancel_intent_). We only need
    // to append a best-effort AsyncCancel SQE if one has not already been
    // appended for this slot (frozen design §8.2: one fixed per-slot
    // cancel_queued bit, no unbounded cancel SQEs).
    std::uint64_t target_cookie = 0;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        if (fatal_error_.has_value())
            return;
        CancelScratch& scratch = cancel_scratch_[h.slot.value];
        if (scratch.cancel_queued)
            return;
        // RESOLVE THE TARGET BEFORE get_sqe (P0-C): find h's LIVE router entry
        // and read its kernel-visible cookie. If h is not currently
        // ring-owned (no LIVE entry), there is nothing to cancel — return
        // WITHOUT obtaining an SQE. This closes the get_sqe-then-discover-
        // nothing-to-cancel hole: io_uring_get_sqe() commits the SQE to the
        // next flush, so we must not obtain one we would then abandon.
        const std::size_t idx = find_live_router_index_(h);
        if (idx == router_.size())
            return; // not currently ring-owned; nothing to cancel
        target_cookie = router_[idx].cookie;
        if (target_cookie == 0 || target_cookie == CONTROL_CANCEL) {
            // Defensive: a LIVE entry must carry a valid operation cookie. A
            // zero/control cookie here is an invariant violation.
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: issue_running_cancel "
                                 "found LIVE router entry with invalid cookie "
                                 "(invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        // From here, under the same lock, the target's cookie cannot change
        // (only dispatch installs/retires router entries, and it takes this
        // lock). Obtain the SQE and fill it.
        io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_state_->ring);
        if (sqe == nullptr) {
            (void)submit_transport();
            if (fatal_error_.has_value())
                return;
            sqe = ::io_uring_get_sqe(&ring_state_->ring);
            if (sqe == nullptr)
                return; // SQ full; cancel retried on next poll (no SQE obtained)
        }
        // NO-FAIL REGION (P0-C): get_sqe committed the SQE. Fill it with the
        // resolved target cookie and the reserved control user_data.
        ::io_uring_prep_cancel64(sqe, target_cookie, /*flags=*/0);
        ::io_uring_sqe_set_data64(sqe, CONTROL_CANCEL);
        scratch.cancel_queued = true;
    }
}

void UringAsyncBackend::cancel(Completion<std::size_t>& c) {
    if (!have_ring_)
        return;
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value())
        return;
    detail::SlotHandle handle = *h;
    detail::CancelDisposition disp;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        // DISARM LOCAL EXECUTION FIRST: remove from the local dispatch ring if
        // present, so a pending/enqueued request cannot both be terminalized
        // locally and have its SQE installed (frozen design §8.1).
        (void)dispatch_->remove_exact(handle);
        // TERMINAL WIN SECOND: drive the shared state machine.
        disp = arena_.cancel(handle);
    }
    if (disp == detail::CancelDisposition::terminal_won) {
        // pending/enqueued cancel won: its SQE was NEVER installed and cannot
        // execute. Signal readiness so reap publishes the canceled terminal.
        bump(stats_, &AsyncStats::canceled_ops);
        signal_ready_progress();
    } else if (disp == detail::CancelDisposition::intent_recorded) {
        // running/ring-owned: intent only. Best-effort AsyncCancel; the slot
        // is retained to the original CQE (frozen design §8.2).
        issue_running_cancel(handle);
    }
    // already_terminal / not_found / not_supported: no-op.
}

void UringAsyncBackend::cancel(Completion<void>& c) {
    if (!have_ring_)
        return;
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value())
        return;
    detail::SlotHandle handle = *h;
    detail::CancelDisposition disp;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        (void)dispatch_->remove_exact(handle);
        disp = arena_.cancel(handle);
    }
    if (disp == detail::CancelDisposition::terminal_won) {
        bump(stats_, &AsyncStats::canceled_ops);
        signal_ready_progress();
    } else if (disp == detail::CancelDisposition::intent_recorded) {
        issue_running_cancel(handle);
    }
}

std::size_t UringAsyncBackend::outstanding() const noexcept {
    return arena_.accepted_outstanding();
}

bool UringAsyncBackend::available() const noexcept {
    return available_;
}

#endif // SLUICE_HAS_LIBURING

} // namespace sluice::async
