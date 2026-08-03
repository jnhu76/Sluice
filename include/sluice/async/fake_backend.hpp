// sluice::async::FakeAsyncBackend (sluice-CORE-019, ADR §4/§10 T1).
//
// A deterministic async backend for tests: ops submitted are held outstanding
// across poll() calls UNTIL THE TEST EXPLICITLY COMPLETES THEM. No kernel, no
// threads. This is the primary unit-test vehicle for all later async work
// (018/018B/021) and the thing that makes the buffer-lifetime contract (gate
// item 1) genuinely testable.
//
// Completion model:
//   - submit_* records the op (no completion produced).
//   - The test calls one of the complete_*() helpers to stage a terminal result
//     for the OLDEST outstanding op of a given kind (FIFO by default, ADR O3 for
//     the fake). Arbitrary order is available via complete_op_at().
//   - poll()/wait_one() then move staged results into the Completions (marking
//     them ready). This keeps the "completions only inside poll/wait_one" rule
//     (ADR A3/O1) even on the fake.
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
// test stages a result (or auto-mode fires); this preserves the "held
// outstanding until explicitly completed" contract. Cancel records the canceled
// terminal directly under Scheme B (pending/enqueued cancel wins).
//
// Identity (review C2): the Completion publication binding lives IN the
// RequestSlot record (install_publication_binding before the Completion CAS);
// reap validates it and publishes Completion-ready through it inside the leaf
// domain. There is NO parallel unordered_map identity bridge — cancel resolves
// a Completion* by the arena's bounded O(capacity) scan. Pre-commit
// bookkeeping is transactional (review C1): the submission-order FIFO is a
// construction-time bounded ring (never allocates, never grows unbounded), and
// every pre-commit failure path rolls the reservation back with zero side
// effects (Completion untouched, slot freed, ring unchanged, no future result
// contamination).
//
// State is instance-owned only (no globals, gate item 6).
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/reference_ready_sink.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include <thread>
#endif

namespace sluice::async {

namespace detail {
// Construction-time bounded SlotHandle FIFO (review C1: the fake's submission-
// order queue is bounded bookkeeping, not an unbounded deque). Storage is
// preallocated in the backend constructor (capacity == request_capacity), so
// push/pop never allocate and the queue can never grow past in-flight ops.
class HandleRing {
  public:
    explicit HandleRing(std::size_t capacity) : storage_(capacity) {}

    bool empty() const noexcept { return count_ == 0; }
    std::size_t size() const noexcept { return count_; }

    // Insert at the tail. Noexcept + preallocated; returns false only if full
    // (the backend mis-counted in-flight ops — an invariant violation, not an
    // allocation failure; at most one handle per in-flight op, so a ring of
    // request_capacity can never overflow a correctly-driven backend).
    bool push(SlotHandle h) noexcept {
        if (count_ == storage_.size()) return false;
        storage_[tail_] = h;
        tail_ = (tail_ + 1) % storage_.size();
        ++count_;
        return true;
    }
    // Undo the most recent push (commit-failure rollback). The submit path is
    // serialized (AsyncIoContext::access_mtx_), so the tail is this submit's
    // own handle when pop_tail is called.
    void pop_tail() noexcept {
        if (count_ == 0) return;
        --count_;
        tail_ = (tail_ == 0) ? storage_.size() - 1 : tail_ - 1;
    }
    // Remove and return the oldest handle; nullopt when empty.
    std::optional<SlotHandle> pop_front() noexcept {
        if (count_ == 0) return std::nullopt;
        SlotHandle h = storage_[head_];
        head_ = (head_ + 1) % storage_.size();
        --count_;
        return h;
    }

  private:
    std::vector<SlotHandle> storage_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
};
}  // namespace detail

class FakeAsyncBackend : public AsyncBackend {
  public:
    explicit FakeAsyncBackend(std::size_t request_capacity = kDefaultCapacity)
        : arena_(detail::ContextIdentity::for_testing(next_backend_id()), request_capacity),
          size_fifo_(request_capacity),
          void_fifo_(request_capacity) {}
    ~FakeAsyncBackend() override = default;

