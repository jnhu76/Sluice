// Implementation of UringAsyncBackend.
//
// Migrated onto the bounded RequestArena / RequestSlot lifecycle
// with a PRIVATE io_uring ring per backend instance (ADR Decision 18 — Uring
// execution-ownership amendment). See
// docs/history/closeout/phase-d1-uring-frozen-design.md.
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
// appended IORING_OP_ASYNC_CANCEL produces a tagged control CQE that is
// informational for terminal selection. The original operation CQE decides;
// publication waits until both operation and control references retire.
#include <sluice/async/uring_backend.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "tax0_ablation_seams.hpp"  // non-installed seam header; internal-testing builds only
#endif

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
#include <thread>
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
// Stub: no request can ever be accepted, so no waiter can ever be registered.
Result<void> UringAsyncBackend::register_waiter(Completion<std::size_t>&,
                                                detail::WaiterToken,
                                                detail::RoutingLease) {
    return make_unexpected<void>(IoError{IoError::Code::not_supported});
}
Result<void> UringAsyncBackend::register_waiter(Completion<void>&,
                                                detail::WaiterToken,
                                                detail::RoutingLease) {
    return make_unexpected<void>(IoError{IoError::Code::not_supported});
}
Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter(
    Completion<std::size_t>&) {
    return make_unexpected<detail::RoutingLease>(
        IoError{IoError::Code::not_supported});
}
Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter(Completion<void>&) {
    return make_unexpected<detail::RoutingLease>(
        IoError{IoError::Code::not_supported});
}
void UringAsyncBackend::close_admission() {
    // Stub: admission never opened; nothing to close, no waiters to wake.
}
std::size_t UringAsyncBackend::outstanding() const noexcept {
    return 0;
}
bool UringAsyncBackend::available() const noexcept {
    return available_;
}

#else // SLUICE_HAS_LIBURING --------------------------------------------------

// ---------------------------------------------------------------------------
// Real io_uring path — private-ring / ring-owned RequestArena model.
//
// Identity: SQE.user_data = a 64-bit op_cookie (operation) or the
// high-bit-tagged target cookie (cancel). The op_cookie is allocated from a
// no-wrap 64-bit counter and is NEVER reused within backend lifetime. The
// CqeRouter is keyed by cookie VALUE: a CQE carries a cookie, we scan the
// fixed router for the LIVE entry whose cookie matches, recover the full
// SlotHandle{slot, full uint64 generation}, and the arena re-validates the
// generation. Because the cookie is never reused, a stale CQE (whose cookie
// was retired) matches no live entry and is dropped — the ABA window that
// existed when user_data carried router_slot+1 (which recycled) is closed.
//
// Dispatch: under ONE dispatch_mtx_ critical section, peek the
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
// CQE: the handler records an original terminal into RequestArena immediately,
// or defers it in fixed router scratch while a matching control is live. It
// never calls AsyncBackend::publish() directly — reap is the sole publisher.
//
// Threading: AsyncBackend is single-driver-thread (poll/wait_one/submit/cancel
// are serialized by AsyncIoContext::access_mtx_ at the context layer). The
// dispatch_mtx_ below guards the local dispatch ring, dispatch/cancel
// arbitration, router installation, and cancel-side router/scratch access.
// CQE lookup/retirement is serialized by AsyncIoContext::access_mtx_ under the
// D1 single-driver call contract and deliberately does not take dispatch_mtx_
// before arena.record_terminal().
// ---------------------------------------------------------------------------

// Opaque pimpl owning the private io_uring instance + the internal-testing
// transport hooks. Defined here (in the sluice::async namespace, matching the
// header's forward declaration) so the public header never includes
// <liburing.h>.
struct UringRingState {
    ::io_uring ring{};
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    UringBackendSubmitTestHooks test_hooks{};
#endif
};

