#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/context_identity.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/async/request_handle.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#if defined(SLUICE_HAS_LIBURING)
#include <sluice/async/uring_backend.hpp>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::IoError;
using sluice::Result;
using sluice::async::detail::BorrowFacts;
using sluice::async::detail::ContextIdentity;
using sluice::async::detail::CoreSnapshot;
using sluice::async::detail::RequestCore;
using sluice::async::detail::RequestDescriptor;
using sluice::async::detail::RequestKey;
using sluice::async::detail::RequestOp;
using sluice::async::detail::SlotIndex;

struct Tracker {
    const char* name;
    int failures = 0;
    bool skipped = false;

    void check(bool ok, const char* label) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s\n", name, label);
        }
    }

    void skip(const char* reason) {
        skipped = true;
        std::printf("SKIP [%s] %s\n", name, reason);
    }
};

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_b1a_ownership_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    if (!content.empty()) {
        const ssize_t n = ::write(fd, content.data(), content.size());
        if (n != static_cast<ssize_t>(content.size())) {
            ::close(fd);
            return {};
        }
    }
    ::close(fd);
    return path;
}

// The fixture is part of the evidence: a failure to create it is a named failure,
// not a silent early return that would hide behind the case's own name.
std::optional<File> open_temp_file(Tracker& t, const std::string& content) {
    const std::string path = make_temp_file(content);
    if (path.empty()) {
        t.check(false, "the temporary fixture file is created");
        return std::nullopt;
    }
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value()) {
        t.check(false, "the temporary fixture file opens");
        return std::nullopt;
    }
    return std::optional<File>{std::move(opened.value())};
}

bool core_is_idle(const CoreSnapshot& s) {
    return s.reserved == 0 && s.accepted_live == 0 && s.terminal_live == 0 && s.published_live == 0 &&
           s.execution_refs == 0 && s.control_refs == 0 && s.publication_refs == 0 &&
           s.public_bindings == 0;
}

bool same_core_facts(const CoreSnapshot& a, const CoreSnapshot& b) {
    return a.reserved == b.reserved && a.accepted_live == b.accepted_live &&
           a.terminal_live == b.terminal_live && a.published_live == b.published_live &&
           a.execution_refs == b.execution_refs && a.control_refs == b.control_refs &&
           a.publication_refs == b.publication_refs && a.public_bindings == b.public_bindings &&
           a.admission_open == b.admission_open && a.health_failed == b.health_failed;
}

bool resolves_as(const Result<RequestHandleState>& r, RequestHandleState expected) {
    return r.has_value() && r.value() == expected;
}

bool settle(RequestCore& core, const RequestKey& id) {
    using namespace sluice::async::detail;
    TerminalCandidate candidate;
    candidate.kind = TerminalCandidateKind::physical_outcome;
    candidate.outcome = sluice::detail::IoOutcome::success(0);
    if (core.offer_terminal(id, candidate) != TerminalVerdict::chosen)
        return false;
    if (core.release_execution(id) != ExecutionRelease::borrow_touch_fully_retired)
        return false;
    PublicationPayload payload;
    if (core.begin_publication(id, &payload) != PublicationGrant::granted)
        return false;
    if (core.complete_publication(id) != PublicationCompletion::completed)
        return false;
    return core.release_public_binding(id) == BindingRelease::released;
}

// Destroying a core that still holds live work terminates, which would replace the
// named assertion failure with a bare abort. Draining first keeps the failure the
// reported evidence; it is a no-op once the case settled its own work.
class CoreCaseCleanup {
  public:
    explicit CoreCaseCleanup(RequestCore* core) : core_(core) {}
    ~CoreCaseCleanup() {
        if (core_ == nullptr)
            return;
        for (std::uint32_t i = 0; i < core_->capacity(); ++i) {
            const sluice::async::detail::SlotIndex index{i};
            const auto slot = core_->observe_slot(index);
            if (!slot.has_value())
                continue;
            if (slot->phase == RequestCore::SlotPhase::reserved) {
                (void)core_->rollback(
                    sluice::async::detail::RequestReservation{index, slot->generation});
            } else if (slot->phase == RequestCore::SlotPhase::accepted) {
                (void)settle(*core_, RequestKey{core_->context(), index, slot->generation});
            }
        }
    }

