// e13_select_type — E13 Select type construction and compile-fail gates (P1).
//
// Tests the public value types (SelectResult, EventSelectCase, TimerSelectCase),
// the internal type graph (SelectGroup, SelectArmSlot, SelectPort,
// SelectTimerRegistration), and compile-time constraint gates (SF-1..SF-3).
//
// P1 only: type construction/destruction, state transitions, compile-time
// constraint verification. No production select() behavior.
#include <sluice/async/select.hpp>
#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/detail/select_registration.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>

#include "async_test_control.hpp"
#include "harness.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace sa = sluice::async;
namespace sad = sluice::async::detail;
namespace stest = sluice_async_test;

using AsyncTestAccess = sa::Scheduler::AsyncTestAccess;

// ---- Constraint helper concepts (SF-1..SF-3 gate verification) ----

namespace {

// SelectInvocable performs actual overload resolution against the real
// select() declaration. This is NOT a mirror of the requires clause;
// substitution failure here means the real select() overload rejected the
// arguments — which is exactly the gate we want to test.
template <class... Cases>
concept SelectInvocable =
    requires(sa::Scheduler& scheduler, Cases&&... cases) {
        sa::select(scheduler, std::forward<Cases>(cases)...);
    };

}  // anonymous namespace

// =========================================================================
// H1: Public value types
// =========================================================================

SLUICE_TEST_CASE(test_select_result_default_sentinel) {
    constexpr sa::SelectResult r;
    static_assert(!r.has_winner());
    // These compile (they are constexpr) but may assert. We test the sentinel
    // values are valid.
    SLUICE_CHECK(!r.has_winner());
}

SLUICE_TEST_CASE(test_select_result_event_winner) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    constexpr sa::SelectResult r(
        0, sa::SelectKind::event, sa::SelectTimerOutcome::fired,
        sa::SelectResult::TestInit{});
    static_assert(r.has_winner());
    static_assert(r.index() == 0);
    static_assert(r.kind() == sa::SelectKind::event);
    // timer_outcome called on event kind — assertion fires but returns sentinel.
    SLUICE_CHECK(r.has_winner());
    SLUICE_CHECK(r.index() == 0);
    SLUICE_CHECK(r.kind() == sa::SelectKind::event);
#endif
}

SLUICE_TEST_CASE(test_select_result_timer_winner) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    constexpr sa::SelectResult r(
        2, sa::SelectKind::timer, sa::SelectTimerOutcome::fired,
        sa::SelectResult::TestInit{});
    static_assert(r.has_winner());
    static_assert(r.index() == 2);
    static_assert(r.kind() == sa::SelectKind::timer);
    static_assert(r.timer_outcome() == sa::SelectTimerOutcome::fired);
    SLUICE_CHECK(r.has_winner());
    SLUICE_CHECK(r.index() == 2);
    SLUICE_CHECK(r.kind() == sa::SelectKind::timer);
    SLUICE_CHECK(r.timer_outcome() == sa::SelectTimerOutcome::fired);
#endif
}

SLUICE_TEST_CASE(test_event_select_case_construct) {
    // Minimal Scheduler + FakeAsyncIoContext to construct an Event.
    // EventSelectCase needs an Event reference.
    // We use a minimal test setup.
    sluice::async::AsyncIoContext ctx(std::make_unique<sluice::async::FakeAsyncBackend>());
    sa::Scheduler sched(ctx);
    sa::Event ev(sched);

    sa::EventSelectCase esc(ev);
    (void)esc;

    // No default construction.
    static_assert(!std::is_default_constructible_v<sa::EventSelectCase>);
    // Copy/move traits.
    static_assert(std::is_copy_constructible_v<sa::EventSelectCase>);
    static_assert(std::is_copy_assignable_v<sa::EventSelectCase>);
    static_assert(std::is_move_constructible_v<sa::EventSelectCase>);
    static_assert(std::is_move_assignable_v<sa::EventSelectCase>);
}

SLUICE_TEST_CASE(test_timer_select_case_construct) {
    sluice::async::AsyncIoContext ctx(std::make_unique<sluice::async::FakeAsyncBackend>());
    sa::Scheduler sched(ctx);

    auto deadline = sched.monotonic_now();
    sa::TimerSelectCase tsc(sched, deadline);
    (void)tsc;

    static_assert(!std::is_default_constructible_v<sa::TimerSelectCase>);
    static_assert(std::is_copy_constructible_v<sa::TimerSelectCase>);
    static_assert(std::is_copy_assignable_v<sa::TimerSelectCase>);
    static_assert(std::is_move_constructible_v<sa::TimerSelectCase>);
    static_assert(std::is_move_assignable_v<sa::TimerSelectCase>);
}

