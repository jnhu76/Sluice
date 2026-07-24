// sluice::async::Scheduler — E13 P4/P5/P6 Select central claim, finalization,
// unified publication authority, and admission core (inline + suspended).
//
// This translation unit owns:
//   - the SINGLE group processor (select_process_group_locked), the preflight
//     validator, the winner-commit and loser-finalize drivers, and the reusable
//     all-authority-closed invariant predicate (P4);
//   - the SINGLE unified publication authority (select_publish_locked, P6),
//     which writes result_ exactly once and publishes exactly one runnable on
//     the suspended branch;
//   - the single non-template admission core (select_admit, P5 inline + P6
//     suspended), reached ONLY via the public variadic select() template.
//
// P6 scope (docs/e13-select-production-test-plan.md §7.6,
//           docs/e13-select-locking-and-publication.md §3/§5,
//           docs/e13-select-formal-production-mapping.md §5):
//   - exactly one winner linearization point (SelectGroup::claim_winner_locked)
//   - exactly one result-publication function (select_publish_locked) that
//     writes result_ exactly once and (suspended branch) publishes exactly one
//     runnable via the single make_runnable + route_runnable_locked call site
//   - the no-ready admission branch commits the caller to Waiting + phase Armed
//     and suspends; on resume it consumes the published result (Completed ->
//     Consumed) under global_mtx_
//   - waiting_select_count_ accounting for Event-only suspended Select
//     liveness (classify_locked + external_wake_possible_locked)
//
// The per-kind finalizer halves live in select_event.cpp (Event winner/loser)
// and select_timer.cpp (Timer winner/loser); the single group processor calls
// into them and owns the iteration over winner and losers. The single
// publication function calls NEITHER a finalizer NOR wake_wait_one_locked.
#include <sluice/async/scheduler.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <list>
#include <stdexcept>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/select.hpp>  // kSelectMaxArms (for the drift static_assert below only)

// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the internal-testing variant pulls in
// the non-installed test-control header so the admission phase call sites below
// resolve to the controller. In the production build this include is absent and
// the seam call sites compile to nothing.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {

namespace {
// Upper bound on Select arms (mirrors the public kSelectMaxArms gate).
// Defined locally so this TU's preflight does not depend on select.hpp for its
// value. The static_assert below ties the mirror to the authoritative constant
// so the two cannot silently drift.
constexpr std::size_t kPreflightMaxArms = 8;
static_assert(kPreflightMaxArms >= 1, "Select must permit at least one arm");
static_assert(kPreflightMaxArms == kSelectMaxArms,
              "kPreflightMaxArms must match the public kSelectMaxArms gate");
}  // namespace

// ---------------------------------------------------------------------------
// Preflight: validate the ENTIRE group before the irreversible winner CAS.
//
// Two-phase structure (P4 §6, §3):
//   Phase A (select_preflight_shape_locked): structural + identity asserts that
//     must ALWAYS hold for any process call on this group (scheduler binding,
//     arms presence, count bounds, candidate-index range, phase). These assert
//     for every call — including a claim-lost second attempt.
//   Then the caller checks winner() != kNoWinner and returns claim-lost WITHOUT
//     mutation (NOT an assert) before the deeper preflight.
//   Phase B (select_preflight_claim_locked): candidate-state + every-arm-state +
//     Event-membership + Timer-ownership asserts that are only meaningful for a
//     FRESH claim (winner == kNoWinner). These assert only on a real claim
//     attempt.
//
// This split makes a claim-lost second attempt return false cleanly (it is a
// valid concurrent/racing outcome) while every genuinely malformed group still
// asserts before the CAS. No allocation in this path.
// ---------------------------------------------------------------------------
void Scheduler::select_preflight_shape_locked(
    detail::SelectGroup& group, std::uint32_t candidate_index) const {
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

    [[maybe_unused]] const auto phase = group.phase();
    assert((phase == detail::GroupPhase::selecting ||
            phase == detail::GroupPhase::armed) &&
           "select_process_group_locked: group phase must be Selecting or Armed");
}

