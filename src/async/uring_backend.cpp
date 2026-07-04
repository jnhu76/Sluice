// Implementation of UringAsyncBackend (sluice-CORE-020B).
//
// Two modes via SLUICE_HAS_LIBURING (matching the 013B experimental-uring gate):
//   * defined: real io_uring via liburing. Submits SQEs for read/write/sync
//     (batched — submit is deferred until poll()/wait_one() or SQE pressure),
//     reaps CQEs in poll()/wait_one() (the single reaping family, ADR A3/O1).
//     CQE res<0 maps to IoError via from_errno_value(-res) (ADR E3). SQE
//     pressure flushes + retries and tallies queue_full_retries.
//   * undefined: UNSUPPORTED STUB. submit_* returns backend_error synchronously
//     so the project builds/links with no liburing dependency; tests run in stub
//     mode and assert the clean-skip contract.
//
// Cancel-race handling (ADR §7 X3 exactly-once). The structural rule:
//   - The ORIGINAL op's CQE is the ONLY event that completes its Completion.
//   - A cancel request submits an IORING_OP_ASYNC_CANCEL SQE and records intent
//     on the OpRec. The cancel CQE never touches the Completion; it is dropped
//     (target already resolved) or ignored (the op's own CQE with -ECANCELED,
//     its real result, or a normal result will follow and drive the terminal).
// This makes exactly-once structural rather than racy: the kernel may still
// deliver the op's real result, deliver -ECANCELED, or report the cancel as
// -ENOENT (already done) — in every case the Completion is mutated exactly once,
// when the original op's CQE is reaped.
#include <sluice/async/uring_backend.hpp>

#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <utility>

#if defined(SLUICE_HAS_LIBURING)
#include <liburing.h>

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>
#endif

namespace sluice::async {

#if !defined(SLUICE_HAS_LIBURING)
// ---------------------------------------------------------------------------
// Stub mode: no liburing dependency. submit_* rejects synchronously; nothing is
// recorded outstanding so outstanding() stays 0 and the L11 check passes.
// ---------------------------------------------------------------------------

UringAsyncBackend::UringAsyncBackend(unsigned queue_depth)
    : available_(false) {
    (void)queue_depth;  // stub: no ring to size
}

UringAsyncBackend::~UringAsyncBackend() = default;

namespace {
Result<void> unsupported_stub() {
    return make_unexpected<void>(IoError{IoError::Code::backend_error});
}
}  // namespace

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

std::size_t UringAsyncBackend::poll() { return 0; }
Result<std::size_t> UringAsyncBackend::wait_one() { return std::size_t{0}; }
void UringAsyncBackend::cancel(Completion<std::size_t>&) {}
void UringAsyncBackend::cancel(Completion<void>&) {}
std::size_t UringAsyncBackend::outstanding() const noexcept { return 0; }
bool UringAsyncBackend::available() const noexcept { return available_; }

#else  // SLUICE_HAS_LIBURING --------------------------------------------------

// ---------------------------------------------------------------------------
// Real io_uring path.
//
// User-data strategy: a monotonic 64-bit counter. Op ids and cancel-op ids share
// the counter (a cancel-op gets its own id so its CQE can be told apart from the
// original op's CQE). Each outstanding op is tracked in an OpRec keyed by its id;
// a separate cancel-index maps a cancel-op id back to the targeted op id so the
// cancel CQE can locate (and clear intent on) the OpRec without touching the
// Completion.
//
// Submit batching: submit_* only acquires an SQE + preps it (flushing on SQE
// pressure). The kernel is poked in poll()/wait_one() (and on pressure). This is
// what distinguishes 020B from the synchronous-over-uring spike (013): many SQEs
// may be in flight per submit, the high-concurrency shape.
//
// Threading note: the AsyncBackend contract is single-driver-thread (poll/
// wait_one are called from one thread, like the blocking Reader/Writer). No
// mutex is needed; liburing's ring is not internally locked. If a caller ever
// drives a ring from multiple threads that is a future ADR (gate item 6).
//
// All io_uring plumbing lives as private methods of Impl (not free functions) so
// the helpers can name Impl types without exposing the private nested class.
// ---------------------------------------------------------------------------

namespace {

// One outstanding operation (read/write/sync). The Completion is completed
// exactly once when this OpRec's own CQE is reaped (exactly-once, ADR §7 X3).
struct OpRec {
    // Exactly one of these is set, discriminated by is_void.
    Completion<std::size_t>* size_c = nullptr;
    Completion<void>* void_c = nullptr;
    bool is_void = false;
    std::size_t requested = 0;     // bytes requested (0 for sync); for short tally
    bool cancel_requested = false; // caller asked to cancel; intent only
    __u64 cancel_op_id = 0;        // id of the cancel SQE submitted for this op,
                                   // or 0 if none submitted yet
};

// Toggle a stat counter if a sink is attached.
inline void bump(sluice::AsyncStats* s, std::uint64_t sluice::AsyncStats::*field) {
    if (s) ++(s->*field);
}

// Reject path shared by all submit_* overloads when the ring isn't usable.
Result<void> no_ring() {
    return make_unexpected<void>(IoError{IoError::Code::backend_error});
}

}  // namespace

struct UringAsyncBackend::Impl {
    io_uring ring{};
    bool have_ring = false;          // ring initialized successfully
    unsigned queue_depth = 0;
    __u64 next_id = 1;               // 0 reserved as "no cancel"
    // Outstanding ops keyed by their user_data id.
    std::unordered_map<__u64, OpRec> ops;
    // Cancel-op id -> targeted op id. Lets the cancel CQE find its OpRec.
    std::unordered_map<__u64, __u64> cancel_to_op;
    // Completion* -> op id (O(1) cancel lookup, B3). Type-erased to void* so one
    // map serves both Completion<size_t> and Completion<void>. Maintained
    // alongside ops: inserted at register_op, erased at reap_op_cqe.
    std::unordered_map<void*, __u64> comp_to_op;

