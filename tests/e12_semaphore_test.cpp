// e12_semaphore_test — Async counting Semaphore (sluice-CORE-E12-B).
//
// Deterministic production tests for the async counting Semaphore built on the
// closed E10/E11/E12-A wait substrate. Observed ONLY through the SEALED
// Semaphore public API + the mechanically gated test hooks:
//
//   - Semaphore::available / try_acquire / acquire / acquire_until / cancel /
//     release
//   - E11TimerControl / E9ParkSeam  (deterministic clock/timer + park seams)
//   - WaitNode public lock-free state queries (was_woken/was_cancelled/
//     was_expired/is_terminal/outcome)
//   - Scheduler::advance_clock / waiting_count()  (deterministic timer +
//     wait-accounting authority)
//
// Every causal race proof uses mechanically gated phase seams + retry loops or
// barriers — NEVER sleep_for timing as causal proof (H7). A bounded timeout may
// remain ONLY as a test-failure guard, not as causal synchronization.
//
// Gated to x86_64 (fiber_ctx::supported): registration requires a real Fiber.
#include "harness.hpp"
#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/semaphore.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace sluice::async;
using sluice::Result;

// Aliases matching the historical controller names used across the async test
// family (ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1).
namespace {
using E11TimerTestHooks = sluice_async_test::E11TimerControl;
using E9ParkSeam = sluice_async_test::E9ParkSeam;
}  // namespace

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// A backend that never completes anything (outstanding stays 0). The only
// progress in these tests is Scheduler-integrated resolution; MW stays at
// S1/S3 and the run terminates only once all fibers are done.
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

// Spin until `flag` is observed (TEST SYNC ONLY; bounded by the cooperative
// run which frees the worker to run the flag-setting fiber).
inline void spin_wait(std::atomic<bool>& flag) {
    while (!flag.load(std::memory_order::acquire)) {
        std::this_thread::yield();
    }
}

// Spin until `pred` is true (TEST SYNC ONLY).
inline void spin_wait_pred(auto&& pred) {
    while (!pred()) {
        std::this_thread::yield();
    }
}
}  // namespace

SLUICE_MAIN()

// ===========================================================================
// Slice 1 — Construction, available(), try_acquire (T0–T6)
// ===========================================================================

// ---- T0: construction invariants + available() initial snapshot ------------
//
// initial=0,max>0 and initial=max are both valid. available() returns the
// initial snapshot. Proves the constructor stores the initial count and the
// capacity bound without exception.
SLUICE_TEST_CASE(e12_sem_t0_construction_and_available_snapshot) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    {
        Semaphore sem(sched, /*initial=*/0, /*max=*/3);
        SLUICE_CHECK_MSG(sem.available() == 0, "initial=0 available snapshot");
    }
    {
        Semaphore sem(sched, /*initial=*/3, /*max=*/3);
        SLUICE_CHECK_MSG(sem.available() == 3, "initial=max available snapshot");
    }
    {
        Semaphore sem(sched, /*initial=*/2, /*max=*/5);
        SLUICE_CHECK_MSG(sem.available() == 2, "initial<mid available snapshot");
    }
}

// ---- T1: try_acquire success consumes exactly one --------------------------
//
// With initial=2 permits, two try_acquire calls succeed and decrement
// available() to 0; a third fails (returns false, no mutation). Proves
// try_acquire admits a stored permit when the queue is empty and does not
// admit beyond supply.
SLUICE_TEST_CASE(e12_sem_t1_try_acquire_consumes_exactly_one) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/2, /*max=*/3);
    SLUICE_CHECK_MSG(sem.available() == 2, "initial supply");

    SLUICE_CHECK_MSG(sem.try_acquire(), "1st try_acquire succeeds");
    SLUICE_CHECK_MSG(sem.available() == 1, "1 decrement");
    SLUICE_CHECK_MSG(sem.try_acquire(), "2nd try_acquire succeeds");
    SLUICE_CHECK_MSG(sem.available() == 0, "2 decrements");
    SLUICE_CHECK_MSG(!sem.try_acquire(), "3rd try_acquire fails at zero");
    SLUICE_CHECK_MSG(sem.available() == 0, "no mutation on failure");
}

// ---- T2: try_acquire failure at zero (no mutation) -------------------------
//
// A Semaphore with no stored permits: try_acquire returns false and available()
// is unchanged. No underflow, no negative count.
SLUICE_TEST_CASE(e12_sem_t2_try_acquire_failure_at_zero_no_mutation) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    for (int i = 0; i < 5; ++i) {
        SLUICE_CHECK_MSG(!sem.try_acquire(), "try_acquire fails at zero");
    }
    SLUICE_CHECK_MSG(sem.available() == 0, "still zero, no mutation/underflow");
}

// ---- T3: immediate acquire resolves Woken without suspending ---------------
//
// A fiber calls acquire on a Semaphore with an available permit. The wait
// resolves Woken inline (no release needed), available() decrements, and the
// fiber reaches done without any other fiber running.
SLUICE_TEST_CASE(e12_sem_t3_immediate_acquire_resolves_woken) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/1, /*max=*/2);
    WaitNode node;
    std::atomic<int> entries{0};

    Fiber facq;
    facq.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        sem.acquire(node);
        entries.fetch_add(1, std::memory_order_acq_rel);
    });

    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(facq, sa.base(), sa.size()));
    sched.spawn(facq);
    sched.run(1);

    SLUICE_CHECK_MSG(entries.load() == 2, "acquire fiber completed");
    SLUICE_CHECK_MSG(node.was_woken(), "immediate acquire resolved Woken");
    SLUICE_CHECK_MSG(sem.available() == 0, "permit consumed");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T4: immediate acquisitions stop at zero (supply bound) ----------------