void Scheduler::select_preflight_claim_locked(
    detail::SelectGroup& group, std::uint32_t candidate_index) const {
    // --- candidate arm checks ---
    [[maybe_unused]] detail::SelectArmSlot& candidate =
        group.arms_[candidate_index];
    assert(candidate.group == &group &&
           "select_process_group_locked: candidate arm.group != &group");
    assert(candidate.state == detail::ArmState::candidate_ready &&
           "select_process_group_locked: candidate arm not CandidateReady");

    // --- every arm checks ---
    for (std::size_t i = 0; i < group.arm_count_; ++i) {
        [[maybe_unused]] detail::SelectArmSlot& arm = group.arms_[i];
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
        [[maybe_unused]] detail::SelectTimerRegistration* reg =
            arm.timer.stable_reg_;
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
    // Phase A: structural + identity shape (asserts for every call).
    select_preflight_shape_locked(group, candidate_index);

    // If another invocation already owns the winner, this invocation performs
    // NO mutation and returns claim-lost (P4 §6: "winner == kNoWinner at entry,
    // or return claim-lost without mutation"). This is a non-asserting return —
    // a racing/sequential second claim is a valid claim-lost outcome. Checked
    // AFTER shape validation but BEFORE the deeper per-arm preflight (a
    // finalized group has Retired arms that are only meaningful to validate on
    // a fresh claim).
    if (group.winner() != detail::kNoWinner) {
        return false;
    }

    // Phase B: candidate-state + every-arm + Event-membership + Timer-ownership
    // preflight (asserts, fresh-claim only).
    select_preflight_claim_locked(group, candidate_index);

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
    // E13 P5 AdmissionClaimed seam: AFTER the winner CAS succeeds (fresh
    // claim), BEFORE winner/loser finalization. Fires only on a real won claim.
    // Pure test-only observation (no allocation, no callback, no production
    // effect): it marks the phase reached and blocks ONLY if a controller is
    // registered AND armed; in the production target (and in any test that does
    // not arm it) it is absent entirely. Does not change P4 finalization order.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::select_admission_claimed);
#endif
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
//
// In P4 this is called in the same critical section immediately AFTER a
// successful preflight+claim+finalize, so it can assume group.arms_ is valid,
// kind is Event/Timer, and Timer regs have passed scheduler/pool/backpointer
// preflight. select_publish_locked also calls this at its entry (P6 §8.1), and
// for that path the publication preflight validates basic group shape first.
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
// ===========================================================================
// E13 P6 — unified Select publication authority.
//
// select_publish_locked is THE single result/runnable publication function
// (docs/e13-select-locking-and-publication.md §5, task §8). It is called after
// a successful select_process_group_locked (winner claimed + every winner/loser
// finalized + every adapter authority closed) in the SAME global_mtx_ critical
// section. There is exactly ONE call site each for Fiber::make_runnable and
// Scheduler::route_runnable_locked inside Select, both here, both only on the
// suspended branch. No Event/Timer finalizer/scan/pump reaches them.
// ===========================================================================
void Scheduler::select_publish_locked(detail::SelectGroup& group) {
    // -------------------------------------------------------------------
    // P6 §8.1 Release-active publication preflight. Do not rely only on
    // assert: mechanically validate the publication-entry shape and fail fast
    // on any violation. This closes the P4-deferred publication-entry shape
    // validation.
    // -------------------------------------------------------------------
    // scheduler identity / arm array / arm count
    if (group.scheduler_ != this ||
        group.arms_ == nullptr ||
        group.arm_count_ < 1 ||
        group.arm_count_ > kPreflightMaxArms) {
        detail::select_invariant_fail_fast();
    }
    // winner exists and is in range
    const std::uint32_t winner_index = group.winner();
    if (winner_index == detail::kNoWinner || winner_index >= group.arm_count_) {
        detail::select_invariant_fail_fast();
    }
    const detail::GroupPhase phase = group.phase();
    // phase must be Selecting (inline) or Armed (suspended) — NOT
    // Completed/Consumed (duplicate publication).
    if (phase != detail::GroupPhase::selecting &&
        phase != detail::GroupPhase::armed) {
        detail::select_invariant_fail_fast();
    }
    // completion mode must be None at entry (publication sets it exactly once).
    if (group.completion_mode_ != detail::CompletionMode::none) {
        detail::select_invariant_fail_fast();
    }
    // result sentinel: result_ must not yet have a winner (written exactly
    // once — a duplicate publication with an existing winner fails here BEFORE
    // any rewrite / count decrement / make_runnable / route, satisfying SN-2).
    if (group.result_.has_winner()) {
        detail::select_invariant_fail_fast();
    }
    // every arm kind valid; winner kind valid.
    for (std::size_t i = 0; i < group.arm_count_; ++i) {
        const detail::SelectArmSlot& arm = group.arms_[i];
        if (arm.kind != detail::ArmKind::event &&
            arm.kind != detail::ArmKind::timer) {
            detail::select_invariant_fail_fast();
        }
    }
    const detail::SelectArmSlot& winner_arm = group.arms_[winner_index];
    if (winner_arm.kind != detail::ArmKind::event &&
        winner_arm.kind != detail::ArmKind::timer) {
        detail::select_invariant_fail_fast();
    }
    // authority-closed predicate (SN-10: an open authority fails fast BEFORE
    // the result write). This also re-derives that every arm is Retired, the
    // winner's Timer registration is CONSUMED, loser Timer regs are RETIRED,
    // and every Event arm is unlinked.
    if (!select_all_authority_closed_locked(group)) {
        detail::select_invariant_fail_fast();
    }

    // For suspended publication the caller + caller_owner must be set and the
    // caller Fiber must be Waiting at this instant (it committed Waiting under
    // G before releasing G to context_switch; the only legal transition out of
    // Waiting for a suspended Select is make_runnable below).
    const bool suspended = (phase == detail::GroupPhase::armed);
    if (suspended) {
        if (group.caller_ == nullptr || group.caller_owner_ == nullptr) {
            detail::select_invariant_fail_fast();
        }
        if (group.caller_->state() != FiberState::waiting) {
            // FP: suspended caller not Waiting (Running/Runnable/done) — a
            // corrupt state machine / duplicate publication / second wake path.
            // Fail fast BEFORE routing.
            detail::select_invariant_fail_fast();
        }
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // P6 §14 select_publish_entry seam: at select_publish_locked entry, before
    // result mutation. Capture the publication-entry snapshot under global_mtx_
    // so the test reads publication preconditions without acquiring G.
    {
        sluice_async_test::PublicationSnapshot snap{};
        snap.phase = group.phase();
        snap.completion_mode = group.completion_mode_;
        snap.winner = group.winner();
        snap.arm_count = group.arm_count_;
        snap.result_has_winner = group.result_.has_winner();
        snap.result_index = group.result_.has_winner() ? group.result_.index() : 0;
        snap.result_kind =
            group.result_.has_winner() ? group.result_.kind() : SelectKind::event;
        snap.all_authority_closed = select_all_authority_closed_locked(group);
        snap.caller_state = group.caller_ ? group.caller_->state() : FiberState::created;
        snap.caller_owner_id = group.caller_owner_ ? group.caller_owner_->id : 0;
        snap.waiting_select_count = waiting_select_count_;
        snap.result_publication_count =
            sluice_async_test::result_publication_count(*this);
        snap.runnable_publication_count =
            sluice_async_test::runnable_publication_count(*this);
        sluice_async_test::capture_publication_snapshot(
            *this, sluice_async_test::PhaseTag::select_publish_entry, snap);
    }
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::select_publish_entry);
#endif

    // -------------------------------------------------------------------
    // P6 §8.2 Result construction from the winner arm. Required source order:
    //   all authority closed
    //   < group.result_ write
    //   < completion mode
    //   < phase Completed
    //   < runnable publication
    // (all authority closed was validated above.)
    // -------------------------------------------------------------------
    SelectResult result;
    if (winner_arm.kind == detail::ArmKind::event) {
        result = SelectResult(winner_index, SelectKind::event,
                              SelectTimerOutcome::fired);
    } else {
        result = SelectResult(winner_index, SelectKind::timer,
                              SelectTimerOutcome::fired);
    }
    group.result_ = result;  // written EXACTLY once (the has_winner check above
                             // guards against a rewrite).

    if (suspended) {
        // ----- Suspended branch (P6 §8.4) -----
        group.completion_mode_ = detail::CompletionMode::suspended;
        group.set_phase(detail::GroupPhase::completed);

        // Decrement waiting_select_count_ exactly once. A duplicate
        // publication cannot reach here (the has_winner / completion_mode
        // checks above fail fast first). Guard against underflow.
        if (waiting_select_count_ == 0) {
            detail::select_invariant_fail_fast();
        }
        --waiting_select_count_;

        // make_runnable is the exactly-once runnable publication guard. On the
        // suspended branch a false return means the caller's state machine is
        // corrupt (not Waiting — already validated above as a precondition, so
        // a false return is unreachable after the FP check, but the CAS is the
        // authority), there is a duplicate publication, or a second wake path
        // exists. Fail fast rather than silently returning.
        const bool published = group.caller_->make_runnable();
        if (!published) {
            detail::select_invariant_fail_fast();
        }
        // The single Select route_runnable_locked call site. ALWAYS route via
        // the stored group.caller_owner_ — NEVER the resolver-thread g_worker
        // (which is null on an external thread).
        route_runnable_locked(group.caller_, group.caller_owner_);

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::increment_result_publication(*this);
        sluice_async_test::increment_runnable_publication(*this);
        // P6 §14 select_publish_done seam: after phase Completed and, for
        // suspended mode, after successful make_runnable + route.
        {
            sluice_async_test::PublicationSnapshot snap{};
            snap.phase = group.phase();
            snap.completion_mode = group.completion_mode_;
            snap.winner = group.winner();
            snap.arm_count = group.arm_count_;
            snap.result_has_winner = group.result_.has_winner();
            snap.result_index = group.result_.has_winner() ? group.result_.index() : 0;
            snap.result_kind =
                group.result_.has_winner() ? group.result_.kind() : SelectKind::event;
            snap.all_authority_closed = select_all_authority_closed_locked(group);
            snap.caller_state = group.caller_->state();
            snap.caller_owner_id = group.caller_owner_->id;
            snap.waiting_select_count = waiting_select_count_;
            snap.result_publication_count =
                sluice_async_test::result_publication_count(*this);
            snap.runnable_publication_count =
                sluice_async_test::runnable_publication_count(*this);
            sluice_async_test::capture_publication_snapshot(
                *this, sluice_async_test::PhaseTag::select_publish_done, snap);
        }
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::select_publish_done);
#endif
    } else {
        // ----- Inline branch (P6 §8.3) -----
        // NO make_runnable, NO route_runnable_locked, NO waiting_select_count
        // mutation. The admission caller copies result_, sets Consumed, returns.
        group.completion_mode_ = detail::CompletionMode::inline_;
        group.set_phase(detail::GroupPhase::completed);

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::increment_result_publication(*this);
        // (runnable_publication_count unchanged on the inline branch.)
        {
            sluice_async_test::PublicationSnapshot snap{};
            snap.phase = group.phase();
            snap.completion_mode = group.completion_mode_;
            snap.winner = group.winner();
            snap.arm_count = group.arm_count_;
            snap.result_has_winner = group.result_.has_winner();
            snap.result_index = group.result_.has_winner() ? group.result_.index() : 0;
            snap.result_kind =
                group.result_.has_winner() ? group.result_.kind() : SelectKind::event;
            snap.all_authority_closed = select_all_authority_closed_locked(group);
            snap.caller_state = group.caller_ ? group.caller_->state() : FiberState::running;
            snap.caller_owner_id = group.caller_owner_ ? group.caller_owner_->id : 0;
            snap.waiting_select_count = waiting_select_count_;
            snap.result_publication_count =
                sluice_async_test::result_publication_count(*this);
            snap.runnable_publication_count =
                sluice_async_test::runnable_publication_count(*this);
            sluice_async_test::capture_publication_snapshot(
                *this, sluice_async_test::PhaseTag::select_publish_done, snap);
        }
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::select_publish_done);
#endif
    }
}

