#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/test_access.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

#ifndef SLUICE_HAS_LIBURING
#error "the ready-batch fault oracle must receive SLUICE_HAS_LIBURING"
#endif
#ifndef SLUICE_ASYNC_INTERNAL_TESTING
#error "the ready-batch fault oracle must receive SLUICE_ASYNC_INTERNAL_TESTING"
#endif

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::IoError;
using sluice::async::detail::TestAccess;

constexpr std::uint64_t kControlTag = std::uint64_t{1} << 63u;

struct ScriptedKernel {
    enum class Mode { consume_all, partial_first, poison, hold };

    Mode mode = Mode::consume_all;
    int calls = 0;
};

int scripted_submit(void* context, ::io_uring* ring) noexcept {
    auto* kernel = static_cast<ScriptedKernel*>(context);
    const unsigned ready = ::io_uring_sq_ready(ring);
    *ring->sq.ktail = ring->sq.sqe_tail;
    ++kernel->calls;
    switch (kernel->mode) {
    case ScriptedKernel::Mode::consume_all:
        *ring->sq.khead += ready;
        return static_cast<int>(ready);
    case ScriptedKernel::Mode::partial_first:
        if (kernel->calls == 1 && ready > 0) {
            *ring->sq.khead += 1;
            kernel->mode = ScriptedKernel::Mode::consume_all;
            return 1;
        }
        *ring->sq.khead += ready;
        return static_cast<int>(ready);
    case ScriptedKernel::Mode::poison:
        kernel->mode = ScriptedKernel::Mode::consume_all;
        return -EIO;
    case ScriptedKernel::Mode::hold:
        return 0;
    }
    return 0;
}

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

struct Fixture {
    std::optional<File> file;
    std::vector<std::byte> shadow;
};

std::optional<Fixture> make_fixture(Tracker& t, std::size_t bytes) {
    char path[] = "/tmp/sluice_595_r3_fault_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0) {
        t.check(false, "temporary fixture file is created");
        return std::nullopt;
    }
    Fixture fixture;
    fixture.shadow.resize(bytes);
    for (std::size_t i = 0; i < bytes; ++i)
        fixture.shadow[i] = static_cast<std::byte>(static_cast<unsigned char>((i * 131u + 7u) & 0xffu));
    std::size_t written = 0;
    while (written < bytes) {
        const ssize_t n = ::write(fd, fixture.shadow.data() + written, bytes - written);
        if (n <= 0) {
            ::close(fd);
            ::unlink(path);
            t.check(false, "temporary fixture file is written");
            return std::nullopt;
        }
        written += static_cast<std::size_t>(n);
    }
    ::close(fd);
    auto opened = File::open(path);
    ::unlink(path);
    if (!opened.has_value()) {
        t.check(false, "temporary fixture file opens");
        return std::nullopt;
    }
    fixture.file.emplace(std::move(opened).value());
    return fixture;
}

std::optional<AsyncIoContext> make_ctx(Tracker& t, std::size_t capacity, std::size_t ring_depth,
                                       ScriptedKernel& kernel, UringAsyncBackend** out) {
    UringBackendSubmitTestHooks hooks;
    hooks.context = &kernel;
    hooks.submit = scripted_submit;
    auto owned = std::make_unique<UringAsyncBackend>(
        UringConfig{capacity, static_cast<unsigned>(ring_depth)}, hooks);
    *out = owned.get();
    AsyncIoContext ctx = sluice::async::detail::make_context(std::move(owned));
    return std::optional<AsyncIoContext>{std::move(ctx)};
}

std::vector<Request<std::size_t>>
requests_for(AsyncIoContext& ctx, const UringAsyncBackend::ReadyReadBatchReport& report) {
    std::vector<Request<std::size_t>> requests;
    for (std::size_t i = 0; i < report.accepted; ++i)
        requests.push_back(TestAccess::make_request<std::size_t>(ctx, report.keys[i]));
    return requests;
}

bool all_ready(const std::vector<Request<std::size_t>>& requests) {
    for (const Request<std::size_t>& request : requests) {
        if (!request.ready())
            return false;
    }
    return true;
}

