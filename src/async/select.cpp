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
#include <sluice/async/select.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {

namespace {

constexpr std::size_t kPreflightMaxArms = 8;
static_assert(kPreflightMaxArms >= 1, "Select must permit at least one arm");
static_assert(kPreflightMaxArms == kSelectMaxArms,
              "kPreflightMaxArms must match the public kSelectMaxArms gate");
} // namespace

void Scheduler::select_preflight_shape_locked(detail::SelectGroup& group,
                                              std::uint32_t candidate_index) const {
    assert(group.scheduler_ == this && "select_process_group_locked: group.scheduler_ != this");
    if (group.scheduler_ != this)
        detail::select_invariant_fail_fast();
    assert(group.arms_ != nullptr && "select_process_group_locked: group.arms_ is null");
    if (group.arms_ == nullptr)
        detail::select_invariant_fail_fast();
    assert(group.arm_count_ >= 1 && "select_process_group_locked: group.arm_count_ < 1");
    if (group.arm_count_ < 1)
        detail::select_invariant_fail_fast();
    assert(group.arm_count_ <= kPreflightMaxArms &&
           "select_process_group_locked: group.arm_count_ exceeds kSelectMaxArms");
    if (group.arm_count_ > kPreflightMaxArms)
        detail::select_invariant_fail_fast();
    assert(candidate_index < group.arm_count_ &&
           "select_process_group_locked: candidate_index out of range");
    if (candidate_index >= group.arm_count_)
        detail::select_invariant_fail_fast();

    [[maybe_unused]] const auto phase = group.phase();
    assert((phase == detail::GroupPhase::selecting || phase == detail::GroupPhase::armed) &&
           "select_process_group_locked: group phase must be Selecting or Armed");
    if (phase != detail::GroupPhase::selecting && phase != detail::GroupPhase::armed) {
        detail::select_invariant_fail_fast();
    }
}

void Scheduler::select_preflight_claim_locked(detail::SelectGroup& group,
                                              std::uint32_t candidate_index) const {
    [[maybe_unused]] detail::SelectArmSlot& candidate = group.arms_[candidate_index];
    assert(candidate.group == &group &&
           "select_process_group_locked: candidate arm.group != &group");
    if (candidate.group != &group)
        detail::select_invariant_fail_fast();
    assert(candidate.state == detail::ArmState::candidate_ready &&
           "select_process_group_locked: candidate arm not CandidateReady");
    if (candidate.state != detail::ArmState::candidate_ready)
        detail::select_invariant_fail_fast();

    for (std::size_t i = 0; i < group.arm_count_; ++i) {
        [[maybe_unused]] detail::SelectArmSlot& arm = group.arms_[i];
        assert(arm.group == &group && "select_process_group_locked: arm.group != &group");
        if (arm.group != &group)
            detail::select_invariant_fail_fast();
        assert((arm.kind == detail::ArmKind::event || arm.kind == detail::ArmKind::timer) &&
               "select_process_group_locked: arm.kind must be Event or Timer");
        if (arm.kind != detail::ArmKind::event && arm.kind != detail::ArmKind::timer) {
            detail::select_invariant_fail_fast();
        }
        assert((arm.state == detail::ArmState::registered ||
                arm.state == detail::ArmState::candidate_ready) &&
               "select_process_group_locked: arm.state must be Registered or "
               "CandidateReady");
        if (arm.state != detail::ArmState::registered &&
            arm.state != detail::ArmState::candidate_ready) {
            detail::select_invariant_fail_fast();
        }
    }

    for (std::size_t i = 0; i < group.arm_count_; ++i) {
        detail::SelectArmSlot& arm = group.arms_[i];
        if (arm.kind != detail::ArmKind::event)
            continue;
        assert(arm.event.event_ != nullptr &&
               "select_process_group_locked: Event arm event_ is null");
        if (arm.event.event_ == nullptr)
            detail::select_invariant_fail_fast();
        Event& ev = *arm.event.event_;
        assert(&ev.scheduler_ == this &&
               "select_process_group_locked: Event does not belong to this Scheduler");
        if (&ev.scheduler_ != this)
            detail::select_invariant_fail_fast();
        assert(arm.home_ == &ev.select_port_ &&
               "select_process_group_locked: Event arm not linked to its Event port");
        if (arm.home_ != &ev.select_port_)
            detail::select_invariant_fail_fast();

        bool found = false;
        for (detail::SelectArmSlot* p = ev.select_port_.head_; p != nullptr; p = p->next_) {
            if (p == &arm) {
                found = true;
                break;
            }
        }
        (void)found;
        assert(found && "select_process_group_locked: Event arm home_ points at "
                        "this Event's port but the arm is not in the intrusive list");
        if (!found)
            detail::select_invariant_fail_fast();
    }

    for (std::size_t i = 0; i < group.arm_count_; ++i) {
        detail::SelectArmSlot& arm = group.arms_[i];
        if (arm.kind != detail::ArmKind::timer)
            continue;
        [[maybe_unused]] detail::SelectTimerRegistration* reg = arm.timer.stable_reg_;
        assert(reg != nullptr && "select_process_group_locked: Timer arm stable_reg_ is null");
        if (reg == nullptr)
            detail::select_invariant_fail_fast();
        assert(reg->scheduler() == this &&
               "select_process_group_locked: Timer registration belongs to "
               "another Scheduler");
        if (reg->scheduler() != this)
            detail::select_invariant_fail_fast();
        assert(pool_owns_select_block_locked(*reg) &&
               "select_process_group_locked: Scheduler pool does not own the "
               "Timer registration");
        if (!pool_owns_select_block_locked(*reg))
            detail::select_invariant_fail_fast();
        assert(reg->is_active() && "select_process_group_locked: Timer registration is not ACTIVE");
        if (!reg->is_active())
            detail::select_invariant_fail_fast();
        assert(reg->arm() == &arm &&
               "select_process_group_locked: Timer registration.arm() != &arm");
        if (reg->arm() != &arm)
            detail::select_invariant_fail_fast();
    }
}

