#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/request_scope.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#if defined(SLUICE_PUBLIC_REQUEST_URING)
#include <sluice/async/uring_backend.hpp>
#endif

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::IoError;
using sluice::Result;
using sluice::async::detail::CoreSnapshot;
using sluice::async::detail::PublicCancel;
using sluice::async::detail::RequestCore;
using sluice::async::detail::RequestKey;
using sluice::async::detail::SlotIndex;

static_assert(std::is_trivially_copyable_v<ScopeTicket<std::size_t>>);
static_assert(std::is_trivially_copyable_v<ScopeTicket<void>>);
static_assert(!std::is_copy_constructible_v<RequestScope>);
static_assert(!std::is_move_constructible_v<RequestScope>);

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
};

std::atomic<std::size_t> g_allocations{0};

}

void* operator new(std::size_t n) {
    const int saved = errno;
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    errno = saved;
    void* p = std::malloc(n);
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    return p;
}

void* operator new[](std::size_t n) {
    return ::operator new(n);
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {

struct AllocationProbe {
    std::size_t before = 0;

    void begin() { before = g_allocations.load(std::memory_order_relaxed); }

    std::size_t end() const {
        return g_allocations.load(std::memory_order_relaxed) - before;
    }
};

#if defined(SLUICE_PUBLIC_REQUEST_URING)

using Backend = UringAsyncBackend;

std::unique_ptr<Backend> make_backend(std::size_t capacity) {
    return std::make_unique<UringAsyncBackend>(UringConfig{capacity, 8});
}

#else

using Backend = ThreadPoolBackend;

std::unique_ptr<Backend> make_backend(std::size_t capacity) {
    return std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{capacity, 1});
}

#endif

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_d1_scope_XXXXXX";
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
    return s.reserved == 0 && s.accepted_live == 0 && s.terminal_live == 0 &&
           s.published_live == 0 && s.execution_refs == 0 && s.control_refs == 0 &&
           s.publication_refs == 0 && s.public_bindings == 0;
}

template <class Gate> struct GateGuard {
    Gate& gate;
    bool rearmed = false;
    ~GateGuard() {
        if (!rearmed) {
            resume_threadpool_gate(gate);
        }
    }
};

struct LifetimeProbe {
    std::vector<char>* order;
    char tag;
    ~LifetimeProbe() { order->push_back(tag); }
};

bool scope_construction_requires_a_claimable_progress_owner(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 owner\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    bool threw_when_external_owner_holds_authority = false;
    {
        std::optional<ProgressOwner> external;
        std::thread claimant([&] {
            auto claimed = ctx.claim_progress_owner();
            if (claimed.has_value()) {
                external = std::move(claimed).value();
            }
        });
        claimant.join();
        if (!external.has_value()) {
            t.check(false, "the external host claims the progress owner");
            return false;
        }
        try {
            RequestScope scope(ctx, 2, ScopeCleanupPolicy::drain);
            (void)scope;
        } catch (const std::runtime_error&) {
            threw_when_external_owner_holds_authority = true;
        }
        external.reset();
    }
    t.check(threw_when_external_owner_holds_authority,
            "scope setup fails before acceptance while an external owner holds the driver");
    t.check(core_is_idle(core.snapshot()),
            "the failed scope setup left no acceptance and no residue");

    bool threw_for_second_scope = false;
    {
        RequestScope first(ctx, 2, ScopeCleanupPolicy::drain);
        try {
            RequestScope second(ctx, 2, ScopeCleanupPolicy::drain);
            (void)second;
        } catch (const std::runtime_error&) {
            threw_for_second_scope = true;
        }
        t.check(threw_for_second_scope,
                "a second live scope cannot duplicate the progress owner");
    }
    bool reclaimed_after_release = false;
    {
        RequestScope reclaims(ctx, 2, ScopeCleanupPolicy::drain);
        reclaimed_after_release = true;
    }
    t.check(reclaimed_after_release,
            "the owner claim is released for the next scope after destruction");
    t.check(core_is_idle(core.snapshot()), "scope lifecycles leave no core residue");
    return true;
}

