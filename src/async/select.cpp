// sluice::async::Scheduler — E13 P4 Select central claim + winner/loser
// finalization core.
//
// This translation unit owns the SINGLE group processor
// (select_process_group_locked), the preflight validator, the winner-commit
// and loser-finalize drivers, and the reusable all-authority-closed invariant
// predicate. The per-kind finalizer halves live in select_event.cpp (Event
// winner/loser) and select_timer.cpp (Timer winner/loser); the single group
// processor calls into them and owns the iteration over winner and losers.
//
// P4 scope (docs/e13-select-production-test-plan.md §7.4,
//           docs/e13-select-locking-and-publication.md §1, §4, §5):
//   - exactly one winner linearization point (SelectGroup::claim_winner_locked)
//   - full-group preflight validation BEFORE the irreversible winner CAS
//   - winner commit + loser finalization in the SAME global_mtx_ CS
//   - all-authority-closed validation before returning true
//   - NO publication, NO admission, NO phase transition to Completed/Consumed,
//     NO Fiber suspension/runnable, NO SelectResult construction
//
// The "claimed and finalized but unpublished" state is an internal transient
// state under global_mtx_; a later stage will publish before the lock is
// released. In P4 it is observable only through guarded internal-testing seams.
#include <sluice/async/scheduler.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>

#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/event.hpp>

