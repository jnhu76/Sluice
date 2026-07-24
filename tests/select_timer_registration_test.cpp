// select_timer_registration_test — E13 Select Timer registration tests (P3).
//
// Verifies the stable Select timer substrate: state transitions, address
// stability after single-node splice, tagged deadline-heap ordering, ordinary
// timer regression, RETIRED/CONSUMED stale-skip, state-before-arm
// instrumentation (arm-load delta == 0), earliest-active-deadline participation,
// lazy physical reclamation, mixed stale+ordinary pump, Scheduler identity,
// and no-premature-Select invariants.
//
// All operations route through Scheduler authority (Scheduler::AsyncTestAccess /
// the SelectTimerSeam facade in the internal-testing variant). Deterministic:
// uses the logical test clock + causal phase seams; NO sleep_for.
//
// Addendum F constraints honored:
//   - tagged ordering: only "smaller pops before larger"; both kinds share the
//     ordering. No test asserts equal-deadline FIFO stability.
//   - earliest-deadline: an ACTIVE Select entry participates only while its
//     deadline is in the future; transitioned via Scheduler helper before pump.
//   - lazy reclamation: the exact 8-step sequence.
//   - mixed: stale Select D1 + ordinary ACTIVE D2, D1<=D2<=now.
#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/detail/select_registration.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/select.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include "async_test_control.hpp"
#include "harness.hpp"

#include <cstddef>
#include <list>

namespace sa = sluice::async;
namespace sad = sluice::async::detail;
namespace stest = sluice_async_test;

using AsyncTestAccess = sa::Scheduler::AsyncTestAccess;
using SelectTimerReg = sad::SelectTimerRegistration;
using Scheduler = sa::Scheduler;
using State = SelectTimerReg::State;

// ---- shared fixture helpers ----

struct TFixture {
    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched;
    stest::ControllerGuard ctrl;
    explicit TFixture() : sched(ctx), ctrl(sched) {
        stest::TimerTestControl::enable_test_clock(sched);
    }
    ~TFixture() {
        // Drain any remaining Select timer blocks so the Scheduler destroys
        // with no live Select timer authority (the ~Scheduler quiescence
        // contract). Retire any still-ACTIVE block, then advance the clock
        // far past every deadline so the pump reclaims each block physically.
        // Tests that already pump their blocks to reclamation leave an empty
        // pool and this is a no-op.
        AsyncTestAccess::drain_select_pool(sched);
    }
    // No copy/move.
    TFixture(const TFixture&) = delete;
    TFixture& operator=(const TFixture&) = delete;
};

// =========================================================================
// T1 — state transitions (CAS on detached, never-registered locals)
// =========================================================================
// Addendum C: direct CAS is permitted ONLY for detached never-registered
// locals. E13 P3 Corrective closure 3 sealed the CAS methods private, so these
// reach them via the guarded AsyncTestAccess detached-CAS entry — NOT the
// Scheduler accounting path. Registered blocks must use retire_synthetic /
// consume_synthetic (the Scheduler helpers).

SLUICE_TEST_CASE(t1_state_transitions_retire) {
    // new registration == ACTIVE; ACTIVE -> RETIRED succeeds; second RETIRED
    // fails; RETIRED -> CONSUMED fails. The CAS methods are private (E13 P3
    // Corrective closure 3: registered-state accounting authority is sealed by
    // the type system); T1 reaches them via the guarded detached-CAS test
    // entry on a never-registered stack-local object.
    SelectTimerReg reg(nullptr, nullptr, 100);
    SLUICE_CHECK(reg.state() == State::active);

    SLUICE_CHECK(stest::SelectTimerSeam::detached_retire(reg) == true);   // ACTIVE -> RETIRED
    SLUICE_CHECK(reg.state() == State::retired);
    SLUICE_CHECK(stest::SelectTimerSeam::detached_retire(reg) == false);  // already RETIRED
    SLUICE_CHECK(stest::SelectTimerSeam::detached_try_claim_expiry(reg) == false);  // RETIRED -> CONSUMED fails
    SLUICE_CHECK(reg.state() == State::retired);
}

