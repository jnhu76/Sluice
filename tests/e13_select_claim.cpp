// e13_select_claim — E13 P4 Select central claim + winner/loser finalization
// tests.
//
// Drives the P4 core (Scheduler::select_process_group_locked) and the
// all-authority-closed invariant (select_all_authority_closed_locked) via
// guarded internal-testing seams (AsyncTestAccess). The test harness builds a
// "registered group" exactly as a future admission would: group.scheduler_
// bound, arms_/arm_count_ set, Event arms linked via select_event_link, Timer
// arms registered via register_synthetic_select_timer, one arm marked
// CandidateReady.
//
// Coverage (production-test-plan §13): C1 first-claim-wins, C2 second-claim-
// loses, C3 winner-identity stability, C4 Event winner+loser, C5 Timer winner+
// loser, C6 Event winner+Timer loser, C7 Timer winner+Event loser, C8 same-
// Event-twice, C9 claim-lost no-mutation, C10 fully-finalized invariant,
// C11 no-publication, C12 Timer loser ordering (SN-9).
//
// Deterministic: test clock + causal phase seams; NO sleep_for. Gated to
// x86_64 (fiber_ctx::supported) for parity with the rest of E13, though P4
// itself suspends no fiber.
#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/detail/select_registration.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/select.hpp>

#include "async_test_control.hpp"
#include "harness.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace sa = sluice::async;
namespace sad = sluice::async::detail;
namespace stest = sluice_async_test;

using AsyncTestAccess = sa::Scheduler::AsyncTestAccess;
using Scheduler = sa::Scheduler;
using Event = sa::Event;
using SelectArmSlot = sad::SelectArmSlot;
using SelectGroup = sad::SelectGroup;
using SelectTimerReg = sad::SelectTimerRegistration;
using ArmKind = sad::ArmKind;
using ArmState = sad::ArmState;
using GroupPhase = sad::GroupPhase;

// ===========================================================================
// Test fixture: build a registered Select group of up to kMaxArms arms.
//
// The harness mirrors the future admission protocol (P5, denied here):
//   - group.scheduler_ = &sched
//   - group.arms_ = arms_.data(); group.arm_count_ = n
//   - each Event arm: construct_event(ev), state=prepared, group=&group, link
//   - each Timer arm: construct_timer(deadline), state=prepared, group=&group,
//     register_synthetic_select_timer (which sets stable_reg_ and ACTIVE)
//   - group phase set to Armed (the phase at which a candidate may be claimed)
//
// Teardown: retire any still-ACTIVE Timer regs (so ~Scheduler sees none),
// unlink any still-linked Event arms (so ~Event sees an empty SelectPort), and
// drain the Select pool. The group is a structural object; if it won a claim
// its destructor does not enforce a terminal phase (it was never mark_admitted).
// ===========================================================================
class ClaimFixture {
public:
    static constexpr std::size_t kMaxArms = 8;

    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched;
    stest::ControllerGuard ctrl;
    SelectGroup group;
    std::array<SelectArmSlot, kMaxArms> arms;
    // Track which arms are Event vs Timer + their registration, for teardown.
    std::array<Event*, kMaxArms> evt_for_arm{};       // nullptr if Timer
    std::array<SelectTimerReg*, kMaxArms> reg_for_arm{};  // nullptr if Event
    std::size_t arm_count{0};

    ClaimFixture() : sched(ctx), ctrl(sched) {
        stest::E11TimerControl::enable_test_clock(sched);
        group.scheduler_ = &sched;
        group.arms_ = arms.data();
        group.set_phase(GroupPhase::armed);
    }