namespace sluice::async {

namespace {
// Upper bound on Select arms (mirrors the public kSelectMaxArms gate).
// Defined locally so this TU's preflight does not depend on select.hpp.
constexpr std::size_t kPreflightMaxArms = 8;
static_assert(kPreflightMaxArms >= 1, "Select must permit at least one arm");
}  // namespace

// ---------------------------------------------------------------------------
// Preflight: validate the ENTIRE group before the irreversible winner CAS.
// Every failure here is a debug assertion that fires BEFORE any mutation, so an
// invalid group cannot become permanently claimed and then fail halfway through
// finalization (P4 §6). No allocation in this path. A Scheduler member because
// it reads Event private fields (scheduler_, select_port_) under the friend
// grant.
// ---------------------------------------------------------------------------
void Scheduler::select_preflight_group_locked(
    detail::SelectGroup& group, std::uint32_t candidate_index) const {
    // --- group-level checks ---
    assert(group.scheduler_ == this &&
           "select_process_group_locked: group.scheduler_ != this");
    assert(group.arms_ != nullptr &&
           "select_process_group_locked: group.arms_ is null");
    assert(group.arm_count_ >= 1 &&
           "select_process_group_locked: group.arm_count_ < 1");
    assert(group.arm_count_ <= kPreflightMaxArms &&
           "select_process_group_locked: group.arm_count_ exceeds kSelectMaxArms");
    assert(candidate_index < group.arm_count_ &&
           "select_process_group_locked: candidate_index out of range");

    const auto phase = group.phase();
    assert((phase == detail::GroupPhase::selecting ||
            phase == detail::GroupPhase::armed) &&
           "select_process_group_locked: group phase must be Selecting or Armed");

    // --- candidate arm checks ---
    detail::SelectArmSlot& candidate = group.arms_[candidate_index];
    assert(candidate.group == &group &&
           "select_process_group_locked: candidate arm.group != &group");
    assert(candidate.state == detail::ArmState::candidate_ready &&
           "select_process_group_locked: candidate arm not CandidateReady");

    // --- every arm checks ---
    for (std::size_t i = 0; i < group.arm_count_; ++i) {
        detail::SelectArmSlot& arm = group.arms_[i];
        assert(arm.group == &group &&
               "select_process_group_locked: arm.group != &group");
        assert((arm.kind == detail::ArmKind::event ||
                arm.kind == detail::ArmKind::timer) &&
               "select_process_group_locked: arm.kind must be Event or Timer");
        assert((arm.state == detail::ArmState::registered ||
                arm.state == detail::ArmState::candidate_ready) &&
               "select_process_group_locked: arm.state must be Registered or "
               "CandidateReady");
    }

    // --- Event arm preflight ---
    for (std::size_t i = 0; i < group.arm_count_; ++i) {
        detail::SelectArmSlot& arm = group.arms_[i];
        if (arm.kind != detail::ArmKind::event) continue;
        assert(arm.event.event_ != nullptr &&
               "select_process_group_locked: Event arm event_ is null");
        Event& ev = *arm.event.event_;
        assert(&ev.scheduler_ == this &&
               "select_process_group_locked: Event does not belong to this Scheduler");
        assert(arm.home_ == &ev.select_port_ &&
               "select_process_group_locked: Event arm not linked to its Event port");
        // Mechanical membership: the arm must actually be reachable from the
        // port's intrusive list (defends against a stale home_ pointer).
        bool found = false;
        for (detail::SelectArmSlot* p = ev.select_port_.head_; p != nullptr;
             p = p->next_) {
            if (p == &arm) { found = true; break; }
        }
        (void)found;
        assert(found && "select_process_group_locked: Event arm home_ points at "
               "this Event's port but the arm is not in the intrusive list");
    }

    // --- Timer arm preflight ---
    for (std::size_t i = 0; i < group.arm_count_; ++i) {
        detail::SelectArmSlot& arm = group.arms_[i];
        if (arm.kind != detail::ArmKind::timer) continue;
        detail::SelectTimerRegistration* reg = arm.timer.stable_reg_;
        assert(reg != nullptr &&
               "select_process_group_locked: Timer arm stable_reg_ is null");
        assert(reg->scheduler() == this &&
               "select_process_group_locked: Timer registration belongs to "
               "another Scheduler");
        assert(pool_owns_select_block_locked(*reg) &&
               "select_process_group_locked: Scheduler pool does not own the "
               "Timer registration");
        assert(reg->is_active() &&
               "select_process_group_locked: Timer registration is not ACTIVE");
        assert(reg->arm() == &arm &&
               "select_process_group_locked: Timer registration.arm() != &arm");
    }
}

// ---------------------------------------------------------------------------
// The single group processor (P4 §3, §7).
// ---------------------------------------------------------------------------
bool Scheduler::select_process_group_locked(detail::SelectGroup& group,
                                            std::uint32_t candidate_index) {
    select_preflight_group_locked(group, candidate_index);

    // If another invocation already owns the winner, this invocation performs
    // NO mutation and returns claim-lost. This check happens after preflight
    // (preflight validates group shape regardless of winner state) but BEFORE
    // the CAS attempt — and the CAS itself is the linearization authority.
    if (group.winner() != detail::kNoWinner) {
        return false;
    }

    // THE single winner linearization point. relaxed/relaxed: synchronization
    // of the surrounding arm-state visibility is provided by global_mtx_, not
    // by the CAS memory order (arm finalization happens AFTER the CAS).
    if (!group.claim_winner_locked(candidate_index)) {
        // Lost a concurrent claim (another invocation won between the winner()
        // read above and this CAS under the SAME global_mtx_ — unreachable for
        // a single held lock, but the CAS is the authority, so honor it).
        return false;
    }

    // Won the claim. Commit the winner, finalize every loser, in this CS.
    select_commit_winner_locked(group, candidate_index);
    for (std::uint32_t i = 0; i < group.arm_count_; ++i) {
        if (i == candidate_index) continue;
        select_finalize_loser_locked(group, i);
    }

    // Assert the all-authority-closed invariant before reporting success. A
    // future select_publish_locked must call this same predicate at its entry.
    assert(select_all_authority_closed_locked(group) &&
           "select_process_group_locked: all-authority-closed invariant failed "
           "after finalization");

    return true;
}

// ---------------------------------------------------------------------------
// Winner commit driver (P4 §8.1 Timer winner, §8.3 Event winner).
// ---------------------------------------------------------------------------
void Scheduler::select_commit_winner_locked(detail::SelectGroup& group,
                                            std::uint32_t winner_index) {
    detail::SelectArmSlot& arm = group.arms_[winner_index];
    if (arm.kind == detail::ArmKind::event) {
        select_finalize_event_winner_locked(group, arm);
    } else {
        select_finalize_timer_winner_locked(group, arm);
    }
}

// ---------------------------------------------------------------------------
// Loser finalize driver (P4 §8.2 Timer loser, §8.4 Event loser).
// ---------------------------------------------------------------------------
void Scheduler::select_finalize_loser_locked(detail::SelectGroup& group,
                                             std::uint32_t loser_index) {
    detail::SelectArmSlot& arm = group.arms_[loser_index];
    if (arm.kind == detail::ArmKind::event) {
        select_finalize_event_loser_locked(group, arm);
    } else {
        select_finalize_timer_loser_locked(group, arm);
    }
}

// ---------------------------------------------------------------------------
// Reusable all-authority-closed invariant predicate (P4 §10, SN-10).
// ---------------------------------------------------------------------------
bool Scheduler::select_all_authority_closed_locked(
    const detail::SelectGroup& group) const {
    if (group.winner() == detail::kNoWinner) return false;
    if (group.winner() >= group.arm_count_) return false;

    const std::uint32_t winner = group.winner();
    for (std::uint32_t i = 0; i < group.arm_count_; ++i) {
        const detail::SelectArmSlot& arm = group.arms_[i];
        if (arm.state != detail::ArmState::retired) return false;

        if (arm.kind == detail::ArmKind::event) {
            if (arm.home_ != nullptr) return false;
            if (arm.next_ != nullptr) return false;
            if (arm.prev_ != nullptr) return false;
        } else {
            const detail::SelectTimerRegistration* reg = arm.timer.stable_reg_;
            if (reg == nullptr) return false;
            // Winner Timer must be CONSUMED; loser Timers must be RETIRED.
            if (i == winner) {
                if (!reg->is_consumed()) return false;
            } else {
                if (!reg->is_retired()) return false;
            }
        }
    }
    return true;
}

}  // namespace sluice::async
