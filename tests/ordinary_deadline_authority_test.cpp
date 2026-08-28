// ordinary_deadline_authority_test — AC-2b executable invariant witness.
//
// Proves the uniform ordinary TimerRegistration lifecycle facts that the
// Scheduler deadline authority (arm / consume / retire, AC-2b) now owns,
// through the PUBLIC admission seams + the deterministic test clock — NOT
// through implementation-private line ordering:
//
//   arm:     active_deadline_count_ == +1 per registration
//   consume: ACTIVE -> CONSUMED decrements the count exactly once (pump)
//   retire:  ACTIVE -> RETIRED decrements the count exactly once (non-timer
//            winner)
//   stale:   repeated/losing terminal attempts never decrement again; a
//            physically retained far-future RETIRED entry stays inert and is
//            reclaimed only by its OWN deadline pop without re-activation
//   mixed:   retire + consume across two epochs keeps the count exact and the
//            earliest-deadline cache correct after the pump's drain horizon
//
// Every race proof uses the controllable monotonic clock + explicit timer
// driver (advance_clock) or queue resolution — NEVER sleep_for timing as
// causal proof (M7). Gated to x86_64 (fiber_ctx::supported): registration
// requires a real Fiber.
#include "harness.hpp"
#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <atomic>
#include <new>

using namespace sluice::async;
using sluice::Result;

namespace {
using TimerCtl = sluice_async_test::TimerTestControl;

// A backend that never completes anything (outstanding stays 0). The only
// progress in these tests is Scheduler-integrated resolution; run() returns
// once all fibers are done (Drain).
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

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

[[maybe_unused]] inline void spin_wait(std::atomic<bool>& flag) {
    while (!flag.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}
}  // namespace

// =============================================================================
// W1 (consume-exactly-once): arm -> count +1; the pump consumes the ACTIVE
// registration when its deadline passes; the count returns to exactly 0; the
// consumed entry is reclaimed by its own deadline pop. The waiter resolved
// Expired, proving the consume preceded node dereference (timer-lifetime).
// =============================================================================
SLUICE_TEST_CASE(od_w1_arm_consume_exact_once) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    TimerCtl::enable_test_clock(sched);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> registered{false};
    std::atomic<std::size_t> seen_active{0};
    std::atomic<bool> ran_after{false};

    Fiber fwait, fdriver;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sched.await_wait_deadline(q, n, Scheduler::deadline_t{50});
        ran_after.store(true, std::memory_order_release);
    });
    // After the waiter registers (ACTIVE count == 1), advance past its
    // deadline: the pump consumes it under global_mtx_.
    fdriver.set_entry([&](Fiber&) {
        spin_wait(registered);
        seen_active.store(TimerCtl::active_deadline_count(sched),
                          std::memory_order_release);
        sched.advance_clock(100);  // 100 >= 50 -> due
    });

    FiberStack sw, sd;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fdriver, sd.base(), sd.size()));
    sched.spawn(fwait);
    sched.spawn(fdriver);
    sched.run(1);

    SLUICE_CHECK_MSG(seen_active.load() == 1,
                     "arm: exactly one ACTIVE deadline (+1)");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "consume: ACTIVE->CONSUMED decremented exactly once");
    SLUICE_CHECK_MSG(TimerCtl::timer_pool_count_in_state(
                         sched, TimerRegistration::State::active) == 0,
                     "no physically retained block remains logically ACTIVE");
    SLUICE_CHECK_MSG(TimerCtl::timer_pool_size(sched) == 0,
                     "consumed entry reclaimed by its own deadline pop");
    SLUICE_CHECK_MSG(n.was_expired(), "waiter resolved Expired");
}