bool zero_capacity_is_a_setup_error(Tracker& t) {
    auto backend = make_backend(2);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    bool threw = false;
    try {
        RequestScope scope(ctx, 0, ScopeCleanupPolicy::drain);
        (void)scope;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    t.check(threw, "zero scope capacity is a setup error");
    t.check(core_is_idle(core.snapshot()), "the failed setup owns nothing");
    bool claim_released = false;
    {
        RequestScope next(ctx, 1, ScopeCleanupPolicy::drain);
        claim_released = true;
    }
    t.check(claim_released, "the rejected setup released the owner claim it never used");
    return true;
}

bool submit_commits_accepted_responsibility(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 commit\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();
    RequestScope scope(ctx, 2, ScopeCleanupPolicy::drain);
    t.check(scope.capacity() == 2, "the scope reports its chosen tracking capacity");

    std::vector<std::byte> buffer(8, std::byte{0});
    auto submitted =
        scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(submitted.has_value(), "the accepted submission returns a ticket");
    if (!submitted.has_value())
        return false;
    ScopeTicket<std::size_t> ticket = submitted.value();
    t.check(ticket.valid(), "the ticket carries the accepted identity");
    t.check(ctx.lookup(ticket.id()) == RequestReadiness::pending,
            "the ticket identity resolves through the core");
    t.check(core.snapshot().accepted_live == 1 && core.snapshot().public_bindings == 1,
            "the scope owns the accepted responsibility in one core slot");

    ScopeTicket<std::size_t> copy = ticket;
    t.check(copy.valid() && copy.id() == ticket.id(),
            "the ticket is a copyable observation handle, not the responsibility");

    auto sync = scope.submit_sync_all(SyncAllOp{NativeFileRef{*file}});
    t.check(sync.has_value(), "the void request is accepted into the scope");
    if (!sync.has_value())
        return false;
    t.check(core.snapshot().public_bindings == 2, "both accepted requests are tracked");

    t.check(scope.finish().has_value(), "explicit finish reports no unconsumed failure");
    t.check(core_is_idle(core.snapshot()), "finish settled both tracked requests");
    auto gone = scope.take(ticket);
    t.check(gone.readiness == RequestReadiness::empty,
            "the consumed ticket no longer resolves after finish");
    return true;
}

bool v18a_scope_capacity_exhaustion_rejects_before_submission(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 v18a\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();
    RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);

    std::vector<std::byte> first(4, std::byte{0});
    auto accepted =
        scope.submit_read(ReadOp{NativeFileRef{*file}, first.data(), first.size(), 0});
    t.check(accepted.has_value(), "the first submission is accepted");
    if (!accepted.has_value())
        return false;

    const CoreSnapshot before = core.snapshot();
    std::vector<std::byte> second(8, std::byte{0xAA});
    auto rejected =
        scope.submit_read(ReadOp{NativeFileRef{*file}, second.data(), second.size(), 0});
    t.check(!rejected.has_value() && rejected.error().code == IoError::Code::would_block,
            "the exhausted scope rejects the next submission before acceptance");
    const CoreSnapshot after = core.snapshot();
    t.check(after.accepted_live == before.accepted_live &&
                after.reserved == before.reserved &&
                after.public_bindings == before.public_bindings,
            "the rejected submission produced no reservation and no acceptance");
    bool buffer_untouched = true;
    for (std::byte b : second) {
        if (b != std::byte{0xAA}) {
            buffer_untouched = false;
        }
    }
    t.check(buffer_untouched, "the rejected operation acquired no borrow and had no effect");

    t.check(scope.finish().has_value(), "explicit finish reports no unconsumed failure");
    t.check(core_is_idle(core.snapshot()),
            "the single accepted request settled before the scope released its resources");
    auto retry = scope.submit_read(ReadOp{NativeFileRef{*file}, second.data(), 0, 0});
    t.check(!retry.has_value() && retry.error().code == IoError::Code::invalid_state,
            "the finished scope refuses further submissions");
    return true;
}

bool v18b_second_submission_rejection_keeps_first_owned(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 v18b\n");
    if (!file.has_value())
        return false;
    auto rejected_source = open_temp_file(t, "sluice d1 v18b closed\n");
    if (!rejected_source.has_value())
        return false;
    File closed = std::move(rejected_source.value());
    const auto closed_close = closed.close();
    if (!closed_close.has_value()) {
        t.check(false, "the fixture close succeeds");
        return false;
    }

    auto backend = make_backend(2);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();
    RequestScope scope(ctx, 2, ScopeCleanupPolicy::drain);

    std::vector<std::byte> first_buffer(4, std::byte{0});
    auto first =
        scope.submit_read(ReadOp{NativeFileRef{*file}, first_buffer.data(), first_buffer.size(), 0});
    t.check(first.has_value(), "the first submission is accepted");
    if (!first.has_value())
        return false;

    auto second = scope.submit_read(ReadOp{NativeFileRef{closed}, first_buffer.data(), 4, 0});
    t.check(!second.has_value() && second.error().code == IoError::Code::invalid_state,
            "the second submission is rejected by the closed file after reservation");
    t.check(core.snapshot().accepted_live == 1 && core.snapshot().public_bindings == 1,
            "the scope still owns the first accepted request");

    std::vector<std::byte> third_buffer(4, std::byte{0});
    auto third =
        scope.submit_read(ReadOp{NativeFileRef{*file}, third_buffer.data(), third_buffer.size(), 0});
    t.check(third.has_value(),
            "the rejected submission returned its reserved tracking slot");
    if (!third.has_value())
        return false;
    t.check(core.snapshot().accepted_live == 2, "both accepted requests are tracked");

    t.check(scope.finish().has_value(), "explicit finish reports no unconsumed failure");
    t.check(core_is_idle(core.snapshot()),
            "both tracked requests settled before the scope returned");
    return true;
}

bool take_consumes_and_releases_the_slot_for_reuse(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 reuse\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();
    RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);

    auto first = scope.submit_read(ReadOp{NativeFileRef{*file}, nullptr, 0, 0});
    t.check(first.has_value(), "the zero-op request is accepted and publishes at acceptance");
    if (!first.has_value())
        return false;
    ScopeTicket<std::size_t> first_ticket = first.value();
    auto consumed = scope.take(first_ticket);
    t.check(consumed.readiness == RequestReadiness::ready && consumed.result.has_value() &&
                consumed.result.value() == 0,
            "take moves the retained ready result out");
    (void)ctx.poll();
    t.check(core_is_idle(core.snapshot()),
            "consuming released the binding and reclaimed the slot");

    std::vector<std::byte> buffer(4, std::byte{0});
    auto second =
        scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(second.has_value(), "the freed scope slot serves a new submission");
    if (!second.has_value())
        return false;
    auto stale = scope.take(first_ticket);
    t.check(stale.readiness == RequestReadiness::empty,
            "the consumed ticket cannot reach the reused slot");
    t.check(scope.finish().has_value(), "explicit finish reports no unconsumed failure");
    t.check(core_is_idle(core.snapshot()), "the reused slot settled and reclaimed");
    return true;
}

