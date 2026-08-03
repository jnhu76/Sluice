// sluice::async default backend for job 017 (ADR §3/§4).
//
// SyncBackend completes ops SYNCHRONOUSLY at the next poll()/wait_one(). It
// holds no kernel state and uses no threads — it is the minimal in-process
// backend that lets the 017 foundation compile, link, and be tested before the
// real backends land (019 FakeAsyncBackend, 020A ThreadPool, 020B Uring).
//
// Semantics for 017: every submitted op is buffered; poll()/wait_one() marks all
// of them ready with a synthetic result. ReadOps complete with their full `len`
// (no actual read — 017 explicitly touches no fd); WriteOps complete with `len`;
// sync ops complete with void. This is enough to test the Completion lifecycle,
// submit/poll/wait plumbing, and AsyncStats. It is NOT a correctness backend
// for real I/O — that comes with 019/020A.
//
// Phase B (ADR-explicit-io-request-contract, Accepted): SyncBackend now drives
// the bounded RequestArena five-stage admission (reserve -> prepare -> commit
// -> enqueue -> dispatch/reap) and the unified reap path with a synchronous
// identity-bearing ReadySink. The public submit_*/poll/wait_one/cancel surface
// is unchanged (ADR Decision 7); the RequestKey is bound privately during
// commit and resolved internally for cancel. The synthetic terminal result is
// stored at dispatch time (record_terminal) so poll deterministically
// transitions pending/enqueued -> backend_ready; poll()/wait_one() reaps.
//
// Identity (review C2): the Completion publication binding lives IN the
// RequestSlot record (install_publication_binding before the Completion CAS);
// reap validates it and publishes Completion-ready through it inside the leaf
// domain. There is NO parallel unordered_map identity bridge — cancel resolves
// a Completion* by the arena's bounded O(capacity) scan. Pre-commit
// bookkeeping is transactional (review C1): every pre-commit failure path
// rolls the reservation back with zero side effects (Completion untouched,
// slot freed).
//
// Cancel (ADR Decision 11): cancel() resolves the Completion* to its slot
// handle and records the canceled terminal under the arena's leaf domain. The
// Completion stays outstanding; poll()/wait_one() publishes the canceled
// result through the unified reap path.
//
// State is instance-owned only (no globals, gate item 6).
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
#include <optional>
#include <type_traits>
#include <utility>

namespace sluice::async {

class SyncBackend : public AsyncBackend {
  public:
    // request_capacity bounds the arena (ADR Decision 13). The default is large
    // enough for the foundation tests; a caller that needs a different bound
    // constructs explicitly.
    explicit SyncBackend(std::size_t request_capacity = kDefaultCapacity)
        : arena_(detail::ContextIdentity::for_testing(next_backend_id()), request_capacity) {}

    ~SyncBackend() override {
        // No implicit cancel/drain; the context checks outstanding() on destroy.
    }

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
        return submit_size(op, c, detail::OperationKind::read, op.len);
    }
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override {
        return submit_size(op, c, detail::OperationKind::write, op.len);
    }
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override {
        return submit_void(op, c, detail::OperationKind::sync_data);
    }
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override {
        return submit_void(op, c, detail::OperationKind::sync_all);
    }

    // Dispatch (enqueued -> backend_ready for every outstanding slot) then reap.
    // The synthetic terminal is decided at dispatch time: full requested length
    // for read/write, void-success for sync — UNLESS a cancel already won the
    // terminal transition (Scheme B), in which case record_terminal is a no-op
    // and the canceled result is reaped.
    std::size_t poll() override { return dispatch_and_reap(); }

    Result<std::size_t> wait_one() override {
        // No real waiting in 017 (no kernel/threads); just drain like poll().
        return dispatch_and_reap();
    }

