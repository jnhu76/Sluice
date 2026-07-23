// async_mutex_primitive_test — Fiber-suspending Async Mutex (sluice-CORE-E12-C).
//
// Deterministic production tests for the Fiber-suspending Async Mutex built on
// the closed E10/E11/E12-A/E12-B wait substrate. Observed ONLY through the
// SEALED AsyncMutex public API + the mechanically gated test hooks:
//
//   - AsyncMutex::try_lock / lock / lock_until / cancel / unlock
//   - TimerTestControl / SchedulerParkSeam / AsyncMutexSeam (deterministic clock/timer +
//     park + owner-before-publication seams)
//   - WaitNode public lock-free state queries (was_woken/was_cancelled/
//     was_expired/is_terminal/outcome)
//   - Scheduler::advance_clock / waiting_count() / await_ready_flag
//     (deterministic timer + wait-accounting + owner-yield authority)
//
// Every causal race proof uses mechanically gated phase seams + retry loops or
// barriers — NEVER sleep_for timing as causal proof. A bounded timeout may
// remain ONLY as a test-failure guard, not as causal synchronization.
//
// CRITICAL scheduling idiom (single-worker cooperative): a fiber that spin_waits
// on a flag set by another fiber DEADLOCKS unless the setter runs. The owner of
// an AsyncMutex holds the lock and does NOT suspend on the mutex, so it MUST
// yield the worker via sched.await_ready_flag(proceed) to let waiters/newcomers
// run. Every spin_wait target is set BEFORE the setter suspends (a happens-before
// barrier, not a true loop) — mirrors e12_semaphore_test T11.
//
// Gated to x86_64 (fiber_ctx::supported): registration requires a real Fiber.
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
using E11TimerTestHooks = sluice_async_test::TimerTestControl;
using SchedulerParkSeam = sluice_async_test::SchedulerParkSeam;
using AsyncMutexSeam = sluice_async_test::AsyncMutexSeam;
using sluice_async_test::ControllerGuard;

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

// Bounded variants: a failed wait must produce a test failure rather than hang
// indefinitely (E12-C Corrective-3 §3: bounded failure behaviour). They poll
// for at most `max_iters` yield-cycles and return false on timeout. A bounded
// wait is a FAILURE GUARD ONLY — it is never used as causal synchronisation.
constexpr unsigned kBoundedWaitIters = 200000;

[[maybe_unused]] inline bool bounded_wait(std::atomic<bool>& flag,
                                          unsigned max_iters = kBoundedWaitIters) {
    for (unsigned i = 0; i < max_iters; ++i) {
        if (flag.load(std::memory_order::acquire)) return true;
        std::this_thread::yield();
    }
    return flag.load(std::memory_order::acquire);
}

// A ready-flag the owner awaits to yield the worker while holding the lock.
// The setter (newcomer/waiter coordinator) sets it to release the owner.
struct ReadyFlag {
    std::atomic<bool> v{false};
    void set() { v.store(true, std::memory_order::release); }
    void wait_spin() {
        while (!v.load(std::memory_order::acquire)) std::this_thread::yield();
    }
};
}  // namespace

SLUICE_MAIN()

// ===========================================================================
// Slice 1 — Construction + immediate try_lock/lock/unlock (baseline)
// ===========================================================================

// ---- T0: construction outside a Fiber; destructor on unlocked+empty -------
SLUICE_TEST_CASE(mtx_t0_construction_and_destruction) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    { AsyncMutex mtx(sched); }  // construct + destroy unlocked/empty: OK
}

// ---- T1: try_lock immediate success + recursive-fails + reacquire --------
SLUICE_TEST_CASE(mtx_t1_try_lock_immediate_recursive_owned) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);

    std::atomic<bool> first_ok{false}, recursive_false{true}, done{false};
    Fiber owner;
    owner.set_entry([&](Fiber&) {
        first_ok.store(mtx.try_lock(), std::memory_order_release);
        recursive_false.store(!mtx.try_lock(), std::memory_order_release);
        mtx.unlock();
        done.store(true, std::memory_order_release);
    });
    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(owner, sa.base(), sa.size()));
    sched.spawn(owner);
    sched.run(1);
    SLUICE_CHECK_MSG(first_ok.load(), "first try_lock on free mutex succeeds");
    SLUICE_CHECK_MSG(recursive_false.load(),
                     "recursive try_lock returns false (no mutation)");
    SLUICE_CHECK_MSG(done.load(), "owner completed + unlocked");

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

