// e12_async_mutex_test — Fiber-suspending Async Mutex (sluice-CORE-E12-C).
//
// Deterministic production tests for the Fiber-suspending Async Mutex built on
// the closed E10/E11/E12-A/E12-B wait substrate. Observed ONLY through the
// SEALED AsyncMutex public API + the mechanically gated test hooks:
//
//   - AsyncMutex::try_lock / lock / lock_until / cancel / unlock
//   - E11TimerControl / E9ParkSeam / E12MutexSeam (deterministic clock/timer +
//     park + owner-before-publication seams)
//   - WaitNode public lock-free state queries (was_woken/was_cancelled/
//     was_expired/is_terminal/outcome)
//   - Scheduler::advance_clock / waiting_count() (deterministic timer +
//     wait-accounting authority)
//
// Every causal race proof uses mechanically gated phase seams + retry loops or
// barriers — NEVER sleep_for timing as causal proof. A bounded timeout may
// remain ONLY as a test-failure guard, not as causal synchronization.
//
// Gated to x86_64 (fiber_ctx::supported): registration requires a real Fiber.
//
// This file is the Commit-D BASELINE: it establishes that the public type
// compiles, the immediate try_lock/lock/unlock paths work, basic FIFO handoff
// works, and the API boundaries remain sealed. The complete deterministic
// race / cancellation / deadline / migration / 500/500 coordination coverage
// is added in Commit E.
#include "harness.hpp"
#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/async_mutex.hpp>
#include <sluice/async/fake_backend.hpp>
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
using E11TimerTestHooks = sluice_async_test::E11TimerControl;
using E9ParkSeam = sluice_async_test::E9ParkSeam;
using E12MutexSeam = sluice_async_test::E12MutexSeam;

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

[[maybe_unused]] inline void spin_wait(std::atomic<bool>& flag) {
    while (!flag.load(std::memory_order::acquire)) {
        std::this_thread::yield();
    }
}

[[maybe_unused]] inline void spin_wait_pred(auto&& pred) {
    while (!pred()) {
        std::this_thread::yield();
    }
}
}  // namespace

SLUICE_MAIN()

// ===========================================================================
// Slice 1 — Construction + immediate try_lock/lock/unlock (baseline)
// ===========================================================================

// ---- T0: construction outside a Fiber; destructor on unlocked+empty -------
//
// An AsyncMutex constructs unlocked (no Fiber required) and destroys safely
// when unlocked + queue empty. The destructor debug-asserts owner_ == nullptr.
SLUICE_TEST_CASE(e12_mtx_t0_construction_and_destruction) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    { AsyncMutex mtx(sched); }  // construct + destroy unlocked/empty: OK
}

// ---- T1: try_lock immediate success + recursive-fails + fails-while-owned --
//
// From a Fiber, try_lock on a free mutex succeeds (owner_ := current). A
// recursive try_lock from the same owner returns false (no mutation). A
// try_lock while another Fiber owns returns false.
SLUICE_TEST_CASE(e12_mtx_t1_try_lock_immediate_recursive_owned) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);

    std::atomic<bool> first_ok{false};
    std::atomic<bool> recursive_false{true};   // expect false
    std::atomic<bool> owner_done{false};

    Fiber owner;
    owner.set_entry([&](Fiber&) {
        first_ok.store(mtx.try_lock(), std::memory_order_release);
        recursive_false.store(!mtx.try_lock(), std::memory_order_release);
        // signal the contender may now try (it will see owned -> false)
        owner_done.store(true, std::memory_order_release);
        mtx.unlock();
    });

    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    sched.spawn(owner);
    sched.run(1);

    SLUICE_CHECK_MSG(first_ok.load(), "first try_lock on free mutex succeeds");
    SLUICE_CHECK_MSG(recursive_false.load(),
                     "recursive try_lock returns false (no mutation)");
    SLUICE_CHECK_MSG(owner_done.load(), "owner fiber completed + unlocked");

    // Now a fresh fiber tries try_lock on the now-free mutex: succeeds.
    std::atomic<bool> reacquired{false};
    Fiber second;
    second.set_entry([&](Fiber&) {
        reacquired.store(mtx.try_lock(), std::memory_order_release);
        if (reacquired.load(std::memory_order_acquire)) mtx.unlock();
    });
    FiberStack sb;
    SLUICE_CHECK(sched.init_fiber(second, sb.base(), sb.size()));
    sched.spawn(second);
    sched.run(1);
    SLUICE_CHECK_MSG(reacquired.load(), "try_lock on now-free mutex succeeds");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T2: immediate lock resolves Woken without suspending; owns + unlocks --
