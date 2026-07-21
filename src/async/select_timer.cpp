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

}  // namespace sluice::async
