// Phase C2e — AsyncIoContext::wait_one CONTEXT-LEVEL detectors
// (Issue #68 rows 15-16 mutant M12 + round-3 P0 mutant D4-RM13 evidence).
//
// The L1 production split-wait path (AsyncIoContext::wait_one, issue #67):
//
//   snapshot -> poll (serialized reap) -> wait_for_change -> interrupted
//   -> ONE final poll (the interrupted-branch final poll) -> return
//
// The final poll closes the interrupt-vs-final-ready race: a request that
// becomes backend-ready in the window between the control wake and the
// context's return is reaped there. The C2e suite's deterministic
// interrupt-window evidence previously covered only the BACKEND-level
// wait_one (tp_c2e_interrupt_final_reap_closes_ready_race, ThreadPoolBackend
// raw wait_one + ControlWakeFinalReapPauseGate) — no detector covered the
// CONTEXT's final poll: deleting it left every existing C2e case GREEN.
//
// This test closes that evidence gap with a TEST-ONLY split-wait backend +
// wait source (public AsyncBackend / BackendWaitSource interfaces — no
// production context field, no new seam): the wait source pauses AFTER
// observing a control interrupt and BEFORE returning `interrupted` to the
// context; while paused, the test records backend-ready. The context's
// interrupted-branch final poll is then the ONLY path that can reap it:
// deleting that poll (mutant M12) makes wait_one return 0 here —
// deterministic RED on the real L1 production context code.
//
// The interleaving is fully gated (parked flag, paused-after-interrupt flag,
// explicit resume) — no sleep_for, no timing proof.
//
// D4-RM13 (P0, round-3): the INTER-ITERATION control-wake detector. The
// control baseline belongs to the whole external wait_one() invocation, not
// one internal progress iteration. A control wake that lands between
// wait_for_change() returning `progress` and the next internal snapshot used
// to be absorbed into the fresh snapshot (rebaselined away), drained, and
// reparked forever. This file's second case pins that invariant on the REAL
// production context loop with a test-only pause at the exact window
// (AsyncIoContext::WaitSourceProgressPauseGate, compiled out of production):
//
//   T1: wait_one begins, Cbase=C0, first wait_for_change returns progress
//       -> PAUSED before the next snapshot
//   T2: consumes A (the next poll finds nothing); B remains blocked
//   controller: interrupt_all(), C0 -> C1
//   resume T1: progress baseline may refresh; control baseline stays C0
//       -> wait_for_change sees C1 != C0 -> interrupted -> final poll -> 0
//
// Under the D4-RM13 mutant (control rebaselined per internal iteration) the
// fresh snapshot captures C1, no epoch delta remains, the stale control
// eventfd is drained, and T1 reparks on B forever (bounded watchdog -> RED).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(5);