// =============================================================================
// W2 (retire-exactly-once + stale inertness): cancel wins -> ACTIVE->RETIRED
// decrements exactly once; the retained far-future RETIRED entry survives many
// pump passes WITHOUT decrementing again and WITHOUT becoming ACTIVE; then a
// clock jump past ITS deadline lazy-pops it (physical reclamation), still with
// zero further decrement and no second publication on the destroyed epoch's
// account (the node here stays alive so the epoch is well-defined; the
// RETIRED gate, not node liveness, is what blocks every stale expiry).
// =============================================================================
SLUICE_TEST_CASE(od_w2_retire_exact_once_stale_inert) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    TimerCtl::enable_test_clock(sched);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> registered{false};
    std::atomic<bool> ran_after{false};
    std::atomic<bool> cancelled_observed{false};

    // Waiter: far-future deadline (90000), never due during registration.
    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        registered.store(true, std::memory_order_release);
        sched.await_wait_deadline(q, n, Scheduler::deadline_t{90000});
        ran_after.store(true, std::memory_order_release);
        cancelled_observed.store(n.was_cancelled(), std::memory_order_release);
    });
    // Canceller: the non-timer winner. Retires the ACTIVE registration in the
    // SAME CS as its resolve_ CAS (ACTIVE->RETIRED decrements exactly once).
    Fiber fcancel;
    fcancel.set_entry([&](Fiber&) {
        spin_wait(registered);
        std::this_thread::yield();  // let the waiter reach await_wait_deadline
        SLUICE_CHECK_MSG(sched.cancel_wait(q, n),
                         "cancel wins the resolve_ CAS");
    });

    FiberStack sw, sc;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fwait);
    sched.spawn(fcancel);
    sched.run(1);
    SLUICE_CHECK_MSG(ran_after.load(), "waiter resumed after cancellation");
    SLUICE_CHECK_MSG(cancelled_observed.load(), "waiter resolved Cancelled");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "retire decremented exactly once");

    // Stale pass 1..k over the RETAINED far-future RETIRED entry (deadline
    // 90000): each pump observes non-ACTIVE, skips WITHOUT touching the count
    // or the node. Pool size unchanged proves no reclamation before ITS OWN
    // deadline (lazy-at-deadline reclamation law).
    for (int i = 0; i < 5; ++i) {
        sched.advance_clock(1000 * (i + 1));  // all below 90000: nothing due
        SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                         "stale pump never double-decrements");
        SLUICE_CHECK_MSG(
            TimerCtl::timer_pool_count_in_state(sched,
                                                TimerRegistration::State::retired) == 1,
            "RETIRED block physically retained, inertly, pre-deadline");
    }

    // Clock jumps PAST the retained entry's own deadline: the pump pops it,
    // erases the pool block (physical reclamation), and STILL decrements
    // nothing (the retire already did, exactly once).
    sched.advance_clock(90001);
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "stale expiry after RETIRED decrements nothing");
    SLUICE_CHECK_MSG(TimerCtl::timer_pool_size(sched) == 0,
                     "retained block reclaimed by its own lazy pop");
    SLUICE_CHECK_MSG(TimerCtl::timer_pool_count_in_state(
                         sched, TimerRegistration::State::active) == 0,
                     "a physical stale heap entry never becomes logically ACTIVE");
}

// =============================================================================
// W3 (mixed causes, exact accounting + cache horizon): two registrations on
// one scheduler via the narrow external-registration hook (non-worker thread,
// T17 pattern): one retired by cancel (non-timer winner), one consumed by the
// pump at its earlier deadline. Final count exact; after the pump's amortized
// single cache refresh, the earliest-active cache reports no obligation.
// =============================================================================
SLUICE_TEST_CASE(od_w3_mixed_causes_exact_accounting_and_cache) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard ctrl(sched);
    TimerCtl::enable_test_clock(sched);
    TimerCtl::set_clock(sched, 100);

    WaitQueue qa, qb;
    WaitNode na, nb;

    // Both epochs armed externally (coordinator thread, before any worker
    // exists — register_test_deadline takes global_mtx_ itself and the drain
    // pump + cancel path resolve them later under run()).
    // A: deadline 400 (to be retired by a cancel winner inside run()).
    // B: deadline 300 (to be consumed by the pump).
    TimerRegistration* reg_a = TimerCtl::register_test_deadline(
        sched, &na, &qa, Scheduler::deadline_t{400});
    TimerRegistration* reg_b = TimerCtl::register_test_deadline(
        sched, &nb, &qb, Scheduler::deadline_t{300});
    SLUICE_CHECK_MSG(reg_a != nullptr && reg_b != nullptr, "both armed");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 2,
                     "two arms: +1 each");

    Scheduler::deadline_t out = 0;
    SLUICE_CHECK_MSG(TimerCtl::earliest_active_deadline(sched, out) && out == 300,
                     "cache reports B(300) as earliest active");

    // Inside run(): the canceller retires A (non-timer winner). The clock is
    // still 100 < B's 300, so the parked worker's per-iteration pumps stay
    // below both deadlines until advance_clock — B's consumption therefore
    // comes from an explicit driver-driven pump (the amortized one-pass cache
    // refresh horizon), not from the admission closure.
    Fiber fcancel;
    fcancel.set_entry([&](Fiber&) {
        // NOTE: the externally-registered epoch has fiber()==nullptr, so
        // cancel_wait wins the resolve_ CAS (and retires the timer) but
        // returns false — no runnable can be routed for a fiber-less epoch.
        // The winner proof is the terminal state, not the bool.
        (void)sched.cancel_wait(qa, na);
        SLUICE_CHECK_MSG(na.was_cancelled(), "cancel retires A");
    });

    FiberStack sc;
    SLUICE_CHECK(sched.init_fiber(fcancel, sc.base(), sc.size()));
    sched.spawn(fcancel);
    sched.run(1);

    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 1,
                     "retire across mixed causes decremented exactly once");
    // Immediate cache refresh on the retire path (pre-existing retire-side
    // horizon): earliest active is now B(300).
    SLUICE_CHECK_MSG(TimerCtl::earliest_active_deadline(sched, out) && out == 300,
                     "earliest still B after retiring A");

    // Pump past B's deadline (300) but NOT A's (400; A is retired anyway):
    // B is consumed -1; the stale RETIRED A entry is NOT yet popped (400 >
    // current clock) and decrements nothing.
    sched.advance_clock(350);
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "consume of B decremented the last unit exactly once");
    SLUICE_CHECK_MSG(nb.was_expired(), "B resolved Expired by the pump");
    SLUICE_CHECK_MSG(!TimerCtl::earliest_active_deadline(sched, out),
                     "after the pump's single amortized cache refresh: no "
                     "remaining ACTIVE obligation");

    // Physical state: B's block was popped+erased by its own deadline pop;
    // A's retired block remains held lazily until ITS deadline passes.
    SLUICE_CHECK_MSG(TimerCtl::timer_pool_size(sched) == 1,
                     "only the stale A block remains physically retained");
    // Jump past A's deadline too: pure physical cleanup, zero logical change.
    sched.advance_clock(500);
    SLUICE_CHECK_MSG(TimerCtl::timer_pool_size(sched) == 0,
                     "A reclaimed by its own deadline pop");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "count untouched by pure reclamation");
}