// ===========================================================================
// E13 P6 — suspended Event / Timer resolution.
//
// select_resolve_event_locked / select_resolve_timer_locked are the
// post-suspension resolvers, reached ONLY under global_mtx_ from
// event_set_broadcast (Event) and the timer pump (Timer). Each performs the
// single-group P8 gate (Event) or ACTIVE validation (Timer), offers
// CandidateReady to the relevant arm(s), drives the single P4 group processor
// exactly once, then select_publish_locked exactly once. No arm finalizer
// calls make_runnable / route_runnable_locked.
// ===========================================================================

// Suspended Event resolver (task §11). Walks this Event's SelectPort for
// eligible arms, applies the single-group P6 gate, marks every eligible arm
// CandidateReady, chooses the lowest INDEX ready arm, processes + publishes
// the group once. Returns true iff a group was published.
bool Scheduler::select_resolve_event_locked(Event& event) {
    assert(&event.scheduler_ == this &&
           "select_resolve_event_locked: Event does not belong to this Scheduler");

    // ---- P6 §11.1 single-group gate: collect eligible arms + distinct groups
    // for THIS Event only. ----
    detail::SelectArmSlot* arms_buf[kPreflightMaxArms] = {};
    std::size_t eligible_count = 0;
    detail::SelectGroup* distinct_groups[kPreflightMaxArms] = {};
    std::size_t distinct_group_count = 0;

    for (detail::SelectArmSlot* arm = event.select_port_.head_;
         arm != nullptr; arm = arm->next_) {
        // Eligible: kind==Event, state==Registered, group!=nullptr,
        // group.phase==Armed, home points to this Event's port,
        // arm Event pointer == this Event.
        if (arm->kind != detail::ArmKind::event) continue;
        if (arm->state != detail::ArmState::registered) continue;
        if (arm->group == nullptr) continue;
        if (arm->group->phase() != detail::GroupPhase::armed) continue;
        if (arm->home_ != &event.select_port_) continue;
        if (arm->event.event_ != &event) continue;

        if (eligible_count < kPreflightMaxArms) {
            arms_buf[eligible_count++] = arm;
        }
        // distinct-group accounting for the P8 gate (one Event may legitimately
        // reach arms in multiple groups — P8 is DENIED at the P6 boundary).
        bool known = false;
        for (std::size_t g = 0; g < distinct_group_count; ++g) {
            if (distinct_groups[g] == arm->group) { known = true; break; }
        }
        if (!known && distinct_group_count < kPreflightMaxArms) {
            distinct_groups[distinct_group_count++] = arm->group;
        }
    }

    if (eligible_count == 0) {
        return false;  // nothing eligible for this Event
    }

    // P8 stage-boundary gate: more than one distinct eligible group sharing one
    // Event is DENIED at P6. Fail fast BEFORE any candidate mutation or winner
    // CAS. (One group with several arms on the same Event is NOT this case and
    // proceeds — the same-Event-twice support.)
    if (distinct_group_count > 1) {
        detail::select_multi_group_event_stage_fail_fast();
    }

    detail::SelectGroup* const group = distinct_groups[0];

    // Mark every eligible arm CandidateReady. (For same-Event-twice-in-one-
    // group this marks both arms; the lowest-index scan below chooses one.)
    for (std::size_t i = 0; i < eligible_count; ++i) {
        arms_buf[i]->state = detail::ArmState::candidate_ready;
    }

    // Choose the candidate by scanning group.arms_[0..arm_count) in INDEX ORDER
    // and choosing the first CandidateReady arm belonging to this Event. Do NOT
    // use intrusive-list order as the tie-break (task §11.2).
    std::uint32_t candidate_index = static_cast<std::uint32_t>(-1);
    for (std::size_t i = 0; i < group->arm_count_; ++i) {
        detail::SelectArmSlot& arm = group->arms_[i];
        if (arm.state != detail::ArmState::candidate_ready) continue;
        if (arm.kind != detail::ArmKind::event) continue;
        if (arm.event.event_ != &event) continue;
        candidate_index = static_cast<std::uint32_t>(i);
        break;
    }
    // A candidate must exist: at least one eligible Event arm was made
    // CandidateReady in this group.
    if (candidate_index >= group->arm_count_) {
        detail::select_invariant_fail_fast();
    }

    // Process the group exactly once (P4 claim + finalize winner + losers +
    // close every authority), then publish exactly once. select_process_group_
    // locked returns claim-lost (false) only if another invocation already won
    // — unreachable under one held global_mtx_ unless a racing Timer/Event
    // already resolved this group; in that case the group is already published
    // (or being published in this same CS), so do NOT publish a second time.
    const bool won = select_process_group_locked(*group, candidate_index);
    if (!won) {
        // Already claimed (a concurrent resolver in this same CS won). The
        // winner's publication is responsible; this call publishes nothing.
        return false;
    }
    select_publish_locked(*group);
    return true;
}