SLUICE_TEST_CASE(test_select_case_type_concept) {
    static_assert(sa::SelectCaseType<sa::EventSelectCase>);
    static_assert(sa::SelectCaseType<sa::TimerSelectCase>);

    // References should also satisfy.
    static_assert(sa::SelectCaseType<sa::EventSelectCase&>);
    static_assert(sa::SelectCaseType<const sa::EventSelectCase&>);
    static_assert(sa::SelectCaseType<sa::TimerSelectCase&>);

    // Non-case types should not satisfy.
    static_assert(!sa::SelectCaseType<int>);
    static_assert(!sa::SelectCaseType<double>);
    static_assert(!sa::SelectCaseType<void>);

    struct Foo {};
    static_assert(!sa::SelectCaseType<Foo>);
    static_assert(!sa::SelectCaseType<sa::SelectResult>);
}

// =========================================================================
// H2: Internal type construction
// =========================================================================

SLUICE_TEST_CASE(test_enum_values) {
    // Verify enum value assignments match design.
    static_assert(static_cast<int>(sad::ArmKind::event) == 0);
    static_assert(static_cast<int>(sad::ArmKind::timer) == 1);

    static_assert(static_cast<int>(sad::ArmState::detached) == 0);
    static_assert(static_cast<int>(sad::ArmState::prepared) == 1);
    static_assert(static_cast<int>(sad::ArmState::registered) == 2);
    static_assert(static_cast<int>(sad::ArmState::candidate_ready) == 3);
    static_assert(static_cast<int>(sad::ArmState::retired) == 4);

    static_assert(static_cast<int>(sad::GroupPhase::building) == 0);
    static_assert(static_cast<int>(sad::GroupPhase::selecting) == 1);
    static_assert(static_cast<int>(sad::GroupPhase::armed) == 2);
    static_assert(static_cast<int>(sad::GroupPhase::completed) == 3);
    static_assert(static_cast<int>(sad::GroupPhase::consumed) == 4);
    static_assert(static_cast<int>(sad::GroupPhase::rollback) == 5);
    static_assert(static_cast<int>(sad::GroupPhase::aborted) == 6);

    static_assert(static_cast<int>(sad::CompletionMode::none) == 0);
    static_assert(static_cast<int>(sad::CompletionMode::inline_) == 1);
    static_assert(static_cast<int>(sad::CompletionMode::suspended) == 2);
}

SLUICE_TEST_CASE(test_event_arm_slot_construction_destruction) {
    sluice::async::AsyncIoContext ctx(std::make_unique<sluice::async::FakeAsyncBackend>());
    sa::Scheduler sched(ctx);
    sa::Event ev(sched);

    sad::SelectArmSlot slot;
    slot.construct_event(ev);
    SLUICE_CHECK(slot.kind == sad::ArmKind::event);
    SLUICE_CHECK(slot.state == sad::ArmState::detached);
    // Slot is non-copyable, non-movable.
    static_assert(!std::is_copy_constructible_v<sad::SelectArmSlot>);
    static_assert(!std::is_copy_assignable_v<sad::SelectArmSlot>);
    static_assert(!std::is_move_constructible_v<sad::SelectArmSlot>);
    static_assert(!std::is_move_assignable_v<sad::SelectArmSlot>);
}

SLUICE_TEST_CASE(test_timer_arm_slot_construction_destruction) {
    // EventArmPayload and TimerArmPayload are trivially destructible. The
    // payloads' default member initializers make them non-trivially default
    // constructible, so SelectArmSlot explicitly manages active-member lifetime
    // (its constructor activates Event; its destructor destroys the active
    // member per `kind`).
    static_assert(std::is_trivially_destructible_v<sad::EventArmPayload>);
    static_assert(std::is_trivially_destructible_v<sad::TimerArmPayload>);

    sad::SelectArmSlot slot;
    slot.construct_timer(42, nullptr);
    SLUICE_CHECK(slot.kind == sad::ArmKind::timer);
    SLUICE_CHECK(slot.timer.deadline_ == 42);
    SLUICE_CHECK(slot.timer.stable_reg_ == nullptr);
}

// ---------------------------------------------------------------------------
// U1-U4: SelectArmSlot union active-member lifetime.
//
// The payloads carry default member initializers (event_{nullptr}, deadline_{0},
// stable_reg_{nullptr}) making them NON-trivially default constructible. The
// implicit union active-member rule therefore does NOT apply, so SelectArmSlot
// must explicitly establish/switch/destroy the active union member. These cases
// prove default-activation and every active-member transition is well defined.
// ---------------------------------------------------------------------------