bool retained_ready_results_occupy_scope_capacity(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 retention\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();
    RequestScope scope(ctx, 2, ScopeCleanupPolicy::drain);

    auto a = scope.submit_read(ReadOp{NativeFileRef{*file}, nullptr, 0, 0});
    auto b = scope.submit_read(ReadOp{NativeFileRef{*file}, nullptr, 0, 0});
    t.check(a.has_value() && b.has_value(), "two retained ready results are accepted");
    if (!a.has_value() || !b.has_value())
        return false;

    const CoreSnapshot retained = core.snapshot();
    t.check(retained.accepted_live == 2 && retained.public_bindings == 2,
            "the retained ready results still occupy their core slots");

    std::vector<std::byte> buffer(4, std::byte{0});
    auto rejected =
        scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(!rejected.has_value() && rejected.error().code == IoError::Code::would_block,
            "retained ready results exhaust the scope tracking budget");

    auto consumed = scope.take(a.value());
    t.check(consumed.readiness == RequestReadiness::ready && consumed.result.has_value(),
            "the retained ready result stays consumable");
    auto twice = scope.take(a.value());
    t.check(twice.readiness == RequestReadiness::empty,
            "a consumed ticket cannot be consumed twice");
    (void)scope.take(b.value());

    auto admitted =
        scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(admitted.has_value(), "consuming a retained result frees tracking capacity");
    if (admitted.has_value()) {
        auto waited = scope.wait_for(admitted.value(), std::chrono::seconds(10));
        t.check(waited.has_value() && waited.value() == ScopeWaitStatus::ready,
                "the admitted request publishes");
        auto admitted_result = scope.take(admitted.value());
        t.check(admitted_result.readiness == RequestReadiness::ready &&
                    admitted_result.result.has_value() && admitted_result.result.value() == 4,
                "the admitted request consumed its confirmed count");
    }
    (void)ctx.poll();
    t.check(scope.finish().has_value(), "explicit finish reports no unconsumed failure");
    t.check(core_is_idle(core.snapshot()), "the retained-result pipeline reclaimed fully");
    return true;
}

bool finish_is_idempotent_and_rejects_late_submissions(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 finish\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();
    RequestScope scope(ctx, 2, ScopeCleanupPolicy::drain);

    std::vector<std::byte> buffer(4, std::byte{0});
    auto ticket =
        scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    if (!ticket.has_value()) {
        t.check(false, "the submission is accepted");
        return false;
    }
    t.check(scope.finish().has_value(), "the first finish reports no unconsumed failure");
    t.check(core_is_idle(core.snapshot()), "finish released every tracked binding");
    t.check(scope.finish().has_value(), "repeated finish is a documented no-op");
    t.check(core_is_idle(core.snapshot()), "repeated finish stayed a no-op");
    auto late = scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), 4, 0});
    t.check(!late.has_value() && late.error().code == IoError::Code::invalid_state,
            "submissions after finish are rejected");
    return true;
}

bool w02_tracer_bounded_pipeline(Tracker& t) {
    auto file = open_temp_file(t, "short");
    if (!file.has_value())
        return false;

    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();
    RequestScope scope(ctx, 3, ScopeCleanupPolicy::drain);

    std::vector<std::byte> destination(8, std::byte{0});
    auto read = scope.submit_read(
        ReadOp{NativeFileRef{*file}, destination.data(), destination.size(), 0});
    t.check(read.has_value(), "the short-read request is accepted");
    if (!read.has_value())
        return false;

    std::vector<std::byte> spill(4, std::byte{0});
    auto overflow =
        scope.submit_read(ReadOp{NativeFileRef{*file}, spill.data(), spill.size(), 0});
    if (!overflow.has_value()) {
        t.check(overflow.error().code == IoError::Code::would_block,
                "admission failure beyond the scope bound is tolerated");
    } else {
        auto overflow_waited = scope.wait_for(overflow.value(), std::chrono::seconds(10));
        t.check(overflow_waited.has_value() && overflow_waited.value() == ScopeWaitStatus::ready,
                "an admitted overflow submission is still processed");
        auto overflow_result = scope.take(overflow.value());
        t.check(overflow_result.readiness == RequestReadiness::ready &&
                    overflow_result.result.has_value() && overflow_result.result.value() == 4,
                "the overflow read consumed its confirmed count");
    }

    auto waited = scope.wait_for(read.value(), std::chrono::seconds(10));
    t.check(waited.has_value() && waited.value() == ScopeWaitStatus::ready,
            "the pipeline waits for the tracked read to publish");
    auto observed = scope.take(read.value());
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == 5,
            "the short read reports its confirmed count");
    t.check(std::memcmp(destination.data(), "short", 5) == 0,
            "the buffer holds the short-read bytes after the acquired terminal");

    auto sync = scope.submit_sync_all(SyncAllOp{NativeFileRef{*file}});
    t.check(sync.has_value(), "the durability request is accepted");
    if (sync.has_value()) {
        auto sync_waited = scope.wait_for(sync.value(), std::chrono::seconds(10));
        t.check(sync_waited.has_value() && sync_waited.value() == ScopeWaitStatus::ready,
                "the void request publishes");
        auto sync_result = scope.take(sync.value());
        t.check(sync_result.readiness == RequestReadiness::ready &&
                    sync_result.result.has_value(),
                "the durability request consumed successfully");
    }

    t.check(scope.finish().has_value(), "explicit finish reports no unconsumed failure");
    t.check(core_is_idle(core.snapshot()), "the tracer pipeline settled without residue");
    return true;
}

bool destructor_settles_on_early_return_with_drain_policy(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 early drain\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> buffer(6, std::byte{0});
    {
        RequestScope scope(ctx, 2, ScopeCleanupPolicy::drain);
        auto ticket =
            scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
        t.check(ticket.has_value(), "the outstanding request is accepted");
        if (!ticket.has_value())
            return false;
    }
    t.check(core_is_idle(core.snapshot()),
            "early-return destruction settled the accepted request before returning");
    t.check(std::memcmp(buffer.data(), "sluice", 6) == 0,
            "the borrow ended only with the acquired publication");
    return true;
}

