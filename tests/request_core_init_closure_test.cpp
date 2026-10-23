#include "request_core_test_driver.hpp"

#include <sluice/async/detail/request_core.hpp>

#include <cstdio>
#include <limits>

namespace {

using sluice::FileKind;
using sluice::async::detail::ObserverPhase;
using sluice::async::detail::RequestOp;
using sluice::async::detail::ReserveStatus;
using sluice::async::detail::SlotIndex;
using sluice_request_core_test::AcceptAttempt;
using sluice_request_core_test::BindingRelease;
using sluice_request_core_test::ExecutionClaim;
using sluice_request_core_test::ExecutionRelease;
using sluice_request_core_test::FakePhysicalDriver;
using sluice_request_core_test::Generation;
using sluice_request_core_test::ObserverDeliveryClaim;
using sluice_request_core_test::ObserverDeliveryRetirement;
using sluice_request_core_test::IoOutcome;
using sluice_request_core_test::PublicationCompletion;
using sluice_request_core_test::PublicationGrant;
using sluice_request_core_test::PublicationTarget;
using sluice_request_core_test::PublicCancel;
using sluice_request_core_test::RequestCore;
using sluice_request_core_test::RequestKey;
using sluice_request_core_test::ReserveAttempt;
using sluice_request_core_test::TerminalCandidate;
using sluice_request_core_test::TerminalCandidateKind;
using sluice_request_core_test::TerminalVerdict;

int g_failures = 0;

void check(bool ok, const char* label) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s\n", label);
    }
}

RequestCore make_core(std::size_t capacity = 4) {
    return RequestCore(sluice::async::detail::ContextIdentity::for_testing(91), capacity);
}

RequestKey key_of(const RequestCore& core, const ReserveAttempt& reserved) {
    return RequestKey{core.context(), reserved.reservation.slot, reserved.reservation.generation};
}

// The state every slot must present while it sits on the free list:
// indistinguishable from a value-initialized slot (modulo generation).
// reserve() re-initializes nothing beyond phase, so every acquire-path-
// invisible field is trusted from this precondition; each assertion names
// the stale field its absence would leak into the next acquire.
bool free_list_clean(const RequestCore& core, std::uint32_t index, std::uint64_t generation) {
    const auto observed = core.observe_slot(SlotIndex{index});
    if (!observed.has_value()) {
        return false;
    }
    const auto& s = *observed;
    return s.phase == RequestCore::SlotPhase::free && s.generation.value == generation &&
           !s.accepted && !s.binding_live && !s.terminal_chosen && !s.published &&
           !s.publication_inflight && !s.execution_claimed && !s.cancel_intent &&
           s.observer_phase == ObserverPhase::unattached && s.execution_refs == 0 &&
           s.control_refs == 0 && s.op == RequestOp::read && s.offset == 0 &&
           s.requested_bytes == 0 && !s.has_outcome && s.outcome.succeeded == false &&
           s.outcome.effect.confirmed_bytes == 0 && s.metadata.kind == FileKind::other &&
           s.metadata.size == 0 && !s.metadata.identity.has_value();
}

// The mid-life state after accept with a distinctive descriptor: only the
// accept-written fields may move; outcome, metadata, publication and observer
// state stay at their free-list values.
bool accepted_distinctive(const RequestCore& core, std::uint32_t index, std::uint64_t generation,
                          std::uint64_t offset, std::uint64_t length) {
    const auto observed = core.observe_slot(SlotIndex{index});
    if (!observed.has_value()) {
        return false;
    }
    const auto& s = *observed;
    return s.phase == RequestCore::SlotPhase::accepted && s.generation.value == generation &&
           s.accepted && s.binding_live && !s.terminal_chosen && !s.published &&
           !s.publication_inflight && !s.execution_claimed && !s.cancel_intent &&
           s.observer_phase == ObserverPhase::unattached && s.execution_refs == 1 &&
           s.control_refs == 0 && s.op == RequestOp::read && s.offset == offset &&
           s.requested_bytes == length && !s.has_outcome;
}