    ~ClaimFixture() {
        // Best-effort teardown for objects that outlive a test case. Most test
        // cases finalize/retire everything explicitly; this only catches
        // intentionally-left-open authority (e.g. claim-lost tests). The public
        // unlink/retire accessors lock global_mtx_ internally.
        for (std::size_t i = 0; i < arm_count; ++i) {
            SelectArmSlot& arm = arms[i];
            if (evt_for_arm[i] != nullptr && arm.home_ != nullptr) {
                AsyncTestAccess::select_event_unlink(sched, *evt_for_arm[i], arm);
            }
            if (reg_for_arm[i] != nullptr && reg_for_arm[i]->is_active()) {
                stest::E13SelectTimerSeam::retire_synthetic(sched, *reg_for_arm[i]);
            }
        }
        // Drain the Select timer pool (retire leftover + pump to reclamation).
        AsyncTestAccess::drain_select_pool(sched);
    }

    ClaimFixture(const ClaimFixture&) = delete;
    ClaimFixture& operator=(const ClaimFixture&) = delete;

    // Add an Event arm at the next slot. Returns its index.
    std::uint32_t add_event_arm(Event& ev) {
        std::uint32_t idx = static_cast<std::uint32_t>(arm_count);
        SelectArmSlot& arm = arms[idx];
        arm.construct_event(ev);
        arm.state = ArmState::prepared;
        arm.group = &group;
        AsyncTestAccess::select_event_link(sched, ev, arm);
        evt_for_arm[idx] = &ev;
        ++arm_count;
        group.arm_count_ = arm_count;
        return idx;
    }

    // Add a Timer arm at the next slot. Returns its index.
    std::uint32_t add_timer_arm(Scheduler::deadline_t deadline) {
        std::uint32_t idx = static_cast<std::uint32_t>(arm_count);
        SelectArmSlot& arm = arms[idx];
        arm.construct_timer(deadline);
        arm.state = ArmState::prepared;
        arm.group = &group;
        SelectTimerReg* reg = stest::E13SelectTimerSeam::register_synthetic(
            sched, &arm, deadline);
        // Admission-equivalent binding: the arm's stable_reg_ back-pointer is
        // set after registration (mirrors the future admission protocol).
        arm.timer.stable_reg_ = reg;
        // Admission-equivalent: a registered Timer arm is in the Registered
        // state (mirrors select_event_link setting Event arms to Registered).
        arm.state = ArmState::registered;
        reg_for_arm[idx] = reg;
        ++arm_count;
        group.arm_count_ = arm_count;
        return idx;
    }

    // Mark arm `idx` CandidateReady (the state a readiness scan produces).
    void mark_candidate(std::uint32_t idx) {
        arms[idx].state = ArmState::candidate_ready;
    }

    // Drive the P4 core for `candidate_index`.
    bool process(std::uint32_t candidate_index) {
        return AsyncTestAccess::select_process_group(sched, group, candidate_index);
    }

    // Read the all-authority-closed invariant.
    bool all_closed() const {
        return AsyncTestAccess::select_all_authority_closed(sched, group);
    }

    // active_deadline_count_ read (E11TimerControl facade).
    std::size_t active_deadline_count() const {
        return stest::E11TimerControl::active_deadline_count(sched);
    }
};

// ===========================================================================
// C1 — first claim wins
// ===========================================================================
// One CandidateReady Event arm; winner initially kNoWinner; process returns
// true; winner == candidate index; arm finalized; all authority closed; phase
// unchanged.
SLUICE_TEST_CASE(c1_first_claim_wins) {
    ClaimFixture f;
    Event ev(f.sched);
    std::uint32_t idx = f.add_event_arm(ev);
    f.mark_candidate(idx);

    SLUICE_CHECK(f.group.winner() == sad::kNoWinner);
    SLUICE_CHECK(f.process(idx) == true);
    SLUICE_CHECK(f.group.winner() == idx);
    SLUICE_CHECK(f.arms[idx].state == ArmState::retired);
    SLUICE_CHECK(f.arms[idx].home_ == nullptr);
    SLUICE_CHECK(f.all_closed());
    SLUICE_CHECK(f.group.phase() == GroupPhase::armed);
    SLUICE_CHECK(f.group.completion_mode_ == sad::CompletionMode::none);
}

