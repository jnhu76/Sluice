#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/test_access.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#ifndef SLUICE_HAS_LIBURING
#error "the ready-batch research oracle must receive SLUICE_HAS_LIBURING"
#endif

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::IoError;
using sluice::async::detail::TestAccess;

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

std::byte pattern_byte(std::size_t index) {
    return static_cast<std::byte>(static_cast<unsigned char>((index * 131u + 7u) & 0xffu));
}

struct Fixture {
    std::optional<File> file;
    std::vector<std::byte> shadow;
};

std::optional<Fixture> make_fixture(Tracker& t, std::size_t bytes) {
    char path[] = "/tmp/sluice_595_r3_batch_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0) {
        t.check(false, "temporary fixture file is created");
        return std::nullopt;
    }
    Fixture fixture;
    fixture.shadow.resize(bytes);
    for (std::size_t i = 0; i < bytes; ++i)
        fixture.shadow[i] = pattern_byte(i);
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

std::optional<AsyncIoContext> make_ctx(Tracker& t, std::size_t capacity) {
    UringConfig config;
    config.request_capacity = capacity;
    config.queue_depth = static_cast<unsigned>(capacity);
    auto made = make_uring_context(config);
    if (!made.has_value()) {
        t.check(false, "io_uring context is constructed");
        return std::nullopt;
    }
    return std::optional<AsyncIoContext>{std::move(made).value()};
}

UringAsyncBackend* backend_of(AsyncIoContext& ctx) {
    return dynamic_cast<UringAsyncBackend*>(TestAccess::context_backend(ctx));
}

bool settle(AsyncIoContext& ctx, std::vector<Request<std::size_t>>& requests,
            std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        bool all_ready = true;
        for (const Request<std::size_t>& request : requests) {
            if (!request.ready()) {
                all_ready = false;
                break;
            }
        }
        if (all_ready)
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        (void)ctx.wait_one(std::chrono::milliseconds(5));
    }
}

std::vector<Request<std::size_t>>
requests_for(AsyncIoContext& ctx, const UringAsyncBackend::ReadyReadBatchReport& report) {
    std::vector<Request<std::size_t>> requests;
    for (std::size_t i = 0; i < report.accepted; ++i)
        requests.push_back(TestAccess::make_request<std::size_t>(ctx, report.keys[i]));
    return requests;
}

std::vector<Request<std::size_t>>
requests_from(TestAccess::ReadyReadsBatch& batch) {
    std::vector<Request<std::size_t>> requests;
    for (std::size_t i = 0; i < batch.accepted; ++i)
        requests.push_back(std::move(batch.requests[i]));
    return requests;
}

void check_bytes(Tracker& t, const std::vector<std::byte>& buffer, const Fixture& fixture,
                 std::size_t offset, const char* label) {
    if (offset + buffer.size() > fixture.shadow.size()) {
        t.check(false, label);
        return;
    }
    t.check(std::memcmp(buffer.data(), fixture.shadow.data() + offset, buffer.size()) == 0, label);
}

void two_ready_reads_one_flush(Tracker& t) {
    auto fixture = make_fixture(t, 8192);
    auto ctx = make_ctx(t, 2);
    if (!fixture.has_value() || !ctx.has_value())
        return;
    UringAsyncBackend* backend = backend_of(*ctx);
    t.check(backend != nullptr, "the context exposes the uring backend");

    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(4096));
    ReadOp ops[2];
    for (std::size_t i = 0; i < 2; ++i) {
        ops[i].file = NativeFileRef(*fixture->file);
        ops[i].dst = buffers[i].data();
        ops[i].len = buffers[i].size();
        ops[i].offset = i * 4096;
    }
    auto batch = TestAccess::context_submit_ready_reads(*ctx, std::span<const ReadOp>(ops, 2));
    t.check(batch.batch_backend, "the shipped library exposes the uring batch backend");
    t.check(batch.accepted == 2, "both simultaneously ready reads are accepted");
    t.check(batch.untouched_begin == 2, "the untouched suffix starts after both accepted reads");
    t.check(batch.untouched == 0, "no untouched suffix when the group fits");
    t.check(batch.stop == UringAsyncBackend::ReadyBatchStop::input_exhausted,
            "the group stops at input exhaustion");
    t.check(batch.flush_attempts == 1, "the accepted prefix uses exactly one transport flush");
    t.check(!batch.residue_advertised, "a fully consumed prefix advertises no residue");

    auto requests = requests_from(batch);
    t.check(requests.size() == 2, "both accepted reads produce a request");
    t.check(settle(*ctx, requests, std::chrono::seconds(5)), "both reads reach a terminal");
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto observed = requests[i].take_result();
        t.check(observed.readiness == RequestReadiness::ready, "terminal is consumable");
        t.check(observed.result.has_value() && observed.result.value() == 4096,
                "each independent read reports its own byte count");
    }
    check_bytes(t, buffers[0], *fixture, 0, "read 0 lands its own bytes");
    check_bytes(t, buffers[1], *fixture, 4096, "read 1 lands its own bytes");
}