// ---- T2: immediate lock resolves Woken without suspending; owns + unlocks -
SLUICE_TEST_CASE(mtx_t2_immediate_lock_woken_then_unlock_no_waiter) {
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
// to G2. The owner yields the worker via await_ready_flag so G1/G2 can register
// and suspend before G0 unlocks.
SLUICE_TEST_CASE(mtx_t3_basic_fifo_handoff_two_waiters) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    std::atomic<bool> g1_registered{false}, g2_registered{false};
    std::atomic<int> s1{0}, s2{0};
    WaitNode m0, m1, m2;
    ReadyFlag release_g0;

    Fiber g0;
    g0.set_entry([&](Fiber&) {
        mtx.lock(m0);   // G0 owns (immediate)
        // yield the worker so G1/G2 register + suspend on the owned mutex
        sched.await_ready_flag(release_g0.v);
        mtx.unlock();   // handoff to FIFO head (G1, registered first)
    });
    Fiber g1;
    g1.set_entry([&](Fiber&) {
        g1_registered.store(true, std::memory_order_release);
        mtx.lock(m1);   // suspends (owned by G0)
        s1.store(1, std::memory_order_release);
        mtx.unlock();   // handoff to G2
        s1.store(2, std::memory_order_release);
    });
    Fiber g2;
    g2.set_entry([&](Fiber&) {
        sched.await_ready_flag(g1_registered);   // g1 set it before suspending
        std::this_thread::yield();
        g2_registered.store(true, std::memory_order_release);
        mtx.lock(m2);   // suspends (owned by G0, then G1)
        s2.store(1, std::memory_order_release);
        mtx.unlock();
        s2.store(2, std::memory_order_release);
    });
    // coordinator: once both queued, release G0 to unlock
    ReadyFlag both_queued;
    Fiber gcoord;
    gcoord.set_entry([&](Fiber&) {
        sched.await_ready_flag(g2_registered);
        std::this_thread::yield();
        release_g0.set();   // G0 may now unlock (handoff to G1)
        both_queued.set();
    });

    FiberStack sg0, sg1, sg2, sgc;
    SLUICE_CHECK(sched.init_fiber(g0, sg0.base(), sg0.size()));
    SLUICE_CHECK(sched.init_fiber(g1, sg1.base(), sg1.size()));
    SLUICE_CHECK(sched.init_fiber(g2, sg2.base(), sg2.size()));
    SLUICE_CHECK(sched.init_fiber(gcoord, sgc.base(), sgc.size()));
    sched.spawn(g1);
    sched.spawn(g2);
    sched.spawn(gcoord);
    sched.spawn(g0);
    sched.run(1);

    SLUICE_CHECK_MSG(s1.load() == 2, "G1 resumed owning then unlocked");
    SLUICE_CHECK_MSG(s2.load() == 2, "G2 resumed owning then unlocked");
    SLUICE_CHECK_MSG(m1.was_woken(), "G1 resolved Woken (handoff)");
    SLUICE_CHECK_MSG(m2.was_woken(), "G2 resolved Woken (handoff)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// Slice 2 — No barging + owner-before-publication
// ===========================================================================

// ---- T4: no barging — newcomer try_lock fails while a waiter is queued -----
//
// owner G0 holds the lock and yields; W1 queues (suspends); a newcomer's
// try_lock MUST fail (W1 has FIFO priority). Then G0 unlocks -> W1 owns.
SLUICE_TEST_CASE(mtx_t4_no_barging_newcomer_trylock_fails) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    std::atomic<bool> w1_registered{false}, newcomer_done{false};
    std::atomic<bool> nres{true};  // expect false
    std::atomic<bool> w1_resumed{false};
    WaitNode m0, m1;
    ReadyFlag release_g0;

    Fiber g0;
    g0.set_entry([&](Fiber&) {
        mtx.lock(m0);  // G0 owns
        sched.await_ready_flag(release_g0.v);  // yield until newcomer tried
        mtx.unlock();  // handoff to W1 (FIFO head)
    });
    Fiber gw1;
    gw1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        mtx.lock(m1);  // suspends (owned by G0)
        w1_resumed.store(true, std::memory_order_release);
        mtx.unlock();
    });
    Fiber gnew;
    gnew.set_entry([&](Fiber&) {
        sched.await_ready_flag(w1_registered);  // W1 queued
        std::this_thread::yield();
        nres.store(!mtx.try_lock(), std::memory_order_release);  // expect false
        release_g0.set();   // let G0 unlock
        newcomer_done.store(true, std::memory_order_release);
    });

    FiberStack s0, s1, s2;
    SLUICE_CHECK(sched.init_fiber(g0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(gw1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(gnew, s2.base(), s2.size()));
    sched.spawn(g0);    // G0 acquires + suspends (await_ready_flag) first
    sched.spawn(gw1);   // then W1 queues (suspends on owned mutex)
    sched.spawn(gnew);  // then newcomer try_lock fails, releases G0
    sched.run(1);

    SLUICE_CHECK_MSG(newcomer_done.load(), "newcomer try_lock ran");
    SLUICE_CHECK_MSG(nres.load(), "newcomer try_lock failed while W1 queued");
    SLUICE_CHECK_MSG(w1_resumed.load(), "W1 resumed owning after G0 unlock");
    SLUICE_CHECK_MSG(m1.was_woken(), "W1 (not newcomer) got the handoff");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T5: owner-before-publication (deterministic phase seam) ---------------
//
// Arm mutex_handoff_before_publication. G0 unlocks; MUTEX-HANDOFF-ONE
// commits owner_ = winner and PAUSES before publication. At that paused point
// the winner is not yet published runnable. Then release; W1 resumes owning.
SLUICE_TEST_CASE(mtx_t5_owner_before_publication_phase) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    ControllerGuard cg(sched);
    WaitNode n0, n1;
    std::atomic<bool> w1_registered{false}, w1_resumed{false};
    ReadyFlag release_g0;

    Fiber g0;
    g0.set_entry([&](Fiber&) {
        mtx.lock(n0);
        AsyncMutexSeam::arm_handoff_before_publication(sched);
        sched.await_ready_flag(release_g0.v);  // yield until W1 queued
        mtx.unlock();  // MUTEX-HANDOFF-ONE pauses after owner commit
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        mtx.lock(n1);  // suspends; handoff winner
        w1_resumed.store(true, std::memory_order_release);
        mtx.unlock();
    });

    FiberStack s0, s1;
    SLUICE_CHECK(sched.init_fiber(g0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(w1, s1.base(), s1.size()));
    sched.spawn(g0);
    sched.spawn(w1);

    std::thread coord([&] {
        for (int i = 0; i < 10000; ++i) {
            if (w1_registered.load(std::memory_order_acquire)) break;
            std::this_thread::yield();
        }
        release_g0.set();   // G0 unlocks -> handoff pauses
        for (int i = 0; i < 10000; ++i) {
            if (AsyncMutexSeam::is_handoff_paused(sched)) break;
            std::this_thread::yield();
        }
        SLUICE_CHECK_MSG(AsyncMutexSeam::is_handoff_paused(sched),
                         "handoff paused after owner commit, before publication");
        SLUICE_CHECK_MSG(!w1_resumed.load(std::memory_order_acquire),
                         "winner not yet published runnable (pre-publication)");
        AsyncMutexSeam::release_handoff(sched);
    });
    sched.run_live(1);
    coord.join();

    SLUICE_CHECK_MSG(w1_resumed.load(), "W1 resumed owning after phase release");
    SLUICE_CHECK_MSG(n1.was_woken(), "W1 resolved Woken (handoff)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// Slice 3 — Admission closure + cancellation
// ===========================================================================

// ---- T6: admission closure — owner releases during W1's admission window ---
//
// W1 enters admission; owner G0 unlocks during W1's register/recheck critical
// section. The closure must NOT strand W1: it acquires inline or G0's unlock
// hands off to W1. Either way W1 owns after, no lost wake.
SLUICE_TEST_CASE(mtx_t6_admission_closure_no_strand) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    WaitNode n0, n1;
    std::atomic<bool> w1_registered{false}, w1_resumed{false};
    ReadyFlag release_g0;

    Fiber g0;
    g0.set_entry([&](Fiber&) {
        mtx.lock(n0);
        sched.await_ready_flag(release_g0.v);  // yield until W1 is in admission
        mtx.unlock();  // release during W1's admission window
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        release_g0.set();   // tell G0 to unlock while W1 is admitting
        mtx.lock(n1);       // admission; G0 may unlock in the window
        w1_resumed.store(true, std::memory_order_release);
        mtx.unlock();
    });
    FiberStack s0, s1;
    SLUICE_CHECK(sched.init_fiber(g0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(w1, s1.base(), s1.size()));
    sched.spawn(g0);
    sched.spawn(w1);
    sched.run(1);
    SLUICE_CHECK_MSG(w1_resumed.load(), "W1 resumed owning (no strand)");
    SLUICE_CHECK_MSG(n1.was_woken(), "W1 resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T7: cancel a suspended waiter; repeated cancel false ------------------
SLUICE_TEST_CASE(mtx_t7_cancel_suspended_then_repeated_false) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    WaitNode n0, n1;
    std::atomic<bool> w1_registered{false}, w1_resumed{false};
    std::atomic<bool> cancel1{false}, cancel2{true};  // c2 expect false
    ReadyFlag release_g0;

    Fiber g0;
    g0.set_entry([&](Fiber&) {
        mtx.lock(n0);
        sched.await_ready_flag(release_g0.v);  // yield until W1 queued
        cancel1.store(mtx.cancel(n1), std::memory_order_release);
        cancel2.store(!mtx.cancel(n1), std::memory_order_release);  // expect false
        mtx.unlock();  // queue empty now -> owner = nullptr
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        release_g0.set();
        mtx.lock(n1);  // suspends
        w1_resumed.store(true, std::memory_order_release);
        // cancelled; do not unlock
    });
    FiberStack s0, s1;
    SLUICE_CHECK(sched.init_fiber(g0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(w1, s1.base(), s1.size()));
    sched.spawn(g0);
    sched.spawn(w1);
    sched.run(1);
    SLUICE_CHECK_MSG(cancel1.load(), "first cancel of suspended W1 succeeds");
    SLUICE_CHECK_MSG(cancel2.load(), "repeated cancel returns false");
    SLUICE_CHECK_MSG(w1_resumed.load(), "W1 resumed after cancel");
    SLUICE_CHECK_MSG(n1.was_cancelled(), "W1 resolved Cancelled");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T8: cancel after handoff false; wrong-mutex (same Scheduler) false ----
SLUICE_TEST_CASE(mtx_t8_cancel_after_handoff_and_wrong_mutex_false) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx_a(sched), mtx_b(sched);
    WaitNode n0, n1, nA;
    std::atomic<bool> w1_registered{false}, a_registered{false}, granted{false};
    std::atomic<bool> w1_resumed{false};
    std::atomic<bool> cancel_after_handoff{true}, wrong_mutex_cancel{true};
    ReadyFlag release_g0;

    Fiber g0;
    g0.set_entry([&](Fiber&) {
        mtx_a.lock(n0);
        sched.await_ready_flag(release_g0.v);
        mtx_a.unlock();  // handoff to W1
        granted.store(true, std::memory_order_release);
        cancel_after_handoff.store(!mtx_a.cancel(n1), std::memory_order_release);
        // wait for fA to have registered nA in mtx_a (it signals before lock)
        sched.await_ready_flag(a_registered);
        wrong_mutex_cancel.store(!mtx_b.cancel(nA), std::memory_order_release);
        (void)mtx_a.cancel(nA);  // drain nA cleanly
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        release_g0.set();
        mtx_a.lock(n1);  // suspends; handoff winner
        w1_resumed.store(true, std::memory_order_release);
        mtx_a.unlock();
    });
    Fiber fA;
    fA.set_entry([&](Fiber&) {
        // suspend (not spin) until W1 owns; single-worker cannot spin-wait here
        sched.await_ready_flag(granted);
        std::this_thread::yield();
        a_registered.store(true, std::memory_order_release);  // signal BEFORE lock
        mtx_a.lock(nA);  // suspends (owned by W1)
        mtx_a.unlock();
    });
    FiberStack s0, s1, s2;
    SLUICE_CHECK(sched.init_fiber(g0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(w1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(fA, s2.base(), s2.size()));
    sched.spawn(g0);
    sched.spawn(w1);
    sched.spawn(fA);
    sched.run(1);
    SLUICE_CHECK_MSG(cancel_after_handoff.load(),
                     "cancel after handoff returns false (Woken is terminal)");
    SLUICE_CHECK_MSG(wrong_mutex_cancel.load(), "wrong-mutex cancel returns false");
    SLUICE_CHECK_MSG(w1_resumed.load(), "W1 resumed owning");
    SLUICE_CHECK_MSG(n1.was_woken(), "W1 resolved Woken (handoff)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T9: wrong-mutex DIFFERENT Scheduler cancel returns false --------------
SLUICE_TEST_CASE(mtx_t9_wrong_mutex_different_scheduler_false) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx_a(std::make_unique<IdleBackend>()), ctx_b(std::make_unique<IdleBackend>());
    Scheduler sched_a(ctx_a), sched_b(ctx_b);
    AsyncMutex mtx_a(sched_a), mtx_b(sched_b);
    WaitNode n0, n1;
    std::atomic<bool> w1_registered{false}, done{false};
    std::atomic<bool> cross_cancel{true};  // expect false
    ReadyFlag release_g0;

    Fiber f0;
    f0.set_entry([&](Fiber&) {
        mtx_a.lock(n0);
        sched_a.await_ready_flag(release_g0.v);
        cross_cancel.store(!mtx_b.cancel(n1), std::memory_order_release);
        (void)mtx_a.cancel(n1);  // drain cleanly via the right mutex
        mtx_a.unlock();
        done.store(true, std::memory_order_release);
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        release_g0.set();
        mtx_a.lock(n1);  // suspends in sched_a/mtx_a
    });
    FiberStack s0, s1;
    SLUICE_CHECK(sched_a.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched_a.init_fiber(w1, s1.base(), s1.size()));
    sched_a.spawn(f0);
    sched_a.spawn(w1);
    sched_a.run(1);
    SLUICE_CHECK_MSG(cross_cancel.load(),
                     "cross-Scheduler wrong-mutex cancel returns false");
    SLUICE_CHECK_MSG(done.load(), "drained + unlocked cleanly");
    SLUICE_CHECK_MSG(sched_a.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T10: external OS-thread cancel succeeds safely ------------------------
SLUICE_TEST_CASE(mtx_t10_external_thread_cancel_succeeds) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    WaitNode n0, n1;
    std::atomic<bool> w1_registered{false}, ext_result{false}, w1_resumed{false};
    // f0 awaits this; the external thread sets it AFTER cancelling W1, so f0
    // resumes (the cancel's signal_wake wakes the parked worker) and unlocks.
    std::atomic<bool> cancel_done{false};

    Fiber f0;
    f0.set_entry([&](Fiber&) {
        mtx.lock(n0);
        // suspend until the external thread has cancelled W1
        sched.await_ready_flag(cancel_done);
        mtx.unlock();  // queue empty (W1 cancelled) -> owner = nullptr
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        mtx.lock(n1);  // suspends
        w1_resumed.store(true, std::memory_order_release);
        // cancelled; do not unlock
    });
    FiberStack s0, s1;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(w1, s1.base(), s1.size()));
    sched.spawn(f0);
    sched.spawn(w1);

    std::thread ext([&] {
        while (!w1_registered.load(std::memory_order::acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ext_result.store(mtx.cancel(n1), std::memory_order_release);
        // The cancel signaled the scheduler wake (route_runnable_locked ->
        // signal_wake_locked). Set cancel_done so f0's await_ready_flag resolves
        // on the worker's post-wake drain.
        cancel_done.store(true, std::memory_order_release);
    });
    sched.run_live(1);
    ext.join();
    SLUICE_CHECK_MSG(ext_result.load(), "external-thread cancel succeeded");
    SLUICE_CHECK_MSG(w1_resumed.load(), "W1 resumed after external cancel");
    SLUICE_CHECK_MSG(n1.was_cancelled(), "W1 resolved Cancelled");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// Slice 4 — Deadline precedence + races
// ===========================================================================

// ---- T11: lock_until free + already-due -> Woken (resource-first) ---------
SLUICE_TEST_CASE(mtx_t11_lock_until_free_due_is_woken) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 100);
    WaitNode node;
    std::atomic<int> entries{0};
    Fiber f;
    f.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        mtx.lock_until(node, /*deadline=*/50);  // due, but free
        entries.fetch_add(1, std::memory_order_acq_rel);
        mtx.unlock();
        entries.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack s;
    SLUICE_CHECK(sched.init_fiber(f, s.base(), s.size()));
    sched.spawn(f);
    sched.run(1);
    SLUICE_CHECK_MSG(entries.load() == 3, "lock+unlock completed");
    SLUICE_CHECK_MSG(node.was_woken(), "free + due -> Woken (ownership beats due deadline)");
    SLUICE_CHECK_MSG(!node.was_expired(), "not Expired (resource-first)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "timer retired at admission (no leak)");
}

// ---- T12: lock_until owned + already-due -> Expired (does not own) ---------
SLUICE_TEST_CASE(mtx_t12_lock_until_owned_due_is_expired) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 100);
    WaitNode n0, n1;
    std::atomic<bool> w1_registered{false}, w1_resumed{false};
    ReadyFlag release_g0;

    Fiber f0;
    f0.set_entry([&](Fiber&) {
        mtx.lock(n0);
        sched.await_ready_flag(release_g0.v);
        mtx.unlock();  // queue empty (W1 expired) -> owner = nullptr
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        release_g0.set();
        mtx.lock_until(n1, /*deadline=*/50);  // owned + due -> Expired at admission
        w1_resumed.store(true, std::memory_order_release);
        // expired; do not unlock
    });
    FiberStack s0, s1;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(w1, s1.base(), s1.size()));
    sched.spawn(f0);
    sched.spawn(w1);
    sched.run(1);
    SLUICE_CHECK_MSG(w1_resumed.load(), "W1 resumed (expired at admission)");
    SLUICE_CHECK_MSG(n1.was_expired(), "owned + due -> Expired");
    SLUICE_CHECK_MSG(!n1.was_woken(), "W1 did not acquire");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "timer retired at admission (no leak)");
}

// ---- T13: unlock wins before timer (handoff; timer loses) ------------------
SLUICE_TEST_CASE(mtx_t13_unlock_wins_before_timer) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 0);
    WaitNode n0, n1;
    std::atomic<bool> w1_registered{false}, granted{false}, w1_resumed{false};
    ReadyFlag release_g0;

    Fiber f0;
    f0.set_entry([&](Fiber&) {
        mtx.lock(n0);
        sched.await_ready_flag(release_g0.v);
        mtx.unlock();  // handoff -> W1 Woken
        granted.store(true, std::memory_order_release);
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        release_g0.set();
        mtx.lock_until(n1, /*deadline=*/1000);  // future; suspends
        w1_resumed.store(true, std::memory_order_release);
        mtx.unlock();
    });
    Fiber fdrv;
    fdrv.set_entry([&](Fiber&) {
        // suspend (not spin) until the handoff wins; single-worker cannot
        // spin-wait while f0/w1 have not yet run
        sched.await_ready_flag(granted);
        std::this_thread::yield();
        sched.advance_clock(2000);  // late timer fires (loser)
    });
    FiberStack s0, s1, sd;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(w1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(fdrv, sd.base(), sd.size()));
    sched.spawn(f0);
    sched.spawn(w1);
    sched.spawn(fdrv);
    sched.run(1);
    SLUICE_CHECK_MSG(w1_resumed.load(), "W1 resumed owning (handoff won)");
    SLUICE_CHECK_MSG(n1.was_woken(), "W1 resolved Woken");
    SLUICE_CHECK_MSG(!n1.was_expired(), "late timer did not expire W1");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "timer retired exactly once (no leak)");
}

// ---- T14: timer wins before unlock (Expired; unlock frees) ----------------
SLUICE_TEST_CASE(mtx_t14_timer_wins_before_unlock) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 0);
    WaitNode n0, n1;
    std::atomic<bool> w1_registered{false}, expired{false}, w1_resumed{false};
    ReadyFlag release_g0;

    Fiber f0;
    f0.set_entry([&](Fiber&) {
        mtx.lock(n0);
        sched.await_ready_flag(release_g0.v);
        // suspend (not spin) until the timer driver expires W1
        sched.await_ready_flag(expired);
        mtx.unlock();  // queue empty (W1 expired) -> owner = nullptr
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        release_g0.set();
        mtx.lock_until(n1, /*deadline=*/100);  // suspends
        w1_resumed.store(true, std::memory_order_release);
        // expired; do not unlock
    });
    Fiber fdrv;
    fdrv.set_entry([&](Fiber&) {
        sched.await_ready_flag(w1_registered);
        std::this_thread::yield();
        for (int i = 0; i < 200 && !n1.is_terminal(); ++i) {
            sched.advance_clock(100);
            std::this_thread::yield();
        }
        expired.store(n1.was_expired(), std::memory_order_release);
    });
    FiberStack s0, s1, sd;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(w1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(fdrv, sd.base(), sd.size()));
    sched.spawn(f0);
    sched.spawn(w1);
    sched.spawn(fdrv);
    sched.run(1);
    SLUICE_CHECK_MSG(w1_resumed.load(), "W1 resumed (expired)");
    SLUICE_CHECK_MSG(n1.was_expired(), "W1 resolved Expired (timer won)");
    SLUICE_CHECK_MSG(!n1.was_woken(), "W1 did not acquire");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "timer retired exactly once (no leak)");
}

// ===========================================================================
// Slice 5 — Cancel races + exactly-once + destruction
// ===========================================================================

// ---- T15: cancel wins before unlock (Cancelled; unlock skips it) -----------
SLUICE_TEST_CASE(mtx_t15_cancel_wins_before_unlock) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    WaitNode n0, n1;
    std::atomic<bool> w1_registered{false}, cancelled{false}, w1_resumed{false};
    ReadyFlag release_g0;

    Fiber f0;
    f0.set_entry([&](Fiber&) {
        mtx.lock(n0);
        sched.await_ready_flag(release_g0.v);
        // suspend (not spin) until the canceller resolves W1
        sched.await_ready_flag(cancelled);
        mtx.unlock();  // queue empty (W1 cancelled) -> owner = nullptr
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        release_g0.set();
        mtx.lock(n1);  // suspends
        w1_resumed.store(true, std::memory_order_release);
    });
    Fiber fcan;
    fcan.set_entry([&](Fiber&) {
        sched.await_ready_flag(w1_registered);
        std::this_thread::yield();
        for (int i = 0; i < 200 && !n1.is_terminal(); ++i) {
            if (mtx.cancel(n1)) break;
            std::this_thread::yield();
        }
        cancelled.store(n1.was_cancelled(), std::memory_order_release);
    });
    FiberStack s0, s1, sc;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(w1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(fcan, sc.base(), sc.size()));
    sched.spawn(f0);
    sched.spawn(w1);
    sched.spawn(fcan);
    sched.run(1);
    SLUICE_CHECK_MSG(w1_resumed.load(), "W1 resumed (cancelled)");
    SLUICE_CHECK_MSG(n1.was_cancelled(), "W1 resolved Cancelled (cancel won)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T16: handoff wins before cancel (Woken; cancel false; no republish) ---
SLUICE_TEST_CASE(mtx_t16_handoff_wins_before_cancel) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    WaitNode n0, n1;
    std::atomic<bool> w1_registered{false}, granted{false}, w1_resumed{false};
    std::atomic<bool> late_cancel{true};  // expect false
    ReadyFlag release_g0;

    Fiber f0;
    f0.set_entry([&](Fiber&) {
        mtx.lock(n0);
        sched.await_ready_flag(release_g0.v);
        mtx.unlock();  // handoff -> W1 Woken
        granted.store(true, std::memory_order_release);
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        release_g0.set();
        mtx.lock(n1);  // suspends; handoff winner
        w1_resumed.store(true, std::memory_order_release);
        mtx.unlock();
    });
    Fiber fcan;
    fcan.set_entry([&](Fiber&) {
        sched.await_ready_flag(granted);  // handoff won first
        std::this_thread::yield();
        late_cancel.store(!mtx.cancel(n1), std::memory_order_release);  // expect false
    });
    FiberStack s0, s1, sc;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(w1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(fcan, sc.base(), sc.size()));
    sched.spawn(f0);
    sched.spawn(w1);
    sched.spawn(fcan);
    sched.run(1);
    SLUICE_CHECK_MSG(w1_resumed.load(), "W1 resumed owning (handoff won)");
    SLUICE_CHECK_MSG(n1.was_woken(), "W1 resolved Woken");
    SLUICE_CHECK_MSG(late_cancel.load(), "late cancel after handoff returns false");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T17: exactly-once — one resolve, one publication, one resume ---------
SLUICE_TEST_CASE(mtx_t17_exactly_once_handoff) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    WaitNode n0, n1;
    std::atomic<bool> w1_registered{false};
    std::atomic<int> entries{0};
    ReadyFlag release_g0;

    Fiber f0;
    f0.set_entry([&](Fiber&) {
        mtx.lock(n0);
        sched.await_ready_flag(release_g0.v);
        mtx.unlock();  // handoff -> W1
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        release_g0.set();
        entries.fetch_add(1, std::memory_order_acq_rel);  // pre
        mtx.lock(n1);
        entries.fetch_add(1, std::memory_order_acq_rel);  // post (resumed exactly once)
        mtx.unlock();
    });
    FiberStack s0, s1;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(w1, s1.base(), s1.size()));
    sched.spawn(f0);
    sched.spawn(w1);
    sched.run(1);
    int outcomes = 0;
    if (n1.was_woken()) ++outcomes;
    if (n1.was_cancelled()) ++outcomes;
    if (n1.was_expired()) ++outcomes;
    SLUICE_CHECK_MSG(outcomes == 1, "exactly one terminal outcome");
    SLUICE_CHECK_MSG(entries.load() == 2, "exactly one resume (entries == 2)");
    SLUICE_CHECK_MSG(n1.was_woken(), "the one outcome is Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "one unlink (queue drained)");
}

// ---- T18: destruction — safe unlocked/empty -------------------------------
SLUICE_TEST_CASE(mtx_t18_destruction_safe_unlocked_empty) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    WaitNode n;
    Fiber f;
    f.set_entry([&](Fiber&) {
        mtx.lock(n);
        mtx.unlock();
    });
    FiberStack s;
    SLUICE_CHECK(sched.init_fiber(f, s.base(), s.size()));
    sched.spawn(f);
    sched.run(1);
    // ~AsyncMutex runs at scope exit with owner_ == nullptr; reaching the end
    // proves the destruction contract holds.
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// Slice 6 — Real E8 migration
// ===========================================================================

// ---- T19: lock-own-migrate-unlock — ownership survives real E8 steal -----
//
// DETERMINISTIC proof that AsyncMutex Fiber*-ownership survives real E8
// migration BETWEEN lock and unlock. Corrective-3: proves the required trace
// (acquire on W0 -> migrate to W1 while owning -> unlock on W1), AND closes
// the blocker-execution race left open by Corrective-2.
//
// THE BLOCKER-EXECUTION RACE (Corrective-2's residual defect): the Corrective-2
// coordinator gated `flag_wake` on `a_suspended` + `waiting_count()>0`. BOTH
// flip BEFORE W0's worker loop pops f_blocker:
//   - a_suspended    is set by fA immediately before await_ready_flag;
//   - waiting_count  flips when fA registers in waiting_ready_ (inside
//                    await_ready_flag).
// So there was a window where f_blocker was still STEALABLE in W0's runnable
// queue at the moment the coordinator released flag_wake (freeing W1). Passing
// repetitions did not structurally exclude f_blocker being stolen to W1.
//
// THE HANDSHAKE THAT CLOSES IT (Corrective-3 + Corrective-4):
//   Corrective-3 added the explicit `blocker_running` handshake.
//   Waiting_count()>0 was also part of the Corrective-3 gate but was an
//   unsynchronized read of Scheduler guarded state (a C++ data race).
//   Corrective-4 removes it: blocker_running alone is the authoritative
//   suspension and W0-occupancy proof.
//
// f_blocker was queued behind fA on W0. f_blocker can execute only after
// fA has context-switched out through await_ready_flag (completed
// registration, committed Waiting via make_waiting(), and yielded the
// worker). Therefore observing blocker_running==true proves fA completed
// its suspension transition AND f_blocker is ws->current on W0 — popped
// from W0's runnable queue and no longer stealable.
//
// blocker_running is published by f_blocker WHILE EXECUTING ON W0 — after
// an assertion that current_worker_id()==0 — using release store. The
// coordinator acquire-loads it. This is state OBSERVATION, never
// sleep_for-based causal sync.
//
// Determinism discipline (mirrors steal_t3): a steal is deterministic ONLY when
// the victim (W0) is provably BUSY running a fiber (cannot pop its own queue)
// and the thief (W1) is IDLE (so it steals). Two load-bearing anti-race gates:
//   (1) f_idle spins on flag_wake on W1 — W1 cannot steal f_blocker while fA
//       is between "queued f_blocker" and "suspended + blocker_running". By the
//       time flag_wake is set, f_blocker is already RUNNING on W0 (observed).
//   (2) f_blocker spins on W0 — W0 cannot pop fA after fA is routed. Released
//       only AFTER the coordinator observes unlock_worker==1.
//
// ORDERED CAUSAL TRACE (checkpoints recorded below):
//   A_LOCKED_ON_W0       fA acquires on W0
//   A_WAITING_WHILE_OWNING fA suspends while owning (a_suspended)
//   BLOCKER_RUNNING_ON_W0 W0 popped f_blocker; it is ws->current on W0
//   WAKE_RELEASED        coordinator sets flag_wake (all three gates passed)
//   A_RESUMED_ON_W1      fA resumes on the thief W1
//   A_UNLOCKED_ON_W1     fA unlocks on W1 (unlock_worker==1)
//   BLOCKER_RELEASED     coordinator releases f_blocker (after unlock observed)
//   F2_ACQUIRED          f2 acquires the now-free mutex (ownership released)
//
// run_live (NOT run/drain) is mandatory: while fA is suspended in waiting_ready_
// the classifier is MW-S3-unresolved; DRAIN would RETURN STALLED. external_wake
// is possible (waiting_ready_ non-empty), so LIVE keeps W1 resident (parks).
//
// Suspension mechanism: await_ready_flag / wake_ready_flags_locked. The ready
// flag is an INDEPENDENT test mechanism (a plain atomic); it does NOT touch the
// AsyncMutex owner or WaitNode. fA retains AsyncMutex ownership throughout.
SLUICE_TEST_CASE(mtx_t19_real_migration_lock_own_unlock) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);

    // Independent ready flag (the suspend/resume mechanism; does NOT touch
    // the AsyncMutex owner or WaitNode). Set by the coordinator OS-thread;
    // observed by the worker-loop classify (wake_ready_flags_locked).
    std::atomic<bool> flag_wake{false};

    // Checkpoint atomics and WaitNodes. Ordered per the causal trace above.
    std::atomic<bool> a_acquired{false};        // A_LOCKED_ON_W0
    std::atomic<bool> a_suspended{false};       // A_WAITING_WHILE_OWNING
    std::atomic<bool> blocker_running{false};   // BLOCKER_RUNNING_ON_W0
    std::atomic<bool> a_unlocked{false};        // A_UNLOCKED_ON_W1
    std::atomic<unsigned> acquire_worker{static_cast<unsigned>(-1)};
    std::atomic<unsigned> unlock_worker{static_cast<unsigned>(-1)};
    std::atomic<bool> f2_acquired{false};       // F2_ACQUIRED
    WaitNode nA;   // fA's mutex lock WaitNode
    WaitNode nF2;  // f2's mutex lock WaitNode

    // f_blocker: queued on W0 by fA (spawn_on from inside fA). After fA
    // suspends, W0 pops f_blocker. Its FIRST meaningful act is to assert it is
    // on W0 and publish blocker_running=true (release) — the handshake that
    // proves f_blocker is ws->current on W0 (no longer stealable). It then
    // spins on release_blocker, keeping W0 busy until the coordinator observes
    // unlock_worker==1. blocker_running is the OBSERVED anti-race state; it is
    // not inferred from waiting_count/running_fiber_count/completion.
    std::atomic<bool> release_blocker{false};
    Fiber f_blocker;
    f_blocker.set_entry([&](Fiber&) {
        SLUICE_CHECK_MSG(Scheduler::current_worker_id() == 0,
                         "f_blocker executes on W0");
        blocker_running.store(true, std::memory_order_release);
        while (!release_blocker.load(std::memory_order_acquire)) {}
    });

    // fA: acquires mutex on W0, queues f_blocker on W0 (behind the still-
    // running fA), then suspends on flag_wake while STILL OWNING the mutex.
    Fiber fA;
    fA.set_entry([&](Fiber&) {
        acquire_worker.store(Scheduler::current_worker_id(), std::memory_order_release);
        mtx.lock(nA);  // ACQUIRE on W0 (immediate; free mutex)
        // Queue f_blocker on THIS worker (W0) while fA is still Running, so
        // f_blocker sits in W0's local_runnable behind fA. W0 cannot pop it
        // until fA suspends.
        sched.spawn_on(f_blocker, Scheduler::current_worker_id());
        a_acquired.store(true, std::memory_order_release);
        a_suspended.store(true, std::memory_order_release);  // about to suspend
        // Suspend on the independent flag while STILL OWNING the mutex.
        sched.await_ready_flag(flag_wake);
        // RESUMED on W1 (after W1 stole fA from W0's queue).
        unlock_worker.store(Scheduler::current_worker_id(), std::memory_order_release);
        mtx.unlock();  // UNLOCK on W1
        a_unlocked.store(true, std::memory_order_release);
    });

    // f_idle: runs on W1. SPINS until flag_wake is set, keeping W1 BUSY so it
    // cannot steal f_blocker off W0's queue while fA is between "queued
    // f_blocker" and "blocker_running observed". Only after the coordinator
    // sets flag_wake (all three gates passed) does f_idle complete, leaving W1
    // idle to steal — by which point f_blocker is RUNNING on W0 (not stealable)
    // and the only stealable ticket on W0 will be fA once the classify routes it.
    Fiber f_idle;
    f_idle.set_entry([&](Fiber&) {
        while (!flag_wake.load(std::memory_order_acquire)) {}
    });

    // f2: acquires mutex AFTER fA unlocks, proving ownership was released.
    Fiber f2;
    f2.set_entry([&](Fiber&) {
        mtx.lock(nF2);
        f2_acquired.store(true, std::memory_order_release);
        mtx.unlock();
    });

    FiberStack sA, si, sb, s2;
    SLUICE_CHECK(sched.init_fiber(fA, sA.base(), sA.size()));
    SLUICE_CHECK(sched.init_fiber(f_idle, si.base(), si.size()));
    SLUICE_CHECK(sched.init_fiber(f_blocker, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    // fA first (-> W0), f_idle second (-> W1). Round-robin: next_spawn=0.
    sched.spawn(fA);       // W0
    sched.spawn(f_idle);   // W1

    // run_live: the run stays resident while fA is suspended (MW-S3 + external
    // wake capable -> W1 parks instead of terminating the run). With run(2)
    // (drain) the run would RETURN STALLED as soon as fA suspends.
    std::thread runner([&] { sched.run_live(2); });

    // On any coordinator-side gate failure, set the release flags so the LIVE
    // run can drain and runner.join() returns instead of hanging. These guards
    // are FAILURE BOUNDS, not causal synchronisation.
    auto release_for_drain = [&] {
        release_blocker.store(true, std::memory_order_release);
        flag_wake.store(true, std::memory_order_release);
    };

    // 1. Wait for fA to have acquired the mutex AND reached its suspend point.
    //    a_suspended is set immediately before await_ready_flag, so by the time
    //    we observe it fA is (or is about to be) registered in waiting_ready_.
    if (!bounded_wait(a_acquired)) {
        release_for_drain();
        runner.join();
        SLUICE_CHECK_MSG(a_acquired.load(), "A_LOCKED_ON_W0: fA acquired on W0");
        return;
    }
    if (!bounded_wait(a_suspended)) {
        release_for_drain();
        runner.join();
        SLUICE_CHECK_MSG(a_suspended.load(),
                         "A_WAITING_WHILE_OWNING: fA suspended while owning");
        return;
    }

    // 2. THE ANTI-RACE HANDSHAKE. Observe blocker_running before releasing
    //    flag_wake. blocker_running==true proves f_blocker is executing on W0
    //    (wrote the flag from W0's TLS context after asserting
    //    current_worker_id()==0). Because f_blocker was queued behind fA on
    //    W0, f_blocker can execute only after fA completed await_ready_flag
    //    (registered in waiting_ready_, committed Waiting via make_waiting(),
    //    and context-switched away). Therefore blocker_running is the
    //    authoritative suspension AND W0-occupancy proof.
    if (!bounded_wait(blocker_running)) {
        release_for_drain();
        runner.join();
        SLUICE_CHECK_MSG(blocker_running.load(),
                         "BLOCKER_RUNNING_ON_W0: f_blocker running on W0 before wake");
        return;
    }

    // 3. WAKE_RELEASED. Make fA runnable: set flag_wake. The worker-loop
    //    classify (wake_ready_flags_locked) routes fA back onto W0's queue
    //    (owner=W0, recorded at await_ready_flag time). W0 is busy spinning
    //    f_blocker, so W0 cannot pop fA; the idle W1 steals it.
    flag_wake.store(true, std::memory_order_release);

    // 4. Wait for fA to resume on W1 (the thief) and unlock.
    if (!bounded_wait(a_unlocked)) {
        release_for_drain();
        runner.join();
        SLUICE_CHECK_MSG(a_unlocked.load(), "A_UNLOCKED_ON_W1: fA resumed on W1 and unlocked");
        return;
    }

    // 5. The coordinator releases f_blocker ONLY AFTER observing unlock_worker
    //    == 1 AND a_unlocked. spawn f2 so it acquires AFTER fA unlocks (proves
    //    ownership released). It is routed round-robin and acquires the now-free
    //    mutex.
    sched.spawn(f2);
    if (!bounded_wait(f2_acquired)) {
        release_for_drain();
        runner.join();
        SLUICE_CHECK_MSG(f2_acquired.load(), "F2_ACQUIRED: f2 acquired after unlock");
        return;
    }

    // 6. Release f_blocker so the run can terminate cleanly.
    release_blocker.store(true, std::memory_order_release);
    runner.join();

    unsigned aw = acquire_worker.load(std::memory_order_acquire);
    unsigned uw = unlock_worker.load(std::memory_order_acquire);

    // THE OWNERSHIP-MIGRATION ASSERTIONS (all observed by fA itself).
    SLUICE_CHECK_MSG(blocker_running.load(),
                     "f_blocker was executing on W0 (anti-race handshake observed)");
    SLUICE_CHECK_MSG(aw == 0, "acquire_worker == W0");
    SLUICE_CHECK_MSG(uw == 1, "unlock_worker == W1 (stolen from W0 while owning)");
    SLUICE_CHECK_MSG(aw != uw, "worker changed between lock and unlock");
    SLUICE_CHECK_MSG(a_unlocked.load(), "fA resumed after migration and unlocked");
    SLUICE_CHECK_MSG(nA.was_woken(), "mutex acquired (Woken)");
    SLUICE_CHECK_MSG(f2_acquired.load(), "F2 acquired after unlock (ownership released)");
    SLUICE_CHECK_MSG(nF2.was_woken(), "F2 resolved Woken");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
    SLUICE_CHECK_MSG(fA.state() == FiberState::done, "fA completed");
    SLUICE_CHECK_MSG(f2.state() == FiberState::done, "f2 completed");
}

// ===========================================================================
// Slice 7 — Coordination stress gate (500/500)
// ===========================================================================

// ---- T20: coordination stress — 500/500 iterations ------------------------
//
// Repeated gate covering FIFO handoff, admission closure, cancel-vs-handoff,
// and exactly-once publication/resume. 500 iterations; each resolves all
// waiters with no queue leak. Per repo convention (e12_semaphore_test T30 /
// e12_event_test), the gate is assertion-only: total_resolved == ITERS * K
// proves 500/500.
SLUICE_TEST_CASE(mtx_t20_coordination_500) {
    if constexpr (!fiber_ctx::supported) return;
    constexpr int ITERS = 500;
    constexpr int K = 2;
    int total_woken = 0, total_cancelled = 0;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);

    for (int it = 0; it < ITERS; ++it) {
        AsyncMutex mtx(sched);
        WaitNode n0, n[K];
        std::atomic<bool> w_registered[K];
        std::atomic<bool> all_registered{false};
        std::atomic<int> reg_count{0};
        std::atomic<bool> cancelled{false};
        for (int k = 0; k < K; ++k) w_registered[k].store(false, std::memory_order_relaxed);
        ReadyFlag release_g0;

        Fiber f0;
        f0.set_entry([&](Fiber&) {
            mtx.lock(n0);
            sched.await_ready_flag(release_g0.v);
            mtx.unlock();
        });
        Fiber fwait[K];
        for (int k = 0; k < K; ++k) {
            fwait[k].set_entry([&, k](Fiber&) {
                if (k > 0) { sched.await_ready_flag(w_registered[k - 1]); std::this_thread::yield(); }
                w_registered[k].store(true, std::memory_order_release);
                if (reg_count.fetch_add(1, std::memory_order_acq_rel) == K - 1) {
                    all_registered.store(true, std::memory_order_release);
                    release_g0.set();
                }
                mtx.lock(n[k]);  // suspends; resolves Woken (handoff) or Cancelled
                if (n[k].was_woken()) mtx.unlock();  // only the owner unlocks
                // a Cancelled waiter does NOT own the mutex; do not unlock
            });
        }
        Fiber fcan;
        fcan.set_entry([&](Fiber&) {
            sched.await_ready_flag(all_registered);
            std::this_thread::yield();
            for (int i = 0; i < 50; ++i) {
                if (mtx.cancel(n[K - 1])) { cancelled.store(true); break; }
                std::this_thread::yield();
            }
        });

        FiberStack s0, sw[K], sc;
        SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
        for (int k = 0; k < K; ++k) SLUICE_CHECK(sched.init_fiber(fwait[k], sw[k].base(), sw[k].size()));
        SLUICE_CHECK(sched.init_fiber(fcan, sc.base(), sc.size()));
        sched.spawn(f0);
        for (int k = 0; k < K; ++k) sched.spawn(fwait[k]);
        sched.spawn(fcan);
        sched.run(1);

        for (int k = 0; k < K; ++k) {
            if (n[k].was_woken()) ++total_woken;
            else if (n[k].was_cancelled()) ++total_cancelled;
        }
        SLUICE_CHECK_MSG(sched.waiting_count() == 0, "iteration drained (no queue leak)");
    }
    SLUICE_CHECK_MSG(total_woken + total_cancelled == ITERS * K,
                     "500/500: all iterations resolved all waiters");
    SLUICE_CHECK_MSG(total_cancelled > 0, "cancel won at least once");
    SLUICE_CHECK_MSG(total_woken > 0, "handoff won at least once");
}

