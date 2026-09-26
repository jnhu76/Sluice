#include "request_core_test_driver.hpp"

#include <sluice/async/detail/request_core.hpp>

#include <cstdio>
#include <limits>

namespace {

using sluice::IoError;
using sluice::async::detail::CoreSnapshot;
using sluice::async::detail::ReserveStatus;
using sluice_request_core_test::AcceptAttempt;
using sluice_request_core_test::BindingRelease;
using sluice_request_core_test::ExecutionClaim;
using sluice_request_core_test::ExecutionRelease;
using sluice_request_core_test::FakePhysicalDriver;
using sluice_request_core_test::Generation;
using sluice_request_core_test::ObserverCancellation;
using sluice_request_core_test::ObserverDeliveryClaim;
using sluice_request_core_test::ObserverDeliveryRetirement;
using sluice_request_core_test::ObserverPhase;
using sluice_request_core_test::ObserverRegistration;
using sluice_request_core_test::IoOutcome;
using sluice_request_core_test::PublicationCompletion;
using sluice_request_core_test::PublicationGrant;
using sluice_request_core_test::PublicationPayload;
using sluice_request_core_test::PublicationTarget;
using sluice_request_core_test::PublicCancel;
using sluice_request_core_test::PublicLookup;
using sluice_request_core_test::RequestCore;
using sluice_request_core_test::RequestKey;
using sluice_request_core_test::RequestReservation;
using sluice_request_core_test::ReserveAttempt;
using sluice_request_core_test::TerminalCandidateKind;
using sluice_request_core_test::TerminalVerdict;

struct Tracker {
    const char* name;
    int failures = 0;

    void check(bool ok, const char* label) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s\n", name, label);
        }
    }
};

RequestCore make_core(std::size_t capacity = 4) {
    return RequestCore(sluice::async::detail::ContextIdentity::for_testing(77), capacity);
}

RequestKey key_of(const RequestCore& core, RequestReservation reservation) {
    return RequestKey{core.context(), reservation.slot, reservation.generation};
}

bool reserve_creates_no_public_identity(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    t.check(attempt.ok(), "reserve succeeds");
    RequestKey id = key_of(core, attempt.reservation);
    t.check(driver.lookup(id) == PublicLookup::not_found, "reserved id not publicly resolvable");
    t.check(driver.cancel(id) == PublicCancel::not_found, "reserved id not publicly cancelable");
    t.check(driver.release_public_binding(id) == BindingRelease::stale,
            "reserved id has no releasable binding");
    CoreSnapshot snap = core.snapshot();
    t.check(snap.reserved == 1, "one reserved slot");
    t.check(snap.accepted_live == 0, "reservation is not acceptance");
    t.check(snap.public_bindings == 0, "no live binding pre-accept");
    t.check(snap.execution_refs == 0, "no execution obligation pre-accept");
    t.check(driver.rollback(attempt.reservation), "reservation rolled back");
    driver.settle_all();
    return t.failures == 0;
}

bool rollback_restores_capacity_and_leaves_no_residue(Tracker& t) {
    RequestCore core = make_core(2);
    FakePhysicalDriver driver(core);
    ReserveAttempt first = driver.reserve();
    t.check(first.ok(), "first reserve succeeds");
    t.check(driver.rollback(first.reservation), "rollback succeeds");
    ReserveAttempt second = driver.reserve();
    t.check(second.ok(), "capacity restored after rollback");
    t.check(second.reservation.slot == first.reservation.slot,
            "rolled back slot is reusable (single slot core)");
    CoreSnapshot snap = core.snapshot();
    t.check(snap.reserved == 1 && snap.accepted_live == 0 && snap.execution_refs == 0 &&
                snap.control_refs == 0 && snap.public_bindings == 0 && snap.publication_refs == 0,
            "rollback leaves no borrow, ref or binding residue");
    t.check(!driver.rollback(first.reservation), "double rollback rejected");
    t.check(driver.rollback(second.reservation), "probe reservation rolled back");
    driver.settle_all();
    return t.failures == 0;
}

bool accept_activates_identity_exactly_once(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 128);
    t.check(accepted.ok(), "accept succeeds");
    RequestKey id = accepted.id;
    t.check(driver.lookup(id) == PublicLookup::outstanding, "accepted id publicly resolvable");
    AcceptAttempt second = driver.accept(attempt.reservation, 0, 128);
    t.check(second.status == sluice::async::detail::AcceptStatus::bad_reservation,
            "double accept rejected");
    CoreSnapshot snap = core.snapshot();
    t.check(snap.accepted_live == 1 && snap.reserved == 0, "exactly one accepted request");
    driver.settle_all();
    return t.failures == 0;
}

bool accept_close_race_has_one_serialized_winner(Tracker& t) {
    RequestCore closed_core = make_core();
    FakePhysicalDriver closed_driver(closed_core);
    ReserveAttempt pending = closed_driver.reserve();
    closed_driver.close_admission();
    AcceptAttempt refused = closed_driver.accept(pending.reservation, 0, 8);
    t.check(refused.status == sluice::async::detail::AcceptStatus::admission_closed,
            "close before accept refuses acceptance");
    t.check(closed_driver.lookup(key_of(closed_core, pending.reservation)) ==
                PublicLookup::not_found,
            "refused reservation has no public identity");
    t.check(closed_driver.rollback(pending.reservation), "refused reservation rolls back");
    CoreSnapshot snap = closed_core.snapshot();
    t.check(snap.accepted_live == 0 && snap.reserved == 0 && !snap.admission_open,
            "no accepted residue after lost race");

    RequestCore open_core = make_core();
    FakePhysicalDriver open_driver(open_core);
    ReserveAttempt attempt = open_driver.reserve();
    AcceptAttempt accepted = open_driver.accept(attempt.reservation, 0, 8);
    t.check(accepted.ok(), "accept before close wins the race");
    open_driver.close_admission();
    t.check(open_driver.lookup(accepted.id) == PublicLookup::outstanding,
            "accepted work remains represented after close");
    t.check(open_driver.cancel(accepted.id) == PublicCancel::won_before_execution,
            "accepted work remains cancelable after close");
    t.check(open_driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "won cancel retires the pending dispatch obligation");
    open_driver.settle_all();
    return t.failures == 0;
}