void cap_leaves_suffix_untouched(Tracker& t) {
    constexpr std::size_t kCount = 16;
    auto fixture = make_fixture(t, kCount * 4096);
    auto ctx = make_ctx(t, kCount);
    if (!fixture.has_value() || !ctx.has_value())
        return;
    UringAsyncBackend* backend = backend_of(*ctx);
    t.check(backend != nullptr, "the context exposes the uring backend");

    std::vector<std::vector<std::byte>> buffers(kCount, std::vector<std::byte>(4096));
    std::vector<ReadOp> ops;
    ops.reserve(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        ReadOp op;
        op.file = NativeFileRef(*fixture->file);
        op.dst = buffers[i].data();
        op.len = buffers[i].size();
        op.offset = i * 4096;
        ops.push_back(op);
    }

    const auto report = backend->submit_ready_reads(std::span<const ReadOp>(ops.data(), ops.size()));
    t.check(report.accepted == UringAsyncBackend::ready_batch_cap,
            "the accepted prefix stops at the research cap");
    t.check(report.stop == UringAsyncBackend::ReadyBatchStop::cap, "the stop reason is the cap");
    t.check(report.untouched_begin == report.accepted,
            "the untouched suffix starts at the accepted prefix boundary");
    t.check(report.untouched == kCount - report.accepted, "the cap stop leaves the rest untouched");

    std::vector<Request<std::size_t>> requests = requests_for(*ctx, report);
    std::vector<RequestId> ids;
    for (const Request<std::size_t>& request : requests)
        ids.push_back(request.id());

    for (std::size_t index = report.accepted; index < kCount; ++index) {
        const auto single =
            backend->submit_ready_reads(std::span<const ReadOp>(&ops[index], 1));
        t.check(single.accepted == 1, "an untouched suffix item takes the immediate path");
        t.check(single.untouched == 0, "a single-read call has no suffix");
        t.check(single.stop == UringAsyncBackend::ReadyBatchStop::input_exhausted,
                "a single-read call stops at input exhaustion");
        requests.push_back(TestAccess::make_request<std::size_t>(*ctx, single.keys[0]));
        ids.push_back(requests.back().id());
    }
    t.check(requests.size() == kCount, "every ready read is accepted exactly once");
    bool duplicate = false;
    for (std::size_t i = 0; i < ids.size(); ++i)
        for (std::size_t j = i + 1; j < ids.size(); ++j)
            duplicate = duplicate || ids[i] == ids[j];
    t.check(!duplicate, "no accepted request is returned again in the suffix");
    t.check(settle(*ctx, requests, std::chrono::seconds(10)), "all sixteen reads terminate");
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto observed = requests[i].take_result();
        t.check(observed.readiness == RequestReadiness::ready &&
                    observed.result.has_value() && observed.result.value() == 4096,
                "every request keeps its own terminal");
    }
    for (std::size_t i = 0; i < kCount; ++i)
        check_bytes(t, buffers[i], *fixture, i * 4096, "order-preserving bytes at every offset");
}

void single_read_keeps_immediate_path(Tracker& t) {
    auto fixture = make_fixture(t, 4096);
    auto ctx = make_ctx(t, 1);
    if (!fixture.has_value() || !ctx.has_value())
        return;
    UringAsyncBackend* backend = backend_of(*ctx);

    std::vector<std::byte> buffer(4096);
    ReadOp op;
    op.file = NativeFileRef(*fixture->file);
    op.dst = buffer.data();
    op.len = buffer.size();
    op.offset = 0;
    const auto report = backend->submit_ready_reads(std::span<const ReadOp>(&op, 1));
    t.check(report.accepted == 1, "a single read is accepted through the immediate path");
    t.check(report.stop == UringAsyncBackend::ReadyBatchStop::input_exhausted,
            "a single read stops at input exhaustion");
    t.check(report.flush_attempts == 1, "the immediate path flushes once");
    auto requests = requests_for(*ctx, report);
    t.check(settle(*ctx, requests, std::chrono::seconds(5)), "the single read terminates");
    const auto observed = requests.front().take_result();
    t.check(observed.result.has_value() && observed.result.value() == 4096,
            "the single read transfers its bytes");
    check_bytes(t, buffer, *fixture, 0, "the single read lands its own bytes");
}