// Bounded wait on an atomic flag. Returns true when the flag became true.
bool wait_flag(std::atomic<bool>& flag, std::chrono::steady_clock::time_point deadline) {
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// Join a thread with a bounded deadline: the thread publishes `done` when
// finished; the joiner waits for the flag (bounded), then really joins.
bool join_bounded(std::thread& t, std::atomic<bool>& done,
                  std::chrono::steady_clock::time_point deadline) {
    if (!t.joinable()) return true;
    while (!done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    t.join();
    return true;
}

// Test-only split-wait backend (option 2 of the B3 review direction). The
// backend simulates exactly the state the CONTEXT's split-wait protocol
// observes: an outstanding request count, a poll() that reaps a
// backend-ready request (returns 1 and consumes it), and a wait source whose
// wait_for_change() pauses AFTER observing a control interrupt and BEFORE
// returning `interrupted` — the deterministic window in which the test
// records backend-ready. The context's interrupted-branch final poll is the
// only code path that can turn that window into a reaped count.
class SplitWaitProbeBackend : public AsyncBackend {
public:
    // A request is outstanding (accepted, not yet terminal).
    std::atomic<std::size_t> outstanding_ops{0};
    // A request is backend-ready (recorded by the test inside the window);
    // poll() reaps it exactly once.
    std::atomic<bool> ready_pending{false};

    // Test-only wait source. Mirrors the ReadyWaitSource epoch protocol
    // (snapshot -> wait_for_change -> interrupt_all) with the added
    // deterministic pause: when a control interrupt is observed, the source
    // blocks at the `paused` gate before returning `interrupted` to the
    // context, so the test can record backend-ready in the exact window.
    class ProbeWaitSource : public BackendWaitSource {
    public:
        std::atomic<bool> parked{false};   // about to block in the cv wait
        std::atomic<bool> paused{false};   // interrupt observed, not yet returned
        std::atomic<bool> resume{false};   // test sets to release the return

        BackendWaitToken snapshot() const noexcept override {
            std::lock_guard<std::mutex> lk(mtx_);
            return BackendWaitToken{progress_epoch_, control_epoch_};
        }

        BackendWakeReason wait_for_change(BackendWaitToken observed) noexcept override {
            std::unique_lock<std::mutex> lk(mtx_);
            parked.store(true, std::memory_order_release);
            cv_.wait(lk, [&] {
                return progress_epoch_ != observed.progress_generation ||
                       control_epoch_ != observed.control_generation;
            });
            if (control_epoch_ != observed.control_generation) {
                // Deterministic B3 window: pause AFTER the interrupt is
                // observed, BEFORE returning `interrupted` to
                // AsyncIoContext::wait_one. The test records backend-ready
                // here; only the context's interrupted-branch final poll can
                // reap it (mutant M12 detector).
                // `lk` is intentionally HELD across this spin: this is a
                // test-only backend and the test never calls
                // interrupt_all()/snapshot() while the spin is live, so no
                // concurrent lock attempt can block on it. Holding the lock
                // keeps the pause deterministic (the paused observation and
                // the later resume store stay ordered under one mutex).
                paused.store(true, std::memory_order_release);
                while (!resume.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                return BackendWakeReason::interrupted;
            }
            return BackendWakeReason::progress;
        }

        void interrupt_all() noexcept override {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                ++control_epoch_;
            }
            cv_.notify_all();
        }

        void signal_progress() noexcept {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                ++progress_epoch_;
            }
            cv_.notify_all();
        }

    private:
        mutable std::mutex mtx_;
        std::condition_variable cv_;
        std::uint64_t progress_epoch_ = 0;
        std::uint64_t control_epoch_ = 0;
    };

    ProbeWaitSource ws;

    // The reap authority here: consumes a pending ready exactly once.
    std::size_t poll() override {
        if (ready_pending.exchange(false)) {
            return 1;
        }
        return 0;
    }

    std::size_t outstanding() const noexcept override {
        return outstanding_ops.load();
    }

    BackendWaitSource* wait_source() noexcept override { return &ws; }

    // Not used: AsyncIoContext takes the split-wait path (wait_source !=
    // null), so backend wait_one is never called by the context.
    Result<std::size_t> wait_one() override { return Result<std::size_t>{std::size_t{0}}; }

    // Submit is not exercised by this detector (the probe drives the wait
    // protocol only); reject synchronously if ever called.
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    void cancel(Completion<std::size_t>&) override {}
    void cancel(Completion<void>&) override {}
};

}  // namespace

SLUICE_MAIN()

