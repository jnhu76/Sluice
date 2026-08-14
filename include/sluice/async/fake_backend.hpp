// sluice::async::FakeAsyncBackend (sluice-CORE-019, ADR §4/§10 T1).
//
// A deterministic async backend for tests: ops submitted are held outstanding
// across poll() calls UNTIL THE TEST EXPLICITLY COMPLETES THEM. No kernel, no
// threads. This is the primary unit-test vehicle for all later async work
// (018/018B/021) and the thing that makes the buffer-lifetime contract (gate
// item 1) genuinely testable.
//
// Completion model (PR #63 review findings #1, #2 — terminal evidence binds to a
// stable RequestKey at completion time, NOT at poll time):
//   - submit_* records the op (no completion produced).
//   - The test calls one of the complete_*() helpers to resolve a terminal
//     result for the OLDEST outstanding op of a given kind. The result is bound
//     IMMEDIATELY to that op's RequestKey via arena_.record_terminal(): the slot
//     becomes backend_ready and the ready-ring records the terminal-winner order
//     (ADR Decision 9 / Decision 12). There is NO staging deque and NO side-band
//     HandleRing — a second complete_*() against an already-terminal op is a
//     record_terminal no-op (terminal-winner rule), so terminal evidence can
//     never leak across generations or strand a later accepted op.
//   - poll()/wait_one() then REAPS: publishes Completion-ready through the
//     slot's own binding, in ready-ring (backend-known) order. This keeps the
//     "completions only inside poll/wait_one" rule (ADR A3/O1) even on the fake.
//
// Error / short-completion injection:
//   - complete_oldest_with_error(IoError) — surface any error (eof/no_space/
//     backend_error/canceled) on the next poll (ADR E2/E3).
//   - complete_oldest_with_bytes(n) — surface a (possibly short) byte count for
//     a read/write op; n < requested is a short completion (exercises 018 retry).
//
// Phase B (ADR-explicit-io-request-contract, Accepted): FakeAsyncBackend now
// drives the bounded RequestArena five-stage admission (reserve -> prepare ->
// commit -> enqueue -> dispatch/reap) and the unified reap path with a
// synchronous identity-bearing ReadySink. The public submit_*/poll/wait_one/
// cancel/complete_* surface is unchanged (ADR Decision 7); the RequestKey is
// bound privately during commit and resolved internally for cancel/complete.
// Ops are held in the `enqueued` slot state with NO terminal recorded until the
// test completes them (or auto-mode fires); this preserves the "held
// outstanding until explicitly completed" contract. Cancel records the canceled
// terminal directly under Scheme B (pending/enqueued cancel wins).
//
// Identity (review C2): the Completion publication binding lives IN the
// RequestSlot record (install_publication_binding before the Completion CAS);
// reap validates it and publishes Completion-ready through it inside the leaf
// domain. There is NO parallel unordered_map identity bridge — cancel resolves
// a Completion* by the arena's bounded O(capacity) scan. Pre-commit
// bookkeeping is transactional (review C1): the publication binding is
// installed into the slot record (no map insert), every pre-commit failure
// path rolls the reservation back with zero side effects (Completion untouched,
// slot freed), and there is no FIFO ring to leave residue (review finding #1
// removed the side-band HandleRing entirely).
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

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include <thread>
#endif

namespace sluice::async {

class FakeAsyncBackend : public AsyncBackend {
  public:
    explicit FakeAsyncBackend(std::size_t request_capacity = kDefaultCapacity)
        : arena_(detail::ContextIdentity::for_testing(next_backend_id()), request_capacity) {}
    ~FakeAsyncBackend() override = default;

    // --- auto-complete mode ---
    // When set, poll() auto-completes each outstanding op with `auto_bytes_`
    // (read/write) or void-success (sync), WITHOUT the test completing anything.
    // This lets the synchronous read_all/write_all coordinators (job 018) drive
    // the fake in a poll-loop, since they submit+poll internally and cannot have
    // the test complete results between their loop steps.
    //   auto_bytes(n)         each outstanding op completes with n bytes
    //                         (n may be < requested => short, exercises retry)
    //   auto_short_then_full(first, rest)
    //                         the FIRST outstanding op completes short (first),
    //                         subsequent ones complete their full remaining
    //                         length; models one short then clean completion.
    //   auto_error(e)         each outstanding op completes with error e
    //   auto_eof()            read completes with 0 bytes (EOF) — shortcut for
    //                         auto_bytes(0).
    //   auto_disable()        stop auto-completing (resume explicit completion).
    void auto_bytes(std::size_t n) {
        auto_mode_ = Auto::bytes;
        auto_bytes_ = n;
    }
    void auto_error(IoError e) {
        auto_mode_ = Auto::err;
        auto_err_ = e;
    }
    void auto_eof() { auto_bytes(0); }
    void auto_disable() { auto_mode_ = Auto::off; }
    void auto_short_then_full(std::size_t first_short) {
        auto_mode_ = Auto::short_then_full;
        auto_bytes_ = first_short;
        auto_short_used_ = false;
    }

