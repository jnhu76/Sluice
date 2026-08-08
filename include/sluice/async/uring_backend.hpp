// sluice::async::UringAsyncBackend (sluice-CORE-020B, ADR §4 Option 4).
//
// The Linux io_uring backend. Phase D1 migrates it onto the bounded
// RequestArena / RequestSlot lifecycle with a PRIVATE io_uring ring per
// backend instance (ADR Decision 18 — Uring execution-ownership amendment):
//
//   RequestArena            = logical request lifecycle / generation / terminal
//   one private io_uring    = execution ownership domain
//   io_uring_submit()       = transport progress only (NO RequestState change)
//   original operation CQE  = execution retirement / terminal candidate
//   RequestArena::reap()    = sole Completion-ready publication authority
//
// GATED behind liburing (ADR §11 D4 — optional dep):
//   * SLUICE_HAS_LIBURING defined (liburing linked): real io_uring path.
//   * otherwise: UNSUPPORTED STUB. submit_* returns IoError::backend_error
//     synchronously; poll()/wait_one() reap nothing. The project builds with
//     no liburing dependency.
//
// Cancel (ADR Decision 11, layered): pending/enqueued cancel may win the
// canceled terminal directly (Scheme B) — its operation SQE was NEVER
// installed into the ring and cannot execute. running/ring-owned cancel
// records intent only and may append an IORING_OP_ASYNC_CANCEL whose CQE is
// CONTROL-INFORMATIONAL (res ∈ {0, -ENOENT, -EALREADY}); it MUST NOT publish
// a terminal or release the slot. The original operation CQE decides the
// terminal (success / ordinary error / -ECANCELED).
//
// Resource bounds (AC-7, ADR Decision 13) — DISTINCT resources:
//   request_capacity : arena slots == dispatch ring entries == CqeRouter slots
//   queue_depth      : io_uring SQ/CQ depth (kernel-owned)
//   request_capacity > queue_depth is LEGAL; excess accepted work stays
//   enqueued locally until an SQE is available.
//
// Shutdown (ADR Decision 15; AGENTS.md §14): destruction is quiescent. The
// destructor tears down the ring; it does NOT implicitly cancel, drain, wait
// for a CQE, publish, or discard accepted work. Non-quiescent destruction is
// a contract violation.
//
// See docs/architecture/phase-d1-uring-frozen-design.md (the frozen design /
// compliance gate). State is instance-owned (no globals).
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/reference_ready_sink.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_URING_INTERNAL_TESTING)
struct io_uring;
#endif

namespace sluice::async {

#if defined(SLUICE_HAS_LIBURING)
// Opaque pimpl holding the io_uring instance + transport state. Defined in the
// .cpp so this header never needs <liburing.h> (the experimental gate defines
// SLUICE_HAS_LIBURING without requiring liburing headers in includers).
struct UringRingState;
#endif

#if defined(SLUICE_HAS_LIBURING)
// Phase D1 configuration (AC-7, ADR Decision 13). Both MUST be > 0.
// request_capacity is independent of queue_depth (ADR Decision 13 / 18);
// request_capacity > queue_depth is legal.
struct UringConfig {
    std::size_t request_capacity = 64; // arena + dispatch ring + router capacity
    unsigned queue_depth = 64;         // io_uring SQ/CQ depth
};
#endif

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_URING_INTERNAL_TESTING)
// Non-installed submit seam used by the dedicated real-liburing fault tests.
// Production targets never define SLUICE_URING_INTERNAL_TESTING and therefore
// expose neither this type nor the constructor overload below.
struct UringBackendSubmitTestHooks {
    using SubmitFn = int (*)(void*, ::io_uring*) noexcept;

    void* context = nullptr;
    SubmitFn submit = nullptr;
};
#endif