bool Scheduler::select_process_group_locked(detail::SelectGroup& group,
                                            std::uint32_t candidate_index) {
    select_preflight_shape_locked(group, candidate_index);

    if (group.winner() != detail::kNoWinner) {
        return false;
    }

    select_preflight_claim_locked(group, candidate_index);

    if (!group.claim_winner_locked(candidate_index)) {
        return false;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this, sluice_async_test::PhaseTag::select_admission_claimed);
#endif
    select_commit_winner_locked(group, candidate_index);
    for (std::uint32_t i = 0; i < group.arm_count_; ++i) {
        if (i == candidate_index)
            continue;
        select_finalize_loser_locked(group, i);
    }

    assert(select_all_authority_closed_locked(group) &&
           "select_process_group_locked: all-authority-closed invariant failed "
           "after finalization");
    if (!select_all_authority_closed_locked(group))
        detail::select_invariant_fail_fast();

    return true;
}

void Scheduler::select_commit_winner_locked(detail::SelectGroup& group,
                                            std::uint32_t winner_index) {
    detail::SelectArmSlot& arm = group.arms_[winner_index];
    if (arm.kind == detail::ArmKind::event) {
        select_finalize_event_winner_locked(group, arm);
    } else {
        select_finalize_timer_winner_locked(group, arm);
    }
}

void Scheduler::select_finalize_loser_locked(detail::SelectGroup& group,
                                             std::uint32_t loser_index) {
    detail::SelectArmSlot& arm = group.arms_[loser_index];
    if (arm.kind == detail::ArmKind::event) {
        select_finalize_event_loser_locked(group, arm);
    } else {
        select_finalize_timer_loser_locked(group, arm);
    }
}

bool Scheduler::select_all_authority_closed_locked(const detail::SelectGroup& group) const {
    if (group.winner() == detail::kNoWinner)
        return false;
    if (group.winner() >= group.arm_count_)
        return false;

    const std::uint32_t winner = group.winner();
    for (std::uint32_t i = 0; i < group.arm_count_; ++i) {
        const detail::SelectArmSlot& arm = group.arms_[i];
        if (arm.state != detail::ArmState::retired)
            return false;

        if (arm.kind == detail::ArmKind::event) {
            if (arm.home_ != nullptr)
                return false;
            if (arm.next_ != nullptr)
                return false;
            if (arm.prev_ != nullptr)
                return false;
        } else {
            const detail::SelectTimerRegistration* reg = arm.timer.stable_reg_;
            if (reg == nullptr)
                return false;

            if (i == winner) {
                if (!reg->is_consumed())
                    return false;
            } else {
                if (!reg->is_retired())
                    return false;
            }
        }
    }
    return true;
}