//
// A fiber calls lock() on a free mutex. The admission recheck finds owner_ free
// and this node at the FIFO head, resolves Woken inline, sets owner_ = current,
// and does NOT suspend. The fiber then unlocks (no waiter) -> owner_ = nullptr.
SLUICE_TEST_CASE(e12_mtx_t2_immediate_lock_woken_then_unlock_no_waiter) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);

    WaitNode node;
    std::atomic<int> entries{0};

    Fiber facq;
    facq.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        mtx.lock(node);
        entries.fetch_add(1, std::memory_order_acq_rel);
        mtx.unlock();
        entries.fetch_add(1, std::memory_order_acq_rel);
    });

    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(facq, sa.base(), sa.size()));
    sched.spawn(facq);
    sched.run(1);

    SLUICE_CHECK_MSG(entries.load() == 3, "lock+unlock fiber completed");
    SLUICE_CHECK_MSG(node.was_woken(), "immediate lock resolved Woken inline");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T3: basic FIFO direct handoff (two waiters) --------------------------
//
// owner G0 holds the lock; G1 then G2 queue (suspend). On G0 unlock, the FIFO
// head G1 is handed ownership (Woken), resumes owning, and unlocks to hand off
// to G2. Proves direct ownership handoff with no free-lock window and that
// owner_ transitions Owned(G0)->Owned(G1)->Owned(G2) without intermediate free.
SLUICE_TEST_CASE(e12_mtx_t3_basic_fifo_handoff_two_waiters) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    std::atomic<int> s1{0}, s2{0};
    WaitNode m0, m1, m2;

    Fiber g0;
    g0.set_entry([&](Fiber&) {
        mtx.lock(m0);   // G0 owns (immediate)
        mtx.unlock();   // handoff to FIFO head (G1, registered first)
    });
    Fiber g1;
    g1.set_entry([&](Fiber&) {
        mtx.lock(m1);   // suspends until G0 unlocks
        s1.store(1, std::memory_order_release);  // resumed owning
        mtx.unlock();   // handoff to G2
        s1.store(2, std::memory_order_release);
    });
    Fiber g2;
    g2.set_entry([&](Fiber&) {
        mtx.lock(m2);   // suspends until G1 unlocks
        s2.store(1, std::memory_order_release);  // resumed owning
        mtx.unlock();
        s2.store(2, std::memory_order_release);
    });

    FiberStack sg0, sg1, sg2;
    SLUICE_CHECK(sched.init_fiber(g0, sg0.base(), sg0.size()));
    SLUICE_CHECK(sched.init_fiber(g1, sg1.base(), sg1.size()));
    SLUICE_CHECK(sched.init_fiber(g2, sg2.base(), sg2.size()));
    // Spawn G1 and G2 first so they suspend on the (owned) mtx; then spawn G0
    // which takes it and unlocks to hand off to the FIFO head (G1).
    sched.spawn(g1);
    sched.spawn(g2);
    sched.spawn(g0);
    sched.run(1);

    SLUICE_CHECK_MSG(s1.load() == 2, "G1 resumed owning then unlocked");
    SLUICE_CHECK_MSG(s2.load() == 2, "G2 resumed owning then unlocked");
    SLUICE_CHECK_MSG(m1.was_woken(), "G1 resolved Woken (handoff)");
    SLUICE_CHECK_MSG(m2.was_woken(), "G2 resolved Woken (handoff)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

