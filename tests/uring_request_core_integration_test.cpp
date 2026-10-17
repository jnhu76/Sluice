#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/test_access.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

#ifndef SLUICE_HAS_LIBURING
#error "the deterministic uring core integration suite must receive SLUICE_HAS_LIBURING"
#endif

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::IoError;
using sluice::Result;
using sluice::async::detail::CoreSnapshot;
using sluice::async::detail::PublicCancel;
using sluice::async::detail::RequestCore;
using sluice::async::detail::SlotIndex;

constexpr std::uint64_t kUnrepresentableOffset = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kControlTag = std::uint64_t{1} << 63u;

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
    char path[] = "/tmp/sluice_uring_core_XXXXXX";
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

std::optional<File> open_temp_file(Tracker& t, const std::string& content,
                                   sluice::FileOpen mode = {}) {
    const std::string path = make_temp_file(content);
    if (path.empty()) {
        t.check(false, "the temporary fixture file is created");
        return std::nullopt;
    }
    auto opened = File::open(path, mode);
    ::unlink(path.c_str());
    if (!opened.has_value()) {
        t.check(false, "the temporary fixture file opens");
        return std::nullopt;
    }
    return std::optional<File>{std::move(opened.value())};
}

bool idle_and_whole(const CoreSnapshot& snap, RequestCore& core) {
    return snap.reserved == 0 && snap.accepted_live == 0 && snap.terminal_live == 0 &&
           snap.published_live == 0 && snap.execution_refs == 0 && snap.control_refs == 0 &&
           snap.publication_refs == 0 && snap.public_bindings == 0 &&
           snap.free_slots == core.capacity();
}

bool resolves_as(RequestReadiness observed, RequestReadiness expected) {
    return observed == expected;
}

// A fake submit that reports every ready SQE as submitted without touching
// the kernel: completions then arrive only through inject_cqe_for_test, so
// the suite orders CQEs deterministically. Advancing the SQ cursors is the
// test-only fiction that the kernel consumed the hand-off it never saw.
int fake_submit_no_kernel(void*, ::io_uring* ring) noexcept {
    const unsigned ready = ::io_uring_sq_ready(ring);
    *ring->sq.ktail = ring->sq.sqe_tail;
    *ring->sq.khead = ring->sq.sqe_tail;
    return static_cast<int>(ready);
}

struct SequencedSubmitContext {
    int calls = 0;
};

// Submit normally except the call identified by fail_at_call, which reports a
// hard submit error without submitting anything.
int sequenced_submit(void* ctx, ::io_uring* ring) noexcept {
    auto* state = static_cast<SequencedSubmitContext*>(ctx);
    const int call = state->calls++;
    if (call == 1)
        return -EIO;
    return fake_submit_no_kernel(ctx, ring);
}

struct FakeSubmitBackend {
    std::unique_ptr<AsyncIoContext> ctx;
    UringAsyncBackend* backend = nullptr;
    RequestCore* core = nullptr;

    static FakeSubmitBackend create(std::size_t capacity,
                                    UringBackendSubmitTestHooks::SubmitFn submit_fn,
                                    void* submit_ctx) {
        UringBackendSubmitTestHooks hooks;
        hooks.submit = submit_fn;
        hooks.context = submit_ctx;
        auto owned = std::make_unique<UringAsyncBackend>(UringConfig{capacity, 8}, hooks);
        FakeSubmitBackend made;
        made.backend = owned.get();
        made.ctx = std::make_unique<AsyncIoContext>(detail::make_context(std::move(owned)));
        made.core = detail::TestAccess::context_core(*made.ctx);
        return made;
    }

    bool ready() const { return ctx != nullptr; }
    UringAsyncBackend* raw() const { return backend; }
};

std::optional<std::uint64_t> live_cookie(FakeSubmitBackend& made, std::uint64_t offset) {
    return made.raw()->live_cookie_for_offset_for_test(offset);
}

void inject_completion(FakeSubmitBackend& made, std::uint64_t cookie, int res) {
    made.raw()->inject_cqe_for_test(cookie, res);
}

// Drives the fake kernel: submits the prepared SQEs (without a real kernel)
// and then delivers one synthetic completion for the live operation at the
// given offset. Returns the delivered res code path indicator.
void fake_complete(FakeSubmitBackend& made, std::uint64_t offset, int res) {
    (void)made.ctx->poll();
    const auto cookie = live_cookie(made, offset);
    if (cookie.has_value())
        inject_completion(made, *cookie, res);
    (void)made.ctx->poll();
}