void Scheduler::select_publish_locked(detail::SelectGroup& group) {
    if (group.scheduler_ != this || group.arms_ == nullptr || group.arm_count_ < 1 ||
        group.arm_count_ > kPreflightMaxArms) {
        detail::select_invariant_fail_fast();
    }

    const std::uint32_t winner_index = group.winner();
    if (winner_index == detail::kNoWinner || winner_index >= group.arm_count_) {
        detail::select_invariant_fail_fast();
    }
    const detail::GroupPhase phase = group.phase();

    if (phase != detail::GroupPhase::selecting && phase != detail::GroupPhase::armed) {
        detail::select_invariant_fail_fast();
    }

    if (group.completion_mode_ != detail::CompletionMode::none) {
        detail::select_invariant_fail_fast();
    }

    if (group.result_.has_winner()) {
        detail::select_invariant_fail_fast();
    }

    for (std::size_t i = 0; i < group.arm_count_; ++i) {
        const detail::SelectArmSlot& arm = group.arms_[i];
        if (arm.kind != detail::ArmKind::event && arm.kind != detail::ArmKind::timer) {
            detail::select_invariant_fail_fast();
        }
    }
    const detail::SelectArmSlot& winner_arm = group.arms_[winner_index];
    if (winner_arm.kind != detail::ArmKind::event && winner_arm.kind != detail::ArmKind::timer) {
        detail::select_invariant_fail_fast();
    }

    if (!select_all_authority_closed_locked(group)) {
        detail::select_invariant_fail_fast();
    }

    const bool suspended = (phase == detail::GroupPhase::armed);
    if (suspended) {
        if (group.caller_ == nullptr || group.caller_owner_ == nullptr) {
            detail::select_invariant_fail_fast();
        }
        if (group.caller_->state() != FiberState::waiting) {
            detail::select_invariant_fail_fast();
        }
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    {
        sluice_async_test::PublicationSnapshot snap{};
        snap.phase = group.phase();
        snap.completion_mode = group.completion_mode_;
        snap.winner = group.winner();
        snap.arm_count = group.arm_count_;
        snap.result_has_winner = group.result_.has_winner();
        snap.result_index = group.result_.has_winner() ? group.result_.index() : 0;
        snap.result_kind = group.result_.has_winner() ? group.result_.kind() : SelectKind::event;
        snap.all_authority_closed = select_all_authority_closed_locked(group);
        snap.caller_state = group.caller_ ? group.caller_->state() : FiberState::created;
        snap.caller_owner_id = group.caller_owner_ ? group.caller_owner_->id : 0;
        snap.waiting_select_count = waiting_select_count_;
        snap.result_publication_count = sluice_async_test::result_publication_count(*this);
        snap.runnable_publication_count = sluice_async_test::runnable_publication_count(*this);
        sluice_async_test::capture_publication_snapshot(
            *this, sluice_async_test::PhaseTag::select_publish_entry, snap);
    }
    sluice_async_test::test_phase(*this, sluice_async_test::PhaseTag::select_publish_entry);
#endif

    SelectResult result;
    if (winner_arm.kind == detail::ArmKind::event) {
        result = SelectResult(winner_index, SelectKind::event, SelectTimerOutcome::fired);
    } else {
        result = SelectResult(winner_index, SelectKind::timer, SelectTimerOutcome::fired);
    }
    group.result_ = result;

    if (suspended) {
        group.completion_mode_ = detail::CompletionMode::suspended;
        group.set_phase(detail::GroupPhase::completed);

        if (waiting_select_count_ == 0) {
            detail::select_invariant_fail_fast();
        }
        --waiting_select_count_;

        const bool published = group.caller_->make_runnable();
        if (!published) {
            detail::select_invariant_fail_fast();
        }

        route_runnable_locked(group.caller_, group.caller_owner_);

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::increment_result_publication(*this);
        sluice_async_test::increment_runnable_publication(*this);

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
            snap.result_publication_count = sluice_async_test::result_publication_count(*this);
            snap.runnable_publication_count = sluice_async_test::runnable_publication_count(*this);
            sluice_async_test::capture_publication_snapshot(
                *this, sluice_async_test::PhaseTag::select_publish_done, snap);
        }
        sluice_async_test::test_phase(*this, sluice_async_test::PhaseTag::select_publish_done);
#endif
    } else {
        group.completion_mode_ = detail::CompletionMode::inline_;
        group.set_phase(detail::GroupPhase::completed);

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::increment_result_publication(*this);

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
            snap.result_publication_count = sluice_async_test::result_publication_count(*this);
            snap.runnable_publication_count = sluice_async_test::runnable_publication_count(*this);
            sluice_async_test::capture_publication_snapshot(
                *this, sluice_async_test::PhaseTag::select_publish_done, snap);
        }
        sluice_async_test::test_phase(*this, sluice_async_test::PhaseTag::select_publish_done);
#endif
    }
}