    Impl() = default;
    ~Impl() {
        if (have_ring) {
            ::io_uring_queue_exit(&ring);
            have_ring = false;
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // Acquire an SQE, flushing the ring on pressure (mirror the 013 spike's
    // flush+retry). Returns nullptr only if the ring cannot make room even after
    // a flush; the caller then reports backend_error/invalid_state. Bumps
    // queue_full_retries here so both the flush path and the still-full path are
    // counted uniformly.
    io_uring_sqe* get_sqe_with_pressure(sluice::AsyncStats* stats) {
        io_uring_sqe* sqe = ::io_uring_get_sqe(&ring);
        if (sqe != nullptr) return sqe;
        bump(stats, &sluice::AsyncStats::queue_full_retries);
        if (::io_uring_submit(&ring) < 0) return nullptr;
        return ::io_uring_get_sqe(&ring);
    }

    // Resolve one CQE for its targeted op: complete the Completion exactly once,
    // tally stats, and erase the OpRec. Cancel CQEs are routed elsewhere.
    //   res < 0            -> IoError via from_errno_value(-res) (ECANCELED here
    //                         is the kernel honoring our cancel; canceled_ops)
    //   res >= 0 (size op) -> bytes transferred (may be short; short_completions)
    //   res >= 0 (void op) -> success
    void reap_op_cqe(__u64 op_id, int res, sluice::AsyncStats* stats) {
        auto it = ops.find(op_id);
        if (it == ops.end()) return;  // unknown id (shouldn't happen); drop
        OpRec& rec = it->second;

        // Determine the Completion* for the reverse-index erase BEFORE we move
        // out of the record. is_void discriminates which pointer is live.
        void* comp_key = rec.is_void
                             ? static_cast<void*>(rec.void_c)
                             : static_cast<void*>(rec.size_c);

        if (rec.is_void) {
            if (res < 0) {
                IoError e = sluice::from_errno_value(-res);
                bool canceled = (e.code == IoError::Code::canceled);
                rec.void_c->complete_with(make_unexpected<void>(e));
                if (canceled) bump(stats, &AsyncStats::canceled_ops);
                else bump(stats, &AsyncStats::completion_errors);
            } else {
                rec.void_c->complete_with(Result<void>{});
            }
        } else {
            if (res < 0) {
                IoError e = sluice::from_errno_value(-res);
                bool canceled = (e.code == IoError::Code::canceled);
                rec.size_c->complete_with(make_unexpected<std::size_t>(e));
                if (canceled) bump(stats, &AsyncStats::canceled_ops);
                else bump(stats, &AsyncStats::completion_errors);
            } else {
                std::size_t got = static_cast<std::size_t>(res);
                rec.size_c->complete_with(Result<std::size_t>{got});
                if (got < rec.requested) bump(stats, &AsyncStats::short_completions);
            }
        }
        ops.erase(it);
        comp_to_op.erase(comp_key);  // B3: keep reverse index in sync
    }

    // O(1) cancel lookup: find the op id for a Completion*, or 0 if not
    // outstanding. Used by both cancel() overloads so cancel is O(1) average
    // instead of O(outstanding). (B3)
    __u64 op_id_for(void* comp_key) const {
        auto it = comp_to_op.find(comp_key);
        return it == comp_to_op.end() ? 0 : it->second;
    }

    // Reap every currently-ready CQE. Cancel CQEs clear intent only; op CQEs
    // complete their Completion. Returns the count of Completions made ready.
    std::size_t reap_ready(sluice::AsyncStats* stats) {
        std::size_t completed = 0;
        // A bounded batch keeps latency predictable; liburing reaps the rest next
        // poll(). 32 is a balance between throughput and per-poll cost.
        constexpr unsigned BATCH = 32;
        io_uring_cqe* cqes[BATCH];
        unsigned got = 0;
        while ((got = ::io_uring_peek_batch_cqe(&ring, cqes, BATCH)) > 0) {
            for (unsigned i = 0; i < got; ++i) {
                io_uring_cqe* cqe = cqes[i];
                __u64 id = static_cast<__u64>(
                    reinterpret_cast<std::uintptr_t>(io_uring_cqe_get_data(cqe)));
                int res = cqe->res;
                ::io_uring_cqe_seen(&ring, cqe);

                auto cit = cancel_to_op.find(id);
                if (cit != cancel_to_op.end()) {
                    // This is a cancel op's CQE. It NEVER completes a Completion.
                    // Clear intent on the targeted op (if still outstanding). The
                    // op's own CQE (-ECANCELED / real result / normal result) will
                    // drive the terminal exactly once.
                    __u64 target = cit->second;
                    cancel_to_op.erase(cit);
                    auto oit = ops.find(target);
                    if (oit != ops.end()) {
                        oit->second.cancel_requested = false;
                        oit->second.cancel_op_id = 0;
                    }
                    // res<0 here (e.g. -ENOENT = nothing to cancel) is
                    // informational only; the op CQE is authoritative. Drop.
                    continue;
                }
                reap_op_cqe(id, res, stats);
                ++completed;
            }
            if (got < BATCH) break;
        }
        return completed;
    }

    // Submit an IORING_OP_ASYNC_CANCEL SQE targeting `op_id`. Records the cancel
    // op id on the targeted OpRec and in cancel_to_op so its CQE can be told
    // apart from the op's own CQE. The cancel CQE never completes a Completion
    // (ADR §7 X3 exactly-once). Best-effort (ADR §7 X2): if no SQE can be
    // acquired, the cancel is dropped and the op completes normally.
    void submit_cancel(__u64 op_id, sluice::AsyncStats* stats) {
        auto it = ops.find(op_id);
        if (it == ops.end()) return;  // op already reaped; nothing to cancel
        io_uring_sqe* sqe = get_sqe_with_pressure(stats);
        if (sqe == nullptr) return;  // best-effort; op completes normally
        __u64 cancel_id = next_id++;
        // Target the original op by its user_data. flags=0: cancel first match.
        ::io_uring_prep_cancel64(sqe, op_id, 0);
        ::io_uring_sqe_set_data(
            sqe, reinterpret_cast<void*>(static_cast<std::uintptr_t>(cancel_id)));
        it->second.cancel_op_id = cancel_id;
        cancel_to_op.emplace(cancel_id, op_id);
    }
};

UringAsyncBackend::UringAsyncBackend(unsigned queue_depth)
    : impl_(new Impl()), available_(false) {
    impl_->queue_depth = queue_depth > 0 ? queue_depth : 64;
    if (::io_uring_queue_init(impl_->queue_depth, &impl_->ring, 0) == 0) {
        impl_->have_ring = true;
        available_ = true;
    } else {
        // Construction is allowed to fail (e.g. kernel without io_uring); the
        // instance is then constructible-but-unavailable, mirroring the stub's
        // available()==false contract. submit_* will reject synchronously below.
        delete impl_;
        impl_ = nullptr;
    }
}

UringAsyncBackend::~UringAsyncBackend() { delete impl_; }

namespace {

// Register a freshly-prepped op (SQE already filled, user_data already set) in
// the outstanding table + the O(1) cancel reverse-index, and mark the
// Completion outstanding. Returns the op id. Templated on the Impl type so this
// free helper does not have to name the private nested Impl; it only needs
// ops/next_id/comp_to_op members, which Impl provides.
template <class ImplLike, class C>
__u64 register_op(ImplLike& impl, __u64 id, C& c, OpRec rec) {
    c.mark_outstanding();
    void* comp_key = rec.is_void
                         ? static_cast<void*>(rec.void_c)
                         : static_cast<void*>(rec.size_c);
    impl.comp_to_op.emplace(comp_key, id);  // B3: O(1) cancel lookup
    impl.ops.emplace(id, std::move(rec));
    impl.next_id = id + 1;
    return id;
}

}  // namespace

Result<void> UringAsyncBackend::submit_read(ReadOp op, Completion<std::size_t>& c) {
    if (!impl_ || !impl_->have_ring) return no_ring();
    if (!c.idle()) {
        bump(stats_, &AsyncStats::queue_full_retries);
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    io_uring_sqe* sqe = impl_->get_sqe_with_pressure(stats_);
    if (sqe == nullptr) {
        bump(stats_, &AsyncStats::queue_full_retries);
        return make_unexpected<void>(IoError{IoError::Code::backend_error});
    }
    __u64 id = impl_->next_id;
    ::io_uring_prep_read(sqe, op.fd, op.dst, static_cast<unsigned>(op.len),
                         static_cast<__s64>(op.offset));
    ::io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)));
    register_op(*impl_, id, c,
                OpRec{&c, nullptr, /*is_void=*/false, op.len, false, 0});
    return {};
}

Result<void> UringAsyncBackend::submit_write(WriteOp op, Completion<std::size_t>& c) {
    if (!impl_ || !impl_->have_ring) return no_ring();
    if (!c.idle()) {
        bump(stats_, &AsyncStats::queue_full_retries);
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    io_uring_sqe* sqe = impl_->get_sqe_with_pressure(stats_);
    if (sqe == nullptr) {
        bump(stats_, &AsyncStats::queue_full_retries);
        return make_unexpected<void>(IoError{IoError::Code::backend_error});
    }
    __u64 id = impl_->next_id;
    ::io_uring_prep_write(sqe, op.fd, op.src, static_cast<unsigned>(op.len),
                          static_cast<__s64>(op.offset));
    ::io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)));
    register_op(*impl_, id, c,
                OpRec{&c, nullptr, /*is_void=*/false, op.len, false, 0});
    return {};
}