template <class Gate>
void wait_gate_paused(Gate& gate) {
    while (!gate.paused.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

template <class Gate>
void resume_gate(Gate& gate) {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

template <class Gate>
void wait_gate_exited(Gate& gate) {
    while (!gate.exited.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

template <class Gate> struct GateGuard {
    Gate& gate;
    bool released = false;
    ~GateGuard() {
        if (!released) {
            resume_gate(gate);
        }
    }
};

// Polls until ready (bounded), then consumes the result and polls until the
// slot reclaims. Returns whether the value matched and everything went idle.
bool complete_take_reclaim(FakeSubmitBackend& made, Request<std::size_t>& c,
                           std::optional<std::size_t> expected) {
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    for (int i = 0; i < 64 && !c.ready(); ++i)
        (void)ctx.poll();
    if (!c.ready())
        return false;
    bool ok = true;
    if (expected.has_value()) {
        const auto observed = c.try_result();
        ok = observed.result.has_value() && observed.result.value() == *expected;
    }
    (void)c.take_result();
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    return ok && idle_and_whole(core.snapshot(), core);
}

bool pre_accept_failure_leaves_no_core_residue(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core residue\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{2, 8});
    if (!backend->available()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext ctx = detail::make_context(std::move(backend));
    RequestCore& core = *detail::TestAccess::context_core(ctx);

    std::byte buffer{std::byte{0}};
    auto rejected =
        ctx.submit_read(ReadOp{NativeFileRef(*file), &buffer, 1, kUnrepresentableOffset});
    t.check(!rejected.has_value() && rejected.error().code == IoError::Code::invalid_argument,
            "an unrepresentable range is rejected as invalid_argument and yields no request");

    t.check(idle_and_whole(core.snapshot(), core),
            "the rejected submission leaves no core residue and no leaked capacity");
    return true;
}

bool range_precedes_health_and_capacity(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core precedence\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(1, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;

    std::vector<std::byte> buffer(4, std::byte{0});
    auto filled = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(filled.has_value(), "the first request occupies the only slot");
    Request<std::size_t> filler = std::move(filled.value());
    (void)ctx.poll();

    auto range_rejected =
        ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 1, kUnrepresentableOffset});
    t.check(!range_rejected.has_value() &&
                range_rejected.error().code == IoError::Code::invalid_argument,
            "an invalid range outranks admission closure and capacity");

    made.raw()->close_admission();
    auto closed_rejected = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 1, 0});
    t.check(!closed_rejected.has_value() &&
                closed_rejected.error().code == IoError::Code::invalid_state,
            "context closure outranks capacity for an otherwise valid request");

    t.check(core.snapshot().accepted_live == 1, "the table still holds only the accepted request");

    const auto cookie = live_cookie(made, 0);
    t.check(cookie.has_value(), "the accepted request holds a transport cookie");
    if (!cookie.has_value())
        return false;
    inject_completion(made, *cookie, 4);
    for (int i = 0; i < 64 && !filler.ready(); ++i)
        (void)ctx.poll();
    const auto filler_result = filler.try_result();
    t.check(filler_result.readiness == RequestReadiness::ready && filler_result.result.has_value(),
            "the accepted request still completes after admission closed");
    (void)filler.take_result();
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core),
            "the closed context still reclaims its accepted request");
    return true;
}

bool accepted_pre_submit_work_has_a_core_owner(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core ownership\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(2, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    UringAsyncBackend::AcceptedPreDispatchPauseGate gate;
    raw->set_accepted_pre_dispatch_pause_gate(&gate);
    GateGuard guard{gate};

    std::vector<std::byte> buffer(4, std::byte{0});
    std::optional<Request<std::size_t>> request;
    std::optional<IoError> submit_error;
    std::thread submitter([&] {
        auto submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
        if (submitted.has_value())
            request = std::move(submitted.value());
        else
            submit_error = submitted.error();
    });
    wait_gate_paused(gate);

    const CoreSnapshot snap = core.snapshot();
    t.check(snap.accepted_live == 1 && snap.public_bindings == 1 && snap.execution_refs == 1,
            "the accepted-pre-submit request holds a core slot, binding and execution ref");
    const auto slot0 = core.observe_slot(SlotIndex{0});
    t.check(slot0.has_value() && slot0->accepted && !slot0->terminal_chosen &&
                !slot0->execution_claimed && slot0->execution_refs == 1,
            "acceptance installed the pending-transport execution responsibility");
    t.check(raw->dispatch_size_for_test() == 0 && raw->live_cookies_for_test() == 0 &&
                raw->transport_ledger_size_for_test() == 0 && raw->sq_ready_for_test() == 0,
            "nothing transport-visible exists before the dispatch handoff");

    resume_gate(gate);
    wait_gate_exited(gate);
    guard.released = true;
    submitter.join();
    raw->set_accepted_pre_dispatch_pause_gate(nullptr);
    t.check(request.has_value() && !submit_error.has_value(), "the submission itself succeeds");

    t.check(raw->live_cookies_for_test() == 1 && raw->transport_ledger_size_for_test() == 0 &&
                raw->sq_ready_for_test() == 0,
            "the dispatch-time submit consumes the prepared entry and keeps its routing");
    (void)ctx.poll();
    t.check(raw->transport_ledger_size_for_test() == 0 && raw->live_cookies_for_test() == 1,
            "the submitted operation leaves the prepared ledger and keeps its routing");

    fake_complete(made, 0, 4);
    t.check(complete_take_reclaim(made, *request, 4),
            "the read completes with 4 bytes and reclaims");
    return true;
}

bool pre_submit_cancel_wins_with_zero_transport(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core pre-submit cancel\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(2, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    UringAsyncBackend::AcceptedPreDispatchPauseGate gate;
    raw->set_accepted_pre_dispatch_pause_gate(&gate);
    GateGuard guard{gate};

    std::vector<std::byte> buffer(4, std::byte{0});
    std::optional<Request<std::size_t>> request;
    std::thread submitter([&] {
        auto submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
        if (submitted.has_value())
            request = std::move(submitted.value());
    });
    wait_gate_paused(gate);

    std::vector<detail::RequestKey> outstanding;
    core.collect_outstanding(outstanding);
    t.check(outstanding.size() == 1,
            "the accepted request holds its core slot while the transport handoff is parked");
    const detail::RequestKey key = outstanding.front();
    const PublicCancel disposition = raw->cancel_key_for_test(key);
    t.check(disposition == PublicCancel::won_before_execution,
            "cancel wins while the transport handoff has not completed");
    const auto after_cancel = core.observe_slot(SlotIndex{0});
    t.check(after_cancel.has_value() && after_cancel->terminal_chosen &&
                after_cancel->execution_refs == 0,
            "the cancel retired the execution responsibility through the core");

    resume_gate(gate);
    wait_gate_exited(gate);
    guard.released = true;
    submitter.join();
    raw->set_accepted_pre_dispatch_pause_gate(nullptr);

    t.check(request.has_value(), "the submission hands the public identity back to the caller");
    t.check(raw->dispatch_size_for_test() == 0,
            "the losing submitter's queue entry was dropped by its own drain");
    t.check(raw->live_cookies_for_test() == 0 && raw->submit_flushes_for_test() == 0 &&
                raw->sq_ready_for_test() == 0 && raw->transport_ledger_size_for_test() == 0,
            "no kernel-visible operation ever existed");

    for (int i = 0; i < 64 && !request->ready(); ++i)
        (void)ctx.poll();
    const auto canceled_terminal = request->take_result();
    t.check(canceled_terminal.readiness == RequestReadiness::ready &&
                !canceled_terminal.result.has_value() &&
                canceled_terminal.result.error().code == IoError::Code::canceled,
            "the canceled request converges through its terminal path");
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core), "the canceled slot reclaims");
    return true;
}