// Suspended Timer resolver (task §12). Replaces the P3 due-ACTIVE stage
// fail-fast with the real P6 resolver. PRE: `reg` is ACTIVE and the pump has
// popped it (the caller validates ACTIVE; a non-ACTIVE block takes the stale
// skip path in select_timer_pump_entry_locked, which never reaches here).
bool Scheduler::select_resolve_timer_locked(
    detail::SelectTimerRegistration& reg) {
    // ACTIVE validation per task §12.1.
    if (reg.scheduler() != this) {
        detail::select_invariant_fail_fast();
    }
    if (!reg.is_active()) {
        // Not ACTIVE: stale. The pump's state-before-arm rule already handles
        // the stale skip without reading arm_; reaching here non-ACTIVE is an
        // invariant violation by the caller.
        detail::select_invariant_fail_fast();
    }
    detail::SelectArmSlot* const arm = reg.arm();
    if (arm == nullptr) {
        detail::select_invariant_fail_fast();
    }
    if (arm->kind != detail::ArmKind::timer) {
        detail::select_invariant_fail_fast();
    }
    if (arm->state != detail::ArmState::registered) {
        detail::select_invariant_fail_fast();
    }
    detail::SelectGroup* const group = arm->group;
    if (group == nullptr) {
        detail::select_invariant_fail_fast();
    }
    if (group->scheduler_ != this) {
        detail::select_invariant_fail_fast();
    }
    if (group->phase() != detail::GroupPhase::armed) {
        detail::select_invariant_fail_fast();
    }
    if (reg.arm() != arm) {
        detail::select_invariant_fail_fast();
    }
    if (!pool_owns_select_block_locked(reg)) {
        detail::select_invariant_fail_fast();
    }

    // Find the exact arm index by scanning group.arms_ for ADDRESS EQUALITY.
    // Do not perform pointer subtraction unless same-array provenance is
    // mechanically established (task §12.1).
    std::uint32_t candidate_index = static_cast<std::uint32_t>(-1);
    for (std::size_t i = 0; i < group->arm_count_; ++i) {
        if (&group->arms_[i] == arm) {
            candidate_index = static_cast<std::uint32_t>(i);
            break;
        }
    }
    if (candidate_index >= group->arm_count_) {
        detail::select_invariant_fail_fast();
    }

    // The Timer registration is NOT consumed before the group winner CAS. P4
    // (select_process_group_locked -> select_finalize_timer_winner_locked)
    // owns the winner/loser finalizer, which CONSUMES the winner registration
    // and RETIRES every loser registration.
    arm->state = detail::ArmState::candidate_ready;

    const bool won = select_process_group_locked(*group, candidate_index);
    if (!won) {
        // Already claimed (e.g. an Event won in a prior resolver call in this
        // same CS, retiring this Timer registration). The winner's publication
        // is responsible; this call publishes nothing.
        return false;
    }
    select_publish_locked(*group);
    return true;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
// Read a SelectGroup's stored result_ (the post-publication winner). Defined
// out-of-line in the internal-testing variant only (it is a guarded test
// accessor; absent in the production target).
SelectResult Scheduler::AsyncTestAccess::group_result(
    const Scheduler& /*s*/, const detail::SelectGroup& group) {
    return group.result_;
}
#endif

// ===========================================================================
// E13 P5/P6 — registration + admission core (inline + suspended).
//
// select_admit is the single non-template admission core reached ONLY by the
// public variadic select() bridge (include/sluice/async/select.hpp). It owns
// every centralized admission step. Both outcomes route through this one core:
//   - validate caller + every case Scheduler identity BEFORE any allocation
//   - materialize fixed caller-frame group + arms (no heap)
//   - allocate every Timer stable block BEFORE global_mtx_
//   - reserve deadline-heap capacity BEFORE the first registration mutation
//   - register every arm under ONE continuous global_mtx_ critical section
//   - FinishRegistration (phase = Selecting)
//   - take ONE immutable readiness snapshot (single captured monotonic_now)
//   - select the lowest ready index (only admission tie-break)
//
//   Inline-ready branch:
//     - call the P4 central claim/finalization core exactly once
//     - call the unified publication authority (Inline completion)
//     - copy result_, set phase Consumed, return WITHOUT suspending or
//       publishing a runnable
//
//   No-ready branch (P6):
//     - commit the caller Fiber to Waiting + phase Armed +
//       waiting_select_count_++ (atomic-under-G lost-wake closure)
//     - release G, context_switch to the owner scheduler context
//     - on resume: reacquire G, validate Completed/Suspended/result, copy
//       result_, set phase Consumed, return
//
// Locking: exactly one global_mtx_ acquisition spans reserve..inline-completion
// (inline branch) or reserve..suspension-commit (suspended branch, then G is
// released before context_switch). No external Event::set / timer pump / other
// Select path may observe a partially-registered group (docs/e13-select-
// locking-and-publication.md §3.3).
//
// Caller-frame storage: SelectGroup + the SelectArmSlot array live in THIS
// function's frame. arm.home_/next_/prev_ point into them; every Event/Timer
// authority is closed before the caller is resumed (suspended) or before this
// frame returns (inline), so no dangling reference survives the call
// (docs/e13-select-production-architecture.md §4.1).
// ===========================================================================
SelectResult Scheduler::select_admit(detail::SelectCaseDescriptor* descs,
                                     std::size_t count) {
    // Release-mode defense-in-depth: reject structurally invalid arguments that
    // would cause a fixed-array out-of-bounds access. This protects against a
    // hypothetical non-friend caller that bypasses the public select() template's
    // compile-time requires clause gate, or a corrupted descriptor pointer.
    assert(descs != nullptr && count >= 1 && count <= kPreflightMaxArms &&
           "select_admit: descs/count out of range (requires clause gate)");
    if (descs == nullptr || count == 0 || count > kSelectMaxArms) {
        detail::select_invariant_fail_fast();
    }

    // -----------------------------------------------------------------------
    // (1) Caller validation — BEFORE any allocation/registration.
    // docs/e13-select-public-api.md §4.11.
    // -----------------------------------------------------------------------
    WorkerState* ws = current_worker();          // == g_worker
    if (ws == nullptr) {
        throw std::logic_error(
            "select() called from a plain OS thread, not a Scheduler worker");
    }
    if (ws->owner_scheduler != this) {
        throw std::logic_error(
            "select() called on a Scheduler that does not own this worker");
    }
    if (ws->current == nullptr) {
        throw std::logic_error("select() called with no current Fiber");
    }
    // Capture caller + owner for the publication path (inline + suspended).
    // Stored on the group; the suspended branch routes the resumed caller via
    // caller_owner_ (NEVER the resolver-thread g_worker).
    Fiber* const caller = ws->current;
    WorkerState* const caller_owner = ws;

    // -----------------------------------------------------------------------
    // (2) Case Scheduler validation — BEFORE any allocation.
    // Cross-Scheduler Event / mismatched Timer case -> std::invalid_argument,
    // thrown before Timer stable-block allocation, heap reserve, or link/splice.
    // -----------------------------------------------------------------------
    for (std::size_t i = 0; i < count; ++i) {
        const detail::SelectCaseDescriptor& d = descs[i];
        switch (d.kind_) {
        case detail::SelectCaseDescriptor::Kind::event: {
            assert(d.event_ != nullptr &&
                   "select_admit: Event descriptor event_ is null");
            if (&d.event_->scheduler_ != this) {
                throw std::invalid_argument(
                    "select(): Event does not belong to this Scheduler");
            }
            break;
        }
        case detail::SelectCaseDescriptor::Kind::timer:
            if (d.scheduler_ != this) {
                throw std::invalid_argument(
                    "select(): Timer case Scheduler does not match select() "
                    "Scheduler argument");
            }
            break;
        default:
            detail::select_invariant_fail_fast();
        }
    }

    // -----------------------------------------------------------------------
    // (3) Materialize fixed caller-frame group + arms. No dynamic allocation.
    // The arms array is sized by count (1..kSelectMaxArms); it lives in this
    // frame and is destroyed when the frame returns (after authority closure).
    // -----------------------------------------------------------------------
    detail::SelectGroup group;                    // caller-frame control block
    group.scheduler_ = this;
    group.caller_ = caller;
    group.caller_owner_ = caller_owner;
    group.completion_mode_ = detail::CompletionMode::none;
    group.set_phase(detail::GroupPhase::building);
    // group.winner_ defaults to kNoWinner (SelectGroup's default init); the
    // single winner CAS happens later via select_process_group_locked.

    std::array<detail::SelectArmSlot, kPreflightMaxArms> arms;
    group.arms_ = arms.data();
    group.arm_count_ = count;

    // (4) Build the Timer tmp_pool (caller-frame temporary) with one ACTIVE node
    // per Timer arm, BEFORE global_mtx_. std::list nodes are pointer-stable;
    // std::list::splice preserves their address after transfer to the Scheduler
    // pool (C++ [list.ops]). Bind arm.timer.stable_reg_ here so the registration
    // loop only splices (no allocation under the lock).
    std::list<detail::SelectTimerRegistration> timer_tmp_pool;
    std::size_t timer_arm_count = 0;

    for (std::size_t i = 0; i < count; ++i) {
        detail::SelectArmSlot& arm = arms[i];
        detail::SelectCaseDescriptor& d = descs[i];
        arm.group = &group;
        arm.state = detail::ArmState::prepared;
        switch (d.kind_) {
        case detail::SelectCaseDescriptor::Kind::event:
            arm.construct_event(*d.event_);
            break;
        case detail::SelectCaseDescriptor::Kind::timer: {
            arm.construct_timer(d.deadline_);
            timer_tmp_pool.emplace_back(&arm, this,
                                        static_cast<deadline_tick_t>(d.deadline_));
            detail::SelectTimerRegistration& node = timer_tmp_pool.back();
            arm.timer.stable_reg_ = &node;
            ++timer_arm_count;
            break;
        }
        default:
            detail::select_invariant_fail_fast();
        }
    }
    // NOTE: mark_admitted() is deferred until AFTER the deadline-heap reserve
    // succeeds (inside the CS below). Per §9.1 a reserve throw must leave "no
    // group admission marker"; if we marked admitted here and reserve then threw,
    // the caller-frame group would unwind in the Building phase and its
    // destructor would enforce the terminal-phase contract (Consumed/Aborted) —
    // turning a clean std::bad_alloc into a std::terminate. The admission marker
    // is only set once the registration transaction is committed (reserve ok).

    // -----------------------------------------------------------------------
    // (5)-(10) Registration transaction under ONE global_mtx_ critical section.
    // -----------------------------------------------------------------------
    {
        LockGuard lk(global_mtx_);

        // (5) Reserve deadline-heap capacity for ALL Timer arms BEFORE any
        // registration mutation (docs/e13-select-public-api.md §5). This is the
        // ONLY allocation permitted under the lock. Checked overflow guard.
        if (timer_arm_count > deadline_heap_.max_size() - deadline_heap_.size()) {
            throw std::length_error(
                "select(): deadline heap capacity overflow on reserve");
        }
        deadline_heap_.reserve(deadline_heap_.size() + timer_arm_count);
        // If reserve threw above: no group admission marker was set, no arm
        // registered, no splice. The exception propagates; the caller-frame
        // group is NOT admitted (destructor enforces no terminal-phase contract)
        // and tmp_pool unwinds cleanly. Nothing to roll back.

        // The registration transaction is now committed (reserve ok). Mark the
        // group admitted: from here the destructor enforces Consumed/Aborted,
        // and the path to a terminal phase is noexcept (or fail-fast, which
        // terminates) — §10: no ordinary throwing operation after reserve.
        group.mark_admitted();

        // (6) Register every arm in index order, wrapped in the P7 registration
        // rollback transaction. registered_count is the authoritative size of the
        // fully committed registered prefix [0, registered_count): it is
        // incremented only AFTER one arm has completed ALL registration effects
        // (Event: linked + Registered; Timer: spliced into the Scheduler pool +
        // heap entry pushed + active_deadline_count_ incremented + Registered).
        // The synthetic failure checkpoint may run only between fully committed
        // arm registrations — never halfway through one arm's commit.
        //
        // Per §17, no ordinary throwing operation exists in this loop after the
        // reserve (select_event_link_locked, select_timer_splice_one_locked, the
        // heap push within reserved capacity, and active_deadline_count_ ++ are
        // all allocation-free). The catch is nevertheless REAL and future-safe:
        // any exception escaping the prefix is rolled back so no Scheduler-
        // visible registration authority outlives the Building group. It is also
        // the path exercised by the test-only synthetic failure seam (§18).
        std::size_t registered_count = 0;
        auto tmp_it = timer_tmp_pool.begin();
        try {
            for (std::size_t i = 0; i < count; ++i) {
                detail::SelectArmSlot& arm = arms[i];

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                // P7 §18: synthetic failure-injection seam, checked at the START
                // of each iteration (BEFORE this arm's commit). The boundary
                // fail_after means "inject after exactly fail_after successful
                // registrations, before the next one". fail_after==0 injects
                // before the FIRST registration (no arm committed); fail_after==
                // arm_count injects AFTER the loop, before FinishRegistration.
                // The throw happens UNDER G, between fully committed arm
                // registrations — never mid-commit. Absent entirely in the
                // production target (no controller, no injection branch, no
                // SelectRegistrationFailure symbol).
                if (sluice_async_test::rollback_should_inject_after(
                        *this, registered_count)) {
                    throw sluice_async_test::SelectRegistrationFailure{};
                }
#endif
                if (arm.kind == detail::ArmKind::event) {
                    // Event arm: link into the Event's private SelectPort. This is
                    // the canonical registry mutation (select_event_link_locked),
                    // which sets arm.state = Registered. Distinct duplicate Event
                    // cases receive DISTINCT SelectArmSlot nodes (each is a
                    // separate array slot linked into the same port).
                    select_event_link_locked(*descs[i].event_, arm);
                } else {
                    // Timer arm: splice exactly one tmp_pool node into the
                    // Scheduler pool, push its tagged DeadlineHeapEntry,
                    // increment active_deadline_count_ exactly once. splice
                    // preserves the stable address bound at arm.timer.stable_reg_
                    // above. Capture the next tmp_pool iterator BEFORE splicing:
                    // splice transfers tmp_it's node out of tmp_pool into
                    // select_timer_pool_, so incrementing tmp_it afterwards
                    // would iterate the destination list, not tmp_pool.
                    auto next_tmp = std::next(tmp_it);
                    detail::SelectTimerRegistration* spliced =
                        select_timer_splice_one_locked(timer_tmp_pool, tmp_it);
                    tmp_it = next_tmp;
                    // The arm's stable_reg_ already points at `spliced` (same
                    // address); no rebinding needed.
                    (void)spliced;
                    arm.state = detail::ArmState::registered;
                }

                // This arm's full registration commit is complete. The prefix
                // [0, registered_count+1) is now committed.
                ++registered_count;
            }

            // After registration, tmp_pool is empty; every Timer arm's
            // stable_reg_ points into select_timer_pool_; every Event arm is
            // linked.

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            // P7 §18: the fail_after == arm_count boundary — inject AFTER all
            // arms are fully registered, immediately BEFORE FinishRegistration
            // (P7-T5: a fully-registered but still-Building group must roll
            // back). Absent in production.
            if (sluice_async_test::rollback_should_inject_after(
                    *this, registered_count)) {
                throw sluice_async_test::SelectRegistrationFailure{};
            }
#endif

            // (7) FinishRegistration.
            group.set_phase(detail::GroupPhase::selecting);
        } catch (...) {
            // P7 rollback transaction (§6.2). The catch executes while the
            // ORIGINAL global_mtx_ critical section is still held (this block
            // opened the LockGuard). registered_count is the exact committed
            // prefix size; the orchestrator rolls back [0, registered_count) in
            // reverse order, normalizes the suffix, and finishes to Aborted.
            // The orchestrator is noexcept (it never throws); it fail-fasts on
            // impossible corruption. The original exception is rethrown
            // UNCHANGED — no translation, no swallow (P7-ABORT-9/10).
            select_rollback_registration_locked(group, arms.data(), count,
                                                registered_count);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            // select_rollback_aborted seam: AFTER Aborted reached, BEFORE rethrow.
            // Proves rollback completed while G is held. Holds global_mtx_.
            sluice_async_test::test_phase(
                *this, sluice_async_test::PhaseTag::select_rollback_aborted);
#endif
            throw;
        }

        // FinishRegistration succeeded (no exception): the group is now
        // Selecting. registered_count == count (all arms committed).

        // (8) AdmissionArmed seam: AFTER every arm registered, AFTER phase
        // becomes Selecting, BEFORE the readiness snapshot.
        // P5 CORRECTIVE: capture the boundary snapshot before the seam so the
        // test can read the mechanical group/arm state without acquiring G.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        {
            sluice_async_test::AdmissionSnapshot snap{};
            snap.phase = group.phase();
            snap.completion_mode = group.completion_mode_;
            snap.winner = group.winner();
            snap.arm_count = group.arm_count_;
            snap.all_authority_closed = false;
            for (std::size_t si = 0; si < group.arm_count_; ++si) {
                snap.arm_states[si] = arms[si].state;
                snap.arm_kinds[si] = arms[si].kind;
                snap.event_linked[si] = (arms[si].home_ != nullptr);
                if (arms[si].kind == detail::ArmKind::timer) {
                    snap.timer_states[si] = arms[si].timer.stable_reg_
                        ? arms[si].timer.stable_reg_->state()
                        : detail::SelectTimerRegistration::State::consumed;
                } else {
                    snap.timer_states[si] = detail::SelectTimerRegistration::State::consumed;
                }
            }
            sluice_async_test::capture_admission_snapshot(
                *this, sluice_async_test::PhaseTag::select_admission_armed, snap);
        }
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::select_admission_armed);
#endif

        // (9) Immutable readiness snapshot. Capture monotonic_now() ONCE and
        // walk EVERY arm in index order — do NOT stop at the first ready arm,
        // do NOT let heap order or Event/Timer kind choose the winner. Mark
        // every ready arm CandidateReady; choose the LOWEST ready index.
        const deadline_t captured_now = monotonic_now();
        std::uint32_t lowest_ready = static_cast<std::uint32_t>(-1);
        bool any_ready = false;
        for (std::size_t i = 0; i < count; ++i) {
            detail::SelectArmSlot& arm = arms[i];
            bool ready = false;
            switch (arm.kind) {
            case detail::ArmKind::event:
                ready = descs[i].event_->set_.load(std::memory_order::acquire);
                break;
            case detail::ArmKind::timer:
                ready = arm.timer.stable_reg_->deadline() <= captured_now;
                break;
            default:
                detail::select_invariant_fail_fast();
            }
            if (ready) {
                arm.state = detail::ArmState::candidate_ready;
                if (!any_ready) {
                    lowest_ready = static_cast<std::uint32_t>(i);
                    any_ready = true;
                }
            }
        }

        if (any_ready) {
            // ----- Inline-ready branch (P5) -----
            // (11)-(12) P4 integration: claim + finalize the whole group for the
            // lowest ready index, EXACTLY ONCE. Because admission and processing
            // occur under the SAME global_mtx_ critical section, an inline
            // admission claim cannot legitimately lose; a false return is an
            // invariant violation.
            const bool won =
                select_process_group_locked(group, lowest_ready);
            if (!won) {
                // Unreachable: under one continuous CS, the winner CAS cannot
                // lose. A false return is an invariant violation.
                detail::select_invariant_fail_fast();
            }

            // (13) Inline completion via the unified publication authority
            // (PUB-3: inline publication regression). select_publish_locked
            // writes result_ + sets completion_mode_=Inline + phase=Completed.
            select_publish_locked(group);

            // Copy result_ to the local return value, then set phase Consumed.
            // AdmissionConsumed seam: AFTER phase becomes Completed, BEFORE
            // Consumed (the inline Completed->Consumed lifecycle).
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            {
                sluice_async_test::AdmissionSnapshot snap{};
                snap.phase = group.phase();
                snap.completion_mode = group.completion_mode_;
                snap.winner = group.winner();
                snap.arm_count = group.arm_count_;
                snap.all_authority_closed = select_all_authority_closed_locked(group);
                for (std::size_t si = 0; si < group.arm_count_; ++si) {
                    snap.arm_states[si] = arms[si].state;
                    snap.arm_kinds[si] = arms[si].kind;
                    snap.event_linked[si] = (arms[si].home_ != nullptr);
                    if (arms[si].kind == detail::ArmKind::timer) {
                        snap.timer_states[si] = arms[si].timer.stable_reg_
                            ? arms[si].timer.stable_reg_->state()
                            : detail::SelectTimerRegistration::State::consumed;
                    } else {
                        snap.timer_states[si] = detail::SelectTimerRegistration::State::consumed;
                    }
                }
                sluice_async_test::capture_admission_snapshot(
                    *this, sluice_async_test::PhaseTag::select_admission_consumed, snap);
            }
            sluice_async_test::test_phase(
                *this, sluice_async_test::PhaseTag::select_admission_consumed);
#endif

            SelectResult return_value = group.result_;
            group.set_phase(detail::GroupPhase::consumed);

            // (14) Unlock (LockGuard destructor) and return. NO make_runnable /
            // route_runnable_locked / make_waiting / context_switch on the
            // inline branch.
            return return_value;
        }

        // ----- No-ready branch (P6 §9): commit suspension -----
        // Preconditions (task §9): phase==Selecting, winner==kNoWinner, every
        // arm Registered, caller==current Fiber, caller_owner==g_worker. All
        // established by the registration transaction above (phase Selecting,
        // no winner, every arm Registered, caller captured at admission). The
        // lost-wake closure (task §9.1): make_waiting + phase Armed +
        // waiting_select_count_++ all under the SAME G critical section, so a
        // resolver that wins after G unlock but before context_switch sees the
        // committed Waiting state and queues the caller exactly once.
        caller->make_waiting();
        group.set_phase(detail::GroupPhase::armed);
        ++waiting_select_count_;
    }  // ---- global_mtx_ released here ----

    // P1-1 corrective (P6-C1 §9.1a): raise the suspend-switch execution
    // authority. A resolver that wins after this point commits the caller
    // Runnable under G and routes it onto caller_owner->local_runnable. Until
    // the physical context_switch below saves the caller's CPU context, that
    // routed ticket is NOT safe for a thief worker to resume (its ctx is
    // stale). try_steal observes suspend_switch_pending and refuses the steal.
    // Set BEFORE the seam so a multi-worker test observer parked at the seam
    // also sees the raised authority. release store pairs with try_steal's
    // acquire load under global_mtx_.
    caller_owner->suspend_switch_pending.store(true, std::memory_order_release);

    // (P6 §9) select_suspend_before_switch seam: AFTER G is released,
    // BEFORE the physical context_switch. A coordinator thread can resolve the
    // group (Event::set / clock advance) here and prove the wake-before-
    // physical-switch window is closed: the caller is committed Waiting +
    // Armed + accounted under G, so a resolver's make_runnable sees Waiting and
    // queues the caller exactly once even before this thread switches away.
    // No wall-clock sleeps.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::select_suspend_before_switch);