bool post_accept_failure_converges_through_terminal(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 64);
    t.check(accepted.ok(), "accept succeeds");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "execution claimed");
    TerminalVerdict verdict = driver.offer_physical_error(
        accepted.id, IoError{.code = IoError::Code::backend_error, .os_errno = 5}, 32, true);
    t.check(verdict == TerminalVerdict::chosen, "post-accept failure becomes terminal candidate");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "execution retires after failure");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted,
            "failed request still publishes");
    t.check(target.acquire_ready(), "failure result is visible");
    t.check(!target.acquired_payload().outcome.succeeded &&
                target.acquired_payload().outcome.effect.confirmed_bytes == 32 &&
                target.acquired_payload().outcome.effect.remaining ==
                    sluice::detail::EffectCertainty::unknown,
            "canonical failure keeps confirmed prefix and unknown remainder");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed,
            "publication epilogue completes");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released,
            "binding release accepted");
    t.check(core.snapshot().accepted_live == 0, "slot reclaimed after settlement");
    driver.settle_all();
    return t.failures == 0;
}

bool zero_op_accepts_and_settles_without_execution(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 0, true);
    t.check(accepted.ok(), "zero-op accepted");
    auto observation = core.observe_slot(accepted.id.slot);
    t.check(observation.has_value() && observation->terminal_chosen && observation->execution_refs == 0,
            "zero-op chooses terminal at acceptance with no execution obligation");
    t.check(observation->outcome.succeeded && observation->outcome.effect.confirmed_bytes == 0,
            "zero-op terminal is success with zero bytes");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted,
            "zero-op publication immediately eligible");
    t.check(target.acquire_ready(), "zero-op result visible");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed,
            "zero-op epilogue completes");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released,
            "zero-op binding released");
    t.check(core.snapshot().accepted_live == 0, "zero-op slot reclaimed");
    driver.settle_all();
    return t.failures == 0;
}

bool wrong_context_is_rejected(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 16);
    RequestKey foreign = accepted.id;
    foreign.context = sluice::async::detail::ContextIdentity::for_testing(78);
    t.check(driver.lookup(foreign) == PublicLookup::not_found, "foreign context lookup rejected");
    t.check(driver.cancel(foreign) == PublicCancel::not_found, "foreign context cancel rejected");
    t.check(driver.offer_physical_success(foreign, 1) == TerminalVerdict::rejected_stale,
            "foreign context terminal rejected");
    driver.settle_all();
    return t.failures == 0;
}

bool wrong_generation_is_rejected(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 16);
    RequestKey shifted = accepted.id;
    shifted.generation = sluice::async::detail::Generation{accepted.id.generation.value + 1};
    t.check(driver.lookup(shifted) == PublicLookup::not_found, "wrong generation lookup rejected");
    t.check(driver.offer_physical_success(shifted, 1) == TerminalVerdict::rejected_stale,
            "wrong generation terminal rejected");
    driver.settle_all();
    return t.failures == 0;
}

bool settled_work_admits_no_new_control_pin(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 16);
    t.check(driver.acquire_control(accepted.id), "control pin acquired before settlement");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 16) == TerminalVerdict::chosen, "terminal");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "execution retired");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released,
            "binding released while the control pin lives");
    t.check(!driver.acquire_control(accepted.id),
            "settled work admits no new control bookkeeping");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::accepted,
            "the existing pin still holds the slot");
    t.check(driver.retire_control(accepted.id) == sluice::async::detail::ControlRelease::released,
            "the existing control pin retires");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "the final control release reclaims in the same call");
    t.check(!driver.acquire_control(accepted.id),
            "a reclaimed identity admits no control bookkeeping either");
    driver.settle_all();
    return t.failures == 0;
}

bool released_identity_stays_invalid_while_control_pin_lives(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 16);
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 16) == TerminalVerdict::chosen, "terminal");
    t.check(driver.acquire_control(accepted.id), "control pin acquired");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "execution retired");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released,
            "binding released");
    t.check(driver.lookup(accepted.id) == PublicLookup::not_found,
            "public id invalid immediately after release");
    t.check(driver.cancel(accepted.id) == PublicCancel::not_found,
            "public cancel invalid after release");
    auto observation = core.observe_slot(accepted.id.slot);
    t.check(observation.has_value() && observation->phase == RequestCore::SlotPhase::accepted,
            "control pin keeps slot alive after release");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::stale,
            "double release rejected");
    t.check(driver.retire_control(accepted.id) == sluice::async::detail::ControlRelease::released,
            "control retires");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "slot reclaimed by final control retirement alone");
    driver.settle_all();
    return t.failures == 0;
}

bool reuse_advances_generation_without_aliasing(Tracker& t) {
    RequestCore core = make_core(1);
    FakePhysicalDriver driver(core);
    ReserveAttempt first = driver.reserve();
    const auto old_generation = first.reservation.generation;
    AcceptAttempt accepted = driver.accept(first.reservation, 0, 16);
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 16) == TerminalVerdict::chosen, "terminal");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "execution retired");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    auto freed = core.observe_slot(accepted.id.slot);
    t.check(freed->phase == RequestCore::SlotPhase::free &&
                freed->generation.value == old_generation.value + 1,
            "reclaim advances generation");

    ReserveAttempt second = driver.reserve();
    t.check(second.ok() && second.reservation.slot == accepted.id.slot &&
                second.reservation.generation.value == old_generation.value + 1,
            "slot reused with advanced generation");
    t.check(driver.lookup(accepted.id) == PublicLookup::not_found,
            "old id never resolves to reused slot");
    t.check(driver.cancel(accepted.id) == PublicCancel::not_found, "old id cancel stale");
    t.check(driver.offer_physical_success(accepted.id, 4) == TerminalVerdict::rejected_stale,
            "old id terminal stale");
    t.check(driver.retire_control(accepted.id) == sluice::async::detail::ControlRelease::stale,
            "old id control release stale");
    t.check(driver.rollback(second.reservation), "alias probe reservation rolled back");
    driver.settle_all();
    return t.failures == 0;
}