Result<void> UringAsyncBackend::submit_sync_data(SyncDataOp op, Completion<void>& c) {
    if (!impl_ || !impl_->have_ring) return no_ring();
    if (!c.idle()) {
        bump(stats_, &AsyncStats::queue_full_retries);
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    io_uring_sqe* sqe = impl_->get_sqe_with_pressure(stats_);
    if (sqe == nullptr) {
        bump(stats_, &AsyncStats::queue_full_retries);
        return make_unexpected<void>(IoError{IoError::Code::backend_error});
    }
    __u64 id = impl_->next_id;
    ::io_uring_prep_fsync(sqe, op.fd, IORING_FSYNC_DATASYNC);
    ::io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)));
    register_op(*impl_, id, c,
                OpRec{nullptr, &c, /*is_void=*/true, 0, false, 0});
    return {};
}

Result<void> UringAsyncBackend::submit_sync_all(SyncAllOp op, Completion<void>& c) {
    if (!impl_ || !impl_->have_ring) return no_ring();
    if (!c.idle()) {
        bump(stats_, &AsyncStats::queue_full_retries);
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    io_uring_sqe* sqe = impl_->get_sqe_with_pressure(stats_);
    if (sqe == nullptr) {
        bump(stats_, &AsyncStats::queue_full_retries);
        return make_unexpected<void>(IoError{IoError::Code::backend_error});
    }
    __u64 id = impl_->next_id;
    ::io_uring_prep_fsync(sqe, op.fd, 0);  // 0 => full fsync (sync_all)
    ::io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)));
    register_op(*impl_, id, c,
                OpRec{nullptr, &c, /*is_void=*/true, 0, false, 0});
    return {};
}