// =============================================================================
// R2-ALLOC (allocation-atomic timed admission). The admission's allocations
// (deadline-heap slot reserve + pool node) run in
// prepare_ordinary_deadline_locked BEFORE register_wait_locked, so a
// std::bad_alloc injected at prepare entry (one-shot internal-testing seam)
// must leave the admission fully unmutated — node Detached, wait and timer
// accounting zero, no pool/heap orphan, no earliest-deadline cache
// publication — and the SAME node + Scheduler must admit a healthy timed wait
// immediately after. These cases regress if any allocation is reintroduced
// after registration on any ordinary arming path.
// =============================================================================

// A1 (generic admission): await_wait_deadline throws cleanly, leaves zero
// residue, and the same node re-admits through the full healthy
// prepare -> register -> publish -> already-due consume path.
SLUICE_TEST_CASE(od_alloc_a1_generic_admission_atomic) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    TimerCtl::enable_test_clock(sched);
    sluice_async_test::ControllerGuard cg(sched);
    TimerCtl::arm_alloc_failure(sched);

    WaitQueue q;
    WaitNode n;
    std::atomic<bool> failed{false};
    std::atomic<bool> node_clean{false};
    std::atomic<bool> healthy_after{false};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        try {
            sched.await_wait_deadline(q, n, Scheduler::deadline_t{50});
        } catch (const std::bad_alloc&) {
            failed.store(true, std::memory_order_release);
        }
        // Zero-residue witness AT THE FAILURE SITE (before any re-admission
        // legitimately moves the node): the failed admission must have left
        // the node Detached and every authority untouched.
        node_clean.store(!n.is_registered() && !n.is_terminal(),
                         std::memory_order_release);
        // Healthy re-admission on the SAME node + Scheduler: an already-due
        // deadline resolves Expired inline through the full admission path
        // (prepare -> register -> publish -> consume), no driver needed.
        sched.await_wait_deadline(q, n, Scheduler::deadline_t{0});
        healthy_after.store(n.was_expired(), std::memory_order_release);
        // The consumed block is lazily reclaimed by its own (already-due)
        // deadline: advance past it so the pool drains deterministically.
        sched.advance_clock(1);
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);
    sched.run(1);

    SLUICE_CHECK_MSG(failed.load(),
                     "injected bad_alloc escaped the timed admission");
    SLUICE_CHECK_MSG(node_clean.load(),
                     "alloc failure left the node Detached (no dead epoch)");
    SLUICE_CHECK_MSG(healthy_after.load(),
                     "node + Scheduler re-admitted a healthy timed wait");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0,
                     "alloc failure left no wait-accounting residue");
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "alloc failure left no timer-accounting residue");
    SLUICE_CHECK_MSG(TimerCtl::timer_pool_size(sched) == 0 &&
                         TimerCtl::deadline_heap_size(sched) == 0,
                     "alloc failure orphaned no pool/heap block");
    Scheduler::deadline_t earliest = 0;
    SLUICE_CHECK_MSG(!TimerCtl::earliest_active_deadline(sched, earliest),
                     "alloc failure published no earliest-deadline obligation");
}