// ===========================================================================
// C2 — second claim loses
// ===========================================================================
SLUICE_TEST_CASE(c2_second_claim_loses) {
    ClaimFixture f;
    Event ev(f.sched);
    std::uint32_t w = f.add_event_arm(ev);
    std::uint32_t l = f.add_event_arm(ev);
    f.mark_candidate(w);
    f.mark_candidate(l);

    SLUICE_CHECK(f.process(w) == true);
    SLUICE_CHECK(f.group.winner() == w);

    // Second attempt with another index returns false; winner unchanged.
    SLUICE_CHECK(f.process(l) == false);
    SLUICE_CHECK(f.group.winner() == w);
    // No state mutation from the second attempt: the loser was already
    // finalized by the first process (it iterated all losers). Its state stays
    // Retired.
    SLUICE_CHECK(f.arms[l].state == ArmState::retired);
}

// ===========================================================================
// C3 — registered-processor winner stability / early claim-lost
// ===========================================================================
// After the first process linearizes the winner via select_process_group, a
// second process on the SAME registered group does NOT execute a second winner
// CAS: select_process_group_locked observes winner() != kNoWinner and returns
// false (claim-lost, no mutation) before reaching the claim preflight or the
// CAS. So this case proves the registered-processor winner-stability / early
// claim-lost path, not a second competing CAS. The underlying winner-CAS
// first-claim-wins / second-claim-loses semantics are covered by the detached
// group unit test (test_select_group_winner_claim via detached_claim_winner).
SLUICE_TEST_CASE(c3_winner_identity_stable) {
    ClaimFixture f;
    Event ev(f.sched);
    std::uint32_t w = f.add_event_arm(ev);
    std::uint32_t l = f.add_event_arm(ev);
    f.mark_candidate(w);
    f.mark_candidate(l);

    SLUICE_CHECK(f.process(w));
    const std::uint32_t first_winner = f.group.winner();
    SLUICE_CHECK(first_winner == w);

    // A second process returns claim-lost (early return on winner() != kNoWinner);
    // it does not reach the CAS. The identity is unchanged.
    SLUICE_CHECK(!f.process(l));
    SLUICE_CHECK(f.group.winner() == first_winner);

    // And a second process for the SAME index also claim-lost (winner set).
    SLUICE_CHECK(!f.process(w));
    SLUICE_CHECK(f.group.winner() == first_winner);
}

// ===========================================================================
// C4 — Event winner + Event loser
// ===========================================================================
SLUICE_TEST_CASE(c4_event_winner_and_loser) {
    ClaimFixture f;
    Event ev(f.sched);
    std::uint32_t w = f.add_event_arm(ev);
    std::uint32_t l = f.add_event_arm(ev);
    f.mark_candidate(w);
    // loser left Registered (also test CandidateReady loser below in C8).
    SLUICE_CHECK(f.process(w));

    SLUICE_CHECK(f.arms[w].state == ArmState::retired);
    SLUICE_CHECK(f.arms[l].state == ArmState::retired);
    SLUICE_CHECK(f.arms[w].home_ == nullptr);
    SLUICE_CHECK(f.arms[l].home_ == nullptr);
    SLUICE_CHECK(f.arms[w].next_ == nullptr);
    SLUICE_CHECK(f.arms[w].prev_ == nullptr);
    SLUICE_CHECK(f.arms[l].next_ == nullptr);
    SLUICE_CHECK(f.arms[l].prev_ == nullptr);
    SLUICE_CHECK(ev.is_set() == false);  // SET unchanged (never set)
    SLUICE_CHECK(f.all_closed());
}