bool Scheduler::select_resolve_event_locked(Event& event) {
    assert(&event.scheduler_ == this &&
           "select_resolve_event_locked: Event does not belong to this Scheduler");

    detail::SelectArmSlot* arms_buf[kPreflightMaxArms] = {};
    std::size_t eligible_count = 0;
    detail::SelectGroup* distinct_groups[kPreflightMaxArms] = {};
    std::size_t distinct_group_count = 0;

    for (detail::SelectArmSlot* arm = event.select_port_.head_; arm != nullptr; arm = arm->next_) {
        if (arm->kind != detail::ArmKind::event)
            continue;
        if (arm->state != detail::ArmState::registered)
            continue;
        if (arm->group == nullptr)
            continue;
        if (arm->group->phase() != detail::GroupPhase::armed)
            continue;
        if (arm->home_ != &event.select_port_)
            continue;
        if (arm->event.event_ != &event)
            continue;

        if (eligible_count < kPreflightMaxArms) {
            arms_buf[eligible_count++] = arm;
        }

        bool known = false;
        for (std::size_t g = 0; g < distinct_group_count; ++g) {
            if (distinct_groups[g] == arm->group) {
                known = true;
                break;
            }
        }
        if (!known && distinct_group_count < kPreflightMaxArms) {
            distinct_groups[distinct_group_count++] = arm->group;
        }
    }

    if (eligible_count == 0) {
        return false;
    }

    if (distinct_group_count > 1) {
        detail::select_multi_group_event_stage_fail_fast();
    }

    detail::SelectGroup* const group = distinct_groups[0];

    for (std::size_t i = 0; i < eligible_count; ++i) {
        arms_buf[i]->state = detail::ArmState::candidate_ready;
    }

    std::uint32_t candidate_index = static_cast<std::uint32_t>(-1);
    for (std::size_t i = 0; i < group->arm_count_; ++i) {
        detail::SelectArmSlot& arm = group->arms_[i];
        if (arm.state != detail::ArmState::candidate_ready)
            continue;
        if (arm.kind != detail::ArmKind::event)
            continue;
        if (arm.event.event_ != &event)
            continue;
        candidate_index = static_cast<std::uint32_t>(i);
        break;
    }

    if (candidate_index >= group->arm_count_) {
        detail::select_invariant_fail_fast();
    }

    const bool won = select_process_group_locked(*group, candidate_index);
    if (!won) {
        return false;
    }
    select_publish_locked(*group);
    return true;
}