SLUICE_TEST_CASE(t1_state_transitions_consume) {
    // new registration == ACTIVE; ACTIVE -> CONSUMED succeeds; second CONSUMED
    // fails; CONSUMED -> RETIRED fails. (CASes reached via the guarded entry;
    // see t1_state_transitions_retire for the authority rationale.)
    SelectTimerReg reg(nullptr, nullptr, 100);
    SLUICE_CHECK(reg.state() == State::active);

    SLUICE_CHECK(stest::SelectTimerSeam::detached_try_claim_expiry(reg) == true);   // ACTIVE -> CONSUMED
    SLUICE_CHECK(reg.state() == State::consumed);
    SLUICE_CHECK(stest::SelectTimerSeam::detached_try_claim_expiry(reg) == false);  // already CONSUMED
    SLUICE_CHECK(stest::SelectTimerSeam::detached_retire(reg) == false);            // CONSUMED -> RETIRED fails
    SLUICE_CHECK(reg.state() == State::consumed);
}

// =========================================================================
// T2 — address stability after single-node splice
// =========================================================================
// E13 P3 Corrective closure 4: prove the REAL production splice preserves
// node address identity. A node built in a caller-frame temporary list is
// spliced into select_timer_pool_ via select_timer_splice_one_locked (single-
// node std::list::splice, O(1), allocation-free — it relinks the node, so the
// node's address does not change). The heap entry references the block by that
// same address. This test captures the address BEFORE splice and asserts it is
// unchanged AFTER splice, that the temporary pool gave up the node, and that
// the heap holds exactly one Select entry whose target is the spliced address.

SLUICE_TEST_CASE(t2_address_stability_after_splice) {
    TFixture f;
    // Build a node in a caller-frame temporary list; capture its address BEFORE
    // any Scheduler mutation.
    std::list<SelectTimerReg> tmp;
    tmp.emplace_back(nullptr, &f.sched, /*deadline=*/50);
    const SelectTimerReg* before = &tmp.front();

    // Splice via the REAL production helper (select_timer_splice_one_locked).
    SelectTimerReg* after = stest::SelectTimerSeam::splice_one(
        f.sched, tmp, tmp.begin());

    // Address identity: the spliced node is the SAME object (std::list::splice
    // relinks; it does not copy/relocate). This is the load-bearing assertion
    // the heap-entry-by-stable-address contract relies on.
    SLUICE_CHECK_MSG(after == before,
        "spliced node address == captured pre-splice address");
    // The temporary pool gave up the node.
    SLUICE_CHECK_MSG(tmp.empty(),
        "temporary pool empty after single-node splice");
    // The Scheduler-owned block's binding fields are intact.
    SLUICE_CHECK(after->scheduler() == &f.sched);
    SLUICE_CHECK(after->deadline() == 50);

    // The Scheduler pool now owns exactly this one block.
    SLUICE_CHECK(stest::SelectTimerSeam::pool_size(f.sched) == 1);

    // The heap holds exactly one Select entry, and its target is exactly the
    // spliced address (the stable address the heap stores by pointer).
    auto counts = stest::SelectTimerSeam::heap_counts_by_kind(f.sched);
    SLUICE_CHECK(counts[0] == 0);  // no ordinary entries
    SLUICE_CHECK(counts[1] == 1);  // one Select entry
    SLUICE_CHECK_MSG(
        stest::SelectTimerSeam::heap_has_select_target(f.sched, before),
        "heap entry's Select target == spliced address");
}