    // --- submit: record outstanding, produce no completion ---
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

    // Phase F3 (ADR-public-request-handle): this backend uses the RequestArena
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

    // --- test-driving helpers: resolve a terminal result for the OLDEST
    // outstanding op of the matching kind. The result is bound IMMEDIATELY to
    // that op's RequestKey (arena_.record_terminal); the Completion is published
    // at the next poll()/wait_one(). ---
    //
    // ADR Decision 12 + review finding #2: terminal evidence attaches to a
    // stable RequestKey at completion-call time, not into an implicit per-kind
    // queue consumed later by guessing. A second call against an op that is
    // already terminal (cancel won, or a prior complete_*) is a record_terminal
    // no-op — the terminal-winner rule prevents any cross-generation pollution.
    // No-op when no enqueued op of the kind is outstanding.

    // Resolve a byte-count result for the oldest outstanding read/write op
    // (n < requested => short completion). No-op if none outstanding. The
    // Completion is published at the next poll() when the slot is reaped.
    void complete_oldest_with_bytes(std::size_t n) {
        resolve_size_terminal(detail::TerminalResult::ok_bytes(n));
    }
    // Resolve an error result for the oldest outstanding read/write op.
    void complete_oldest_with_error(IoError e) {
        resolve_size_terminal(detail::TerminalResult::err(e));
    }
    // Resolve a void success for the oldest outstanding sync op.
    void complete_oldest_sync_ok() {
        resolve_void_terminal(detail::TerminalResult::ok_void());
    }
    // Resolve a void error for the oldest outstanding sync op.
    void complete_oldest_sync_error(IoError e) {
        resolve_void_terminal(detail::TerminalResult::err(e));
    }

    // --- reap: apply auto/canceled results then publish Completions. Auto-mode
    // drains every enqueued slot; otherwise the complete_*/cancel calls already
    // recorded their terminals. poll()/wait_one() then reaps the ready-ring in
    // terminal-winner order (ADR Decision 9). ---
    std::size_t poll() override { return dispatch_and_reap(); }

    Result<std::size_t> wait_one() override {
        // No real waiting (no kernel/threads); just poll. Tests drive timing.
        return dispatch_and_reap();
    }

    // Issue #67: FakeAsyncBackend intentionally has NO split wait capability.
    // Its wait_one is NON-BLOCKING by contract (E7 no-progress boundary: an
    // un-staged op returns 0 immediately so a Drain-mode coordinated run can
    // terminate instead of parking on a completion that only an external
    // complete_* call can produce). It advertises that non-blocking contract
    // so ApplicationRuntime accepts it without a wait source (D3).
    bool wait_one_is_nonblocking() const noexcept override { return true; }

    // Phase F1 (issue #98): production waiter registration / cancellation
    // (ADR Decision 10). The Completion is resolved to its current SlotHandle
    // by the arena's own bounded scan — the same identity bridge the public
    // cancel path uses — and the call is forwarded verbatim to the REAL arena
    // authorities. No side-band waiter map, no reimplementation of the waiter
    // state machine. register_waiter: success, or invalid_state for a second
    // registration / a non-accepted-or-already-reaped slot; not_found for an
    // unresolvable (unbound, cross-context, stale) Completion. cancel_waiter:
    // removes ONLY the waiter (never the I/O, never the borrow) and returns
    // the moved-out lease, or not_found when reap already closed the
    // registration.
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