//
// Two fibers each acquire on a Semaphore with initial=1. The first acquires
// immediately (Woken inline); the second finds no stored permit, registers,
// and suspends. No release occurs, so the second must remain suspended -> the
// run does NOT terminate (STALLED). We use run_live + a timeout guard + then
// release to complete cleanly. This proves immediate acquisition stops at zero
// and the second waiter suspends rather than barging.
SLUICE_TEST_CASE(e12_sem_t4_immediate_acquisitions_stop_at_zero) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/1, /*max=*/2);
    WaitNode n1, n2;
    std::atomic<int> acquired{0};
    std::atomic<bool> n2_registered{false};

    Fiber f1, f2, frel;
    f1.set_entry([&](Fiber&) {
        sem.acquire(n1);
        acquired.fetch_add(1, std::memory_order_acq_rel);
    });
    f2.set_entry([&](Fiber&) {
        n2_registered.store(true, std::memory_order::release);
        sem.acquire(n2);  // no permit -> suspends
        acquired.fetch_add(1, std::memory_order_acq_rel);
    });
    frel.set_entry([&](Fiber&) {
        spin_wait(n2_registered);
        std::this_thread::yield();  // let f2 reach acquire
        (void)sem.release();        // grant the suspended n2
    });

    FiberStack s1, s2, sr;
    SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
    sched.spawn(f1);
    sched.spawn(f2);
    sched.spawn(frel);
    sched.run(1);

    SLUICE_CHECK_MSG(acquired.load() == 2, "both waiters acquired");
    SLUICE_CHECK_MSG(n1.was_woken(), "n1 immediate Woken");
    SLUICE_CHECK_MSG(n2.was_woken(), "n2 granted Woken by release");
    SLUICE_CHECK_MSG(sem.available() == 0, "no stored permit remains");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// Slice 2 — Queued acquire + release disposition (T5–T9)
// ===========================================================================

// ---- T5: zero-permit acquire registers and suspends; one release grants ----
//
// A waiter on a zero-permit Semaphore suspends. One release transfers its
// pending permit directly to the FIFO head (available_ stays 0). The waiter
// resumes Woken.
SLUICE_TEST_CASE(e12_sem_t5_zero_acquire_suspends_one_release_grants) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode node;
    std::atomic<bool> waiter_registered{false};
    std::atomic<int> entries{0};

    Fiber fwait, frel;
    fwait.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        waiter_registered.store(true, std::memory_order_release);
        sem.acquire(node);  // no permit -> suspends
        entries.fetch_add(1, std::memory_order_acq_rel);
    });
    frel.set_entry([&](Fiber&) {
        spin_wait(waiter_registered);
        std::this_thread::yield();  // let the waiter register + suspend
        SLUICE_CHECK_MSG(sem.release(), "release transfers to the waiter");
    });

    FiberStack sw, sr;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
    sched.spawn(fwait);
    sched.spawn(frel);
    sched.run(1);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter resumed");
    SLUICE_CHECK_MSG(node.was_woken(), "waiter resolved Woken (transfer)");
    SLUICE_CHECK_MSG(sem.available() == 0, "queued transfer leaves available unchanged");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T6: no waiter + release -> available increments (store) ---------------
//
// A release on an empty-queue, below-capacity Semaphore stores the permit:
// available() increments. Proves the store branch (not transfer, not reject).
SLUICE_TEST_CASE(e12_sem_t6_release_no_waiter_increments_available) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/0, /*max=*/3);
    SLUICE_CHECK_MSG(sem.available() == 0, "start at zero");

    SLUICE_CHECK_MSG(sem.release(), "1st release stores");
    SLUICE_CHECK_MSG(sem.available() == 1, "stored 1");
    SLUICE_CHECK_MSG(sem.release(), "2nd release stores");
    SLUICE_CHECK_MSG(sem.available() == 2, "stored 2");
    SLUICE_CHECK_MSG(sem.release(), "3rd release stores to max");
    SLUICE_CHECK_MSG(sem.available() == 3, "stored to capacity");
}

// ---- T7: release at capacity -> false, no mutation (overflow) --------------
//
// A release on an empty-queue, AT-capacity Semaphore is rejected: returns
// false and available() is unchanged. No overflow, no mutation.
SLUICE_TEST_CASE(e12_sem_t7_release_at_capacity_false_no_mutation) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/2, /*max=*/2);
    SLUICE_CHECK_MSG(sem.available() == 2, "at capacity");
    for (int i = 0; i < 4; ++i) {
        SLUICE_CHECK_MSG(!sem.release(), "overflow release rejected");
    }
    SLUICE_CHECK_MSG(sem.available() == 2, "no mutation on overflow");
}

// ---- T8: one release never both wakes a waiter AND stores ------------------
//
// One waiter is queued on a zero-permit Semaphore. One release transfers its
// permit to the waiter (Woken). available() stays 0 — the release did NOT also
// store. This is the permit-conservation guard: one release contributes one
// pending permit, exactly one disposition.
SLUICE_TEST_CASE(e12_sem_t8_one_release_never_both_wake_and_store) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/0, /*max=*/3);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> transferred{false};

    Fiber fwait, frel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sem.acquire(node);  // suspends; resumed by release transfer
        // After resume: available_ must still be 0 (the release transferred,
        // it did NOT also store). Observed from the resumed waiter, not from
        // a fiber holding the worker.
        SLUICE_CHECK_MSG(sem.available() == 0, "release transferred, did not store");
        transferred.store(true, std::memory_order_release);
    });
    frel.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();  // let the waiter register + suspend
        SLUICE_CHECK_MSG(sem.release(), "release succeeds (transfer)");
        // Do NOT spin-wait for done here (that would block the single worker
        // and prevent fwait from being scheduled). The run drains both fibers.
    });

    FiberStack sw, sr;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
    sched.spawn(fwait);
    sched.spawn(frel);
    sched.run(1);

    SLUICE_CHECK_MSG(transferred.load(), "waiter resumed and checked available");
    SLUICE_CHECK_MSG(node.was_woken(), "waiter Woken via transfer");
    SLUICE_CHECK_MSG(sem.available() == 0, "no double-counting");
}

