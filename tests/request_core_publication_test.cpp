#include "request_core_test_driver.hpp"

#include <sluice/async/detail/request_core.hpp>

#include <cstdio>
#include <limits>

namespace {

using sluice::IoError;
using sluice::async::detail::AcceptAttempt;
using sluice::async::detail::ExecutionClaim;
using sluice::async::detail::ExecutionRelease;
using sluice_request_core_test::BindingRelease;
using sluice_request_core_test::FakePhysicalDriver;
using sluice_request_core_test::PublicationCompletion;
using sluice_request_core_test::PublicationGrant;
using sluice_request_core_test::PublicationPayload;
using sluice_request_core_test::PublicationTarget;
using sluice_request_core_test::PublicCancel;
using sluice_request_core_test::PublicLookup;
using sluice_request_core_test::PublishOrder;
using sluice_request_core_test::RequestCore;
using sluice_request_core_test::RequestKey;
using sluice_request_core_test::RequestOp;
using sluice_request_core_test::RequestReservation;
using sluice_request_core_test::ReserveAttempt;
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

RequestCore make_core(std::size_t capacity = 2) {
    return RequestCore(sluice::async::detail::ContextIdentity::for_testing(21), capacity);
}

bool happy_path_publication_is_ordered(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 4096, 512, false, RequestOp::write);
    t.check(accepted.ok(), "accepted");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claimed");
    t.check(driver.offer_physical_success(accepted.id, 512) == TerminalVerdict::chosen,
            "physical outcome");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "borrow-touching retirement");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(target.acquire_ready(), "ready is acquire-observable");
    const PublicationPayload& payload = target.acquired_payload();
    t.check(payload.id == accepted.id && payload.op == RequestOp::write && payload.offset == 4096 &&
                payload.requested_bytes == 512 && payload.outcome.succeeded &&
                payload.outcome.effect.confirmed_bytes == 512,
            "published payload matches the canonical result");
    t.check(driver.lookup(accepted.id) == PublicLookup::outstanding,
            "core lookup is conservative until the epilogue");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.lookup(accepted.id) == PublicLookup::published,
            "published lookup after epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(!target.publisher_touched_after_ready() && !target.seal_followed_ready(),
            "publisher never touches the caller target after the ready store");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "slot reclaimed");
    driver.settle_all();
    return t.failures == 0;
}

bool terminal_known_while_execution_retirement_delayed(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 64);
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claimed");
    t.check(driver.offer_physical_success(accepted.id, 64) == TerminalVerdict::chosen,
            "terminal known early");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::not_eligible,
            "publication refused while an execution obligation remains");
    t.check(!target.acquire_ready(), "nothing visible during the delay");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::not_visible_yet,
            "binding not releasable before a visible terminal");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "delayed retirement arrives");
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted,
            "publication eligible after the delay");
    t.check(target.acquire_ready(), "result visible");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(core.snapshot().accepted_live == 0, "reclaimed");
    driver.settle_all();
    return t.failures == 0;
}

bool delayed_control_retirement_after_publication(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 32);
    t.check(driver.acquire_control(accepted.id), "control ref acquired");
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claimed");
    t.check(driver.offer_physical_success(accepted.id, 32) == TerminalVerdict::chosen, "terminal");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "execution retired");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted,
            "control-only ref does not block publication");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::accepted,
            "delayed control retirement still pins the slot");
    t.check(driver.retire_control(accepted.id) ==
                sluice::async::detail::ControlRelease::released,
            "control retirement arrives without any unrelated work");
    t.check(core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::free,
            "reclaim driven by the final retirement itself");
    driver.settle_all();
    return t.failures == 0;
}

bool duplicate_terminal_before_publication_changes_nothing(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 16);
    t.check(driver.claim_execution(accepted.id) == ExecutionClaim::claimed, "claimed");
    t.check(driver.offer_physical_success(accepted.id, 16) == TerminalVerdict::chosen, "winner");
    t.check(driver.offer_physical_error(accepted.id, IoError{.code = IoError::Code::no_space}) ==
                TerminalVerdict::rejected_duplicate,
            "duplicate rejected");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "retired");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted, "published");
    t.check(target.acquired_payload().outcome.effect.confirmed_bytes == 16,
            "published result is the canonical winner");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(driver.offer_physical_success(accepted.id, 16) == TerminalVerdict::rejected_stale,
            "late duplicate after publication changes nothing");
    t.check(core.snapshot().accepted_live == 0, "reclaimed");
    driver.settle_all();
    return t.failures == 0;
}