void ineligible_head_keeps_immediate_path(Tracker& t) {
    auto fixture = make_fixture(t, 16384);
    auto ctx = make_ctx(t, 2);
    if (!fixture.has_value() || !ctx.has_value())
        return;
    UringAsyncBackend* backend = backend_of(*ctx);

    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(8192));
    ReadOp ops[2];
    for (std::size_t i = 0; i < 2; ++i) {
        ops[i].file = NativeFileRef(*fixture->file);
        ops[i].dst = buffers[i].data();
        ops[i].len = buffers[i].size();
        ops[i].offset = i * 8192;
    }
    const auto report = backend->submit_ready_reads(std::span<const ReadOp>(ops, 2));
    t.check(report.accepted == 1, "a non-4KiB head takes the immediate path on its own");
    t.check(report.stop == UringAsyncBackend::ReadyBatchStop::ineligible,
            "the ineligible head is reported, not silently batched");
    t.check(report.untouched_begin == 1, "the untouched suffix starts after the immediate head");
    t.check(report.untouched == 1, "the rest of an ineligible group stays untouched");
    auto requests = requests_for(*ctx, report);

    const auto second = backend->submit_ready_reads(std::span<const ReadOp>(&ops[1], 1));
    t.check(second.accepted == 1, "the untouched ineligible item takes the immediate path");
    requests.push_back(TestAccess::make_request<std::size_t>(*ctx, second.keys[0]));
    t.check(settle(*ctx, requests, std::chrono::seconds(5)), "both 8KiB reads terminate");
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto observed = requests[i].take_result();
        t.check(observed.result.has_value() && observed.result.value() == 8192,
                "the ineligible read keeps its byte count");
        check_bytes(t, buffers[i], *fixture, i * 8192, "the ineligible read keeps its bytes");
    }
}

void cancel_terminates_one_member(Tracker& t) {
    auto ctx = make_ctx(t, 2);
    if (!ctx.has_value())
        return;
    UringAsyncBackend* backend = backend_of(*ctx);
    int fds[2];
    if (::pipe(fds) != 0) {
        t.check(false, "pipe fixture is created");
        return;
    }
    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(4096));
    ReadOp ops[2];
    for (std::size_t i = 0; i < 2; ++i) {
        ops[i].file = NativeFileRef(fds[0], sluice::FileAccess::read_only);
        ops[i].dst = buffers[i].data();
        ops[i].len = buffers[i].size();
        ops[i].offset = 0;
    }
    const auto report = backend->submit_ready_reads(std::span<const ReadOp>(ops, 2));
    t.check(report.accepted == 2, "both pipe reads are accepted as independent members");
    auto requests = requests_for(*ctx, report);
    const auto disposition = requests[1].cancel();
    t.check(disposition.has_value(), "cancel of a batched member returns a disposition");

    std::vector<std::byte> payload(4096, std::byte{0x5a});
    const ssize_t wrote = ::write(fds[1], payload.data(), payload.size());
    t.check(wrote == static_cast<ssize_t>(payload.size()), "the pipe releases the blocked reads");
    ::close(fds[1]);

    t.check(settle(*ctx, requests, std::chrono::seconds(5)),
            "both batched members reach a terminal after cancel");
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto observed = requests[i].take_result();
        t.check(observed.readiness == RequestReadiness::ready,
                "the canceled member still publishes a terminal");
        if (observed.result.has_value())
            bytes += observed.result.value();
    }
    t.check(bytes == 4096, "cancel and completion do not double-consume the pipe payload");
    ::close(fds[0]);
}

void closed_admission_stops_before_accept(Tracker& t) {
    auto fixture = make_fixture(t, 8192);
    auto ctx = make_ctx(t, 2);
    if (!fixture.has_value() || !ctx.has_value())
        return;
    UringAsyncBackend* backend = backend_of(*ctx);
    ctx->close_admission();

    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(4096));
    ReadOp ops[2];
    for (std::size_t i = 0; i < 2; ++i) {
        ops[i].file = NativeFileRef(*fixture->file);
        ops[i].dst = buffers[i].data();
        ops[i].len = buffers[i].size();
        ops[i].offset = i * 4096;
    }
    const auto report = backend->submit_ready_reads(std::span<const ReadOp>(ops, 2));
    t.check(report.accepted == 0, "closed admission accepts no batched read");
    t.check(report.stop == UringAsyncBackend::ReadyBatchStop::rejected,
            "closed admission stops at the authoritative rejection");
    t.check(report.error.code == IoError::Code::invalid_state,
            "closed admission keeps the existing invalid_state mapping");
    t.check(report.untouched_begin == 1, "the rejected item is not reported as untouched");
    t.check(report.untouched == 1, "the unaccepted item is reported untouched, not accepted");
}