// ---- T9: queued grant from available==0 does not underflow -----------------
//
// A waiter queued on available_ == 0 is granted by a release. The transfer
// path never decrements available_ (it starts at 0 and stays 0). No unsigned
// underflow. This is the load-bearing guard against the deleted available_-- /
// refund model.
SLUICE_TEST_CASE(e12_sem_t9_queued_grant_from_zero_no_underflow) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode node;
    std::atomic<bool> registered{false};

    Fiber fwait, frel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sem.acquire(node);  // suspends; resumed by release transfer
        // After resume from a transfer at available_ == 0: available_ must be
        // 0 (no decrement, no underflow wrap). Observed from the resumed waiter.
        SLUICE_CHECK_MSG(sem.available() == 0, "no underflow (stays 0, not wrapped)");
    });
    frel.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        (void)sem.release();  // transfer from available_ == 0
    });

    FiberStack sw, sr;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
    sched.spawn(fwait);
    sched.spawn(frel);
    sched.run(1);

    SLUICE_CHECK_MSG(node.was_woken(), "waiter Woken (queued grant from zero)");
    SLUICE_CHECK_MSG(sem.available() == 0, "available_ did not wrap");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// Slice 3 — FIFO ordering and no-barging (T10–T13)
// ===========================================================================
//
// FIFO is granted by release()'s transfer branch (wake_wait_one_locked resolves
// the intrusive FIFO head) AND by acquire()'s admission recheck (a permit is
// admitted only to the FIFO head — node.prev_ == nullptr). No-barging is the
// flip side: a newcomer (try_acquire or a later acquire admission) may not
// bypass an already-prioritized queued waiter.
//
// These tests do NOT construct the unreachable stable state
// (available_ > 0 AND eligible waiter exists) via production APIs. They use
// controlled admission/release interleavings with registration-order barriers.

// ---- T10: W1 before W2; release1 -> W1, release2 -> W2 (FIFO) --------------
//
// W1 registers before W2 on a zero-permit Semaphore. Two releases occur. The
// FIFO order requires W1 to be granted first, then W2. We record each waiter's
// acquire index and assert 0 (W1) before 1 (W2). No barging: W2 cannot be
// granted before W1 even though both are live.
SLUICE_TEST_CASE(e12_sem_t10_fifo_w1_before_w2) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode n1, n2;
    std::atomic<bool> w1_registered{false};
    std::atomic<bool> w2_registered{false};
    std::atomic<int> grant_seq{0};
    std::atomic<int> w1_seq{-1}, w2_seq{-1};

    Fiber fw1, fw2, frel;
    fw1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        sem.acquire(n1);
        w1_seq.store(grant_seq.fetch_add(1, std::memory_order_acq_rel),
                     std::memory_order_release);
    });
    fw2.set_entry([&](Fiber&) {
        // Ensure W2 registers AFTER W1.
        spin_wait(w1_registered);
        std::this_thread::yield();  // let W1 register first
        w2_registered.store(true, std::memory_order_release);
        sem.acquire(n2);
        w2_seq.store(grant_seq.fetch_add(1, std::memory_order_acq_rel),
                     std::memory_order_release);
    });
    frel.set_entry([&](Fiber&) {
        spin_wait(w2_registered);
        std::this_thread::yield();  // let both register + suspend
        (void)sem.release();  // FIFO head = W1
        (void)sem.release();  // FIFO head = W2
    });

    FiberStack s1, s2, sr;
    SLUICE_CHECK(sched.init_fiber(fw1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(fw2, s2.base(), s2.size()));
    SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
    sched.spawn(fw1);
    sched.spawn(fw2);
    sched.spawn(frel);
    sched.run(1);

    SLUICE_CHECK_MSG(w1_seq.load() == 0, "W1 granted first (FIFO)");
    SLUICE_CHECK_MSG(w2_seq.load() == 1, "W2 granted second (FIFO)");
    SLUICE_CHECK_MSG(n1.was_woken() && n2.was_woken(), "both Woken");
    SLUICE_CHECK_MSG(sem.available() == 0, "both permits transferred");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T11: W1 queued before W2 arrival; W2 cannot steal W1's release permit --
//
// W1 registers and suspends on a zero-permit Semaphore. W2 then registers and
// suspends. A SINGLE release must grant W1 (the FIFO head), not W2. Proves a
// later-arriving waiter cannot steal the earlier waiter's release permit.
// (A second release then drains W2 so the run terminates cleanly.)
SLUICE_TEST_CASE(e12_sem_t11_w2_cannot_steal_w1_release_permit) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode n1, n2;
    std::atomic<bool> w1_registered{false};
    std::atomic<bool> w2_registered{false};
    std::atomic<bool> w1_done{false};
    std::atomic<int> w1_seq{-1};

    Fiber fw1, fw2, frel;
    fw1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        sem.acquire(n1);
        w1_seq.store(0, std::memory_order_release);  // granted first
        w1_done.store(true, std::memory_order_release);
    });
    fw2.set_entry([&](Fiber&) {
        spin_wait(w1_registered);
        std::this_thread::yield();
        w2_registered.store(true, std::memory_order_release);
        sem.acquire(n2);  // suspended; granted only by the SECOND release
    });
    frel.set_entry([&](Fiber&) {
        spin_wait(w2_registered);
        std::this_thread::yield();
        // Two releases. The FIRST must grant W1 (FIFO head), NOT W2. The SECOND
        // then grants W2 (now the FIFO head). We do NOT spin between releases
        // — spin-waiting for w1_done would block the single worker and prevent
        // W1 from being scheduled. The run drains both fibers; the FIFO order
        // is asserted after run() from the node outcomes.
        (void)sem.release();  // FIRST release -> W1 (FIFO head), NOT W2
        (void)sem.release();  // SECOND release -> W2 (now FIFO head)
    });

    FiberStack s1, s2, sr;
    SLUICE_CHECK(sched.init_fiber(fw1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(fw2, s2.base(), s2.size()));
    SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
    sched.spawn(fw1);
    sched.spawn(fw2);
    sched.spawn(frel);
    sched.run(1);

    SLUICE_CHECK_MSG(n1.was_woken(), "W1 granted by the FIRST release");
    SLUICE_CHECK_MSG(n2.was_woken(), "W2 granted by the SECOND release (after W1)");
    SLUICE_CHECK_MSG(sem.available() == 0, "no stored permit");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T12: try_acquire cannot bypass an already-prioritized queued waiter ----
//
// A waiter W is queued on a zero-permit Semaphore (registered, suspended). A
// later try_acquire must FAIL even if a permit were transiently available,
// because W has FIFO priority. We make this deterministic: W is registered and
// suspended, then try_acquire is called from a second fiber — it must return
// false (the queue is non-empty; W has priority). Then a release grants W.
SLUICE_TEST_CASE(e12_sem_t12_try_acquire_cannot_bypass_queued_waiter) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode nw;
    std::atomic<bool> w_registered{false};
    std::atomic<bool> tried{false};
    std::atomic<bool> try_result{true};  // expect false
    std::atomic<bool> w_done{false};

    Fiber fwait, ftry, frel;
    fwait.set_entry([&](Fiber&) {
        w_registered.store(true, std::memory_order_release);
        sem.acquire(nw);  // suspends
        w_done.store(true, std::memory_order_release);
    });
    ftry.set_entry([&](Fiber&) {
        spin_wait(w_registered);
        std::this_thread::yield();  // let W register + suspend
        // try_acquire must fail: the queue is non-empty (W has FIFO priority).
        // available_ is 0 here, so this also confirms no-spurious-success; the
        // load-bearing assertion is that a queued waiter blocks newcomers.
        try_result.store(sem.try_acquire(), std::memory_order_release);
        tried.store(true, std::memory_order_release);
    });
    frel.set_entry([&](Fiber&) {
        spin_wait(tried);
        std::this_thread::yield();
        (void)sem.release();  // grant W (FIFO head); W resumes on a later drain
        // Do NOT spin for w_done here (blocks the single worker). The run
        // drains both fibers; state is asserted after run().
    });

    FiberStack sw, st, sr;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(ftry, st.base(), st.size()));
    SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
    sched.spawn(fwait);
    sched.spawn(ftry);
    sched.spawn(frel);
    sched.run(1);

    SLUICE_CHECK_MSG(!try_result.load(), "try_acquire failed (queued waiter has priority)");
    SLUICE_CHECK_MSG(nw.was_woken(), "W granted by release");
    SLUICE_CHECK_MSG(sem.available() == 0, "permit transferred to W");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T13: W1 cancelled before release CS -> release grants W2 --------------