class UringAsyncBackend : public AsyncBackend {
  public:
    // Legacy source-compatible constructor: maps to
    // UringConfig{request_capacity == queue_depth, queue_depth}. Stub mode
    // (no liburing) ignores depth and reports available()==false.
    explicit UringAsyncBackend(unsigned queue_depth = 64);

#if defined(SLUICE_HAS_LIBURING)
    // Phase D1 explicit bounded configuration. Both fields MUST be > 0; a 0
    // value is rejected with std::invalid_argument.
    explicit UringAsyncBackend(UringConfig config);
#endif
#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_URING_INTERNAL_TESTING)
    UringAsyncBackend(UringConfig config, UringBackendSubmitTestHooks hooks);
#endif
    ~UringAsyncBackend() override;

    UringAsyncBackend(const UringAsyncBackend&) = delete;
    UringAsyncBackend& operator=(const UringAsyncBackend&) = delete;

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override;
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override;
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override;
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override;

    std::size_t poll() override;
    Result<std::size_t> wait_one() override;

    void cancel(Completion<std::size_t>& c) override;
    void cancel(Completion<void>& c) override;

    std::size_t outstanding() const noexcept override;

    // Whether this instance initialized a real io_uring (false in stub mode
    // or when the host kernel cannot create a ring). This is a capability
    // query, not a health query.
    bool available() const noexcept;

#if defined(SLUICE_HAS_LIBURING)
    // Phase D1 does NOT override wait_source(): Uring is a single-driver
    // backend whose wait_one() blocks in the kernel (io_uring_submit_and_wait)
    // and reaps CQEs synchronously on the calling thread. There is no separate
    // worker thread to signal a ReadyWaitSource, so declaring split-wait
    // capability would make AsyncIoContext::wait_one park in wait_for_change
    // forever. A future shard/M:N topology with a dedicated CQE-reaper thread
    // may revisit this; D1 leaves wait_source() returning nullptr (the default).
    // close_admission() also remains the default (no-op) for D1; D4 owns the
    // full close/drain redesign.

    // Phase D1 resource introspection (method-only seams; no member data).
    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_accepted_outstanding() const noexcept {
        return arena_.accepted_outstanding();
    }
    std::size_t arena_capacity_rejections() const noexcept { return arena_.capacity_rejections(); }
    std::size_t configured_queue_depth() const noexcept { return queue_depth_; }
#endif

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_URING_INTERNAL_TESTING)
    // Test-only: number of io_uring_submit() transport flushes actually issued
    // (proves submit is transport progress, decoupled from lifecycle).
    std::uint64_t submit_flushes_for_test() const noexcept {
        return submit_flushes_.load(std::memory_order_relaxed);
    }
    // Test-only: live operation cookies in the CqeRouter (bounded by
    // request_capacity).
    std::size_t live_cookies_for_test() const noexcept {
        return live_cookies_.load(std::memory_order_relaxed);
    }
#endif

  private:
#if defined(SLUICE_HAS_LIBURING)
    // ---- fixed tagged operation payload (per-slot scratch, Scheme B) -------
    // Sized to request_capacity at construction, indexed by SlotIndex (1:1 with
    // arena slots). Carries the SQE descriptor; filled in prepare(), read by
    // dispatch after the slot is current-generation enqueued.
    struct PreparedUringOp {
        detail::OperationKind kind = detail::OperationKind::read;
        int fd = -1;
        const std::byte* buffer = nullptr; // dst (read) / src (write) / null (sync)
        std::size_t length = 0;            // requested length (for short-completion tally)
        unsigned native_length = 0;        // liburing nbytes (validated <= UINT_MAX at prepare)
        std::uint64_t offset = 0;
    };

    // Bounded op_cookie -> full SlotHandle router (frozen design §7.2).
    // Construction-time capacity == request_capacity. NOT a request store: it
    // is transport routing metadata (ADR Decision 3 backend scratch). The arena
    // re-validates the full handle before any mutation.
    //
    // Kernel-visible identity discipline (P0-B): the SQE user_data carries the
    // COOKIE VALUE, not a router array index. The cookie is allocated from a
    // no-wrap 64-bit counter and is NEVER reused within backend lifetime (mirors
    // RequestArena generation no-wrap). The router ARRAY slot is recycled via a
    // free-list, but because routing keys on the cookie value, a stale CQE
    // (whose cookie belongs to a retired entry) no longer matches any live
    // entry and is dropped — the ABA window that existed when user_data carried
    // router_slot+1 is closed. The arena still re-validates the full generation
    // as a second layer of defense. CONTROL_CANCEL (= UINT64_MAX) is reserved
    // for cancel-SQE user_data outside the operation-cookie domain [1, UINT64_MAX-1].
    struct RouterEntry {
        std::uint64_t cookie = 0; // 0 = not a live operation cookie
        detail::SlotHandle handle{};
        bool in_use = false;
    };

    // Bounded local dispatch ring (capacity == request_capacity). Stores
    // SlotHandle only; push_back/front/remove_exact are noexcept. Mirrors the
    // ThreadPool BoundedDispatchQueue discipline.
    class BoundedDispatchQueue;

    // Process-wide monotonic id for ContextIdentity provenance (distinct per
    // UringAsyncBackend instance — ADR Decision 2).
    static std::uint64_t next_backend_id() noexcept {
        static std::atomic<std::uint64_t> id{0x55720000u}; // 'Ur' provenance tag
        return ++id;
    }

    // Descriptor validation for a REAL syscall backend (ADR Decision 6;
    // AGENTS.md §9.1 — no fcntl(F_GETFD) preflight TOCTOU).
    static Result<void> validate_read(ReadOp op);
    static Result<void> validate_write(WriteOp op);
    static Result<void> validate_sync(SyncDataOp op);
    static Result<void> validate_sync(SyncAllOp op);
    template <class Op> static Result<void> validate_op(const Op& op) noexcept;

    // Five-stage admission mirroring ThreadPoolBackend (ADR Decision 5).
    template <class Op>
    Result<void> submit_size(Op op, Completion<std::size_t>& c, detail::OperationKind kind,
                             std::size_t len);
    template <class Op>
    Result<void> submit_void(Op op, Completion<void>& c, detail::OperationKind kind);

    template <class Op> static detail::BorrowMetadata borrow_of(const Op& op) noexcept {
        if constexpr (std::is_same_v<Op, ReadOp>) {
            return {op.fd, op.dst, op.len};
        } else {
            return {op.fd, op.src, op.len};
        }
    }

    // Publication thunks (ADR Decision 9): convert the arena's TerminalResult
    // to a Result<T> and publish Completion-ready through the protected helper.
    // Static + type-erased; the arena calls them inside the leaf domain at reap.
    static void publish_size_ready(void* completion, const detail::TerminalResult& t) noexcept;
    static void publish_void_ready(void* completion, const detail::TerminalResult& t) noexcept;
    static Result<std::size_t> terminal_to_size(const detail::TerminalResult& t) noexcept;
    static Result<void> terminal_to_void(const detail::TerminalResult& t) noexcept;

    // Dispatch one enqueued request toward ring ownership (frozen design §4).
    // Acquires dispatch_mtx_. Returns false if the request could not be
    // dispatched this pass (SQ full / fatal); true if it became ring-owned.
    bool dispatch_one(detail::SlotHandle h) noexcept;
    // P0-A peek protocol: assumes dispatch_mtx_ is held AND h == dispatch_
    // ->front(). The caller peeks the front and does NOT remove it before this
    // call; on a successful transfer this function removes h exactly once via
    // dispatch_->remove_exact(h) (a miss is an invariant violation -> fail-fast).
    // On a NULL SQE the function returns false WITHOUT mutating the queue (h
    // stays at the front). Used by the poll()/wait_one() peek drains and by
    // enqueue_after_commit's single-critical-section path.
    bool dispatch_one_locked(detail::SlotHandle h) noexcept;

    // Unified enqueue + dispatch attempt under ONE dispatch_mtx_ critical
    // section. noexcept; the caller has already committed. Holding the lock
    // across push_back -> dispatch_one_locked is load-bearing: it closes the
    // window in which a cancel() could terminalize h between enqueue and
    // dispatch (cancel takes the same lock). Therefore mark_running(h)==false
    // inside the transaction is an invariant violation, not a cancel-won race.
    void enqueue_after_commit(detail::SlotHandle h) noexcept;

    // Transport progress: io_uring_submit(). DOES NOT mutate RequestState.
    // Returns the liburing result for diagnostics/ring-poison handling.
    int submit_transport() noexcept;

    // Reap all currently-ready CQEs: route op cookies to full SlotHandles,
    // validate generation, record_terminal ONLY (never publish). Control
    // cancel CQEs update only fixed cancel bookkeeping. Returns the count of
    // operation CQEs whose terminal was recorded.
    std::size_t reap_cqes() noexcept;

    // Decode one CQE: op cookie -> record_terminal; control -> bookkeeping.
    void handle_one_cqe(std::uint64_t user_data, int res) noexcept;

    // Allocate a unique operation cookie from the no-wrap counter. Domain is
    // [1, UINT64_MAX-1]; 0 is unused and UINT64_MAX (CONTROL_CANCEL) is
    // reserved. If the counter would reach CONTROL_CANCEL, fail-fast (mirrors
    // RequestArena generation no-wrap discipline) — never wrap. The cookie is
    // NEVER reused within backend lifetime, so a stale CQE's cookie cannot
    // match a later LIVE entry.
    std::uint64_t allocate_cookie_() noexcept;

    // Find the router ARRAY index of the LIVE entry whose SlotHandle matches h
    // (slot + full generation). Returns the index, or request_capacity (==
    // router_.size()) if no live entry matches (h is not currently ring-owned).
    // Bounded O(request_capacity) scan, allocation-free.
    std::size_t find_live_router_index_(detail::SlotHandle h) const noexcept;

    // Per-slot backend cancel bookkeeping. The arena owns cancel_intent_
    // (intent authority); this struct only tracks whether an AsyncCancel SQE
    // has already been appended for this slot, so repeated cancel() calls do
    // not enqueue unbounded cancel SQEs (frozen design §8.2).
    struct CancelScratch {
        bool cancel_queued = false;
    };

    // Cancel bookkeeping + best-effort AsyncCancel SQE append for a running
    // request. Idempotent per-slot (one cancel_queued bit).
    void issue_running_cancel(detail::SlotHandle h) noexcept;

    // Wake the ready domain. D1 is single-driver (wait_one blocks in the
    // kernel and reaps synchronously), so there is no separate worker to wake;
    // this is a no-op kept as a seam for a future shard/M:N topology with a
    // dedicated CQE-reaper thread.
    void signal_ready_progress() noexcept {}

    // ---- members -----------------------------------------------------------
    detail::RequestArena arena_;
    detail::ReferenceReadySink sink_;
    std::vector<PreparedUringOp> prepared_ops_;       // size == request_capacity
    std::vector<RouterEntry> router_;                 // size == request_capacity
    std::vector<CancelScratch> cancel_scratch_;       // size == request_capacity
    std::vector<detail::SlotIndex> cookie_free_list_; // free router slots
    std::uint64_t next_cookie_ = 1;                   // 0 reserved; CONTROL_CANCEL = max
    unsigned queue_depth_ = 64;

    // The private io_uring instance (opaque pimpl — owns the io_uring + the
    // internal-testing submit hook). One ring per backend (ADR Decision 18).
    std::unique_ptr<UringRingState> ring_state_;
    bool have_ring_ = false;
    bool admission_closed_ = false;
    std::optional<IoError> fatal_error_; // permanent transport poison

    // Backend dispatch domain: local dispatch ring + dispatch/cancel
    // arbitration + cookie/router/scratch mutation. Lock order:
    // dispatch_mtx_ -> arena leaf only — never nested with the ready-wait
    // mutex. io_uring_submit() (syscall) is transport progress and may be
    // called under dispatch_mtx_ but NEVER under the arena mutex.
    mutable std::mutex dispatch_mtx_;
    std::unique_ptr<BoundedDispatchQueue> dispatch_;

    std::atomic<std::uint64_t> submit_flushes_{0};
    std::atomic<std::size_t> live_cookies_{0};
#endif // SLUICE_HAS_LIBURING

    bool available_ = false;
};

} // namespace sluice::async