bool destructor_cleanup_uses_the_selected_policy(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 policy\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> buffer(6, std::byte{0});
    {
        RequestScope scope(ctx, 2, ScopeCleanupPolicy::cancel_then_drain);
        auto ticket =
            scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
        t.check(ticket.has_value(), "the outstanding request is accepted");
        if (!ticket.has_value())
            return false;
    }
    t.check(core_is_idle(core.snapshot()),
            "cancel-then-drain destruction settled the accepted request before returning");
    return true;
}

bool finish_reports_an_unconsumed_operation_error(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 finish error\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();
    RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);

    const NativeFileRef unwritable{file->native_handle(), sluice::FileAccess::read_write};
    std::vector<std::byte> source(4, std::byte{0xAB});
    auto accepted = scope.submit_write(WriteOp{unwritable, source.data(), source.size(), 0});
    t.check(accepted.has_value(), "the kernel-doomed write is accepted into the scope");
    if (!accepted.has_value())
        return false;

    auto settled = scope.finish();
    t.check(!settled.has_value() && settled.error().code == IoError::Code::backend_error,
            "explicit finish reports the unconsumed operation error instead of "
            "discarding it");
    t.check(core_is_idle(core.snapshot()),
            "error reporting retained no responsibility: every request is settled and "
            "released");
    auto late = scope.submit_read(ReadOp{NativeFileRef{*file}, nullptr, 0, 0});
    t.check(!late.has_value() && late.error().code == IoError::Code::invalid_state,
            "the reporting finish is still terminal for the scope");
    return true;
}

bool destructor_cleanup_preserves_the_original_exception_and_still_releases(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 unwind error\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    bool original_survived = false;
    try {
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
        const NativeFileRef unwritable{file->native_handle(), sluice::FileAccess::read_write};
        std::vector<std::byte> source(4, std::byte{0xAB});
        auto accepted =
            scope.submit_write(WriteOp{unwritable, source.data(), source.size(), 0});
        if (!accepted.has_value()) {
            t.check(false, "the kernel-doomed write is accepted");
            return false;
        }
        throw std::runtime_error("original");
    } catch (const std::runtime_error& e) {
        original_survived = std::strcmp(e.what(), "original") == 0;
    }
    t.check(original_survived, "destruction during unwinding preserved the original exception");
    t.check(core_is_idle(core.snapshot()),
            "the unwound scope still settled and released the failing operation");
    return true;
}

bool near_max_wait_bound_stays_well_defined(Tracker& t) {
    auto file = open_temp_file(t, "short");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();
    RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);

    std::vector<std::byte> first(8, std::byte{0});
    auto unbounded =
        scope.submit_read(ReadOp{NativeFileRef{*file}, first.data(), first.size(), 0});
    t.check(unbounded.has_value(), "the first read is accepted");
    if (!unbounded.has_value())
        return false;
    auto exact_max = scope.wait_for(unbounded.value(), std::chrono::nanoseconds::max());
    t.check(exact_max.has_value() && exact_max.value() == ScopeWaitStatus::ready,
            "the exact-maximum bound waits without a deadline and reports publication");
    auto first_result = scope.take(unbounded.value());
    t.check(first_result.readiness == RequestReadiness::ready &&
                first_result.result.has_value() && first_result.result.value() == 5,
            "the unbounded wait consumed the real publication");

    std::vector<std::byte> second(8, std::byte{0});
    auto bounded =
        scope.submit_read(ReadOp{NativeFileRef{*file}, second.data(), second.size(), 0});
    t.check(bounded.has_value(), "the second read is accepted");
    if (!bounded.has_value())
        return false;
    const auto near_max = std::chrono::nanoseconds::max() - std::chrono::nanoseconds{1};
    auto waited = scope.wait_for(bounded.value(), near_max);
    t.check(waited.has_value() && waited.value() == ScopeWaitStatus::ready,
            "a near-maximum finite bound computes its deadline without overflow and "
            "reports publication");
    auto second_result = scope.take(bounded.value());
    t.check(second_result.readiness == RequestReadiness::ready &&
                second_result.result.has_value() && second_result.result.value() == 5,
            "the near-max wait consumed the real publication");

    t.check(scope.finish().has_value(), "explicit finish reports no unconsumed failure");
    t.check(core_is_idle(core.snapshot()), "the near-max pipeline reclaimed fully");
    return true;
}

bool v18c_exception_unwind_settles_and_preserves_the_exception(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 v18c\n");
    if (!file.has_value())
        return false;

    std::vector<char> destruction_order;
    bool injected_caught = false;
    bool settled_before_catch = false;
    {
        auto backend = make_backend(4);
        AsyncIoContext ctx(std::move(backend));
        RequestCore& core = *ctx.context_core_for_test();

        File& file_storage = *file;
        LifetimeProbe file_probe{&destruction_order, 'f'};

        std::vector<std::byte> buffer(6, std::byte{0});
        LifetimeProbe buffer_probe{&destruction_order, 'b'};

        try {
            RequestScope scope(ctx, 2, ScopeCleanupPolicy::cancel_then_drain);
            auto ticket = scope.submit_read(
                ReadOp{NativeFileRef{file_storage}, buffer.data(), buffer.size(), 0});
            t.check(ticket.has_value(), "the request accepted before the injected failure");
            if (!ticket.has_value())
                return false;
            throw std::runtime_error("injected");
        } catch (const std::runtime_error& e) {
            injected_caught = std::strcmp(e.what(), "injected") == 0;
            settled_before_catch = core_is_idle(core.snapshot());
        }
    }
    t.check(injected_caught, "the original exception survived scope cleanup");
    t.check(settled_before_catch,
            "the unwound scope settled its accepted request before the catch ran");
    t.check(destruction_order.size() == 2 && destruction_order[0] == 'b' &&
                destruction_order[1] == 'f',
            "the scope unwound before the buffer and file owners");
    return true;
}