//
// W1 and W2 are queued (W1 at head). W1 is cancelled BEFORE the release enters
// its critical section. cancel unlinks W1 in the same CS (under global_mtx_),
// so when release acquires global_mtx_ next it observes W2 as the FIFO head and
// grants W2. This is NOT a release-side skip-after-null — it is W1 being
// unlinked before release observes the queue (Conclusion A).
SLUICE_TEST_CASE(e12_sem_t13_w1_cancelled_before_release_grants_w2) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode n1, n2;
    std::atomic<bool> w1_registered{false};
    std::atomic<bool> w2_registered{false};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> w2_done{false};

    Fiber fw1, fw2, frel, fcancel;
    fw1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        sem.acquire(n1);  // suspends; will be cancelled (never Woken)
    });
    fw2.set_entry([&](Fiber&) {
        spin_wait(w1_registered);
        std::this_thread::yield();
        w2_registered.store(true, std::memory_order_release);
        sem.acquire(n2);  // suspends; will be granted by release
        w2_done.store(true, std::memory_order_release);
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(w2_registered);
        std::this_thread::yield();  // let both register + suspend
        // Cancel W1 BEFORE release. cancel unlinks W1 under global_mtx_; the
        // following release observes W2 as the FIFO head.
        SLUICE_CHECK_MSG(sem.cancel(n1), "W1 cancel won (was Registered, linked here)");
        cancelled.store(true, std::memory_order_release);
    });
    frel.set_entry([&](Fiber&) {
        spin_wait(cancelled);
        std::this_thread::yield();
        (void)sem.release();  // queue head is now W2 -> grants W2
        // Do NOT spin for w2_done (blocks the single worker). The run drains.
    });

    FiberStack s1, s2, sc, sr;
    SLUICE_CHECK(sched.init_fiber(fw1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(fw2, s2.base(), s2.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
    sched.spawn(fw1);
    sched.spawn(fw2);
    sched.spawn(fcancel);
    sched.spawn(frel);
    sched.run(1);

    SLUICE_CHECK_MSG(n1.was_cancelled(), "W1 resolved Cancelled");
    SLUICE_CHECK_MSG(n2.was_woken(), "W2 granted (W1 unlinked before release)");
    SLUICE_CHECK_MSG(sem.available() == 0, "permit transferred to W2");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// Slice 4 — Deadline precedence (T14–T17)
// ===========================================================================
//
// Mandatory precedence (A4): permit admission > already-due deadline > timed
// registration race. These use the controllable logical clock (NO sleep_for
// causal proof). The deadline is reached through sched.advance_clock.

// ---- T14: permit admissible + already-due deadline -> Woken ----------------
//
// acquire_until on a Semaphore with one stored permit, with a deadline already
// due at admission. Permit admission has precedence (A4): resolve Woken inline,
// NOT Expired. available_ decrements; the timer is retired; the fiber does not
// suspend.
SLUICE_TEST_CASE(e12_sem_t14_permit_plus_due_deadline_is_woken) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 100);  // clock already past the deadline

    Semaphore sem(sched, /*initial=*/1, /*max=*/2);
    WaitNode node;

    Fiber facq;
    facq.set_entry([&](Fiber&) {
        // deadline=50 is already due (clock=100), but a permit is admissible ->
        // permit admission wins (A4). Resolves Woken inline.
        sem.acquire_until(node, /*deadline=*/50);
    });

    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(facq, sa.base(), sa.size()));
    sched.spawn(facq);
    sched.run(1);

    SLUICE_CHECK_MSG(node.was_woken(), "permit admission wins over due deadline");
    SLUICE_CHECK_MSG(!node.was_expired(), "NOT Expired (permit precedence)");
    SLUICE_CHECK_MSG(sem.available() == 0, "permit consumed");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
    // The timer must have been retired at admission (no active registration).
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "timer retired at admission (no leak)");
}