bool poll_ready(AsyncIoContext& ctx, const std::vector<Request<std::size_t>>& requests) {
    for (int round = 0; round < 64; ++round) {
        if (all_ready(requests))
            return true;
        (void)ctx.poll();
    }
    return all_ready(requests);
}

void partial_submit_keeps_residue(Tracker& t) {
    ScriptedKernel kernel;
    kernel.mode = ScriptedKernel::Mode::partial_first;
    UringAsyncBackend* backend = nullptr;
    auto ctx = make_ctx(t, 2, 2, kernel, &backend);
    auto fixture = make_fixture(t, 8192);
    if (!ctx.has_value() || !fixture.has_value())
        return;

    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(4096));
    ReadOp ops[2];
    for (std::size_t i = 0; i < 2; ++i) {
        ops[i].file = NativeFileRef(*fixture->file);
        ops[i].dst = buffers[i].data();
        ops[i].len = buffers[i].size();
        ops[i].offset = i * 4096;
    }
    const auto report = backend->submit_ready_reads(std::span<const ReadOp>(ops, 2));
    t.check(report.accepted == 2, "both reads are accepted before the partial flush");
    t.check(report.flush_attempts == 1, "the batch attempts exactly one flush");
    t.check(report.residue_advertised, "partial kernel consumption advertises dispatch residue");
    t.check(backend->transport_ledger_size_for_test() == 1,
            "one prepared SQE stays in the transport ledger");
    t.check(backend->submit_flushes_for_test() == 1, "one transport flush so far");

    (void)ctx->poll();
    t.check(backend->transport_ledger_size_for_test() == 0,
            "the next progress pass consumes the remaining ledger prefix");
    t.check(backend->submit_flushes_for_test() == 2,
            "the residue costs exactly one further transport flush");

    const auto cookie0 = backend->live_cookie_for_offset_for_test(0);
    const auto cookie1 = backend->live_cookie_for_offset_for_test(4096);
    t.check(cookie0.has_value() && cookie1.has_value(), "both dispatched routes stay live");
    if (cookie0.has_value())
        backend->inject_cqe_for_test(*cookie0, 4096);
    if (cookie1.has_value())
        backend->inject_cqe_for_test(*cookie1, 4096);
    auto requests = requests_for(*ctx, report);
    t.check(poll_ready(*ctx, requests), "both retried reads reach a terminal");
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto observed = requests[i].take_result();
        t.check(observed.readiness == RequestReadiness::ready &&
                    observed.result.has_value() && observed.result.value() == 4096,
                "each read keeps its own byte count after the retry");
    }
}

void poison_on_batch_flush_settles_every_member(Tracker& t) {
    ScriptedKernel kernel;
    kernel.mode = ScriptedKernel::Mode::poison;
    UringAsyncBackend* backend = nullptr;
    auto ctx = make_ctx(t, 2, 2, kernel, &backend);
    auto fixture = make_fixture(t, 8192);
    if (!ctx.has_value() || !fixture.has_value())
        return;

    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(4096));
    ReadOp ops[2];
    for (std::size_t i = 0; i < 2; ++i) {
        ops[i].file = NativeFileRef(*fixture->file);
        ops[i].dst = buffers[i].data();
        ops[i].len = buffers[i].size();
        ops[i].offset = i * 4096;
    }
    const auto report = backend->submit_ready_reads(std::span<const ReadOp>(ops, 2));
    t.check(report.accepted == 2, "both reads are accepted before the poisoned flush");
    t.check(report.residue_advertised, "a poisoned flush advertises the residue wake");

    auto requests = requests_for(*ctx, report);
    t.check(poll_ready(*ctx, requests), "both poisoned members reach a terminal");
    for (Request<std::size_t>& request : requests) {
        const auto observed = request.take_result();
        t.check(observed.readiness == RequestReadiness::ready,
                "the poisoned member publishes a terminal");
        t.check(!observed.result.has_value() &&
                    observed.result.error().code == IoError::Code::backend_error,
                "the poisoned member reports the physical failure, never success");
    }
    const auto outcome = ctx->shutdown(ShutdownPolicy::drain);
    t.check(outcome.has_value() && outcome.value() == ShutdownOutcome::completed,
            "a poisoned batch settles without unresolved accepted work");
}