bool stale_events_after_slot_reuse_miss_everywhere(Tracker& t) {
    RequestCore core = make_core(1);
    FakePhysicalDriver driver(core);
    ReserveAttempt first = driver.reserve();
    AcceptAttempt old_id = driver.accept(first.reservation, 0, 8);
    t.check(driver.claim_execution(old_id.id) == ExecutionClaim::claimed, "claimed");
    t.check(driver.offer_physical_success(old_id.id, 8) == TerminalVerdict::chosen, "terminal");
    t.check(driver.retire_execution(old_id.id) == ExecutionRelease::borrow_touch_fully_retired,
            "retired");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(old_id.id, target) == PublicationGrant::granted, "published");
    t.check(driver.finish_publication(old_id.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(old_id.id) == BindingRelease::released, "released");
    t.check(core.observe_slot(old_id.id.slot)->phase == RequestCore::SlotPhase::free, "freed");

    ReserveAttempt second = driver.reserve();
    AcceptAttempt reused = driver.accept(second.reservation, 0, 8);
    t.check(reused.ok(), "slot reused");
    t.check(driver.publish_to(old_id.id, driver.make_target()) == PublicationGrant::stale,
            "stale generation publication rejected");
    t.check(driver.finish_publication(old_id.id) == PublicationCompletion::stale,
            "stale generation epilogue rejected");
    t.check(driver.retire_execution(old_id.id) == ExecutionRelease::stale,
            "stale generation execution release rejected");
    t.check(driver.release_public_binding(old_id.id) == BindingRelease::stale,
            "stale generation binding release rejected");
    t.check(!core.observe_slot(reused.id.slot)->published,
            "reused request unaffected by stale events");
    driver.settle_all();
    return t.failures == 0;
}

bool public_release_before_publication_is_refused(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.cancel(accepted.id) == PublicCancel::won_before_execution, "cancel wins");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
                "won cancel retires the pending dispatch obligation");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::not_visible_yet,
            "release before any publication is refused");
    auto observation = core.observe_slot(accepted.id.slot);
    t.check(observation->binding_live, "binding still live after refused release");
    t.check(observation->phase == RequestCore::SlotPhase::accepted, "request still represented");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted,
            "publication proceeds normally after refused release");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released,
            "release accepted once visible");
    t.check(core.snapshot().accepted_live == 0, "reclaimed");
    driver.settle_all();
    return t.failures == 0;
}

bool release_racing_publication_epilogue_is_safe(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 8);
    t.check(driver.cancel(accepted.id) == PublicCancel::won_before_execution, "cancel wins");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
                "won cancel retires the pending dispatch obligation");
    PublicationPayload payload;
    t.check(driver.begin_publication(accepted.id, &payload) == PublicationGrant::granted,
            "publication begins");
    bool released_during_epilogue = false;
    bool lookup_invalidated = false;
    bool slot_pinned_during_window = false;
    bool publisher_clean = false;
    {
        PublicationTarget target;
        target.store(payload);
        target.publish_ready();
        publisher_clean = !target.publisher_touched_after_ready() && !target.seal_followed_ready();
        t.check(target.acquire_ready(), "consumer acquires ready");
        t.check(target.acquired_payload().outcome.is_canceled(), "canceled result delivered");
        target.destroy();
        released_during_epilogue =
            driver.release_public_binding(accepted.id) == BindingRelease::released;
        lookup_invalidated = driver.lookup(accepted.id) == PublicLookup::not_found;
        slot_pinned_during_window =
            core.observe_slot(accepted.id.slot)->publication_inflight &&
            core.observe_slot(accepted.id.slot)->phase == RequestCore::SlotPhase::accepted;
        publisher_clean = publisher_clean && !target.publisher_touched_after_ready() &&
                          !target.used_after_destroy();
    }
    t.check(publisher_clean, "ready store was the publisher's last target access");
    t.check(released_during_epilogue, "release during epilogue accepted");
    t.check(lookup_invalidated, "public id invalid immediately");
    t.check(slot_pinned_during_window, "publication pin prevented premature reuse");
    t.check(core.snapshot().accepted_live == 1, "slot still occupied before epilogue");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed,
            "epilogue completes after target destruction");
    t.check(core.snapshot().accepted_live == 0,
            "reclaim driven by the epilogue without touching the destroyed target");
    driver.settle_all();
    return t.failures == 0;
}

