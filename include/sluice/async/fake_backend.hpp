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
#include <unordered_map>
#include <utility>

namespace sluice::async {

class FakeAsyncBackend : public AsyncBackend {
  public:
    explicit FakeAsyncBackend(std::size_t request_capacity = kDefaultCapacity)
        : arena_(detail::ContextIdentity::for_testing(next_backend_id()), request_capacity) {}
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
    // it works on any outstanding op, not just the FIFO oldest: a targeted
    // cancel takes precedence over FIFO staging for its specific completion.
    // Phase B: resolves Completion* -> SlotHandle, then arena.cancel() records
    // the canceled terminal under Scheme B (enqueued cancel wins the terminal
    // transition directly).
    void cancel(Completion<std::size_t>& c) override {
        auto h = lookup(&c);
        if (h.has_value()) {
            targeted_size_.push_back(*h);
        }
    }
    void cancel(Completion<void>& c) override {
        auto h = lookup(&c);
        if (h.has_value()) {
            targeted_void_.push_back(*h);
        }
    }

    std::size_t outstanding() const noexcept override { return arena_.accepted_outstanding(); }

    // Phase B test-only introspection (the arena is a private detail).
    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_capacity_rejections() const noexcept { return arena_.capacity_rejections(); }
    std::size_t sink_deliveries() const noexcept { return sink_.deliveries(); }

  private:
    static constexpr std::size_t kDefaultCapacity = 64;

    static std::uint64_t next_backend_id() noexcept {
        static std::atomic<std::uint64_t> id{0x4A410000u}; // 'FA' provenance tag
        return ++id;
    }

