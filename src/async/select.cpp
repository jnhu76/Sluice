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
#include <list>
#include <stdexcept>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/event.hpp>
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

    const auto phase = group.phase();
    assert((phase == detail::GroupPhase::selecting ||
            phase == detail::GroupPhase::armed) &&
           "select_process_group_locked: group phase must be Selecting or Armed");
}

void Scheduler::select_preflight_claim_locked(
    detail::SelectGroup& group, std::uint32_t candidate_index) const {
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
        *this, sluice_async_test::PhaseTag::e13_admission_claimed);
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
// preflight. A future P6 publication entry guard that calls this predicate on
// a NOT-just-preflighted group should additionally validate basic group shape
// (scheduler binding, arms_/arm_count_ bounds, winner < arm_count) here, so the
// validator does not dereference fields on a corrupted group.
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
// E13 P5 — registration + inline admission.
//
// select_admit_inline is the single non-template admission core reached ONLY by
// the public variadic select() bridge (include/sluice/async/select.hpp). It owns
// every centralized admission step. The inline-ready case only:
//   - validate caller + every case Scheduler identity BEFORE any allocation
//   - materialize fixed caller-frame group + arms (no heap)
//   - allocate every Timer stable block BEFORE global_mtx_
//   - reserve deadline-heap capacity BEFORE the first registration mutation
//   - register every arm under ONE continuous global_mtx_ critical section
//   - FinishRegistration (phase = Selecting)
//   - take ONE immutable readiness snapshot (single captured monotonic_now)
//   - select the lowest ready index (only admission tie-break)
//   - call the P4 central claim/finalization core exactly once
//   - close every Event/Timer authority
//   - construct exactly ONE SelectResult, Inline completion (Completed->Consumed)
//   - return WITHOUT suspending or publishing a runnable
//
// The no-ready branch fails fast (suspended completion is P6, denied).
//
// Locking: exactly one global_mtx_ acquisition spans reserve..inline-completion,
// so no external Event::set / timer pump / other Select path may observe a
// partially-registered group (docs/e13-select-locking-and-publication.md §3.3).
//
// Caller-frame storage: SelectGroup + the SelectArmSlot array live in THIS
// function's frame. arm.home_/next_/prev_ point into them; every Event/Timer
// authority is closed before this frame returns, so no dangling reference
// survives the call (docs/e13-select-production-architecture.md §4.1).
// ===========================================================================
SelectResult Scheduler::select_admit_inline(detail::SelectCaseDescriptor* descs,
                                            std::size_t count) {
    // Release-mode defense-in-depth: reject structurally invalid arguments that
    // would cause a fixed-array out-of-bounds access. This protects against a
    // hypothetical non-friend caller that bypasses the public select() template's
    // compile-time requires clause gate, or a corrupted descriptor pointer.
    assert(descs != nullptr && count >= 1 && count <= kPreflightMaxArms &&
           "select_admit_inline: descs/count out of range (requires clause gate)");
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
    // Capture caller + owner for the (future P6) publication path. Stored on the
    // group; unused by the inline path but recorded for correctness/audit.
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
                   "select_admit_inline: Event descriptor event_ is null");
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

        // (6) Register every arm in index order.
        auto tmp_it = timer_tmp_pool.begin();
        for (std::size_t i = 0; i < count; ++i) {
            detail::SelectArmSlot& arm = arms[i];
            if (arm.kind == detail::ArmKind::event) {
                // Event arm: link into the Event's private SelectPort. This is
                // the canonical registry mutation (select_event_link_locked),
                // which sets arm.state = Registered. Distinct duplicate Event
                // cases receive DISTINCT SelectArmSlot nodes (each is a separate
                // array slot linked into the same port).
                select_event_link_locked(*descs[i].event_, arm);
            } else {
                // Timer arm: splice exactly one tmp_pool node into the Scheduler
                // pool, push its tagged DeadlineHeapEntry, increment
                // active_deadline_count_ exactly once. splice preserves the
                // stable address bound at arm.timer.stable_reg_ above.
                // Capture the next tmp_pool iterator BEFORE splicing: splice
                // transfers tmp_it's node out of tmp_pool into select_timer_
                // pool_, so incrementing tmp_it afterwards would iterate the
                // destination list, not tmp_pool.
                auto next_tmp = std::next(tmp_it);
                detail::SelectTimerRegistration* spliced =
                    select_timer_splice_one_locked(timer_tmp_pool, tmp_it);
                tmp_it = next_tmp;
                // The arm's stable_reg_ already points at `spliced` (same
                // address); no rebinding needed.
                (void)spliced;
                arm.state = detail::ArmState::registered;
            }
        }

        // After registration, tmp_pool is empty; every Timer arm's stable_reg_
        // points into select_timer_pool_; every Event arm is linked.

        // (7) FinishRegistration.
        group.set_phase(detail::GroupPhase::selecting);

        // (8) AdmissionArmed seam: AFTER every arm registered, AFTER phase
        // becomes Selecting, BEFORE the readiness snapshot.
        // P5 CORRECTIVE: capture the boundary snapshot before the seam so the
        // test can read the mechanical group/arm state without acquiring G.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        {
            sluice_async_test::AdmissionSnapshot snap;
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
                *this, sluice_async_test::PhaseTag::e13_admission_armed, snap);
        }
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::e13_admission_armed);
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

        // (10) No-ready stage guard. P5 implements inline-ready only; suspended
        // completion is P6 (denied). Fail fast in this CS — do NOT return a
        // no-winner result, do NOT unwind with live authority, do NOT suspend.
        if (!any_ready) {
            // P5 inline-only Select reached no-ready admission; suspended
            // completion is P6.
            detail::select_admission_no_ready_fail_fast();
        }

        // (11)-(12) P4 integration: claim + finalize the whole group for the
        // lowest ready index, EXACTLY ONCE. Because admission and processing
        // occur under the SAME global_mtx_ critical section, an inline admission
        // claim cannot legitimately lose; a false return is an invariant
        // violation. select_process_group_locked finalizes winner + losers and
        // asserts all-authority-closed before returning true.
        const bool won =
            select_process_group_locked(group, lowest_ready);
        if (!won) {
            // Unreachable: under one continuous CS, the winner CAS cannot lose
            // (no concurrent claimer holds the lock). A false return means the
            // group was already claimed — an invariant violation.
            detail::select_admission_no_ready_fail_fast();
        }

        // (13) Inline completion lifecycle (docs/e13-select-locking-and-
        // publication.md §5.4 inline branch; task §13).
        //   1. assert all authority closed  (select_process_group_locked did)
        //   2. construct the one local SelectResult from group.winner()
        //   3. completion_mode_ = Inline
        //   4. phase = Completed
        //   5. AdmissionConsumed seam (Completed before Consumed)
        //   6. copy result to local return value
        //   7. phase = Consumed
        assert(select_all_authority_closed_locked(group) &&
               "select_admit_inline: authority not closed after P4 processing");

        const std::uint32_t winner_index = group.winner();
        const detail::SelectArmSlot& winner_arm = arms[winner_index];
        SelectResult result;
        if (winner_arm.kind == detail::ArmKind::event) {
            result = SelectResult(winner_index, SelectKind::event,
                                  SelectTimerOutcome::fired);
        } else {
            result = SelectResult(winner_index, SelectKind::timer,
                                  SelectTimerOutcome::fired);
        }

        group.completion_mode_ = detail::CompletionMode::inline_;
        group.set_phase(detail::GroupPhase::completed);

        // AdmissionConsumed seam: AFTER phase becomes Completed, BEFORE Consumed.
        // P5 CORRECTIVE: capture the boundary snapshot before the seam so the
        // test can read the mechanical inline lifecycle state.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        {
            sluice_async_test::AdmissionSnapshot snap;
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
                *this, sluice_async_test::PhaseTag::e13_admission_consumed, snap);
        }
        sluice_async_test::test_phase(
            *this, sluice_async_test::PhaseTag::e13_admission_consumed);
#endif

        SelectResult return_value = result;  // copy to local return value
        group.set_phase(detail::GroupPhase::consumed);

        // (14) Unlock (LockGuard destructor) and return. NO make_runnable /
        // route_runnable_locked / make_waiting / context_switch — suspended
        // publication is P6, denied.
        return return_value;
    }
}

}  // namespace sluice::async
