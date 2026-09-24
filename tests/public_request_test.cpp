#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/async/request.hpp>
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
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::IoError;
using sluice::Result;
using sluice::async::detail::CoreSnapshot;
using sluice::async::detail::RequestCore;
using sluice::async::detail::RequestKey;
using sluice::async::detail::SlotIndex;

static_assert(!std::is_copy_constructible_v<Request<std::size_t>>);
static_assert(!std::is_copy_assignable_v<Request<std::size_t>>);
static_assert(std::is_nothrow_move_constructible_v<Request<std::size_t>>);
static_assert(std::is_nothrow_move_assignable_v<Request<std::size_t>>);
static_assert(!std::is_copy_constructible_v<Request<void>>);
static_assert(std::is_nothrow_move_constructible_v<Request<void>>);
static_assert(std::is_nothrow_move_assignable_v<Request<void>>);
static_assert(std::is_trivially_copyable_v<RequestId>);

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
    char path[] = "/tmp/sluice_b2_request_XXXXXX";
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

#if defined(SLUICE_PUBLIC_REQUEST_URING)

using Backend = UringAsyncBackend;

std::unique_ptr<Backend> make_backend(std::size_t capacity) {
    return std::make_unique<UringAsyncBackend>(UringConfig{capacity, 8});
}

bool backend_execution_quiescent(Backend& raw) {
    return raw.dispatch_size_for_test() == 0 && raw.live_cookies_for_test() == 0;
}

#else

using Backend = ThreadPoolBackend;

std::unique_ptr<Backend> make_backend(std::size_t capacity) {
    return std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{capacity, 1});
}

bool backend_execution_quiescent(Backend& raw) {
    return raw.dispatch_occupancy() == 0 && raw.active_workers() == 0;
}

#endif

template <class Gate> struct GateGuard {
    Gate& gate;
    bool rearmed = false;
    ~GateGuard() {
        if (!rearmed) {
            gate.resume.store(true, std::memory_order_release);
            gate.resume.notify_all();
        }
    }
};

template <class Gate> void wait_gate_paused(Gate& gate) noexcept {
    std::atomic<bool>& paused = gate.paused;
    bool seen = paused.load(std::memory_order_acquire);
    while (!seen) {
        paused.wait(seen, std::memory_order_acquire);
        seen = paused.load(std::memory_order_acquire);
    }
}

bool core_is_idle(const CoreSnapshot& s) {
    return s.reserved == 0 && s.accepted_live == 0 && s.terminal_live == 0 &&
           s.published_live == 0 && s.execution_refs == 0 && s.control_refs == 0 &&
           s.publication_refs == 0 && s.public_bindings == 0;
}

bool empty_request_probes_are_invalid(Tracker& t) {
    Request<std::size_t> empty;
    t.check(!empty.valid(), "a default request holds no responsibility");
    t.check(!empty.ready(), "an empty request is never ready");
    t.check(!empty.id().valid(), "an empty request has no identity");
    t.check(empty.try_result().readiness == RequestReadiness::empty,
            "an empty result probe reports the empty disposition");
    t.check(empty.take_result().readiness == RequestReadiness::empty,
            "an empty consume reports the empty disposition");
    auto canceled = empty.cancel();
    t.check(!canceled.has_value() && canceled.error().code == IoError::Code::invalid_state,
            "an empty cancel reports invalid_state");
    empty.discard();
    t.check(!empty.valid(), "discarding an empty request changes nothing");

    Request<std::size_t> source;
    Request<std::size_t> moved_from = std::move(source);
    t.check(!moved_from.valid() && !source.valid(), "moving an empty request moves nothing");
    return true;
}