namespace {

// Tagged control identity. Operation cookies occupy [1, 2^63-1]. An
// IORING_OP_ASYNC_CANCEL CQE carries CONTROL_TAG | target_operation_cookie, so
// the informational control reference can be retired against the exact bounded
// router entry even when the original operation CQE arrives first.
constexpr std::uint64_t CONTROL_TAG = std::uint64_t{1} << 63u;
constexpr std::uint64_t COOKIE_MASK = CONTROL_TAG - 1u;

constexpr bool is_control_cookie(std::uint64_t user_data) noexcept {
    return (user_data & CONTROL_TAG) != 0;
}

constexpr std::uint64_t make_control_cookie(std::uint64_t operation_cookie) noexcept {
    return CONTROL_TAG | operation_cookie;
}

constexpr std::uint64_t control_target_cookie(std::uint64_t user_data) noexcept {
    return user_data & COOKIE_MASK;
}

// Toggle a stat counter if a sink is attached.
inline void bump(sluice::AsyncStats* s, std::uint64_t sluice::AsyncStats::* field) {
    if (s)
        ++(s->*field);
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
    // invariant fail-fast, not would_block (AGENTS.md §3.5).
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
// Bounded physical transport ledger.
//
// The ledger contains exactly the SQEs obtained via get_sqe() but not yet
// confirmed consumed by a positive submit return. Its identity is a no-wrap
// logical sequence; the masked SQ position is storage location only. On poison
// the entries remain as quarantined teardown evidence and are individually
// marked recovery-retired after the separate Class-A proof controller acts.
// ---------------------------------------------------------------------------

class UringAsyncBackend::TransportLedger {
  public:
    enum class Kind : std::uint8_t { operation, cancel_control };

    struct Entry {
        std::uint64_t sequence = 0;
        std::uint32_t physical_position = 0;
        Kind kind = Kind::operation;
        std::uint64_t cookie = 0;
        detail::SlotHandle handle{};
        bool class_a_recovery_retired = false;
    };

    explicit TransportLedger(std::size_t capacity) : storage_(capacity), capacity_(capacity) {
        if (capacity_ == 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: zero-capacity transport "
                                 "ledger (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
    }

    bool empty() const noexcept { return size_ == 0; }
    std::size_t size() const noexcept { return size_; }

    void append(Kind kind, std::uint32_t physical_position, std::uint64_t cookie,
                detail::SlotHandle handle) noexcept {
        const std::uint32_t expected_physical =
            last_sequence_ == 0
                ? 0
                : static_cast<std::uint32_t>(
                      (static_cast<std::uint64_t>(last_physical_position_) + 1u) % capacity_);
        if (size_ >= capacity_ || physical_position >= capacity_ || next_sequence_ == 0 ||
            next_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
            physical_position != expected_physical || next_sequence_ != last_sequence_ + 1u) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: transport ledger "
                                 "overflow/non-monotonic physical sequence/sequence exhaustion "
                                 "(invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        storage_[(head_ + size_) % capacity_] =
            Entry{next_sequence_++, physical_position, kind, cookie, handle, false};
        last_sequence_ = next_sequence_ - 1u;
        last_physical_position_ = physical_position;
        ++size_;
    }

    Entry pop_front() noexcept {
        if (size_ == 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: transport ledger "
                                 "underflow (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        Entry out = storage_[head_];
        if (out.sequence != retired_prefix_sequence_ + 1u) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: transport ledger retired a "
                                 "non-monotonic logical prefix (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        retired_prefix_sequence_ = out.sequence;
        head_ = (head_ + 1) % capacity_;
        --size_;
        return out;
    }

    Entry& at(std::size_t offset) noexcept {
        if (offset >= size_) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: transport ledger index "
                                 "out of range (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        return storage_[(head_ + offset) % capacity_];
    }

    bool all_class_a_recovery_retired() const noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            if (!storage_[(head_ + i) % capacity_].class_a_recovery_retired)
                return false;
        }
        return true;
    }

  private:
    std::vector<Entry> storage_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t retired_prefix_sequence_ = 0;
    std::uint32_t last_physical_position_ = 0;
};

// ---------------------------------------------------------------------------
// Descriptor validation (ADR Decision 6; AGENTS.md §3.8). A REAL syscall
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
    // liburing's io_uring_prep_read/write take `unsigned nbytes`. Validate
    // against UINT_MAX (not SSIZE_MAX) so a >4GiB length is rejected here
    // instead of being silently truncated by the implicit size_t->unsigned
    // narrowing at SQE fill time. The shared checked_uring_length helper uses
    // a different vocabulary (invalid_state); map to the Uring public
    // validation vocabulary (invalid_argument) at this boundary.
    auto nlen = sluice::detail::checked_uring_length(op.len);
    if (!nlen.has_value()) {
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
    auto nlen = sluice::detail::checked_uring_length(op.len);
    if (!nlen.has_value()) {
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
    : UringAsyncBackend(validate_config_(config), ValidatedConfigTag{}) {}

UringConfig UringAsyncBackend::validate_config_(UringConfig config) {
    constexpr std::size_t slot_index_max =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    if (config.request_capacity == 0 || config.request_capacity > slot_index_max ||
        config.queue_depth == 0) {
        throw std::invalid_argument(
            "UringConfig request_capacity must be in [1, UINT32_MAX] and queue_depth must be > 0");
    }
    return config;
}

UringAsyncBackend::UringAsyncBackend(UringConfig config, ValidatedConfigTag)
    : arena_(detail::ContextIdentity::for_testing(next_backend_id()), config.request_capacity),
      prepared_ops_(config.request_capacity), router_(config.request_capacity),
      cancel_scratch_(config.request_capacity), cookie_free_list_(config.request_capacity),
      queue_depth_(config.queue_depth), ring_state_(std::make_unique<UringRingState>()) {
    // Seed the router ARRAY-slot free-list: every router slot is initially
    // free. The free-list recycles router ARRAY slots only (the bounded
    // router); it NEVER recycles cookie VALUES. Cookie uniqueness comes from
    // allocate_cookie_()'s no-wrap counter, which fail-fasts before ever
    // reaching the reserved high-bit control range (mirrors RequestArena
    // generation no-wrap discipline). next_cookie_ starts at 1 (0 is unused).
    for (std::uint32_t i = 0; i < config.request_capacity; ++i) {
        cookie_free_list_[i] = detail::SlotIndex{i};
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // TAX-0 router-fix shootout (#255): the R3 candidate's fixed cookie
    // table is sized and allocated ONCE here (construction-time bounded
    // memory; zero steady-state allocation). Internal-testing builds only;
    // production objects keep the pre-seam layout.
    cookie_table_for_test_ =
        std::make_unique<RouterCookieTableForTest>(config.request_capacity);
    // RE-H0 ATTR-B F07 seam: cache the construction-invariant router
    // extent once (router_ is never resized; see router_extent_()).
    router_extent_cached_for_test_ = router_.size();
#endif
    dispatch_ = std::make_unique<BoundedDispatchQueue>(config.request_capacity);
    if (::io_uring_queue_init(config.queue_depth, &ring_state_->ring, /*flags=*/0) == 0) {
        // The physical ledger is sized from the ACTUAL SQ capacity returned by
        // ring setup. Linux may round queue_depth up (e.g. 3 -> 4, 65 -> 128).
        // If this construction-time allocation fails, release the already-
        // initialized ring before propagating the exception.
        try {
            transport_ledger_ =
                std::make_unique<TransportLedger>(ring_state_->ring.sq.ring_entries);
            wait_source_ = std::make_unique<detail::UringWaitSource>();
            wait_source_->set_ring_fd(ring_state_->ring.ring_fd);
        } catch (...) {
            ::io_uring_queue_exit(&ring_state_->ring);
            throw;
        }
        have_ring_ = true;
        available_ = true;
    }
    // Construction is allowed to fail (e.g. kernel without io_uring); the
    // instance is then constructible-but-unavailable, mirroring the stub's
    // available()==false contract. submit_* will reject synchronously.
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
UringAsyncBackend::UringAsyncBackend(UringConfig config, UringBackendSubmitTestHooks hooks)
    : UringAsyncBackend(config) {
    ring_state_->test_hooks = hooks;
}
#endif

UringAsyncBackend::~UringAsyncBackend() {
    // Quiescent preflight (AGENTS.md §3.7; mirrors ThreadPoolBackend). Fail-fast
    // BEFORE io_uring_queue_exit() so a non-quiescent destroy is reported as a
    // contract violation, not masked by ring teardown. This is quiescent
    // teardown only — it does NOT cancel/drain/wait/reap. The preflight order is
    // backend quiescence → ring teardown eligibility (matching ThreadPool: full
    // quiescence check, then actual teardown), and dispatch_mtx_ → arena leaf is
    // the existing frozen lock order, so this preflight is lock-order legal.
    // live_cookies_ != 0 is structurally implied by slot_in_use != 0 (a ring-
    // owned request keeps its slot bound), but the redundant check improves
    // corruption detection.
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        const auto q = arena_.quiescence_snapshot();
        const bool ledger_quiescent =
            transport_ledger_ == nullptr || transport_ledger_->empty() ||
            (fatal_error_.has_value() && transport_ledger_->all_class_a_recovery_retired());
        if (!dispatch_->empty() || live_cookies_.load(std::memory_order_relaxed) != 0 ||
            live_control_sqes_.load(std::memory_order_relaxed) != 0 || !ledger_quiescent ||
            q.slot_in_use != 0 || q.accepted_outstanding != 0 || q.backend_ready != 0) {
            detail::uring_non_quiescent_destruction_fail_fast();
        }
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic destructor-order probe (D4-RM11 detector): reached ONLY
    // when the preflight above PASSED (a non-quiescent destroy terminated
    // before this point). A death child installs a fn that _Exit(90) so a
    // mutant that removes/bypasses the preflight is caught at this teardown
    // boundary instead of being masked by another fail-fast authority. The
    // hook is allocation-free (raw fn pointer + ctx) and production behavior
    // is unchanged when no fn is installed. It does NOT alter queue_exit
    // semantics.
    if (auto* fn = before_queue_exit_fn_.load(std::memory_order_acquire)) {
        fn(before_queue_exit_ctx_.load(std::memory_order_acquire));
    }
#endif
    if (have_ring_) {
        ::io_uring_queue_exit(&ring_state_->ring);
        have_ring_ = false;
    }
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
std::size_t UringAsyncBackend::dispatch_size_for_test() const noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    return dispatch_->size();
}

std::size_t UringAsyncBackend::transport_ledger_size_for_test() const noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    return transport_ledger_ == nullptr ? 0 : transport_ledger_->size();
}

std::size_t UringAsyncBackend::sq_ready_for_test() const noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    return have_ring_ ? static_cast<std::size_t>(::io_uring_sq_ready(&ring_state_->ring)) : 0;
}

std::size_t UringAsyncBackend::live_control_entries_for_test() const noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    std::size_t live = 0;
    for (const RouterEntry& entry : router_) {
        if (entry.in_use && entry.control_state != RouterEntry::ControlState::none)
            ++live;
    }
    return live;
}
#endif

// ---------------------------------------------------------------------------
// Five-stage admission (ADR Decision 5; mirrors ThreadPoolBackend).
// Stages 1–3c run under the dispatch admission path; the Completion
// `binding -> outstanding` release-store is the commit/accept linearization
// point. Capacity full -> would_block; admission closed -> invalid_state;
// malformed descriptor -> invalid_argument (Stage 1.5, after reserve).
// ---------------------------------------------------------------------------

template <class Op>
Result<void> UringAsyncBackend::submit_size(Op op, Completion<std::size_t>& c,
                                            detail::OperationKind kind) {
    // Ring availability, poison, and admission-closed are all checked in
    // the policy's stage0_precheck hook — the single Stage-0 authority
    // under dispatch_mtx_ (accepted #157 review; ring -> poison ->
    // admission hook-internal order).
    detail::SlotHandle h{};
    {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        // D4 C2e (close-wins window): the submit pauses BEFORE taking the
        // admission transaction lock — see submit_size in
        // threadpool_backend.cpp (same seam, same discipline). Compiled out
        // of production builds.
        wait_before_admission_lock_pause_();
#endif
        // Admission transaction domain: dispatch_mtx_ (one lock for
        // admission + dispatch on this backend). The shared ladder runs
        // under it; its first step is the policy's in-lock Stage-0
        // ring/poison/admission gate (precedence preserved verbatim).
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        SubmitPolicy<Op, Completion<std::size_t>> policy{*this, kind};
        auto r = detail::submit_transaction(arena_, c, op, policy);
        if (!r.has_value()) {
            return make_unexpected<void>(r.error());
        }
        h = r.value();
    } // dispatch_mtx_ released BEFORE enqueue (no-fail, needs no serialization).

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // D3 C2b row 5 (Scheme-B pending cancel): deterministic pause between the
    // commit/accept linearization point (Completion outstanding, slot pending,
    // dispatch_mtx_ FREE) and enqueue. A test drives cancel() in this exact
    // window and proves it wins the canceled terminal with no SQE / no router
    // cookie / no transport ledger entry / no syscall. Compiled out of
    // production builds.
    wait_after_commit_before_enqueue_pause_();
#endif

    // Stage 4: enqueue + dispatch attempt under one dispatch_mtx_ critical
    // section (frozen design §4.2).
    enqueue_after_commit(h);
    return {};
}

template <class Op>
Result<void> UringAsyncBackend::submit_void(Op op, Completion<void>& c,
                                            detail::OperationKind kind) {
    // Stage-0 authority (ring/poison/admission) lives in the policy hook
    // — see submit_size.
    detail::SlotHandle h{};
    {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        wait_before_admission_lock_pause_();
#endif
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        SubmitPolicy<Op, Completion<void>> policy{*this, kind};
        auto r = detail::submit_transaction(arena_, c, op, policy);
        if (!r.has_value()) {
            return make_unexpected<void>(r.error());
        }
        h = r.value();
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    wait_after_commit_before_enqueue_pause_();
#endif

    enqueue_after_commit(h);
    return {};
}

Result<void> UringAsyncBackend::submit_read(ReadOp op, Completion<std::size_t>& c) {
    return submit_size(op, c, detail::OperationKind::read);
}
Result<void> UringAsyncBackend::submit_write(WriteOp op, Completion<std::size_t>& c) {
    return submit_size(op, c, detail::OperationKind::write);
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
    bool newly_poisoned = false; // set iff THIS call poisons the backend
    detail::EnqueueOutcome outcome;
    {
        std::unique_lock<std::mutex> lk(dispatch_mtx_);
        // fatal_error_ is dispatch_mtx_-authority: read the before/after pair
        // ONLY inside the lock (no unlocked reads, the same
        // discipline as the Stage-0 admission check).
        const bool poisoned_before = fatal_error_.has_value();
        outcome = arena_.enqueue(h); // pending -> enqueued OR terminal_noop
        if (outcome == detail::EnqueueOutcome::enqueued) {
            dispatch_->push_back(h);
            // Peek protocol: enqueue (push_back) and the best-effort
            // FIFO ownership transfers run under ONE dispatch_mtx_ critical
            // section. Always dispatch the current front: h is the newly
            // appended tail and an older request may already be queued after
            // SQ pressure. This is the same front/peek/remove_exact protocol
            // used by poll()/wait_one(). Holding the lock across push_back and
            // the drain also prevents cancel() from interposing before
            // mark_running. On SQ pressure, the current front stays queued and
            // the loop stops; no tail may bypass it.
            for (;;) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                if (before_dispatch_transfer_gate_.load(std::memory_order_acquire) !=
                    nullptr) {
                    // D3 C2b row 6 (enqueued cancel): Gate-B analogue — release
                    // dispatch_mtx_ while paused so a test can drive cancel()
                    // (remove_exact + arena.cancel) against the enqueued front.
                    // The front is RE-READ after resume, so a cancel-won
                    // request (removed from the queue) is never dispatched.
                    lk.unlock();
                    wait_before_dispatch_transfer_pause_();
                    lk.lock();
                }
#endif
                if (dispatch_->empty())
                    break;
                const detail::SlotHandle front = dispatch_->front();
                if (!dispatch_one_locked(front))
                    break;
            }
        }
        // terminal_noop: a pending cancel won first (Scheme B). No dispatch
        // linkage; that winner owns readiness.
        // A permanent transport failure during the
        // enqueue-dispatch loop retired the local queue / Class-A ledger to
        // backend-ready terminals under this lock. The before/after poison
        // pair is snapshotted HERE (inside dispatch_mtx_); the wake itself is
        // deferred until the lock is released below (state first, then wake).
        newly_poisoned = !poisoned_before && fatal_error_.has_value();
    }
    if (outcome != detail::EnqueueOutcome::enqueued) {
        // terminal_noop: re-arm the ready condition so the wake is not lost
        // (ADR Decision 4 / I19). Done OUTSIDE the dispatch lock — it is a
        // no-op seam in the single-driver model.
        signal_ready_progress();
    }
    // A permanent transport failure during this enqueue-
    // dispatch retired the local queue / Class-A ledger to backend-ready
    // terminals. No reap runs on this path (the caller is a submit, not a
    // poll), so the split-phase waiters must be woken HERE — but only after
    // dispatch_mtx_ is released (state first, then wake; the wait-source
    // mutex is a leaf never acquired under dispatch_mtx_ — frozen lock
    // order). fatal_error_ was snapshotted under the lock above, so
    // the before/after compare is exact.
    if (newly_poisoned) {
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

// The ownership-transfer transaction (frozen design §4).
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
        (void)submit_transport_locked(); // transport only; no RequestState change
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
        ::io_uring_prep_read(sqe, prep.fd, const_cast<std::byte*>(prep.buffer), prep.native_length,
                             static_cast<off_t>(static_cast<std::int64_t>(prep.offset)));
        break;
    case detail::OperationKind::write:
        ::io_uring_prep_write(sqe, prep.fd, prep.buffer, prep.native_length,
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
    // recycled router ARRAY slot (the ABA fix).
    ::io_uring_sqe_set_data64(sqe, op_cookie);
    RouterEntry& route = router_[router_slot.value];
    route = RouterEntry{};
    route.cookie = op_cookie;
    route.handle = h;
    route.in_use = true;
    live_cookies_.fetch_add(1, std::memory_order_relaxed);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // TAX-0 router-fix shootout (#255): R3 candidate maintains its derived
    // cookie table exactly on the production install path. No-op for every
    // other mode; compiled out of production builds.
    router_table_insert_(op_cookie, router_slot.value);
#endif
    const auto& sq = ring_state_->ring.sq;
    const std::uint32_t physical_position =
        static_cast<std::uint32_t>((sq.sqe_tail - 1u) & sq.ring_mask);
    transport_ledger_->append(TransportLedger::Kind::operation, physical_position, op_cookie, h);

    // mark_running: enqueued -> running (the ownership transfer). Under the
    // lock discipline (enqueue + dispatch share ONE dispatch_mtx_ critical
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
    // Unlink from the local dispatch ring. Peek protocol: h was the front
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
// Transport progress and ledger recovery. submit itself remains transport-only;
// the separate recovery controller consumes the zero-consumption proof.
// ---------------------------------------------------------------------------

int UringAsyncBackend::submit_transport_locked() noexcept {
    if (fatal_error_.has_value() || transport_ledger_ == nullptr || transport_ledger_->empty())
        return 0;

    submit_flushes_.fetch_add(1, std::memory_order_relaxed);
    int rc = 0;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (ring_state_->test_hooks.submit != nullptr) {
        rc = ring_state_->test_hooks.submit(ring_state_->test_hooks.context, &ring_state_->ring);
    } else {
        rc = ::io_uring_submit(&ring_state_->ring);
    }
#else
    rc = ::io_uring_submit(&ring_state_->ring);
#endif
    account_transport_result_locked(rc, /*had_pending_transport=*/true);
    return rc;
}

void UringAsyncBackend::account_transport_result_locked(int rc,
                                                        bool had_pending_transport) noexcept {
    if (!had_pending_transport)
        return; // a to_submit=0 wait result is not submission evidence

    if (rc > 0) {
        const std::size_t consumed = static_cast<std::size_t>(rc);
        if (consumed > transport_ledger_->size()) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: submit consumed more SQEs "
                                 "than the physical ledger contains (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        for (std::size_t i = 0; i < consumed; ++i) {
            const TransportLedger::Entry entry = transport_ledger_->pop_front();
            if (entry.kind == TransportLedger::Kind::cancel_control) {
                const std::size_t router_index = find_live_router_cookie_(entry.cookie);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                fold_router_lookup_diag_for_test(RouterLookupKindForTest::transport);
#endif
                if (router_index == router_.size() ||
                    router_[router_index].handle.slot.value != entry.handle.slot.value ||
                    router_[router_index].handle.generation.value !=
                        entry.handle.generation.value ||
                    router_[router_index].control_state != RouterEntry::ControlState::prepared) {
                    std::fprintf(stderr, "sluice::async::UringAsyncBackend: consumed control "
                                         "lost its exact prepared router reference "
                                         "(invariant violation)\n");
                    std::fflush(stderr);
                    std::terminate();
                }
                router_[router_index].control_state = RouterEntry::ControlState::submitted;
                live_control_sqes_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }
    if (rc == 0)
        return;

    const int err = -rc;
    if (err == EINTR || err == EAGAIN || err == EBUSY)
        return; // transient transport evidence; retry without lifecycle mutation

    // The syscall is still not terminal authority. The separate controller
    // below consumes the Linux-6.1/liburing-2.14 theorem: permanent negative
    // return + the frozen-design topology => zero entries from this ledger were
    // consumed, hence every retained entry is execution-impossible Class-A.
    poison_and_recover_locked(IoError{IoError::Code::backend_error, err});
}

void UringAsyncBackend::poison_and_recover_locked(IoError error) noexcept {
    if (fatal_error_.has_value())
        return;
    fatal_error_ = error;
    admission_closed_ = true;

    for (std::size_t i = 0; i < transport_ledger_->size(); ++i) {
        TransportLedger::Entry& physical = transport_ledger_->at(i);
        if (physical.class_a_recovery_retired) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: duplicate Class-A "
                                 "recovery retirement (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }

        if (physical.kind == TransportLedger::Kind::operation) {
            const std::size_t router_index = find_live_router_cookie_(physical.cookie);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            fold_router_lookup_diag_for_test(RouterLookupKindForTest::transport);
#endif
            if (router_index == router_.size() ||
                router_[router_index].handle.slot.value != physical.handle.slot.value ||
                router_[router_index].handle.generation.value != physical.handle.generation.value) {
                std::fprintf(stderr, "sluice::async::UringAsyncBackend: Class-A operation "
                                     "recovery lost identity "
                                     "(invariant violation)\n");
                std::fflush(stderr);
                std::terminate();
            }
            RouterEntry& route = router_[router_index];
            const detail::TerminalResult terminal = detail::TerminalResult::err(error);
            if (route.control_state == RouterEntry::ControlState::none) {
                finalize_operation_terminal_(router_index, terminal);
            } else {
                // A later control entry in this exact Class-A batch must retire
                // before the logical operation becomes reap-eligible.
                route.deferred_terminal = terminal;
                route.deferred_terminal_stored = true;
            }
        } else {
            const std::size_t router_index = find_live_router_cookie_(physical.cookie);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            fold_router_lookup_diag_for_test(RouterLookupKindForTest::transport);
#endif
            if (router_index == router_.size() ||
                router_[router_index].handle.slot.value != physical.handle.slot.value ||
                router_[router_index].handle.generation.value != physical.handle.generation.value ||
                router_[router_index].control_state != RouterEntry::ControlState::prepared) {
                std::fprintf(stderr, "sluice::async::UringAsyncBackend: Class-A control "
                                     "recovery lost its exact prepared router reference "
                                     "(invariant violation)\n");
                std::fflush(stderr);
                std::terminate();
            }
            RouterEntry& route = router_[router_index];
            route.control_state = RouterEntry::ControlState::none;
            cancel_scratch_[physical.handle.slot.value].cancel_queued = false;
            if (route.deferred_terminal_stored)
                finalize_operation_terminal_(router_index, route.deferred_terminal);
        }
        physical.class_a_recovery_retired = true;
    }

    // The local queue is exactly the never-dispatched set under the held
    // front/peek/remove dispatch lock. It has no SQE or kernel identity.
    detail::SlotHandle local{};
    while (dispatch_->pop_front(local)) {
        if (!arena_.record_terminal(local, detail::TerminalResult::err(error))) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: local poison retirement "
                                 "lost terminal authority (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        cancel_scratch_[local.slot.value].cancel_queued = false;
        bump(stats_, &AsyncStats::completion_errors);
    }
    // NOTE: NO wake here. poison_and_recover_locked always
    // runs under dispatch_mtx_, and the wait-source mutex is a LEAF domain
    // that must never be acquired while holding dispatch_mtx_ (frozen lock
    // order: access_mtx_ -> dispatch_mtx_ -> arena leaf -> wait-source mtx_).
    // The retired entries are backend-ready terminals that the next
    // poll()/wait_one() reap publishes (its n>0 path signals); the two paths
    // with NO following reap — enqueue_after_commit and issue_running_cancel
    // — defer the wake past their own dispatch_mtx_ release (state first,
    // then wake).
}

// ---------------------------------------------------------------------------
// Cookie allocator. The operation-cookie domain is [1, 2^63-1]; 0 is
// unused and the high bit is reserved for tagged control identities. The
// counter never enters the control range, mirroring RequestArena generation
// no-wrap discipline. Because the cookie is never reused, a stale
// CQE's cookie cannot match a later LIVE router entry.
// ---------------------------------------------------------------------------
std::uint64_t UringAsyncBackend::allocate_cookie_() noexcept {
    // The counter is mutated only under dispatch_mtx_ (dispatch_one_locked /
    // the test seam). If next_cookie_ has reached the tagged control range,
    // the operation-cookie domain is exhausted — fail-fast rather than enter
    // the tagged-control range or wrap to 0 (which would reuse cookie 1 and
    // reopen the ABA window the cookie-keyed router exists to close).
    if (next_cookie_ == 0 || next_cookie_ >= CONTROL_TAG) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: operation-cookie "
                             "domain exhausted (would enter tagged control range / "
                             "wrap; invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    return next_cookie_++;
}

// RE-H0 ATTR-B F07 research seam accessor (internal-testing builds only;
// the declaration + cache member live under the guard in the public
// header). R0 (flag off): exactly router_.size(). R1: the construction-
// cached extent — identical by the never-resized invariant. The mode flag
// is checked per call, so the R1 arm conservatively pays the branch. The
// production build never compiles this definition and keeps its original
// router_.size() text at every site (bit-identical production objects).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
std::size_t UringAsyncBackend::router_extent_() const noexcept {
    return detail::tax0_f07_skip_extent_reprobes()
               ? router_extent_cached_for_test_
               : router_.size();
}
#endif

// Find the router ARRAY index of the LIVE entry whose SlotHandle exactly
// matches h. Returns router_.size() when no live entry matches (h is not
// currently ring-owned). Bounded O(request_capacity), allocation-free.
// (Cancel-path lookup: not part of the ATTR-B treated per-op surface —
// the measured workload never exercises it.)
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

std::size_t UringAsyncBackend::find_live_router_cookie_(std::uint64_t cookie) const noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // TAX-0 research instrumentation (#250 U0 witness + #255 router-fix
    // shootout): the IDENTICAL matching predicate (in_use && cookie
    // equality) with a selectable scan direction / fix candidate and exact
    // per-call accounting. Live cookies are unique within backend lifetime
    // (no-wrap allocate_cookie_), so NO candidate can change the semantic
    // answer — at most one index matches, or the cookie is stale/unknown
    // and every candidate reports the same not-found. production_baseline
    // tracks the SHIPPED production scan, which is REVERSE since the R1
    // production landing (#255); forward_ablation is the pre-fix forward
    // traversal, kept as the causal-comparator direction. The production
    // build in the #else below carries the same reverse direction.
    std::size_t examined = 0;
    // F07 ATTR-B seam extent: identical to router_.size() in R0; the
    // construction-cached value under the R1 flag (never-resized
    // invariant). Production builds compile only the #else branch below.
    const std::size_t extent = router_extent_();
    std::size_t found = extent;
    const bool reverse =
        router_fix_mode_for_test_ == RouterFixModeForTest::reverse_scan ||
        (router_fix_mode_for_test_ ==
             RouterFixModeForTest::production_baseline &&
         router_scan_mode_for_test_ !=
             RouterScanModeForTest::forward_ablation);
    if (router_fix_mode_for_test_ ==
            RouterFixModeForTest::bounded_cookie_table &&
        cookie_table_for_test_ != nullptr) {
        // R3: fixed-table resolution. Same miss contract (router_.size()).
        // A hit MUST name the live entry carrying exactly this cookie — a
        // table/router desync is an invariant violation, never a fallback.
        const std::size_t idx = cookie_table_for_test_->lookup(cookie);
        examined = static_cast<std::size_t>(cookie_table_for_test_->last_probes);
        if (idx != RouterCookieTableForTest::kMiss) {
            if (idx >= extent || !router_[idx].in_use ||
                router_[idx].cookie != cookie) {
                std::fprintf(stderr,
                             "sluice::async::UringAsyncBackend: router cookie "
                             "table resolved a stale/non-matching router entry "
                             "(invariant violation)\n");
                std::fflush(stderr);
                std::terminate();
            }
            found = idx;
        }
    } else if (reverse) {
        for (std::size_t i = extent; i-- > 0;) {
            ++examined;
            if (router_[i].in_use && router_[i].cookie == cookie) {
                found = i;
                break;
            }
        }
    } else {
        for (std::size_t i = 0; i < extent; ++i) {
            ++examined;
            if (router_[i].in_use && router_[i].cookie == cookie) {
                found = i;
                break;
            }
        }
    }
    RouterScanDiagnosticsForTest& diag = router_diag_for_test_;
    diag.last_call_iterations = examined;
    diag.lookup_calls += 1;
    if (reverse)
        diag.reverse_mode_calls += 1;
    if (found != extent) {
        diag.lookup_hits += 1;
        diag.matched_router_index_sum += found;
        if (found > diag.matched_router_index_max)
            diag.matched_router_index_max = found;
    } else {
        diag.lookup_misses += 1;
    }
    if (router_fix_mode_for_test_ ==
        RouterFixModeForTest::bounded_cookie_table) {
        diag.table_lookup_probes_total += examined;
        if (examined > diag.table_lookup_probes_max)
            diag.table_lookup_probes_max = examined;
    }
    return found;
#else
    // Shipped scan direction: REVERSE (T0-U-ROUTER / #255 — the R1 candidate
    // selected by the #256 shootout). Live cookies are unique within backend
    // lifetime (no-wrap allocate_cookie_ above), so at most one entry matches
    // and the traversal order cannot change the semantic answer. High-index
    // LIFO placement concentrates the steady live set near the top in the
    // measured fixed-depth regime, so reverse traversal removes the measured
    // capacity-dependent lookup tax in that regime. Worst-case lookup remains
    // O(request_capacity).
    for (std::size_t i = router_.size(); i-- > 0;) {
        if (router_[i].in_use && router_[i].cookie == cookie)
            return i;
    }
    return router_.size();
#endif
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
// TAX-0 router-fix shootout (#255): R3 table maintenance on the PRODUCTION
// install/retire paths (guarded; no-ops for every other mode so the same
// production functions serve all candidates). The table is derived
// transport metadata: insert exactly on router install, erase exactly on
// router retirement; any impossible state (duplicate insert, missing erase,
// probe overrun) fail-fasts inside the table.
void UringAsyncBackend::router_table_insert_(std::uint64_t cookie,
                                             std::size_t router_index) noexcept {
    if (router_fix_mode_for_test_ != RouterFixModeForTest::bounded_cookie_table ||
        cookie_table_for_test_ == nullptr)
        return;
    cookie_table_for_test_->insert(cookie, router_index);
    fold_router_table_probes_for_test_('i', cookie_table_for_test_->last_probes);
}

void UringAsyncBackend::router_table_erase_(std::uint64_t cookie) noexcept {
    if (router_fix_mode_for_test_ != RouterFixModeForTest::bounded_cookie_table ||
        cookie_table_for_test_ == nullptr)
        return;
    cookie_table_for_test_->erase(cookie);
    fold_router_table_probes_for_test_('e', cookie_table_for_test_->last_probes);
}

void UringAsyncBackend::fold_router_table_probes_for_test_(
    char which, std::uint64_t probes) const noexcept {
    RouterScanDiagnosticsForTest& diag = router_diag_for_test_;
    switch (which) {
    case 'i':
        diag.table_insert_calls += 1;
        diag.table_insert_probes_total += probes;
        if (probes > diag.table_insert_probes_max)
            diag.table_insert_probes_max = probes;
        break;
    case 'e':
        diag.table_erase_calls += 1;
        diag.table_erase_probes_total += probes;
        if (probes > diag.table_erase_probes_max)
            diag.table_erase_probes_max = probes;
        break;
    default:
        break;
    }
}
#endif

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
// Attribute the just-completed find_live_router_cookie_ call (its
// last_call_iterations) to one callsite family. Called ONLY from
// single-driver contexts, immediately after the lookup, before any other
// lookup can run.
void UringAsyncBackend::fold_router_lookup_diag_for_test(
    RouterLookupKindForTest kind) const noexcept {
    RouterScanDiagnosticsForTest& diag = router_diag_for_test_;
    const std::uint64_t it = diag.last_call_iterations;
    switch (kind) {
    case RouterLookupKindForTest::operation_cqe:
        diag.operation_cookie_lookup_calls += 1;
        diag.operation_lookup_iterations_total += it;
        if (it > diag.operation_lookup_iterations_max)
            diag.operation_lookup_iterations_max = it;
        break;
    case RouterLookupKindForTest::control_cqe:
        diag.control_cookie_lookup_calls += 1;
        diag.control_lookup_iterations_total += it;
        if (it > diag.control_lookup_iterations_max)
            diag.control_lookup_iterations_max = it;
        break;
    case RouterLookupKindForTest::transport:
        diag.transport_cookie_lookup_calls += 1;
        diag.transport_lookup_iterations_total += it;
        if (it > diag.transport_lookup_iterations_max)
            diag.transport_lookup_iterations_max = it;
        break;
    }
}
#endif

void UringAsyncBackend::retire_router_entry_(std::size_t router_index) noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (router_index >= router_extent_() || !router_[router_index].in_use) {
#else
    if (router_index >= router_.size() || !router_[router_index].in_use) {
#endif
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: invalid router retirement "
                             "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    RouterEntry& entry = router_[router_index];
    if (entry.control_state != RouterEntry::ControlState::none) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: router retired with a live "
                             "control reference (invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // TAX-0 router-fix shootout (#255): R3 erases exactly on retirement,
    // before the entry (and its cookie) is cleared. No-op for other modes;
    // an absent-cookie erase is an impossible state and fail-fasts.
    router_table_erase_(entry.cookie);
#endif
    entry = RouterEntry{};
    cookie_free_list_.push_back(detail::SlotIndex{static_cast<std::uint32_t>(router_index)});
    live_cookies_.fetch_sub(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// CQE reap. Route op cookies to full SlotHandles, validate generation, and
// record_terminal only after all matching backend control references retire
// (never publish directly). Returns the count of non-control CQEs observed.
// ---------------------------------------------------------------------------

void UringAsyncBackend::finalize_operation_terminal_(
    std::size_t router_index, const detail::TerminalResult& terminal) noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (router_index >= router_extent_() || !router_[router_index].in_use ||
        router_[router_index].control_state != RouterEntry::ControlState::none) {
#else
    if (router_index >= router_.size() || !router_[router_index].in_use ||
        router_[router_index].control_state != RouterEntry::ControlState::none) {
#endif
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: invalid operation terminal "
                             "finalization (invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    const detail::SlotHandle h = router_[router_index].handle;
    if (!arena_.record_terminal(h, terminal)) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: operation terminal lost "
                             "RequestArena winner authority (invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    const PreparedUringOp& prep = prepared_ops_[h.slot.value];
    const bool is_byte_op =
        prep.kind == detail::OperationKind::read || prep.kind == detail::OperationKind::write;
    if (terminal.stored && terminal.is_error && terminal.error.code == IoError::Code::canceled) {
        bump(stats_, &AsyncStats::canceled_ops);
    } else if (terminal.stored && terminal.is_error) {
        bump(stats_, &AsyncStats::completion_errors);
    } else if (is_byte_op && terminal.bytes < prep.length) {
        bump(stats_, &AsyncStats::short_completions);
    }

    cancel_scratch_[h.slot.value].cancel_queued = false;
    retire_router_entry_(router_index);
}

void UringAsyncBackend::handle_one_cqe(std::uint64_t user_data, int res) noexcept {
    // Tagged control CQE: informational with respect to terminal selection, but
    // authoritative for retirement of one exact backend execution reference.
    if (is_control_cookie(user_data)) {
        const std::uint64_t target_cookie = control_target_cookie(user_data);
        if (target_cookie == 0)
            return; // never emitted; harmless unknown tagged control value
        const std::size_t router_index = find_live_router_cookie_(target_cookie);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        fold_router_lookup_diag_for_test(RouterLookupKindForTest::control_cqe);
        if (router_index == router_extent_())
#else
        if (router_index == router_.size())
#endif
            return; // stale duplicate control CQE; its exact router is already retired

        RouterEntry& route = router_[router_index];
        if (route.control_state != RouterEntry::ControlState::submitted ||
            live_control_sqes_.load(std::memory_order_relaxed) == 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: control CQE without its "
                                 "exact submitted control reference (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        route.control_state = RouterEntry::ControlState::none;
        live_control_sqes_.fetch_sub(1, std::memory_order_relaxed);
        cancel_scratch_[route.handle.slot.value].cancel_queued = false;
        if (route.deferred_terminal_stored)
            finalize_operation_terminal_(router_index, route.deferred_terminal);
        return;
    }
    if (user_data == 0)
        return; // never emitted by this backend; harmless unknown value
    // Operation CQE: route by COOKIE VALUE. A bounded O(request_capacity)
    // scan of the fixed router finds the LIVE entry whose cookie matches. If no
    // live entry matches, this is a STALE cookie (its entry was retired and the
    // array slot may have been reused by a different cookie) — drop it. This is
    // the central ABA fix: the old router_slot+1 encoding resolved a stale CQE
    // to whatever NEW SlotHandle now occupied the recycled array slot.
    const std::size_t router_index = find_live_router_cookie_(user_data);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    fold_router_lookup_diag_for_test(RouterLookupKindForTest::operation_cqe);
    if (router_index == router_extent_())
#else
    if (router_index == router_.size())
#endif
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
    detail::TerminalResult terminal;
    if (res < 0) {
        terminal = detail::TerminalResult::err(sluice::from_errno_value(-res));
    } else if (is_byte_op) {
        terminal = detail::TerminalResult::ok_bytes(static_cast<std::uint64_t>(res));
    } else {
        terminal = detail::TerminalResult::ok_void();
    }
    // A submitted/prepared AsyncCancel is a real backend reference. Preserve
    // the original operation's terminal in bounded router scratch and keep the
    // accepted request outstanding until the matching tagged control CQE (or a
    // proven Class-A control recovery) retires. This closes the otherwise
    // unobservable accepted_outstanding==0 / live-control>0 teardown gap.
    if (entry.control_state != RouterEntry::ControlState::none) {
        if (!entry.deferred_terminal_stored) {
            entry.deferred_terminal = terminal;
            entry.deferred_terminal_stored = true;
        }
        return; // duplicate original CQE while control is live is harmless
    }
    finalize_operation_terminal_(router_index, terminal);
}

std::size_t UringAsyncBackend::reap_cqes() noexcept {
    // Count of NON-CONTROL CQEs observed this pass. This is NOT "operation
    // terminals recorded": handle_one_cqe drops an unknown/stale cookie without
    // recording a terminal, but such a CQE is still a non-control CQE observed
    // by the reap loop. Production callers (poll/wait_one) discard this value;
    // it exists for bounded diagnostics only. (Renamed from `recorded` to make
    // the observed-vs-recorded distinction explicit.)
    std::size_t non_control_observed = 0;
    constexpr unsigned BATCH = 32;
    io_uring_cqe* cqes[BATCH];
    unsigned got = 0;
    while ((got = ::io_uring_peek_batch_cqe(&ring_state_->ring, cqes, BATCH)) > 0) {
        for (unsigned i = 0; i < got; ++i) {
            io_uring_cqe* cqe = cqes[i];
            // Integer user_data API end-to-end: the SQE side
            // installs the op_cookie via io_uring_sqe_set_data64, so the CQE
            // side reads it via io_uring_cqe_get_data64. No pointer/uintptr_t
            // token round-trip — the kernel-visible identity is an integer at
            // both ends, which is the contract on every liburing target.
            const std::uint64_t user_data = ::io_uring_cqe_get_data64(cqe);
            const int res = cqe->res;
            ::io_uring_cqe_seen(&ring_state_->ring, cqe);
            // Snapshot whether this CQE is an operation (vs control) BEFORE
            // handle_one_cqe potentially retires the router entry.
            const bool is_op = (!is_control_cookie(user_data) && user_data != 0);
            handle_one_cqe(user_data, res);
            if (is_op)
                ++non_control_observed;
        }
        if (got < BATCH)
            break;
    }
    return non_control_observed;
}

// ---------------------------------------------------------------------------
// poll / wait_one — reap is the SOLE Completion-ready publication authority
// ---------------------------------------------------------------------------

int UringAsyncBackend::wait_cqe_without_submit() noexcept {
    // Do not call any liburing helper that flushes sqe_head/sqe_tail here. On a
    // poisoned ring the shared SQ tail still names the quarantined Class-A
    // batch. to_submit=0 is the proof boundary that waits for old Class-C CQEs
    // without ever consuming that batch.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (ring_state_->test_hooks.before_poison_wait != nullptr)
        ring_state_->test_hooks.before_poison_wait(ring_state_->test_hooks.context);
#endif
    return ::io_uring_enter(static_cast<unsigned>(ring_state_->ring.ring_fd), 0, 1,
                            IORING_ENTER_GETEVENTS, nullptr);
}

std::size_t UringAsyncBackend::poll() {
    if (!have_ring_)
        return 0;
    // Re-dispatch enqueued requests toward ring ownership (best-effort; an
    // SQ-full request stays enqueued and retries next poll). Done under
    // dispatch_mtx_; dispatch_one_locked assumes the lock is held.
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        if (!fatal_error_.has_value()) {
            // Peek protocol: the dispatch queue owns local execution. We PEEK
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
        // Flush transport while holding the ledger/dispatch domain. Positive
        // results consume only the ledger prefix; permanent failure invokes
        // the separate proof controller under this same frozen ownership view.
        if (!fatal_error_.has_value())
            (void)submit_transport_locked();
    }
    // Reap CQEs -> record_terminal ONLY. Then reap publishes Completion-ready
    // through the slot binding inside the leaf domain (ADR Decision 9 / I11).
    (void)reap_cqes();
    const std::size_t n =
        arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
    // State first, then notify: after real readiness is
    // published, advance the split-phase wait's progress epoch and wake every
    // parked participant so it re-polls (a concurrent wait_one must not sleep
    // through a publication made by this poll).
    if (n > 0) {
        signal_ready_progress();
    }
    return n;
}

Result<std::size_t> UringAsyncBackend::wait_one() {
    if (!have_ring_)
        return std::size_t{0};
    // First a non-blocking pass: flush transport, reap any already-ready CQEs,
    // and publish whatever is backend_ready. This resolves the common case
    // (work already complete) without a kernel wait.
    {
        // Re-dispatch enqueued work toward ring ownership before flushing.
        // Peek protocol (see poll() for the full rationale): peek the
        // front, attempt the transfer, break on NULL SQE. Never pop→dispatch→
        // push_back.
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        if (!fatal_error_.has_value()) {
            while (!dispatch_->empty()) {
                detail::SlotHandle h = dispatch_->front();
                if (!dispatch_one_locked(h))
                    break; // SQ full (or fatal); h remains at front for next call
            }
            if (!fatal_error_.has_value())
                (void)submit_transport_locked();
        }
    }
    (void)reap_cqes();
    std::size_t n = arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
    if (n > 0) {
        signal_ready_progress();
        return n;
    }
    if (arena_.accepted_outstanding() == 0 &&
        live_control_sqes_.load(std::memory_order_relaxed) == 0 &&
        (fatal_error_.has_value() || transport_ledger_->empty()))
        return std::size_t{0};

    // Nothing ready yet but work is outstanding: block in the KERNEL until at
    // least one CQE arrives (single-driver model — there is no separate worker
    // thread to signal a ReadyWaitSource). io_uring_submit_and_wait both
    // flushes pending SQEs and blocks for min_complete=1 CQE. A transient wake
    // is not a drained boundary: only accepted_outstanding()==0 may return 0.
    auto submit_and_wait_once = [&]() noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        if (ring_state_->test_hooks.submit_and_wait != nullptr) {
            return ring_state_->test_hooks.submit_and_wait(ring_state_->test_hooks.context,
                                                           &ring_state_->ring, 1);
        }
#endif
        return ::io_uring_submit_and_wait(&ring_state_->ring, 1);
    };

    for (;;) {
        int rc = 0;
        bool poisoned_wait = false;
        {
            std::lock_guard<std::mutex> lk(dispatch_mtx_);
            poisoned_wait = fatal_error_.has_value();
            if (!poisoned_wait) {
                const bool had_pending_transport = !transport_ledger_->empty();
                rc = sluice::detail::retry_uring_wait_on_eintr(submit_and_wait_once);
                account_transport_result_locked(rc, had_pending_transport);
            }
        }
        if (poisoned_wait) {
            rc = sluice::detail::retry_uring_wait_on_eintr(
                [&]() noexcept { return wait_cqe_without_submit(); });
        } else if (fatal_error_.has_value()) {
            // A permanent negative submit just ran the Class-A recovery
            // controller. Do not surface the transport error as an out-of-band
            // wait failure; reap publishes the defined per-request terminals.
            rc = 0;
        }

        if (rc < 0 && rc != -EAGAIN && rc != -EBUSY) {
            // A genuine to_submit=0 wait error (or a wait error when no ledger
            // was pending) has no Class-A proof. Surface it without fabricating
            // a terminal for old kernel-owned work.
            return make_unexpected<std::size_t>(sluice::from_errno_value(-rc));
        }
        (void)reap_cqes();
        n = arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
        if (n > 0) {
            signal_ready_progress();
            return n;
        }
        if (arena_.accepted_outstanding() == 0 &&
            live_control_sqes_.load(std::memory_order_relaxed) == 0 &&
            (fatal_error_.has_value() || transport_ledger_->empty()))
            return std::size_t{0};
        // -EAGAIN/-EBUSY, a spurious/empty wake, or a control-only CQE while
        // user work remains outstanding: retry without reporting a false
        // drain.
    }
}

// ---------------------------------------------------------------------------
// cancel — Completion-keyed, drives the shared state machine (ADR Decision 11)
// ---------------------------------------------------------------------------

// The production cancel core shared by the public Completion-keyed cancel()
// overloads and the guarded cancel_handle_for_test seam: DISARM LOCAL
// EXECUTION FIRST (remove from the local dispatch ring under dispatch_mtx_, so
// a pending/enqueued request cannot both be terminalized locally and have its
// SQE installed — frozen design §8.1), TERMINAL WIN SECOND (drive the shared
// state machine). On terminal_won the canceled terminal is already stored and
// readiness is signaled so reap publishes once; on intent_recorded a bounded
// best-effort AsyncCancel SQE is appended (frozen design §8.2).
detail::CancelDisposition UringAsyncBackend::cancel_handle_(
    detail::SlotHandle handle) noexcept {
    detail::CancelDisposition disp;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        (void)dispatch_->remove_exact(handle);
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
    return disp;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
void UringAsyncBackend::wait_after_commit_before_enqueue_pause_() noexcept {
    auto* g = after_commit_before_enqueue_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    while (!g->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    g->exited.store(true, std::memory_order_release);
}

void UringAsyncBackend::wait_before_dispatch_transfer_pause_() noexcept {
    auto* g = before_dispatch_transfer_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->dispatch_domain_released.store(true, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    while (!g->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    g->exited.store(true, std::memory_order_release);
}

void UringAsyncBackend::wait_before_commit_binding_pause_() noexcept {
    auto* g = before_commit_binding_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->admission_domain_held.store(true, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    while (!g->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    g->exited.store(true, std::memory_order_release);
}

void UringAsyncBackend::wait_before_admission_lock_pause_() noexcept {
    auto* g = before_admission_lock_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    while (!g->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    g->exited.store(true, std::memory_order_release);
}
#endif

void UringAsyncBackend::cancel(Completion<std::size_t>& c) {
    if (!have_ring_)
        return;
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value())
        return;
    (void)cancel_handle_(*h);
}

void UringAsyncBackend::cancel(Completion<void>& c) {
    if (!have_ring_)
        return;
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value())
        return;
    (void)cancel_handle_(*h);
}

// Production waiter registration / cancellation (ADR Decision 10).
// The waiter registration is ORTHOGONAL to the execution state: it needs no
// ring interaction (no SQE, no dispatch lock) — the arena leaf serializes
// registration against reap extraction and cancel_waiter against reap, and
// CQE/transport progress never reads waiter state. The candidate lease is
// consumed at the by-value boundary on any failure.
Result<void> UringAsyncBackend::register_waiter(Completion<std::size_t>& c,
                                                detail::WaiterToken token,
                                                detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}

Result<void> UringAsyncBackend::register_waiter(Completion<void>& c,
                                                detail::WaiterToken token,
                                                detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}

Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter(
    Completion<std::size_t>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}

Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter(
    Completion<void>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}

void UringAsyncBackend::issue_running_cancel(detail::SlotHandle h) noexcept {
    // The arena already recorded cancel intent (cancel_intent_). We only need
    // to append a best-effort AsyncCancel SQE if one has not already been
    // appended for this slot (frozen design §8.2: one fixed per-slot
    // cancel_queued bit, no unbounded cancel SQEs).
    bool newly_poisoned = false; // set iff THIS call poisons the backend
    std::uint64_t target_cookie = 0;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        // fatal_error_ is dispatch_mtx_-authority: every read here is inside
        // the lock (no unlocked reads). An already-poisoned
        // backend returns without a wake: the path that set the poison owns
        // the deferred wake.
        if (fatal_error_.has_value())
            return;
        CancelScratch& scratch = cancel_scratch_[h.slot.value];
        if (scratch.cancel_queued)
            return;
        // RESOLVE THE TARGET BEFORE get_sqe: find h's LIVE router entry
        // and read its kernel-visible cookie. If h is not currently
        // ring-owned (no LIVE entry), there is nothing to cancel — return
        // WITHOUT obtaining an SQE. This closes the get_sqe-then-discover-
        // nothing-to-cancel hole: io_uring_get_sqe() commits the SQE to the
        // next flush, so we must not obtain one we would then abandon.
        const std::size_t idx = find_live_router_index_(h);
        if (idx == router_.size())
            return; // not currently ring-owned; nothing to cancel
        target_cookie = router_[idx].cookie;
        if (target_cookie == 0 || target_cookie >= CONTROL_TAG) {
            // Defensive: a LIVE entry must carry a valid operation cookie. A
            // zero/control cookie here is an invariant violation.
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: issue_running_cancel "
                                 "found LIVE router entry with invalid cookie "
                                 "(invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        // From here, the target's cookie cannot change: dispatch/cancel-side
        // access holds this lock, while CQE retirement is excluded by the
        // AsyncIoContext::access_mtx_ single-driver call domain. Obtain the
        // SQE and fill it.
        io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_state_->ring);
        if (sqe == nullptr) {
            (void)submit_transport_locked();
            if (fatal_error_.has_value()) {
                // THIS call's transport flush permanently
                // poisoned the backend and retired the retained ledger
                // entries to backend-ready terminals. No reap runs on this
                // path, so the parked split-phase waiters MUST still be woken
                // — the wake is deferred past the lock scope below instead of
                // being dropped by an early return (state published, wake
                // obligation missing would strand a parked wait_one forever;
                // AGENTS.md §3.6).
                newly_poisoned = true;
            } else {
                sqe = ::io_uring_get_sqe(&ring_state_->ring);
            }
        }
        if (sqe != nullptr) {
            // NO-FAIL REGION: get_sqe committed the SQE. Fill it with
            // the resolved target cookie and its exact tagged control
            // user_data. (When sqe is still nullptr — SQ full without a new
            // poison, or the newly-poisoned branch above — there is no SQE to
            // fill: the cancel retries on the next poll, and the deferred
            // wake below covers the poison case.)
            ::io_uring_prep_cancel64(sqe, target_cookie, /*flags=*/0);
            ::io_uring_sqe_set_data64(sqe, make_control_cookie(target_cookie));
            scratch.cancel_queued = true;
            RouterEntry& route = router_[idx];
            if (route.control_state != RouterEntry::ControlState::none) {
                std::fprintf(stderr, "sluice::async::UringAsyncBackend: duplicate control state "
                                     "before AsyncCancel append (invariant violation)\n");
                std::fflush(stderr);
                std::terminate();
            }
            route.control_state = RouterEntry::ControlState::prepared;
            const auto& sq = ring_state_->ring.sq;
            const std::uint32_t physical_position =
                static_cast<std::uint32_t>((sq.sqe_tail - 1u) & sq.ring_mask);
            transport_ledger_->append(TransportLedger::Kind::cancel_control, physical_position,
                                      target_cookie, h);
        }
    }
    // A permanent transport failure during the cancel-SQE
    // append retired ledger/queue entries to backend-ready terminals. No reap
    // runs on this path, so wake the split-phase waiters here, AFTER
    // dispatch_mtx_ is released (state first, then wake; the wait-source
    // mutex is a leaf never acquired under dispatch_mtx_ — frozen lock
    // order). fatal_error_ was snapshotted under the lock above, so
    // newly_poisoned is exact: an already-poisoned backend returned at the
    // top of the lock scope and its poisoner owns the wake.
    if (newly_poisoned) {
        signal_ready_progress();
    }
}

// Admission close (ADR Decision 15; mirrors ThreadPoolBackend).
// close_admission() takes the SAME lock the submit admission transaction
// (reserve .. commit_binding, the `binding -> outstanding` release-store being
// the accept linearization point) holds, so it serializes against an in-flight
// submit: after this returns, no new acceptance LP can occur — an in-flight
// submit either completed its LP first (submit wins) or a later submit
// observes admission closed at Stage 0 inside the lock and rejects
// synchronously with invalid_state (close wins). It does NOT cancel, rewrite,
// discard, or release accepted work; cancel/poll/wait_one/reap remain legal.
// THEN wakes every participant parked in the split-phase ready wait (close
// must not starve a parked wait_one). The wake is a one-shot
// control generation advance — a re-evaluation signal, not a fabricated
// completion and not a persistent "never park again" state: future waits
// snapshot the advanced generation and park normally, so an admission-closed
// backend with outstanding work never busy-spins.
void UringAsyncBackend::close_admission() {
    if (!have_ring_)
        return;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        arena_.close_admission();
        admission_closed_ = true;
    }
    if (wait_source_) {
        wait_source_->interrupt_all();
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