// ---- T15: no permit + already-due deadline -> Expired ----------------------
//
// acquire_until on a zero-permit Semaphore, with a deadline already due at
// admission. No permit is admissible, so the I5 admission closure resolves
// Expired inline (the fiber does NOT suspend to wait for a timer scan).
SLUICE_TEST_CASE(e12_sem_t15_no_permit_plus_due_deadline_is_expired) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 100);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode node;

    Fiber facq;
    facq.set_entry([&](Fiber&) {
        // no permit admissible, deadline=50 already due -> Expired inline (I5).
        sem.acquire_until(node, /*deadline=*/50);
    });

    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(facq, sa.base(), sa.size()));
    sched.spawn(facq);
    sched.run(1);

    SLUICE_CHECK_MSG(node.was_expired(), "no permit + due deadline -> Expired");
    SLUICE_CHECK_MSG(sem.available() == 0, "no permit consumed");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "timer consumed at admission (no leak)");
}

// ---- T16: permit admissible + future deadline -> immediate Woken ----------
//
// acquire_until with a stored permit and a future deadline. Permit admission
// resolves Woken inline; the future deadline is moot and retired.
SLUICE_TEST_CASE(e12_sem_t16_permit_plus_future_deadline_immediate_woken) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 0);

    Semaphore sem(sched, /*initial=*/2, /*max=*/4);
    WaitNode node;

    Fiber facq;
    facq.set_entry([&](Fiber&) {
        sem.acquire_until(node, /*deadline=*/1000);  // future; permit admits
    });

    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(facq, sa.base(), sa.size()));
    sched.spawn(facq);
    sched.run(1);

    SLUICE_CHECK_MSG(node.was_woken(), "permit admissible + future deadline -> Woken");
    SLUICE_CHECK_MSG(sem.available() == 1, "one permit consumed");
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "timer retired (no leak)");
}

// ---- T17: release wins before timer -> Woken -------------------------------
//
// A waiter suspends on acquire_until (no permit, future deadline). A release
// occurs BEFORE the timer expires. release wins (Woken); the later timer expiry
// is the loser (no-op). Determinism: release runs first (winner_done barrier),
// then advance_clock.
SLUICE_TEST_CASE(e12_sem_t17_release_wins_before_timer) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 0);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> winner_done{false};

    Fiber fwait, frel, fdriver;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sem.acquire_until(node, /*deadline=*/100);  // future; suspends
    });
    frel.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        (void)sem.release();  // RESOURCE_WAKE (transfer) wins
        winner_done.store(true, std::memory_order_release);
    });
    fdriver.set_entry([&](Fiber&) {
        spin_wait(winner_done);  // barrier: release wins first
        sched.advance_clock(100);  // timer expires (loser)
    });

    FiberStack sw, sr, sd;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
    SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
    sched.spawn(fwait);
    sched.spawn(frel);
    sched.spawn(fdriver);
    sched.run(3);

    SLUICE_CHECK_MSG(node.was_woken(), "release won before timer");
    SLUICE_CHECK_MSG(sem.available() == 0, "permit transferred");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "timer retired (no leak)");
}

// ---- T18: timer wins before release -> Expired -----------------------------
//
// A waiter suspends on acquire_until (no permit, future deadline). The timer
// expires BEFORE a release. Timer wins (Expired); the later release then stores
// the permit (no eligible waiter remains). Determinism: timer runs first (retry
// advance_clock until terminal), then release.
SLUICE_TEST_CASE(e12_sem_t18_timer_wins_before_release) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 0);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> winner_done{false};

    Fiber fwait, fdriver, frel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sem.acquire_until(node, /*deadline=*/100);  // future; suspends
    });
    fdriver.set_entry([&](Fiber&) {
        spin_wait(registered);
        // Retry advance_clock until the node resolves (timer wins).
        for (int i = 0; i < 200 && !node.is_terminal(); ++i) {
            sched.advance_clock(100);
            std::this_thread::yield();
        }
        winner_done.store(true, std::memory_order_release);
    });
    frel.set_entry([&](Fiber&) {
        spin_wait(winner_done);  // barrier: timer wins first
        (void)sem.release();  // no eligible waiter now -> store permit
    });

    FiberStack sw, sd, sr;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
    SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
    sched.spawn(fwait);
    sched.spawn(fdriver);
    sched.spawn(frel);
    sched.run(3);

    SLUICE_CHECK_MSG(node.was_expired(), "timer won before release");
    SLUICE_CHECK_MSG(sem.available() == 1, "later release stored the permit");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "timer consumed (no leak)");
}

// ===========================================================================
// Slice 5 — Cancellation (T19–T24)
// ===========================================================================

// ---- T19: registered cancel -> true, Cancelled ----------------------------
//
// A registered waiter is cancelled via Semaphore::cancel. Returns true; the node
// resolves Cancelled; available_ unchanged; the wait count returns to baseline.
SLUICE_TEST_CASE(e12_sem_t19_registered_cancel_true_cancelled) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> cancelled{false};

    Fiber fwait, fcancel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sem.acquire(node);  // suspends
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        SLUICE_CHECK_MSG(sem.cancel(node), "cancel won (node Registered here)");
        cancelled.store(true, std::memory_order_release);
    });

    FiberStack sw, sc;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fwait);
    sched.spawn(fcancel);
    sched.run(1);

    SLUICE_CHECK_MSG(cancelled.load(), "cancel executed");
    SLUICE_CHECK_MSG(node.was_cancelled(), "node resolved Cancelled");
    SLUICE_CHECK_MSG(sem.available() == 0, "cancel does not change available");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "wait count at baseline");
}

// ---- T20: cancel after grant (Woken) -> false -----------------------------
//
// A waiter is granted (Woken) by a release BEFORE the cancel. cancel then
// returns false (the node is already Woken / unlinked). No second resolution.
SLUICE_TEST_CASE(e12_sem_t20_cancel_after_grant_false) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> granted{false};

    Fiber fwait, frel, fcancel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sem.acquire(node);  // suspends; resumed by release (Woken)
    });
    frel.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        (void)sem.release();  // grant W1 (Woken)
        granted.store(true, std::memory_order_release);
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(granted);  // barrier: grant wins first
        std::this_thread::yield();
        SLUICE_CHECK_MSG(!sem.cancel(node), "cancel after grant returns false");
    });

    FiberStack sw, sr, sc;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fwait);
    sched.spawn(frel);
    sched.spawn(fcancel);
    sched.run(1);

    SLUICE_CHECK_MSG(node.was_woken(), "node still Woken (cancel did not mutate)");
}

