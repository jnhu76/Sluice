// Phase C2e — AsyncIoContext::wait_one interrupted-branch final poll detector
// (Issue #68 rows 15-16; mutant M12 evidence).
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