    // Minimal cancel (ADR §7 X2): REQUESTS cancel. The op stays outstanding and
    // is completed (exactly-once, X3) at the next poll()/wait_one() with
    // IoError::canceled. We do NOT complete here — A3/O1: completions are
    // produced only inside poll/wait_one. Cancel is POINTER-KEYED (targeted) so
    // it works on any outstanding op, not just the oldest.
    // Phase B (ADR Decision 11, review round-4 finding 1): resolves Completion*
    // -> SlotHandle via the arena's bounded slot scan (the slot's own binding is
    // the identity — no parallel map), then arena.cancel() acts per state:
    //   - terminal_won    — cancel won the terminal transition under Scheme B
    //                       (pending/enqueued -> backend_ready(canceled)). This
    //                       is the confirmed canceled winner; tally canceled_ops
    //                       here (exactly-once; reap publishes the stored result).
    //   - intent_recorded — running: cancel recorded INTENT only (best-effort).
    //                       No terminal is stored, so canceled_ops is NOT tallied
    //                       here; a backend that later CONFIRMS the cancellation
    //                       (a valid interruption / cancel CQE winner) records
    //                       TerminalResult::err(canceled) and tallies there. The
    //                       Phase B reference backends never enter `running`, so
    //                       this branch is dormant here.
    //   - already_terminal / not_found — no-op (losers never overwrite).
    // The Completion stays outstanding; poll/wait_one publishes through reap.
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

    std::size_t outstanding() const noexcept override { return arena_.accepted_outstanding(); }

    // ADR Decision 15 (reference semantics): close admission. New reserve()
    // returns invalid_state (Completion idle, no borrow) while existing accepted
    // requests continue; cancel/poll/wait_one/reap remain legal. Idempotent.
    // Takes the backend admission transaction lock (ADR §"Commit / accept"
    // :453-462 — the winning submit retains its context/admission lock through
    // the Step 5 `binding -> outstanding` release-store, the commit/accept
    // linearization point), so after this returns no new acceptance LP can
    // occur (Decision 15).
    // FakeAsyncBackend has NO split wait capability (its wait_one is
    // non-blocking by contract), so there is no parked participant to wake —
    // the arena admission flag alone is the full reference semantics, and the
    // shared close/drain suite (C2e) drives this identically for Fake and
    // ThreadPool. Mirrors ThreadPoolBackend::close_admission. Not noexcept:
    // acquiring admission_mtx_ (lock_guard) may throw std::system_error, and
    // the ThreadPool mirror carries no noexcept either.
    void close_admission() {
        std::lock_guard<std::mutex> lk(admission_mtx_);
        arena_.close_admission();
    }