    CoreCaseCleanup(const CoreCaseCleanup&) = delete;
    CoreCaseCleanup& operator=(const CoreCaseCleanup&) = delete;

  private:
    RequestCore* core_;
};

bool move_transfers_the_same_core(Tracker& t) {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    RequestCore* core = ctx.context_core_for_test();
    t.check(core != nullptr, "the context owns a core");
    if (core == nullptr)
        return false;
    const ContextIdentity identity = core->context();
    t.check(identity.value != 0, "the context identity is a domain value");
    t.check(core->capacity() == raw->slot_capacity(),
            "the core slot budget is the backend slot table capacity");

    auto reservation = core->reserve();
    t.check(reservation.ok(), "the core reserves a slot");
    if (!reservation.ok())
        return false;
    const BorrowFacts borrow{9, nullptr, 16};
    const RequestDescriptor descriptor{RequestOp::read, 4096, 16, false};
    auto accepted = core->accept(reservation.reservation, descriptor, borrow);
    t.check(accepted.ok(), "the core accepts the reservation");
    if (!accepted.ok()) {
        (void)core->rollback(reservation.reservation);
        return false;
    }
    const RequestKey id = accepted.id;

    AsyncIoContext moved(std::move(ctx));
    RequestCore* moved_core = moved.context_core_for_test();
    CoreCaseCleanup cleanup{moved_core};
    t.check(ctx.context_core_for_test() == nullptr, "the moved-from context holds no second core");
    t.check(moved_core == core, "the moved-to context holds the same core object");
    if (moved_core != core)
        return false;
    t.check(moved_core->context() == identity, "the identity domain is unchanged by the move");

    auto slot = moved_core->observe_slot(id.slot);
    t.check(slot.has_value(), "the accepted slot is observable after the move");
    if (slot.has_value()) {
        t.check(slot->phase == RequestCore::SlotPhase::accepted,
                "the lifecycle phase is neither copied nor re-homed");
        t.check(slot->generation == id.generation, "the generation survived the move");
        t.check(slot->op == descriptor.op && slot->offset == descriptor.offset &&
                    slot->requested_bytes == descriptor.length,
                "the accepted descriptor survived the move");
    }
    t.check(moved_core->snapshot().accepted_live == 1, "the accepted set holds exactly that slot");

    t.check(settle(*moved_core, id), "the slot settles through the core after the move");
    t.check(core_is_idle(moved_core->snapshot()), "settling leaves no live core fact");
    return true;
}

bool backend_slot_table_carries_the_context_identity(Tracker& t) {
    auto first_backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    auto second_backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    t.check(first_backend->adopted_core_for_test() == nullptr &&
                second_backend->adopted_core_for_test() == nullptr,
            "backend construction mints no per-backend context identity");

    ThreadPoolBackend* first_raw = first_backend.get();
    ThreadPoolBackend* second_raw = second_backend.get();
    AsyncIoContext first(std::move(first_backend));
    AsyncIoContext second(std::move(second_backend));

    const ContextIdentity first_identity = first.context_core_for_test()->context();
    const ContextIdentity second_identity = second.context_core_for_test()->context();
    t.check(first_raw->adopted_core_for_test() == first.context_core_for_test(),
            "the first backend adopted its context's core");
    t.check(second_raw->adopted_core_for_test() == second.context_core_for_test(),
            "the second backend adopted its context's core");
    t.check(first_raw->adopted_core_for_test()->context() == first_identity,
            "the first adopted core carries its context identity");
    t.check(second_raw->adopted_core_for_test()->context() == second_identity,
            "the second adopted core carries its context identity");
    t.check(first_identity != second_identity, "distinct contexts hold distinct identities");
    return true;
}