bool ready_store_is_the_last_publisher_access_audited(Tracker& t) {
    PublicationPayload payload;
    {
        PublicationTarget target;
        target.store(payload);
        target.publish_ready();
        t.check(target.acquire_ready(), "the ordered publication becomes ready");
        t.check(!target.seal_followed_ready(), "seal bookkeeping precedes the ready store");
        target.destroy();
        t.check(!target.used_after_destroy(), "no publisher access after consumer destroy");
    }
    {
        PublicationTarget target{PublishOrder::ready_before_seal};
        target.store(payload);
        target.publish_ready();
        t.check(target.acquire_ready(), "the misordered variant still becomes ready");
        t.check(target.seal_followed_ready(),
                "the order audit catches publisher bookkeeping after the ready store");
    }
    {
        PublicationTarget target;
        target.publish_ready();
        target.store(payload);
        t.check(target.publisher_touched_after_ready(), "a post-ready store is flagged");
    }
    {
        PublicationTarget target;
        target.publish_ready();
        t.check(target.acquire_ready(), "ready");
        target.destroy();
        target.store(payload);
        t.check(target.used_after_destroy(),
                "a publisher touch after consumer destroy is flagged");
    }
    return t.failures == 0;
}

bool final_blocking_release_drives_each_reclaim_variant(Tracker& t) {
    RequestKey ids[3];
    RequestCore core = make_core(3);
    FakePhysicalDriver driver(core);
    for (int i = 0; i < 3; ++i) {
        ReserveAttempt attempt = driver.reserve();
        AcceptAttempt accepted = driver.accept(attempt.reservation,
                                               static_cast<std::uint64_t>(i) * 8, 8);
        ids[i] = accepted.id;
        t.check(driver.cancel(ids[i]) == PublicCancel::won_before_execution, "cancel wins");
        t.check(driver.retire_execution(ids[i]) == ExecutionRelease::borrow_touch_fully_retired,
                    "won cancel retires the pending dispatch obligation");
    }
    {
        PublicationTarget& target = driver.make_target();
        t.check(driver.publish_to(ids[0], target) == PublicationGrant::granted, "published 0");
        t.check(driver.finish_publication(ids[0]) == PublicationCompletion::completed, "epilogue 0");
        t.check(driver.release_public_binding(ids[0]) == BindingRelease::released, "binding last");
        t.check(core.observe_slot(ids[0].slot)->phase == RequestCore::SlotPhase::free,
                "binding release alone reclaims when it is the final pin");
    }
    {
        t.check(driver.acquire_control(ids[1]), "control pin 1");
        PublicationTarget& target = driver.make_target();
        t.check(driver.publish_to(ids[1], target) == PublicationGrant::granted, "published 1");
        t.check(driver.finish_publication(ids[1]) == PublicationCompletion::completed, "epilogue 1");
        t.check(driver.release_public_binding(ids[1]) == BindingRelease::released, "release 1");
        t.check(core.observe_slot(ids[1].slot)->phase == RequestCore::SlotPhase::accepted,
                "control pin defers reclaim");
        t.check(driver.retire_control(ids[1]) ==
                    sluice::async::detail::ControlRelease::released,
                "control last");
        t.check(core.observe_slot(ids[1].slot)->phase == RequestCore::SlotPhase::free,
                "final control release reclaims");
    }
    {
        PublicationTarget& target = driver.make_target();
        t.check(driver.publish_to(ids[2], target) == PublicationGrant::granted, "published 2");
        t.check(driver.release_public_binding(ids[2]) == BindingRelease::released,
                "release during in-flight publication");
        t.check(core.observe_slot(ids[2].slot)->phase == RequestCore::SlotPhase::accepted,
                "publication pin defers reclaim");
        t.check(driver.finish_publication(ids[2]) == PublicationCompletion::completed,
                "epilogue last");
        t.check(core.observe_slot(ids[2].slot)->phase == RequestCore::SlotPhase::free,
                "final epilogue release reclaims");
    }
    t.check(core.snapshot().free_slots == 3, "all three slots reusable with no unrelated work");
    driver.settle_all();
    return t.failures == 0;
}