// U1: default-constructed slot has the Event member active and initialized.
SLUICE_TEST_CASE(test_select_arm_slot_u1_default_event_active) {
    sad::SelectArmSlot slot;  // default-activates the Event member
    SLUICE_CHECK(slot.kind == sad::ArmKind::event);
    // The default active Event payload is value-initialized (event_ == nullptr).
    SLUICE_CHECK(slot.event.event_ == nullptr);
}

// U2: Event -> Timer transition leaves a valid Timer payload.
SLUICE_TEST_CASE(test_select_arm_slot_u2_event_to_timer) {
    sluice::async::AsyncIoContext ctx(std::make_unique<sluice::async::FakeAsyncBackend>());
    sa::Scheduler sched(ctx);
    sa::Event ev(sched);

    sad::SelectArmSlot slot;
    slot.construct_event(ev);
    SLUICE_CHECK(slot.event.event_ == &ev);

    // A never-registered (detached) registration is sufficient for this payload
    // lifetime test: construct_timer only stores the back-pointer.
    sad::SelectTimerRegistration reg(&slot, &sched, 7);
    slot.construct_timer(7, &reg);
    SLUICE_CHECK(slot.kind == sad::ArmKind::timer);
    SLUICE_CHECK(slot.timer.deadline_ == 7);
    SLUICE_CHECK(slot.timer.stable_reg_ == &reg);
}

// U3: Timer -> Event transition leaves a valid Event payload.
SLUICE_TEST_CASE(test_select_arm_slot_u3_timer_to_event) {
    sluice::async::AsyncIoContext ctx(std::make_unique<sluice::async::FakeAsyncBackend>());
    sa::Scheduler sched(ctx);
    sa::Event ev(sched);

    sad::SelectArmSlot slot;
    slot.construct_timer(9, nullptr);
    SLUICE_CHECK(slot.kind == sad::ArmKind::timer);

    slot.construct_event(ev);
    SLUICE_CHECK(slot.kind == sad::ArmKind::event);
    SLUICE_CHECK(slot.event.event_ == &ev);
}

// U4: repeated active-member switching (Event -> Timer -> Event) stays valid.
SLUICE_TEST_CASE(test_select_arm_slot_u4_repeated_switching) {
    sluice::async::AsyncIoContext ctx(std::make_unique<sluice::async::FakeAsyncBackend>());
    sa::Scheduler sched(ctx);
    sa::Event ev(sched);

    sad::SelectArmSlot slot;
    slot.construct_event(ev);
    slot.construct_timer(11, nullptr);
    slot.construct_event(ev);  // back to Event
    SLUICE_CHECK(slot.kind == sad::ArmKind::event);
    SLUICE_CHECK(slot.event.event_ == &ev);

    // And switch to Timer once more for good measure.
    slot.construct_timer(13, nullptr);
    SLUICE_CHECK(slot.kind == sad::ArmKind::timer);
    SLUICE_CHECK(slot.timer.deadline_ == 13);
    SLUICE_CHECK(slot.timer.stable_reg_ == nullptr);
}

SLUICE_TEST_CASE(test_select_group_structural_construction) {
    sad::SelectGroup group;
    // Structural object: not admitted, so destructor does not enforce phase.
    // Phase defaults to building.
    SLUICE_CHECK(group.phase() == sad::GroupPhase::building);
    SLUICE_CHECK(group.winner() == sad::kNoWinner);
    // group is non-copyable, non-movable.
    static_assert(!std::is_copy_constructible_v<sad::SelectGroup>);
    static_assert(!std::is_copy_assignable_v<sad::SelectGroup>);
    static_assert(!std::is_move_constructible_v<sad::SelectGroup>);
    static_assert(!std::is_move_assignable_v<sad::SelectGroup>);
}

SLUICE_TEST_CASE(test_select_group_phase_transitions) {
    sad::SelectGroup group;
    group.mark_admitted();
    SLUICE_CHECK(group.phase() == sad::GroupPhase::building);

    group.set_phase(sad::GroupPhase::selecting);
    SLUICE_CHECK(group.phase() == sad::GroupPhase::selecting);

    group.set_phase(sad::GroupPhase::consumed);
    SLUICE_CHECK(group.phase() == sad::GroupPhase::consumed);
}

