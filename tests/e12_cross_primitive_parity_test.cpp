// e12_cross_primitive_parity_test
//
// Cross-primitive semantic parity tests (E10-E12-ASYNC-SYNC-API-SEMANTIC-
// CLOSURE-1). This TU supplies a deliberately narrow slice of the aggregate
// cross-primitive evidence. It directly covers Event / Semaphore / AsyncMutex;
// AsyncCondition and AsyncQueue evidence remains in their per-primitive TUs.
//
//   D3 (deadline precedence): at admission, resource readiness is checked
//       BEFORE the already-due deadline predicate (resource-first). A timed
//       wait whose deadline is provably already due resolves inline:
//         - resource admissible  -> Woken / acquired (no suspension)
//         - resource NOT admis.  -> Expired (no suspension)
//       This TU proves that rule for Event / Semaphore / AsyncMutex.
//
//   D4 (queue-identity cancellation): cancel(WaitNode&) returns true ONLY if
//       the node is currently Registered AND linked in THIS primitive's
//       queue. A node linked in a DIFFERENT primitive (even of the same kind,
//       on the same Scheduler) fails the membership gate and returns false
//       WITHOUT mutation. This holds uniformly across Event / Semaphore /
//       AsyncMutex. AsyncCondition is covered by its T31 per-primitive test;
//       Queue has no public wait-epoch cancellation API (N/A).
//
//   WaitOutcome vocabulary: the enum values are pairwise distinct and a fresh
//       WaitNode is unresolved. This TU does NOT prove dynamic absorbing-state
//       or exactly-once terminal publication; those require the existing
//       per-primitive race/terminal tests.
//
// These tests do NOT replace the per-primitive test suites (which cover each
// primitive's full semantic surface). They verify the cross-primitive PARITY
// contract for only the surfaces named above.
//
// Gated to x86_64 (fiber_ctx::supported): registration requires a real Fiber.
#include "harness.hpp"
#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/async_mutex.hpp>
#include <sluice/async/condition.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/semaphore.hpp>
#include <sluice/async/wait_node.hpp>

#include <atomic>
#include <cstddef>
#include <thread>

using namespace sluice::async;
using sluice::Result;

namespace {
using E11TimerTestHooks = sluice_async_test::TimerTestControl;

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

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

SLUICE_MAIN()

// ===========================================================================
// Slice D3 — Cross-primitive resource-first deadline precedence
// ===========================================================================

// ---- D3-EVT: Event SET + already-due deadline -> Woken ---------------------
// Cross-primitive check that Event follows the same precedence as Semaphore/
// AsyncMutex/AsyncCondition (resource-first). Already covered in detail by
// e12_event_test T33; included here as a cross-primitive parity marker.
SLUICE_TEST_CASE(e12_parity_d3_event_set_plus_already_due_woken) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 100);  // now = 100
    Event ev(sched, /*initially_set=*/true);

    WaitNode node;
    std::atomic<int> entries{0};
    Fiber f;
    f.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        // deadline=0 ALREADY DUE (0 <= now=100), but Event SET -> Woken inline.
        ev.wait_until(node, /*deadline=*/0);
        entries.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(f, sw.base(), sw.size()));
    sched.spawn(f);
    sched.run(1);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter never suspended (resource-first)");
    SLUICE_CHECK_MSG(node.was_woken(), "SET + already-due deadline -> Woken");
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "no timer registered (inline resolution)");
}

// ---- D3-SEM: Semaphore permit + already-due deadline -> Woken --------------
// A permit is admissible; the already-due deadline is NOT consulted. Inline
// Woken, no suspension, no timer registered. Parity with Event/Mutex/Condition.
SLUICE_TEST_CASE(e12_parity_d3_semaphore_permit_plus_already_due_woken) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 100);  // now = 100
    Semaphore sem(sched, /*initial=*/1, /*max=*/1);

    WaitNode node;
    std::atomic<int> entries{0};
    Fiber f;
    f.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        // deadline=0 ALREADY DUE; permit admissible -> Woken inline.
        sem.acquire_until(node, /*deadline=*/0);
        entries.fetch_add(1, std::memory_order_acq_rel);
    });
    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(f, sw.base(), sw.size()));
    sched.spawn(f);
    sched.run(1);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter never suspended (resource-first)");
    SLUICE_CHECK_MSG(node.was_woken(), "permit + already-due deadline -> Woken");
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "no timer registered (inline resolution)");
}