bool post_accept_dispatch_failure_publishes_through_terminal(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 100);
    t.check(accepted.ok(), "accepted");
    t.check(driver.offer_physical_error(
                accepted.id,
                IoError{.code = IoError::Code::backend_error, .os_errno = 125}, 40,
                true) == TerminalVerdict::chosen,
            "dispatch failure becomes a terminal with unknown remainder");
    t.check(driver.retire_execution(accepted.id) == ExecutionRelease::borrow_touch_fully_retired,
            "pending obligation retires");
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted,
            "failed initiation still publishes");
    const PublicationPayload& payload = target.acquired_payload();
    t.check(!payload.outcome.succeeded && payload.outcome.effect.confirmed_bytes == 40 &&
                payload.outcome.effect.remaining == sluice::detail::EffectCertainty::unknown,
            "failed initiation does not claim a zero-byte effect");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(core.snapshot().accepted_live == 0, "reclaimed");
    driver.settle_all();
    return t.failures == 0;
}

bool zero_op_publication_needs_no_execution(Tracker& t) {
    RequestCore core = make_core();
    FakePhysicalDriver driver(core);
    ReserveAttempt attempt = driver.reserve();
    AcceptAttempt accepted = driver.accept(attempt.reservation, 0, 0, true);
    PublicationTarget& target = driver.make_target();
    t.check(driver.publish_to(accepted.id, target) == PublicationGrant::granted,
            "zero-op publishes without dispatch");
    t.check(target.acquire_ready() && target.acquired_payload().outcome.succeeded,
            "zero-op result visible");
    t.check(driver.finish_publication(accepted.id) == PublicationCompletion::completed, "epilogue");
    t.check(driver.release_public_binding(accepted.id) == BindingRelease::released, "released");
    t.check(core.snapshot().accepted_live == 0, "reclaimed");
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
        {"happy_path_publication_is_ordered", happy_path_publication_is_ordered},
        {"terminal_known_while_execution_retirement_delayed",
         terminal_known_while_execution_retirement_delayed},
        {"delayed_control_retirement_after_publication",
         delayed_control_retirement_after_publication},
        {"duplicate_terminal_before_publication_changes_nothing",
         duplicate_terminal_before_publication_changes_nothing},
        {"stale_events_after_slot_reuse_miss_everywhere",
         stale_events_after_slot_reuse_miss_everywhere},
        {"public_release_before_publication_is_refused",
         public_release_before_publication_is_refused},
        {"release_racing_publication_epilogue_is_safe",
         release_racing_publication_epilogue_is_safe},
        {"ready_store_is_the_last_publisher_access_audited",
         ready_store_is_the_last_publisher_access_audited},
        {"final_blocking_release_drives_each_reclaim_variant",
         final_blocking_release_drives_each_reclaim_variant},
        {"post_accept_dispatch_failure_publishes_through_terminal",
         post_accept_dispatch_failure_publishes_through_terminal},
        {"zero_op_publication_needs_no_execution", zero_op_publication_needs_no_execution},
    };
    int failed = 0;
    for (const NamedTest& test : tests) {
        Tracker tracker{test.name};
        if (!test.fn(tracker)) {
            ++failed;
        }
    }
    if (failed != 0) {
        std::fprintf(stderr, "%d of %zu request core publication tests failed\n", failed,
                     sizeof(tests) / sizeof(tests[0]));
        return 1;
    }
    std::printf("all %zu request core publication tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