#if !defined(SLUICE_PUBLIC_REQUEST_URING)

class ThrowingSubmitBackend final : public AsyncBackend {
  public:
    std::size_t poll() override {
        return 0;
    }

    std::size_t outstanding() const noexcept override {
        return 0;
    }

    bool internal_work_retired() const noexcept override {
        return true;
    }

    bool signals_physical_progress() const noexcept override {
        return true;
    }

  private:
    std::size_t slot_capacity() const noexcept override {
        return 4;
    }

    Result<detail::RequestKey> submit_read(ReadOp, Completion<std::size_t>*) override {
        throw std::runtime_error("pre-accept submission throw");
    }

    Result<detail::RequestKey> submit_write(WriteOp, Completion<std::size_t>*) override {
        throw std::runtime_error("pre-accept submission throw");
    }

    Result<detail::RequestKey> submit_sync_data(SyncDataOp, Completion<void>*) override {
        throw std::runtime_error("pre-accept submission throw");
    }

    Result<detail::RequestKey> submit_sync_all(SyncAllOp, Completion<void>*) override {
        throw std::runtime_error("pre-accept submission throw");
    }

    Result<detail::RequestKey> submit_file_info(FileInfoOp, Completion<sluice::FileInfo>*) override {
        throw std::runtime_error("pre-accept submission throw");
    }

    Result<detail::RequestKey> submit_size(SizeOp, Completion<sluice::FileSize>*) override {
        throw std::runtime_error("pre-accept submission throw");
    }

    detail::PublicCancel cancel_identity(detail::RequestKey) override {
        return detail::PublicCancel::not_found;
    }
};

bool child_dies_running(void (*scenario)()) {
    const pid_t pid = ::fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        ::alarm(30);
        scenario();
        std::_Exit(0);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid)
        return false;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}


bool v19_timeout_preserves_responsibility_until_cleanup(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 v19\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    {
        Backend::WorkerClaimedPauseGate gate;
        raw->set_worker_claimed_pause_gate(&gate);
        GateGuard<Backend::WorkerClaimedPauseGate> guard{gate};
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);

        std::vector<std::byte> buffer(6, std::byte{0});
        auto ticket =
            scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
        t.check(ticket.has_value(), "the gated request is accepted");
        if (!ticket.has_value())
            return false;
        wait_threadpool_gate_paused(gate);

        auto waited = scope.wait_for(ticket.value(), std::chrono::milliseconds(10));
        t.check(waited.has_value() && waited.value() == ScopeWaitStatus::timeout,
                "the wait deadline expired while the operation stayed accepted");
        const CoreSnapshot timed_out = core.snapshot();
        t.check(timed_out.accepted_live == 1 && timed_out.terminal_live == 0 &&
                    timed_out.public_bindings == 1,
                "the accepted responsibility survived the timeout unsettled");
        t.check(raw->syscall_count_for_test() == 0,
                "the timeout cancelled nothing and no buffer access happened");

        resume_threadpool_gate(gate);
        wait_threadpool_gate_exited(gate);
        auto after = scope.wait_for(ticket.value(), std::chrono::seconds(10));
        t.check(after.has_value() && after.value() == ScopeWaitStatus::ready,
                "cleanup continues after the timeout and acquires the public terminal");
        auto consumed = scope.take(ticket.value());
        t.check(consumed.readiness == RequestReadiness::ready &&
                    consumed.result.has_value() && consumed.result.value() == 6,
                "the result consumed after the deadline, never instead of settlement");
        t.check(scope.finish().has_value(), "explicit finish reports no unconsumed failure");
        t.check(core_is_idle(core.snapshot()), "the timed-out pipeline reclaimed fully");
    }
    raw->set_worker_claimed_pause_gate(nullptr);
    return true;
}

bool wait_for_reports_sticky_control_interruption(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 control\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    {
        Backend::WorkerClaimedPauseGate gate;
        raw->set_worker_claimed_pause_gate(&gate);
        GateGuard<Backend::WorkerClaimedPauseGate> guard{gate};
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);

        std::vector<std::byte> buffer(4, std::byte{0});
        auto ticket =
            scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
        if (!ticket.has_value()) {
            t.check(false, "the request is accepted");
            return false;
        }
        wait_threadpool_gate_paused(gate);
        ctx.interrupt_progress_waiters();

        auto first = scope.wait_for(ticket.value(), std::chrono::seconds(10));
        t.check(first.has_value() && first.value() == ScopeWaitStatus::interrupted,
                "the wait reports the observed control interruption");
        auto second = scope.wait_for(ticket.value(), std::chrono::milliseconds(10));
        t.check(second.has_value() && second.value() == ScopeWaitStatus::interrupted,
                "unacknowledged control stays sticky for the next wait");
        ctx.acknowledge_progress_control();
        auto third = scope.wait_for(ticket.value(), std::chrono::milliseconds(10));
        t.check(third.has_value() && third.value() == ScopeWaitStatus::timeout,
                "after acknowledgement the wait waits again and can time out");

        resume_threadpool_gate(gate);
        wait_threadpool_gate_exited(gate);
        t.check(scope.finish().has_value(), "explicit finish reports no unconsumed failure");
        t.check(core_is_idle(ctx.context_core_for_test()->snapshot()),
                "the interrupted pipeline still settled");
    }
    raw->set_worker_claimed_pause_gate(nullptr);
    return true;
}