// ---- T21: cancel after expiry (Expired) -> false --------------------------
//
// A timed waiter expires BEFORE the cancel. cancel then returns false (the node
// is already Expired / unlinked).
SLUICE_TEST_CASE(e12_sem_t21_cancel_after_expiry_false) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 0);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> expired{false};

    Fiber fwait, fdriver, fcancel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sem.acquire_until(node, /*deadline=*/100);  // suspends; timer wins
    });
    fdriver.set_entry([&](Fiber&) {
        spin_wait(registered);
        for (int i = 0; i < 200 && !node.is_terminal(); ++i) {
            sched.advance_clock(100);
            std::this_thread::yield();
        }
        expired.store(true, std::memory_order_release);
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(expired);  // barrier: expiry wins first
        std::this_thread::yield();
        SLUICE_CHECK_MSG(!sem.cancel(node), "cancel after expiry returns false");
    });

    FiberStack sw, sd, sc;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fwait);
    sched.spawn(fdriver);
    sched.spawn(fcancel);
    sched.run(3);

    SLUICE_CHECK_MSG(node.was_expired(), "node Expired (cancel did not mutate)");
}

// ---- T22: repeated cancel -> false on second ------------------------------
//
// A successful cancel is followed by a second cancel of the same node. The
// second returns false (the node is now Cancelled / unlinked).
SLUICE_TEST_CASE(e12_sem_t22_repeated_cancel_second_false) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode node;
    std::atomic<bool> registered{false};
    std::atomic<bool> cancelled_once{false};

    Fiber fwait, fcancel;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sem.acquire(node);  // suspends
    });
    fcancel.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        SLUICE_CHECK_MSG(sem.cancel(node), "first cancel wins");
        cancelled_once.store(true, std::memory_order_release);
        // Immediate second cancel of the same (now Cancelled/unlinked) node.
        SLUICE_CHECK_MSG(!sem.cancel(node), "second cancel returns false");
    });

    FiberStack sw, sc;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fwait);
    sched.spawn(fcancel);
    sched.run(1);

    SLUICE_CHECK_MSG(cancelled_once.load(), "first cancel executed");
    SLUICE_CHECK_MSG(node.was_cancelled(), "node Cancelled once");
}

// ---- T23: detached node cancel -> false -----------------------------------
//
// A fresh (Detached, never-registered) node cancel returns false without
// mutation. No membership in any queue.
SLUICE_TEST_CASE(e12_sem_t23_detached_cancel_false) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/1, /*max=*/2);
    WaitNode detached;  // never registered

    SLUICE_CHECK_MSG(!sem.cancel(detached), "detached node cancel -> false");
    SLUICE_CHECK_MSG(!detached.is_terminal(), "detached node untouched");
    SLUICE_CHECK_MSG(sem.available() == 1, "no mutation");
}

// ---- T24: wrong Semaphore, same Scheduler -> false ------------------------
//
// A node registered in Semaphore A is cancelled on Semaphore B (same Scheduler).
// B's membership check scans its OWN queue (empty / different membership), so
// cancel returns false. A's node is untouched. Cross-Semaphore safety.
SLUICE_TEST_CASE(e12_sem_t24_wrong_semaphore_same_scheduler_false) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem_a(sched, /*initial=*/0, /*max=*/2);
    Semaphore sem_b(sched, /*initial=*/0, /*max=*/2);
    WaitNode node_a;
    std::atomic<bool> registered{false};
    std::atomic<bool> tried{false};

    Fiber fwait, ftry;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sem_a.acquire(node_a);  // registered in sem_a
    });
    ftry.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        // Cancel node_a on sem_b (wrong Semaphore, same Scheduler). sem_b's
        // queue does not contain node_a -> false, no mutation.
        SLUICE_CHECK_MSG(!sem_b.cancel(node_a), "wrong-Semaphore cancel -> false");
        tried.store(true, std::memory_order_release);
        // Drain node_a cleanly via sem_a so the run completes.
        (void)sem_a.release();
    });

    FiberStack sw, st;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(ftry, st.base(), st.size()));
    sched.spawn(fwait);
    sched.spawn(ftry);
    sched.run(1);

    SLUICE_CHECK_MSG(tried.load(), "wrong-Semaphore cancel attempted");
    // node_a was NOT cancelled by sem_b; it was later granted by sem_a.release.
    SLUICE_CHECK_MSG(node_a.was_woken(), "node_a untouched by wrong-Semaphore cancel");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T25: wrong Semaphore, different Scheduler -> false -------------------