    // Phase B test-only introspection (the arena is a private detail).
    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_capacity_rejections() const noexcept { return arena_.capacity_rejections(); }
    // Test-only (the production sink is stateless; the delivery counter exists
    // only under SLUICE_ASYNC_INTERNAL_TESTING — CodeRabbit finding / AGENTS §8).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    std::size_t sink_deliveries() const noexcept { return sink_.deliveries(); }
#endif
    bool arena_enqueue_pin_live(std::uint32_t slot) const noexcept {
        return arena_.enqueue_pin_live(detail::SlotIndex{slot});
    }
    bool arena_state_is(std::uint32_t slot, detail::RequestState st) const noexcept {
        return arena_.state_of(detail::SlotIndex{slot}) == st;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic causal seam (Phase B / review test-gap 1): pause the submit
    // path between commit and enqueue so a backend-level test can interleave
    // cancel exactly in the Scheme-B window (the window AsyncIoContext::
    // access_mtx_ serialization hides). Test-only: production builds of this
    // header (no macro) carry no field and no pause; the layout cost is
    // accepted and documented (AGENTS.md §8 — internal-testing variants may
    // carry guarded seams).
    struct SubmitPauseGate {
        std::atomic<bool> paused{false};  // the submit path set this when paused
        std::atomic<bool> resume{false};  // the test sets this to resume
    };
    void set_submit_pause_after_commit(SubmitPauseGate* gate) noexcept {
        submit_pause_gate_ = gate;
    }

    // Test-only: resolve a Completion pointer to its current slot+generation.
    // Returns nullopt if the Completion is not bound to any slot. Mirrors the
    // ThreadPoolBackend seam (Phase C2b row 4 identity tests capture a handle,
    // release the slot, reuse it, then inject the stale handle).
    std::optional<detail::SlotHandle> handle_for_completion_for_test(
        const void* completion) const noexcept {
        return arena_.resolve_completion(completion);
    }

    // Test-only identity-injection seam (Phase C2b row 4): drive a CAPTURED
    // SlotHandle (typically a stale-generation handle from a released occupant)
    // through the REAL cancel authority path — arena_.cancel(h) — instead of
    // the pointer-keyed public cancel(Completion&). This is what proves a
    // stale-generation event cannot act on a live N+1 occupant of the same
    // physical slot: the handle validation in arena_.cancel rejects it with
    // not_found and no side effect. Returns the disposition so the test can
    // assert not_found/already_terminal. Mirrors observe_for_test (test-only,
    // guarded; production builds carry nothing). The seam accepts/returns
    // pointer-free identity (SlotHandle), not a Completion reverse map.
    detail::CancelDisposition cancel_handle_for_test(detail::SlotHandle h) noexcept {
        detail::CancelDisposition disp = arena_.cancel(h);
        if (disp == detail::CancelDisposition::terminal_won) {
            tally_canceled();
        }
        return disp;
    }

    // --- Phase C2c seams (rows 11-14): route a real accepted Completion
    // through the REAL arena waiter/borrow authorities. No side-band waiter
    // map, no reimplementation of the waiter state machine: the Completion is
    // resolved to its current SlotHandle by the arena's own bounded scan
    // (the same identity bridge the public cancel path uses) and the call is
    // forwarded verbatim. Guarded; production builds carry nothing. ---

    // Register one waiter on the slot bound to a real accepted Completion.
    // Returns the arena's own register_waiter result (not_found for an
    // unbound/stale Completion; invalid_state for a second registration or a
    // non-accepted/unreaped slot — registration is orthogonal to execution
    // state, ADR Decision 10).
    Result<void> register_waiter_for_test(Completion<std::size_t>& c,
                                          detail::WaiterToken token,
                                          detail::RoutingLease lease) {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::not_found});
        }
        return arena_.register_waiter(*h, token, std::move(lease));
    }
    Result<void> register_waiter_for_test(Completion<void>& c,
                                          detail::WaiterToken token,
                                          detail::RoutingLease lease) {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::not_found});
        }
        return arena_.register_waiter(*h, token, std::move(lease));
    }

    // Wait-cancel through the REAL arena authority: removes ONLY the waiter,
    // never the I/O. Returns the moved-out RoutingLease (the caller owns it),
    // or not_found when no registered waiter remains (already closed by reap /
    // already canceled / unbound Completion).
    Result<detail::RoutingLease> cancel_waiter_for_test(Completion<std::size_t>& c) {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<detail::RoutingLease>(
                IoError{IoError::Code::not_found});
        }
        return arena_.cancel_waiter(*h);
    }
    Result<detail::RoutingLease> cancel_waiter_for_test(Completion<void>& c) {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<detail::RoutingLease>(
                IoError{IoError::Code::not_found});
        }
        return arena_.cancel_waiter(*h);
    }

    // Stale-generation waiter injection (C2c row 14a): drive a CAPTURED
    // SlotHandle (typically a stale-generation handle from a released
    // occupant) through the REAL arena register/cancel_waiter authorities.
    // This is what proves a stale waiter authority cannot act on a live N+1
    // occupant's registration: handle validation rejects it with not_found and
    // zero side effect. Mirrors cancel_handle_for_test (C2b) — pointer-free
    // identity only, no Completion reverse map, no side-band waiter map.
    Result<void> register_waiter_handle_for_test(detail::SlotHandle h,
                                                 detail::WaiterToken token,
                                                 detail::RoutingLease lease) {
        return arena_.register_waiter(h, token, std::move(lease));
    }
    Result<detail::RoutingLease> cancel_waiter_handle_for_test(detail::SlotHandle h) {
        return arena_.cancel_waiter(h);
    }

    // Generation-validated by-value borrow snapshot for a captured SlotHandle.
    std::optional<detail::RequestArena::BorrowSnapshot> borrow_for_test(
        detail::SlotHandle h) const noexcept {
        return arena_.borrow_for_test(h);
    }

    // Generation-validated by-value single-waiter registration observation.
    std::optional<detail::RequestArena::WaiterObservation> waiter_for_test(
        detail::SlotHandle h) const noexcept {
        return arena_.waiter_for_test(h);
    }

    // C2c sink observation (fixed-size, allocation-free, test-only): the last
    // delivered ReadyEvent's waiter payload + total delivery count. Read after
    // poll()/wait_one() returns.
    bool sink_last_has_waiter() const noexcept { return sink_.last_has_waiter(); }
    detail::WaiterToken sink_last_token() const noexcept { return sink_.last_token(); }
    std::uint64_t sink_last_lease_id() const noexcept { return sink_.last_lease_id(); }