bool cancel_won_before_execution_settles_as_a_canceled_terminal(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 cancel won\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    {
        Backend::AcceptedPreDispatchPauseGate gate;
        raw->set_accepted_pre_dispatch_pause_gate(&gate);
        GateGuard<Backend::AcceptedPreDispatchPauseGate> guard{gate};
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);

        std::vector<std::byte> buffer(4, std::byte{0});
        std::optional<ScopeTicket<std::size_t>> ticket;
        std::thread canceler([&] {
            wait_threadpool_gate_paused(gate);
            const auto paused_slot = core.observe_slot(SlotIndex{0});
            if (paused_slot.has_value()) {
                const RequestKey paused_key{core.context(), SlotIndex{0},
                                            paused_slot->generation};
                const PublicCancel disposition = raw->cancel_key_for_test(paused_key);
                if (disposition != PublicCancel::won_before_execution) {
                    std::fprintf(stderr, "unexpected cancel disposition %d\n",
                                 static_cast<int>(disposition));
                }
            }
            resume_threadpool_gate(gate);
        });

        auto submitted =
            scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
        canceler.join();
        t.check(submitted.has_value(), "the losing-race submitter still returns the ticket");
        if (!submitted.has_value())
            return false;
        while (raw->dispatch_size_for_test() != 0)
            std::this_thread::yield();
        t.check(raw->syscall_count_for_test() == 0, "the cancel-won operation never ran");

        (void)ctx.poll();
        auto consumed = scope.take(submitted.value());
        t.check(consumed.readiness == RequestReadiness::ready && !consumed.result.has_value() &&
                    consumed.result.error().code == IoError::Code::canceled,
                "the cancel win converged to a canceled terminal, not to the disposition");
        t.check(scope.finish().has_value(), "explicit finish reports no unconsumed failure");
        t.check(core_is_idle(core.snapshot()),
                "the cancel-won request settled through publication");
    }
    raw->set_accepted_pre_dispatch_pause_gate(nullptr);
    return true;
}

bool pre_accept_throw_rolls_back_the_reservation(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 throw\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThrowingSubmitBackend>();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();
    RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);

    std::vector<std::byte> buffer(4, std::byte{0});
    bool first_threw = false;
    try {
        (void)scope.submit_read(
            ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    } catch (const std::runtime_error&) {
        first_threw = true;
    }
    t.check(first_threw, "the submission exception reaches the caller after the reservation");
    t.check(ctx.outstanding() == 0 && core_is_idle(core.snapshot()),
            "the thrown submission left no acceptance and no core residue");

    bool second_reached_the_backend = false;
    try {
        (void)scope.submit_read(
            ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    } catch (const std::runtime_error&) {
        second_reached_the_backend = true;
    }
    t.check(second_reached_the_backend,
            "the reservation rolled back: a leaked slot would answer would_block before "
            "the backend is reached again");
    return true;
}

bool near_max_bound_waits_out_a_stalled_operation(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 near max stall\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    {
        Backend::WorkerClaimedPauseGate gate;
        raw->set_worker_claimed_pause_gate(&gate);
        GateGuard<Backend::WorkerClaimedPauseGate> guard{gate};
        RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);

        std::vector<std::byte> buffer(6, std::byte{0});
        auto ticket =
            scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
        t.check(ticket.has_value(), "the gated request is accepted");
        if (!ticket.has_value())
            return false;
        wait_threadpool_gate_paused(gate);

        std::thread resumer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            resume_threadpool_gate(gate);
        });

        const auto near_max = std::chrono::nanoseconds::max() - std::chrono::nanoseconds{1};
        auto waited = scope.wait_for(ticket.value(), near_max);
        resumer.join();
        wait_threadpool_gate_exited(gate);
        t.check(waited.has_value() && waited.value() == ScopeWaitStatus::ready,
                "a near-maximum finite bound keeps waiting for a stalled operation "
                "instead of overflowing into an instant timeout");
        auto consumed = scope.take(ticket.value());
        t.check(consumed.readiness == RequestReadiness::ready &&
                    consumed.result.has_value() && consumed.result.value() == 6,
                "the near-max wait acquired the stalled operation's publication");

        t.check(scope.finish().has_value(), "explicit finish reports no unconsumed failure");
        t.check(core_is_idle(core.snapshot()), "the stalled near-max pipeline reclaimed");
    }
    raw->set_worker_claimed_pause_gate(nullptr);
    return true;
}

bool explicit_finish_does_not_invoke_the_cleanup_cancel(Tracker& t) {
    auto file = open_temp_file(t, "sluice");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    {
        Backend::WorkerClaimedPauseGate gate;
        raw->set_worker_claimed_pause_gate(&gate);
        GateGuard<Backend::WorkerClaimedPauseGate> guard{gate};
        RequestScope scope(ctx, 2, ScopeCleanupPolicy::cancel_then_drain);

        std::vector<std::byte> held(6, std::byte{0});
        auto first =
            scope.submit_read(ReadOp{NativeFileRef{*file}, held.data(), held.size(), 0});
        t.check(first.has_value(), "the first read is accepted");
        if (!first.has_value())
            return false;
        wait_threadpool_gate_paused(gate);

        std::vector<std::byte> canary(6, std::byte{0xAA});
        auto second =
            scope.submit_read(ReadOp{NativeFileRef{*file}, canary.data(), canary.size(), 0});
        t.check(second.has_value(), "the second read is accepted");
        if (!second.has_value())
            return false;
        while (raw->dispatch_size_for_test() != 1)
            std::this_thread::yield();

        std::thread resumer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            resume_threadpool_gate(gate);
        });

        auto settled = scope.finish();
        resumer.join();
        wait_threadpool_gate_exited(gate);

        t.check(settled.has_value(),
                "explicit finish under cancel_then_drain is a normal exit: the held "
                "operation settles naturally instead of being canceled");
        t.check(raw->syscall_count_for_test() == 2,
                "both accepted operations executed; the dispatch-held read was not "
                "canceled before execution");
        t.check(std::memcmp(canary.data(), "sluice", 6) == 0,
                "the second read's borrow acquired its publication");
        t.check(core_is_idle(core.snapshot()), "the natural drain reclaimed fully");
    }
    raw->set_worker_claimed_pause_gate(nullptr);
    return true;
}