void capacity_exhaustion_stops_mid_group(Tracker& t) {
    auto fixture = make_fixture(t, 3 * 4096);
    auto ctx = make_ctx(t, 2);
    if (!fixture.has_value() || !ctx.has_value())
        return;
    UringAsyncBackend* backend = backend_of(*ctx);

    std::vector<std::vector<std::byte>> held(1, std::vector<std::byte>(4096));
    ReadOp first;
    first.file = NativeFileRef(*fixture->file);
    first.dst = held[0].data();
    first.len = held[0].size();
    first.offset = 0;
    const auto held_report = backend->submit_ready_reads(std::span<const ReadOp>(&first, 1));
    t.check(held_report.accepted == 1, "the first capacity slot is occupied");

    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(4096));
    ReadOp ops[2];
    for (std::size_t i = 0; i < 2; ++i) {
        ops[i].file = NativeFileRef(*fixture->file);
        ops[i].dst = buffers[i].data();
        ops[i].len = buffers[i].size();
        ops[i].offset = (i + 1) * 4096;
    }
    const auto report = backend->submit_ready_reads(std::span<const ReadOp>(ops, 2));
    t.check(report.accepted == 1, "the batch accepts only the free capacity slot");
    t.check(report.stop == UringAsyncBackend::ReadyBatchStop::rejected,
            "capacity exhaustion stops at the authoritative reserve rejection");
    t.check(report.error.code == IoError::Code::would_block,
            "capacity exhaustion keeps the existing would_block mapping");
    t.check(report.untouched_begin == 2,
            "the rejected item sits after the accepted prefix, not in the suffix");
    t.check(report.untouched == 0, "a rejected tail item is not reported as untouched");

    std::vector<Request<std::size_t>> requests = requests_for(*ctx, held_report);
    for (std::size_t i = 0; i < report.accepted; ++i)
        requests.push_back(TestAccess::make_request<std::size_t>(*ctx, report.keys[i]));
    t.check(settle(*ctx, requests, std::chrono::seconds(5)),
            "the held and accepted reads terminate");
    for (Request<std::size_t>& request : requests) {
        const auto observed = request.take_result();
        t.check(observed.readiness == RequestReadiness::ready &&
                    observed.result.has_value() && observed.result.value() == 4096,
                "each accepted read keeps its own terminal");
    }
    check_bytes(t, held[0], *fixture, 0, "the held read keeps its bytes");
    check_bytes(t, buffers[0], *fixture, 4096, "the accepted batch read keeps its bytes");
}

void post_shutdown_entry_returns_recovery(Tracker& t) {
    auto fixture = make_fixture(t, 8192);
    auto ctx = make_ctx(t, 2);
    if (!fixture.has_value() || !ctx.has_value())
        return;
    UringAsyncBackend* backend = backend_of(*ctx);

    ctx->close_admission();
    const auto outcome = ctx->shutdown(ShutdownPolicy::drain);
    t.check(outcome.has_value() && outcome.value() == ShutdownOutcome::completed,
            "the fresh context settles cleanly");

    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(4096));
    ReadOp ops[2];
    for (std::size_t i = 0; i < 2; ++i) {
        ops[i].file = NativeFileRef(*fixture->file);
        ops[i].dst = buffers[i].data();
        ops[i].len = buffers[i].size();
        ops[i].offset = i * 4096;
    }
    const auto report = backend->submit_ready_reads(std::span<const ReadOp>(ops, 2));
    t.check(report.accepted == 0, "a retired ring accepts nothing");
    t.check(report.stop == UringAsyncBackend::ReadyBatchStop::recovery,
            "a retired ring is reported as the recovery boundary");
    t.check(report.error.code == IoError::Code::backend_error,
            "a retired ring reports the existing backend_error");
    t.check(report.untouched == 2, "both writes to a retired ring stay untouched");
}

} // namespace

int main() {
    Tracker tracker{"uring_ready_batch_research"};
    two_ready_reads_one_flush(tracker);
    cap_leaves_suffix_untouched(tracker);
    single_read_keeps_immediate_path(tracker);
    ineligible_head_keeps_immediate_path(tracker);
    cancel_terminates_one_member(tracker);
    closed_admission_stops_before_accept(tracker);
    capacity_exhaustion_stops_mid_group(tracker);
    post_shutdown_entry_returns_recovery(tracker);
    if (tracker.failures != 0) {
        std::fprintf(stderr, "%s: %d failure(s)\n", tracker.name, tracker.failures);
        return 1;
    }
    std::printf("%s: all research oracles passed\n", tracker.name);
    return 0;
}