// Event loser that is CandidateReady (not just Registered) is also finalized.
SLUICE_TEST_CASE(c4b_event_candidate_loser_finalized) {
    ClaimFixture f;
    Event ev(f.sched);
    std::uint32_t w = f.add_event_arm(ev);
    std::uint32_t l = f.add_event_arm(ev);
    f.mark_candidate(w);
    f.mark_candidate(l);  // loser is CandidateReady
    SLUICE_CHECK(f.process(w));
    SLUICE_CHECK(f.arms[l].state == ArmState::retired);
    SLUICE_CHECK(f.arms[l].home_ == nullptr);
    SLUICE_CHECK(f.all_closed());
}

// ===========================================================================
// C5 — Timer winner + Timer loser
// ===========================================================================
SLUICE_TEST_CASE(c5_timer_winner_and_loser) {
    ClaimFixture f;
    std::uint32_t w = f.add_timer_arm(/*deadline=*/100);
    std::uint32_t l = f.add_timer_arm(/*deadline=*/200);
    f.mark_candidate(w);

    const std::size_t adc_before = f.active_deadline_count();
    SLUICE_CHECK(adc_before == 2);

    SLUICE_CHECK(f.process(w));

    // winner CONSUMED, loser RETIRED, both arms Retired.
    SLUICE_CHECK(f.reg_for_arm[w]->is_consumed());
    SLUICE_CHECK(f.reg_for_arm[l]->is_retired());
    SLUICE_CHECK(f.arms[w].state == ArmState::retired);
    SLUICE_CHECK(f.arms[l].state == ArmState::retired);

    // active_deadline_count decreases exactly twice (one consume + one retire).
    SLUICE_CHECK(f.active_deadline_count() == adc_before - 2);

    // Heap entries remain for lazy physical reclamation (lazy-at-deadline).
    SLUICE_CHECK(stest::E13SelectTimerSeam::pool_size(f.sched) == 2);

    SLUICE_CHECK(f.all_closed());
}

// ===========================================================================
// C6 — Event winner + Timer loser
// ===========================================================================
SLUICE_TEST_CASE(c6_event_winner_timer_loser) {
    ClaimFixture f;
    Event ev(f.sched);
    std::uint32_t w = f.add_event_arm(ev);
    std::uint32_t l = f.add_timer_arm(/*deadline=*/100);
    f.mark_candidate(w);

    const std::size_t adc_before = f.active_deadline_count();
    SLUICE_CHECK(f.process(w));

    // Event winner unlinked.
    SLUICE_CHECK(f.arms[w].state == ArmState::retired);
    SLUICE_CHECK(f.arms[w].home_ == nullptr);
    // Timer loser RETIRED.
    SLUICE_CHECK(f.arms[l].state == ArmState::retired);
    SLUICE_CHECK(f.reg_for_arm[l]->is_retired());
    // No active Select timer authority.
    SLUICE_CHECK(f.active_deadline_count() == adc_before - 1);
    SLUICE_CHECK(f.all_closed());
}

// ===========================================================================
// C7 — Timer winner + Event loser
// ===========================================================================
SLUICE_TEST_CASE(c7_timer_winner_event_loser) {
    ClaimFixture f;
    Event ev(f.sched);
    std::uint32_t w = f.add_timer_arm(/*deadline=*/100);
    std::uint32_t l = f.add_event_arm(ev);
    f.mark_candidate(w);

    SLUICE_CHECK(f.process(w));

    // Timer winner CONSUMED.
    SLUICE_CHECK(f.reg_for_arm[w]->is_consumed());
    SLUICE_CHECK(f.arms[w].state == ArmState::retired);
    // Event loser unlinked; SET unchanged.
    SLUICE_CHECK(f.arms[l].state == ArmState::retired);
    SLUICE_CHECK(f.arms[l].home_ == nullptr);
    // P6 CORRECTIVE: ev.set() now drives the full suspended-Select resolver
    // (claim+finalize+publish) and would re-enter this already-finalized
    // synthetic armed group. The Event-persistence law (Select never clears
    // set_) is now proven directly via a pre-set Event whose SET survives the
    // Select finalize path (see c7b). Force the SET bit directly and confirm
    // it is observable — the loser finalize never touched set_.
    ev.set();
    SLUICE_CHECK(ev.is_set());
    SLUICE_CHECK(f.all_closed());
}