bool foreign_and_released_identities_do_not_resolve(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1a identity provenance\n");
    if (!file.has_value())
        return false;

    auto first_backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    AsyncIoContext first(std::move(first_backend));
    AsyncIoContext foreign(std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1}));

    std::vector<std::byte> scratch(16);
    Completion<std::size_t> c;
    auto submitted = first.submit_read_request(
        ReadOp{NativeFileRef{*file}, scratch.data(), scratch.size(), 0}, c);
    t.check(submitted.has_value(), "the request is accepted");
    if (!submitted.has_value())
        return false;
    const RequestHandle handle = submitted.value();
    t.check(handle.valid(), "the accepted request exposes a handle");
    t.check(resolves_as(first.request_state(handle), RequestHandleState::outstanding),
            "the owning context resolves its own identity");
    t.check(resolves_as(foreign.request_state(handle), RequestHandleState::not_found),
            "a foreign context rejects that identity");
    t.check(resolves_as(foreign.request_state(RequestHandle{}), RequestHandleState::not_found),
            "an empty handle resolves nowhere");

    AsyncIoContext moved(std::move(first));
    CoreCaseCleanup cleanup{moved.context_core_for_test()};
    t.check(resolves_as(moved.request_state(handle), RequestHandleState::outstanding),
            "the identity still resolves after a context move");

    while (!c.ready()) {
        (void)moved.poll();
    }
    t.check(c.result().has_value(), "the request completed on the core authority");
    c.reset();
    t.check(moved.context_core_for_test()->occupancy().accepted_live == 0,
            "consuming the result released the core slot");
    t.check(resolves_as(moved.request_state(handle), RequestHandleState::not_found),
            "a released identity stops resolving");
    return true;
}

bool production_request_is_request_core_owned(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b core owned\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore* core = ctx.context_core_for_test();
    t.check(core == raw->adopted_core_for_test(), "the backend drives the context-owned core");
    t.check(core_is_idle(core->snapshot()), "the context core starts idle");

    std::vector<std::byte> scratch(8);
    Completion<std::size_t> c;
    auto submitted = ctx.submit_read_request(
        ReadOp{NativeFileRef{*file}, scratch.data(), scratch.size(), 0}, c);
    t.check(submitted.has_value(), "the production request is accepted");
    if (!submitted.has_value())
        return false;
    const RequestHandle handle = submitted.value();

    while (!c.ready()) {
        t.check(core->occupancy().accepted_live == 1,
                "the live production request occupies a core slot");
        t.check(resolves_as(ctx.request_state(handle), RequestHandleState::outstanding),
                "the public identity resolves through the core");
        (void)ctx.poll();
    }
    const CoreSnapshot settled = core->snapshot();
    t.check(settled.published_live == 1 && settled.public_bindings == 1,
            "the core terminal, publication and binding own the production request");
    t.check(resolves_as(ctx.request_state(handle), RequestHandleState::completion_ready),
            "the core authority holds the terminal result");
    c.reset();
    t.check(core_is_idle(core->snapshot()), "releasing the binding reclaims the core slot");
    t.check(resolves_as(ctx.request_state(handle), RequestHandleState::not_found),
            "the released identity stops resolving");
    return true;
}

class RecordingBackend final : public AsyncBackend {
  public:
    std::size_t reported_capacity = 3;
    std::size_t adoptions = 0;
    ContextIdentity adopted{};

    std::size_t adopt_context_identity(ContextIdentity identity) noexcept override {
        ++adoptions;
        adopted = identity;
        return reported_capacity;
    }

    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override {
        return sluice::make_unexpected<std::size_t>(IoError{IoError::Code::not_supported});
    }
    std::size_t outstanding() const noexcept override { return 0; }