// Drives one full terminal lifecycle carrying distinctive bytes and metadata
// and returns once the slot has been reclaimed onto the free list.
void drive_full_lifecycle(RequestCore& core, FakePhysicalDriver& driver) {
    ReserveAttempt reserved = driver.reserve();
    check(reserved.ok(), "reserve succeeded");
    const std::uint32_t index = reserved.reservation.slot.value;
    const std::uint64_t generation = reserved.reservation.generation.value;

    AcceptAttempt accepted = driver.accept(reserved.reservation, 0xdeadbeef, 4096);
    check(accepted.ok(), "accept succeeded");
    check(accepted.id == key_of(core, reserved), "key derived from the reservation");
    check(accepted_distinctive(core, index, generation, 0xdeadbeef, 4096),
          "accepted state holds only accept-written fields");

    check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "execution claimed");
    TerminalCandidate metadata_terminal;
    metadata_terminal.kind = TerminalCandidateKind::physical_outcome;
    metadata_terminal.outcome = IoOutcome::success(4096);
    metadata_terminal.has_metadata = true;
    metadata_terminal.metadata = sluice::FileInfo{FileKind::regular, 0x9abc, {}};
    check(core.offer_terminal(accepted.id, metadata_terminal) == TerminalVerdict::chosen,
          "metadata terminal chosen");
    check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
          "execution fully retired");
    PublicationTarget target;
    check(driver.publish_to(accepted.id, target) == PublicationGrant::granted,
          "publication granted");
    check(core.observe_slot(SlotIndex{index})->publication_inflight,
          "publication pin visible before completion");
    check(driver.finish_publication(accepted.id) == PublicationCompletion::completed,
          "publication completed");
    check(driver.discard_public_result(accepted.id) == BindingRelease::released,
          "binding released");

    check(free_list_clean(core, index, generation + 1),
          "reclaimed slot is indistinguishable from a fresh slot with generation+1");
}

}  // namespace