// Variant: Event is already SET before processing; Select must not clear it.
SLUICE_TEST_CASE(c7b_timer_winner_event_loser_set_persists) {
    ClaimFixture f;
    // Construct the Event already-SET. A pre-SET Event proves the persistence
    // law (InvEventPersistentStateNotConsumed) WITHOUT driving the production
    // ev.set() broadcast path, which under P6 would attempt to resolve the
    // synthetic armed group (no real caller fiber). Select finalizing the Event
    // loser arm must NOT clear set_.
    Event ev(f.sched, /*initially_set=*/true);
    std::uint32_t w = f.add_timer_arm(/*deadline=*/100);
    std::uint32_t l = f.add_event_arm(ev);
    f.mark_candidate(w);

    SLUICE_CHECK(ev.is_set());
    SLUICE_CHECK(f.process(w));
    SLUICE_CHECK(ev.is_set());  // still SET — Event persistent state preserved
    // Loser arm finalized too.
    SLUICE_CHECK(f.arms[l].state == ArmState::retired);
    SLUICE_CHECK(f.arms[l].home_ == nullptr);
    SLUICE_CHECK(f.all_closed());
}

// ===========================================================================
// C8 — same Event appears twice
// ===========================================================================
// Two distinct arm nodes linked to one Event. Process one candidate and verify
// one winner identity, both arms finalized, both nodes removed, no corrupt
// intrusive links. (Not the P5 lowest-index admission test; candidate supplied
// directly.)
SLUICE_TEST_CASE(c8_same_event_twice) {
    ClaimFixture f;
    Event ev(f.sched);
    std::uint32_t a = f.add_event_arm(ev);
    std::uint32_t b = f.add_event_arm(ev);
    f.mark_candidate(a);

    SLUICE_CHECK(f.process(a));

    SLUICE_CHECK(f.group.winner() == a);  // one winner identity
    SLUICE_CHECK(f.arms[a].state == ArmState::retired);
    SLUICE_CHECK(f.arms[b].state == ArmState::retired);
    // both nodes removed (no duplicate/corrupt links).
    SLUICE_CHECK(f.arms[a].home_ == nullptr);
    SLUICE_CHECK(f.arms[a].next_ == nullptr);
    SLUICE_CHECK(f.arms[a].prev_ == nullptr);
    SLUICE_CHECK(f.arms[b].home_ == nullptr);
    SLUICE_CHECK(f.arms[b].next_ == nullptr);
    SLUICE_CHECK(f.arms[b].prev_ == nullptr);
    // The Event's SelectPort is now empty (no live arms).
    SLUICE_CHECK(ev.is_set() == false);
    SLUICE_CHECK(f.all_closed());
}

// ===========================================================================
// C9 — claim-lost no mutation
// ===========================================================================
// Pre-win the group with one index, then attempt process with another. Compare
// snapshots of arm states, Event memberships, Timer states,
// active_deadline_count, group phase. No second-attempt mutation allowed.
//
// Implementation: process once (wins + finalizes), snapshot, then attempt a
// second process. Because the group is already fully finalized, the second
// process must return false and leave every observable unchanged.
SLUICE_TEST_CASE(c9_claim_lost_no_mutation) {
    ClaimFixture f;
    Event ev(f.sched);
    std::uint32_t w = f.add_event_arm(ev);
    std::uint32_t l = f.add_event_arm(ev);
    f.mark_candidate(w);
    f.mark_candidate(l);

    SLUICE_CHECK(f.process(w));

    // Snapshot after the winning process.
    const auto phase_s = f.group.phase();
    const auto winner_s = f.group.winner();
    const auto w_state = f.arms[w].state;
    const auto l_state = f.arms[l].state;
    const auto w_home = f.arms[w].home_;
    const auto l_home = f.arms[l].home_;
    const auto mode_s = f.group.completion_mode_;

    // Second attempt loses and mutates nothing.
    SLUICE_CHECK(!f.process(l));
    SLUICE_CHECK(f.group.phase() == phase_s);
    SLUICE_CHECK(f.group.winner() == winner_s);
    SLUICE_CHECK(f.arms[w].state == w_state);
    SLUICE_CHECK(f.arms[l].state == l_state);
    SLUICE_CHECK(f.arms[w].home_ == w_home);
    SLUICE_CHECK(f.arms[l].home_ == l_home);
    SLUICE_CHECK(f.group.completion_mode_ == mode_s);
}

