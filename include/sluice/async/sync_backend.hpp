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
// stored at submit time (record_terminal) so dispatch deterministically
// transitions pending/enqueued -> backend_ready; poll()/wait_one() reaps.
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
#include <unordered_map>
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

    // ADR Decision 11: cancel resolves the Completion* to its slot handle and
    // records the canceled terminal. The Completion stays outstanding; poll()/
    // wait_one() publishes through the unified reap path. Idempotent: a second
    // cancel on an already-terminal slot is a no-op (already_terminal). Cancel
    // on an unknown/already-reaped Completion is a no-op. canceled_ops /
    // completion_errors are tallied at reap (the publish path) so the canonical
    // accounting is single-sourced.
    void cancel(Completion<std::size_t>& c) override {
        auto h = lookup(&c);
        if (h.has_value())
            (void)arena_.cancel(*h);
    }
    void cancel(Completion<void>& c) override {
        auto h = lookup(&c);
        if (h.has_value())
            (void)arena_.cancel(*h);
    }

    std::size_t outstanding() const noexcept override { return arena_.accepted_outstanding(); }

    // Phase B test-only introspection (the arena is a private detail).
    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_capacity_rejections() const noexcept { return arena_.capacity_rejections(); }
    std::size_t sink_deliveries() const noexcept { return sink_.deliveries(); }

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
    // The requested length is carried in the binding for the dispatch step.
    // Bind/FIFO bookkeeping happens BEFORE the Completion CAS (I9 / ADR
    // Decision 14): nothing allocates after the submit-success LP.
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
            (void)arena_.release(h); // roll back reservation
            return make_unexpected<void>(ph.error());
        }
        // Pre-CAS bookkeeping (may allocate; failure rolls back cleanly).
        bind(h, &c, len);
        // Stage 3a: Completion CAS idle -> binding elects ONE submitter. Loser:
        // roll back only our own slot + binding entry.
        if (!begin_binding(c)) {
            bindings_.erase(h.slot.value);
            (void)arena_.release(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        // Stage 3b: commit (pending + pin + accepted++ + borrow begins).
        auto ch = arena_.commit(h);
        if (!ch.has_value()) {
            rollback_binding_before_accept(c);
            bindings_.erase(h.slot.value);
            (void)arena_.release(h);
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
            (void)arena_.release(h);
            return make_unexpected<void>(ph.error());
        }
        bind(h, &c);
        if (!begin_binding(c)) {
            bindings_.erase(h.slot.value);
            (void)arena_.release(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        auto ch = arena_.commit(h);
        if (!ch.has_value()) {
            rollback_binding_before_accept(c);
            bindings_.erase(h.slot.value);
            (void)arena_.release(h);
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
    // This is the dispatch step (design §5: "Fake/Sync deterministically
    // transition to backend_ready but do NOT make Completion ready inline").
    void dispatch_enqueued() {
        for (auto& [idx, b] : bindings_) {
            detail::SlotHandle h{detail::SlotIndex{idx}, b.generation};
            auto st = arena_.state_of(h.slot);
            if (st == detail::RequestState::enqueued) {
                if (b.size_completion) {
                    (void)arena_.record_terminal(
                        h, detail::TerminalResult::ok_bytes(b.requested_bytes));
                } else {
                    (void)arena_.record_terminal(h, detail::TerminalResult::ok_void());
                }
            }
        }
    }

    std::size_t dispatch_and_reap() {
        dispatch_enqueued();
        return arena_.reap(sink_, [this](const detail::RequestArena::ReapPublication& p) {
            publish_reaped(p);
        });
    }

    // Resolve the reaped slot back to its Completion* and publish the terminal
    // result. The SLOT IS NOT RELEASED HERE: it stays bound (slot_in_use == 1)
    // until the caller resets or destroys the ready Completion (ADR Decision
    // 4/15; design §9 — the caller's completion_ready -> free handshake via the
    // release capability installed at commit). The binding is erased before
    // publish so the Completion pointer is not retained across user-observable
    // ready.
    void publish_reaped(const detail::RequestArena::ReapPublication& p) {
        auto it = bindings_.find(p.handle.slot.value);
        if (it == bindings_.end())
            return; // defensive: unknown slot
        detail::SlotHandle bh{p.handle.slot, p.handle.generation};
        if (it->second.generation.value != bh.generation.value) {
            // The binding's generation does not match the publication; the slot
            // was reused. Defensive only — the binding entry is erased in the
            // same publication that carries the slot's generation, and a slot
            // cannot be released (generation bump) before its Completion is
            // published ready, so this cannot occur.
            return;
        }
        Completion<std::size_t>* sc = it->second.size_completion;
        Completion<void>* vc = it->second.void_completion;
        bindings_.erase(it); // drop the pointer before publish
        if (sc) {
            publish(*sc, terminal_to_size(p.terminal));
            if (stats_) {
                if (p.terminal.stored && p.terminal.is_error &&
                    p.terminal.error.code == IoError::Code::canceled) {
                    ++stats_->canceled_ops;
                } else if (p.terminal.stored && p.terminal.is_error) {
                    ++stats_->completion_errors;
                }
            }
        } else if (vc) {
            publish(*vc, terminal_to_void(p.terminal));
            if (stats_) {
                if (p.terminal.stored && p.terminal.is_error &&
                    p.terminal.error.code == IoError::Code::canceled) {
                    ++stats_->canceled_ops;
                } else if (p.terminal.stored && p.terminal.is_error) {
                    ++stats_->completion_errors;
                }
            }
        }
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

    // Phase B pointer-keyed bridge: map slot index -> (generation, Completion*,
    // requested_bytes). This is the bridge until the public RequestHandle lands
    // (later ADR); cancel/complete remain pointer-keyed in the public API (ADR
    // Decision 7). requested_bytes carries the synthetic full-length result for
    // the dispatch step.
    struct Binding {
        detail::Generation generation{};
        Completion<std::size_t>* size_completion = nullptr;
        Completion<void>* void_completion = nullptr;
        std::size_t requested_bytes = 0;
    };
    void bind(detail::SlotHandle h, Completion<std::size_t>* c, std::size_t len) {
        bindings_[h.slot.value] = {h.generation, c, nullptr, len};
    }
    void bind(detail::SlotHandle h, Completion<void>* c) {
        bindings_[h.slot.value] = {h.generation, nullptr, c, 0};
    }
    std::optional<detail::SlotHandle> lookup(Completion<std::size_t>* c) const {
        for (const auto& [idx, b] : bindings_) {
            if (b.size_completion == c) {
                return detail::SlotHandle{detail::SlotIndex{idx}, b.generation};
            }
        }
        return std::nullopt;
    }
    std::optional<detail::SlotHandle> lookup(Completion<void>* c) const {
        for (const auto& [idx, b] : bindings_) {
            if (b.void_completion == c) {
                return detail::SlotHandle{detail::SlotIndex{idx}, b.generation};
            }
        }
        return std::nullopt;
    }

    detail::RequestArena arena_;
    detail::ReferenceReadySink sink_;
    std::unordered_map<std::uint32_t, Binding> bindings_;
};

} // namespace sluice::async