bool Scheduler::select_resolve_timer_locked(detail::SelectTimerRegistration& reg) {
    if (reg.scheduler() != this) {
        detail::select_invariant_fail_fast();
    }
    if (!reg.is_active()) {
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

    arm->state = detail::ArmState::candidate_ready;

    const bool won = select_process_group_locked(*group, candidate_index);
    if (!won) {
        return false;
    }
    select_publish_locked(*group);
    return true;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

SelectResult Scheduler::AsyncTestAccess::group_result(const Scheduler&,
                                                      const detail::SelectGroup& group) {
    return group.result_;
}
#endif

SelectResult Scheduler::select_admit(detail::SelectCaseDescriptor* descs, std::size_t count) {
    assert(descs != nullptr && count >= 1 && count <= kPreflightMaxArms &&
           "select_admit: descs/count out of range (requires clause gate)");
    if (descs == nullptr || count == 0 || count > kSelectMaxArms) {
        detail::select_invariant_fail_fast();
    }

    WorkerState* ws = current_worker();
    if (ws == nullptr) {
        throw std::logic_error("select() called from a plain OS thread, not a Scheduler worker");
    }
    if (ws->owner_scheduler != this) {
        throw std::logic_error("select() called on a Scheduler that does not own this worker");
    }
    if (ws->current == nullptr) {
        throw std::logic_error("select() called with no current Fiber");
    }

    Fiber* const caller = ws->current;
    WorkerState* const caller_owner = ws;

    for (std::size_t i = 0; i < count; ++i) {
        const detail::SelectCaseDescriptor& d = descs[i];
        switch (d.kind_) {
        case detail::SelectCaseDescriptor::Kind::event: {
            assert(d.event_ != nullptr && "select_admit: Event descriptor event_ is null");
            if (&d.event_->scheduler_ != this) {
                throw std::invalid_argument("select(): Event does not belong to this Scheduler");
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

    detail::SelectGroup group;
    group.scheduler_ = this;
    group.caller_ = caller;
    group.caller_owner_ = caller_owner;
    group.completion_mode_ = detail::CompletionMode::none;
    group.set_phase(detail::GroupPhase::building);

    std::array<detail::SelectArmSlot, kPreflightMaxArms> arms;
    group.arms_ = arms.data();
    group.arm_count_ = count;

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
            timer_tmp_pool.emplace_back(&arm, this, static_cast<deadline_tick_t>(d.deadline_));
            detail::SelectTimerRegistration& node = timer_tmp_pool.back();
            arm.timer.stable_reg_ = &node;
            ++timer_arm_count;
            break;
        }
        default:
            detail::select_invariant_fail_fast();
        }
    }

    {
        LockGuard lk(global_mtx_);

        if (timer_arm_count > deadline_heap_.max_size() - deadline_heap_.size()) {
            throw std::length_error("select(): deadline heap capacity overflow on reserve");
        }
        deadline_heap_.reserve(deadline_heap_.size() + timer_arm_count);

        group.mark_admitted();

        std::size_t registered_count = 0;
        auto tmp_it = timer_tmp_pool.begin();
        try {
            for (std::size_t i = 0; i < count; ++i) {
                detail::SelectArmSlot& arm = arms[i];

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                if (sluice_async_test::rollback_should_inject_after(*this, registered_count)) {
                    throw sluice_async_test::SelectRegistrationFailure{};
                }
#endif
                if (arm.kind == detail::ArmKind::event) {
                    select_event_link_locked(*descs[i].event_, arm);
                } else {
                    auto next_tmp = std::next(tmp_it);
                    detail::SelectTimerRegistration* spliced =
                        select_timer_splice_one_locked(timer_tmp_pool, tmp_it);
                    tmp_it = next_tmp;

                    (void)spliced;
                    arm.state = detail::ArmState::registered;
                }

                ++registered_count;
            }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

            if (sluice_async_test::rollback_should_inject_after(*this, registered_count)) {
                throw sluice_async_test::SelectRegistrationFailure{};
            }
#endif

            group.set_phase(detail::GroupPhase::selecting);
        } catch (...) {
            select_rollback_registration_locked(group, arms.data(), count, registered_count);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

            sluice_async_test::test_phase(*this,
                                          sluice_async_test::PhaseTag::select_rollback_aborted);
#endif
            throw;
        }

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
        sluice_async_test::test_phase(*this, sluice_async_test::PhaseTag::select_admission_armed);
#endif

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
            const bool won = select_process_group_locked(group, lowest_ready);
            if (!won) {
                detail::select_invariant_fail_fast();
            }

            select_publish_locked(group);

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
                        snap.timer_states[si] =
                            arms[si].timer.stable_reg_
                                ? arms[si].timer.stable_reg_->state()
                                : detail::SelectTimerRegistration::State::consumed;
                    } else {
                        snap.timer_states[si] = detail::SelectTimerRegistration::State::consumed;
                    }
                }
                sluice_async_test::capture_admission_snapshot(
                    *this, sluice_async_test::PhaseTag::select_admission_consumed, snap);
            }
            sluice_async_test::test_phase(*this,
                                          sluice_async_test::PhaseTag::select_admission_consumed);
#endif

            SelectResult return_value = group.result_;
            group.set_phase(detail::GroupPhase::consumed);

            return return_value;
        }

        commit_suspend_locked(caller_owner, caller);
        group.set_phase(detail::GroupPhase::armed);
        ++waiting_select_count_;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this, sluice_async_test::PhaseTag::select_suspend_before_switch);