int main() {
    constexpr std::size_t kCapacity = 4;

    // State 1: first construction — every slot presents the value-initialized
    // state on the free list.
    {
        RequestCore core = make_core(kCapacity);
        for (std::uint32_t i = 0; i < kCapacity; ++i) {
            check(free_list_clean(core, i, 0), "constructed slot is clean");
        }
        check(core.snapshot().free_slots == kCapacity, "all slots on the free list");
    }

    // State 2: first reserve writes the phase onto a clean slot.
    {
        RequestCore core = make_core(kCapacity);
        FakePhysicalDriver driver(core);
        ReserveAttempt reserved = driver.reserve();
        check(reserved.ok(), "first reserve succeeded");
        const auto observed = core.observe_slot(reserved.reservation.slot);
        check(observed.has_value() &&
                  observed->phase == RequestCore::SlotPhase::reserved && !observed->accepted &&
                  !observed->binding_live && !observed->published &&
                  observed->observer_phase == ObserverPhase::unattached,
              "reserved state is phase-only on a clean slot");
        AcceptAttempt accepted = driver.accept(reserved.reservation, 0, 0, true);
        check(accepted.ok(), "zero-op accept succeeded");
        PublicationTarget target;
        check(driver.publish_to(accepted.id, target) == PublicationGrant::granted,
              "zero-op publication granted");
        check(driver.finish_publication(accepted.id) == PublicationCompletion::completed,
              "zero-op publication completed");
        check(driver.discard_public_result(accepted.id) == BindingRelease::released,
              "zero-op binding released");
        check(free_list_clean(core, reserved.reservation.slot.value,
                              reserved.reservation.generation.value + 1),
              "zero-op slot reclaimed clean");
    }

    // State 4 (exercised early: later cleanup paths rely on it):
    // rollback returns the slot clean and kills the reservation
    // identity; the slot returns to service.
    {
        RequestCore core = make_core(kCapacity);
        FakePhysicalDriver driver(core);
        ReserveAttempt reserved = driver.reserve();
        check(reserved.ok(), "reserve for rollback succeeded");
        const std::uint64_t generation = reserved.reservation.generation.value;
        check(driver.rollback(reserved.reservation), "rollback succeeded");
        check(free_list_clean(core, reserved.reservation.slot.value, generation + 1),
              "rolled-back slot is clean with generation+1");
        check(!driver.rollback(reserved.reservation), "the reservation identity is dead");
        ReserveAttempt served = driver.reserve();
        check(served.ok() && served.reservation.slot.value == reserved.reservation.slot.value,
              "rolled-back slot returns to service");
        check(driver.rollback(served.reservation), "service reserve released for teardown");
    }

    // States 3/5/6/8/9: full lifecycle with distinctive bytes and metadata
    // reclaims clean; the reclaimed slot is the next one served (free-list
    // LIFO) with the generation advanced.
    {
        RequestCore core = make_core(kCapacity);
        FakePhysicalDriver driver(core);
        drive_full_lifecycle(core, driver);
        ReserveAttempt again = driver.reserve();
        check(again.ok() && again.reservation.slot.value == 0,
              "the reclaimed slot is the next one served");
        check(again.reservation.generation.value == 1, "generation advanced across reuse");
        check(driver.rollback(again.reservation), "reuse reserve released for teardown");
    }

    // States 6/13: a cancel that wins before execution reclaims clean through
    // the same publication/discard edges as a physical terminal.
    {
        RequestCore core = make_core(kCapacity);
        FakePhysicalDriver driver(core);
        ReserveAttempt reserved = driver.reserve();
        AcceptAttempt accepted = driver.accept(reserved.reservation, 0, 512);
        RequestKey id = accepted.id;
        check(driver.register_observer(id) ==
                  sluice::async::detail::ObserverRegistration::armed,
              "observer armed");
        check(driver.cancel(id) == sluice::async::detail::PublicCancel::won_before_execution,
              "cancel wins before execution");
        check(driver.retire_execution(id) == ExecutionRelease::borrow_touch_fully_retired,
              "cancel-terminal execution retires");
        PublicationTarget target;
        check(driver.publish_to(id, target) == PublicationGrant::granted,
              "cancel-won publication granted");
        check(driver.finish_publication(id) == PublicationCompletion::completed,
              "cancel-won publication completed");
        check(driver.claim_observer_delivery(id) == ObserverDeliveryClaim::claimed,
              "armed observer claimed for delivery");
        check(driver.retire_observer_delivery(id) == ObserverDeliveryRetirement::retired,
              "delivered observer retired");
        check(driver.discard_public_result(id) == BindingRelease::released,
              "cancel-won binding released");
        check(free_list_clean(core, reserved.reservation.slot.value,
                              reserved.reservation.generation.value + 1),
              "cancel-won slot reclaimed clean");
    }

    // State 7: observer registration on a published slot is rejected as
    // already-terminal and the consumed slot still reclaims clean.
    {
        RequestCore core = make_core(kCapacity);
        FakePhysicalDriver driver(core);
        ReserveAttempt reserved = driver.reserve();
        AcceptAttempt accepted = driver.accept(reserved.reservation, 0, 512);
        RequestKey id = accepted.id;
        check(driver.claim_execution(id) == ExecutionClaim::claimed, "claimed");
        check(driver.offer_physical_success(id, 512) == TerminalVerdict::chosen,
              "terminal chosen");
        check(driver.retire_execution(id) == ExecutionRelease::borrow_touch_fully_retired,
              "retired");
        PublicationTarget target;
        check(driver.publish_to(id, target) == PublicationGrant::granted, "granted");
        check(driver.finish_publication(id) == PublicationCompletion::completed, "completed");
        check(driver.register_observer(id) ==
                  sluice::async::detail::ObserverRegistration::already_terminal,
              "late observer rejected on the published slot");
        check(driver.discard_public_result(id) == BindingRelease::released, "released");
        check(free_list_clean(core, reserved.reservation.slot.value,
                              reserved.reservation.generation.value + 1),
              "consumed slot reclaimed clean");
    }

    // States 10/11: generation discipline — a slot advances one generation
    // per release; the saturated generation retires the slot and it never
    // returns to the free list; the exhausted core refuses reserve.
    {
        RequestCore core = make_core(kCapacity);
        FakePhysicalDriver driver(core);
        const std::uint64_t saturated = std::numeric_limits<std::uint64_t>::max();
        for (std::uint32_t i = 0; i < kCapacity; ++i) {
            core.force_generation_for_testing(SlotIndex{i}, Generation{saturated - 1});
        }
        for (std::size_t i = 0; i < kCapacity; ++i) {
            ReserveAttempt r = driver.reserve();
            check(r.ok() && r.reservation.generation.value == saturated - 1,
                  "the pre-saturated generation is served");
            check(driver.rollback(r.reservation), "release advances the generation");
            check(free_list_clean(core, r.reservation.slot.value, saturated),
                  "the slot re-enters the free list at the saturated generation");
            ReserveAttempt r2 = driver.reserve();
            check(r2.ok() && r2.reservation.slot.value == r.reservation.slot.value &&
                      r2.reservation.generation.value == saturated,
                  "the saturated generation is served");
            check(driver.rollback(r2.reservation),
                  "release at the saturated generation retires");
            const auto retired =
                core.observe_slot(SlotIndex{r.reservation.slot.value});
            check(retired.has_value() && retired->phase == RequestCore::SlotPhase::retired,
                  "the saturated slot retired");
        }
        check(core.snapshot().free_slots == 0, "no slot remains on the free list");
        check(core.snapshot().retired_slots == kCapacity, "every slot retired");
        ReserveAttempt after = driver.reserve();
        check(!after.ok() && after.status == ReserveStatus::exhausted,
              "the exhausted core refuses reserve");
    }

    // State 14: closed admission refuses reserve and leaves the free list
    // untouched; poison marks health without touching slots.
    {
        RequestCore core = make_core(kCapacity);
        FakePhysicalDriver driver(core);
        check(driver.close_admission(), "admission closed");
        ReserveAttempt refused = driver.reserve();
        check(!refused.ok() && refused.status == ReserveStatus::admission_closed,
              "closed admission refuses reserve");
        for (std::uint32_t i = 0; i < kCapacity; ++i) {
            check(free_list_clean(core, i, 0), "free list unchanged by shutdown");
        }
    }
    {
        RequestCore core = make_core(kCapacity);
        FakePhysicalDriver driver(core);
        driver.note_health_failure();
        check(core.health_failed(), "poison observed");
        check(!driver.reserve().ok(), "poisoned core refuses reserve");
    }

    // State 13 witness: alternating cancel/terminal rounds through the same
    // recycled slot pair; every release lands clean. The concurrent
    // interleaving itself is owned by submit_concurrency_test and the
    // protocol suite.
    {
        RequestCore core = make_core(2);
        FakePhysicalDriver driver(core);
        for (int round = 0; round < 50; ++round) {
            ReserveAttempt r1 = driver.reserve();
            ReserveAttempt r2 = driver.reserve();
            check(r1.ok() && r2.ok(), "two reservations at capacity 2");
            AcceptAttempt a1 = driver.accept(r1.reservation, 0, 64);
            check(a1.ok(), "first accept");
            if (round % 2 == 0) {
                check(driver.cancel(a1.id) ==
                          sluice::async::detail::PublicCancel::won_before_execution,
                      "cancel wins");
                check(driver.retire_execution(a1.id) ==
                          ExecutionRelease::borrow_touch_fully_retired,
                      "cancel-terminal execution retires");
            } else {
                check(driver.claim_execution(a1.id) == ExecutionClaim::claimed,
                      "execution claimed");
                check(driver.offer_physical_success(a1.id, 64) == TerminalVerdict::chosen,
                      "terminal chosen");
                check(driver.retire_execution(a1.id) ==
                          ExecutionRelease::borrow_touch_fully_retired,
                      "terminal execution retires");
            }
            PublicationTarget target;
            const PublicationGrant grant = driver.publish_to(a1.id, target);
            check(grant == PublicationGrant::granted, "publication granted");
            check(driver.finish_publication(a1.id) == PublicationCompletion::completed,
                  "publication completed");
            check(driver.discard_public_result(a1.id) == BindingRelease::released,
                  "binding released");
            check(free_list_clean(core, r1.reservation.slot.value,
                                  r1.reservation.generation.value + 1),
                  "interleaved slot reclaimed clean");
            check(driver.rollback(r2.reservation), "second reservation rolled back");
        }
    }

    if (g_failures == 0) {
        std::printf("request_core_init_closure: all states clean\n");
        return 0;
    }
    std::printf("request_core_init_closure: failures=%d\n", g_failures);
    return 1;
}
