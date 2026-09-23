#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/async/request_handle.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#if defined(SLUICE_HAS_LIBURING)
#include <sluice/async/uring_backend.hpp>
#endif

#include <cstddef>
#include <cstdio>
#include <memory>
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
    t.check(core->capacity() == raw->arena_capacity(),
            "the core slot budget is the backend slot table capacity");

    auto reservation = core->reserve();
    t.check(reservation.ok(), "the core reserves a slot");
    const BorrowFacts borrow{9, nullptr, 16};
    const RequestDescriptor descriptor{RequestOp::read, 4096, 16, false};
    auto accepted = core->accept(reservation.reservation, descriptor, borrow);
    t.check(accepted.ok(), "the core accepts the reservation");
    if (!accepted.ok())
        return false;
    const RequestKey id = accepted.id;

    AsyncIoContext moved(std::move(ctx));
    t.check(ctx.context_core_for_test() == nullptr, "the moved-from context holds no second core");
    RequestCore* moved_core = moved.context_core_for_test();
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
    t.check(first_backend->arena_context_identity() == second_backend->arena_context_identity(),
            "backend construction mints no per-backend context identity");

    ThreadPoolBackend* first_raw = first_backend.get();
    ThreadPoolBackend* second_raw = second_backend.get();
    AsyncIoContext first(std::move(first_backend));
    AsyncIoContext second(std::move(second_backend));

    const ContextIdentity first_identity = first.context_core_for_test()->context();
    const ContextIdentity second_identity = second.context_core_for_test()->context();
    t.check(first_raw->arena_context_identity() == first_identity,
            "the first slot table carries its context identity");
    t.check(second_raw->arena_context_identity() == second_identity,
            "the second slot table carries its context identity");
    t.check(first_identity != second_identity, "distinct contexts hold distinct identities");
    t.check(first_raw->arena_context_identity() != second_raw->arena_context_identity(),
            "distinct slot tables claim disjoint identity domains");
    return true;
}

bool foreign_and_released_identities_do_not_resolve(Tracker& t) {
    const std::string path = make_temp_file("sluice b1a identity provenance\n");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto first_backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    ThreadPoolBackend* first_raw = first_backend.get();
    AsyncIoContext first(std::move(first_backend));
    AsyncIoContext foreign(std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1}));

    std::vector<std::byte> scratch(16);
    Completion<std::size_t> c;
    auto submitted =
        first.submit_read_request(ReadOp{NativeFileRef{file}, scratch.data(), scratch.size(), 0}, c);
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
    t.check(resolves_as(moved.request_state(handle), RequestHandleState::outstanding),
            "the identity still resolves after a context move");

    while (!c.ready()) {
        (void)moved.poll();
    }
    t.check(c.result().has_value(), "the request completed on the backend authority");
    c.reset();
    t.check(first_raw->arena_slot_in_use() == 0, "consuming the result released the slot");
    t.check(resolves_as(moved.request_state(handle), RequestHandleState::not_found),
            "a released identity stops resolving");
    return true;
}

bool production_request_is_not_request_core_owned(Tracker& t) {
    const std::string path = make_temp_file("sluice b1a not migrated\n");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore* core = ctx.context_core_for_test();
    t.check(core_is_idle(core->snapshot()), "the context core starts idle");

    std::vector<std::byte> scratch(8);
    Completion<std::size_t> c;
    auto submitted =
        ctx.submit_read_request(ReadOp{NativeFileRef{file}, scratch.data(), scratch.size(), 0}, c);
    t.check(submitted.has_value(), "the production request is accepted");
    if (!submitted.has_value())
        return false;
    const RequestHandle handle = submitted.value();
    t.check(raw->arena_slot_in_use() == 1, "the request occupies a backend slot");
    t.check(core_is_idle(core->snapshot()), "no core slot mirrors the production request");

    while (!c.ready()) {
        (void)ctx.poll();
    }
    t.check(resolves_as(ctx.request_state(handle), RequestHandleState::completion_ready),
            "the backend authority holds the terminal result");
    t.check(core_is_idle(core->snapshot()),
            "no core terminal, publication or binding mirrors the production request");
    t.check(core->admission_open(), "the core admission gate is untouched");
    c.reset();
    t.check(core_is_idle(core->snapshot()), "reclaiming the backend slot leaves the core idle");
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

#if defined(SLUICE_HAS_LIBURING)

bool uring_context_carries_the_context_identity(Tracker& t) {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{4, 8});
    if (!backend->available()) {
        std::printf("NOT RUN: io_uring unavailable on this host\n");
        return true;
    }
    UringAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore* core = ctx.context_core_for_test();
    t.check(core->capacity() == raw->arena_capacity(),
            "the core slot budget is the backend slot table capacity");
    t.check(raw->arena_context_identity() == core->context(),
            "the io_uring slot table carries its context identity");

    const std::string path = make_temp_file("sluice b1a uring ownership\n");
    if (path.empty())
        return false;
    File file = std::move(File::open(path).value());
    ::unlink(path.c_str());

    std::vector<std::byte> scratch(8);
    Completion<std::size_t> c;
    auto submitted =
        ctx.submit_read_request(ReadOp{NativeFileRef{file}, scratch.data(), scratch.size(), 0}, c);
    t.check(submitted.has_value(), "the io_uring request is accepted");
    if (!submitted.has_value())
        return false;
    const RequestHandle handle = submitted.value();
    t.check(raw->arena_slot_in_use() == 1, "the request occupies a backend slot");
    t.check(core_is_idle(core->snapshot()), "no core slot mirrors the io_uring request");

    while (!c.ready()) {
        (void)ctx.poll();
    }
    t.check(resolves_as(ctx.request_state(handle), RequestHandleState::completion_ready),
            "the io_uring authority holds the terminal result");
    t.check(core_is_idle(core->snapshot()),
            "no core terminal, publication or binding mirrors the io_uring request");
    c.reset();
    t.check(core_is_idle(core->snapshot()), "reclaiming the backend slot leaves the core idle");
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
        {"production_request_is_not_request_core_owned",
         production_request_is_not_request_core_owned},
        {"move_transfers_the_same_core", move_transfers_the_same_core},
        {"backend_slot_table_carries_the_context_identity",
         backend_slot_table_carries_the_context_identity},
        {"foreign_and_released_identities_do_not_resolve",
         foreign_and_released_identities_do_not_resolve},
        {"adoption_seam_carries_no_lifecycle_authority",
         adoption_seam_carries_no_lifecycle_authority},
        {"domain_identities_are_not_reused_across_contexts",
         domain_identities_are_not_reused_across_contexts},
#if defined(SLUICE_HAS_LIBURING)
        {"uring_context_carries_the_context_identity", uring_context_carries_the_context_identity},
#endif
    };
    int failed = 0;
    for (const NamedTest& test : tests) {
        Tracker tracker{test.name};
        const bool reached_end = test.fn(tracker);
        if (!reached_end || tracker.failures != 0) {
            ++failed;
        }
    }
    if (failed != 0) {
        std::fprintf(stderr, "%d of %zu request core ownership tests failed\n", failed,
                     sizeof(tests) / sizeof(tests[0]));
        return 1;
    }
    std::printf("all %zu request core ownership tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