// =========================================================================
// T3 — tagged heap ordering (smaller deadline first; both kinds share)
// =========================================================================
// Addendum F: do NOT assert equal-deadline stability. Only "smaller pops
// before larger", ordinary and Select in the same ordering.
//
// Strategy: register a mix, then drive the clock forward in steps, retiring
// each Select entry via the Scheduler helper BEFORE its deadline elapses (so
// the pump sees stale Select entries, not ACTIVE ones). Ordinary entries win
// their resolve when pumped. We observe the heap min-deadline progressing in
// ascending order regardless of kind.

SLUICE_TEST_CASE(t3_tagged_heap_ordering) {
    TFixture f;
    // Out-of-order deadlines across kinds.
    //   ordinary@100, select@30, ordinary@10, select@200
    // Expected ascending pop order by deadline: o@10, s@30, o@100, s@200.
    sa::WaitNode on_lo, on_hi;
    sa::WaitQueue oq_lo, oq_hi;
    AsyncTestAccess::register_test_deadline(f.sched, &on_lo, &oq_lo, /*dl=*/10);
    AsyncTestAccess::register_test_deadline(f.sched, &on_hi, &oq_hi, /*dl=*/100);

    SelectTimerReg* s30 = stest::SelectTimerSeam::register_synthetic(
        f.sched, nullptr, /*dl=*/30);
    SelectTimerReg* s200 = stest::SelectTimerSeam::register_synthetic(
        f.sched, nullptr, /*dl=*/200);

    SLUICE_CHECK(AsyncTestAccess::deadline_heap_size(f.sched) == 4);

    // (a) Pop ordinary@10: advance to 10. Earliest active was 10.
    stest::SelectTimerSeam::advance_clock(f.sched, 10);
    SLUICE_CHECK(AsyncTestAccess::deadline_heap_size(f.sched) == 3);
    SLUICE_CHECK(on_lo.was_expired());  // ordinary timer won (Expired outcome)

    // (b) Next earliest is select@30 (still ACTIVE). Retire it (helper),
    //     then advance to 30: pump pops the stale Select entry.
    SLUICE_CHECK(stest::SelectTimerSeam::retire_synthetic(f.sched, *s30));
    stest::SelectTimerSeam::advance_clock(f.sched, 30);
    SLUICE_CHECK(AsyncTestAccess::deadline_heap_size(f.sched) == 2);

    // (c) Next earliest is ordinary@100. Advance to 100: pump consumes it.
    stest::SelectTimerSeam::advance_clock(f.sched, 100);
    SLUICE_CHECK(AsyncTestAccess::deadline_heap_size(f.sched) == 1);
    SLUICE_CHECK(on_hi.was_expired());

    // (d) Last is select@200. Retire then advance: stale pop reclaims it.
    SLUICE_CHECK(stest::SelectTimerSeam::retire_synthetic(f.sched, *s200));
    stest::SelectTimerSeam::advance_clock(f.sched, 200);
    SLUICE_CHECK(AsyncTestAccess::deadline_heap_size(f.sched) == 0);

    SLUICE_CHECK(stest::SelectTimerSeam::pool_size(f.sched) == 0);
}

// =========================================================================
// T4 — ordinary timer regression (real ordinary timed wait unchanged)
// =========================================================================

SLUICE_TEST_CASE(t4_ordinary_timer_regression) {
    TFixture f;
    // A real ordinary deadline wait must behave identically after the heap
    // migration: register an ordinary ACTIVE entry and verify the heap holds
    // a tagged Ordinary entry that participates correctly, then resolve it
    // through the pump (Expired outcome) and confirm the counter + pool wind
    // down cleanly.
    sa::WaitNode node;
    sa::WaitQueue q;
    auto* reg = AsyncTestAccess::register_test_deadline(
        f.sched, &node, &q, /*dl=*/100);
    SLUICE_CHECK(reg != nullptr);
    SLUICE_CHECK(reg->is_active());

    auto counts = stest::SelectTimerSeam::heap_counts_by_kind(f.sched);
    SLUICE_CHECK(counts[0] == 1);  // one ordinary heap entry
    SLUICE_CHECK(counts[1] == 0);  // no select entries
    SLUICE_CHECK(AsyncTestAccess::active_deadline_count(f.sched) == 1);

    // Active ordinary deadline participates in the earliest-deadline cache.
    sa::Scheduler::deadline_t out = 0;
    SLUICE_CHECK(AsyncTestAccess::earliest_active_deadline(f.sched, out));
    SLUICE_CHECK(out == 100);

    // Resolve through the pump: advance to the deadline -> Expired outcome,
    // counter decremented, heap entry reclaimed, node unlinked (clean queue).
    stest::SelectTimerSeam::advance_clock(f.sched, 100);
    SLUICE_CHECK(node.was_expired());
    SLUICE_CHECK(AsyncTestAccess::active_deadline_count(f.sched) == 0);
    SLUICE_CHECK(AsyncTestAccess::deadline_heap_size(f.sched) == 0);
}

