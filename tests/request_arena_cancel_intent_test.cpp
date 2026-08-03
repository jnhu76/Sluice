// Phase B — round-4 review regression: ADR Decision 11 best-effort cancel.
//
// ADR-explicit-io-request-contract (Accepted) Decision 11 + round-4 finding 1:
// cancel is BEST-EFFORT. A RUNNING blocking syscall records cancel INTENT
// (cancel_intent_) only; it does NOT store a terminal. The syscall's ordinary
// result, ordinary error, or valid interruption later competes for the
// terminal winner via record_terminal, which records the REAL result VERBATIM
// — an ordinary success is NOT secretly rewritten to canceled. A backend that
// CONFIRMS the cancellation actually took effect (a valid interruption, a
// cancel CQE winner) records TerminalResult::err(canceled) explicitly, and
// THAT call wins the terminal.
//
// The Phase B reference backends never enter `running` (they dispatch
// deterministically enqueued -> backend_ready), so the running-cancel path is
// exercised here by driving the arena mechanism directly through the
// mark_running() dispatch seam (design §9). This is the test the round-4
// review identified as missing: without it, the cancel-intent substitution
// bug (record_terminal rewriting an ordinary success to canceled when intent
// was set) was invisible because no test observed a running op whose syscall
// later succeeded.
//
// Also covers:
//   - CancelDisposition is split into terminal_won (pending/enqueued cancel
//     stores the canceled terminal directly — Scheme B) and intent_recorded
//     (running cancel records intent only). The OLD single `requested` value
//     conflated "canceled terminal stored" with "intent only" and let the
//     backend tally canceled_ops at intent-request time (round-4 finding 1).
//   - cancel_intent_live() accessor observes the intent flag.
//   - A confirmed canceled terminal (record_canceled on a running slot whose
//     intent was set) records the canceled terminal and consumes the intent.
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/result.hpp>

#include "harness.hpp"

#include <cstddef>
#include <cstdint>

using sluice::Result;
using sluice::async::detail::CancelDisposition;
using sluice::async::detail::ContextIdentity;
using sluice::async::detail::OperationKind;
using sluice::async::detail::RequestArena;
using sluice::async::detail::RequestState;
using sluice::async::detail::SlotHandle;
using sluice::async::detail::TerminalResult;

SLUICE_MAIN()

namespace {
void noop_binding_publish(void*, const TerminalResult&) noexcept {}

void install_noop_binding(RequestArena& arena, SlotHandle h) {
    static int dummy_completion = 0;
    auto r = arena.install_publication_binding(h, &dummy_completion, 0, &noop_binding_publish);
    SLUICE_CHECK_MSG(r.has_value(), "noop binding install must succeed on a prepared slot");
}

struct NoopSink : sluice::async::detail::SynchronousReadySink {
    void on_ready(sluice::async::detail::ReadyEvent) noexcept override {}
};

// A publication thunk that captures the TerminalResult the arena published, so
// a test can assert the EXACT result that reached reap (round-4 finding 1: the
// bug rewrote an ordinary success to canceled; this captures the rewritten
// value and the assertion catches it). The capture outlives the reap call.
struct CapturedTerminal {
    bool published = false;
    bool is_error = false;
    std::uint64_t bytes = 0;
    sluice::IoError error{};
};
void capturing_publish(void* raw, const TerminalResult& t) noexcept {
    auto* cap = static_cast<CapturedTerminal*>(raw);
    cap->published = true;
    cap->is_error = t.is_error;
    cap->bytes = t.bytes;
    cap->error = t.error;
}
void install_capturing_binding(RequestArena& arena, SlotHandle h, CapturedTerminal* cap) {
    auto r = arena.install_publication_binding(h, cap, 0, &capturing_publish);
    SLUICE_CHECK_MSG(r.has_value(), "capturing binding install must succeed");
}

// Drive a slot through reserve -> prepare -> install binding -> commit ->
// enqueue -> mark_running (the dispatch seam). Writes the live handle into
// `out` and returns true on success. (Returns a bool rather than SlotHandle so
// the SLUICE_CHECK early-return macro composes cleanly inside the helper.)
bool drive_to_running(RequestArena& arena, SlotHandle& out) {
    auto rh = arena.reserve();
    if (!rh.has_value()) return false;
    out = rh.value();
    if (!arena.prepare(out, OperationKind::read, {}).has_value()) return false;
    install_noop_binding(arena, out);
    if (!arena.commit(out).has_value()) return false;
    if (arena.enqueue(out) != sluice::async::detail::EnqueueOutcome::enqueued) return false;
    if (!arena.mark_running(out)) return false;  // enqueued -> running
    return arena.state_of(out.slot) == RequestState::running;
}
}  // namespace