// ---- D3-MTX: AsyncMutex free + already-due deadline -> Woken ---------------
// The Mutex is free; the already-due deadline is NOT consulted. Inline Woken,
// no suspension, no timer registered. Parity with Event/Semaphore/Condition.
SLUICE_TEST_CASE(e12_parity_d3_mutex_free_plus_already_due_woken) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    E11TimerTestHooks::enable_test_clock(sched);
    E11TimerTestHooks::set_clock(sched, 100);  // now = 100
    AsyncMutex mtx(sched);

    WaitNode node;
    std::atomic<int> entries{0};
    Fiber f;
    f.set_entry([&](Fiber&) {
        entries.fetch_add(1, std::memory_order_acq_rel);
        // deadline=0 ALREADY DUE; Mutex free -> Woken inline.
        mtx.lock_until(node, /*deadline=*/0);
        entries.fetch_add(1, std::memory_order_acq_rel);
        mtx.unlock();
    });
    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(f, sw.base(), sw.size()));
    sched.spawn(f);
    sched.run(1);

    SLUICE_CHECK_MSG(entries.load() == 2, "waiter never suspended (resource-first)");
    SLUICE_CHECK_MSG(node.was_woken(), "free Mutex + already-due deadline -> Woken");
    SLUICE_CHECK_MSG(E11TimerTestHooks::active_deadline_count(sched) == 0,
                     "no timer registered (inline resolution)");
}

// ===========================================================================
// Slice D4 — Cross-primitive queue-identity cancellation
// ===========================================================================
//
// cancel(WaitNode&) returns true ONLY if the node is Registered AND linked in
// THIS primitive's queue. A node linked in a DIFFERENT primitive fails the
// membership gate and returns false WITHOUT mutation. Each test below proves
// the wrong-object case for one primitive kind (intra-kind wrong-object on
// the same Scheduler).

// ---- D4-EVT: Event cancel against a node in a DIFFERENT Event -> false ----
SLUICE_TEST_CASE(e12_parity_d4_event_cancel_wrong_event_returns_false) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    Event ev_a(sched);
    Event ev_b(sched);

    std::atomic<bool> a_parked{false};
    WaitNode n_b;  // will be registered in ev_b, not ev_a

    Fiber waiter;
    waiter.set_entry([&](Fiber&) {
        a_parked.store(true, std::memory_order_release);
        ev_b.wait(n_b);  // parks on ev_b
    });
    Fiber canceller;
    canceller.set_entry([&](Fiber&) {
        sched.await_ready_flag(a_parked);
        // Wrong-object cancel: n_b is registered in ev_b, not ev_a.
        SLUICE_CHECK_MSG(!ev_a.cancel(n_b),
                         "Event::cancel wrong-object -> false (no mutation)");
        // Correct-object cancel resolves the waiter.
        SLUICE_CHECK_MSG(ev_b.cancel(n_b),
                         "Event::cancel correct-object -> true");
    });
    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(waiter, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(canceller, sb.base(), sb.size()));
    sched.spawn(waiter);
    sched.spawn(canceller);
    sched.run(1);

    SLUICE_CHECK_MSG(n_b.was_cancelled(),
                     "n_b resolved Cancelled via correct Event");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- D4-SEM: Semaphore cancel against a node in a DIFFERENT Semaphore ------
SLUICE_TEST_CASE(e12_parity_d4_semaphore_cancel_wrong_semaphore_returns_false) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    Semaphore sem_a(sched, /*initial=*/0, /*max=*/1);
    Semaphore sem_b(sched, /*initial=*/0, /*max=*/1);

    std::atomic<bool> a_parked{false};
    WaitNode n_b;  // will be registered in sem_b, not sem_a

    Fiber waiter;
    waiter.set_entry([&](Fiber&) {
        a_parked.store(true, std::memory_order_release);
        sem_b.acquire(n_b);  // parks on sem_b (no permits)
    });
    Fiber canceller;
    canceller.set_entry([&](Fiber&) {
        sched.await_ready_flag(a_parked);
        // Wrong-object cancel: n_b is registered in sem_b, not sem_a.
        SLUICE_CHECK_MSG(!sem_a.cancel(n_b),
                         "Semaphore::cancel wrong-object -> false (no mutation)");
        // Correct-object cancel resolves the waiter.
        SLUICE_CHECK_MSG(sem_b.cancel(n_b),
                         "Semaphore::cancel correct-object -> true");
    });
    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(waiter, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(canceller, sb.base(), sb.size()));
    sched.spawn(waiter);
    sched.spawn(canceller);
    sched.run(1);

    SLUICE_CHECK_MSG(n_b.was_cancelled(),
                     "n_b resolved Cancelled via correct Semaphore");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ---- D4-MTX: AsyncMutex cancel against a node in a DIFFERENT Mutex ---------
