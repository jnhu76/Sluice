#include <sluice/async/detail/request_core.hpp>

#include <cstdio>
#include <type_traits>

namespace sluice::async {
class AsyncBackend;
class AsyncIoContext;
class Fiber;
class Scheduler;
template <class T> class Completion;
namespace detail {
class RequestArena;
class SynchronousReadySink;
struct WaiterToken;
class RoutingLease;
}  // namespace detail
}  // namespace sluice::async

namespace {

template <class T, class = void> struct is_complete : std::false_type {};
template <class T> struct is_complete<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};

static_assert(!is_complete<sluice::async::AsyncIoContext>::value,
              "the substrate must not pull the async context into a consumer");
static_assert(!is_complete<sluice::async::AsyncBackend>::value,
              "the substrate must not pull a backend into a consumer");
static_assert(!is_complete<sluice::async::Scheduler>::value,
              "the substrate must not pull the Scheduler into a consumer");
static_assert(!is_complete<sluice::async::Fiber>::value,
              "the substrate must not pull a Fiber into a consumer");
static_assert(!is_complete<sluice::async::Completion<int>>::value,
              "the substrate must not pull a Completion into a consumer");
static_assert(!is_complete<sluice::async::detail::RequestArena>::value,
              "the substrate must not pull the arena into a consumer");
static_assert(!is_complete<sluice::async::detail::SynchronousReadySink>::value,
              "the substrate must not pull a ready sink into a consumer");
static_assert(!is_complete<sluice::async::detail::WaiterToken>::value,
              "the substrate must not pull waiter vocabulary into a consumer");
static_assert(!is_complete<sluice::async::detail::RoutingLease>::value,
              "the substrate must not pull routing vocabulary into a consumer");

using sluice::async::detail::AcceptStatus;
using sluice::async::detail::BindingRelease;
using sluice::async::detail::BorrowFacts;
using sluice::async::detail::ExecutionClaim;
using sluice::async::detail::ExecutionRelease;
using sluice::async::detail::IoOutcome;
using sluice::async::detail::PublicationCompletion;
using sluice::async::detail::PublicationGrant;
using sluice::async::detail::PublicationPayload;
using sluice::async::detail::PublicCancel;
using sluice::async::detail::PublicLookup;
using sluice::async::detail::RequestCore;
using sluice::async::detail::RequestDescriptor;
using sluice::async::detail::RequestKey;
using sluice::async::detail::RequestOp;
using sluice::async::detail::RequestReservation;
using sluice::async::detail::ReserveStatus;
using sluice::async::detail::TerminalCandidate;
using sluice::async::detail::TerminalCandidateKind;
using sluice::async::detail::TerminalVerdict;

struct ProbeTarget {
    PublicationPayload payload{};
    bool ready = false;

    void store(const PublicationPayload& p) { payload = p; }
    void publish_ready() { ready = true; }
    bool acquire_ready() const { return ready; }
};

bool full_lifecycle_on_the_public_surface() {
    RequestCore core(sluice::async::detail::ContextIdentity::for_testing(1), 2);

    auto attempt = core.reserve();
    if (attempt.status != ReserveStatus::reserved) {
        return false;
    }
    const RequestKey reserved_key{core.context(), attempt.reservation.slot,
                                  attempt.reservation.generation};
    if (core.lookup(reserved_key) != PublicLookup::not_found) {
        return false;
    }

    RequestDescriptor descriptor;
    descriptor.op = RequestOp::read;
    descriptor.offset = 0;
    descriptor.length = 32;
    BorrowFacts borrow;
    borrow.fd = 4;
    borrow.buffer = reinterpret_cast<const void*>(0x2000);
    borrow.length = 32;
    auto accepted = core.accept(attempt.reservation, descriptor, borrow);
    if (accepted.status != AcceptStatus::accepted) {
        return false;
    }
    if (core.lookup(accepted.id) != PublicLookup::outstanding) {
        return false;
    }
    if (core.accept(attempt.reservation, descriptor, borrow).status != AcceptStatus::bad_reservation) {
        return false;
    }

    if (core.claim_execution(accepted.id) != ExecutionClaim::claimed) {
        return false;
    }
    TerminalCandidate success;
    success.kind = TerminalCandidateKind::physical_outcome;
    success.outcome = IoOutcome::success(32);
    if (core.offer_terminal(accepted.id, success) != TerminalVerdict::chosen) {
        return false;
    }
    if (core.cancel(accepted.id) != PublicCancel::already_terminal) {
        return false;
    }
    if (core.release_execution(accepted.id) != ExecutionRelease::borrow_touch_fully_retired) {
        return false;
    }

    ProbeTarget target;
    if (core.begin_publication(accepted.id, &target.payload) != PublicationGrant::granted) {
        return false;
    }
    target.publish_ready();
    if (!target.acquire_ready() || !target.payload.outcome.succeeded ||
        target.payload.outcome.effect.confirmed_bytes != 32) {
        return false;
    }
    if (core.complete_publication(accepted.id) != PublicationCompletion::completed) {
        return false;
    }
    if (core.lookup(accepted.id) != PublicLookup::published) {
        return false;
    }
    if (core.release_public_binding(accepted.id) != BindingRelease::released) {
        return false;
    }
    if (core.lookup(accepted.id) != PublicLookup::not_found) {
        return false;
    }

    auto reused = core.reserve();
    const bool reusable = reused.status == ReserveStatus::reserved &&
                          reused.reservation.slot == accepted.id.slot &&
                          reused.reservation.generation.value == accepted.id.generation.value + 1;
    return reusable && core.rollback(reused.reservation);
}

bool close_admission_is_usable_without_a_host() {
    RequestCore core(sluice::async::detail::ContextIdentity::for_testing(2), 1);
    auto pending = core.reserve();
    core.close_admission();
    if (pending.status != ReserveStatus::reserved) {
        return false;
    }
    RequestDescriptor descriptor;
    BorrowFacts borrow;
    if (core.accept(pending.reservation, descriptor, borrow).status != AcceptStatus::admission_closed) {
        return false;
    }
    if (!core.rollback(pending.reservation)) {
        return false;
    }
    if (core.reserve().status != ReserveStatus::admission_closed) {
        return false;
    }
    core.note_health_failure();
    return core.health_failed() && !core.admission_open();
}

}  // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"full_lifecycle_on_the_public_surface", full_lifecycle_on_the_public_surface},
        {"close_admission_is_usable_without_a_host", close_admission_is_usable_without_a_host},
    };
    for (const NamedTest& test : tests) {
        if (!test.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", test.name);
            return 1;
        }
    }
    std::printf("all %zu request core consumer probe tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