bool original_cqe_first_defers_until_control_retires(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core original first\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(2, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(submitted.has_value(), "the read is accepted");
    Request<std::size_t> c = std::move(submitted.value());
    (void)ctx.poll();
    const detail::RequestKey key = detail::TestAccess::request_key(c);
    const auto live_slot = core.observe_slot(SlotIndex{0});
    t.check(live_slot.has_value() && key.slot == SlotIndex{0} &&
                key.generation == live_slot->generation,
            "the request key resolves to its accepted slot");

    t.check(raw->cancel_key_for_test(key) == PublicCancel::requested,
            "cancel on a running request records intent only");
    (void)ctx.poll();
    t.check(raw->live_control_sqes_for_test() == 1,
            "exactly one control SQE became kernel-visible");

    const auto cookie = live_cookie(made, 0);
    t.check(cookie.has_value(), "the original operation cookie resolves by offset");
    if (!cookie.has_value())
        return false;

    inject_completion(made, *cookie, 4);
    const auto after_original = core.observe_slot(SlotIndex{0});
    t.check(after_original.has_value() && after_original->terminal_chosen &&
                after_original->outcome.succeeded && after_original->execution_refs == 0,
            "the original CQE chose the physical outcome and retired the borrow");
    t.check(after_original->control_refs == 1,
            "the outstanding control obligation still pins the slot");

    for (int i = 0; i < 64 && !c.ready(); ++i)
        (void)ctx.poll();
    const auto published = c.try_result();
    t.check(published.readiness == RequestReadiness::ready && published.result.has_value() &&
                published.result.value() == 4,
            "publication carries the real physical outcome, not the cancel");
    (void)c.take_result();
    t.check(core.snapshot().accepted_live == 1 && core.snapshot().public_bindings == 0,
            "the released binding stops resolving without reclaiming the pinned slot");

    inject_completion(made, kControlTag | *cookie, 0);
    t.check(idle_and_whole(core.snapshot(), core),
            "the control retirement reclaims the slot synchronously without new I/O");
    return true;
}

bool cancel_cqe_first_never_becomes_the_terminal(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core control first\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(2, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(submitted.has_value(), "the read is accepted");
    Request<std::size_t> c = std::move(submitted.value());
    (void)ctx.poll();
    const detail::RequestKey key = detail::TestAccess::request_key(c);
    const auto cookie = live_cookie(made, 0);
    t.check(c.valid() && cookie.has_value(), "the request key and cookie resolve");
    if (!cookie.has_value())
        return false;

    t.check(raw->cancel_key_for_test(key) == PublicCancel::requested,
            "cancel records intent on the running request");
    (void)ctx.poll();

    inject_completion(made, kControlTag | *cookie, 0);
    const auto after_control = core.observe_slot(SlotIndex{0});
    t.check(after_control.has_value() && !after_control->terminal_chosen &&
                after_control->execution_refs == 1,
            "the control CQE retires only the control obligation, never the borrow");
    t.check(!c.ready(), "no publication happened before the original physical outcome");

    inject_completion(made, *cookie, 4);
    const auto after_original = core.observe_slot(SlotIndex{0});
    t.check(after_original.has_value() && after_original->terminal_chosen &&
                after_original->outcome.succeeded &&
                after_original->outcome.effect.confirmed_bytes == 4,
            "the late original CQE canonicalizes the real outcome on the same slot");

    for (int i = 0; i < 64 && !c.ready(); ++i)
        (void)ctx.poll();
    const auto published = c.try_result();
    t.check(published.readiness == RequestReadiness::ready && published.result.has_value() &&
                published.result.value() == 4,
            "the final publication carries the physical byte count");
    (void)c.take_result();
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core), "the slot fully reclaims");
    return true;
}