bool pre_accept_rejection_returns_no_request(Tracker& t) {
    const std::string path = make_temp_file("x");
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value()) {
        t.check(false, "the fixture file opens");
        return false;
    }
    File closed = std::move(opened.value());
    (void)closed.close();

    auto backend = make_backend(2);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::byte buffer{std::byte{0}};
    auto rejected = ctx.submit_read(ReadOp{NativeFileRef{closed}, &buffer, 1, 0});
    t.check(!rejected.has_value() && rejected.error().code == IoError::Code::invalid_state,
            "a closed file is rejected before admission with no Request");
    t.check(core_is_idle(core.snapshot()), "the rejected submission leaves no core residue");
    return true;
}

bool accepted_request_pends_until_publication(Tracker& t) {
    auto file = open_temp_file(t, "sluice b2 pending window\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> buffer(8, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(submitted.has_value(), "the accepted submission returns a Request, not an error");
    if (!submitted.has_value())
        return false;
    Request<std::size_t> request = std::move(submitted).value();
    const RequestId id = request.id();
    t.check(request.valid() && id.valid(), "the Request carries the public responsibility");
    t.check(ctx.lookup(id) == RequestReadiness::pending,
            "the copied id resolves as pending through the core");
    t.check(!request.ready(), "the request is not ready before publication");
    t.check(request.try_result().readiness == RequestReadiness::pending,
            "try_result reports pending before publication");
    t.check(request.take_result().readiness == RequestReadiness::pending,
            "a pending take_result leaves the request unchanged");
    t.check(request.valid(), "the pending take_result did not consume the responsibility");
    t.check(core.snapshot().accepted_live == 1 && core.snapshot().public_bindings == 1,
            "the accepted request occupies one core slot with a live binding");

    while (!request.ready())
        (void)ctx.poll();
    t.check(ctx.lookup(id) == RequestReadiness::ready, "the published id resolves as ready");
    auto observed = request.try_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == 8,
            "try_result acquires the publication and observes the confirmed count");
    t.check(std::memcmp(buffer.data(), "sluice b", 8) == 0,
            "the buffer is reusable after the publication was acquired");

    auto consumed = request.take_result();
    t.check(consumed.readiness == RequestReadiness::ready && consumed.result.has_value() &&
                consumed.result.value() == 8,
            "take_result moves the canonical outcome out exactly once");
    t.check(!request.valid() && !request.id().valid(), "consuming empties the request");
    t.check(request.take_result().readiness == RequestReadiness::empty,
            "a second take_result reports the empty disposition");
    t.check(core_is_idle(core.snapshot()), "consumption released the binding and reclaimed");
    t.check(ctx.lookup(id) == RequestReadiness::empty, "the consumed identity stops resolving");
    auto stale = ctx.cancel(id);
    t.check(stale.has_value() && stale.value() == CancelDisposition::not_found,
            "cancel by the consumed identity reports not_found");
    return true;
}

#if defined(SLUICE_PUBLIC_REQUEST_URING)