std::size_t UringAsyncBackend::poll() {
    if (!impl_ || !impl_->have_ring) return 0;
    // Flush any pending SQEs (non-blocking) so progress is visible without a
    // blocking wait. submit() returns submitted-or-negative; either way we reap.
    (void)::io_uring_submit(&impl_->ring);
    return impl_->reap_ready(stats_);
}

Result<std::size_t> UringAsyncBackend::wait_one() {
    if (!impl_ || !impl_->have_ring) return std::size_t{0};
    // submit_and_wait flushes pending SQEs AND blocks for >=1 CQE. -errno on
    // failure (e.g. -EINTR); map to a backend error and let the caller retry.
    int rc = ::io_uring_submit_and_wait(&impl_->ring, 1);
    if (rc < 0) {
        // EINTR on the wait is recoverable; report it as interrupted so the
        // caller can retry rather than treating it as a hard backend failure.
        IoError e = sluice::from_errno_value(-rc);
        return make_unexpected<std::size_t>(e);
    }
    return impl_->reap_ready(stats_);
}

void UringAsyncBackend::cancel(Completion<std::size_t>& c) {
    if (!impl_ || !impl_->have_ring) return;
    // O(1) cancel lookup via the reverse index (B3). Was a linear scan of ops.
    const __u64 id = impl_->op_id_for(static_cast<void*>(&c));
    if (id == 0) return;  // not outstanding (already reaped or never submitted)
    auto it = impl_->ops.find(id);
    if (it == impl_->ops.end()) return;
    if (it->second.cancel_requested) return;  // already requested; exactly-once intent
    it->second.cancel_requested = true;
    impl_->submit_cancel(id, stats_);
}

void UringAsyncBackend::cancel(Completion<void>& c) {
    if (!impl_ || !impl_->have_ring) return;
    const __u64 id = impl_->op_id_for(static_cast<void*>(&c));
    if (id == 0) return;
    auto it = impl_->ops.find(id);
    if (it == impl_->ops.end()) return;
    if (it->second.cancel_requested) return;
    it->second.cancel_requested = true;
    impl_->submit_cancel(id, stats_);
}

std::size_t UringAsyncBackend::outstanding() const noexcept {
    return impl_ ? impl_->ops.size() : 0;
}

bool UringAsyncBackend::available() const noexcept { return available_; }

#endif  // SLUICE_HAS_LIBURING

}  // namespace sluice::async