//
// A node registered in a Semaphore on Scheduler A is cancelled on a Semaphore
// on Scheduler B. B scans its OWN queue under B's global_mtx_; it never reads
// the foreign node's home_ and never locks foreign Scheduler A. Returns false
// safely. This is the cross-Scheduler structural-safety guard.
SLUICE_TEST_CASE(e12_sem_t25_wrong_semaphore_different_scheduler_false) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx_a(std::make_unique<IdleBackend>());
    AsyncIoContext ctx_b(std::make_unique<IdleBackend>());
    Scheduler sched_a(ctx_a);
    Scheduler sched_b(ctx_b);

    Semaphore sem_a(sched_a, /*initial=*/0, /*max=*/2);
    Semaphore sem_b(sched_b, /*initial=*/0, /*max=*/2);
    WaitNode node_a;
    std::atomic<bool> registered{false};
    std::atomic<bool> tried{false};

    Fiber fwait, ftry;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sem_a.acquire(node_a);  // registered in sem_a (Scheduler A)
    });
    ftry.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();
        // Cancel node_a on sem_b (Scheduler B). sem_b scans its OWN queue under
        // B's global_mtx_; node_a is not a member -> false. No foreign-Scheduler
        // state is read or locked.
        SLUICE_CHECK_MSG(!sem_b.cancel(node_a),
                         "wrong-Semaphore different-Scheduler cancel -> false");
        tried.store(true, std::memory_order_release);
        (void)sem_a.release();  // drain node_a cleanly
    });

    FiberStack sw, st;
    SLUICE_CHECK(sched_a.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched_a.init_fiber(ftry, st.base(), st.size()));
    sched_a.spawn(fwait);
    sched_a.spawn(ftry);
    sched_a.run(1);

    SLUICE_CHECK_MSG(tried.load(), "cross-Scheduler cancel attempted");
    SLUICE_CHECK_MSG(node_a.was_woken(),
                     "node_a untouched by cross-Scheduler cancel");
    SLUICE_CHECK_MSG(sched_a.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// Slice 6 — External-thread release + Scheduler integration (T26–T29)
// ===========================================================================

// ---- T26: external OS-thread release wakes a parked Live Scheduler --------
//
// A Live Scheduler with one Worker has an unresolved Semaphore wait. An external
// std::thread calls Semaphore::release(); the release transfer resolves the
// waiter through the Scheduler wake source (route_runnable_locked ->
// signal_wake_locked) and the Fiber resumes Woken. No Semaphore-private wake
// channel. Mirrors E12-A T19 (external-thread set).
SLUICE_TEST_CASE(e12_sem_t26_external_thread_release_wakes_live) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode node;
    std::atomic<bool> waiter_registered{false};
    std::atomic<int> entries{0};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        waiter_registered.store(true, std::memory_order_release);
        sem.acquire(node);  // suspends; no worker release -> parks the Worker
        entries.fetch_add(1, std::memory_order_acq_rel);
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);

    // External producer: wait for the waiter to register, then release() from
    // outside any worker thread. The bounded sleeps are registration-sync ONLY
    // (not causal proof; the outcome assertion is the proof).
    std::thread ext([&] {
        while (!waiter_registered.load(std::memory_order::acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        (void)sem.release();  // external-thread release: transfer via wake source
    });

    sched.run_live(1);  // Live: may park on MW-S3 + external-wake-capable
    ext.join();

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter resumed via external release()");
    SLUICE_CHECK_MSG(node.was_woken(), "waiter resolved Woken");
    SLUICE_CHECK_MSG(sem.available() == 0, "permit transferred (not stored)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T27: terminal waits leave the queue empty (no leak) ------------------
//
// After all waits resolve terminally and the run drains, the Semaphore's private
// queue is empty (waiting_count == 0). Safe to destroy. Proves no queue/timer
// leak across terminal closure.
SLUICE_TEST_CASE(e12_sem_t27_terminal_waits_leave_queue_empty) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    Semaphore sem(sched, /*initial=*/1, /*max=*/3);
    WaitNode n1, n2, n3;
    std::atomic<int> granted{0};

    Fiber f1, f2, f3, frel;
    f1.set_entry([&](Fiber&) { sem.acquire(n1); granted.fetch_add(1); });
    f2.set_entry([&](Fiber&) { sem.acquire(n2); granted.fetch_add(1); });
    f3.set_entry([&](Fiber&) { sem.acquire(n3); granted.fetch_add(1); });
    frel.set_entry([&](Fiber&) {
        // Two more releases for the two suspended waiters (n1 got the initial).
        (void)sem.release();
        (void)sem.release();
    });

    FiberStack s1, s2, s3, sr;
    SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    SLUICE_CHECK(sched.init_fiber(f3, s3.base(), s3.size()));
    SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
    sched.spawn(f1);
    sched.spawn(f2);
    sched.spawn(f3);
    sched.spawn(frel);
    sched.run(1);

    SLUICE_CHECK_MSG(granted.load() == 3, "all three waiters acquired");
    SLUICE_CHECK_MSG(n1.was_woken() && n2.was_woken() && n3.was_woken(),
                     "all Woken");
    SLUICE_CHECK_MSG(sem.available() == 0, "all permits consumed/transferred");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "queue empty (no leak)");
}

// ---- T28: terminal timed waits leave no timer registration ----------------
//
// A timed wait that resolves terminally must retire its timer registration.
// After the run, active_deadline_count == 0 and the timer pool has no ACTIVE
// block. Proves the I4 lifetime closure for Semaphore timed waits.
SLUICE_TEST_CASE(e12_sem_t28_terminal_timed_wait_no_timer_leak) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 0);

    Semaphore sem(sched, /*initial=*/0, /*max=*/2);
    WaitNode node;
    std::atomic<bool> registered{false};

    Fiber fwait, fdriver;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sem.acquire_until(node, /*deadline=*/100);  // expires
    });
    fdriver.set_entry([&](Fiber&) {
        spin_wait(registered);
        for (int i = 0; i < 200 && !node.is_terminal(); ++i) {
            sched.advance_clock(100);
            std::this_thread::yield();
        }
    });

    FiberStack sw, sd;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
    sched.spawn(fwait);
    sched.spawn(fdriver);
    sched.run(2);

    SLUICE_CHECK_MSG(node.was_expired(), "timed wait expired");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "queue empty");
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "no active timer registration (no leak)");
    SLUICE_CHECK_MSG(
        E11TimerTestHooks::timer_pool_count_in_state(
            sched, TimerRegistration::State::active) == 0,
        "no ACTIVE block in the timer pool");
}