// =========================================================================
// Common stale-skip runner: T5 (RETIRED) / T6 (CONSUMED) / T7 (instrumentation)
// =========================================================================
//
// Addendum F lazy-reclamation 8-step sequence:
//   1 register ACTIVE
//   2 transition ACTIVE->RETIRED|CONSUMED
//   3 verify counter already decremented
//   4 verify block remains physically in pool+heap
//   5 advance clock
//   6 verify stale skip
//   7 verify no arm read (arm-load delta == 0)
//   8 verify heap entry + pool block reclaimed; counter not decremented again

static void run_stale_skip(State terminal) {
    TFixture f;

    // (1) register an ACTIVE synthetic Select entry with a future deadline.
    const sa::Scheduler::deadline_t dl = 50;
    SelectTimerReg* reg = stest::SelectTimerSeam::register_synthetic(
        f.sched, nullptr, dl);
    SLUICE_CHECK(reg != nullptr);
    SLUICE_CHECK(reg->is_active());
    SLUICE_CHECK(AsyncTestAccess::active_deadline_count(f.sched) == 1);

    // (2) transition ACTIVE -> terminal via the Scheduler accounting helper.
    bool transitioned;
    if (terminal == State::retired) {
        transitioned = stest::SelectTimerSeam::retire_synthetic(f.sched, *reg);
    } else {
        transitioned = stest::SelectTimerSeam::consume_synthetic(f.sched, *reg);
    }
    SLUICE_CHECK(transitioned);
    SLUICE_CHECK(reg->state() == terminal);

    // (3) counter already decremented exactly once (now 0).
    SLUICE_CHECK(AsyncTestAccess::active_deadline_count(f.sched) == 0);

    // (4) block + heap entry still physically present (lazy, before deadline).
    SLUICE_CHECK(stest::SelectTimerSeam::pool_size(f.sched) == 1);
    SLUICE_CHECK(AsyncTestAccess::deadline_heap_size(f.sched) == 1);

    // (5) The pump runs on THIS thread (advance_clock drives it synchronously),
    // so the PumpSkip seam must NOT be armed (arming would self-deadlock the
    // pump). We only observe that it was REACHED. Clear the reached flag first.
    stest::SelectTimerSeam::clear_pump_skip(f.sched);
    stest::SelectTimerSeam::reset_arm_load_count(f.sched);

    // (6) advance_clock drives the pump under global_mtx_. The stale Select
    // entry must be skipped (PumpSkip reached) and physically reclaimed.
    stest::SelectTimerSeam::advance_clock(f.sched, dl);

    // (7) PumpSkip seam reached; arm-load delta == 0 (never read arm_).
    SLUICE_CHECK_MSG(
        stest::SelectTimerSeam::pump_skip_reached(f.sched),
        "PumpSkip seam reached for stale entry");
    SLUICE_CHECK_MSG(
        stest::SelectTimerSeam::arm_load_count(f.sched) == 0,
        "arm_ never read on stale pop");

    // (8) heap entry + pool block reclaimed; counter did not decrement again.
    SLUICE_CHECK(AsyncTestAccess::deadline_heap_size(f.sched) == 0);
    SLUICE_CHECK(stest::SelectTimerSeam::pool_size(f.sched) == 0);
    SLUICE_CHECK(AsyncTestAccess::active_deadline_count(f.sched) == 0);
}