// Timer variant: second process leaves Timer states + active_deadline_count
// unchanged.
SLUICE_TEST_CASE(c9b_claim_lost_timer_no_mutation) {
    ClaimFixture f;
    std::uint32_t w = f.add_timer_arm(100);
    std::uint32_t l = f.add_timer_arm(200);
    f.mark_candidate(w);
    SLUICE_CHECK(f.process(w));

    const auto adc_s = f.active_deadline_count();
    const auto w_state = f.reg_for_arm[w]->state();
    const auto l_state = f.reg_for_arm[l]->state();

    SLUICE_CHECK(!f.process(l));
    SLUICE_CHECK(f.active_deadline_count() == adc_s);
    SLUICE_CHECK(f.reg_for_arm[w]->state() == w_state);
    SLUICE_CHECK(f.reg_for_arm[l]->state() == l_state);
}

// ===========================================================================
// C10 — fully finalized invariant
// ===========================================================================
// The authority-closed predicate is false before processing and true after.
SLUICE_TEST_CASE(c10_authority_closed_predicate) {
    ClaimFixture f;
    Event ev(f.sched);
    std::uint32_t w = f.add_event_arm(ev);
    std::uint32_t l = f.add_event_arm(ev);
    f.mark_candidate(w);

    // Before processing: no winner AND the loser's Event authority is open
    // (home_ != nullptr), so the predicate must be false.
    SLUICE_CHECK(f.arms[l].home_ != nullptr);
    SLUICE_CHECK(!f.all_closed());
    SLUICE_CHECK(f.process(w));
    SLUICE_CHECK(f.all_closed());   // true after successful processing
}

// The predicate also rejects a group whose winner is set but an authority is
// still open (defensive: proves the predicate checks each arm, not just the
// winner). After a normal process everything is closed; we then synthetically
// re-open one arm's Event authority (home_ is a public field on SelectArmSlot)
// and confirm the predicate flips to false. The sentinel is never dereferenced
// (the predicate only checks non-null), so a reinterpret_cast is safe here.
SLUICE_TEST_CASE(c10b_predicate_rejects_open_authority) {
    ClaimFixture f;
    Event ev(f.sched);
    std::uint32_t w = f.add_event_arm(ev);
    f.mark_candidate(w);
    SLUICE_CHECK(f.process(w));
    SLUICE_CHECK(f.all_closed());

    // Synthetically re-open the winner's Event authority: home_ != nullptr
    // while the winner is set. The predicate must return false.
    auto* sentinel = reinterpret_cast<sad::SelectPort*>(0x1);  // never dereferenced
    f.arms[w].home_ = sentinel;
    SLUICE_CHECK(!f.all_closed());
    // restore for clean teardown
    f.arms[w].home_ = nullptr;
    SLUICE_CHECK(f.all_closed());
}