bool post_accept_failure_converges_on_the_returned_request(Tracker& t) {
    auto file = open_temp_file(t, "sluice b2 v05\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    if (!backend->available()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    Backend::DispatchFailureInjection injection;
    injection.armed.store(true, std::memory_order_release);
    raw->set_dispatch_failure_injection(&injection);

    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(submitted.has_value(),
            "the post-accept dispatch failure still returns the owned Request");
    raw->set_dispatch_failure_injection(nullptr);
    if (!submitted.has_value())
        return false;
    Request<std::size_t> request = std::move(submitted).value();
    t.check(injection.fired.load() == 1, "the injected dispatch failure fired once");

    while (!request.ready())
        (void)ctx.poll();
    auto consumed = request.take_result();
    t.check(consumed.readiness == RequestReadiness::ready && !consumed.result.has_value() &&
                consumed.result.error().code == IoError::Code::backend_error,
            "the post-accept failure surfaces as the request's terminal error");
    t.check(core_is_idle(core.snapshot()), "the failed request reclaims after consumption");
    return true;
}

#else

bool post_accept_failure_converges_on_the_returned_request(Tracker& t) {
    auto file = open_temp_file(t, "sluice b2 v05\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    Backend::DispatchFailureInjection injection;
    injection.armed.store(true, std::memory_order_release);
    raw->set_dispatch_failure_injection(&injection);

    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(submitted.has_value(),
            "the post-accept dispatch failure still returns the owned Request");
    raw->set_dispatch_failure_injection(nullptr);
    if (!submitted.has_value())
        return false;
    Request<std::size_t> request = std::move(submitted).value();
    t.check(injection.fired.load() == 1, "the injected dispatch failure fired once");
    t.check(raw->syscall_count_for_test() == 0, "no worker ever ran the operation");

    while (!request.ready())
        (void)ctx.poll();
    auto consumed = request.take_result();
    t.check(consumed.readiness == RequestReadiness::ready && !consumed.result.has_value() &&
                consumed.result.error().code == IoError::Code::backend_error,
            "the post-accept failure surfaces as the request's terminal error");
    t.check(core_is_idle(core.snapshot()), "the failed request reclaims after consumption");
    return true;
}

#endif

bool zero_op_request_publishes_at_acceptance(Tracker& t) {
    auto file = open_temp_file(t, "sluice b2 zero\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    AsyncIoContext ctx(std::move(backend));

    auto submitted = ctx.submit_read(ReadOp{NativeFileRef{*file}, nullptr, 0, 0});
    t.check(submitted.has_value(), "the zero-length request is accepted");
    if (!submitted.has_value())
        return false;
    Request<std::size_t> request = std::move(submitted).value();
    t.check(request.ready(),
            "the zero-op request publishes at acceptance and is ready on return");
    auto observed = request.try_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == 0,
            "the zero-op request observes success with zero bytes");
    auto canceled = request.cancel();
    t.check(canceled.has_value() && canceled.value() == CancelDisposition::already_terminal,
            "cancel on a published request reports already_terminal");
    auto consumed = request.take_result();
    t.check(consumed.readiness == RequestReadiness::ready && consumed.result.has_value() &&
                consumed.result.value() == 0,
            "the zero-op request consumes its zero result");
    (void)ctx.poll();
    return true;
}

bool cancel_wins_before_dispatch_and_converges(Tracker& t) {
    auto file = open_temp_file(t, "sluice b2 cancel\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    Backend::AcceptedPreDispatchPauseGate gate;
    raw->set_accepted_pre_dispatch_pause_gate(&gate);
    GateGuard<Backend::AcceptedPreDispatchPauseGate> guard{gate};

    std::vector<std::byte> buffer(4, std::byte{0});
    std::optional<Result<Request<std::size_t>>> submitted;
    std::thread submitter([&] {
        submitted =
            ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    });
    wait_gate_paused(gate);
    t.check(submitted == std::nullopt,
            "the submitter pauses inside the accepting submission before returning");

    const auto paused_slot = core.observe_slot(SlotIndex{0});
    t.check(paused_slot.has_value() && paused_slot->accepted && !paused_slot->terminal_chosen,
            "the accepted-undispatched request holds its slot without a terminal");
    const RequestKey paused_key{core.context(), SlotIndex{0}, paused_slot->generation};
    const detail::PublicCancel window_cancel = raw->cancel_key_for_test(paused_key);
    t.check(window_cancel == detail::PublicCancel::won_before_execution,
            "cancel wins while dispatch handoff has not completed");
    t.check(core.observe_slot(SlotIndex{0})->terminal_chosen,
            "the cancel win selected the canceled terminal");

    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
    submitter.join();
    guard.rearmed = true;
    raw->set_accepted_pre_dispatch_pause_gate(nullptr);
    t.check(submitted.has_value() && submitted->has_value(),
            "the losing submitter still returns the owned Request");
    while (raw->dispatch_size_for_test() != 0)
        std::this_thread::yield();
    Request<std::size_t> request = std::move(submitted->value());

    const auto late_cancel = request.cancel();
    t.check(late_cancel.has_value() &&
                late_cancel.value() == CancelDisposition::already_terminal,
            "a cancel routed through the request after the terminal reports already_terminal");

    while (!request.ready())
        (void)ctx.poll();
    auto consumed = request.take_result();
    t.check(consumed.readiness == RequestReadiness::ready && !consumed.result.has_value() &&
                consumed.result.error().code == IoError::Code::canceled,
            "the cancel win converges to a canceled terminal on the request");
#if !defined(SLUICE_PUBLIC_REQUEST_URING)
    t.check(raw->syscall_count_for_test() == 0, "no physical syscall ever ran");
#endif
    t.check(core_is_idle(core.snapshot()), "the canceled request reclaims after consumption");
    return true;
}

bool consumed_result_with_a_live_control_ref_pins_the_slot(Tracker& t) {
    auto file = open_temp_file(t, "sluice b2 v08\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(1);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    auto submitted = ctx.submit_read(ReadOp{NativeFileRef{*file}, nullptr, 0, 0});
    t.check(submitted.has_value(), "the zero-op request is accepted");
    if (!submitted.has_value())
        return false;
    Request<std::size_t> request = std::move(submitted).value();
    const RequestId id = request.id();

    auto consumed = request.take_result();
    t.check(consumed.readiness == RequestReadiness::ready,
            "the published zero-op result is consumed while the ready event is still owed");
    t.check(ctx.lookup(id) == RequestReadiness::empty,
            "the released binding stops resolving while the control ref remains");
    t.check(core.snapshot().accepted_live == 1 && core.snapshot().control_refs == 1,
            "the owed delivery control ref keeps pinning the slot");

    auto rejected = ctx.submit_read(ReadOp{NativeFileRef{*file}, nullptr, 0, 0});
    t.check(!rejected.has_value() && rejected.error().code == IoError::Code::would_block,
            "the pinned slot refuses new admission");

    (void)ctx.poll();
    t.check(core_is_idle(core.snapshot()),
            "delivering the owed event retires the control ref and reclaims synchronously");
    auto next = ctx.submit_read(ReadOp{NativeFileRef{*file}, nullptr, 0, 0});
    t.check(next.has_value(), "the reclaimed slot serves a new request");
    if (next.has_value()) {
        Request<std::size_t> reused = std::move(next).value();
        t.check(ctx.lookup(id) == RequestReadiness::empty,
                "the old identity cannot reach the reused slot");
        auto old_cancel = ctx.cancel(id);
        t.check(old_cancel.has_value() && old_cancel.value() == CancelDisposition::not_found,
                "cancel by the old identity reports not_found and cannot alias the new request");
        t.check(reused.ready(), "the reused-slot request is unaffected by the stale cancel");
        auto next_id = reused.id();
        (void)reused.take_result();
        (void)ctx.poll();
        t.check(ctx.lookup(next_id) == RequestReadiness::empty,
                "the new identity also retires after consumption");
    }
    return true;
}

bool retained_published_result_pins_capacity_while_execution_is_quiescent(Tracker& t) {
    auto file = open_temp_file(t, "sluice b2 retention\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(1);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(submitted.has_value(), "the first request is accepted");
    if (!submitted.has_value())
        return false;
    Request<std::size_t> retained = std::move(submitted).value();
    while (!retained.ready())
        (void)ctx.poll();

    t.check(backend_execution_quiescent(*raw),
            "physical execution resources are quiescent behind the retained result");
    t.check(core.snapshot().accepted_live == 1 && core.snapshot().public_bindings == 1,
            "the retained published result keeps occupying its slot");

    const auto probe = retained.try_result();
    t.check(probe.readiness == RequestReadiness::ready && probe.result.has_value() &&
                probe.result.value() == 4,
            "try_result observes the retained result without consuming it");
    t.check(core.snapshot().accepted_live == 1 && core.snapshot().public_bindings == 1,
            "observing does not release the retained public binding");

    auto rejected =
        ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(!rejected.has_value() && rejected.error().code == IoError::Code::would_block,
            "the retained result consumes bounded core capacity");

    auto consumed = retained.take_result();
    t.check(consumed.readiness == RequestReadiness::ready && consumed.result.has_value() &&
                consumed.result.value() == 4,
            "the retained result stays consumable");
    t.check(core_is_idle(core.snapshot()), "consumption reclaims the retained slot");
    auto accepted = ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(accepted.has_value(), "the freed slot admits new work without unrelated I/O");
    if (accepted.has_value()) {
        Request<std::size_t> followup = std::move(accepted).value();
        while (!followup.ready())
            (void)ctx.poll();
        (void)followup.take_result();
    }
    return true;
}

bool discard_releases_the_published_binding(Tracker& t) {
    auto file = open_temp_file(t, "sluice b2 discard\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(1);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    if (!submitted.has_value()) {
        t.check(false, "the request is accepted");
        return false;
    }
    Request<std::size_t> request = std::move(submitted).value();
    while (!request.ready())
        (void)ctx.poll();
    request.discard();
    t.check(!request.valid(), "discarding empties the request");
    t.check(core_is_idle(core.snapshot()), "discarding released the binding and reclaimed");
    auto again = ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(again.has_value(), "the discarded slot admits new work");
    if (again.has_value()) {
        Request<std::size_t> next = std::move(again).value();
        while (!next.ready())
            (void)ctx.poll();
        next.discard();
    }
    return true;
}

bool move_transfers_responsibility_without_duplicating_it(Tracker& t) {
    auto file = open_temp_file(t, "sluice b2 move\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> buffer(4, std::byte{0});
    auto first = ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    t.check(first.has_value(), "the first request is accepted");
    if (!first.has_value())
        return false;

    const RequestId first_id = first.value().id();
    Request<std::size_t> pending = std::move(first).value();
    t.check(!first.value().valid() && pending.valid(),
            "move construction empties the source without touching the context");
    t.check(pending.id() == first_id, "the moved request keeps the same identity value");
    while (!pending.ready())
        (void)ctx.poll();
    auto consumed = pending.take_result();
    t.check(consumed.result.has_value() && consumed.result.value() == 4,
            "the move-constructed request settles and consumes normally");

    auto second = ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    if (!second.has_value()) {
        t.check(false, "the second request is accepted");
        return false;
    }
    Request<std::size_t> terminal = std::move(second).value();
    while (!terminal.ready())
        (void)ctx.poll();

    Request<std::size_t> destination;
    destination = std::move(terminal);
    t.check(!terminal.valid() && destination.valid(),
            "move assignment into an empty destination transfers the responsibility");
    (void)destination.take_result();
    t.check(core_is_idle(core.snapshot()),
            "the transferred responsibility still releases exactly one binding");

    auto third = ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    if (!third.has_value()) {
        t.check(false, "the third request is accepted");
        return false;
    }
    Request<std::size_t> overwrite_destination = std::move(third).value();
    while (!overwrite_destination.ready())
        (void)ctx.poll();
    auto fourth = ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    if (!fourth.has_value()) {
        t.check(false, "the fourth request is accepted");
        return false;
    }
    Request<std::size_t> incoming = std::move(fourth).value();
    while (!incoming.ready())
        (void)ctx.poll();
    overwrite_destination = std::move(incoming);
    t.check(!incoming.valid() && overwrite_destination.valid(),
            "move assignment over a terminal destination discards the old binding first");
    t.check(core.snapshot().accepted_live == 1,
            "the overwritten destination released its binding without duplicating ownership");

    Request<std::size_t>& self_ref = overwrite_destination;
    overwrite_destination = std::move(self_ref);
    t.check(overwrite_destination.valid() && overwrite_destination.ready(),
            "a self-move neither abandons nor duplicates the request");
    auto self_consumed = overwrite_destination.take_result();
    t.check(self_consumed.result.has_value() && !overwrite_destination.valid(),
            "the self-moved request still consumes exactly once");
    t.check(core_is_idle(core.snapshot()), "the move chain leaves no core residue");
    return true;
}

bool context_move_preserves_bound_requests(Tracker& t) {
    auto file = open_temp_file(t, "sluice b2 context move\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    AsyncIoContext ctx(std::move(backend));
    RequestCore* core = ctx.context_core_for_test();

    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    if (!submitted.has_value()) {
        t.check(false, "the request is accepted");
        return false;
    }
    Request<std::size_t> request = std::move(submitted).value();
    const RequestId id = request.id();

    AsyncIoContext moved(std::move(ctx));
    t.check(moved.context_core_for_test() == core,
            "the context move transfers the same core");
    while (!request.ready())
        (void)moved.poll();
    t.check(moved.lookup(id) == RequestReadiness::ready,
            "the bound request resolves through the moved context");
    auto consumed = request.take_result();
    t.check(consumed.result.has_value() && consumed.result.value() == 4,
            "the request consumes through the moved context's core");
    t.check(core_is_idle(moved.context_core_for_test()->snapshot()),
            "the moved context reclaims the slot");
    return true;
}

bool sync_request_returns_result_void(Tracker& t) {
    auto file = open_temp_file(t, "sluice b2 sync\n");
    if (!file.has_value())
        return false;

    auto backend = make_backend(2);
    AsyncIoContext ctx(std::move(backend));

    auto submitted = ctx.submit_sync_all(SyncAllOp{NativeFileRef{*file}});
    t.check(submitted.has_value(), "the sync request is accepted");
    if (!submitted.has_value())
        return false;
    Request<void> request = std::move(submitted).value();
    while (!request.ready())
        (void)ctx.poll();
    auto consumed = request.take_result();
    t.check(consumed.readiness == RequestReadiness::ready && consumed.result.has_value(),
            "the sync request consumes a void success");
    t.check(!request.valid(), "the void request empties on consumption");
    return true;
}

struct NamedTest {
    const char* name;
    bool (*fn)(Tracker&);
    bool needs_real_backend = true;
};

}

int main() {
#if defined(SLUICE_PUBLIC_REQUEST_URING)
    {
        auto probe = make_backend(1);
        if (!probe->available()) {
            std::printf("SKIP all public request tests: io_uring is unavailable on this host\n");
            return 0;
        }
    }
#endif
    const NamedTest tests[] = {
        {"empty_request_probes_are_invalid", empty_request_probes_are_invalid, false},
        {"pre_accept_rejection_returns_no_request", pre_accept_rejection_returns_no_request},
        {"accepted_request_pends_until_publication",
         accepted_request_pends_until_publication},
        {"post_accept_failure_converges_on_the_returned_request",
         post_accept_failure_converges_on_the_returned_request},
        {"zero_op_request_publishes_at_acceptance", zero_op_request_publishes_at_acceptance},
        {"cancel_wins_before_dispatch_and_converges",
         cancel_wins_before_dispatch_and_converges},
        {"consumed_result_with_a_live_control_ref_pins_the_slot",
         consumed_result_with_a_live_control_ref_pins_the_slot},
        {"retained_published_result_pins_capacity_while_execution_is_quiescent",
         retained_published_result_pins_capacity_while_execution_is_quiescent},
        {"discard_releases_the_published_binding", discard_releases_the_published_binding},
        {"move_transfers_responsibility_without_duplicating_it",
         move_transfers_responsibility_without_duplicating_it},
        {"context_move_preserves_bound_requests", context_move_preserves_bound_requests},
        {"sync_request_returns_result_void", sync_request_returns_result_void},
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
        std::fprintf(stderr, "%d of %zu public request tests failed\n", failed,
                     sizeof(tests) / sizeof(tests[0]));
        return 1;
    }
    std::printf("%d of %zu public request tests passed, %d skipped\n", passed,
                sizeof(tests) / sizeof(tests[0]), skipped);
    return 0;
}