    // --- auto-complete mode ---
    // When set, poll() auto-completes each outstanding op with `auto_bytes_`
    // (read/write) or void-success (sync), WITHOUT the test staging anything.
    // This lets the synchronous read_all/write_all coordinators (job 018) drive
    // the fake in a poll-loop, since they submit+poll internally and cannot have
    // the test stage results between their loop steps.
    //   auto_bytes(n)         each outstanding op completes with n bytes
    //                         (n may be < requested => short, exercises retry)
    //   auto_short_then_full(first, rest)
    //                         the FIRST outstanding op completes short (first),
    //                         subsequent ones complete their full remaining
    //                         length; models one short then clean completion.
    //   auto_error(e)         each outstanding op completes with error e
    //   auto_eof()            read completes with 0 bytes (EOF) — shortcut for
    //                         auto_bytes(0).
    //   auto_disable()        stop auto-completing (resume explicit staging).
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

    // --- test-driving helpers: stage terminal results for the OLDEST
    // outstanding op of the matching kind. Applied at the next poll(). ---

    // Stage a byte-count result for the oldest outstanding read/write op
    // (n < requested => short completion). No-op if none outstanding. The result
    // is consumed at the next poll() when the matching Completion is reaped.
    void complete_oldest_with_bytes(std::size_t n) {
        if (size_fifo_.empty())
            return;
        staged_size_.push_back(n);
    }
    // Stage an error result for the oldest outstanding read/write op.
    void complete_oldest_with_error(IoError e) {
        if (size_fifo_.empty())
            return;
        staged_size_err_.push_back(e);
    }
    // Stage a void success for the oldest outstanding sync op.
    void complete_oldest_sync_ok() {
        if (void_fifo_.empty())
            return;
        staged_void_ok_.push_back(true);
    }
    // Stage a void error for the oldest outstanding sync op.
    void complete_oldest_sync_error(IoError e) {
        if (void_fifo_.empty())
            return;
        staged_void_err_.push_back(e);
    }

    // --- reap: apply staged/canceled/auto results to Completions (mark ready).
    // Order: targeted cancels first (pointer-keyed, take precedence over FIFO
    // staging), then auto-mode (drains everything), then explicit FIFO staging.
    std::size_t poll() override { return dispatch_and_reap(); }

    Result<std::size_t> wait_one() override {
        // No real waiting (no kernel/threads); just poll. Tests drive timing.
        return dispatch_and_reap();
    }

    // Minimal cancel (ADR §7 X2): REQUESTS cancel. The op stays outstanding and
    // is completed (exactly-once, X3) at the next poll()/wait_one() with
    // IoError::canceled. We do NOT complete here — A3/O1: completions are
    // produced only inside poll/wait_one. Cancel is POINTER-KEYED (targeted) so
    // it works on any outstanding op, not just the FIFO oldest.
    // Phase B (ADR Decision 11): resolves Completion* -> SlotHandle via the
    // arena's bounded slot scan (the slot's own binding is the identity — no
    // parallel map), then arena.cancel() wins the terminal transition under
    // Scheme B (pending -> backend_ready(canceled) directly; enqueued ->
    // canceled terminal; a slot that already went terminal is a no-op — losers
    // never overwrite). canceled_ops is tallied at the terminal-winner site
    // (exactly-once; reap publishes the stored result). The Completion stays
    // outstanding; poll/wait_one publishes through reap.
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
    std::size_t sink_deliveries() const noexcept { return sink_.deliveries(); }
    // The submission-order ring depth (review C1: bounded, never grows past
    // in-flight ops; a rejected submit leaves it unchanged).
    std::size_t size_fifo_count() const noexcept { return size_fifo_.size(); }
    std::size_t void_fifo_count() const noexcept { return void_fifo_.size(); }
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
#endif

  private:
    static constexpr std::size_t kDefaultCapacity = 64;

    static std::uint64_t next_backend_id() noexcept {
        static std::atomic<std::uint64_t> id{0x4A410000u}; // 'FA' provenance tag
        return ++id;
    }