#endif

    fiber_ctx::Switch s;
    s.old = &caller->ctx;
    s.new_ = &caller_owner->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    // ---- Control resumes here when a resolver publishes + routes the caller ----

    // P1-1 corrective (P6-C1 §9.1b): the physical context_switch has returned
    // control to the scheduler continuation, so the caller's CPU context is now
    // saved. Drop the suspend authority UNCONDITIONALLY (the switch completed
    // regardless of whether a resolver raced it). This must run BEFORE the
    // resume validation block reacquires G, and BEFORE any subsequent suspend
    // cycle of this worker would re-raise it. A future steal of this fiber (on
    // a later suspend cycle) is no longer blocked.
    caller_owner->suspend_switch_pending.store(false, std::memory_order_release);

    // (P6 §10) Resume + ConsumeResult path. Reacquire global_mtx_ and validate
    // the published result before reading it.
    {
        LockGuard lk(global_mtx_);

        // Require the publication committed the suspended lifecycle. NOTE: the
        // resumed caller does NOT re-derive select_all_authority_closed_locked
        // here: that predicate dereferences Scheduler-owned Timer registrations
        // (arm.timer.stable_reg_), which the timer pump may LAZILY RECLAIM
        // (pop+erase) between publication and resume once their deadline has
        // elapsed (docs/e13-select-timer-adapter.md §6.1 lazy-at-deadline
        // reclamation). The publication authority already closed all authority
        // BEFORE make_runnable (docs/e13-select-type-and-lifetime.md §4.2); the
        // caller trusts that. The caller validates only caller-owned state
        // (phase, completion mode, result, winner, caller Fiber state).
        if (group.phase() != detail::GroupPhase::completed ||
            group.completion_mode_ != detail::CompletionMode::suspended ||
            !group.result_.has_winner() ||
            group.result_.index() != group.winner() ||
            caller->state() != FiberState::running) {
            detail::select_invariant_fail_fast();
        }
        // waiting_select_count_ no longer includes this group (the suspended
        // branch decremented it exactly once at publication).
        // (Defensive: a correct publication guarantees this; we do not assert
        // an exact global value since other Selects may be in flight.)

        // select_suspended_before_consume seam: AFTER the resumed caller
        // reacquires G and validates the result, BEFORE phase Consumed. Its
        // snapshot proves phase==Completed, mode==Suspended, result present,
        // winner stable, caller resumed, runnable publication count == 1.
        // NOTE: all_authority_closed is NOT re-derived here (left false): the
        // timer pump may have lazily reclaimed the consumed Timer registration
        // between publication and resume, so re-deriving would dereference a
        // freed block. The publish_done snapshot (captured BEFORE reclamation,
        // inside the publication CS) is the authoritative all-authority-closed
        // observation; PUB-4 asserts that snapshot instead.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        {
            sluice_async_test::PublicationSnapshot snap{};
            snap.phase = group.phase();
            snap.completion_mode = group.completion_mode_;
            snap.winner = group.winner();
            snap.arm_count = group.arm_count_;
            snap.result_has_winner = group.result_.has_winner();
            snap.result_index = group.result_.has_winner() ? group.result_.index() : 0;
            snap.result_kind =
                group.result_.has_winner() ? group.result_.kind() : SelectKind::event;
            snap.all_authority_closed = false;  // not re-derived (lazy reclamation)
            snap.caller_state = caller->state();
            snap.caller_owner_id = caller_owner->id;
            snap.waiting_select_count = waiting_select_count_;
            snap.result_publication_count =
                sluice_async_test::result_publication_count(*this);
            snap.runnable_publication_count =
                sluice_async_test::runnable_publication_count(*this);
            sluice_async_test::capture_publication_snapshot(
                *this, sluice_async_test::PhaseTag::select_suspended_before_consume, snap);
        }
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::select_suspended_before_consume);
#endif

        // Only the resumed caller sets Consumed (the resolver/publication thread
        // leaves the group at Completed). Copy result_ to the local return value
        // and transition Completed -> Consumed. After return, group destruction
        // passes naturally (destructor enforces Consumed/Aborted).
        SelectResult return_value = group.result_;
        group.set_phase(detail::GroupPhase::consumed);
        return return_value;
    }
}