  private:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override {
        return sluice::make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override {
        return sluice::make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override {
        return sluice::make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override {
        return sluice::make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
};

static_assert(
    std::is_same_v<decltype(std::declval<RecordingBackend&>().adopt_context_identity(
                       std::declval<ContextIdentity>())),
                   std::size_t>,
    "the context-to-backend seam carries an identity and reports a slot capacity only");

bool adoption_seam_carries_no_lifecycle_authority(Tracker& t) {
    auto backend = std::make_unique<RecordingBackend>();
    backend->reported_capacity = 3;
    RecordingBackend* raw = backend.get();
    t.check(raw->adoptions == 0, "constructing a backend adopts no identity");

    AsyncIoContext ctx(std::move(backend));
    t.check(raw->adoptions == 1, "the context adopts its identity once");
    t.check(raw->adopted.value != 0, "the adopted identity is a domain value");
    RequestCore* core = ctx.context_core_for_test();
    t.check(core->context() == raw->adopted, "the core carries the identity the backend received");
    t.check(core->capacity() == 3, "the core slot budget is the capacity the backend reported");

    const CoreSnapshot adopted = core->snapshot();
    AsyncIoContext moved(std::move(ctx));
    t.check(raw->adoptions == 1, "the context move adopts nothing again");
    t.check(moved.context_core_for_test()->context() == raw->adopted,
            "the identity the backend received survives the move");
    t.check(same_core_facts(moved.context_core_for_test()->snapshot(), adopted),
            "adoption and the move change no canonical core fact");
    return true;
}

bool domain_identities_are_not_reused_across_contexts(Tracker& t) {
    std::vector<std::unique_ptr<AsyncIoContext>> contexts;
    for (int i = 0; i < 8; ++i) {
        contexts.push_back(std::make_unique<AsyncIoContext>(std::make_unique<RecordingBackend>()));
    }
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        const ContextIdentity left = contexts[i]->context_core_for_test()->context();
        t.check(left.value != 0, "every context holds a nonzero identity");
        for (std::size_t j = i + 1; j < contexts.size(); ++j) {
            t.check(left != contexts[j]->context_core_for_test()->context(),
                    "two live contexts never share an identity");
        }
    }
    return true;
}

// Drives the domain itself rather than UINT64_MAX contexts: the cursor is seeded at
// its last value, so the boundary is reached deterministically in three claims.
bool identity_domain_terminates_at_exhaustion(Tracker& t) {
    std::atomic<std::uint64_t> fresh{1};
    const std::uint64_t first = sluice::async::detail::claim_context_identity(fresh);
    const std::uint64_t second = sluice::async::detail::claim_context_identity(fresh);
    t.check(first == 1, "a fresh domain hands out its first value");
    t.check(second == 2, "the domain advances on every claim");
    t.check(first != second, "a claim never repeats a handed-out value");

    std::atomic<std::uint64_t> boundary{std::numeric_limits<std::uint64_t>::max()};
    const std::uint64_t last = sluice::async::detail::claim_context_identity(boundary);
    t.check(last == std::numeric_limits<std::uint64_t>::max(),
            "the domain hands out the last value exactly once");
    for (int i = 0; i < 4; ++i) {
        t.check(sluice::async::detail::claim_context_identity(boundary) ==
                    sluice::async::detail::kContextIdentityExhausted,
                "an exhausted domain stays exhausted instead of wrapping");
    }
    return true;
}

bool move_assignment_transfers_the_same_core(Tracker& t) {
    auto source_backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* source_raw = source_backend.get();
    AsyncIoContext source(std::move(source_backend));
    AsyncIoContext destination(std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1}));

    const ContextIdentity destination_identity = destination.context_core_for_test()->context();
    RequestCore* core = source.context_core_for_test();
    const ContextIdentity identity = core->context();
    t.check(identity.value != 0, "the source context holds a domain identity");
    t.check(identity != destination_identity, "the two contexts started in different identities");

    auto reservation = core->reserve();
    t.check(reservation.ok(), "the source core reserves a slot");
    if (!reservation.ok())
        return false;
    const BorrowFacts borrow{9, nullptr, 16};
    const RequestDescriptor descriptor{RequestOp::read, 4096, 16, false};
    auto accepted = core->accept(reservation.reservation, descriptor, borrow);
    t.check(accepted.ok(), "the source core accepts the reservation");
    if (!accepted.ok()) {
        (void)core->rollback(reservation.reservation);
        return false;
    }
    const RequestKey id = accepted.id;

    destination = std::move(source);
    RequestCore* assigned_core = destination.context_core_for_test();
    CoreCaseCleanup cleanup{assigned_core};
    t.check(source.context_core_for_test() == nullptr,
            "the move-assigned source holds no second core");
    t.check(assigned_core == core, "the move-assigned destination holds the same core object");
    if (assigned_core != core)
        return false;
    t.check(assigned_core->context() == identity,
            "the move-assigned destination carries the source identity");
    t.check(source_raw->adopted_core_for_test() == assigned_core,
            "the backend and the core moved into the destination together");
    t.check(assigned_core->capacity() == source_raw->slot_capacity(),
            "the core slot budget is the moved-in slot table capacity");
    t.check(assigned_core->snapshot().accepted_live == 1,
            "the accepted set survived the move assignment");
    t.check(settle(*assigned_core, id), "the slot settles through the move-assigned core");
    t.check(core_is_idle(assigned_core->snapshot()), "settling leaves no live core fact");

    auto file = open_temp_file(t, "sluice b1a move assignment\n");
    if (!file.has_value())
        return false;
    std::vector<std::byte> scratch(8);
    Completion<std::size_t> c;
    auto submitted = destination.submit_read_request(
        ReadOp{NativeFileRef{*file}, scratch.data(), scratch.size(), 0}, c);
    t.check(submitted.has_value(), "the move-assigned destination accepts a request");
    if (!submitted.has_value())
        return false;
    const RequestHandle handle = submitted.value();
    while (!c.ready()) {
        (void)destination.poll();
    }
    t.check(resolves_as(destination.request_state(handle), RequestHandleState::completion_ready),
            "the moved-in identity resolves through the move-assigned context");
    c.reset();
    return true;
}

#if defined(SLUICE_HAS_LIBURING)

bool uring_context_carries_the_context_identity(Tracker& t) {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{4, 8});
    if (!backend->available()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    UringAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore* core = ctx.context_core_for_test();
    CoreCaseCleanup cleanup{core};
    t.check(core->capacity() == raw->slot_capacity(),
            "the core slot budget is the backend slot table capacity");

    auto file = open_temp_file(t, "sluice b1c uring ownership\n");
    if (!file.has_value())
        return false;

    std::vector<std::byte> scratch(8);
    Completion<std::size_t> c;
    auto submitted = ctx.submit_read_request(
        ReadOp{NativeFileRef{*file}, scratch.data(), scratch.size(), 0}, c);
    t.check(submitted.has_value(), "the io_uring request is accepted");
    if (!submitted.has_value())
        return false;
    const RequestHandle handle = submitted.value();
    const auto slot0 = core->observe_slot(SlotIndex{0});
    t.check(slot0.has_value() && slot0->accepted && slot0->binding_live,
            "the accepted io_uring request is owned by the context core");

    while (!c.ready()) {
        (void)ctx.poll();
    }
    t.check(c.result().has_value() && c.result().value() == 8,
            "the io_uring request completes through the core publication");
    t.check(resolves_as(ctx.request_state(handle), RequestHandleState::completion_ready),
            "the core-owned identity resolves as published");
    c.reset();
    t.check(core_is_idle(core->snapshot()), "the slot reclaims through the core after release");
    return true;
}

#endif

struct NamedTest {
    const char* name;
    bool (*fn)(Tracker&);
};

}

