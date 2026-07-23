// sluice::async::Scheduler — E13 P4 Select Event winner/loser finalizers.
//
// Per-kind finalizer halves for Event arms, called by the single group
// processor (select_process_group_locked via select_commit_winner_locked /
// select_finalize_loser_locked) under global_mtx_.
//
// Source order (docs/e13-select-locking-and-publication.md §4.3 / §4.4):
//
//   Event winner:
//     1. group winner CAS already succeeded (the driver's caller did it)
//     2. targeted winner handling (this function)
//     3. unlink arm from Event SelectPort (canonical select_event_unlink_locked)
//     4. arm.state = Retired
//     5. Event SET flag UNCHANGED (never cleared/consumed)
//     6. NO publication
//
//   Event loser:
//     1. arm.state = Retired  (classification FIRST)
//     2. unlink arm from Event SelectPort (canonical select_event_unlink_locked)
//     3. Event SET flag UNCHANGED
//     4. NO publication (never writes result/runnable)
//
// Every unlink reuses the existing canonical Scheduler Event unlink helper —
// no duplicated intrusive-list mutation logic (P4 §8.4). Event::set_ is NEVER
// mutated: Select preserves the persistent-readiness property
// (InvEventPersistentStateNotConsumed).
#include <sluice/async/scheduler.hpp>

#include <cassert>

#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/event.hpp>

namespace sluice::async {

// Event winner finalize (§8.3). The winner unlink happens BEFORE arm.state is
// set to Retired, matching the documented source order; the canonical unlink
// helper accepts the registered/candidate_ready state at entry.
void Scheduler::select_finalize_event_winner_locked(
    detail::SelectGroup& group, detail::SelectArmSlot& arm) {
    assert(arm.kind == detail::ArmKind::event &&
           "select_finalize_event_winner_locked: arm is not an Event arm");
    assert(arm.event.event_ != nullptr &&
           "select_finalize_event_winner_locked: event_ is null");
    Event& ev = *arm.event.event_;
    assert(arm.home_ == &ev.select_port_ &&
           "select_finalize_event_winner_locked: arm not linked to its Event");

    // 3. unlink via the canonical helper (repairs intrusive links, clears
    //    home_/next_/prev_).
    select_event_unlink_locked(ev, arm);
    // 4. arm terminal classification.
    arm.state = detail::ArmState::retired;
    // 5. Event::set_ is deliberately NOT touched.
    (void)group;
}

// Event loser finalize (§8.4). Arm classification (Retired) PRECEDES the
// unlink, matching the documented loser source order.
void Scheduler::select_finalize_event_loser_locked(
    detail::SelectGroup& group, detail::SelectArmSlot& arm) {
    assert(arm.kind == detail::ArmKind::event &&
           "select_finalize_event_loser_locked: arm is not an Event arm");
    assert(arm.event.event_ != nullptr &&
           "select_finalize_event_loser_locked: event_ is null");
    Event& ev = *arm.event.event_;
    assert(arm.home_ == &ev.select_port_ &&
           "select_finalize_event_loser_locked: arm not linked to its Event");

    // 1. arm loser classification FIRST.
    arm.state = detail::ArmState::retired;
    // 2. unlink via the canonical helper.
    select_event_unlink_locked(ev, arm);
    // 3. Event::set_ is deliberately NOT touched.
    // NO publication: this path never writes group.result_ and never calls
    // make_runnable / route_runnable_locked.
    (void)group;
}

}  // namespace sluice::async