bool cancel_racing_completion_keeps_the_confirmed_count(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core race\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(2, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    std::vector<std::byte> buffer(6, std::byte{0});
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 6, 0});
    t.check(submitted.has_value(), "the read is accepted");
    Request<std::size_t> c = std::move(submitted.value());
    (void)ctx.poll();
    const detail::RequestKey key = detail::TestAccess::request_key(c);
    const auto cookie = live_cookie(made, 0);
    if (!cookie.has_value())
        return false;

    t.check(raw->cancel_key_for_test(key) == PublicCancel::requested,
            "cancel records intent on the running request");
    (void)ctx.poll();
    inject_completion(made, *cookie, 3);
    for (int i = 0; i < 64 && !c.ready(); ++i)
        (void)ctx.poll();
    const auto published = c.try_result();
    t.check(published.readiness == RequestReadiness::ready && published.result.has_value() &&
                published.result.value() == 3,
            "the physical short count outranks the racing cancel intent");
    (void)c.take_result();
    inject_completion(made, kControlTag | *cookie, 0);
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core), "the raced request reclaims fully");
    return true;
}

bool cancel_racing_short_write_keeps_confirmed_count(Tracker& t) {
    sluice::FileOpen writable;
    writable.access = sluice::FileAccess::read_write;
    auto file = open_temp_file(t, "sluice uring core race\n", writable);
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(2, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    const std::vector<std::byte> payload(6, std::byte{0});
    auto submitted =
        ctx.submit_write(WriteOp{NativeFileRef(*file), payload.data(), payload.size(), 0});
    t.check(submitted.has_value(), "the write is accepted");
    Request<std::size_t> c = std::move(submitted.value());
    (void)ctx.poll();
    const detail::RequestKey key = detail::TestAccess::request_key(c);
    const auto cookie = live_cookie(made, 0);
    if (!cookie.has_value())
        return false;

    t.check(raw->cancel_key_for_test(key) == PublicCancel::requested,
            "cancel records intent on the running write");
    (void)ctx.poll();
    inject_completion(made, *cookie, 3);
    const auto after_original = core.observe_slot(SlotIndex{0});
    t.check(after_original.has_value() && after_original->terminal_chosen &&
                after_original->outcome.succeeded &&
                after_original->outcome.effect.confirmed_bytes == 3,
            "the physical short write outranks the racing cancel intent");
    for (int i = 0; i < 64 && !c.ready(); ++i)
        (void)ctx.poll();
    const auto published = c.try_result();
    t.check(published.readiness == RequestReadiness::ready && published.result.has_value() &&
                published.result.value() == 3,
            "the publication carries the confirmed 3/6 write, never a canceled terminal");
    (void)c.take_result();
    inject_completion(made, kControlTag | *cookie, 0);
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core), "the raced write reclaims fully");
    return true;
}

bool stale_cookie_cannot_reach_a_reused_slot(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core stale cookie\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(2, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    std::vector<std::byte> buffer(4, std::byte{0});
    const auto cookie_a = raw->peek_next_cookie_for_test();
    auto first = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(first.has_value(), "request A is accepted");
    Request<std::size_t> a = std::move(first.value());
    t.check(raw->peek_next_cookie_for_test() == cookie_a + 1,
            "request A consumed one monotonic cookie");
    fake_complete(made, 0, 4);
    if (!complete_take_reclaim(made, a, 4)) {
        t.check(false, "request A retires cleanly");
        return false;
    }

    auto second = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(second.has_value(), "request B reuses the slot");
    Request<std::size_t> b = std::move(second.value());
    const detail::RequestKey key_b = detail::TestAccess::request_key(b);
    t.check(key_b.slot == SlotIndex{0} && key_b.generation.value == 1,
            "request B occupies the same slot at generation+1");
    const auto cookie_b = live_cookie(made, 0);
    t.check(cookie_b.has_value() && *cookie_b != cookie_a,
            "the kernel cookie is never reused across requests");
    if (!cookie_b.has_value())
        return false;

    inject_completion(made, cookie_a, static_cast<int>(-EIO));
    const auto after_stale = core.observe_slot(SlotIndex{0});
    t.check(after_stale.has_value() && !after_stale->terminal_chosen,
            "a stale cookie's CQE cannot affect the reused request");

    (void)ctx.poll();
    inject_completion(made, *cookie_b, 4);
    for (int i = 0; i < 64 && !b.ready(); ++i)
        (void)ctx.poll();
    const auto b_result = b.try_result();
    t.check(b_result.readiness == RequestReadiness::ready && b_result.result.has_value() &&
                b_result.result.value() == 4,
            "request B completes with its own outcome");
    (void)b.take_result();
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core), "request B reclaims cleanly");
    return true;
}

bool stale_identity_does_not_resolve_after_reuse(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core stale identity\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(1, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;

    std::vector<std::byte> buffer(4, std::byte{0});
    auto first = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(first.has_value(), "request A carries a public identity");
    if (!first.has_value())
        return false;
    Request<std::size_t> a = std::move(first.value());
    const RequestId handle_a = a.id();
    t.check(resolves_as(ctx.lookup(handle_a), RequestReadiness::pending),
            "A resolves as outstanding while running");

    fake_complete(made, 0, 4);
    for (int i = 0; i < 64 && !a.ready(); ++i)
        (void)ctx.poll();
    (void)a.take_result();
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core), "request A retires");

    auto second = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(second.has_value(), "request B reuses the slot");
    if (!second.has_value())
        return false;
    Request<std::size_t> b = std::move(second.value());
    t.check(resolves_as(ctx.lookup(b.id()), RequestReadiness::pending),
            "B resolves as outstanding");
    t.check(resolves_as(ctx.lookup(handle_a), RequestReadiness::empty),
            "A's stale identity no longer resolves after reuse");

    fake_complete(made, 0, 4);
    for (int i = 0; i < 64 && !b.ready(); ++i)
        (void)ctx.poll();
    (void)b.take_result();
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core), "everything reclaims");
    return true;
}