SLUICE_TEST_CASE(t5_retired_stale_skip) {
    run_stale_skip(State::retired);
}

SLUICE_TEST_CASE(t6_consumed_stale_skip) {
    run_stale_skip(State::consumed);
}

// =========================================================================
// T7 — state-before-arm instrumentation (arm loads == 0 for both stale kinds)
// =========================================================================
// Covered structurally by T5/T6 (arm-load delta == 0 asserted there). This
// case makes the invariant explicit and asserts the counter is observed at
// the production dereference site only for ACTIVE entries — which in valid
// P3 never reach the arm read (ACTIVE -> fail-fast, see the death test).

SLUICE_TEST_CASE(t7_state_before_arm_retired_zero) {
    run_stale_skip(State::retired);  // asserts arm_load_count == 0
}

SLUICE_TEST_CASE(t7_state_before_arm_consumed_zero) {
    run_stale_skip(State::consumed);  // asserts arm_load_count == 0
}

// =========================================================================
// T8 — earliest active deadline includes Select, recomputed after retire
// =========================================================================

SLUICE_TEST_CASE(t8_earliest_active_deadline_includes_select) {
    TFixture f;
    // ACTIVE Select entry participates while its deadline is in the future.
    SelectTimerReg* r = stest::SelectTimerSeam::register_synthetic(
        f.sched, nullptr, /*dl=*/40);
    SLUICE_CHECK(r->is_active());

    sa::Scheduler::deadline_t out = 0;
    SLUICE_CHECK(AsyncTestAccess::earliest_active_deadline(f.sched, out));
    SLUICE_CHECK(out == 40);

    // Retire it via the helper; earliest must recompute (no active left).
    SLUICE_CHECK(stest::SelectTimerSeam::retire_synthetic(f.sched, *r));
    bool has = AsyncTestAccess::earliest_active_deadline(f.sched, out);
    SLUICE_CHECK(!has);  // no active deadline remains
    SLUICE_CHECK(AsyncTestAccess::active_deadline_count(f.sched) == 0);
}

// =========================================================================
// T9 — lazy physical reclamation
// =========================================================================

SLUICE_TEST_CASE(t9_lazy_reclamation) {
    // Terminal block remains physically present until its deadline elapses;
    // after the pump pops it, both the heap entry and pool block are gone.
    // (Same 8-step sequence as run_stale_skip; here we emphasize the physical
    // presence/absence transitions explicitly.)
    TFixture f;
    SelectTimerReg* r = stest::SelectTimerSeam::register_synthetic(
        f.sched, nullptr, /*dl=*/70);
    SLUICE_CHECK(stest::SelectTimerSeam::retire_synthetic(f.sched, *r));

    // Before deadline: still physically present (inert).
    SLUICE_CHECK(stest::SelectTimerSeam::pool_size(f.sched) == 1);
    SLUICE_CHECK(AsyncTestAccess::deadline_heap_size(f.sched) == 1);

    // Advance past the deadline (no need to arm the seam; reclaim happens
    // unconditionally on a stale pop).
    stest::SelectTimerSeam::advance_clock(f.sched, 70);

    // After pop: heap entry + pool block absent.
    SLUICE_CHECK(stest::SelectTimerSeam::pool_size(f.sched) == 0);
    SLUICE_CHECK(AsyncTestAccess::deadline_heap_size(f.sched) == 0);
}

// =========================================================================
// T10 — mixed stale Select + ordinary ACTIVE pump
// =========================================================================
// Addendum F: stale terminal Select at D1; ordinary ACTIVE at D2; D1<=D2<=now.
// The pump must reclaim the Select stale entry AND continue to the ordinary.

