

























#include <sluice/async/scheduler.hpp>

#include <cassert>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/event.hpp>

namespace sluice::async {




void Scheduler::select_finalize_event_winner_locked(
    detail::SelectGroup& group, detail::SelectArmSlot& arm) {
    assert(arm.kind == detail::ArmKind::event &&
           "select_finalize_event_winner_locked: arm is not an Event arm");
    if (arm.kind != detail::ArmKind::event)
        detail::select_invariant_fail_fast();
    assert(arm.event.event_ != nullptr &&
           "select_finalize_event_winner_locked: event_ is null");
    if (arm.event.event_ == nullptr) detail::select_invariant_fail_fast();
    Event& ev = *arm.event.event_;
    assert(arm.home_ == &ev.select_port_ &&
           "select_finalize_event_winner_locked: arm not linked to its Event");
    if (arm.home_ != &ev.select_port_)
        detail::select_invariant_fail_fast();



    select_event_unlink_locked(ev, arm);

    arm.state = detail::ArmState::retired;

    (void)group;
}



void Scheduler::select_finalize_event_loser_locked(
    detail::SelectGroup& group, detail::SelectArmSlot& arm) {
    assert(arm.kind == detail::ArmKind::event &&
           "select_finalize_event_loser_locked: arm is not an Event arm");
    if (arm.kind != detail::ArmKind::event)
        detail::select_invariant_fail_fast();
    assert(arm.event.event_ != nullptr &&
           "select_finalize_event_loser_locked: event_ is null");
    if (arm.event.event_ == nullptr) detail::select_invariant_fail_fast();
    Event& ev = *arm.event.event_;
    assert(arm.home_ == &ev.select_port_ &&
           "select_finalize_event_loser_locked: arm not linked to its Event");
    if (arm.home_ != &ev.select_port_)
        detail::select_invariant_fail_fast();


    arm.state = detail::ArmState::retired;

    select_event_unlink_locked(ev, arm);



    (void)group;
}

}
