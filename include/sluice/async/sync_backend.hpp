// sluice::async default backend (ADR §3/§4).
//
// SyncBackend completes ops SYNCHRONOUSLY at the next poll()/wait_one(). It
// holds no kernel state and uses no threads — it is the minimal in-process
// backend that lets the async foundation compile, link, and be tested ahead
// of the real backends (FakeAsyncBackend, ThreadPoolBackend, UringAsyncBackend).
//
// Semantics: every submitted op is buffered; poll()/wait_one() marks all
// of them ready with a synthetic result. ReadOps complete with their full `len`
// (no actual read — this backend touches no fd); WriteOps complete with `len`;
// sync ops complete with void. This is enough to test the Completion lifecycle,
// submit/poll/wait plumbing, and AsyncStats. It is NOT a correctness backend
// for real I/O — FakeAsyncBackend and ThreadPoolBackend provide that.
//
// SyncBackend drives the bounded RequestArena five-stage admission
// (reserve -> prepare -> commit -> enqueue -> dispatch/reap) under
// ADR-explicit-io-request-contract (Accepted) and the unified reap path with a
// synchronous identity-bearing ReadySink. The public submit_*/poll/wait_one/cancel
// surface is unchanged (ADR Decision 7); the RequestKey is bound privately during
// commit and resolved internally for cancel. The synthetic terminal result is
// stored at dispatch time (record_terminal) so poll deterministically
// transitions pending/enqueued -> backend_ready; poll()/wait_one() reaps.
//
// Identity: the Completion publication binding lives IN the
// RequestSlot record (install_publication_binding before the Completion CAS);
// reap validates it and publishes Completion-ready through it inside the leaf
// domain. There is NO parallel unordered_map identity bridge — cancel resolves
// a Completion* by the arena's bounded O(capacity) scan. Pre-commit
// bookkeeping is transactional: every pre-commit failure path
// rolls the reservation back with zero side effects (Completion untouched,
// slot freed).
//
// Cancel (ADR Decision 11): cancel() resolves the Completion* to its slot
// handle and records the canceled terminal under the arena's leaf domain. The
// Completion stays outstanding; poll()/wait_one() publishes the canceled
// result through the unified reap path.
//
// State is instance-owned only (no globals).
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/reference_ready_sink.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/async/detail/submit_transaction.hpp>
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
        return submit_size(op, c, detail::OperationKind::read);
    }
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override {
        return submit_size(op, c, detail::OperationKind::write);
    }
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override {
        return submit_void(op, c, detail::OperationKind::sync_data);
    }
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override {
        return submit_void(op, c, detail::OperationKind::sync_all);
    }

    // ADR-public-request-handle: this backend uses the RequestArena
    // identity contract, so it produces and resolves public RequestHandles.
    bool supports_request_identity() const noexcept override { return true; }

  private:
    // Sealed override of the private virtual in AsyncBackend: reached only via
    // AsyncIoContext::request_state -> AsyncBackend::request_handle_state. A raw
    // backend pointer must not expose the raw identity-tuple consumer.
    Result<RequestHandleState> resolve_identity_state(std::uint64_t ctx, std::uint32_t slot,
                                                      std::uint64_t gen) const override {
        return arena_.identity_handle_state(detail::SlotIndex{slot},
                                            detail::Generation{gen},
                                            detail::ContextIdentity{ctx});
    }

  public:

    // Dispatch (enqueued -> backend_ready for every outstanding slot) then reap.
    // The synthetic terminal is decided at dispatch time: full requested length
    // for read/write, void-success for sync — UNLESS a cancel already won the
    // terminal transition (Scheme B), in which case record_terminal is a no-op
    // and the canceled result is reaped.
    std::size_t poll() override { return dispatch_and_reap(); }

    Result<std::size_t> wait_one() override {
        // No real waiting (no kernel/threads); just drain like poll().
        return dispatch_and_reap();
    }

    // SyncBackend intentionally has NO split wait capability. Its
    // wait_one is NON-BLOCKING by contract (poll drains every outstanding slot,
    // so a wait never needs to park). It advertises that non-blocking contract
    // so ApplicationRuntime accepts it without a wait source.
    bool wait_one_is_nonblocking() const noexcept override { return true; }

    // ADR Decision 11: cancel resolves the Completion* to its slot handle (the
    // arena's bounded scan of the slot records' own bindings — no parallel
    // map) and records the canceled terminal. The Completion stays outstanding;
    // poll()/wait_one() publishes through the unified reap path. Idempotent: a
    // second cancel on an already-terminal slot is a no-op (already_terminal).
    // Cancel on an unknown/already-reaped Completion is a no-op.
    // canceled_ops / completion_errors are tallied ONLY when cancel wins the
    // terminal transition (terminal_won — exactly-once; the static publish
    // thunks have no instance state to tally from). A running-slot cancel
    // (intent_recorded) does NOT tally here: best-effort intent does not promise
    // a canceled terminal (ADR Decision 11); the tally happens at the confirmed
    // canceled terminal winner.
    void cancel(Completion<std::size_t>& c) override {
        auto h = arena_.resolve_completion(&c);
        if (h.has_value()) {
            if (arena_.cancel(*h) == detail::CancelDisposition::terminal_won) {
                tally_canceled();
            }
        }
    }
    void cancel(Completion<void>& c) override {
        auto h = arena_.resolve_completion(&c);
        if (h.has_value()) {
            if (arena_.cancel(*h) == detail::CancelDisposition::terminal_won) {
                tally_canceled();
            }
        }
    }

    // Production waiter registration / cancellation
    // (ADR Decision 10), forwarded verbatim to the REAL arena authorities
    // through the same resolve_completion identity bridge as cancel. No
    // side-band waiter map. register_waiter: success, or invalid_state for a
    // second registration / a non-accepted-or-already-reaped slot; not_found
    // for an unresolvable (unbound, cross-context, stale) Completion.
    // cancel_waiter: removes ONLY the waiter (never the I/O, never the borrow)
    // and returns the moved-out lease, or not_found when reap already closed
    // the registration.
    Result<void> register_waiter(Completion<std::size_t>& c,
                                 detail::WaiterToken token,
                                 detail::RoutingLease lease) override {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::not_found});
        }
        return arena_.register_waiter(*h, token, std::move(lease));
    }
    Result<void> register_waiter(Completion<void>& c,
                                 detail::WaiterToken token,
                                 detail::RoutingLease lease) override {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::not_found});
        }
        return arena_.register_waiter(*h, token, std::move(lease));
    }
    Result<detail::RoutingLease> cancel_waiter(Completion<std::size_t>& c) override {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<detail::RoutingLease>(
                IoError{IoError::Code::not_found});
        }
        return arena_.cancel_waiter(*h);
    }
    Result<detail::RoutingLease> cancel_waiter(Completion<void>& c) override {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<detail::RoutingLease>(
                IoError{IoError::Code::not_found});
        }
        return arena_.cancel_waiter(*h);
    }

    std::size_t outstanding() const noexcept override { return arena_.accepted_outstanding(); }

    // Test-only introspection (the arena is a private detail).
    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_capacity_rejections() const noexcept { return arena_.capacity_rejections(); }
    // Test-only (production sink is stateless — AGENTS §8).
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

    // Five-stage admission for a byte-carrying op: ONE call into the
    // shared pre-accept ladder (detail::submit_transaction). The
    // synthetic terminal is NOT recorded here: it is decided at dispatch
    // (poll) time so a cancel between submit and poll can still win the
    // terminal transition (Scheme B). The requested length is carried in the
    // slot's publication binding for the dispatch step. Transactional
    // pre-commit path: the publication binding is installed INTO
    // the slot record (no map insert, no allocation) and every pre-commit
    // failure rolls the reservation back with zero side effects; a lost
    // Completion CAS rolls back only this submit's slot. Nothing after
    // commit_binding may throw. Admission serialization is EXTERNAL
    // (the context's access_mtx_); this backend has no admission lock of its
    // own, so the wrapper is lock-free — the ladder runs as-is.
    template <class Op>
    Result<void> submit_size(Op op, Completion<std::size_t>& c, detail::OperationKind kind) {
        SubmitPolicy<Op, Completion<std::size_t>> policy{kind};
        auto r = detail::submit_transaction(arena_, c, op, policy);
        if (!r.has_value()) {
            return make_unexpected<void>(r.error());
        }
        // Stage 4: enqueue (pending -> enqueued OR terminal no-op). noexcept.
        (void)arena_.enqueue(r.value());
        return {};
    }

    template <class Op>
    Result<void> submit_void(Op op, Completion<void>& c, detail::OperationKind kind) {
        SubmitPolicy<Op, Completion<void>> policy{kind};
        auto r = detail::submit_transaction(arena_, c, op, policy);
        if (!r.has_value()) {
            return make_unexpected<void>(r.error());
        }
        (void)arena_.enqueue(r.value());
        return {};
    }

    // The backend's policy for detail::submit_transaction: the
    // reference backend's divergence set is empty by construction — no
    // Stage-0 gate (admission is serialized externally by the context's
    // access_mtx_; the arena's own reserve check arbitrates admission), no
    // descriptor validation (DIV-14: the fd is a metadata carrier here, not
    // a syscall target — the deferred-validation divergence lives in this
    // trivial hook), no prepared-op scratch (the synthetic terminal is
    // decided at dispatch from the slot binding), no deterministic pause
    // seam. Every production hook is the trivial one; this struct is the
    // visible, greppable declaration of that divergence.
    template <class Op, class Comp>
    struct SubmitPolicy {
        using completion_type = Comp;
        using op_type = Op;

        explicit SubmitPolicy(detail::OperationKind kind) noexcept : kind_(kind) {}

        // --- data accessors ---
        detail::OperationKind kind() const noexcept { return kind_; }
        static detail::BorrowMetadata borrow(const Op& op) noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                return borrow_of(op);
            } else {
                return detail::BorrowMetadata{op.fd, nullptr, 0};
            }
        }
        static std::uint64_t requested_bytes(const Op& op) noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                return op.len;
            } else {
                return 0;
            }
        }
        static auto publish_thunk() noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                return &SyncBackend::publish_size_ready;
            } else {
                return &SyncBackend::publish_void_ready;
            }
        }
        // --- binding trio (protected AsyncBackend statics, reached through
        // the enclosing backend — the trusted backend-author role) ---
        static bool begin_binding(Comp& c) noexcept {
            return SyncBackend::begin_binding(c);
        }
        static void install_binding(Comp& c, detail::RequestArena* arena,
                                    detail::SlotHandle h) noexcept {
            SyncBackend::install_binding(c, arena, h);
        }
        static void commit_binding(Comp& c) noexcept {
            SyncBackend::commit_binding(c);
        }
        static void rollback_binding(Comp& c) noexcept {
            SyncBackend::rollback_binding_before_accept(c);
        }
        // --- production hooks (all trivial; see the struct comment) ---
        Result<void> stage0_precheck() const noexcept { return {}; }
        Result<void> validate(const Op&) const noexcept { return {}; }
        void write_scratch(detail::SlotHandle, const Op&) const noexcept {}
        void pause_before_commit_binding() const noexcept {}
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        // The reference backend carries no C2d injection harness.
        std::optional<IoError> injected_precommit_stage_failure(
            detail::SubmitStage) const noexcept {
            return std::nullopt;
        }
#endif

      private:
        detail::OperationKind kind_;
    };

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
    // Snapshot consistency: the composed handle
    // {idx, generation_of(idx)} is built from independent locked snapshots, but
    // record_terminal re-validates the generation under its OWN lock before
    // writing — so if a release_completed_binding (run outside access_mtx_)
    // advanced the generation between the snapshot and the record, record_terminal
    // returns false (no write, no corruption) and the next poll re-dispatches the
    // still-enqueued slot. The benign skip is the authority guarantee; the
    // reference backend is also single-threaded under access_mtx_, so the
    // window does not arise for it (the multi-threaded backends use a single
    // arena-locked dispatch scan, not this composed-snapshot path).
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
        // Deliver identity events to the attached Scheduler-owned
        // routing sink when one is set; otherwise the no-op reference sink.
        return arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
    }

    // --- Completion publication ---
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
