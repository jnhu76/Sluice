#include <sluice/async/scheduler.hpp>

#include <cassert>
#include <list>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/detail/select_registration.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {

detail::SelectTimerRegistration*
Scheduler::select_timer_splice_one_locked(std::list<detail::SelectTimerRegistration>& tmp_pool,
                                          std::list<detail::SelectTimerRegistration>::iterator it) {
    assert(it != tmp_pool.end() && "select_timer_splice_one_locked: iterator is end-of-temp-pool");
    if (it == tmp_pool.end())
        detail::select_invariant_fail_fast();
    assert(it->scheduler() == this && "select_timer_splice_one_locked: block scheduler_ != this");
    if (it->scheduler() != this)
        detail::select_invariant_fail_fast();

    select_timer_pool_.splice(select_timer_pool_.end(), tmp_pool, it);

    detail::SelectTimerRegistration& reg = select_timer_pool_.back();

    heap_push_entry_locked(detail::DeadlineHeapEntry::for_select(reg));

    ++active_deadline_count_;
    recompute_earliest_deadline_locked();

    return &reg;
}

bool Scheduler::select_timer_pump_entry_locked(detail::SelectTimerRegistration& reg) {
    auto state = reg.state();

    if (state != detail::SelectTimerRegistration::State::active) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(*this, sluice_async_test::PhaseTag::select_timer_pump_skip);
#endif
        return true;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    sluice_async_test::test_phase(*this, sluice_async_test::PhaseTag::select_timer_pump_active);
    ++select_timer_arm_load_count_;
#endif
    (void)select_resolve_timer_locked(reg);
    return false;
}

bool Scheduler::select_timer_retire_locked(detail::SelectTimerRegistration& reg) {
    assert(reg.scheduler() == this && "select_timer_retire_locked: block scheduler_ != this");
    if (reg.scheduler() != this)
        detail::select_invariant_fail_fast();
    const bool owned = pool_owns_select_block_locked(reg);
    assert(owned && "select_timer_retire_locked: block not in select_timer_pool_");
    if (!owned)
        detail::select_invariant_fail_fast();

    if (reg.retire()) {
        --active_deadline_count_;
        recompute_earliest_deadline_locked();
        return true;
    }
    return false;
}

bool Scheduler::select_timer_consume_locked(detail::SelectTimerRegistration& reg) {
    assert(reg.scheduler() == this && "select_timer_consume_locked: block scheduler_ != this");
    if (reg.scheduler() != this)
        detail::select_invariant_fail_fast();
    const bool owned = pool_owns_select_block_locked(reg);
    assert(owned && "select_timer_consume_locked: block not in select_timer_pool_");
    if (!owned)
        detail::select_invariant_fail_fast();

    if (reg.try_claim_expiry()) {
        --active_deadline_count_;
        recompute_earliest_deadline_locked();
        return true;
    }
    return false;
}

void Scheduler::erase_popped_select_registration_locked(detail::SelectTimerRegistration* r) {
    if (r == nullptr)
        return;
    for (auto it = select_timer_pool_.begin(); it != select_timer_pool_.end(); ++it) {
        if (&*it == r) {
            select_timer_pool_.erase(it);
            return;
        }
    }

    assert(false && "erase_popped_select_registration_locked: block not in pool");
    detail::select_invariant_fail_fast();
}

bool Scheduler::pool_owns_select_block_locked(
    const detail::SelectTimerRegistration& reg) const noexcept {
    for (const auto& b : select_timer_pool_) {
        if (&b == &reg)
            return true;
    }
    return false;
}

void Scheduler::select_finalize_timer_winner_locked(detail::SelectGroup& group,
                                                    detail::SelectArmSlot& arm) {
    assert(arm.kind == detail::ArmKind::timer &&
           "select_finalize_timer_winner_locked: arm is not a Timer arm");
    if (arm.kind != detail::ArmKind::timer)
        detail::select_invariant_fail_fast();
    detail::SelectTimerRegistration* reg = arm.timer.stable_reg_;
    assert(reg != nullptr && "select_finalize_timer_winner_locked: stable_reg_ is null");
    if (reg == nullptr)
        detail::select_invariant_fail_fast();
    assert(reg->scheduler() == this &&
           "select_finalize_timer_winner_locked: registration belongs to "
           "another Scheduler");
    if (reg->scheduler() != this)
        detail::select_invariant_fail_fast();
    const bool owned = pool_owns_select_block_locked(*reg);
    assert(owned && "select_finalize_timer_winner_locked: pool does not own the "
                    "registration");
    if (!owned)
        detail::select_invariant_fail_fast();
    assert(reg->is_active() && "select_finalize_timer_winner_locked: registration not ACTIVE at "
                               "winner finalize (group was claimed but registration already "
                               "terminal — invariant violation)");
    if (!reg->is_active())
        detail::select_invariant_fail_fast();
    assert(reg->arm() == &arm && "select_finalize_timer_winner_locked: registration.arm() != &arm");
    if (reg->arm() != &arm)
        detail::select_invariant_fail_fast();

    const bool consumed = select_timer_consume_locked(*reg);
    if (!consumed) {
        detail::select_invariant_fail_fast();
    }

    arm.state = detail::ArmState::retired;

    (void)group;
}

void Scheduler::select_finalize_timer_loser_locked(detail::SelectGroup& group,
                                                   detail::SelectArmSlot& arm) {
    assert(arm.kind == detail::ArmKind::timer &&
           "select_finalize_timer_loser_locked: arm is not a Timer arm");
    if (arm.kind != detail::ArmKind::timer)
        detail::select_invariant_fail_fast();
    detail::SelectTimerRegistration* reg = arm.timer.stable_reg_;
    assert(reg != nullptr && "select_finalize_timer_loser_locked: stable_reg_ is null");
    if (reg == nullptr)
        detail::select_invariant_fail_fast();
    assert(reg->scheduler() == this &&
           "select_finalize_timer_loser_locked: registration belongs to "
           "another Scheduler");
    if (reg->scheduler() != this)
        detail::select_invariant_fail_fast();
    const bool owned = pool_owns_select_block_locked(*reg);
    assert(owned && "select_finalize_timer_loser_locked: pool does not own the "
                    "registration");
    if (!owned)
        detail::select_invariant_fail_fast();
    assert(reg->arm() == &arm && "select_finalize_timer_loser_locked: registration.arm() != &arm");
    if (reg->arm() != &arm)
        detail::select_invariant_fail_fast();

    arm.state = detail::ArmState::retired;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    sluice_async_test::test_phase(*this,
                                  sluice_async_test::PhaseTag::select_timer_loser_arm_classified);
#endif

    assert(reg->is_active() && "select_finalize_timer_loser_locked: registration not ACTIVE at "
                               "retire (arm classified but registration already terminal — "
                               "invariant violation)");
    if (!reg->is_active())
        detail::select_invariant_fail_fast();
    const bool retired = select_timer_retire_locked(*reg);
    if (!retired) {
        detail::select_invariant_fail_fast();
    }

    (void)group;
}

} // namespace sluice::async