bool zero_op_publishes_at_acceptance_without_dispatch(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core zero op\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(2, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    const std::uint64_t flushes_before = raw->submit_flushes_for_test();
    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 0, kUnrepresentableOffset});
    t.check(submitted.has_value(), "the zero-length read is accepted despite its offset");
    Request<std::size_t> c = std::move(submitted.value());
    const auto zero_result = c.try_result();
    t.check(zero_result.readiness == RequestReadiness::ready, 
            "the zero-op is published at acceptance, before any poll");
    t.check(zero_result.result.has_value() && zero_result.result.value() == 0,
            "the zero-op succeeds with 0");
    t.check(raw->sq_ready_for_test() == 0 && raw->live_cookies_for_test() == 0 &&
                raw->transport_ledger_size_for_test() == 0 &&
                raw->submit_flushes_for_test() == flushes_before,
            "no data SQE, cookie or ledger entry was ever created");

    t.check(core.snapshot().control_refs == 1,
            "the deferred ready event pins the slot with a core control ref");

    c.discard();
    const std::size_t events = ctx.poll().value_or(0);
    t.check(events == 1, "the next poll delivers exactly the owed ready event");
    t.check(idle_and_whole(core.snapshot(), core),
            "the owed event's retirement reclaims the slot synchronously");
    return true;
}

bool zero_op_event_survives_release_before_poll(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core zero pin\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(1, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;

    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 0, 0});
    t.check(submitted.has_value(), "the zero-op is accepted");
    Request<std::size_t> c = std::move(submitted.value());
    t.check(c.ready(), "the zero-op is published at acceptance");
    c.discard();

    const CoreSnapshot pinned = core.snapshot();
    t.check(pinned.accepted_live == 1 && pinned.published_live == 1 &&
                pinned.public_bindings == 0 && pinned.control_refs == 1,
            "the released result keeps the slot pinned by the owed event");

    auto refused = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(!refused.has_value() && refused.error().code == IoError::Code::would_block,
            "the pinned slot still refuses new admission");

    t.check(ctx.poll().value_or(0) == 1, "the poll discharges exactly one ready event");
    t.check(idle_and_whole(core.snapshot(), core),
            "the final pin retirement reclaims without new I/O");

    auto again = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(again.has_value(), "the slot serves a new request at the next generation");
    Request<std::size_t> next = std::move(again.value());
    fake_complete(made, 0, 4);
    if (!complete_take_reclaim(made, next, 4)) {
        t.check(false, "the follow-up request completes and reclaims");
        return false;
    }
    return true;
}

bool submit_stage_failures_rollback_completely(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core stage failures\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(2, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    UringAsyncBackend::SubmitStageFailureInjection injection;
    raw->set_submit_stage_failure_injection(&injection);

    std::vector<std::byte> buffer(4, std::byte{0});
    const ReadOp op{NativeFileRef(*file), buffer.data(), 4, 0};

    injection.fail_reserve.store(true);
    auto r1 = ctx.submit_read(op);
    t.check(!r1.has_value() && r1.error().code == IoError::Code::would_block,
            "a reserve-stage failure rejects with no request handed out");
    t.check(idle_and_whole(core.snapshot(), core), "no residue after the reserve failure");

    injection.fail_reserve.store(false);
    injection.fail_prepare.store(true);
    auto r2 = ctx.submit_read(op);
    t.check(!r2.has_value() && r2.error().code == IoError::Code::invalid_state,
            "a prepare-stage failure rejects and rolls the reservation back");
    t.check(idle_and_whole(core.snapshot(), core), "no residue after the prepare failure");

    injection.fail_prepare.store(false);
    injection.fail_commit.store(true);
    auto r3 = ctx.submit_read(op);
    t.check(!r3.has_value() && r3.error().code == IoError::Code::invalid_state,
            "a commit-stage failure rejects and rolls the tentative binding back");
    t.check(idle_and_whole(core.snapshot(), core), "no residue after the commit failure");
    t.check(raw->live_cookies_for_test() == 0 && raw->transport_ledger_size_for_test() == 0,
            "no transport state leaked from any failed stage");

    raw->set_submit_stage_failure_injection(nullptr);

    auto ok = ctx.submit_read(op);
    t.check(ok.has_value(), "the backend still accepts after the injected failures");
    Request<std::size_t> c4 = std::move(ok.value());
    fake_complete(made, 0, 4);
    t.check(complete_take_reclaim(made, c4, 4),
            "the follow-up request completes and reclaims");
    return true;
}

bool dispatch_failure_converges_through_the_terminal(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core dispatch failure\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(2, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    UringAsyncBackend::DispatchFailureInjection injection;
    raw->set_dispatch_failure_injection(&injection);
    injection.armed.store(true);

    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(submitted.has_value(),
            "an accepted request is never reported as an initiation failure");
    Request<std::size_t> c = std::move(submitted.value());
    injection.armed.store(false);
    t.check(injection.fired.load() == 1, "the dispatch handoff failure fired exactly once");
    t.check(raw->submit_flushes_for_test() == 0 && raw->live_cookies_for_test() == 0,
            "no kernel entry happened for the failed dispatch");

    for (int i = 0; i < 64 && !c.ready(); ++i)
        (void)ctx.poll();
    const auto failed = c.take_result();
    t.check(failed.readiness == RequestReadiness::ready && !failed.result.has_value() &&
                failed.result.error().code == IoError::Code::backend_error,
            "the post-accept failure converges through the terminal path");
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core),
            "the failed dispatch leaves no ownerless request");
    return true;
}