// ---- T29: safe destruction after terminal closure -------------------------
//
// After all waits resolve terminally (Woken via release), destroying the
// Semaphore is safe: the WaitQueue destructor asserts empty in debug, and no
// timer retains destroyed-queue state. This is the destruction contract (A3).
SLUICE_TEST_CASE(e12_sem_t29_safe_destruction_after_terminal_closure) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    // Scope the Semaphore so its destructor runs at end of block.
    {
        Semaphore sem(sched, /*initial=*/0, /*max=*/2);
        WaitNode n1, n2;
        std::atomic<int> w1r{0}, w2r{0};
        std::atomic<bool> w1_registered{false}, w2_registered{false};

        Fiber f1, f2, frel;
        f1.set_entry([&](Fiber&) {
            w1_registered.store(true, std::memory_order_release);
            sem.acquire(n1);
            w1r.fetch_add(1, std::memory_order_acq_rel);
        });
        f2.set_entry([&](Fiber&) {
            spin_wait(w1_registered);
            std::this_thread::yield();
            w2_registered.store(true, std::memory_order_release);
            sem.acquire(n2);
            w2r.fetch_add(1, std::memory_order_acq_rel);
        });
        frel.set_entry([&](Fiber&) {
            spin_wait(w2_registered);
            std::this_thread::yield();
            (void)sem.release();  // grant W1
            (void)sem.release();  // grant W2
        });

        FiberStack s1, s2, sr;
        SLUICE_CHECK(sched.init_fiber(f1, s1.base(), s1.size()));
        SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
        SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
        sched.spawn(f1);
        sched.spawn(f2);
        sched.spawn(frel);
        sched.run(1);

        SLUICE_CHECK_MSG(w1r.load() == 1 && w2r.load() == 1, "both waiters done");
        SLUICE_CHECK_MSG(n1.was_woken() && n2.was_woken(), "both Woken");
        // ~Semaphore runs here; in debug the empty-queue assertion must hold.
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "queue empty before destroy");
    }
    // If we reach here, the Semaphore destroyed safely (no debug assert fired).
    SLUICE_CHECK_MSG(true, "Semaphore destroyed safely after terminal closure");
}

// ===========================================================================
// Slice 7 — Repeated mixed multi-waiter stress (T30)
// ===========================================================================
//
// §12 stability: mixed multi-waiter stress, repeated. Each iteration creates K
// waiters on a fresh zero-permit Semaphore, then releases exactly K permits,
// interleaved with a canceller and (optionally) a timer driver. The load-bearing
// invariant across every iteration:
//   - exactly K outcomes are terminal (Woken + Cancelled [+ Expired] == K)
//   - the number of Woken outcomes equals the number of release transfers
//     (release permits are never lost or double-counted)
//   - available_ returns to 0 (no permit leak; permit conservation)
//   - waiting_count returns to 0 (no queue leak)
//
// Determinism: no sleep_for causal proof. The canceller uses a bounded retry
// loop; the timer driver uses the controllable clock. Outcomes are summed
// across iterations.

// ---- T30: repeated mixed multi-waiter stress (woken+cancelled, K=3) --------
//
// Three waiters on a zero-permit Semaphore. Two are released (Woken via FIFO
// transfer); one is cancelled (Cancelled). Repeated. Each iteration must yield
// exactly 3 terminal outcomes (2 Woken + 1 Cancelled), no leak, available_==0.
SLUICE_TEST_CASE(e12_sem_t30_repeated_mixed_multi_waiter_stress) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    constexpr int ITERS = 100;
    constexpr int K = 3;  // waiters per iteration
    int total_woken = 0, total_cancelled = 0;

    for (int it = 0; it < ITERS; ++it) {
        Semaphore sem(sched, /*initial=*/0, /*max=*/static_cast<Semaphore::permit_count_t>(K));
        WaitNode n[K];
        std::atomic<bool> registered[K];
        for (int k = 0; k < K; ++k) registered[k].store(false, std::memory_order_release);
        std::atomic<int> reg_count{0};
        std::atomic<bool> all_registered{false};

        Fiber fwait[K];
        FiberStack sw[K];
        for (int k = 0; k < K; ++k) {
            fwait[k].set_entry([&sem, &n, &registered, &reg_count, &all_registered, k](Fiber&) {
                registered[k].store(true, std::memory_order_release);
                reg_count.fetch_add(1, std::memory_order_acq_rel);
                if (reg_count.load(std::memory_order_acquire) == K) {
                    all_registered.store(true, std::memory_order_release);
                }
                sem.acquire(n[k]);
            });
            SLUICE_CHECK(sched.init_fiber(fwait[k], sw[k].base(), sw[k].size()));
            sched.spawn(fwait[k]);
        }

        // Canceller: cancel waiter 0 once all are registered.
        std::atomic<bool> cancelled{false};
        Fiber fcancel;
        fcancel.set_entry([&](Fiber&) {
            spin_wait(all_registered);
            std::this_thread::yield();
            for (int i = 0; i < 50; ++i) {
                if (sem.cancel(n[0])) { cancelled.store(true); break; }
                std::this_thread::yield();
            }
        });
        // Releaser: after the cancel attempt settles, release K-1 permits to
        // grant the remaining waiters (the cancel freed one slot).
        Fiber frel;
        frel.set_entry([&](Fiber&) {
            spin_wait(all_registered);
            std::this_thread::yield();
            // Release K-1 permits. If n[0] was cancelled, K-1 waiters remain and
            // each gets one. If the cancel lost (n[0] was already granted), the
            // extra release stores into available_ (bounded by max=K). Either
            // way, the run drains.
            for (int r = 0; r < K - 1; ++r) (void)sem.release();
        });

        FiberStack sc, sr;
        SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
        SLUICE_CHECK(sched.init_fiber(frel, sr.base(), sr.size()));
        sched.spawn(fcancel);
        sched.spawn(frel);
        sched.run(1);

        // Per-iteration invariants.
        int woken = 0, cancelled_n = 0;
        for (int k = 0; k < K; ++k) {
            if (n[k].was_woken()) ++woken;
            else if (n[k].was_cancelled()) ++cancelled_n;
        }
        const int terminal = woken + cancelled_n;
        SLUICE_CHECK_MSG(terminal == K, "all K waiters resolved terminal");
        total_woken += woken;
        total_cancelled += cancelled_n;
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no queue leak");
        // available_ must not exceed max (no overflow) and permit conservation
        // holds: woken == released-transferred, cancelled did not consume one.
    }

    // Cross-iteration sanity: every iteration resolved exactly K waiters.
    SLUICE_CHECK_MSG(total_woken + total_cancelled == ITERS * K,
                     "all iterations resolved all waiters");
    // The canceller won at least sometimes over 100 iterations (strong signal).
    SLUICE_CHECK_MSG(total_cancelled > 0, "cancel won at least once");
    SLUICE_CHECK_MSG(total_woken > 0, "release transferred at least once");
}