bool generation_exhaustion_retires_slot(Tracker& t) {
    RequestCore core = make_core(1);
    FakePhysicalDriver driver(core);
    core.force_generation_for_testing(sluice::async::detail::SlotIndex{0},
                                      sluice::async::detail::Generation{
                                          std::numeric_limits<std::uint64_t>::max()});
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(accepted.ok(), "final generation accept");
    t.check(driver.cancel(accepted.id) == PublicCancel::won_before_execution,
            "pre-claim cancel wins");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "won cancel retires the pending dispatch obligation");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    auto slot = core.observe_slot(sluice::async::detail::SlotIndex{0});
    t.check(slot->phase == RequestCore::SlotPhase::retired, "exhausted slot permanently retired");
    t.check(core.snapshot().retired_slots == 1, "retire counted");
    ReserveAttempt next = driver.reserve();
    t.check(next.status == ReserveStatus::exhausted,
            "all slots permanently unavailable reports exhaustion, not capacity");
    driver.settle_all();
    return t.failures == 0;
}

bool first_admissible_terminal_wins(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 32);
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 32) == TerminalVerdict::chosen,
            "first candidate wins");
    t.check(driver.offer_physical_error(accepted.id, IoError{.code = IoError::Code::no_space}) ==
                TerminalVerdict::rejected_duplicate,
            "second candidate cannot overwrite");
    t.check(driver.offer_zero_effect_cancel(accepted.id) == TerminalVerdict::rejected_duplicate,
            "cancel cannot overwrite a chosen physical outcome");
    auto observation = core.observe_slot(accepted.id.slot);
    t.check(observation->outcome.succeeded && observation->outcome.effect.confirmed_bytes == 32,
            "canonical winner preserved");
    driver.settle_all();
    return t.failures == 0;
}

bool duplicate_and_stale_events_do_not_double_release(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::late, "double claim late");
    t.check(driver.offer_physical_success(accepted.id, 8) == TerminalVerdict::chosen, "terminal");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "first release retires");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::underflow_rejected,
            "duplicate execution release rejected");
    t.check(driver.acquire_control(accepted.id), "control acquired");
    t.check(driver.retire_control(accepted.id) == sluice::async::detail::ControlRelease::released,
            "control released");
    t.check(driver.retire_control(accepted.id) == sluice::async::detail::ControlRelease::underflow_rejected,
            "duplicate control release rejected");
    auto observation = core.observe_slot(accepted.id.slot);
    t.check(observation->execution_refs == 0 && observation->control_refs == 0,
            "no ref corruption from duplicate events");
    driver.settle_all();
    return t.failures == 0;
}

bool stale_event_cannot_touch_reused_generation(Tracker& t) {
    RequestCore core = make_core(1);
    FakePhysicalDriver driver(core);
    ReserveAttempt first = driver.reserve();
    AcceptAttempt old_id = driver.accept(first.reservation, 0, 8);
    t.check(driver.cancel(old_id.id) == PublicCancel::won_before_execution, "old request settles");
    t.check(driver.retire_execution(old_id.id) == ExecutionRelease::borrow_touch_fully_retired,
                "won cancel retires the pending dispatch obligation");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(old_id.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(old_id.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(old_id.id) == BindingRelease::released, "released");
    t.check(core.observe_slot(old_id.id.slot)->phase == RequestCore::SlotPhase::free, "freed");

    ReserveAttempt second = driver.reserve();
    AcceptAttempt reused = driver.accept(second.reservation, 0, 8);
    t.check(reused.ok(), "slot reused");
    t.check(driver.offer_physical_success(old_id.id, 99) == TerminalVerdict::rejected_stale,
            "stale generation terminal does nothing");
    t.check(!core.observe_slot(old_id.id.slot)->terminal_chosen,
            "reused request untouched by stale event");
    driver.settle_all();
    return t.failures == 0;
}

bool cancel_intent_alone_never_terminalizes(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.cancel(accepted.id) == PublicCancel::requested,
            "cancel after claim records intent only");
    auto observation = core.observe_slot(accepted.id.slot);
    t.check(!observation->terminal_chosen && observation->cancel_intent,
            "intent recorded, no terminal chosen");
    t.check(driver.offer_zero_effect_cancel(accepted.id) == TerminalVerdict::rejected_inadmissible,
            "zero-effect cancel inadmissible after execution escaped");
    t.check(driver.offer_physical_success(accepted.id, 8) == TerminalVerdict::chosen,
            "physical outcome remains authoritative over earlier intent");
    observation = core.observe_slot(accepted.id.slot);
    t.check(observation->outcome.succeeded && !observation->cancel_intent,
            "success not erased into a fabricated cancel result");
    driver.settle_all();
    return t.failures == 0;
}

bool pre_execution_cancel_wins_only_without_claim(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.cancel(accepted.id) == PublicCancel::won_before_execution,
            "pre-claim cancel wins with known zero effect");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "won cancel retires the pending dispatch obligation");
    auto observation = core.observe_slot(accepted.id.slot);
    t.check(observation->terminal_chosen && observation->outcome.is_canceled() &&
                observation->outcome.effect.remaining == sluice::detail::EffectCertainty::accounted,
            "won cancel is an accounted zero-effect terminal");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::late,
            "no execution claim after cancel won");
    driver.settle_all();
    return t.failures == 0;
}

bool health_failure_alone_never_terminalizes(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    driver.note_health_failure();
    t.check(core.health_failed(), "health failure recorded");
    auto observation = core.observe_slot(accepted.id.slot);
    t.check(!observation->terminal_chosen, "health flag invents no result");
    t.check(driver.offer_physical_error(accepted.id, IoError{.code = IoError::Code::backend_error},
                                        0, true) == TerminalVerdict::chosen,
            "real retirement-backed failure still terminalizes");
    driver.settle_all();
    return t.failures == 0;
}