    // Five-stage admission for a byte-carrying op. No terminal is recorded: the
    // op stays enqueued until the test stages a result or auto-mode fires.
    //   reserve -> prepare -> install publication binding -> begin_binding CAS
    //   -> [bounded ring push] -> commit -> install release capability ->
    //   commit_binding (submit-success LP) -> enqueue (noexcept).
    //
    // Transactional pre-commit path (review C1): every step before the commit
    // LP is rollback-able with ZERO side effects. The publication binding is
    // installed INTO the slot record (no map insert); the submission-order FIFO
    // is a construction-time bounded ring (push never allocates and cannot grow
    // unbounded); the Completion CAS is the only electing step and a lost CAS
    // rolls back ONLY this submit's slot (no FIFO residue — the ring is
    // untouched at that point). Nothing after commit_binding may throw (I9).
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
        // context. Loser: roll back only our own slot + binding (the ring is
        // untouched — zero FIFO residue).
        if (!begin_binding(c)) {
            (void)arena_.rollback_reserved_or_prepared(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        // Stage 3b: FIFO bookkeeping — bounded ring push (noexcept; capacity ==
        // request_capacity so it can never overflow a correctly-driven backend).
        (void)size_fifo_.push(h);
        // Stage 3c: commit (prepared -> pending, enqueue pin live, accepted++,
        // borrow begins — the submit-success LP's slot half).
        auto ch = arena_.commit(h);
        if (!ch.has_value()) {
            size_fifo_.pop_tail();  // undo the push (serialized submit path)
            rollback_binding_before_accept(c);
            (void)arena_.rollback_reserved_or_prepared(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        // Deterministic causal seam: pause between commit and enqueue so a
        // backend-level test can interleave cancel exactly in the Scheme-B
        // window (the context's access_mtx_ serialization hides it otherwise).
        wait_submit_pause_();
#endif
        // Stage 3d: install the slot-release capability (ADR Decision 7), then
        // publish outstanding. AFTER commit_binding NOTHING may throw: the
        // remaining steps (enqueue) are noexcept.
        install_binding(c, &arena_, h);
        commit_binding(c);
        // Stage 4: enqueue (pending -> enqueued OR terminal no-op; ack pin as
        // the final slot access). Allocation-free, noexcept.
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
        (void)void_fifo_.push(h);
        auto ch = arena_.commit(h);
        if (!ch.has_value()) {
            void_fifo_.pop_tail();
            rollback_binding_before_accept(c);
            (void)arena_.rollback_reserved_or_prepared(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        wait_submit_pause_();
#endif
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

    // Dispatch: record the terminal result on each slot per the active policy,
    // transitioning enqueued -> backend_ready. A slot that already has a
    // terminal (a Scheme-B cancel won first) is left untouched (record_terminal
    // is a no-op). After dispatch, reap publishes every backend_ready slot
    // through the slot's own publication binding (inside the leaf domain).
    std::size_t dispatch_and_reap() {
        // (1) Auto-complete mode: drain every enqueued slot with the auto result.
        if (auto_mode_ != Auto::off) {
            drain_auto_size();
            drain_auto_void();
        } else {
            // (2) Explicit FIFO staging: apply staged results to the
            // FIFO-oldest enqueued slots of each kind.
            apply_size_staging();
            apply_void_staging();
        }

        return arena_.reap(sink_);
    }

    // Pop FIFO-oldest size handles whose slot is still enqueued (skipping slots
    // that already went terminal via cancel). Returns the next live handle or
    // nullopt if the FIFO is exhausted.
    //
    // Serialization note: the handle is pushed to the ring BEFORE the Completion
    // CAS / commit / enqueue, but poll() only runs under
    // AsyncIoContext::access_mtx_, which the submit holds through enqueue — so
    // a poll can never observe a slot still `pending` here. If a later phase
    // drops that serialization, this pop-then-check would drop the handle and
    // strand the op; the check must then move into the arena domain.
    std::optional<detail::SlotHandle> pop_live_size_front() {
        while (auto oh = size_fifo_.pop_front()) {
            detail::SlotHandle h = *oh;
            auto st = arena_.state_of(h.slot);
            if (st == detail::RequestState::enqueued &&
                arena_.generation_of(h.slot).value == h.generation.value) {
                return h;
            }
            // Already terminal (cancel won) or reaped — skip.
        }
        return std::nullopt;
    }
    std::optional<detail::SlotHandle> pop_live_void_front() {
        while (auto oh = void_fifo_.pop_front()) {
            detail::SlotHandle h = *oh;
            auto st = arena_.state_of(h.slot);
            if (st == detail::RequestState::enqueued &&
                arena_.generation_of(h.slot).value == h.generation.value) {
                return h;
            }
        }
        return std::nullopt;
    }

    void drain_auto_size() {
        while (auto oh = pop_live_size_front()) {
            detail::SlotHandle h = *oh;
            std::size_t requested = static_cast<std::size_t>(arena_.requested_bytes_of(h.slot));
            detail::TerminalResult res = auto_size_result(requested);
            bool won = arena_.record_terminal(h, res);
            tally_terminal_result(won, res);
        }
    }
    void drain_auto_void() {
        while (auto oh = pop_live_void_front()) {
            detail::SlotHandle h = *oh;
            if (auto_mode_ == Auto::err) {
                (void)arena_.record_terminal(h, detail::TerminalResult::err(auto_err_));
            } else {
                (void)arena_.record_terminal(h, detail::TerminalResult::ok_void());
            }
        }
    }

    void apply_size_staging() {
        while (!size_fifo_.empty() && has_size_stage()) {
            auto oh = pop_live_size_front();
            if (!oh.has_value())
                break;
            detail::TerminalResult res = take_size_stage();
            bool won = arena_.record_terminal(*oh, res);
            tally_terminal_result(won, res);
        }
    }
    void apply_void_staging() {
        while (!void_fifo_.empty() && has_void_stage()) {
            auto oh = pop_live_void_front();
            if (!oh.has_value())
                break;
            detail::TerminalResult res = take_void_stage();
            bool won = arena_.record_terminal(*oh, res);
            tally_terminal_result(won, res);
        }
    }

    bool has_size_stage() const { return !staged_size_.empty() || !staged_size_err_.empty(); }
    bool has_void_stage() const { return !staged_void_ok_.empty() || !staged_void_err_.empty(); }
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
    detail::TerminalResult take_size_stage() {
        if (!staged_size_err_.empty()) {
            IoError e = staged_size_err_.front();
            staged_size_err_.pop_front();
            return detail::TerminalResult::err(e);
        }
        std::size_t n = staged_size_.front();
        staged_size_.pop_front();
        return detail::TerminalResult::ok_bytes(n);
    }
    detail::TerminalResult take_void_stage() {
        if (!staged_void_err_.empty()) {
            IoError e = staged_void_err_.front();
            staged_void_err_.pop_front();
            return detail::TerminalResult::err(e);
        }
        staged_void_ok_.pop_front();
        return detail::TerminalResult::ok_void();
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
    // SlotHandle FIFOs in submission order (ADR O3 for the fake). Construction-
    // time bounded rings (review C1): storage is preallocated in the backend
    // constructor (capacity == request_capacity), so push/pop never allocate
    // and the queue can never grow past in-flight ops — a failed submit leaves
    // the ring unchanged (zero side effects). Drained as ops are completed; a
    // handle whose slot has already gone terminal (cancel) or been reaped is
    // skipped by pop_live_*_front.
    detail::HandleRing size_fifo_;
    detail::HandleRing void_fifo_;
    // Staged terminal results the test queued; consumed at poll().
    std::deque<std::size_t> staged_size_;
    std::deque<IoError> staged_size_err_;
    std::deque<bool> staged_void_ok_;
    std::deque<IoError> staged_void_err_;

    // Auto-complete mode state.
    enum class Auto : std::uint8_t { off, bytes, err, short_then_full };
    Auto auto_mode_ = Auto::off;
    std::size_t auto_bytes_ = 0;
    IoError auto_err_{IoError::Code::backend_error};
    bool auto_short_used_ = false;
};

} // namespace sluice::async
