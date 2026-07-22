// sluice::async::Scheduler — E13 P3 Select Timer registration substrate.
//
// This translation unit implements the Scheduler-owned stable Select timer
// pool and its registered-state accounting authority (Addendum C). It does
// NOT implement the pump branch body or any Select operation — those land in
// a later P3 commit. P3 has no admission path; the substrate here exists so
// the future reserve-then-register protocol is possible without redesign.
//
// Ownership law (docs/e13-select-timer-adapter.md §4.1):
//   - before transfer: a caller-frame temporary list owns the block;
//   - after single-node splice under global_mtx_: the Scheduler pool owns it;
//   - after heap insertion: the heap holds a stable pointer to the block.
//
// Accounting law (Addendum C): every ACTIVE->terminal transition of a
// registered Select block routes through select_timer_retire_locked /
// select_timer_consume_locked, which decrement active_deadline_count_ exactly
// once (on a successful CAS) and recompute the earliest-deadline cache. A
// failed CAS mutates no counter. The stale pump-pop path performs physical
// reclamation only and does NOT decrement again.
//
// No P4 behavior: no winner claim, no CandidateReady, no finalization, no
// publication, no fiber suspension/runnable.
#include <sluice/async/scheduler.hpp>

#include <cassert>
#include <list>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>  // complete SelectArmSlot (P4 finalizers)
#include <sluice/async/detail/select_registration.hpp>

// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the internal-testing variant pulls
// in the non-installed test-control header so the phase call sites below
// resolve to the controller. In the production build this include is absent
// and the seam call sites compile to nothing.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {

// Splice ONE SelectTimerRegistration node from a caller-frame temporary pool
// into select_timer_pool_, push its DeadlineHeapEntry {Select}, and return a
// stable pointer to the now-Scheduler-owned block.
//
// Single-node std::list::splice is O(1) and allocation-free (it relinks
// nodes); the heap push does not allocate within reserved capacity. The
// caller is responsible for reserving deadline_heap_ capacity BEFORE any
// registration mutation (so a bad_alloc cannot leave the Scheduler with a
// partially-registered group). After splice, the block's address is stable
// for the lifetime of the registration (std::list nodes do not relocate).
detail::SelectTimerRegistration* Scheduler::select_timer_splice_one_locked(
    std::list<detail::SelectTimerRegistration>& tmp_pool,
    std::list<detail::SelectTimerRegistration>::iterator it) {
    // Validate the iterator belongs to the caller's temporary pool and the
    // block is bound to this Scheduler.
    assert(it != tmp_pool.end() &&
           "select_timer_splice_one_locked: iterator is end-of-temp-pool");
    assert(it->scheduler() == this &&
           "select_timer_splice_one_locked: block scheduler_ != this");

    // Splice exactly this one node to the end of the Scheduler pool.
    select_timer_pool_.splice(select_timer_pool_.end(), tmp_pool, it);

    // The spliced node is now the back of the Scheduler pool; its address is
    // stable and is the authority the heap entry references.
    detail::SelectTimerRegistration& reg = select_timer_pool_.back();

    // Push the tagged Select entry (cached deadline + Select pointer).
    heap_push_entry_locked(detail::DeadlineHeapEntry::for_select(reg));

    // The block starts ACTIVE; account it so it participates in the MW/park
    // classification + earliest-deadline cache (docs/e13-select-timer-adapter
    // §4.2). The matching decrement is in the retire/consume helper.
    ++active_deadline_count_;
    recompute_earliest_deadline_locked();

    return &reg;
}

// E13 P3 Select timer pump branch body (state-before-arm rule, Addendum E/D).
bool Scheduler::select_timer_pump_entry_locked(
    detail::SelectTimerRegistration& reg) {
    // State-before-arm: load state FIRST. A non-ACTIVE state means the arm
    // pointer MUST NOT be read (it may belong to a destroyed caller frame).
    auto state = reg.state();

    if (state != detail::SelectTimerRegistration::State::active) {
        // Stale entry (RETIRED or CONSUMED). Skip: do not dereference arm_,
        // do not touch SelectGroup, do not claim, do not publish. The caller
        // physically reclaims the popped block. This is the I4 closure.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::e13_timer_pump_skip);
#endif
        return true;  // stale; skipped
    }

    // ACTIVE: a due ACTIVE Select entry is UNREACHABLE in valid P3 (there is
    // no admission path). Reach the PumpActive seam, then instrument the
    // exact production arm dereference site, then fail fast. P3 does NOT
    // claim/mark/retire/consume/erase an ACTIVE block — that is P4, denied.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::e13_timer_pump_active);
    // Instrument the exact production dereference site (immediately before
    // the load), so stale tests can prove arm-load delta == 0.
    ++select_timer_arm_load_count_;
    auto* arm [[maybe_unused]] = reg.arm();  // safe under I4: ACTIVE implies live
    (void)arm;
#endif
    // Stage-boundary invariant violation: a due ACTIVE Select entry where no
    // admission exists. Fail fast rather than silently returning with an
    // ACTIVE registration's authority still open. (When SLUICE_ASYNC_INTERNAL_
    // TESTING is undefined, the arm read above is absent — production never
    // dereferences arm_ here; it goes straight to fail-fast.)
    detail::select_timer_pump_active_fail_fast();
}