    // Five-stage admission for a byte-carrying op. No terminal is recorded: the
    // op stays enqueued until the test stages a result or auto-mode fires.
    template <class Op>
    Result<void> submit_size(Op /*op*/, Completion<std::size_t>& c, detail::OperationKind kind,
                             std::size_t len) {
        auto rh = arena_.reserve();
        if (!rh.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        detail::SlotHandle h = rh.value();
        auto ph = arena_.prepare(h, kind);
        if (!ph.has_value()) {
            (void)arena_.release(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (!begin_binding(c)) {
            (void)arena_.release(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        auto ch = arena_.commit(h);
        if (!ch.has_value()) {
            rollback_binding_before_accept(c);
            (void)arena_.release(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        commit_binding(c);
        bind(h, &c, len);
        size_fifo_.push_back(h); // FIFO submission order (ADR O3 for fake)
        (void)arena_.enqueue(h);
        return {};
    }

    template <class Op>
    Result<void> submit_void(Op /*op*/, Completion<void>& c, detail::OperationKind kind) {
        auto rh = arena_.reserve();
        if (!rh.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        detail::SlotHandle h = rh.value();
        auto ph = arena_.prepare(h, kind);
        if (!ph.has_value()) {
            (void)arena_.release(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (!begin_binding(c)) {
            (void)arena_.release(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        auto ch = arena_.commit(h);
        if (!ch.has_value()) {
            rollback_binding_before_accept(c);
            (void)arena_.release(h);
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        commit_binding(c);
        bind(h, &c);
        void_fifo_.push_back(h);
        (void)arena_.enqueue(h);
        return {};
    }

    // Dispatch: record the terminal result on each slot per the active policy,
    // transitioning enqueued -> backend_ready. A slot that already has a
    // terminal (a Scheme-B cancel won first) is left untouched (record_terminal
    // is a no-op). After dispatch, reap publishes every backend_ready slot.
    std::size_t dispatch_and_reap() {
        // (1) Targeted cancels (pointer-keyed). Record canceled terminal on each
        // targeted slot. record_terminal is a no-op if the slot is already
        // terminal (exactly-once; losers never overwrite).
        while (!targeted_size_.empty()) {
            detail::SlotHandle h = targeted_size_.front();
            targeted_size_.pop_front();
            if (arena_.state_for_testing(h.slot) == detail::RequestState::enqueued) {
                (void)arena_.record_terminal(
                    h, detail::TerminalResult::err(IoError{IoError::Code::canceled}));
            }
        }
        while (!targeted_void_.empty()) {
            detail::SlotHandle h = targeted_void_.front();
            targeted_void_.pop_front();
            if (arena_.state_for_testing(h.slot) == detail::RequestState::enqueued) {
                (void)arena_.record_terminal(
                    h, detail::TerminalResult::err(IoError{IoError::Code::canceled}));
            }
        }

        // (2) Auto-complete mode: drain every enqueued slot with the auto result.
        if (auto_mode_ != Auto::off) {
            drain_auto_size();
            drain_auto_void();
        } else {
            // (3) Explicit FIFO staging: apply staged results to the
            // FIFO-oldest enqueued slots of each kind.
            apply_size_staging();
            apply_void_staging();
        }

        return arena_.reap(sink_, [this](const detail::RequestArena::ReapPublication& p) {
            publish_and_release(p);
        });
    }

    // Pop FIFO-oldest size handles whose slot is still enqueued (skipping slots
    // that already went terminal via targeted cancel). Returns the next live
    // handle or nullopt if the FIFO is exhausted.
    std::optional<detail::SlotHandle> pop_live_size_front() {
        while (!size_fifo_.empty()) {
            detail::SlotHandle h = size_fifo_.front();
            size_fifo_.pop_front();
            auto st = arena_.state_for_testing(h.slot);
            if (st == detail::RequestState::enqueued &&
                arena_.generation_for_testing(h.slot).value == h.generation.value) {
                return h;
            }
            // Already terminal (cancel won) or reaped — skip.
        }
        return std::nullopt;
    }
    std::optional<detail::SlotHandle> pop_live_void_front() {
        while (!void_fifo_.empty()) {
            detail::SlotHandle h = void_fifo_.front();
            void_fifo_.pop_front();
            auto st = arena_.state_for_testing(h.slot);
            if (st == detail::RequestState::enqueued &&
                arena_.generation_for_testing(h.slot).value == h.generation.value) {
                return h;
            }
        }
        return std::nullopt;
    }

    void drain_auto_size() {
        while (auto oh = pop_live_size_front()) {
            detail::SlotHandle h = *oh;
            std::size_t requested = bindings_[h.slot.value].requested_bytes;
            (void)arena_.record_terminal(h, auto_size_result(requested));
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
            (void)arena_.record_terminal(*oh, take_size_stage());
        }
    }
    void apply_void_staging() {
        while (!void_fifo_.empty() && has_void_stage()) {
            auto oh = pop_live_void_front();
            if (!oh.has_value())
                break;
            (void)arena_.record_terminal(*oh, take_void_stage());
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

    // Resolve a reaped slot back to its Completion*, publish the terminal
    // result, then release the slot. See SyncBackend for the same handshake.
    void publish_and_release(const detail::RequestArena::ReapPublication& p) {
        auto it = bindings_.find(p.handle.slot.value);
        if (it == bindings_.end())
            return;
        detail::SlotHandle bh{p.handle.slot, p.handle.generation};
        if (it->second.generation.value != bh.generation.value)
            return;
        Completion<std::size_t>* sc = it->second.size_completion;
        Completion<void>* vc = it->second.void_completion;
        bindings_.erase(it);
        if (sc) {
            publish(*sc, terminal_to_size(p.terminal));
            tally_terminal(p.terminal);
        } else if (vc) {
            publish(*vc, terminal_to_void(p.terminal));
            tally_terminal(p.terminal);
        }
        (void)arena_.release(bh);
    }

    void tally_terminal(const detail::TerminalResult& t) {
        if (!stats_)
            return;
        if (t.stored && t.is_error && t.error.code == IoError::Code::canceled) {
            ++stats_->canceled_ops;
        } else if (t.stored && t.is_error) {
            ++stats_->completion_errors;
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

    // Phase B pointer-keyed bridge (see SyncBackend).
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
    // SlotHandle FIFOs in submit order (ADR O3 for the fake). Drained as ops
    // are completed; a handle whose slot has already gone terminal (cancel) or
    // been reaped is skipped by pop_live_*_front.
    std::deque<detail::SlotHandle> size_fifo_;
    std::deque<detail::SlotHandle> void_fifo_;
    // Staged terminal results the test queued; consumed at poll().
    std::deque<std::size_t> staged_size_;
    std::deque<IoError> staged_size_err_;
    std::deque<bool> staged_void_ok_;
    std::deque<IoError> staged_void_err_;
    // Pointer-keyed cancel requests resolved to SlotHandles at cancel() time.
    // Applied at poll() before FIFO staging (targeted cancel takes precedence).
    std::deque<detail::SlotHandle> targeted_size_;
    std::deque<detail::SlotHandle> targeted_void_;
    std::unordered_map<std::uint32_t, Binding> bindings_;

    // Auto-complete mode state.
    enum class Auto : std::uint8_t { off, bytes, err, short_then_full };
    Auto auto_mode_ = Auto::off;
    std::size_t auto_bytes_ = 0;
    IoError auto_err_{IoError::Code::backend_error};
    bool auto_short_used_ = false;
};

} // namespace sluice::async