// ---- running cancel records INTENT only (no terminal stored) ----------------
// Decision 11: cancel on a running slot returns intent_recorded and sets
// cancel_intent_; it does NOT store a terminal, does NOT transition to
// backend_ready, and does NOT push the ready-ring. The slot stays running and
// accepted_outstanding stays 1.
SLUICE_TEST_CASE(running_cancel_records_intent_only_no_terminal) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    SlotHandle h{};
    SLUICE_CHECK_MSG(drive_to_running(arena, h), "drive_to_running must reach the running state");

    SLUICE_CHECK(arena.cancel(h) == CancelDisposition::intent_recorded);

    // Intent recorded but NO terminal stored, NO state transition.
    SLUICE_CHECK(arena.cancel_intent_live(h.slot));
    SLUICE_CHECK(!arena.terminal_stored(h.slot));
    SLUICE_CHECK(arena.state_of(h.slot) == RequestState::running);
    SLUICE_CHECK(arena.backend_ready_count() == 0);  // not on the ready-ring
    SLUICE_CHECK(arena.accepted_outstanding() == 1);
    SLUICE_CHECK(arena.slot_in_use() == 1);

    // A reap now publishes nothing (the slot is not backend_ready).
    NoopSink sink;
    SLUICE_CHECK(arena.reap(sink) == 0);

    // Drain cleanly so the arena destructor is quiescent.
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(0)));
    SLUICE_CHECK(arena.reap(sink) == 1);
    arena.release_completed_binding(h);
}

// ---- THE KEY ROUND-4 REGRESSION: ordinary success wins over cancel intent ---
// record_terminal records the REAL result VERBATIM. A running op whose cancel
// recorded intent but whose syscall then succeeded records the ordinary
// success — NOT canceled. This is the bug the round-4 review found: the prior
// record_terminal substituted `canceled` for an ordinary success when
// cancel_intent_ was set, secretly turning best-effort cancel into cancel-wins.
// On the pre-fix code this test fails: the captured published result is
// canceled, not a 4-byte success.
SLUICE_TEST_CASE(running_cancel_intent_then_ordinary_success_wins_verbatim) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    // Use a CAPTURING binding so the test observes the exact TerminalResult
    // that reached reap (the bug would rewrite it to canceled).
    CapturedTerminal cap;
    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    SlotHandle h = rh.value();
    SLUICE_CHECK(arena.prepare(h, OperationKind::read, {}).has_value());
    install_capturing_binding(arena, h, &cap);
    SLUICE_CHECK(arena.commit(h).has_value());
    SLUICE_CHECK(arena.enqueue(h) == sluice::async::detail::EnqueueOutcome::enqueued);
    SLUICE_CHECK(arena.mark_running(h));
    SLUICE_CHECK(arena.state_of(h.slot) == RequestState::running);

    // Cancel records intent (best-effort). The syscall then succeeds with 4.
    SLUICE_CHECK(arena.cancel(h) == CancelDisposition::intent_recorded);
    SLUICE_CHECK(arena.cancel_intent_live(h.slot));

    // record_terminal wins the terminal and records the REAL result (4 bytes),
    // NOT canceled. The intent is consumed on any winner.
    SLUICE_CHECK(arena.record_terminal(h, TerminalResult::ok_bytes(4)));
    SLUICE_CHECK(!arena.cancel_intent_live(h.slot));  // consumed

    // Reap publishes the ordinary 4-byte success through the capturing binding.
    NoopSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    // Release the slot BEFORE asserting on the captured result so a regression
    // surfaces as a clean test failure (record_failure + exit 1) rather than a
    // destructor fail-fast masking the message.
    arena.release_completed_binding(h);

    SLUICE_CHECK(cap.published);
    SLUICE_CHECK_MSG(!cap.is_error, "ordinary success must NOT be rewritten to an error");
    SLUICE_CHECK_MSG(cap.bytes == 4, "the real 4-byte success is the terminal, not canceled");
}

// ---- confirmed canceled terminal wins on a running slot (valid interrupt) ----
// A backend that CONFIRMS the cancellation actually took effect records
// TerminalResult::err(canceled) explicitly via record_canceled. THAT call wins
// the terminal — it is the confirmed canceled winner, not the intent. This is
// the ONLY way a running op reaches a canceled terminal (the intent alone does
// not promise one).
SLUICE_TEST_CASE(running_cancel_intent_then_confirmed_canceled_wins) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    CapturedTerminal cap;
    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    SlotHandle h = rh.value();
    SLUICE_CHECK(arena.prepare(h, OperationKind::read, {}).has_value());
    install_capturing_binding(arena, h, &cap);
    SLUICE_CHECK(arena.commit(h).has_value());
    SLUICE_CHECK(arena.enqueue(h) == sluice::async::detail::EnqueueOutcome::enqueued);
    SLUICE_CHECK(arena.mark_running(h));

    SLUICE_CHECK(arena.cancel(h) == CancelDisposition::intent_recorded);
    SLUICE_CHECK(arena.cancel_intent_live(h.slot));

    // A confirmed interruption: the backend records the canceled terminal
    // explicitly (e.g. an io_uring cancel CQE winner).
    SLUICE_CHECK(arena.record_canceled(h));
    SLUICE_CHECK(!arena.cancel_intent_live(h.slot));  // consumed by the winner
    SLUICE_CHECK(arena.terminal_stored(h.slot));
    SLUICE_CHECK(arena.state_of(h.slot) == RequestState::backend_ready);
    SLUICE_CHECK(arena.backend_ready_count() == 1);

    NoopSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    arena.release_completed_binding(h);
    // The confirmed canceled terminal reached reap verbatim.
    SLUICE_CHECK(cap.published);
    SLUICE_CHECK_MSG(cap.is_error, "confirmed cancel publishes an error terminal");
    SLUICE_CHECK_MSG(cap.error.code == sluice::IoError::Code::canceled,
                     "the published error is canceled");
}