// ===========================================================================
// E13 P7 — Select registration-rollback authority (private Scheduler members).
// (docs/e13-select-p7-rollback-closeout.md §25, docs/e13-select-type-and-
// lifetime.md §5.2). Refines the closed E13SelectContract.tla actions
// ContractBeginRollback / ContractRollbackRelease(i) / ContractCloseAuthority(i)
// / ContractFinishRollback. All helpers are noexcept and run under one
// continuous global_mtx_ critical section; rollback never publishes and never
// throws (P7-ABORT-9/10). They may assert / fail-fast on impossible corruption.
// ===========================================================================

// BeginRollback preflight (§8): mechanically validate the rollback domain and
// set group.phase = Rollback. Rejects every forbidden domain (Selecting, Armed,
// Completed, Consumed, Aborted, Rollback re-entry). No arm authority is closed.
void Scheduler::select_begin_rollback_locked(detail::SelectGroup& group) noexcept {
    // Mechanical preconditions (debug asserts + release fail-fast).
    assert(group.scheduler_ == this &&
           "select_begin_rollback_locked: group.scheduler_ != this");
    assert(group.arms_ != nullptr &&
           "select_begin_rollback_locked: group.arms_ is null");
    assert(group.arm_count_ >= 1 &&
           "select_begin_rollback_locked: group.arm_count_ < 1");
    assert(group.arm_count_ <= kPreflightMaxArms &&
           "select_begin_rollback_locked: arm_count_ exceeds kSelectMaxArms");
    // Release defense-in-depth: wrong-home group, null arms, or an arm_count_
    // outside the valid registration range is a structural invariant violation
    // that must fail fast before phase/winner inspection (no recovery).
    if (group.scheduler_ != this || group.arms_ == nullptr ||
        group.arm_count_ < 1 || group.arm_count_ > kPreflightMaxArms) {
        detail::select_invariant_fail_fast();
    }

    const detail::GroupPhase phase = group.phase();
    // Release-mode defense-in-depth: only Building is the legal rollback domain.
    if (phase != detail::GroupPhase::building) {
        detail::select_invariant_fail_fast();
    }
    // winner must be kNoWinner throughout rollback (the group was never claimed).
    if (group.winner() != detail::kNoWinner) {
        detail::select_invariant_fail_fast();
    }
    // completion_mode must be None (no publication occurred).
    if (group.completion_mode_ != detail::CompletionMode::none) {
        detail::select_invariant_fail_fast();
    }
    // result_ must have no winner (publication is the only writer; rollback is
    // forbidden after any publication). Access the private field via friend.
    if (group.result_.has_winner()) {
        detail::select_invariant_fail_fast();
    }
    // caller must be set and still Running (P7 is pre-suspension rollback).
    if (group.caller_ == nullptr || group.caller_owner_ == nullptr) {
        detail::select_invariant_fail_fast();
    }
    if (group.caller_->state() != FiberState::running) {
        detail::select_invariant_fail_fast();
    }

    group.set_phase(detail::GroupPhase::rollback);
}