// ===========================================================================
// C11 — no publication
// ===========================================================================
// After successful processing: phase remains Selecting or Armed; completion_mode
// remains None; no Fiber becomes Runnable; no result is produced. We cannot
// observe runnable_count delta without fibers, but we assert phase + mode +
// that the (absent) result publication left no trace. The group has no result_
// field in P4 (publication is denied), so we assert the two observable gates.
SLUICE_TEST_CASE(c11_no_publication) {
    ClaimFixture f;
    Event ev(f.sched);
    std::uint32_t w = f.add_event_arm(ev);
    std::uint32_t l = f.add_event_arm(ev);
    f.mark_candidate(w);

    const auto phase_before = f.group.phase();
    const auto mode_before = f.group.completion_mode_;
    const auto runnable_before = f.sched.runnable_count();

    SLUICE_CHECK(f.process(w));

    // phase remains Selecting or Armed (unchanged from before).
    SLUICE_CHECK(f.group.phase() == phase_before);
    SLUICE_CHECK((f.group.phase() == GroupPhase::selecting ||
                  f.group.phase() == GroupPhase::armed));
    // completion_mode remains None.
    SLUICE_CHECK(f.group.completion_mode_ == sad::CompletionMode::none);
    SLUICE_CHECK(f.group.completion_mode_ == mode_before);
    // No Fiber became Runnable.
    SLUICE_CHECK(f.sched.runnable_count() == runnable_before);
    // No result is produced: P4 does not construct a SelectResult.
    (void)l;
}

// ===========================================================================
// C12 — Timer loser ordering (SN-9)
// ===========================================================================
// Mechanically prove: arm.state became Retired WHILE the stable registration
// was still ACTIVE, THEN the registration became RETIRED. Use the
// select_timer_loser_arm_classified phase seam: arm it, run a process with a
// Timer winner + Timer loser. At the seam the controller observes the loser
// arm is Retired and its registration is still ACTIVE. After release, the
// retire CAS flips it to RETIRED. No wall-clock timing.
SLUICE_TEST_CASE(c12_timer_loser_ordering) {
    ClaimFixture f;
    std::uint32_t w = f.add_timer_arm(100);
    std::uint32_t l = f.add_timer_arm(200);
    f.mark_candidate(w);

    // Arm the SN-9 phase: the loser-arm-classified point. Use a coordinator
    // thread that runs the process while the main thread observes the seam.
    // Because the seam BLOCKS the process thread at the classification point,
    // we run process on a worker thread and assert from the main thread.
    sluice_async_test::arm(f.sched, stest::PhaseTag::select_timer_loser_arm_classified);

    std::atomic<bool> done{false};
    std::thread worker([&] {
        bool won = f.process(w);
        // After release below, process completes.
        SLUICE_CHECK_MSG(won, "process should win the claim");
        done.store(true, std::memory_order::release);
    });

    // Wait until the process thread is paused at the classification seam.
    sluice_async_test::wait_paused(f.sched,
                                   stest::PhaseTag::select_timer_loser_arm_classified);

    // SN-9 mechanical observation: at the seam, the loser arm is Retired AND
    // its registration is STILL ACTIVE. (read under the controller's mtx via a
    // best-effort snapshot — the process thread is blocked at the seam holding
    // global_mtx_, so the state is stable.)
    SLUICE_CHECK_MSG(f.arms[l].state == ArmState::retired,
                     "loser arm classified Retired before retire CAS");
    SLUICE_CHECK_MSG(f.reg_for_arm[l]->is_active(),
                     "loser registration still ACTIVE at arm classification (SN-9)");

    // Release: the retire CAS proceeds, flipping ACTIVE -> RETIRED.
    sluice_async_test::release(f.sched,
                               stest::PhaseTag::select_timer_loser_arm_classified);
    worker.join();
    SLUICE_CHECK(done.load());

    SLUICE_CHECK(f.reg_for_arm[l]->is_retired());
    SLUICE_CHECK(f.arms[l].state == ArmState::retired);
    SLUICE_CHECK(f.all_closed());
}

SLUICE_MAIN()