#endif

  private:
    static constexpr std::size_t kDefaultCapacity = 64;

    static std::uint64_t next_backend_id() noexcept {
        static std::atomic<std::uint64_t> id{0x4A410000u}; // 'FA' provenance tag
        return ++id;
    }

    // Resolve a terminal result on the OLDEST enqueued read/write op, binding it
    // to that op's RequestKey immediately (review finding #2: no staging deque,
    // no cross-generation pollution). A no-op when no size op is outstanding, or
    // a no-op on record_terminal when the oldest op already went terminal
    // (terminal-winner rule — a second complete_* cannot overwrite). Allocation-
    // free: bounded O(capacity) scan + record_terminal under the arena lock.
    void resolve_size_terminal(detail::TerminalResult res) {
        auto oh = arena_.oldest_enqueued_of(detail::OperationKind::read);
        // read and write share the size completion type; complete the oldest of
        // EITHER size kind by scanning both. (complete_oldest_* historically
        // targeted "the oldest read/write op"; preserve that by taking the
        // older of the read-oldest and write-oldest.)
        auto wh = arena_.oldest_enqueued_of(detail::OperationKind::write);
        std::optional<detail::SlotHandle> target;
        if (oh.has_value() && wh.has_value()) {
            target = (arena_.submit_seq_of(oh->slot) <= arena_.submit_seq_of(wh->slot))
                         ? oh : wh;
        } else if (oh.has_value()) {
            target = oh;
        } else {
            target = wh;
        }
        if (!target.has_value()) return;
        bool won = arena_.record_terminal(*target, res);
        tally_terminal_result(won, res);
    }
    // Resolve a terminal result on the OLDEST enqueued sync op (sync_data /
    // sync_all share the void completion type).
    void resolve_void_terminal(detail::TerminalResult res) {
        auto dh = arena_.oldest_enqueued_of(detail::OperationKind::sync_data);
        auto ah = arena_.oldest_enqueued_of(detail::OperationKind::sync_all);
        std::optional<detail::SlotHandle> target;
        if (dh.has_value() && ah.has_value()) {
            target = (arena_.submit_seq_of(dh->slot) <= arena_.submit_seq_of(ah->slot))
                         ? dh : ah;
        } else if (dh.has_value()) {
            target = dh;
        } else {
            target = ah;
        }
        if (!target.has_value()) return;
        bool won = arena_.record_terminal(*target, res);
        tally_terminal_result(won, res);
    }

    // Five-stage admission for a byte-carrying op. No terminal is recorded: the
    // op stays enqueued until the test completes it or auto-mode fires.
    //   reserve -> prepare -> install publication binding -> begin_binding CAS
    //   -> commit (Step 4; the submit-success LP's slot half) -> install release
    //   capability -> commit_binding (Step 5: `binding -> outstanding`
    //   release-store — the commit/accept linearization point) -> enqueue
    //   (noexcept).
    //
    // Transactional pre-commit path (review C1): every step before the commit
    // LP is rollback-able with ZERO side effects. The publication binding is
    // installed INTO the slot record (no map insert); the Completion CAS is the
    // only electing step and a lost CAS rolls back ONLY this submit's slot (no
    // FIFO residue — there is no side-band FIFO anymore, review finding #1).
    // Nothing after commit_binding may throw (I9).
    //
    // The whole Stage 1-3 protocol runs under the backend admission
    // transaction lock (ADR :453-462: the winning submit retains its
    // context/admission lock through Step 5). close_admission() takes the same
    // lock, so after it returns no new acceptance LP can occur (Decision 15):
    // an in-flight submit either completes its LP before close returns (submit
    // wins) or observes admission closed at reserve and rejects (close wins).
    // The lock is released before enqueue (no-fail).
    template <class Op>
    Result<void> submit_size(Op op, Completion<std::size_t>& c, detail::OperationKind kind,
                             std::size_t len) {
        detail::SlotHandle h{};
        {
            std::lock_guard<std::mutex> admission_lk(admission_mtx_);
            // Stage 1: reserve. Arena full -> would_block; admission closed ->
            // invalid_state (ADR Decision 6/13: capacity pressure is NEVER
            // invalid_state).
            auto rh = arena_.reserve();
            if (!rh.has_value()) {
                return make_unexpected<void>(rh.error());
            }
            h = rh.value();
            // Stage 2: prepare (writes the op kind + fd/buffer borrow metadata).
            auto ph = arena_.prepare(h, kind, borrow_of(op));
            if (!ph.has_value()) {
                (void)arena_.rollback_reserved_or_prepared(h);  // roll back reservation
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
            // Stage 3a: Completion CAS idle -> binding elects ONE submitting
            // context. Loser: roll back only our own slot + binding (zero residue —
            // there is no side-band FIFO to contaminate).
            if (!begin_binding(c)) {
                (void)arena_.rollback_reserved_or_prepared(h);
                return make_unexpected<void>(IoError{IoError::Code::invalid_state});
            }
            // Stage 3b: commit (prepared -> pending, enqueue pin live, accepted++,
            // borrow begins, submit_seq assigned — the submit-success LP's slot
            // half; Step 4).
            auto ch = arena_.commit(h);
            if (!ch.has_value()) {
                rollback_binding_before_accept(c);
                (void)arena_.rollback_reserved_or_prepared(h);
                return make_unexpected<void>(IoError{IoError::Code::invalid_state});
            }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            // Deterministic causal seam: pause between commit and enqueue so a
            // backend-level test can interleave cancel exactly in the Scheme-B
            // window (the context's access_mtx_ serialization hides it otherwise),
            // and so a C2e test can interleave close_admission() against the
            // in-flight acceptance protocol before the Step 5 LP
            // (fake_c2e_close_waits_for_inflight_acceptance_lp; mutant M11
            // detector — the pause is INSIDE the admission transaction, so a
            // close that returned here would permit a new LP after close).
            wait_submit_pause_();
#endif
            // Stage 3c: install the slot-release capability (ADR Decision 7), then
            // publish outstanding. AFTER commit_binding NOTHING may throw: the
            // remaining steps (enqueue) are noexcept.
            install_binding(c, &arena_, h);
            commit_binding(c);
        }
        // Stage 4: enqueue (pending -> enqueued OR terminal no-op; ack pin as
        // the final slot access). Allocation-free, noexcept.
        (void)arena_.enqueue(h);
        return {};
    }

    template <class Op>
    Result<void> submit_void(Op op, Completion<void>& c, detail::OperationKind kind) {
        detail::SlotHandle h{};
        {
            std::lock_guard<std::mutex> admission_lk(admission_mtx_);
            auto rh = arena_.reserve();
            if (!rh.has_value()) {
                return make_unexpected<void>(rh.error());
            }
            h = rh.value();
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
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            wait_submit_pause_();
#endif
            install_binding(c, &arena_, h);
            commit_binding(c);
        }
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

    // Dispatch: in auto-complete mode, record the terminal result on each
    // enqueued slot (drained in submission order via oldest_enqueued_of),
    // transitioning enqueued -> backend_ready. A slot that already has a
    // terminal (a Scheme-B cancel won first) is left untouched (record_terminal
    // is a no-op). After dispatch, reap publishes every backend_ready slot
    // through the slot's own publication binding in ready-ring order.
    std::size_t dispatch_and_reap() {
        if (auto_mode_ != Auto::off) {
            drain_auto_size();
            drain_auto_void();
        }
        // Non-auto: complete_*/cancel already recorded their terminals directly
        // (review finding #2 — no staging step here). Just reap.
        // Phase F1: deliver identity events to the attached Scheduler-owned
        // routing sink when one is set; otherwise the no-op reference sink.
        return arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
    }

    void drain_auto_size() {
        // Drain every enqueued read/write op in submission order, recording the
        // auto result. oldest_enqueued_of returns the next-oldest after each
        // record_terminal transitions the previous one out of `enqueued`.
        for (;;) {
            auto oh = arena_.oldest_enqueued_of(detail::OperationKind::read);
            auto wh = arena_.oldest_enqueued_of(detail::OperationKind::write);
            std::optional<detail::SlotHandle> target;
            if (oh.has_value() && wh.has_value()) {
                target = (arena_.submit_seq_of(oh->slot) <= arena_.submit_seq_of(wh->slot))
                             ? oh : wh;
            } else if (oh.has_value()) {
                target = oh;
            } else {
                target = wh;
            }
            if (!target.has_value()) break;
            std::size_t requested =
                static_cast<std::size_t>(arena_.requested_bytes_of(target->slot));
            detail::TerminalResult res = auto_size_result(requested);
            bool won = arena_.record_terminal(*target, res);
            tally_terminal_result(won, res);
        }
    }
    void drain_auto_void() {
        for (;;) {
            auto dh = arena_.oldest_enqueued_of(detail::OperationKind::sync_data);
            auto ah = arena_.oldest_enqueued_of(detail::OperationKind::sync_all);
            std::optional<detail::SlotHandle> target;
            if (dh.has_value() && ah.has_value()) {
                target = (arena_.submit_seq_of(dh->slot) <= arena_.submit_seq_of(ah->slot))
                             ? dh : ah;
            } else if (dh.has_value()) {
                target = dh;
            } else {
                target = ah;
            }
            if (!target.has_value()) break;
            detail::TerminalResult res = (auto_mode_ == Auto::err)
                                             ? detail::TerminalResult::err(auto_err_)
                                             : detail::TerminalResult::ok_void();
            // CodeRabbit finding: tally void auto-completions too (parity with
            // drain_auto_size), so auto_error increments completion_errors for a
            // sync op just as it does for a read/write op.
            bool won = arena_.record_terminal(*target, res);
            tally_terminal_result(won, res);
        }
    }

    // Build the auto-completion result for a read/write op given its requested
    // length (auto mode). short_then_full: first op short, then full remaining.
    detail::TerminalResult auto_size_result(std::size_t requested) {
        switch (auto_mode_) {
        case Auto::bytes:
            return detail::TerminalResult::ok_bytes(auto_bytes_);
        case Auto::err:
            return detail::TerminalResult::err(auto_err_);
        case Auto::short_then_full:
            if (!auto_short_used_) {
                auto_short_used_ = true;
                return detail::TerminalResult::ok_bytes(auto_bytes_);
            }
            return detail::TerminalResult::ok_bytes(requested);
        default:
            return detail::TerminalResult::ok_bytes(requested);
        }
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

    // Stats tally at the TERMINAL-WINNER site (exactly-once: record_terminal
    // returns true only for the single winner, and cancel returns `requested`
    // only when it stored the canceled terminal; losers never tally). The
    // tally was previously done at reap publication; both are exactly-once for
    // an accepted op, and only the winner site is reachable from the static
    // publish thunks (which have no instance state).
    void tally_canceled() noexcept {
        if (stats_) ++stats_->canceled_ops;
    }
    void tally_terminal_result(bool won, const detail::TerminalResult& t) noexcept {
        if (!stats_ || !won || !t.stored || !t.is_error) return;
        if (t.error.code == IoError::Code::canceled) {
            ++stats_->canceled_ops;
        } else {
            ++stats_->completion_errors;
        }
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    void wait_submit_pause_() noexcept {
        SubmitPauseGate* g = submit_pause_gate_.load(std::memory_order_relaxed);
        if (g == nullptr) return;
        g->paused.store(true, std::memory_order_release);
        while (!g->resume.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    std::atomic<SubmitPauseGate*> submit_pause_gate_{nullptr};
#endif

    detail::RequestArena arena_;
    detail::ReferenceReadySink sink_;
    // Backend admission transaction domain (ADR §"Commit / accept" :453-462:
    // the winning submit retains its context/admission lock through Step 5 —
    // the `binding -> outstanding` release-store, the commit/accept
    // linearization point). close_admission() takes the same lock, so after it
    // returns no new acceptance LP can occur (Decision 15). Acquired ONLY by
    // the submit paths (reserve .. commit_binding) and close_admission();
    // released before enqueue. Lock order: admission_mtx_ -> arena leaf only.
    mutable std::mutex admission_mtx_;
    // No side-band HandleRing or staged_* deques (review findings #1, #2): the
    // submission-order selection is a bounded O(capacity) scan via the arena's
    // oldest_enqueued_of, and terminal evidence binds to a RequestKey at
    // complete_*/cancel call time. This removes the stale-handle accumulation
    // that could strand a later accepted op and the cross-generation terminal
    // pollution, and it makes the complete_*/cancel path genuinely allocation-
    // free (Decision 14).

    // Auto-complete mode state.
    enum class Auto : std::uint8_t { off, bytes, err, short_then_full };
    Auto auto_mode_ = Auto::off;
    std::size_t auto_bytes_ = 0;
    IoError auto_err_{IoError::Code::backend_error};
    bool auto_short_used_ = false;
};

} // namespace sluice::async