bool health_failure_closes_new_acceptance_from_reserve(Tracker& t) {
    RequestCore core = make_core(2);
    FakePhysicalDriver driver(core);
    driver.note_health_failure();
    ReserveAttempt attempt = driver.reserve();
    t.check(attempt.status == ReserveStatus::admission_closed,
            "failed health refuses new reservation");
    CoreSnapshot snap = core.snapshot();
    t.check(snap.reserved == 0 && snap.accepted_live == 0 && snap.health_failed &&
                !snap.admission_open,
            "no reservation residue and the admission gate is closed under failed health");
    driver.settle_all();
    return t.failures == 0;
}

bool health_between_reserve_and_accept_refuses_commit(Tracker& t) {
    RequestCore core = make_core(2);
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    t.check(attempt.ok(), "reserve before the health failure");
    driver.note_health_failure();
    AcceptAttempt refused = driver.accept(attempt.reservation, 0, 8);
    t.check(refused.status == sluice::async::detail::AcceptStatus::admission_closed,
            "accept re-checks health under the same authority");
    t.check(driver.lookup(key_of(core, attempt.reservation)) == PublicLookup::not_found,
            "the refused reservation mints no public identity");
    t.check(driver.rollback(attempt.reservation), "the refused reservation rolls back");
    t.check(core.snapshot().accepted_live == 0, "no acceptance residue after the health refusal");
    ReserveAttempt fresh = driver.reserve();
    t.check(fresh.status == ReserveStatus::admission_closed,
            "admission stays closed while health is failed");
    driver.settle_all();
    return t.failures == 0;
}

bool execution_refs_block_publication(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 64);
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 64) == TerminalVerdict::chosen,
            "terminal chosen while execution live");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::not_eligible,
            "publication refused while borrow-touching ref live");
    t.check(!target.acquire_ready(), "nothing visible");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "final borrow-touch retirement");
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted,
            "publication eligible exactly after retirement");
    t.check(target.acquire_ready(), "result visible");
    driver.settle_all();
    return t.failures == 0;
}

bool execution_responsibility_chain_has_no_gap(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    auto observation = core.observe_slot(accepted.id.slot);
    t.check(observation->execution_refs == 1, "acceptance establishes the pending obligation");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.acquire_execution(accepted.id), "executor acquires its obligation");
    observation = core.observe_slot(accepted.id.slot);
    t.check(observation->execution_refs == 2, "handoff overlaps");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::released,
            "dispatcher releases after handoff");
    observation = core.observe_slot(accepted.id.slot);
    t.check(observation->execution_refs == 1, "chain never drops to zero mid-flight");
    t.check(driver.offer_physical_success(accepted.id, 8) == TerminalVerdict::chosen,
            "executor reports the outcome before its last touch");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "final retirement ends the chain");
    observation = core.observe_slot(accepted.id.slot);
    t.check(observation->execution_refs == 0, "chain ends only at retirement");
    driver.settle_all();
    return t.failures == 0;
}

bool last_execution_release_requires_a_terminal(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::premature_rejected,
            "the final execution reference cannot retire before a terminal exists");
    auto observation = core.observe_slot(accepted.id.slot);
    t.check(observation->execution_refs == 1 && !observation->terminal_chosen,
            "the rejected release leaves the obligation intact");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed,
            "the claim handoff remains available");
    t.check(driver.acquire_execution(accepted.id), "the executor handoff acquisition remains available");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::released,
            "a non-final retirement is unaffected");
    t.check(driver.offer_physical_error(
                accepted.id, IoError{.code = IoError::Code::backend_error}) ==
                TerminalVerdict::chosen,
            "the terminal remains formable while the chain is live");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "the final retirement is accepted once the terminal holds the outcome");
    driver.settle_all();
    return t.failures == 0;
}

bool terminal_choice_freezes_execution_capability(Tracker& t) {
    RequestCore core = make_core(3);
    FakePhysicalDriver driver(core);
    ReserveAttempt canceled_attempt = driver.reserve();
    AcceptAttempt canceled = driver.accept(canceled_attempt.reservation, 0, 8);
    t.check(driver.cancel(canceled.id) == PublicCancel::won_before_execution,
            "pre-claim cancel wins");
    t.check(!driver.acquire_execution(canceled.id),
            "no execution authority is granted after a zero-effect cancel win");
    t.check(driver.claim_execution(canceled.id) == ExecutionClaim::late,
            "no execution claim after a zero-effect cancel win");
    t.check(driver.retire_execution(canceled.id) == ExecutionRelease::borrow_touch_fully_retired,
            "the pending dispatch obligation still retires");

    ReserveAttempt finished_attempt = driver.reserve();
    AcceptAttempt finished = driver.accept(finished_attempt.reservation, 0, 8);
    t.check(driver.claim_execution(finished.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(finished.id, 8) == TerminalVerdict::chosen, "terminal");
    t.check(!driver.acquire_execution(finished.id),
            "no new execution reference after a chosen terminal");
    t.check(driver.retire_execution(finished.id) == ExecutionRelease::borrow_touch_fully_retired,
            "the reporting reference retires");

    ReserveAttempt zero_attempt = driver.reserve();
    AcceptAttempt zero = driver.accept(zero_attempt.reservation, 0, 0, true);
    t.check(zero.ok(), "zero-op accepted");
    t.check(!driver.acquire_execution(zero.id), "no execution authority on a zero-op terminal");
    driver.settle_all();
    return t.failures == 0;
}

bool publication_states_are_distinguishable(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    auto observation = core.observe_slot(accepted.id.slot);
    t.check(!observation->terminal_chosen && !observation->publication_inflight &&
                !observation->published,
            "accepted: nothing chosen, published or in flight");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 8) == TerminalVerdict::chosen, "terminal");
    observation = core.observe_slot(accepted.id.slot);
    t.check(observation->terminal_chosen && !observation->publication_inflight &&
                !observation->published,
            "chosen is not published");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "retire");
    PublicationPayload payload;
    t.check(driver.begin_publication(accepted.id, &payload) == PublicationGrant::granted,
            "begin");
    observation = core.observe_slot(accepted.id.slot);
    t.check(observation->publication_inflight && !observation->published,
            "in flight is not visible");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed,
            "complete");
    observation = core.observe_slot(accepted.id.slot);
    t.check(!observation->publication_inflight && observation->published,
            "epilogue complete is distinct from in flight");
    t.check(driver.lookup(accepted.id) == PublicLookup::published,
            "core lookup reports published after epilogue");
    driver.settle_all();
    return t.failures == 0;
}