// Retire a registered Select block (Addendum C accounting authority).
bool Scheduler::select_timer_retire_locked(
    detail::SelectTimerRegistration& reg) {
    // Ownership validation: the block must belong to this Scheduler and to
    // this Scheduler's pool (defends against a stray/detached local object or
    // a cross-Scheduler mistake).
    assert(reg.scheduler() == this &&
           "select_timer_retire_locked: block scheduler_ != this");
    assert(pool_owns_select_block_locked(reg) &&
           "select_timer_retire_locked: block not in select_timer_pool_");

    if (reg.retire()) {  // CAS ACTIVE -> RETIRED
        --active_deadline_count_;  // exactly once
        recompute_earliest_deadline_locked();
        return true;
    }
    return false;  // already terminal: no counter mutation
}

// Consume a registered Select block (Addendum C accounting authority).
bool Scheduler::select_timer_consume_locked(
    detail::SelectTimerRegistration& reg) {
    assert(reg.scheduler() == this &&
           "select_timer_consume_locked: block scheduler_ != this");
    assert(pool_owns_select_block_locked(reg) &&
           "select_timer_consume_locked: block not in select_timer_pool_");

    if (reg.try_claim_expiry()) {  // CAS ACTIVE -> CONSUMED
        --active_deadline_count_;  // exactly once
        recompute_earliest_deadline_locked();
        return true;
    }
    return false;  // already terminal: no counter mutation
}

// Physical reclamation of a popped Select block. SAFE only because the caller
// (pump) has ALREADY removed the block from the deadline heap. Address match,
// never reads arm_ (I4-safe). Physical-only: does NOT decrement
// active_deadline_count_ (the retire/consume helper already did, exactly
// once — Addendum C).
void Scheduler::erase_popped_select_registration_locked(
    detail::SelectTimerRegistration* r) {
    if (r == nullptr) return;
    for (auto it = select_timer_pool_.begin();
         it != select_timer_pool_.end(); ++it) {
        if (&*it == r) {
            select_timer_pool_.erase(it);
            return;  // r is now dangling; caller must not touch it
        }
    }
    // Not finding the block would indicate a double-erase or a block that was
    // never Scheduler-owned — both are invariant violations. Fail loudly in
    // debug rather than silently returning.
    assert(false && "erase_popped_select_registration_locked: block not in pool");
}

// Predicate: is `reg` a Scheduler-owned block in select_timer_pool_?
bool Scheduler::pool_owns_select_block_locked(
    const detail::SelectTimerRegistration& reg) const noexcept {
    for (const auto& b : select_timer_pool_) {
        if (&b == &reg) return true;
    }
    return false;
}

// ===========================================================================
// E13 P4 Select Timer winner/loser finalizers.
//
// Per-kind finalizer halves for Timer arms, called by the single group
// processor (select_process_group_locked via select_commit_winner_locked /
// select_finalize_loser_locked) under global_mtx_.
//
// Source order (docs/e13-select-locking-and-publication.md §4.1 / §4.2):
//
//   Timer winner (§8.1 / §4.1):
//     1. group winner CAS already succeeded (the driver's caller did it)
//     2. SelectTimerRegistration ACTIVE -> CONSUMED via
//        select_timer_consume_locked (the Timer registration CAS MUST NOT
//        precede the group winner CAS)
//     3. arm.state = Retired
//     4. accounting closed exactly once (select_timer_consume_locked did it)
//     5. NO publication
//
//   Timer loser (§8.2 / §4.2):
//     1. arm.state = Retired  (classification FIRST — SN-9)
//     2. SelectTimerRegistration ACTIVE -> RETIRED via
//        select_timer_retire_locked (the retire CAS comes AFTER arm
//        classification, not before)
//     3. accounting closed exactly once
//     4. NO publication (never writes result/runnable)
//
// The accounting helpers (select_timer_consume_locked /
// select_timer_retire_locked) own the single ACTIVE->terminal accounting
// authority (Addendum C): they CAS + decrement active_deadline_count_ +
// recompute the earliest-deadline cache exactly once on success. A failed CAS
// mutates no counter. After a successful group claim, a Timer winner consume
// MUST succeed (preflight guarantees the registration is ACTIVE), so a false
// return is an invariant violation that fails fast.
// ===========================================================================