#endif

    fiber_ctx::Switch s;
    s.old = &caller->ctx;
    s.new_ = &caller_owner->sched_ctx;
    (void)fiber_ctx::context_switch(&s);

    {
        LockGuard lk(global_mtx_);

        if (group.phase() != detail::GroupPhase::completed ||
            group.completion_mode_ != detail::CompletionMode::suspended ||
            !group.result_.has_winner() || group.result_.index() != group.winner() ||
            caller->state() != FiberState::running) {
            detail::select_invariant_fail_fast();
        }

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
            snap.all_authority_closed = false;
            snap.caller_state = caller->state();
            snap.caller_owner_id = caller_owner->id;
            snap.waiting_select_count = waiting_select_count_;
            snap.result_publication_count = sluice_async_test::result_publication_count(*this);
            snap.runnable_publication_count = sluice_async_test::runnable_publication_count(*this);
            sluice_async_test::capture_publication_snapshot(
                *this, sluice_async_test::PhaseTag::select_suspended_before_consume, snap);
        }
        sluice_async_test::test_phase(*this,
                                      sluice_async_test::PhaseTag::select_suspended_before_consume);
#endif

        SelectResult return_value = group.result_;
        group.set_phase(detail::GroupPhase::consumed);
        return return_value;
    }
}

void Scheduler::select_begin_rollback_locked(detail::SelectGroup& group) noexcept {
    assert(group.scheduler_ == this && "select_begin_rollback_locked: group.scheduler_ != this");
    assert(group.arms_ != nullptr && "select_begin_rollback_locked: group.arms_ is null");
    assert(group.arm_count_ >= 1 && "select_begin_rollback_locked: group.arm_count_ < 1");
    assert(group.arm_count_ <= kPreflightMaxArms &&
           "select_begin_rollback_locked: arm_count_ exceeds kSelectMaxArms");

    if (group.scheduler_ != this || group.arms_ == nullptr || group.arm_count_ < 1 ||
        group.arm_count_ > kPreflightMaxArms) {
        detail::select_invariant_fail_fast();
    }

    const detail::GroupPhase phase = group.phase();

    if (phase != detail::GroupPhase::building) {
        detail::select_invariant_fail_fast();
    }

    if (group.winner() != detail::kNoWinner) {
        detail::select_invariant_fail_fast();
    }

    if (group.completion_mode_ != detail::CompletionMode::none) {
        detail::select_invariant_fail_fast();
    }

    if (group.result_.has_winner()) {
        detail::select_invariant_fail_fast();
    }

    if (group.caller_ == nullptr || group.caller_owner_ == nullptr) {
        detail::select_invariant_fail_fast();
    }
    if (group.caller_->state() != FiberState::running) {
        detail::select_invariant_fail_fast();
    }

    group.set_phase(detail::GroupPhase::rollback);
}