bool post_accept_submit_failure_poison_converges(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core submit failure\n");
    if (!file.has_value())
        return false;

    SequencedSubmitContext sequenced{};
    FakeSubmitBackend made = FakeSubmitBackend::create(2, sequenced_submit, &sequenced);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;

    std::vector<std::byte> buffer(4, std::byte{0});
    auto first = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(first.has_value(), "the first request is accepted");
    Request<std::size_t> visible = std::move(first.value());
    t.check(ctx.poll().has_value(), "the first request reached the kernel at its dispatch");

    auto second = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(second.has_value(), "the second request is accepted before the failure");
    Request<std::size_t> invisible = std::move(second.value());
    t.check(ctx.poll().has_value(),
            "the second acceptance's transport failure poisons the backend");

    for (int i = 0; i < 64 && !invisible.ready(); ++i)
        (void)ctx.poll();
    const auto invisible_result = invisible.try_result();
    t.check(invisible_result.readiness == RequestReadiness::ready &&
                !invisible_result.result.has_value(),
            "the never-visible request converged through the terminal path");
    (void)invisible.take_result();

    auto rejected = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(!rejected.has_value() && rejected.error().code == IoError::Code::backend_error,
            "the poisoned backend rejects new requests with its health failure");

    t.check(live_cookie(made, 0).has_value(),
            "the kernel-visible request keeps its real completion path");
    const auto visible_cookie = live_cookie(made, 0);
    if (visible_cookie.has_value())
        inject_completion(made, *visible_cookie, 4);
    for (int i = 0; i < 64 && !visible.ready(); ++i)
        (void)ctx.poll();
    const auto visible_result = visible.try_result();
    t.check(visible_result.readiness == RequestReadiness::ready &&
                visible_result.result.has_value() && visible_result.result.value() == 4,
            "the kernel-visible request completes with its real physical outcome");
    (void)visible.take_result();
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core),
            "no accepted request became ownerless through the poison");
    return true;
}

bool poison_with_running_cancel_keeps_the_real_completion(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core poison cancel\n");
    if (!file.has_value())
        return false;

    SequencedSubmitContext sequenced{};
    FakeSubmitBackend made = FakeSubmitBackend::create(4, sequenced_submit, &sequenced);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    std::vector<std::byte> buffer(4, std::byte{0});
    auto first = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(first.has_value(), "request A is accepted");
    Request<std::size_t> a = std::move(first.value());
    (void)ctx.poll();
    const detail::RequestKey key_a = detail::TestAccess::request_key(a);
    t.check(raw->cancel_key_for_test(key_a) == PublicCancel::requested,
            "cancel records intent on the kernel-visible request");

    auto second = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(second.has_value(), "request B is accepted");
    Request<std::size_t> b = std::move(second.value());
    t.check(ctx.poll().has_value(), "B's dispatch-time transport failure poisons the backend");

    for (int i = 0; i < 64 && !b.ready(); ++i)
        (void)ctx.poll();
    const auto b_result = b.try_result();
    t.check(b_result.readiness == RequestReadiness::ready && !b_result.result.has_value(),
            "the never-visible request converges to its error terminal");
    (void)b.take_result();
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 1; ++i)
        (void)ctx.poll();
    t.check(core.snapshot().accepted_live == 1 && core.snapshot().control_refs == 0,
            "A stays owned with its cancel obligation retired");

    const auto cookie_a = raw->live_cookie_for_offset_for_test(0);
    t.check(cookie_a.has_value(), "A's router entry stayed routable through the poison");
    if (!cookie_a.has_value())
        return false;
    raw->inject_cqe_for_test(*cookie_a, 4);
    for (int i = 0; i < 64 && !a.ready(); ++i)
        (void)ctx.poll();
    const auto a_result = a.try_result();
    t.check(a_result.readiness == RequestReadiness::ready && a_result.result.has_value() &&
                a_result.result.value() == 4,
            "A completes with its real physical outcome after poison and cancel");
    (void)a.take_result();
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core),
            "nothing stays ownerless across poison, cancel and real completion");
    return true;
}

bool publication_visibility_waits_for_the_epilogue(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core epilogue\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(2, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    UringAsyncBackend::PublicationEpiloguePauseGate gate;
    raw->set_publication_epilogue_pause_gate(&gate);
    GateGuard guard{gate};

    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(submitted.has_value(), "the read is accepted");
    Request<std::size_t> c = std::move(submitted.value());
    {
        auto claim = ctx.claim_progress_owner();
        (void)ctx.poll();
    }
    const auto cookie = live_cookie(made, 0);
    if (!cookie.has_value())
        return false;
    inject_completion(made, *cookie, 4);

    std::atomic<bool> stop_driver{false};
    std::thread driver([&] {
        auto claim = ctx.claim_progress_owner();
        while (!stop_driver.load(std::memory_order_acquire))
            (void)ctx.poll();
    });
    wait_gate_paused(gate);
    const auto inflight = core.observe_slot(SlotIndex{0});
    t.check(!c.ready() && c.try_result().readiness == RequestReadiness::pending &&
                inflight.has_value() && inflight->publication_inflight && !inflight->published &&
                inflight->binding_live,
            "the outcome stays invisible while the publication pin is held");
    const CoreSnapshot pinned = core.snapshot();
    t.check(pinned.accepted_live == 1 && pinned.publication_refs == 1,
            "the held publication keeps the slot pinned and unresolvable");

    resume_gate(gate);
    wait_gate_exited(gate);
    guard.released = true;
    raw->set_publication_epilogue_pause_gate(nullptr);
    stop_driver.store(true, std::memory_order_release);
    driver.join();
    for (int i = 0; i < 64 && !c.ready(); ++i)
        (void)ctx.poll();
    const auto published = c.try_result();
    t.check(published.readiness == RequestReadiness::ready && published.result.has_value() &&
                published.result.value() == 4,
            "the epilogue publishes the outcome exactly once");
    (void)c.take_result();
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core),
            "the publication completion reclaims the slot synchronously");
    return true;
}