// Per-arm rollback (§9): classify the registered arm Retired FIRST, then close
// its external callback authority.
//   Event: state=Retired; then the single canonical unlink path.
//   Timer: state=Retired; then the single canonical retire CAS authority.
void Scheduler::select_rollback_arm_locked(
    detail::SelectGroup& group, detail::SelectArmSlot& arm) noexcept {
    // The arm is a fully-registered arm of this group.
    assert(arm.group == &group &&
           "select_rollback_arm_locked: arm.group != &group");
    if (arm.group != &group) {
        detail::select_invariant_fail_fast();
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Record the arm BEFORE classification (its kind + whether its Event
    // authority was linked), for the reverse-order observation (§19).
    const bool ev_linked_before =
        (arm.kind == detail::ArmKind::event) && (arm.home_ != nullptr);
    sluice_async_test::rollback_record_arm(
        *this, static_cast<std::uint32_t>(
                   static_cast<std::size_t>(&arm - group.arms_)),
        static_cast<std::uint8_t>(arm.kind), ev_linked_before);
#endif

    if (arm.kind == detail::ArmKind::event) {
        // (§9.1) Required preconditions for an Event arm.
        assert(arm.state == detail::ArmState::registered &&
               "select_rollback_arm_locked: Event arm not Registered");
        assert(arm.event.event_ != nullptr &&
               "select_rollback_arm_locked: Event arm event_ is null");
        // Release fail-fast BEFORE any unlink/retire/state mutation: a
        // non-Registered Event arm must not be classified/closed here.
        if (arm.state != detail::ArmState::registered) {
            detail::select_invariant_fail_fast();
        }
        // Mechanical membership: home_ must point at this Event's port (P7-N5
        // wrong-Event membership fails here BEFORE any unlink mutation).
        Event& ev = *arm.event.event_;
        if (arm.home_ != &ev.select_port_) {
            detail::select_invariant_fail_fast();
        }
        // 1. classify Retired FIRST (matches RollbackEventArm / Event-loser
        //    discipline).
        arm.state = detail::ArmState::retired;
        // 2. close authority via the single canonical unlink path.
        select_event_unlink_locked(ev, arm);
        // Terminal state (§9.1): state==Retired, home_/next_/prev_==nullptr
        // (select_event_unlink_locked cleared the linkage). Event::set_ is
        // never mutated.
    } else {
        // (§9.2) Required preconditions for a Timer arm.
        assert(arm.state == detail::ArmState::registered &&
               "select_rollback_arm_locked: Timer arm not Registered");
        // Release fail fast BEFORE the reg validity checks / retire: a
        // non-Registered Timer arm must not be classified/closed here.
        if (arm.state != detail::ArmState::registered) {
            detail::select_invariant_fail_fast();
        }
        detail::SelectTimerRegistration* reg = arm.timer.stable_reg_;
        if (reg == nullptr ||
            reg->scheduler() != this ||
            !pool_owns_select_block_locked(*reg) ||
            reg->arm() != &arm) {
            // P7-N6: Timer block not Scheduler-owned / wrong back-pointer.
            // Fail fast BEFORE any state/accounting mutation.
            detail::select_invariant_fail_fast();
        }
        // The registration must still be ACTIVE (P7-N7: an already-terminal
        // registration is an invariant violation, NOT an idempotent success).
        if (!reg->is_active()) {
            detail::select_invariant_fail_fast();
        }
        // 1. classify Retired FIRST — refines RollbackCancelTimer(i)
        //    (arm Registered -> TimerCancelled, timer_state still Active).
        arm.state = detail::ArmState::retired;
        // 2. close authority via the single canonical retire CAS — refines
        //    RollbackRetireTimer(i) (timer_state Active -> Retired). The helper
        //    owns ACTIVE->RETIRED + --active_deadline_count_ + recompute exactly
        //    once. A false return is an invariant violation -> fail fast.
        if (!select_timer_retire_locked(*reg)) {
            detail::select_invariant_fail_fast();
        }
    }
}

// FinishRollback postconditions (§13): prove every registered authority closed
// + every never-registered arm Detached, then set phase = Aborted.
void Scheduler::select_finish_rollback_locked(
    detail::SelectGroup& group, detail::SelectArmSlot* arms,
    std::size_t arm_count, std::size_t registered_count) noexcept {
    // Every registered arm must be Retired and fully unlinked/retired.
    for (std::size_t i = 0; i < registered_count; ++i) {
        const detail::SelectArmSlot& arm = arms[i];
        if (arm.state != detail::ArmState::retired) {
            // P7-N8: a registered authority remains open before Aborted.
            detail::select_invariant_fail_fast();
        }
        if (arm.kind == detail::ArmKind::event) {
            // Event: fully unlinked.
            if (arm.home_ != nullptr || arm.next_ != nullptr ||
                arm.prev_ != nullptr) {
                detail::select_invariant_fail_fast();
            }
        }
        // Timer: the registration is non-ACTIVE (retired). We do NOT dereference
        // stable_reg_ here for a state check that could race lazy pump
        // reclamation — select_rollback_arm_locked already proved the retire CAS
        // succeeded under the SAME held G. Caller-frame state only (§22).
    }
    // Every never-registered suffix arm must be Detached (normalized by the
    // orchestrator before this call; verify here too).
    for (std::size_t i = registered_count; i < arm_count; ++i) {
        const detail::SelectArmSlot& arm = arms[i];
        if (arm.state != detail::ArmState::detached) {
            detail::select_invariant_fail_fast();
        }
        if (arm.kind == detail::ArmKind::event) {
            if (arm.home_ != nullptr || arm.next_ != nullptr ||
                arm.prev_ != nullptr) {
                detail::select_invariant_fail_fast();
            }
        }
    }
    // winner / completion_mode / result_ still clean (unchanged since Begin).
    if (group.winner() != detail::kNoWinner ||
        group.completion_mode_ != detail::CompletionMode::none ||
        group.result_.has_winner()) {
        detail::select_invariant_fail_fast();
    }
    // caller still Running (no liveness mutation happened during rollback).
    if (group.caller_ == nullptr ||
        group.caller_->state() != FiberState::running) {
        detail::select_invariant_fail_fast();
    }

    // Only now set Aborted. admitted_ is intentionally NOT cleared:
    // admitted_ && phase==Aborted is the intended destruction proof.
    group.set_phase(detail::GroupPhase::aborted);

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::rollback_record_finish(*this);
#endif
}

// The rollback orchestrator (§10/§11): begin -> reverse per-arm rollback over
// the registered prefix -> suffix normalization -> finish. noexcept.
void Scheduler::select_rollback_registration_locked(
    detail::SelectGroup& group, detail::SelectArmSlot* arms,
    std::size_t arm_count, std::size_t registered_count) noexcept {
    assert(arms != nullptr &&
           "select_rollback_registration_locked: arms is null");
    assert(registered_count <= arm_count &&
           "select_rollback_registration_locked: registered_count > arm_count");
    // Release fail fast BEFORE indexing/traversing: a null arms array or an
    // out-of-range registered_count would walk arms[idx-1] out of bounds.
    if (arms == nullptr || registered_count > arm_count) {
        detail::select_invariant_fail_fast();
    }

    select_begin_rollback_locked(group);

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::rollback_record_begin(*this, registered_count);
#endif

    // (§10) Roll back the fully registered prefix in REVERSE registration order
    // (registered_count-1 .. 0). Registration is index-order acquisition;
    // rollback is reverse-order release.
    for (std::size_t idx = registered_count; idx > 0; --idx) {
        select_rollback_arm_locked(group, arms[idx - 1]);
    }

    // (§11) Normalize the never-registered suffix [registered_count, arm_count)
    // to Detached. No Scheduler-visible registration authority was committed for
    // these arms: Event suffix arms have home_/next_/prev_ == nullptr; Timer
    // suffix blocks remain in the caller-frame timer_tmp_pool (never Scheduler-
    // owned, never retired through Scheduler authority). They are destroyed by
    // the frame unwind after the exception escapes.
    for (std::size_t i = registered_count; i < arm_count; ++i) {
        detail::SelectArmSlot& arm = arms[i];
        if (arm.state == detail::ArmState::prepared) {
            arm.state = detail::ArmState::detached;
        }
    }

    select_finish_rollback_locked(group, arms, arm_count, registered_count);
}

}  // namespace sluice::async