bool destructor_cleanup_cancels_undispatched_work_before_execution(Tracker& t) {
    auto file = open_temp_file(t, "sluice");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> held(6, std::byte{0});
    std::vector<std::byte> canary(6, std::byte{0xAA});
    {
        Backend::WorkerClaimedPauseGate gate;
        raw->set_worker_claimed_pause_gate(&gate);
        GateGuard<Backend::WorkerClaimedPauseGate> guard{gate};
        std::thread resumer;
        {
            RequestScope scope(ctx, 2, ScopeCleanupPolicy::cancel_then_drain);

            auto first =
                scope.submit_read(ReadOp{NativeFileRef{*file}, held.data(), held.size(), 0});
            t.check(first.has_value(), "the first read is accepted");
            if (!first.has_value())
                return false;
            wait_threadpool_gate_paused(gate);

            auto second = scope.submit_read(
                ReadOp{NativeFileRef{*file}, canary.data(), canary.size(), 0});
            t.check(second.has_value(), "the second read is accepted");
            if (!second.has_value())
                return false;
            while (raw->dispatch_size_for_test() != 1)
                std::this_thread::yield();

            resumer = std::thread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                resume_threadpool_gate(gate);
            });
        }
        resumer.join();
        wait_threadpool_gate_exited(gate);
    }
    raw->set_worker_claimed_pause_gate(nullptr);

    t.check(raw->syscall_count_for_test() == 1,
            "destruction canceled the dispatch-held operation before execution; only "
            "the claimed read ever ran");
    bool canary_untouched = true;
    for (std::byte b : canary) {
        if (b != std::byte{0xAA}) {
            canary_untouched = false;
        }
    }
    t.check(canary_untouched, "the canceled operation acquired no borrow and had no effect");
    t.check(core_is_idle(core.snapshot()),
            "cancel-then-drain destruction still settled every tracked request");
    return true;
}

bool explicit_finish_reports_an_externally_canceled_terminal(Tracker& t) {
    auto file = open_temp_file(t, "sluice");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    {
        Backend::WorkerClaimedPauseGate gate;
        raw->set_worker_claimed_pause_gate(&gate);
        GateGuard<Backend::WorkerClaimedPauseGate> guard{gate};
        RequestScope scope(ctx, 2, ScopeCleanupPolicy::cancel_then_drain);

        std::vector<std::byte> held(6, std::byte{0});
        auto first =
            scope.submit_read(ReadOp{NativeFileRef{*file}, held.data(), held.size(), 0});
        t.check(first.has_value(), "the first read is accepted");
        if (!first.has_value())
            return false;
        wait_threadpool_gate_paused(gate);

        std::vector<std::byte> canary(6, std::byte{0xAA});
        auto second =
            scope.submit_read(ReadOp{NativeFileRef{*file}, canary.data(), canary.size(), 0});
        t.check(second.has_value(), "the second read is accepted");
        if (!second.has_value())
            return false;
        while (raw->dispatch_size_for_test() != 1)
            std::this_thread::yield();

        const auto held_slot = core.observe_slot(SlotIndex{1});
        t.check(held_slot.has_value(), "the dispatch-held request is observed in the core");
        bool canceled_externally = false;
        if (held_slot.has_value()) {
            const RequestKey second_key{core.context(), SlotIndex{1}, held_slot->generation};
            canceled_externally =
                raw->cancel_key_for_test(second_key) == PublicCancel::won_before_execution;
        }
        t.check(canceled_externally, "the external control cancel wins before execution");
        (void)ctx.poll();

        std::thread resumer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            resume_threadpool_gate(gate);
        });

        auto settled = scope.finish();
        resumer.join();
        wait_threadpool_gate_exited(gate);

        t.check(!settled.has_value() && settled.error().code == IoError::Code::canceled,
                "explicit finish reports an externally canceled terminal instead of "
                "suppressing it through the construction policy");
        bool canary_untouched = true;
        for (std::byte b : canary) {
            if (b != std::byte{0xAA}) {
                canary_untouched = false;
            }
        }
        t.check(canary_untouched, "the externally canceled operation had no effect");
        t.check(core_is_idle(core.snapshot()), "the reporting finish reclaimed fully");
    }
    raw->set_worker_claimed_pause_gate(nullptr);
    return true;
}

bool health_failure_during_cleanup_fails_fast_instead_of_returning(Tracker& t) {
    struct Scenario {
        static void run() {
            std::string path = make_temp_file("sluice d1 health\n");
            auto opened = File::open(path);
            ::unlink(path.c_str());
            if (!opened.has_value())
                std::_Exit(2);
            File file = std::move(opened.value());

            auto backend = make_backend(2);
            Backend* raw = backend.get();
            AsyncIoContext ctx(std::move(backend));

            Backend::WorkerClaimedPauseGate gate;
            raw->set_worker_claimed_pause_gate(&gate);
            RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);
            std::vector<std::byte> buffer(4, std::byte{0});
            auto ticket =
                scope.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), buffer.size(), 0});
            if (!ticket.has_value())
                std::_Exit(2);
            wait_threadpool_gate_paused(gate);
            ctx.set_wait_health_failed_for_test();
            (void)scope.finish();
            std::_Exit(0);
        }
    };
    t.check(child_dies_running(&Scenario::run),
            "cleanup with a failed progress domain fails fast rather than returning with "
            "live borrows");
    return true;
}

