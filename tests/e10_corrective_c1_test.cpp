// e10_corrective_c1_test — E10-CORRECTIVE C1 external wake-domain classification
// regression (sluice-CORE-E10).
//
// C1 (accepted defect): Scheduler::await_wait increments/owns waiting_waitq_count_,
// which participates in classify_locked() (MW-S3) but did NOT participate in
// external_wake_possible_locked(). A WaitQueue wait IS externally resolvable:
// wake_wait_one / cancel_wait are callable from any thread and both reach
// signal_wake_locked (the unified Scheduler wake source) via route_runnable_locked
// — exactly the same wake capability as a ready-flag wait. The pre-corrective
// classifier therefore under-classified an E10 wait: a Live run could park/STALLED
// on a source that cannot observe the wait's resolution.
//
// T1 (pre-corrective counterexample): a Live run with a single WaitQueue waiter.
// An external thread calls Scheduler::wake_wait_one. Against uncorrected 0debd21,
// external_wake_possible_locked() returned false, so:
//   worker: MW-S3 + live + !external_wake_possible -> idle++ -> terminate STALLED
// the run returns immediately and the external wake is delivered to a dead run
// (the waiter is stranded). This test's assertion (waiter resumes exactly once)
// FAILS on uncorrected E10 and PASSES after the C1 correction.
//
// T3 (corrective regression): after the correction, the Live run stays resident
// long enough for the external wake_wait_one to resume the waiter exactly once.
//
// This is the same shape as e9_external_wake_test T1 (external Future wake, no
// caller re-entry) but for an E10 WaitQueue wait resolved from an external thread.
//
// Gated to x86_64 (fiber_ctx::supported).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/result.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace sluice::async;
using sluice::Result;

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// A backend that never completes anything (outstanding stays 0: no submit). The
// parked worker's wake must come from the external WaitQueue resolver, NOT the
// backend. poll/wait_one are no-ops so MW stays at S3 (unresolved wait), forcing
// the run-lifetime decision (park vs STALLED) to depend SOLELY on
// external_wake_possible_locked().
class IdleBackend : public AsyncBackend {
public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override { return {}; }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override { return {}; }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return 0; }
    void cancel(Completion<std::size_t>&) override {}
    void cancel(Completion<void>&) override {}
    std::size_t outstanding() const noexcept override { return 0; }
};
}  // namespace

// T1/T3: a Live run with one E10 waiter; an external thread resolves it via
// wake_wait_one. The waiter MUST resume exactly once. Against uncorrected E10
// the run STALLED before the external wake could land (the regression).
SLUICE_TEST_CASE(e10_corrective_c1_external_waitq_wake_live) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode node;
    std::atomic<int> entries{0};
    std::atomic<bool> waiter_registered{false};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);  // before await
        waiter_registered.store(true, std::memory_order_release);
        sched.await_wait(q, node);
        entries.fetch_add(1, std::memory_order_acq_rel);  // after resume
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);

    // External resolver thread: once the waiter has registered, wake it from
    // OUTSIDE any worker thread. This is the C1 wake source — a WaitQueue wait
    // resolved by wake_wait_one, which reaches signal_wake_locked.
    std::thread ext([&] {
        while (!waiter_registered.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        // Give the worker a beat to reach MW-S3 + the park decision, then wake.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        bool woke = sched.wake_wait_one(q);
        SLUICE_CHECK_MSG(woke, "external wake_wait_one delivered a wake");
    });

    // Live run: the run must stay resident (park) because an externally-resolvable
    // wait is registered, so the external wake is observable. Against uncorrected
    // E10 this returned STALLED before the wake could land.
    sched.run_live(1);
    ext.join();

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter resumed exactly once via external wake");
    SLUICE_CHECK_MSG(node.was_woken(), "node resolved Woken by external wake_wait_one");
    SLUICE_CHECK_MSG(fwait.state() == FiberState::done, "waiter reached done");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// T1b/T3b: symmetric via cancel_wait from an external thread (Cancelled outcome).
SLUICE_TEST_CASE(e10_corrective_c1_external_waitq_cancel_live) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    WaitQueue q;
    WaitNode node;
    std::atomic<int> entries{0};
    std::atomic<bool> waiter_registered{false};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        waiter_registered.store(true, std::memory_order_release);
        sched.await_wait(q, node);
        entries.fetch_add(1, std::memory_order_acq_rel);
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);

    std::thread ext([&] {
        while (!waiter_registered.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        bool cancelled = sched.cancel_wait(q, node);
        SLUICE_CHECK_MSG(cancelled, "external cancel_wait won");
    });

    sched.run_live(1);
    ext.join();

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter resumed exactly once via external cancel");
    SLUICE_CHECK_MSG(node.was_cancelled(), "node resolved Cancelled by external cancel_wait");
    SLUICE_CHECK_MSG(fwait.state() == FiberState::done, "waiter reached done");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

SLUICE_MAIN()