    // ADR Decision 11: cancel resolves the Completion* to its slot handle (the
    // arena's bounded scan of the slot records' own bindings — no parallel
    // map) and records the canceled terminal. The Completion stays outstanding;
    // poll()/wait_one() publishes through the unified reap path. Idempotent: a
    // second cancel on an already-terminal slot is a no-op (already_terminal).
    // Cancel on an unknown/already-reaped Completion is a no-op. canceled_ops /
    // completion_errors are tallied at the terminal-winner site (exactly-once;
    // the static publish thunks have no instance state to tally from).
    void cancel(Completion<std::size_t>& c) override {
        auto h = arena_.resolve_completion(&c);
        if (h.has_value()) {
            if (arena_.cancel(*h) == detail::CancelDisposition::requested) {
                tally_canceled();
            }
        }
    }
    void cancel(Completion<void>& c) override {
        auto h = arena_.resolve_completion(&c);
        if (h.has_value()) {
            if (arena_.cancel(*h) == detail::CancelDisposition::requested) {
                tally_canceled();
            }
        }
    }

    std::size_t outstanding() const noexcept override { return arena_.accepted_outstanding(); }

    // Phase B test-only introspection (the arena is a private detail).
    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_capacity_rejections() const noexcept { return arena_.capacity_rejections(); }
    // Test-only (production sink is stateless — CodeRabbit finding / AGENTS §8).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    std::size_t sink_deliveries() const noexcept { return sink_.deliveries(); }
#endif

  private:
    static constexpr std::size_t kDefaultCapacity = 64;

    // Process-wide monotonic id for ContextIdentity provenance. Distinct per
    // SyncBackend instance so two contexts in the same process do not share a
    // domain (ADR Decision 2). for_testing() is the only public ContextIdentity
    // constructor; production construction is internal to RequestArena/context.
    static std::uint64_t next_backend_id() noexcept {
        static std::atomic<std::uint64_t> id{0x51590000u}; // 'Sy' provenance tag
        return ++id;
    }

    // Five-stage admission for a byte-carrying op. The synthetic terminal is
    // NOT recorded here: it is decided at dispatch (poll) time so a cancel
    // between submit and poll can still win the terminal transition (Scheme B).
    // The requested length is carried in the slot's publication binding for
    // the dispatch step. Transactional pre-commit path (review C1): the
    // publication binding is installed INTO the slot record (no map insert,
    // no allocation) and every pre-commit failure rolls the reservation back
    // with zero side effects; a lost Completion CAS rolls back only this
    // submit's slot. Nothing after commit_binding may throw (I9).
    template <class Op>
    Result<void> submit_size(Op op, Completion<std::size_t>& c, detail::OperationKind kind,
                             std::size_t len) {
        // Stage 1: reserve. Arena full -> would_block; admission closed ->
        // invalid_state (ADR Decision 6/13: capacity pressure is NEVER
        // invalid_state).
        auto rh = arena_.reserve();
        if (!rh.has_value()) {
            return make_unexpected<void>(rh.error());
        }
        detail::SlotHandle h = rh.value();
        // Stage 2: prepare (op kind + fd/buffer borrow metadata).
        auto ph = arena_.prepare(h, kind, borrow_of(op));
        if (!ph.has_value()) {
            (void)arena_.rollback_reserved_or_prepared(h); // roll back reservation
            return make_unexpected<void>(ph.error());
        }
        // Stage 2.5: install the slot's Completion publication binding (review
        // C2 — the slot is the identity carrier; reap publishes through it
        // inside the leaf domain). A later CAS loss rolls the binding back
        // with the slot.
        auto bh = arena_.install_publication_binding(h, &c, len, &publish_size_ready);
        if (!bh.has_value()) {
            (void)arena_.rollback_reserved_or_prepared(h);
            return make_unexpected<void>(bh.error());
        }
        // Stage 3a: Completion CAS idle -> binding elects ONE submitter. Loser:
        // roll back only our own slot + binding (zero side effects).
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
        // Stage 3c: install the slot-release capability, then publish
        // outstanding (submit-success LP). AFTER this nothing may throw.
        install_binding(c, &arena_, h);
        commit_binding(c);
        // Stage 4: enqueue (pending -> enqueued OR terminal no-op). noexcept.
        (void)arena_.enqueue(h);
        return {};
    }

    template <class Op>
    Result<void> submit_void(Op op, Completion<void>& c, detail::OperationKind kind) {
        auto rh = arena_.reserve();
        if (!rh.has_value()) {
            return make_unexpected<void>(rh.error());
        }
        detail::SlotHandle h = rh.value();
        auto ph = arena_.prepare(h, kind, detail::BorrowMetadata{op.fd, nullptr, 0});
        if (!ph.has_value()) {
            (void)arena_.rollback_reserved_or_prepared(h);
            return make_unexpected<void>(ph.error());
        }
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
        (void)arena_.enqueue(h);
        return {};
    }