// The terminal lands in the EXACT window between the interrupted control wake
// and the CONTEXT's interrupted-branch final poll (deterministic via the
// test-only wait source's pause-after-interrupt gate). wait_one must return
// the reaped count (1), NOT 0 — the control interrupt must not swallow the
// final ready. This is the deterministic detector for the "the context drops
// the interrupted-branch final poll" mutation (M12): without the final poll,
// wait_one returns 0 here.
SLUICE_TEST_CASE(ctx_wait_one_interrupt_final_poll_closes_ready_race) {
    auto backend = std::make_unique<SplitWaitProbeBackend>();
    SplitWaitProbeBackend* raw = backend.get();
    SplitWaitProbeBackend::ProbeWaitSource* ws = &backend->ws;
    // One request is in flight; nothing ready yet.
    raw->outstanding_ops.store(1);
    AsyncIoContext ctx(std::move(backend));

    Result<std::size_t> a_wait{std::size_t{0}};
    std::atomic<bool> a_finished{false};
    std::thread waiter([&] {
        a_wait = ctx.wait_one();
        a_finished.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    const char* fail_msg = nullptr;
    // The waiter must be PARKED (empty reap, about to block) BEFORE the
    // control interrupt, so the interrupt reaches the parked wait (a waiter
    // that snapshots the advanced control generation parks normally and is
    // never woken — a test-ordering error, not a production defect).
    if (!wait_flag(ws->parked, deadline)) {
        fail_msg = "waiter never entered the ready wait";
    } else if (a_finished.load(std::memory_order_acquire)) {
        fail_msg = "waiter must still be parked";
    }

    // Control interrupt: the wait source wakes, observes it, and pauses
    // before returning `interrupted` to the context.
    if (fail_msg == nullptr) {
        ctx.interrupt_backend_waiters();
        if (!wait_flag(ws->paused, deadline)) {
            fail_msg = "wait source never paused after observing the interrupt";
        } else if (a_finished.load(std::memory_order_acquire)) {
            fail_msg = "waiter must be paused, not finished";
        }
    }

    // NOW the request becomes backend-ready — inside the interrupt window,
    // while the context's wait_for_change call has not yet returned. The
    // context's interrupted-branch final poll is the ONLY path that can reap
    // it.
    if (fail_msg == nullptr) {
        raw->ready_pending.store(true);
        ws->resume.store(true, std::memory_order_release);
        if (!wait_flag(a_finished, deadline)) {
            fail_msg = "waiter never finished after the pause release";
        } else if (!a_wait.has_value() || a_wait.value() != 1) {
            fail_msg = "wait_one must return the reaped count (1), not 0 — "
                       "the context's interrupted-branch final poll must not "
                       "be dropped";
        } else if (raw->ready_pending.load()) {
            fail_msg = "the final poll must have consumed the ready";
        }
    }

    // cleanup (both paths): release the pause (idempotent), join bounded,
    // clear outstanding before the context destructor's L11 check.
    ws->resume.store(true, std::memory_order_release);
    (void)join_bounded(waiter, a_finished, deadline);
    raw->outstanding_ops.store(0);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---------------------------------------------------------------------------
// D4-RM13 — inter-iteration control-wake detector (the round-3 P0).
//
// Scenario (fully deterministic, no timing proof):
//
//   T1: external wait_one begins, Cbase = C0
//       poll -> 0, outstanding > 0
//       wait_for_change({P0, C0}) -> parks in the ready wait
//   controller: signal_progress() -> progress_epoch P0 -> P1, cv notify
//   T1: wait_for_change returns progress (P1 != P0, C still C0)
//       -> PAUSED at the context's WaitSourceProgressPauseGate (the exact
//          inter-iteration window: wait_for_change returned, the next
//          snapshot has not run)
//   controller: consume A (ready_pending set + signal_progress so T2's poll
//       returns 1; here we instead just set outstanding to leave one blocked
//       op, and drive a progress that T1 will NOT see because the next poll
//       finds nothing ready)
//   controller: interrupt_all() -> control_epoch C0 -> C1
//   resume T1: the next snapshot sees {P_now, C1}. The fix pins control to
//       Cbase=C0, so wait_for_change({P_now, C0}) sees C1 != C0 and returns
//       interrupted -> final poll -> 0. Under the D4-RM13 mutant (control
//       rebaselined per internal iteration) the snapshot absorbs C1 into the
//       observed token, the stale control event is drained, and wait_for_
//       change finds no delta and reparks forever (bounded watchdog -> RED).
//
// The control wake is delivered AFTER the first progress wake has already
// returned to the context — this is the inter-iteration window the round-2
// UringWaitSource fix could not close (the race is past wait_for_change's
// return), so the authority lives at the context.
//
// The waiter keeps one blocked op outstanding so it parks (and stays parked
// under the mutant); a clean teardown resets outstanding to 0 before the L11
// destructor check.
SLUICE_TEST_CASE(ctx_wait_one_inter_iteration_control_wake_not_lost) {
    auto backend = std::make_unique<SplitWaitProbeBackend>();
    SplitWaitProbeBackend* raw = backend.get();
    SplitWaitProbeBackend::ProbeWaitSource* ws = &backend->ws;
    // One blocked op keeps outstanding > 0 so the waiter parks.
    raw->outstanding_ops.store(1);
    AsyncIoContext ctx(std::move(backend));

    AsyncIoContext::WaitSourceProgressPauseGate gate;
    ctx.set_wait_source_progress_pause_gate_for_test(&gate);

    Result<std::size_t> w{std::size_t{999}};
    std::atomic<bool> done{false};
    std::thread waiter([&] {
        w = ctx.wait_one();
        done.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    const char* fail_msg = nullptr;

    // Step 1: T1 must park in the ready wait (empty reap, about to block).
    if (!wait_flag(ws->parked, deadline)) {
        fail_msg = "T1 never entered the ready wait";
    }

    // Step 2: drive a PROGRESS wake so wait_for_change returns progress. T1
    // then reaches the context-level inter-iteration pause gate.
    if (fail_msg == nullptr) {
        ws->signal_progress();
        if (!wait_flag(gate.paused, deadline)) {
            fail_msg = "T1 never reached the inter-iteration pause after a "
                       "progress wake (the pause seam may be mis-wired)";
        } else if (done.load(std::memory_order_acquire)) {
            fail_msg = "T1 must still be paused, not finished";
        }
    }

    // Step 3: while T1 is paused at the inter-iteration window, fire a
    // CONTROL wake. No ready op is recorded, so T1's next poll reaps nothing
    // and the blocked op stays outstanding.
    if (fail_msg == nullptr) {
        ctx.interrupt_backend_waiters();
    }

    // Step 4: release T1. Under the fix the control baseline (C0) is
    // preserved across the internal loop, so wait_for_change sees C1 != C0
    // and returns interrupted -> final poll -> 0. Under the D4-RM13 mutant
    // (control rebaselined per internal iteration) the fresh snapshot absorbs
    // C1, no delta remains, and T1 reparks on the blocked op forever.
    //
    // NOTE on the second pause: the ProbeWaitSource pauses AGAIN when its
    // wait_for_change observes the interrupt and is about to return
    // `interrupted` (this is the close-the-interrupt-vs-final-ready-race gate
    // reused by the probe — it does NOT affect which branch wait_for_change
    // takes). For D4-RM13 the load-bearing observation is that T1 REACHES
    // this second pause at all: under the mutant T1 never re-enters
    // wait_for_change with a control delta (it absorbed C1 into the token and
    // reparks), so `ws->paused` never fires the SECOND time. We wait for the
    // second ws->paused here, then release the probe so T1 can return
    // `interrupted` -> final poll -> 0.
    if (fail_msg == nullptr) {
        gate.resume.store(true, std::memory_order_release);
        // The first wait_for_change returns `progress` (P0 -> P1), which takes
        // the progress branch and does NOT set ws->paused (only the interrupt
        // branch sets paused). The reset is therefore DEFENSIVE: it clears any
        // stale observation so wait_flag(ws->paused, ...) below observes ONLY
        // the second pause — the interrupt-branch pause that proves T1 reached
        // the inter-iteration control-wake recheck.
        ws->paused.store(false, std::memory_order_release);
        if (!wait_flag(ws->paused, deadline)) {
            fail_msg = "T1 never returned after resume — the inter-iteration "
                       "control wake was rebaselined away and T1 reparked "
                       "(D4-RM13 mutant: control_generation pinned per loop)";
        }
    }

    // Release the probe's interrupt-window pause so T1 can finish (interrupted
    // -> final poll -> 0). The probe gate is the LAST thing holding T1.
    if (fail_msg == nullptr) {
        ws->resume.store(true, std::memory_order_release);
        if (!wait_flag(done, deadline)) {
            fail_msg = "T1 never finished after the probe release";
        } else if (!w.has_value() || w.value() != 0) {
            fail_msg = "wait_one must return 0 (interrupted, nothing "
                       "fabricated) after the inter-iteration control wake";
        }
    }

    // cleanup (both paths): release the pauses (idempotent), clear the
    // inter-iteration gate, and FORCE-wake any stranded parker before joining
    // (the D4-RM13 mutant leaves T1 reparked on the blocked op; without a
    // force-wake the waiter would strand and the std::thread destructor would
    // terminate instead of reporting the failure cleanly). The cleanup uses a
    // FRESH deadline so a 5s RED observation does not starve the join. clear
    // outstanding before the L11 destructor check.
    //
    // Ordering note: ws->resume MUST be set true BEFORE ws->signal_progress()
    // or ws->interrupt_all(). ProbeWaitSource::wait_for_change spins on
    // `resume` while HOLDING mtx_ (the test-only lock-during-pause exception),
    // and signal_progress()/interrupt_all() both acquire mtx_ to bump their
    // epoch + notify the cv. If resume were set AFTER the wake, a parker still
    // spinning under mtx_ would block the wake's lock acquisition and strand —
    // so the resume-before-wake order is an intentional exception to the normal
    // "set persistent state before signaling" expectation, and it is preserved
    // here.
    gate.resume.store(true, std::memory_order_release);
    ws->resume.store(true, std::memory_order_release);
    ws->signal_progress();  // nudge a stranded parker's cv predicate
    ws->interrupt_all();    // bump control so a parked cv returns
    ctx.set_wait_source_progress_pause_gate_for_test(nullptr);
    const auto cleanup_deadline =
        std::chrono::steady_clock::now() + kWaitTimeout;
    (void)join_bounded(waiter, done, cleanup_deadline);
    raw->outstanding_ops.store(0);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// Phase G review P1b (PR #108): the bounded-park capability contract. A
// third-party BackendWaitSource that implements only the one-argument
// unbounded wait_for_change (like this file's ProbeWaitSource — it overrides
// neither the bounded overload nor supports_bounded_wait) must NEVER receive
// a finite park cap as a silently-discarded bound: AsyncIoContext::
// wait_one(max_park) rejects it synchronously with not_supported BEFORE any
// park or accounting side effect, while the capability queries report the
// truthful split-without-bounded state. The unbounded no-argument form keeps
// its documented behavior (empty wait -> 0, no park). No threads, no timing.
SLUICE_TEST_CASE(ctx_wait_one_bounded_cap_requires_capability) {
    auto backend = std::make_unique<SplitWaitProbeBackend>();
    SplitWaitProbeBackend::ProbeWaitSource* ws = &backend->ws;
    AsyncIoContext ctx(std::move(backend));

    if (!ctx.has_split_wait_capability()) SLUICE_FAIL("split capability expected");
    if (ctx.has_bounded_split_wait_capability()) {
        SLUICE_FAIL("probe wait source must not report bounded capability");
    }

    // Finite cap without the bounded transport: synchronous not_supported —
    // never a silently unbounded park (the base wait_for_change would
    // discard the bound and park past the caller's deadline).
    auto bounded = ctx.wait_one(std::chrono::milliseconds(1));
    if (bounded.has_value()) {
        SLUICE_FAIL("finite cap on a capability-less wait source must not succeed");
    }
    if (bounded.error().code != IoError::Code::not_supported) {
        SLUICE_FAIL("finite cap rejection must be not_supported");
    }
    // The rejection happens BEFORE the observe phase: the source never
    // parked (its parked flag is set only inside wait_for_change).
    if (ws->parked.load(std::memory_order_acquire)) {
        SLUICE_FAIL("rejected bounded wait must not park the wait source");
    }

    // The unbounded no-argument form is unchanged: nothing outstanding ->
    // the empty wait is a no-progress boundary, returned as 0 without
    // parking.
    auto unbounded = ctx.wait_one();
    if (!unbounded.has_value() || unbounded.value() != 0) {
        SLUICE_FAIL("unbounded empty wait must return 0");
    }
    if (ws->parked.load(std::memory_order_acquire)) {
        SLUICE_FAIL("empty wait must not park the wait source");
    }
}