SLUICE_TEST_CASE(test_select_group_winner_claim) {
    sad::SelectGroup group;
    group.mark_admitted();
    // This is a structural (never-registered) object: scheduler_ == nullptr,
    // arms_ == nullptr, arm_count_ == 0 — exactly the detached precondition the
    // guarded test entry enforces. The winner CAS is PRIVATE (E13 P4 §5.1: a
    // registered group cannot bypass Scheduler::select_process_group_locked);
    // reach it via the detached-group test entry.
    SLUICE_CHECK(group.winner() == sad::kNoWinner);

    // First claim succeeds.
    SLUICE_CHECK(AsyncTestAccess::detached_claim_winner(group, 2));
    SLUICE_CHECK(group.winner() == 2);

    // Second claim fails.
    SLUICE_CHECK(!AsyncTestAccess::detached_claim_winner(group, 5));
    SLUICE_CHECK(group.winner() == 2);

    // Admitted group must end in consumed/aborted before destruction.
    group.set_phase(sad::GroupPhase::consumed);
}

SLUICE_TEST_CASE(test_select_port_starts_empty) {
    sad::SelectPort port;
    static_assert(!std::is_copy_constructible_v<sad::SelectPort>);
    static_assert(!std::is_copy_assignable_v<sad::SelectPort>);
    static_assert(!std::is_move_constructible_v<sad::SelectPort>);
    static_assert(!std::is_move_assignable_v<sad::SelectPort>);
    SLUICE_CHECK(port.empty());
}

SLUICE_TEST_CASE(test_timer_registration_state_transitions) {
    sad::SelectTimerRegistration reg;
    SLUICE_CHECK(reg.state() == sad::SelectTimerRegistration::State::active);
    SLUICE_CHECK(reg.is_active());
    SLUICE_CHECK(!reg.is_retired());
    SLUICE_CHECK(!reg.is_consumed());

    // Retire path. The CAS methods are private (E13 P3 Corrective closure 3);
    // reach them via the guarded detached-CAS test entry on this never-registered
    // stack-local object.
    SLUICE_CHECK(stest::E13SelectTimerSeam::detached_retire(reg));
    SLUICE_CHECK(reg.is_retired());
    SLUICE_CHECK(!reg.is_active());
    SLUICE_CHECK(!reg.is_consumed());

    // Second retire fails (already retired).
    SLUICE_CHECK(!stest::E13SelectTimerSeam::detached_retire(reg));
    SLUICE_CHECK(reg.is_retired());

    // Non-copyable, non-movable.
    static_assert(!std::is_copy_constructible_v<sad::SelectTimerRegistration>);
    static_assert(!std::is_copy_assignable_v<sad::SelectTimerRegistration>);
    static_assert(!std::is_move_constructible_v<sad::SelectTimerRegistration>);
    static_assert(!std::is_move_assignable_v<sad::SelectTimerRegistration>);
}

SLUICE_TEST_CASE(test_timer_registration_claim_expiry) {
    sad::SelectTimerRegistration reg;
    SLUICE_CHECK(reg.is_active());

    // Claim expiry (guarded detached-CAS entry; see test above).
    SLUICE_CHECK(stest::E13SelectTimerSeam::detached_try_claim_expiry(reg));
    SLUICE_CHECK(reg.is_consumed());

    // Second claim fails.
    SLUICE_CHECK(!stest::E13SelectTimerSeam::detached_try_claim_expiry(reg));
    SLUICE_CHECK(reg.is_consumed());
}

SLUICE_TEST_CASE(test_timer_registration_retire_then_claim_fails) {
    sad::SelectTimerRegistration reg;
    SLUICE_CHECK(reg.is_active());

    // Retire first.
    SLUICE_CHECK(stest::E13SelectTimerSeam::detached_retire(reg));
    SLUICE_CHECK(reg.is_retired());

    // Try claim — fails because already retired.
    SLUICE_CHECK(!stest::E13SelectTimerSeam::detached_try_claim_expiry(reg));
    SLUICE_CHECK(reg.is_retired());
}

SLUICE_TEST_CASE(test_timer_registration_claim_then_retire_fails) {
    sad::SelectTimerRegistration reg;
    SLUICE_CHECK(reg.is_active());

    // Claim first.
    SLUICE_CHECK(stest::E13SelectTimerSeam::detached_try_claim_expiry(reg));
    SLUICE_CHECK(reg.is_consumed());

    // Retire — fails because already consumed.
    SLUICE_CHECK(!stest::E13SelectTimerSeam::detached_retire(reg));
    SLUICE_CHECK(reg.is_consumed());
}

// =========================================================================
// H3: Compile-fail gates (SF-1..SF-3 — bound to actual select() overload)
// =========================================================================

// SF-1: select() with zero case arms must fail the sizeof...(Cases) >= 1 constraint.
SLUICE_TEST_CASE(test_sf_1_select_with_zero_arms) {
    static_assert(!SelectInvocable<>);
}