    // fd/buffer borrow metadata for a byte-carrying op (ADR Decision 3/8).
    template <class Op>
    static detail::BorrowMetadata borrow_of(const Op& op) {
        if constexpr (std::is_same_v<Op, ReadOp>) {
            return {op.fd, op.dst, op.len};
        } else {
            return {op.fd, op.src, op.len};
        }
    }

    // Dispatch: every enqueued slot that does not yet have a terminal result
    // gets its synthetic terminal recorded (full length / void-success). A
    // cancel that already won the terminal transition (Scheme B) made
    // record_terminal a no-op on that slot — the canceled result is reaped.
    // Iterates the fixed slot array via the arena's read-only accessors (the
    // slot's own binding carries the dispatch data — no parallel map).
    //
    // Snapshot consistency (CodeRabbit finding): the composed handle
    // {idx, generation_of(idx)} is built from independent locked snapshots, but
    // record_terminal re-validates the generation under its OWN lock before
    // writing — so if a release_completed_binding (run outside access_mtx_)
    // advanced the generation between the snapshot and the record, record_terminal
    // returns false (no write, no corruption) and the next poll re-dispatches the
    // still-enqueued slot. The benign skip is the authority guarantee; the
    // Phase B reference backend is also single-threaded under access_mtx_, so the
    // window does not arise until the multi-threaded backends of later phases
    // (which will use a single arena-locked dispatch scan, not this composed-
    // snapshot path).
    void dispatch_enqueued() {
        for (std::size_t i = 0; i < arena_.capacity(); ++i) {
            detail::SlotIndex idx{static_cast<std::uint32_t>(i)};
            if (arena_.state_of(idx) != detail::RequestState::enqueued) continue;
            detail::SlotHandle h{idx, arena_.generation_of(idx)};
            detail::OperationKind kind = arena_.kind_of(idx);
            detail::TerminalResult res =
                (kind == detail::OperationKind::read || kind == detail::OperationKind::write)
                    ? detail::TerminalResult::ok_bytes(arena_.requested_bytes_of(idx))
                    : detail::TerminalResult::ok_void();
            (void)arena_.record_terminal(h, res);
        }
    }

    std::size_t dispatch_and_reap() {
        dispatch_enqueued();
        return arena_.reap(sink_);
    }

    // --- Completion publication (review C2/C3) ---
    // The arena publishes Completion-ready through the slot-bound thunk INSIDE
    // the leaf domain. The thunks are written here (a trusted backend-author —
    // they reach the protected AsyncBackend::publish helpers) and installed
    // into the slot at submit time via install_publication_binding. They are
    // static + type-erased: the arena never dereferences the Completion pointer
    // itself, and the thunk does not touch the backend (no lock, no allocation).
    static void publish_size_ready(void* completion,
                                   const detail::TerminalResult& t) noexcept {
        AsyncBackend::publish(*static_cast<Completion<std::size_t>*>(completion),
                              terminal_to_size(t));
    }
    static void publish_void_ready(void* completion,
                                   const detail::TerminalResult& t) noexcept {
        AsyncBackend::publish(*static_cast<Completion<void>*>(completion),
                              terminal_to_void(t));
    }

    static Result<std::size_t> terminal_to_size(const detail::TerminalResult& t) noexcept {
        if (t.stored && t.is_error)
            return make_unexpected<std::size_t>(t.error);
        return Result<std::size_t>{static_cast<std::size_t>(t.bytes)};
    }
    static Result<void> terminal_to_void(const detail::TerminalResult& t) noexcept {
        if (t.stored && t.is_error)
            return make_unexpected<void>(t.error);
        return {};
    }

    // Stats tally at the TERMINAL-WINNER site (exactly-once: cancel returns
    // `requested` only when it stored the canceled terminal; losers never
    // tally). The tally was previously done at reap publication; both are
    // exactly-once for an accepted op, and only the winner site is reachable
    // from the static publish thunks (which have no instance state).
    void tally_canceled() noexcept {
        if (stats_) ++stats_->canceled_ops;
    }

    detail::RequestArena arena_;
    detail::ReferenceReadySink sink_;
};

} // namespace sluice::async