// ===========================================================================
// Slice 8 — Three-party no-barging causal proof
// ===========================================================================

// ---- T21: three-party no-barging — newcomer try_lock fails while two waiters queued
//
// F0 owns AsyncMutex. W1 queues first. W2 queues second.
// Newcomer N attempts try_lock -> false (F0 owns, W1 + W2 queued).
// F0 unlocks -> W1 owns. W1 unlocks -> W2 owns.
// This is the runtime proof of the no-barging topology theorem:
//   InvNoOwnerlessQueuedDemand + InvImmediateAcquireRequiresEmptyEligiblePreQueue
//   + InvFIFOGrant + InvGrantOwnerCommit imply an arriving Fiber cannot bypass
//   an older eligible queued waiter.
SLUICE_TEST_CASE(mtx_t21_three_party_no_barging) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    WaitNode n0, n1, n2;
    std::atomic<bool> w1_registered{false}, w2_registered{false};
    std::atomic<bool> newcomer_result{true};  // expect false
    std::atomic<bool> w1_resumed{false}, w2_resumed{false};
    ReadyFlag release_f0;

    Fiber f0;
    f0.set_entry([&](Fiber&) {
        mtx.lock(n0);  // F0 owns
        sched.await_ready_flag(release_f0.v);  // yield until both queued + newcomer tried
        mtx.unlock();  // handoff to W1 (FIFO head)
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        mtx.lock(n1);  // suspends (owned by F0)
        w1_resumed.store(true, std::memory_order_release);
        mtx.unlock();  // handoff to W2
    });
    Fiber w2;
    w2.set_entry([&](Fiber&) {
        sched.await_ready_flag(w1_registered);
        std::this_thread::yield();
        w2_registered.store(true, std::memory_order_release);
        mtx.lock(n2);  // suspends (owned by F0, then W1)
        w2_resumed.store(true, std::memory_order_release);
        mtx.unlock();
    });
    Fiber newcomer;
    newcomer.set_entry([&](Fiber&) {
        sched.await_ready_flag(w2_registered);  // both queued
        std::this_thread::yield();
        newcomer_result.store(!mtx.try_lock(), std::memory_order_release);  // expect false
        release_f0.set();  // let F0 unlock
    });

    FiberStack s0, s1, s2, sn;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(w1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(w2, s2.base(), s2.size()));
    SLUICE_CHECK(sched.init_fiber(newcomer, sn.base(), sn.size()));
    sched.spawn(f0);    // F0 acquires + suspends first
    sched.spawn(w1);    // W1 queues
    sched.spawn(w2);    // W2 queues
    sched.spawn(newcomer);  // newcomer try_lock fails, releases F0
    sched.run(1);

    SLUICE_CHECK_MSG(newcomer_result.load(), "newcomer try_lock failed (F0 owns, W1+W2 queued)");
    SLUICE_CHECK_MSG(w1_resumed.load(), "W1 resumed owning after F0 unlock");
    SLUICE_CHECK_MSG(w2_resumed.load(), "W2 received ownership after W1 unlock");
    SLUICE_CHECK_MSG(n1.was_woken(), "W1 resolved Woken (handoff from F0)");
    SLUICE_CHECK_MSG(n2.was_woken(), "W2 resolved Woken (handoff from W1)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- T22: cancelled-head then handoff — newcomer cannot barge ---------------
//
// F0 owns. W1 queues. W2 queues. W1 cancellation wins.
// Newcomer attempts try_lock before F0 unlocks -> false (W2 still queued).
// F0 unlocks -> W2 receives ownership.
SLUICE_TEST_CASE(mtx_t22_cancelled_head_then_handoff) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx(sched);
    WaitNode n0, n1, n2;
    std::atomic<bool> w1_registered{false}, w2_registered{false};
    std::atomic<bool> w1_cancelled{false}, newcomer_result{true};  // expect false
    std::atomic<bool> w2_resumed{false};
    ReadyFlag release_f0;

    Fiber f0;
    f0.set_entry([&](Fiber&) {
        mtx.lock(n0);
        sched.await_ready_flag(release_f0.v);  // yield until newcomer tried
        mtx.unlock();  // queue has W2 (W1 cancelled) -> handoff to W2
    });
    Fiber w1;
    w1.set_entry([&](Fiber&) {
        w1_registered.store(true, std::memory_order_release);
        release_f0.set();   // signal that W1 is queued; F0 unlocks after newcomer tries
        mtx.lock(n1);  // suspends (owned by F0, then handoff to W1)
        // cancelled; do not unlock
    });
    Fiber w2;
    w2.set_entry([&](Fiber&) {
        sched.await_ready_flag(w1_registered);
        w2_registered.store(true, std::memory_order_release);
        mtx.lock(n2);  // suspends (owned by F0, then handoff to W1)
        w2_resumed.store(true, std::memory_order_release);
        mtx.unlock();
    });
    Fiber fcan;
    fcan.set_entry([&](Fiber&) {
        sched.await_ready_flag(w2_registered);
        for (int i = 0; i < 200 && !n1.is_terminal(); ++i) {
            if (mtx.cancel(n1)) { w1_cancelled.store(true, std::memory_order_release); break; }
            std::this_thread::yield();
        }
    });
    Fiber newcomer;
    newcomer.set_entry([&](Fiber&) {
        sched.await_ready_flag(w1_cancelled);
        newcomer_result.store(!mtx.try_lock(), std::memory_order_release);  // expect false
    });

    FiberStack s0, s1, s2, sc, sn;
    SLUICE_CHECK(sched.init_fiber(f0, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(w1, s1.base(), s1.size()));
    SLUICE_CHECK(sched.init_fiber(w2, s2.base(), s2.size()));
    SLUICE_CHECK(sched.init_fiber(fcan, sc.base(), sc.size()));
    SLUICE_CHECK(sched.init_fiber(newcomer, sn.base(), sn.size()));
    sched.spawn(f0);
    sched.spawn(w1);
    sched.spawn(w2);
    sched.spawn(fcan);
    sched.spawn(newcomer);
    sched.run(1);

    SLUICE_CHECK_MSG(w1_cancelled.load(), "W1 cancelled");
    SLUICE_CHECK_MSG(newcomer_result.load(), "newcomer try_lock failed (W2 still queued)");
    SLUICE_CHECK_MSG(w2_resumed.load(), "W2 received ownership after F0 unlock");
    SLUICE_CHECK_MSG(n1.was_cancelled(), "W1 resolved Cancelled");
    SLUICE_CHECK_MSG(n2.was_woken(), "W2 resolved Woken (handoff from F0)");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}