bool duplicate_publisher_rejected(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.cancel(accepted.id) == PublicCancel::won_before_execution, "cancel wins");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
                "won cancel retires the pending dispatch obligation");
    PublicationPayload first;
    t.check(driver.begin_publication(accepted.id, &first) == PublicationGrant::granted, "first");
    PublicationPayload second;
    t.check(driver.begin_publication(accepted.id, &second) == PublicationGrant::duplicate_publisher,
            "second publisher rejected");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "complete");
    PublicationPayload third;
    t.check(driver.begin_publication(accepted.id, &third) == PublicationGrant::duplicate_publisher,
            "republishing a published request rejected");
    driver.settle_all();
    return t.failures == 0;
}

bool terminal_without_publication_does_not_reclaim(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.cancel(accepted.id) == PublicCancel::won_before_execution, "cancel wins");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
                "won cancel retires the pending dispatch obligation");
    t.check(core.snapshot().accepted_live == 1, "terminal alone keeps the slot");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::accepted,
            "not reclaimed without publication");
    driver.settle_all();
    return t.failures == 0;
}

bool publication_without_binding_release_does_not_reclaim(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.cancel(accepted.id) == PublicCancel::won_before_execution, "cancel wins");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
                "won cancel retires the pending dispatch obligation");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    auto observation = core.observe_slot(accepted.id.slot);
    t.check(observation->phase == RequestCore::SlotPhase::accepted && observation->binding_live,
            "live binding pins the slot");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "final release reclaims immediately");
    driver.settle_all();
    return t.failures == 0;
}

bool binding_release_with_pins_live_does_not_reclaim(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 8) == TerminalVerdict::chosen, "terminal");
    t.check(driver.acquire_control(accepted.id), "control pin");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "execution retired");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released,
            "binding released");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::accepted,
            "control pin defers reclaim");
    t.check(driver.retire_control(accepted.id) == sluice::async::detail::ControlRelease::released,
            "control retires");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "final pin release itself drives reclaim");
    driver.settle_all();
    return t.failures == 0;
}

bool observer_registration_arms_once_and_reports_occupied(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.register_observer(accepted.id) == ObserverRegistration::armed,
            "the first registration arms");
    t.check(driver.register_observer(accepted.id) == ObserverRegistration::duplicate,
            "a second registration reports the occupied disposition");
    auto observation = core.observe_slot(accepted.id.slot);
    t.check(observation.has_value() && observation->observer_phase == ObserverPhase::armed,
            "the registration existence is core-owned state");
    RequestKey stale{accepted.id.context, accepted.id.slot, Generation{accepted.id.generation.value + 1}};
    t.check(driver.register_observer(stale) == ObserverRegistration::not_found,
            "a stale identity cannot register");
    t.check(driver.cancel_observer(accepted.id) == ObserverCancellation::retired,
            "cancellation acquires the registration retirement fact");
    t.check(core.observe_slot(accepted.id.slot)->observer_phase == ObserverPhase::retired,
            "the retired phase is reclaim-visible");
    driver.settle_all();
    return t.failures == 0;
}

bool attach_after_publication_returns_already_terminal(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.cancel(accepted.id) == PublicCancel::won_before_execution, "cancel wins");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "won cancel retires the pending dispatch obligation");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.register_observer(accepted.id) == ObserverRegistration::already_terminal,
            "attach after publication returns the already-terminal disposition");
    t.check(core.observe_slot(accepted.id.slot)->observer_phase == ObserverPhase::unattached,
            "an already-terminal attach installs no registration");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(driver.register_observer(accepted.id) == ObserverRegistration::not_found,
            "attach on a released binding cannot register");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "the slot reclaims with no observer pin installed");
    driver.settle_all();
    return t.failures == 0;
}

bool observer_registration_pins_reclaim_until_retirement(Tracker& t) {
    RequestCore core = make_core(1);
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.register_observer(accepted.id) == ObserverRegistration::armed,
            "the observer arms before terminal");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 8) == TerminalVerdict::chosen, "terminal");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "execution retired");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released,
            "the public binding may release while the delivery stays pinned");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::accepted,
            "the observer pin defers reclaim past binding release");
    t.check(core.observe_slot(accepted.id.slot)->observer_phase == ObserverPhase::queued,
            "the pin is the queued registration itself");
    t.check(driver.claim_observer_delivery(accepted.id) == ObserverDeliveryClaim::claimed,
            "the driver claims the queued delivery exclusively");
    t.check(core.observe_slot(accepted.id.slot)->observer_phase == ObserverPhase::delivering,
            "the delivering phase is distinguishable before retirement");
    t.check(driver.retire_observer_delivery(accepted.id) == ObserverDeliveryRetirement::retired,
            "delivery retirement releases the registration");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "delivery retirement itself drives reclaim");
    ReserveAttempt reused = driver.reserve();
    t.check(reused.ok(), "the retired slot is reusable");
    t.check(core.observe_slot(reused.reservation.slot)->observer_phase == ObserverPhase::unattached,
            "reuse installs no registration residue");
    t.check(driver.retire_observer_delivery(accepted.id) == ObserverDeliveryRetirement::not_found,
            "the retired identity cannot retire again after reuse");
    t.check(driver.rollback(reused.reservation), "reuse probe rolled back");
    driver.settle_all();
    return t.failures == 0;
}