#endif

bool submit_path_allocates_nothing_after_acceptance(Tracker& t) {
    auto file = open_temp_file(t, "sluice d1 alloc\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();
    RequestScope scope(ctx, 1, ScopeCleanupPolicy::drain);

    AllocationProbe probe;
    probe.begin();
    auto ticket = scope.submit_read(ReadOp{NativeFileRef{*file}, nullptr, 0, 0});
    const std::size_t submit_allocations = probe.end();
    t.check(ticket.has_value(), "the zero-op request is accepted");
    t.check(submit_allocations == 0,
            "reserve, acceptance, and the post-accept commit allocate nothing");

    probe.begin();
    (void)scope.take(ticket.value());
    t.check(probe.end() == 0, "consumption and slot release allocate nothing");

    std::vector<std::byte> buffer(4, std::byte{0});
    (void)scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    probe.begin();
    auto rejected =
        scope.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(probe.end() == 0, "the capacity rejection allocates nothing");
    t.check(!rejected.has_value(), "the exhausted scope rejected the probe submission");
    t.check(scope.finish().has_value(), "explicit finish reports no unconsumed failure");
    t.check(core_is_idle(core.snapshot()), "the allocation probe pipeline reclaimed");
    return true;
}

struct NamedTest {
    const char* name;
    bool (*fn)(Tracker&);
};

}

int main() {
#if defined(SLUICE_PUBLIC_REQUEST_URING)
    {
        auto probe = make_backend(1);
        if (!probe->available()) {
            std::printf("SKIP all request scope tests: io_uring is unavailable on this host\n");
            return 0;
        }
    }
#endif
    const NamedTest tests[] = {
        {"scope_construction_requires_a_claimable_progress_owner",
         scope_construction_requires_a_claimable_progress_owner},
        {"zero_capacity_is_a_setup_error", zero_capacity_is_a_setup_error},
        {"submit_commits_accepted_responsibility", submit_commits_accepted_responsibility},
        {"v18a_scope_capacity_exhaustion_rejects_before_submission",
         v18a_scope_capacity_exhaustion_rejects_before_submission},
        {"v18b_second_submission_rejection_keeps_first_owned",
         v18b_second_submission_rejection_keeps_first_owned},
        {"take_consumes_and_releases_the_slot_for_reuse",
         take_consumes_and_releases_the_slot_for_reuse},
        {"retained_ready_results_occupy_scope_capacity",
         retained_ready_results_occupy_scope_capacity},
        {"finish_is_idempotent_and_rejects_late_submissions",
         finish_is_idempotent_and_rejects_late_submissions},
        {"finish_reports_an_unconsumed_operation_error",
         finish_reports_an_unconsumed_operation_error},
        {"destructor_cleanup_preserves_the_original_exception_and_still_releases",
         destructor_cleanup_preserves_the_original_exception_and_still_releases},
        {"w02_tracer_bounded_pipeline", w02_tracer_bounded_pipeline},
        {"destructor_settles_on_early_return_with_drain_policy",
         destructor_settles_on_early_return_with_drain_policy},
        {"destructor_cleanup_uses_the_selected_policy",
         destructor_cleanup_uses_the_selected_policy},
        {"v18c_exception_unwind_settles_and_preserves_the_exception",
         v18c_exception_unwind_settles_and_preserves_the_exception},
        {"near_max_wait_bound_stays_well_defined", near_max_wait_bound_stays_well_defined},
#if !defined(SLUICE_PUBLIC_REQUEST_URING)
        {"pre_accept_throw_rolls_back_the_reservation",
         pre_accept_throw_rolls_back_the_reservation},
        {"near_max_bound_waits_out_a_stalled_operation",
         near_max_bound_waits_out_a_stalled_operation},
        {"explicit_finish_does_not_invoke_the_cleanup_cancel",
         explicit_finish_does_not_invoke_the_cleanup_cancel},
        {"destructor_cleanup_cancels_undispatched_work_before_execution",
         destructor_cleanup_cancels_undispatched_work_before_execution},
        {"explicit_finish_reports_an_externally_canceled_terminal",
         explicit_finish_reports_an_externally_canceled_terminal},
        {"v19_timeout_preserves_responsibility_until_cleanup",
         v19_timeout_preserves_responsibility_until_cleanup},
        {"wait_for_reports_sticky_control_interruption",
         wait_for_reports_sticky_control_interruption},
        {"cancel_won_before_execution_settles_as_a_canceled_terminal",
         cancel_won_before_execution_settles_as_a_canceled_terminal},
        {"health_failure_during_cleanup_fails_fast_instead_of_returning",
         health_failure_during_cleanup_fails_fast_instead_of_returning},
#endif
        {"submit_path_allocates_nothing_after_acceptance",
         submit_path_allocates_nothing_after_acceptance},
    };
    int failed = 0;
    int passed = 0;
    for (const NamedTest& test : tests) {
        Tracker tracker{test.name};
        const bool reached_end = test.fn(tracker);
        if (!reached_end || tracker.failures != 0) {
            ++failed;
        } else {
            ++passed;
        }
    }
    if (failed != 0) {
        std::fprintf(stderr, "%d of %zu request scope tests failed\n", failed,
                     sizeof(tests) / sizeof(tests[0]));
        return 1;
    }
    std::printf("%d of %zu request scope tests passed\n", passed,
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