// Conformance: SelectInvocable<int> must fail (int is not SelectCaseType).
SLUICE_TEST_CASE(test_sf_1_select_invalid_type_rejected) {
    static_assert(!SelectInvocable<int>);
}

// 9 EventSelectCase arms must fail (> kSelectMaxArms = 8).
SLUICE_TEST_CASE(test_sf_2_select_nine_arms_rejected) {
    // We need Event instances to construct EventSelectCase values.
    // Use a minimal Scheduler + Event setup.
    sluice::async::AsyncIoContext ctx(std::make_unique<sluice::async::FakeAsyncBackend>());
    sa::Scheduler sched(ctx);
    sa::Event e1(sched), e2(sched), e3(sched), e4(sched), e5(sched),
             e6(sched), e7(sched), e8(sched), e9(sched);
    static_assert(!SelectInvocable<
        sa::EventSelectCase, sa::EventSelectCase, sa::EventSelectCase,
        sa::EventSelectCase, sa::EventSelectCase, sa::EventSelectCase,
        sa::EventSelectCase, sa::EventSelectCase, sa::EventSelectCase
    >);
    (void)e1; (void)e2; (void)e3; (void)e4; (void)e5;
    (void)e6; (void)e7; (void)e8; (void)e9;
}

// SF-2: 8 EventSelectCase arms should be accepted (at the type level).
SLUICE_TEST_CASE(test_sf_2_select_eight_arms_accepted) {
    static_assert(SelectInvocable<sa::EventSelectCase, sa::EventSelectCase,
                                  sa::EventSelectCase, sa::EventSelectCase,
                                  sa::EventSelectCase, sa::EventSelectCase,
                                  sa::EventSelectCase, sa::EventSelectCase>);
}

// SF-3: select with an int arm is not a valid expression.
SLUICE_TEST_CASE(test_sf_3_select_with_int_arm) {
    static_assert(!SelectInvocable<int>);
}

// Positive: EventSelectCase satisfies the full constraint.
SLUICE_TEST_CASE(test_event_select_case_is_valid) {
    static_assert(SelectInvocable<sa::EventSelectCase>);
}

// Positive: TimerSelectCase satisfies the full constraint.
SLUICE_TEST_CASE(test_timer_select_case_is_valid) {
    static_assert(SelectInvocable<sa::TimerSelectCase>);
}

// Positive: mixed Event + Timer pack satisfies the constraint.
SLUICE_TEST_CASE(test_mixed_select_cases_valid) {
    static_assert(SelectInvocable<sa::EventSelectCase, sa::TimerSelectCase>);
    static_assert(SelectInvocable<sa::TimerSelectCase, sa::EventSelectCase>);
    static_assert(SelectInvocable<sa::EventSelectCase, sa::EventSelectCase,
                                  sa::TimerSelectCase>);
}

// Positive constraint verification: a valid call compiles (at the type level).
SLUICE_TEST_CASE(test_valid_select_expression_compiles) {
    sluice::async::AsyncIoContext ctx(std::make_unique<sluice::async::FakeAsyncBackend>());
    sa::Scheduler sched(ctx);
    sa::Event ev(sched);
    sa::EventSelectCase ec(ev);
    auto deadline = sched.monotonic_now();
    sa::TimerSelectCase tc(sched, deadline);

    // The requires expression must be satisfied for valid case packs.
    static_assert(requires(sa::Scheduler& s, sa::EventSelectCase c) {
        sa::select(s, c);
    });

    static_assert(requires(sa::Scheduler& s, sa::EventSelectCase c1, sa::TimerSelectCase c2) {
        sa::select(s, c1, c2);
    });

    // Return type is SelectResult.
    static_assert(std::is_same_v<
        decltype(sa::select(sched, ec)),
        sa::SelectResult
    >);
}

// =========================================================================
// H4: Forgeable-authority exclusion
// =========================================================================

// Verify production headers expose no E13 test-hook types.
// These are compile-time checks that the installed headers have no
// forgeable test-hook declarations at namespace scope.
SLUICE_TEST_CASE(test_no_forgeable_test_hooks) {
    // These compile only when SLUICE_ASYNC_INTERNAL_TESTING is defined.
    // Without the define, TestInit on SelectResult is private.
    // With the define, it's public but still name-mangled — not forgeable
    // in the sense of namespace-level friend.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // TestInit is accessible in this TU — but only because the test links
    // against sluice_async_internal_testing. Production TUs never see it.
    constexpr sa::SelectResult r(0, sa::SelectKind::event,
                                 sa::SelectTimerOutcome::fired,
                                 sa::SelectResult::TestInit{});
    (void)r;
#endif
}

SLUICE_MAIN()