// Timer winner finalize (§8.1). The registration CAS (ACTIVE->CONSUMED) happens
// AFTER the group winner CAS (the driver's caller) and BEFORE arm.state =
// Retired. select_timer_consume_locked closes accounting exactly once.
void Scheduler::select_finalize_timer_winner_locked(
    detail::SelectGroup& group, detail::SelectArmSlot& arm) {
    assert(arm.kind == detail::ArmKind::timer &&
           "select_finalize_timer_winner_locked: arm is not a Timer arm");
    detail::SelectTimerRegistration* reg = arm.timer.stable_reg_;
    assert(reg != nullptr &&
           "select_finalize_timer_winner_locked: stable_reg_ is null");
    assert(reg->scheduler() == this &&
           "select_finalize_timer_winner_locked: registration belongs to "
           "another Scheduler");
    assert(pool_owns_select_block_locked(*reg) &&
           "select_finalize_timer_winner_locked: pool does not own the "
           "registration");
    assert(reg->is_active() &&
           "select_finalize_timer_winner_locked: registration not ACTIVE at "
           "winner finalize (group was claimed but registration already "
           "terminal — invariant violation)");
    assert(reg->arm() == &arm &&
           "select_finalize_timer_winner_locked: registration.arm() != &arm");

    // 2. ACTIVE -> CONSUMED via the accounting helper (CAS + decrement +
    //    recompute, exactly once). After a successful group claim with an
    //    ACTIVE registration (preflight-checked), this MUST succeed.
    const bool consumed = select_timer_consume_locked(*reg);
    if (!consumed) {
        // Unreachable after preflight + a successful group claim under the
        // same held global_mtx_. A false return means the registration was
        // not ACTIVE (it raced to terminal), which contradicts the single
        // global_mtx_ CS — an invariant violation. Fail fast.
        detail::select_timer_pump_active_fail_fast();
    }
    // 3. arm terminal classification.
    arm.state = detail::ArmState::retired;
    // 4. accounting closed by select_timer_consume_locked above.
    // 5. NO publication.
    (void)group;
}

// Timer loser finalize (§8.2). SN-9: arm.state = Retired is classified FIRST
// (while the registration is still ACTIVE), THEN the registration is retired.
// An internal-testing phase seam at the classification point lets a test prove
// the registration is still ACTIVE at that instant (the load-bearing SN-9
// ordering). select_timer_retire_locked closes accounting exactly once.
void Scheduler::select_finalize_timer_loser_locked(
    detail::SelectGroup& group, detail::SelectArmSlot& arm) {
    assert(arm.kind == detail::ArmKind::timer &&
           "select_finalize_timer_loser_locked: arm is not a Timer arm");
    detail::SelectTimerRegistration* reg = arm.timer.stable_reg_;
    assert(reg != nullptr &&
           "select_finalize_timer_loser_locked: stable_reg_ is null");
    assert(reg->scheduler() == this &&
           "select_finalize_timer_loser_locked: registration belongs to "
           "another Scheduler");
    assert(pool_owns_select_block_locked(*reg) &&
           "select_finalize_timer_loser_locked: pool does not own the "
           "registration");
    assert(reg->arm() == &arm &&
           "select_finalize_timer_loser_locked: registration.arm() != &arm");

    // 1. arm loser classification FIRST (SN-9). The registration is still
    //    ACTIVE at this point.
    arm.state = detail::ArmState::retired;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic SN-9 observation: at the classification point, the
    // registration MUST still be ACTIVE. The phase seam lets a test pause
    // here and assert reg->is_active() (arm Retired while reg ACTIVE), then
    // the retire CAS below flips it to RETIRED.
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::e13_timer_loser_arm_classified);
#endif

    // 2. ACTIVE -> RETIRED via the accounting helper, AFTER arm
    //    classification. Preflight guarantees the registration is ACTIVE for
    //    every arm at process entry, and this CS is held continuously, so the
    //    CAS must succeed. A false return is an invariant violation.
    assert(reg->is_active() &&
           "select_finalize_timer_loser_locked: registration not ACTIVE at "
           "retire (arm classified but registration already terminal — "
           "invariant violation)");
    const bool retired = select_timer_retire_locked(*reg);
    if (!retired) {
        // Unreachable: preflight checked ACTIVE, this CS is held, no other
        // path retires a Select registration under this lock.
        detail::select_timer_pump_active_fail_fast();
    }
    // 3. accounting closed by select_timer_retire_locked above.
    // 4. NO publication: this path never writes group.result_ and never calls
    //    make_runnable / route_runnable_locked.
    (void)group;
}

}  // namespace sluice::async