// ---- running cancel then ordinary ERROR wins verbatim (not forced canceled) --
// Symmetric to the success case: a running op whose cancel recorded intent but
// whose syscall then failed with a real error records that real error, NOT
// canceled. The intent is consumed; the ordinary error is the terminal.
SLUICE_TEST_CASE(running_cancel_intent_then_ordinary_error_wins_verbatim) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    CapturedTerminal cap;
    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    SlotHandle h = rh.value();
    SLUICE_CHECK(arena.prepare(h, OperationKind::read, {}).has_value());
    install_capturing_binding(arena, h, &cap);
    SLUICE_CHECK(arena.commit(h).has_value());
    SLUICE_CHECK(arena.enqueue(h) == sluice::async::detail::EnqueueOutcome::enqueued);
    SLUICE_CHECK(arena.mark_running(h));

    SLUICE_CHECK(arena.cancel(h) == CancelDisposition::intent_recorded);

    // The syscall fails with a real I/O error (not a cancel interruption).
    auto backend_err = TerminalResult::err(
        sluice::IoError{sluice::IoError::Code::backend_error});
    SLUICE_CHECK(arena.record_terminal(h, backend_err));
    SLUICE_CHECK(!arena.cancel_intent_live(h.slot));  // consumed
    SLUICE_CHECK(arena.terminal_stored(h.slot));

    NoopSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    arena.release_completed_binding(h);
    SLUICE_CHECK(cap.published);
    SLUICE_CHECK_MSG(cap.is_error, "ordinary error terminal published");
    SLUICE_CHECK_MSG(cap.error.code == sluice::IoError::Code::backend_error,
                     "the real backend_error is the terminal, NOT rewritten to canceled");
}

// ---- a second cancel after intent is already_terminal-free but idempotent ---
// A second cancel on the same running slot re-records intent (idempotent) and
// stays intent_recorded. It does NOT stack terminals or push the ready-ring.
SLUICE_TEST_CASE(running_cancel_intent_idempotent_second_call) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    SlotHandle h{};
    SLUICE_CHECK_MSG(drive_to_running(arena, h), "drive_to_running must reach the running state");

    SLUICE_CHECK(arena.cancel(h) == CancelDisposition::intent_recorded);
    SLUICE_CHECK(arena.cancel(h) == CancelDisposition::intent_recorded);
    SLUICE_CHECK(arena.cancel_intent_live(h.slot));
    SLUICE_CHECK(!arena.terminal_stored(h.slot));
    SLUICE_CHECK(arena.state_of(h.slot) == RequestState::running);
    SLUICE_CHECK(arena.backend_ready_count() == 0);

    // Drain.
    SLUICE_CHECK(arena.record_canceled(h));
    NoopSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    arena.release_completed_binding(h);
}

// ---- mark_running on a backend_ready slot backs off (cancel won first) ------
// The dispatch path observes a slot that already went terminal (e.g. cancel won
// before dispatch). mark_running returns false WITHOUT starting the syscall;
// the dispatch backs off (losers do not publish, ADR Decision 12).
SLUICE_TEST_CASE(mark_running_backs_off_on_backend_ready) {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    SlotHandle h = rh.value();
    SLUICE_CHECK(arena.prepare(h, OperationKind::read, {}).has_value());
    install_noop_binding(arena, h);
    SLUICE_CHECK(arena.commit(h).has_value());
    // Cancel wins the terminal before enqueue/dispatch (Scheme B, pending).
    SLUICE_CHECK(arena.cancel(h) == CancelDisposition::terminal_won);
    SLUICE_CHECK(arena.state_of(h.slot) == RequestState::backend_ready);

    // enqueue observes backend_ready -> successful no-op (acks the pin).
    SLUICE_CHECK(arena.enqueue(h) == sluice::async::detail::EnqueueOutcome::terminal_noop);
    // mark_running now backs off: the slot is already terminal.
    SLUICE_CHECK(!arena.mark_running(h));
    SLUICE_CHECK(arena.state_of(h.slot) == RequestState::backend_ready);

    NoopSink sink;
    SLUICE_CHECK(arena.reap(sink) == 1);
    arena.release_completed_binding(h);
}