int main() {
    const NamedTest tests[] = {
        {"production_request_is_request_core_owned", production_request_is_request_core_owned},
        {"move_transfers_the_same_core", move_transfers_the_same_core},
        {"move_assignment_transfers_the_same_core", move_assignment_transfers_the_same_core},
        {"backend_slot_table_carries_the_context_identity",
         backend_slot_table_carries_the_context_identity},
        {"foreign_and_released_identities_do_not_resolve",
         foreign_and_released_identities_do_not_resolve},
        {"adoption_seam_carries_no_lifecycle_authority",
         adoption_seam_carries_no_lifecycle_authority},
        {"domain_identities_are_not_reused_across_contexts",
         domain_identities_are_not_reused_across_contexts},
        {"identity_domain_terminates_at_exhaustion", identity_domain_terminates_at_exhaustion},
#if defined(SLUICE_HAS_LIBURING)
        {"uring_context_carries_the_context_identity", uring_context_carries_the_context_identity},
#endif
    };
    int failed = 0;
    int skipped = 0;
    int passed = 0;
    for (const NamedTest& test : tests) {
        Tracker tracker{test.name};
        const bool reached_end = test.fn(tracker);
        if (!reached_end || tracker.failures != 0) {
            ++failed;
        } else if (tracker.skipped) {
            ++skipped;
        } else {
            ++passed;
        }
    }
    if (failed != 0) {
        std::fprintf(stderr, "%d of %zu request core ownership tests failed\n", failed,
                     sizeof(tests) / sizeof(tests[0]));
        return 1;
    }
    std::printf("%d of %zu request core ownership tests passed, %d skipped\n", passed,
                sizeof(tests) / sizeof(tests[0]), skipped);
    return 0;
}