bool observer_retirement_reports_unregistered_distinctly(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.cancel_observer(accepted.id) == ObserverCancellation::not_registered,
            "cancelling an unattached request reports not_registered");
    t.check(driver.register_observer(accepted.id) == ObserverRegistration::armed, "armed");
    t.check(driver.cancel_observer(accepted.id) == ObserverCancellation::retired, "retired");
    t.check(driver.cancel_observer(accepted.id) == ObserverCancellation::not_registered,
            "double cancellation reports not_registered, not not_found");
    t.check(driver.retire_observer_delivery(accepted.id) == ObserverDeliveryRetirement::not_registered,
            "delivery retirement without a claim reports not_registered");
    RequestKey foreign{sluice::async::detail::ContextIdentity::for_testing(78),
                       accepted.id.slot, accepted.id.generation};
    t.check(driver.cancel_observer(foreign) == ObserverCancellation::not_found,
            "a foreign context identity reports not_found");
    driver.settle_all();
    return t.failures == 0;
}

bool attach_during_publication_window_arms_for_that_publication(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 8) == TerminalVerdict::chosen, "terminal");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "execution retired");
    PublicationPayload payload{};
    t.check(driver.begin_publication(accepted.id, &payload) == PublicationGrant::granted,
            "publication begins (window open)");
    auto mid_window = core.observe_slot(accepted.id.slot);
    t.check(mid_window->publication_inflight && !mid_window->published,
            "the window is terminal-visible but not published");
    t.check(driver.register_observer(accepted.id) == ObserverRegistration::armed,
            "attach inside the publication window arms rather than reporting already-terminal");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(core.observe_slot(accepted.id.slot)->observer_phase == ObserverPhase::queued,
            "publication queues the armed registration as the pending delivery");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released,
            "the binding may release");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::accepted,
            "the pending delivery pins the slot past binding release");
    t.check(driver.claim_observer_delivery(accepted.id) == ObserverDeliveryClaim::claimed,
            "the driver claims the pending delivery");
    t.check(driver.retire_observer_delivery(accepted.id) == ObserverDeliveryRetirement::retired,
            "delivery retirement releases the pending delivery");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "reclaim follows delivery retirement");
    driver.settle_all();
    return t.failures == 0;
}

bool attach_on_terminal_choice_before_publication_still_arms(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 8) == TerminalVerdict::chosen, "terminal");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "execution retired");
    t.check(driver.register_observer(accepted.id) == ObserverRegistration::armed,
            "attach after terminal choice but before publication arms for the pending delivery");
    t.check(driver.register_observer(accepted.id) == ObserverRegistration::duplicate,
            "the pending-delivery registration still enforces the occupied disposition");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(core.observe_slot(accepted.id.slot)->observer_phase == ObserverPhase::queued,
            "the registration rides the publication as the one owed delivery");
    t.check(target.acquire_ready(), "polling the publication stays valid with an observer live");
    t.check(driver.cancel_observer(accepted.id) == ObserverCancellation::retired,
            "cancellation suppresses the queued delivery");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "the slot reclaims once the delivery retires and the binding releases");
    driver.settle_all();
    return t.failures == 0;
}

bool registration_failure_preserves_accepted_ownership(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    RequestKey stale{accepted.id.context, accepted.id.slot,
                     Generation{accepted.id.generation.value + 1}};
    t.check(driver.register_observer(stale) == ObserverRegistration::not_found,
            "a stale identity fails registration");
    RequestKey foreign{sluice::async::detail::ContextIdentity::for_testing(78), accepted.id.slot,
                       accepted.id.generation};
    t.check(driver.register_observer(foreign) == ObserverRegistration::not_found,
            "a foreign context identity fails registration");
    t.check(core.observe_slot(accepted.id.slot)->observer_phase == ObserverPhase::unattached,
            "failed registrations install no state");
    t.check(driver.lookup(accepted.id) == PublicLookup::outstanding,
            "the accepted request stays owned and observable after registration failure");
    t.check(driver.cancel(accepted.id) == PublicCancel::won_before_execution,
            "the accepted request stays cancelable after registration failure");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "the won cancel retires the dispatch obligation");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "settlement needs no observer participation");
    driver.settle_all();
    return t.failures == 0;
}

bool delivery_claim_is_exclusive_and_at_most_once(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.register_observer(accepted.id) == ObserverRegistration::armed, "armed");
    t.check(driver.claim_observer_delivery(accepted.id) == ObserverDeliveryClaim::none,
            "an armed-but-unpublished registration owes no claimable delivery yet");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 8) == TerminalVerdict::chosen, "terminal");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "execution retired");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(core.observe_slot(accepted.id.slot)->observer_phase == ObserverPhase::queued,
            "publication moved the registration to queued");
    t.check(driver.claim_observer_delivery(accepted.id) == ObserverDeliveryClaim::claimed,
            "the first claim wins");
    t.check(core.observe_slot(accepted.id.slot)->observer_phase == ObserverPhase::delivering,
            "the claim installs the delivering phase");
    t.check(driver.claim_observer_delivery(accepted.id) == ObserverDeliveryClaim::none,
            "a second claim is refused (at most once per registration generation)");
    t.check(driver.retire_observer_delivery(accepted.id) == ObserverDeliveryRetirement::retired,
            "the hook returns and retires the delivery");
    t.check(driver.claim_observer_delivery(accepted.id) == ObserverDeliveryClaim::none,
            "no claim exists after retirement");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "the slot reclaims once retired and released");
    driver.settle_all();
    return t.failures == 0;
}

bool observer_cancellation_never_cancels_the_operation(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.register_observer(accepted.id) == ObserverRegistration::armed, "armed");
    t.check(driver.cancel_observer(accepted.id) == ObserverCancellation::retired,
            "cancelling the armed registration retires it");
    t.check(core.observe_slot(accepted.id.slot)->observer_phase == ObserverPhase::retired,
            "the cancellation retirement is phase-visible");
    t.check(driver.lookup(accepted.id) == PublicLookup::outstanding,
            "observer cancellation leaves the operation outstanding");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed,
            "the operation still executes after observer cancellation");
    t.check(driver.offer_physical_success(accepted.id, 8) == TerminalVerdict::chosen,
            "the physical terminal is unaffected by observer cancellation");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "execution retired");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted,
            "the operation still publishes");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.claim_observer_delivery(accepted.id) == ObserverDeliveryClaim::none,
            "a canceled registration owes no delivery");
    t.check(target.acquire_ready(), "the publication is visible despite the canceled observer");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "the canceled-observer slot reclaims normally");
    driver.settle_all();
    return t.failures == 0;
}