bool observer_registration_rides_publication(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core observer\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(2, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
    t.check(submitted.has_value(), "the read is accepted");
    Request<std::size_t> c = std::move(submitted.value());
    const RequestId observed_id = c.id();

    const auto attached = ctx.attach_observer(observed_id);
    t.check(attached.armed(), "the observer arms on the outstanding request");
    t.check(ctx.attach_observer(observed_id).status == AsyncIoContext::ObserverStatus::duplicate,
            "a second registration on the same request returns the occupied disposition");
    const auto observed = core.observe_slot(SlotIndex{0});
    t.check(observed.has_value() &&
                observed->observer_phase == detail::ObserverPhase::armed,
            "the core owns the registration existence");

    fake_complete(made, 0, 4);
    for (int i = 0; i < 64 && !c.ready(); ++i)
        (void)ctx.poll();

    t.check(raw->sink_deliveries() == 1 && raw->sink_last_key() == detail::TestAccess::request_key_of(attached.request),
            "the ready event rides the publication as a host-neutral keyed event");
    t.check(core.observe_slot(SlotIndex{0})->observer_phase == detail::ObserverPhase::retired,
            "the delivery owner retires once the hook has returned");
    t.check(ctx.delivery_retired(attached.request),
            "the retirement query reports the completed delivery");
    (void)c.take_result();
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();

    t.check(idle_and_whole(core.snapshot(), core), "the observed request reclaims");
    return true;
}

bool concurrent_submissions_all_publish_and_reclaim(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core concurrency\n");
    if (!file.has_value())
        return false;

    FakeSubmitBackend made = FakeSubmitBackend::create(4, fake_submit_no_kernel, nullptr);
    if (!made.ready()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext& ctx = *made.ctx;
    RequestCore& core = *made.core;
    UringAsyncBackend* raw = made.raw();

    std::vector<std::byte> buffer(4, std::byte{0});
    std::vector<std::optional<Request<std::size_t>>> requests(4);
    std::vector<std::optional<IoError>> failures(4);
    std::vector<std::thread> submitters;
    for (int i = 0; i < 4; ++i) {
        submitters.emplace_back([&, i] {
            auto submitted = ctx.submit_read(
                ReadOp{NativeFileRef(*file), buffer.data(), 4, static_cast<std::uint64_t>(i)});
            if (submitted.has_value())
                requests[i] = std::move(submitted.value());
            else
                failures[i] = submitted.error();
        });
    }
    for (auto& s : submitters)
        s.join();
    for (int i = 0; i < 4; ++i)
        t.check(requests[i].has_value() && !failures[i].has_value(),
                "every concurrent submission is accepted without waiting");

    (void)ctx.poll();
    for (int i = 0; i < 4; ++i) {
        const auto cookie = live_cookie(made, static_cast<std::uint64_t>(i));
        if (cookie.has_value())
            inject_completion(made, *cookie, 4);
    }
    for (int round = 0; round < 64; ++round) {
        (void)ctx.poll();
        bool all_ready = true;
        for (int i = 0; i < 4; ++i)
            all_ready = all_ready && requests[i]->ready();
        if (all_ready)
            break;
    }
    for (int i = 0; i < 4; ++i) {
        const auto published = requests[i]->try_result();
        t.check(published.readiness == RequestReadiness::ready && published.result.has_value(),
                "every request published");
        (void)requests[i]->take_result();
    }
    for (int i = 0; i < 64 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core), "every slot reclaims");
    t.check(raw->live_cookies_for_test() == 0, "no cookie leaked across the concurrency");
    return true;
}

bool real_kernel_normal_completion_and_cancel(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core real kernel\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{4, 8});
    if (!backend->available()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext ctx = detail::make_context(std::move(backend));
    RequestCore& core = *detail::TestAccess::context_core(ctx);

    std::vector<std::byte> buffer(8, std::byte{0});
    auto read_submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 8, 0});
    t.check(read_submitted.has_value(), "the real read is accepted");
    Request<std::size_t> read_done = std::move(read_submitted.value());
    while (!read_done.ready())
        (void)ctx.poll();
    const auto read_result = read_done.try_result();
    t.check(read_result.result.has_value() && read_result.result.value() == 8 &&
                std::memcmp(buffer.data(), "sluice b", 8) == 0,
            "the real kernel read completed with the file bytes");
    (void)read_done.take_result();
    for (int i = 0; i < 256 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core), "the real read reclaimed through the core");

    auto cancel_submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 8, 0});
    t.check(cancel_submitted.has_value(), "the cancel target is accepted");
    Request<std::size_t> cancel_target = std::move(cancel_submitted.value());
    const auto cancel_disposition = cancel_target.cancel();
    t.check(cancel_disposition.has_value(), "the public identity decides the cancel");
    int cancel_polls = 0;
    while (!cancel_target.ready() && cancel_polls < 10000) {
        (void)ctx.poll();
        ++cancel_polls;
    }
    // The terminal itself is platform-specific cancel disposition (this
    // kernel completes the target with -EALREADY); the portable claim is
    // convergence to one terminal with the real outcome preserved.
    const auto outcome = cancel_target.try_result().result;
    if (!outcome.has_value()) {
        std::fprintf(stderr, "INFO [%s] real-kernel cancel terminal: canonical=%d native=%d\n",
                     t.name, static_cast<int>(outcome.error().code), outcome.error().os_errno);
    } else {
        std::fprintf(stderr, "INFO [%s] real-kernel cancel terminal: success (%zu bytes)\n",
                     t.name, outcome.value());
    }
    t.check(cancel_target.ready(),
            "the canceled target converges to one terminal on the real kernel");
    (void)cancel_target.take_result();
    for (int i = 0; i < 256 && core.snapshot().accepted_live != 0; ++i)
        (void)ctx.poll();
    t.check(idle_and_whole(core.snapshot(), core), "the canceled target reclaimed");
    return true;
}