void Scheduler::select_rollback_arm_locked(detail::SelectGroup& group,
                                           detail::SelectArmSlot& arm) noexcept {
    assert(arm.group == &group && "select_rollback_arm_locked: arm.group != &group");
    if (arm.group != &group) {
        detail::select_invariant_fail_fast();
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    const bool ev_linked_before = (arm.kind == detail::ArmKind::event) && (arm.home_ != nullptr);
    sluice_async_test::rollback_record_arm(
        *this, static_cast<std::uint32_t>(static_cast<std::size_t>(&arm - group.arms_)),
        static_cast<std::uint8_t>(arm.kind), ev_linked_before);
#endif

    if (arm.kind == detail::ArmKind::event) {
        assert(arm.state == detail::ArmState::registered &&
               "select_rollback_arm_locked: Event arm not Registered");
        assert(arm.event.event_ != nullptr &&
               "select_rollback_arm_locked: Event arm event_ is null");

        if (arm.state != detail::ArmState::registered) {
            detail::select_invariant_fail_fast();
        }

        Event& ev = *arm.event.event_;
        if (arm.home_ != &ev.select_port_) {
            detail::select_invariant_fail_fast();
        }

        arm.state = detail::ArmState::retired;

        select_event_unlink_locked(ev, arm);

    } else {
        assert(arm.state == detail::ArmState::registered &&
               "select_rollback_arm_locked: Timer arm not Registered");

        if (arm.state != detail::ArmState::registered) {
            detail::select_invariant_fail_fast();
        }
        detail::SelectTimerRegistration* reg = arm.timer.stable_reg_;
        if (reg == nullptr || reg->scheduler() != this || !pool_owns_select_block_locked(*reg) ||
            reg->arm() != &arm) {
            detail::select_invariant_fail_fast();
        }

        if (!reg->is_active()) {
            detail::select_invariant_fail_fast();
        }

        arm.state = detail::ArmState::retired;

        if (!select_timer_retire_locked(*reg)) {
            detail::select_invariant_fail_fast();
        }
    }
}

void Scheduler::select_finish_rollback_locked(detail::SelectGroup& group,
                                              detail::SelectArmSlot* arms, std::size_t arm_count,
                                              std::size_t registered_count) noexcept {
    for (std::size_t i = 0; i < registered_count; ++i) {
        const detail::SelectArmSlot& arm = arms[i];
        if (arm.state != detail::ArmState::retired) {
            detail::select_invariant_fail_fast();
        }
        if (arm.kind == detail::ArmKind::event) {
            if (arm.home_ != nullptr || arm.next_ != nullptr || arm.prev_ != nullptr) {
                detail::select_invariant_fail_fast();
            }
        }
    }

    for (std::size_t i = registered_count; i < arm_count; ++i) {
        const detail::SelectArmSlot& arm = arms[i];
        if (arm.state != detail::ArmState::detached) {
            detail::select_invariant_fail_fast();
        }
        if (arm.kind == detail::ArmKind::event) {
            if (arm.home_ != nullptr || arm.next_ != nullptr || arm.prev_ != nullptr) {
                detail::select_invariant_fail_fast();
            }
        }
    }

    if (group.winner() != detail::kNoWinner ||
        group.completion_mode_ != detail::CompletionMode::none || group.result_.has_winner()) {
        detail::select_invariant_fail_fast();
    }

    if (group.caller_ == nullptr || group.caller_->state() != FiberState::running) {
        detail::select_invariant_fail_fast();
    }

    group.set_phase(detail::GroupPhase::aborted);

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::rollback_record_finish(*this);
#endif
}

void Scheduler::select_rollback_registration_locked(detail::SelectGroup& group,
                                                    detail::SelectArmSlot* arms,
                                                    std::size_t arm_count,
                                                    std::size_t registered_count) noexcept {
    assert(arms != nullptr && "select_rollback_registration_locked: arms is null");
    assert(registered_count <= arm_count &&
           "select_rollback_registration_locked: registered_count > arm_count");

    if (arms == nullptr || registered_count > arm_count) {
        detail::select_invariant_fail_fast();
    }

    select_begin_rollback_locked(group);

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::rollback_record_begin(*this, registered_count);
#endif

    for (std::size_t idx = registered_count; idx > 0; --idx) {
        select_rollback_arm_locked(group, arms[idx - 1]);
    }

    for (std::size_t i = registered_count; i < arm_count; ++i) {
        detail::SelectArmSlot& arm = arms[i];
        if (arm.state == detail::ArmState::prepared) {
            arm.state = detail::ArmState::detached;
        }
    }

    select_finish_rollback_locked(group, arms, arm_count, registered_count);
}

} // namespace sluice::async