bool queued_cancellation_suppresses_the_delivery(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.register_observer(accepted.id) == ObserverRegistration::armed, "armed");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 8) == TerminalVerdict::chosen, "terminal");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "execution retired");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(core.observe_slot(accepted.id.slot)->observer_phase == ObserverPhase::queued,
            "the registration is queued by the publication");
    t.check(driver.cancel_observer(accepted.id) == ObserverCancellation::retired,
            "cancelling the queued registration suppresses the delivery");
    t.check(driver.claim_observer_delivery(accepted.id) == ObserverDeliveryClaim::none,
            "the suppressed delivery can no longer be claimed");
    t.check(driver.claim_observer_delivery(accepted.id) == ObserverDeliveryClaim::none,
            "no late claim resurrects the delivery");
    t.check(target.acquire_ready(),
            "the suppressed delivery does not suppress the publication");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "the suppressed-delivery slot reclaims after cancellation");
    driver.settle_all();
    return t.failures == 0;
}

bool cancel_during_delivery_is_distinguishable_and_delivery_completes(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.register_observer(accepted.id) == ObserverRegistration::armed, "armed");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 8) == TerminalVerdict::chosen, "terminal");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "execution retired");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.claim_observer_delivery(accepted.id) == ObserverDeliveryClaim::claimed,
            "the delivery is claimed");
    t.check(driver.cancel_observer(accepted.id) == ObserverCancellation::delivery_in_progress,
            "cancellation during delivery reports in-progress, not retired");
    t.check(core.observe_slot(accepted.id.slot)->observer_phase == ObserverPhase::delivering,
            "the in-progress report did not retire the registration");
    t.check(driver.cancel_observer(accepted.id) == ObserverCancellation::delivery_in_progress,
            "repeated cancellation stays in-progress");
    t.check(driver.retire_observer_delivery(accepted.id) == ObserverDeliveryRetirement::retired,
            "the retained delivery completes and retires");
    t.check(core.observe_slot(accepted.id.slot)->observer_phase == ObserverPhase::retired,
            "the final phase after a completed delivery is retired");
    t.check(driver.cancel_observer(accepted.id) == ObserverCancellation::not_registered,
            "cancellation after retirement reports not_registered");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "the slot reclaims after delivery retirement and release");
    driver.settle_all();
    return t.failures == 0;
}

bool slot_reuse_clears_canonical_result(Tracker& t) {
    RequestCore core = make_core(1);
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.cancel(accepted.id) == PublicCancel::won_before_execution, "cancel wins");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
                "won cancel retires the pending dispatch obligation");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    ReserveAttempt second = driver.reserve();
    t.check(second.ok(), "slot reusable");
    auto observation = core.observe_slot(second.reservation.slot);
    t.check(!observation->terminal_chosen && !observation->has_outcome && !observation->accepted,
            "reuse starts from cleared canonical storage");
    t.check(driver.rollback(second.reservation), "reuse probe rolled back");
    driver.settle_all();
    return t.failures == 0;
}

bool close_prevents_future_acceptance(Tracker& t) {
    RequestCore core = make_core(2);
    FakePhysicalDriver driver(core);
    ReserveAttempt pending = driver.reserve();
    ReserveAttempt live = driver.reserve();
    AcceptAttempt accepted = driver.accept(live.reservation, 0, 8);
    t.check(accepted.ok(), "accepted before close");
    driver.close_admission();
    ReserveAttempt fresh = driver.reserve();
    t.check(fresh.status == ReserveStatus::admission_closed, "no new reservations after close");
    AcceptAttempt refused = driver.accept(pending.reservation, 0, 8);
    t.check(refused.status == sluice::async::detail::AcceptStatus::admission_closed,
            "private reservation cannot bypass close");
    t.check(driver.rollback(pending.reservation), "reservation rolls back after lost race");
    CoreSnapshot snap = core.snapshot();
    t.check(snap.accepted_live == 1 && snap.reserved == 0,
            "accepted set unchanged by close races");
    driver.settle_all();
    return t.failures == 0;
}

bool accepted_set_remains_finite_and_observable(Tracker& t) {
    RequestCore core = make_core(3);
    FakePhysicalDriver driver(core);
    RequestKey ids[3];
    for (int i = 0; i < 3; ++i) {
        ReserveAttempt attempt = driver.reserve();
        AcceptAttempt accepted = driver.accept(attempt.reservation,
                                               static_cast<std::uint64_t>(i) * 8, 8);
        t.check(accepted.ok(), "accepted");
        ids[i] = accepted.id;
    }
    ReserveAttempt overflow = driver.reserve();
    t.check(overflow.status == ReserveStatus::capacity, "capacity bounds the accepted set");
    driver.close_admission();
    CoreSnapshot snap = core.snapshot();
    t.check(snap.accepted_live == 3 && snap.free_slots == 0,
            "snapshot counts remain consistent at capacity");
    for (int i = 0; i < 3; ++i) {
        t.check(driver.lookup(ids[i]) == PublicLookup::outstanding,
                "accepted work still represented after close");
    }
    driver.settle_all();
    return t.failures == 0;
}

bool snapshot_stays_consistent_through_a_lifecycle(Tracker& t) {
    RequestCore core = make_core(2);
    FakePhysicalDriver driver(core);
    auto consistent = [&](const char* label) {
        CoreSnapshot snap = core.snapshot();
        t.check(snap.reserved + snap.accepted_live + snap.free_slots + snap.retired_slots == 2,
                label);
        t.check(snap.publication_refs <= snap.accepted_live, label);
        t.check(snap.public_bindings <= snap.accepted_live, label);
    };
    consistent("initial");
    ReserveAttempt attempt = driver.reserve();
    consistent("reserved");
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    consistent("accepted");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claim");
    t.check(driver.offer_physical_success(accepted.id, 8) == TerminalVerdict::chosen, "terminal");
    t.check(driver.acquire_control(accepted.id), "control");
    consistent("pins live");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "retire");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    consistent("in flight");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    consistent("released");
    t.check(driver.retire_control(accepted.id) == sluice::async::detail::ControlRelease::released,
            "control");
    consistent("reclaimed");
    driver.settle_all();
    return t.failures == 0;
}