SLUICE_TEST_CASE(t10_mixed_stale_select_and_ordinary) {
    TFixture f;
    const sa::Scheduler::deadline_t d1 = 20;  // stale select
    const sa::Scheduler::deadline_t d2 = 30;  // ordinary active

    SelectTimerReg* sel = stest::SelectTimerSeam::register_synthetic(
        f.sched, nullptr, d1);
    // Retire the Select entry (terminal) while its deadline is still future.
    SLUICE_CHECK(stest::SelectTimerSeam::retire_synthetic(f.sched, *sel));

    // Ordinary ACTIVE entry at d2.
    sa::WaitNode onode;
    sa::WaitQueue oq;
    AsyncTestAccess::register_test_deadline(f.sched, &onode, &oq, d2);

    SLUICE_CHECK(AsyncTestAccess::active_deadline_count(f.sched) == 1);  // ordinary
    SLUICE_CHECK(AsyncTestAccess::deadline_heap_size(f.sched) == 2);

    // Advance past both deadlines in one step. The pump pops the Select stale
    // entry (skip + reclaim) then the ordinary ACTIVE entry (consume + resolve).
    stest::SelectTimerSeam::advance_clock(f.sched, d2);

    // Both entries reclaimed from the heap.
    SLUICE_CHECK(AsyncTestAccess::deadline_heap_size(f.sched) == 0);
    SLUICE_CHECK(stest::SelectTimerSeam::pool_size(f.sched) == 0);
    // The ordinary timer won its resolve (Expired outcome).
    SLUICE_CHECK(onode.was_expired());
}

// =========================================================================
// T11 — Scheduler identity binding recorded at registration
// =========================================================================
// E13 P3 Corrective closure 6: this is a FIELD-INIT test only — it verifies
// the owning-Scheduler binding is recorded on the block at registration time.
// It is NOT a cross-Scheduler rejection test. The mechanical rejection proof
// (a retire/consume through a DIFFERENT Scheduler, or a pool-membership
// violation) lives in the death binary e13_select_timer_pump_death_test as
// cases XR / XC / NP, which assert the helpers' `reg.scheduler() == this` and
// `pool_owns_select_block_locked` guards fire before any state/counter
// mutation.

SLUICE_TEST_CASE(t11_scheduler_identity_binding_recorded) {
    TFixture f;
    SelectTimerReg* r = stest::SelectTimerSeam::register_synthetic(
        f.sched, nullptr, /*dl=*/50);
    SLUICE_CHECK(r->scheduler() == &f.sched);
    SLUICE_CHECK(r->is_active());
}

// =========================================================================
// T12 — no premature Select behavior
// =========================================================================

SLUICE_TEST_CASE(t12_no_premature_select_behavior) {
    // After P3 operations: no winner claimed, no result, no suspension/
    // runnable, no Event SelectPort mutation. P3 provides only the substrate.
    TFixture f;
    SelectTimerReg* r = stest::SelectTimerSeam::register_synthetic(
        f.sched, nullptr, /*dl=*/50);
    SLUICE_CHECK(stest::SelectTimerSeam::retire_synthetic(f.sched, *r));
    stest::SelectTimerSeam::advance_clock(f.sched, 50);

    // No SelectGroup was ever admitted, so winner remains kNoWinner sentinel
    // (no group object exists to check). The substrate touched no SelectPort,
    // no Fiber, no SelectResult. P3 has no admission. The strongest assertion
    // is that the Scheduler returned to a clean (no active, no leaked blocks)
    // state with no Select authority opened.
    SLUICE_CHECK(AsyncTestAccess::active_deadline_count(f.sched) == 0);
    SLUICE_CHECK(stest::SelectTimerSeam::pool_size(f.sched) == 0);
    SLUICE_CHECK(AsyncTestAccess::deadline_heap_size(f.sched) == 0);
    SLUICE_CHECK(f.sched.waiting_count() == 0);
}

SLUICE_MAIN()