// A2 (public primitive admission): the same invariant through the Event
// public API (Event::wait_until) — the failure is catchable at the caller and
// the primitive is immediately reusable.
SLUICE_TEST_CASE(od_alloc_a2_event_admission_atomic) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    TimerCtl::enable_test_clock(sched);
    sluice_async_test::ControllerGuard cg(sched);
    TimerCtl::arm_alloc_failure(sched);

    Event ev(sched);
    WaitNode n;
    std::atomic<bool> failed{false};
    std::atomic<bool> node_clean{false};
    std::atomic<bool> healthy_after{false};

    Fiber fwait;
    fwait.set_entry([&](Fiber&) {
        try {
            ev.wait_until(n, Scheduler::deadline_t{50});
        } catch (const std::bad_alloc&) {
            failed.store(true, std::memory_order_release);
        }
        node_clean.store(!n.is_registered() && !n.is_terminal(),
                         std::memory_order_release);
        // Unset Event + already-due deadline: the healthy admission resolves
        // Expired inline through the full prepare -> publish -> consume path.
        ev.wait_until(n, Scheduler::deadline_t{0});
        healthy_after.store(n.was_expired(), std::memory_order_release);
        sched.advance_clock(1);
    });

    FiberStack sw;
    SLUICE_CHECK(sched.init_fiber(fwait, sw.base(), sw.size()));
    sched.spawn(fwait);
    sched.run(1);

    SLUICE_CHECK_MSG(failed.load(),
                     "injected bad_alloc escaped Event::wait_until");
    SLUICE_CHECK_MSG(node_clean.load(),
                     "alloc failure left the Event node Detached");
    SLUICE_CHECK_MSG(healthy_after.load(),
                     "Event re-admitted a healthy timed wait after the failure");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0 &&
                         TimerCtl::active_deadline_count(sched) == 0,
                     "no accounting residue after the failed Event admission");
    SLUICE_CHECK_MSG(TimerCtl::timer_pool_size(sched) == 0 &&
                         TimerCtl::deadline_heap_size(sched) == 0,
                     "no pool/heap orphan after the failed Event admission");
}

// =============================================================================
// A3 (heap growth curve): the prepare-phase heap reserve must preserve
// vector's geometric growth — forcing the growth allocation only when the
// heap is exactly full — so reaching N concurrent deadlines costs O(log N)
// reallocations, not the O(N) reallocations / O(N^2) element moves of an
// unconditional size+1 reserve. Drives the REAL production prepare path K
// times via the coordinator registration seam, sampling capacity after every
// arm; then drains through the pump to prove the grown heap still pops
// correctly.
// =============================================================================
SLUICE_TEST_CASE(od_alloc_a3_heap_growth_stays_geometric) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    TimerCtl::enable_test_clock(sched);
    sluice_async_test::ControllerGuard cg(sched);
    TimerCtl::set_clock(sched, 100);

    constexpr std::size_t kArms = 64;
    WaitNode nodes[kArms];
    WaitQueue queues[kArms];

    std::size_t prev_cap = TimerCtl::deadline_heap_capacity(sched);
    std::size_t growths = 0;
    for (std::size_t i = 0; i < kArms; ++i) {
        TimerRegistration* reg = TimerCtl::register_test_deadline(
            sched, &nodes[i], &queues[i], Scheduler::deadline_t{1000});
        SLUICE_CHECK_MSG(reg != nullptr,
                         "arm went through the production prepare path");
        SLUICE_CHECK_MSG(TimerCtl::deadline_heap_size(sched) == i + 1,
                         "each arm pushed exactly one heap entry");
        const std::size_t cap = TimerCtl::deadline_heap_capacity(sched);
        SLUICE_CHECK_MSG(cap >= i + 1,
                         "capacity always covers size (publish push stays "
                         "within prepare's reservation)");
        if (cap != prev_cap) {
            SLUICE_CHECK_MSG(cap > prev_cap, "capacity never shrinks");
            ++growths;
            prev_cap = cap;
        }
    }
    // Drain FIRST (before the growth-curve verdict): a failing growth
    // assertion must not abandon registered waiters into the WaitQueue
    // destructors — the drain itself is part of the evidence.
    sched.advance_clock(1001);
    SLUICE_CHECK_MSG(TimerCtl::active_deadline_count(sched) == 0,
                     "all 64 deadlines consumed exactly once");
    SLUICE_CHECK_MSG(sched.waiting_count() == 0,
                     "pump wins balanced every coordinator wait increment");
    SLUICE_CHECK_MSG(TimerCtl::timer_pool_size(sched) == 0 &&
                         TimerCtl::deadline_heap_size(sched) == 0,
                     "grown heap drained to empty with full reclamation");

    // Doubling from empty reaches 64 within 7 steps (1,2,4,...,64); allow one
    // step of slack. The rejected one-slot reserve walks capacity up 63 times
    // across these same 64 arms.
    SLUICE_CHECK_MSG(growths <= 8,
                     "heap growth stayed geometric, not one-slot-per-admission");
}

SLUICE_MAIN()