void control_intent_stops_the_batch(Tracker& t) {
    ScriptedKernel kernel;
    kernel.mode = ScriptedKernel::Mode::hold;
    UringAsyncBackend* backend = nullptr;
    auto ctx = make_ctx(t, 4, 4, kernel, &backend);
    auto fixture = make_fixture(t, 4 * 4096);
    if (!ctx.has_value() || !fixture.has_value())
        return;

    std::vector<std::vector<std::byte>> buffers(4, std::vector<std::byte>(4096));
    ReadOp ops[4];
    for (std::size_t i = 0; i < 4; ++i) {
        ops[i].file = NativeFileRef(*fixture->file);
        ops[i].dst = buffers[i].data();
        ops[i].len = buffers[i].size();
        ops[i].offset = i * 4096;
    }
    const auto held = backend->submit_ready_reads(std::span<const ReadOp>(ops, 4));
    t.check(held.accepted == 4, "the held batch fills the SQ");
    t.check(backend->transport_ledger_size_for_test() == 4,
            "the held batch keeps every SQE unproven");

    auto held_requests = requests_for(*ctx, held);
    const auto cancel = held_requests.front().cancel();
    t.check(cancel.has_value(), "a batched member can be cancelled");
    t.check(backend->pending_control_snapshot_for_test().authoritative_pending != 0,
            "the cancel leaves a sticky control intent");

    ReadOp extra[2];
    for (std::size_t i = 0; i < 2; ++i) {
        extra[i].file = NativeFileRef(*fixture->file);
        extra[i].dst = buffers[i].data();
        extra[i].len = buffers[i].size();
        extra[i].offset = i * 4096;
    }
    const auto stopped = backend->submit_ready_reads(std::span<const ReadOp>(extra, 2));
    t.check(stopped.accepted == 0, "an owed control intent accepts no batch member");
    t.check(stopped.stop == UringAsyncBackend::ReadyBatchStop::control,
            "the control boundary is reported");
    t.check(stopped.untouched_begin == 0 && stopped.untouched == 2,
            "the whole group stays untouched at the control boundary");

    kernel.mode = ScriptedKernel::Mode::consume_all;
    (void)ctx->poll();
    (void)ctx->poll();

    const auto cookie0 = backend->live_cookie_for_offset_for_test(0);
    const auto cookie1 = backend->live_cookie_for_offset_for_test(4096);
    const auto cookie2 = backend->live_cookie_for_offset_for_test(8192);
    const auto cookie3 = backend->live_cookie_for_offset_for_test(12288);
    if (cookie0.has_value()) {
        backend->inject_cqe_for_test(*cookie0, -ECANCELED);
        backend->inject_cqe_for_test(kControlTag | *cookie0, 0);
    }
    if (cookie1.has_value())
        backend->inject_cqe_for_test(*cookie1, 4096);
    if (cookie2.has_value())
        backend->inject_cqe_for_test(*cookie2, 4096);
    if (cookie3.has_value())
        backend->inject_cqe_for_test(*cookie3, 4096);

    t.check(poll_ready(*ctx, held_requests), "every held member reaches a terminal");
    for (std::size_t i = 0; i < held_requests.size(); ++i) {
        const auto observed = held_requests[i].take_result();
        t.check(observed.readiness == RequestReadiness::ready,
                "each held member publishes a terminal");
        if (i == 0) {
            t.check(!observed.result.has_value() &&
                        observed.result.error().code == IoError::Code::canceled,
                    "the cancelled member reports cancellation");
        } else {
            t.check(observed.result.has_value() && observed.result.value() == 4096,
                    "the untouched members keep their own results");
        }
    }
    const auto outcome = ctx->shutdown(ShutdownPolicy::drain);
    t.check(outcome.has_value() && outcome.value() == ShutdownOutcome::completed,
            "the control path settles");
}

} // namespace

int main() {
    Tracker tracker{"uring_ready_batch_fault"};
    partial_submit_keeps_residue(tracker);
    poison_on_batch_flush_settles_every_member(tracker);
    control_intent_stops_the_batch(tracker);
    if (tracker.failures != 0) {
        std::fprintf(stderr, "%s: %d failure(s)\n", tracker.name, tracker.failures);
        return 1;
    }
    std::printf("%s: all research fault oracles passed\n", tracker.name);
    return 0;
}