bool capacity_exhaustion_is_transient_until_permanent(Tracker& t) {
    RequestCore core = make_core(1);
    FakePhysicalDriver driver(core);
    ReserveAttempt first = driver.reserve();
    ReserveAttempt second = driver.reserve();
    t.check(second.status == ReserveStatus::capacity, "full table reports capacity");
    t.check(first.ok(), "first reservation still valid");
    t.check(driver.rollback(first.reservation), "rollback");
    ReserveAttempt retry = driver.reserve();
    t.check(retry.ok(), "capacity is transient until slots retire permanently");
    t.check(driver.rollback(retry.reservation), "retry reservation rolled back");
    driver.settle_all();
    return t.failures == 0;
}

}  // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)(Tracker&);
    };
    const NamedTest tests[] = {
        {"reserve_creates_no_public_identity", reserve_creates_no_public_identity},
        {"rollback_restores_capacity_and_leaves_no_residue",
         rollback_restores_capacity_and_leaves_no_residue},
        {"accept_activates_identity_exactly_once", accept_activates_identity_exactly_once},
        {"accept_close_race_has_one_serialized_winner",
         accept_close_race_has_one_serialized_winner},
        {"post_accept_failure_converges_through_terminal",
         post_accept_failure_converges_through_terminal},
        {"zero_op_accepts_and_settles_without_execution",
         zero_op_accepts_and_settles_without_execution},
        {"wrong_context_is_rejected", wrong_context_is_rejected},
        {"wrong_generation_is_rejected", wrong_generation_is_rejected},
        {"released_identity_stays_invalid_while_control_pin_lives",
         released_identity_stays_invalid_while_control_pin_lives},
        {"reuse_advances_generation_without_aliasing", reuse_advances_generation_without_aliasing},
        {"generation_exhaustion_retires_slot", generation_exhaustion_retires_slot},
        {"first_admissible_terminal_wins", first_admissible_terminal_wins},
        {"duplicate_and_stale_events_do_not_double_release",
         duplicate_and_stale_events_do_not_double_release},
        {"stale_event_cannot_touch_reused_generation",
         stale_event_cannot_touch_reused_generation},
        {"cancel_intent_alone_never_terminalizes", cancel_intent_alone_never_terminalizes},
        {"pre_execution_cancel_wins_only_without_claim",
         pre_execution_cancel_wins_only_without_claim},
        {"health_failure_alone_never_terminalizes", health_failure_alone_never_terminalizes},
        {"health_failure_closes_new_acceptance_from_reserve",
         health_failure_closes_new_acceptance_from_reserve},
        {"health_between_reserve_and_accept_refuses_commit",
         health_between_reserve_and_accept_refuses_commit},
        {"settled_work_admits_no_new_control_pin", settled_work_admits_no_new_control_pin},
        {"execution_refs_block_publication", execution_refs_block_publication},
        {"execution_responsibility_chain_has_no_gap", execution_responsibility_chain_has_no_gap},
        {"last_execution_release_requires_a_terminal", last_execution_release_requires_a_terminal},
        {"terminal_choice_freezes_execution_capability",
         terminal_choice_freezes_execution_capability},
        {"publication_states_are_distinguishable", publication_states_are_distinguishable},
        {"duplicate_publisher_rejected", duplicate_publisher_rejected},
        {"terminal_without_publication_does_not_reclaim",
         terminal_without_publication_does_not_reclaim},
        {"publication_without_binding_release_does_not_reclaim",
         publication_without_binding_release_does_not_reclaim},
        {"binding_release_with_pins_live_does_not_reclaim",
         binding_release_with_pins_live_does_not_reclaim},
        {"observer_registration_arms_once_and_reports_occupied",
         observer_registration_arms_once_and_reports_occupied},
        {"attach_after_publication_returns_already_terminal",
         attach_after_publication_returns_already_terminal},
        {"observer_registration_pins_reclaim_until_retirement",
         observer_registration_pins_reclaim_until_retirement},
        {"observer_retirement_reports_unregistered_distinctly",
         observer_retirement_reports_unregistered_distinctly},
        {"attach_during_publication_window_arms_for_that_publication",
         attach_during_publication_window_arms_for_that_publication},
        {"attach_on_terminal_choice_before_publication_still_arms",
         attach_on_terminal_choice_before_publication_still_arms},
        {"registration_failure_preserves_accepted_ownership",
         registration_failure_preserves_accepted_ownership},
        {"delivery_claim_is_exclusive_and_at_most_once",
         delivery_claim_is_exclusive_and_at_most_once},
        {"observer_cancellation_never_cancels_the_operation",
         observer_cancellation_never_cancels_the_operation},
        {"queued_cancellation_suppresses_the_delivery",
         queued_cancellation_suppresses_the_delivery},
        {"cancel_during_delivery_is_distinguishable_and_delivery_completes",
         cancel_during_delivery_is_distinguishable_and_delivery_completes},
        {"slot_reuse_clears_canonical_result", slot_reuse_clears_canonical_result},
        {"close_prevents_future_acceptance", close_prevents_future_acceptance},
        {"accepted_set_remains_finite_and_observable", accepted_set_remains_finite_and_observable},
        {"snapshot_stays_consistent_through_a_lifecycle",
         snapshot_stays_consistent_through_a_lifecycle},
        {"capacity_exhaustion_is_transient_until_permanent",
         capacity_exhaustion_is_transient_until_permanent},
    };
    int failed = 0;
    for (const NamedTest& test : tests) {
        Tracker tracker{test.name};
        if (!test.fn(tracker)) {
            ++failed;
        }
    }
    if (failed != 0) {
        std::fprintf(stderr, "%d of %zu request core protocol tests failed\n", failed,
                     sizeof(tests) / sizeof(tests[0]));
        return 1;
    }
    std::printf("all %zu request core protocol tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