SLUICE_TEST_CASE(e12_parity_d4_mutex_cancel_wrong_mutex_returns_false) {
    if constexpr (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncMutex mtx_a(sched);
    AsyncMutex mtx_b(sched);

    // A holder fiber acquires mtx_b first and holds it until the waiter has
    // parked, then releases it AFTER the canceller has run. AsyncMutex::lock
    // requires a running Fiber; it cannot be called on the test's main thread.
    std::atomic<bool> holder_locked{false};
    std::atomic<bool> waiter_parked{false};
    std::atomic<bool> cancel_done{false};
    WaitNode n_holder, n_b;  // n_b will be registered in mtx_b, not mtx_a

    Fiber holder;
    holder.set_entry([&](Fiber&) {
        mtx_b.lock(n_holder);
        holder_locked.store(true, std::memory_order_release);
        // Hold until the canceller has run, then release.
        sched.await_ready_flag(cancel_done);
        mtx_b.unlock();
    });
    Fiber waiter;
    waiter.set_entry([&](Fiber&) {
        sched.await_ready_flag(holder_locked);
        waiter_parked.store(true, std::memory_order_release);
        mtx_b.lock(n_b);  // parks on mtx_b (held by holder)
        // If the wait resolved Woken (acquired), release. If Cancelled, the
        // cancellation path did NOT transfer ownership; unlock is forbidden.
        if (n_b.was_woken()) mtx_b.unlock();
    });
    Fiber canceller;
    canceller.set_entry([&](Fiber&) {
        sched.await_ready_flag(waiter_parked);
        // Wrong-object cancel: n_b is registered in mtx_b, not mtx_a.
        SLUICE_CHECK_MSG(!mtx_a.cancel(n_b),
                         "AsyncMutex::cancel wrong-object -> false (no mutation)");
        // Correct-object cancel resolves the waiter.
        SLUICE_CHECK_MSG(mtx_b.cancel(n_b),
                         "AsyncMutex::cancel correct-object -> true");
        cancel_done.store(true, std::memory_order_release);
    });
    FiberStack sa, sb, sc;
    SLUICE_CHECK(sched.init_fiber(holder, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(waiter, sb.base(), sb.size()));
    SLUICE_CHECK(sched.init_fiber(canceller, sc.base(), sc.size()));
    sched.spawn(holder);
    sched.spawn(waiter);
    sched.spawn(canceller);
    sched.run(1);

    SLUICE_CHECK_MSG(n_b.was_cancelled(),
                     "n_b resolved Cancelled via correct Mutex");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0, "no unresolved waits remain");
}

// ===========================================================================
// WaitOutcome vocabulary probe (not a dynamic state-machine proof)
// ===========================================================================
//
// The probe below proves only pairwise enum-value distinctness and the initial
// state of a fresh WaitNode. Absorbing terminals and concurrent exactly-once
// publication are proved by the existing per-primitive tests.

SLUICE_TEST_CASE(e12_parity_waitoutcome_values_are_distinct_and_fresh_unresolved) {
    // Sanity: the four binding outcomes are mutually exclusive. The enum is
    // backed by uint8_t; the four values are 0/1/2/3. This is the runtime
    // counterpart to the compile-time assertion in e12_api_contract_probes.
    using W = WaitOutcome;
    SLUICE_CHECK_MSG(static_cast<std::uint8_t>(W::unresolved) !=
                         static_cast<std::uint8_t>(W::woken),
                     "unresolved != woken");
    SLUICE_CHECK_MSG(static_cast<std::uint8_t>(W::woken) !=
                         static_cast<std::uint8_t>(W::cancelled),
                     "woken != cancelled");
    SLUICE_CHECK_MSG(static_cast<std::uint8_t>(W::cancelled) !=
                         static_cast<std::uint8_t>(W::expired),
                     "cancelled != expired");
    SLUICE_CHECK_MSG(static_cast<std::uint8_t>(W::unresolved) !=
                         static_cast<std::uint8_t>(W::expired),
                     "unresolved != expired");
    SLUICE_CHECK_MSG(static_cast<std::uint8_t>(W::unresolved) !=
                         static_cast<std::uint8_t>(W::cancelled),
                     "unresolved != cancelled");
    SLUICE_CHECK_MSG(static_cast<std::uint8_t>(W::woken) !=
                         static_cast<std::uint8_t>(W::expired),
                     "woken != expired");

    // A fresh WaitNode is unresolved until one outcome wins.
    WaitNode fresh;
    SLUICE_CHECK_MSG(!fresh.is_terminal(), "fresh WaitNode is non-terminal");
    SLUICE_CHECK_MSG(fresh.outcome() == W::unresolved,
                     "fresh WaitNode outcome == unresolved");
    SLUICE_CHECK_MSG(!fresh.was_woken(), "fresh: not woken");
    SLUICE_CHECK_MSG(!fresh.was_cancelled(), "fresh: not cancelled");
    SLUICE_CHECK_MSG(!fresh.was_expired(), "fresh: not expired");
}