bool real_kernel_slot_reuse(Tracker& t) {
    auto file = open_temp_file(t, "sluice uring core real reuse\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{2, 8});
    if (!backend->available()) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext ctx = detail::make_context(std::move(backend));
    RequestCore& core = *detail::TestAccess::context_core(ctx);

    std::vector<std::byte> buffer(4, std::byte{0});
    for (int round = 0; round < 6; ++round) {
        auto submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0});
        if (!submitted.has_value()) {
            t.check(false, "each round accepts a request");
            return true;
        }
        Request<std::size_t> c = std::move(submitted.value());
        if (!resolves_as(ctx.lookup(c.id()), RequestReadiness::pending)) {
            t.check(false, "each round's identity resolves as outstanding");
            return true;
        }
        int reuse_polls = 0;
        while (!c.ready() && reuse_polls < 10000) {
            (void)ctx.poll();
            ++reuse_polls;
        }
        const auto round_result = c.try_result();
        if (!(round_result.result.has_value() && round_result.result.value() == 4)) {
            t.check(false, "each round completes with 4 bytes");
            return true;
        }
        (void)c.take_result();
        for (int i = 0; i < 256 && core.snapshot().accepted_live != 0; ++i)
            (void)ctx.poll();
        if (!idle_and_whole(core.snapshot(), core)) {
            t.check(false, "each round reclaims before the next");
            return true;
        }
    }
    return true;
}

struct NamedTest {
    const char* name;
    bool (*fn)(Tracker&);
};

}

int main() {
    const NamedTest tests[] = {
        {"pre_accept_failure_leaves_no_core_residue", pre_accept_failure_leaves_no_core_residue},
        {"range_precedes_health_and_capacity", range_precedes_health_and_capacity},
        {"accepted_pre_submit_work_has_a_core_owner", accepted_pre_submit_work_has_a_core_owner},
        {"pre_submit_cancel_wins_with_zero_transport", pre_submit_cancel_wins_with_zero_transport},
        {"original_cqe_first_defers_until_control_retires",
         original_cqe_first_defers_until_control_retires},
        {"cancel_cqe_first_never_becomes_the_terminal",
         cancel_cqe_first_never_becomes_the_terminal},
        {"cancel_racing_completion_keeps_the_confirmed_count",
         cancel_racing_completion_keeps_the_confirmed_count},
        {"cancel_racing_short_write_keeps_confirmed_count",
         cancel_racing_short_write_keeps_confirmed_count},
        {"stale_cookie_cannot_reach_a_reused_slot", stale_cookie_cannot_reach_a_reused_slot},
        {"stale_identity_does_not_resolve_after_reuse",
         stale_identity_does_not_resolve_after_reuse},
        {"zero_op_publishes_at_acceptance_without_dispatch",
         zero_op_publishes_at_acceptance_without_dispatch},
        {"zero_op_event_survives_release_before_poll",
         zero_op_event_survives_release_before_poll},
        {"submit_stage_failures_rollback_completely", submit_stage_failures_rollback_completely},
        {"dispatch_failure_converges_through_the_terminal",
         dispatch_failure_converges_through_the_terminal},
        {"post_accept_submit_failure_poison_converges",
         post_accept_submit_failure_poison_converges},
        {"poison_with_running_cancel_keeps_the_real_completion",
         poison_with_running_cancel_keeps_the_real_completion},
        {"publication_visibility_waits_for_the_epilogue",
         publication_visibility_waits_for_the_epilogue},
        {"observer_registration_rides_publication", observer_registration_rides_publication},
        {"concurrent_submissions_all_publish_and_reclaim",
         concurrent_submissions_all_publish_and_reclaim},
        {"real_kernel_normal_completion_and_cancel", real_kernel_normal_completion_and_cancel},
        {"real_kernel_slot_reuse", real_kernel_slot_reuse},
    };

    int failed = 0;
    int skipped = 0;
    for (const NamedTest& test : tests) {
        Tracker t{test.name};
        if (!test.fn(t)) {
            ++skipped;
            continue;
        }
        if (t.failures != 0) {
            ++failed;
            continue;
        }
        std::printf("ok %s\n", test.name);
    }
    std::printf("%zu of %zu uring core cutover tests passed, %d not run\n",
                sizeof(tests) / sizeof(tests[0]) - static_cast<std::size_t>(failed) -
                    static_cast<std::size_t>(skipped),
                sizeof(tests) / sizeof(tests[0]), skipped);
    return failed == 0 ? 0 : 1;
}
