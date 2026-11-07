// #595 R3 ready-batch deterministic oracles. Each scenario names the
// invariant or failure mode it distinguishes. Timing/fault control uses the
// existing pipe hold, scripted-kernel submit hook and ProgressSource pause
// gates; there are no sleeps used as concurrency evidence.

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/test_access.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include "../bench/r3_ready_batch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

#ifndef SLUICE_HAS_LIBURING
#error "the ready-batch deterministic oracle must receive SLUICE_HAS_LIBURING"
#endif
#ifndef SLUICE_ASYNC_INTERNAL_TESTING
#error "the ready-batch deterministic oracle must receive SLUICE_ASYNC_INTERNAL_TESTING"
#endif

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::IoError;
using sluice::async::detail::TestAccess;

constexpr std::size_t kPage = 4096;
constexpr std::uint64_t kControlTag = std::uint64_t{1} << 63u;
constexpr std::chrono::milliseconds kDriveBudget{5000};

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
    char path[] = "/tmp/sluice_595_r3_det_XXXXXX";
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

struct EnterCall {
    unsigned ready = 0;
    std::array<unsigned, 8> opcodes{};
};

struct KernelOracle {
    std::array<EnterCall, 512> calls{};
    unsigned count = 0;
    unsigned total_sqes = 0;
    unsigned max_sqes = 0;
    unsigned get_events_calls = 0;
};

int oracle_submit(void* context, ::io_uring* ring) noexcept {
    auto* oracle = static_cast<KernelOracle*>(context);
    if (oracle->count < oracle->calls.size()) {
        EnterCall& call = oracle->calls[oracle->count++];
        call.ready = ::io_uring_sq_ready(ring);
        const unsigned head = *ring->sq.khead;
        const unsigned mask = *ring->sq.kring_mask;
        const unsigned n = std::min(call.ready, 8u);
        for (unsigned i = 0; i < n; ++i) {
            const unsigned index = ring->sq.array != nullptr
                                       ? ring->sq.array[(head + i) & mask]
                                       : (ring->sq.sqe_tail - call.ready + i) & mask;
            call.opcodes[i] = ring->sq.sqes[index].opcode;
        }
        oracle->total_sqes += call.ready;
        oracle->max_sqes = std::max(oracle->max_sqes, call.ready);
    }
    return ::io_uring_submit(ring);
}

int oracle_get_events(void* context, ::io_uring* ring) noexcept {
    auto* oracle = static_cast<KernelOracle*>(context);
    ++oracle->get_events_calls;
    return ::io_uring_get_events(ring);
}

struct ScriptedKernel {
    enum class Mode : int { consume_all, partial_first, hold, partial_then_poison };

    std::atomic<int> mode{static_cast<int>(Mode::consume_all)};
    std::atomic<int> calls{0};
    std::atomic<unsigned> first_ready{0};
    std::atomic<unsigned> second_ready{0};
    std::atomic<int> first_rc{0};
    std::atomic<int> second_rc{0};

    Mode load() const noexcept { return static_cast<Mode>(mode.load(std::memory_order_acquire)); }
    void store(Mode value) noexcept { mode.store(static_cast<int>(value), std::memory_order_release); }
};

int scripted_submit(void* context, ::io_uring* ring) noexcept {
    auto* kernel = static_cast<ScriptedKernel*>(context);
    const unsigned ready = ::io_uring_sq_ready(ring);
    *ring->sq.ktail = ring->sq.sqe_tail;
    const int call = kernel->calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call == 1) {
        kernel->first_ready.store(ready, std::memory_order_relaxed);
    } else if (call == 2) {
        kernel->second_ready.store(ready, std::memory_order_relaxed);
    }
    int rc = 0;
    switch (kernel->load()) {
    case ScriptedKernel::Mode::consume_all:
        *ring->sq.khead += ready;
        rc = static_cast<int>(ready);
        break;
    case ScriptedKernel::Mode::partial_first:
        if (call == 1 && ready > 0) {
            *ring->sq.khead += 1;
            kernel->store(ScriptedKernel::Mode::consume_all);
            rc = 1;
        } else {
            *ring->sq.khead += ready;
            rc = static_cast<int>(ready);
        }
        break;
    case ScriptedKernel::Mode::hold:
        rc = 0;
        break;
    case ScriptedKernel::Mode::partial_then_poison:
        if (call == 1 && ready > 0) {
            *ring->sq.khead += 1;
            kernel->store(ScriptedKernel::Mode::consume_all);
            rc = -EIO;
        } else {
            *ring->sq.khead += ready;
            rc = static_cast<int>(ready);
        }
        break;
    }
    if (call == 1) {
        kernel->first_rc.store(rc, std::memory_order_relaxed);
    } else if (call == 2) {
        kernel->second_rc.store(rc, std::memory_order_relaxed);
    }
    return rc;
}

struct Harness {
    std::optional<AsyncIoContext> ctx;
    UringAsyncBackend* backend = nullptr;
    KernelOracle* oracle = nullptr;
    ScriptedKernel* kernel = nullptr;
};

std::optional<Harness> make_real_harness(Tracker& t, std::size_t capacity, unsigned depth,
                                         KernelOracle* oracle) {
    std::unique_ptr<UringAsyncBackend> owned;
    if (oracle != nullptr) {
        UringBackendSubmitTestHooks hooks;
        hooks.context = oracle;
        hooks.submit = oracle_submit;
        hooks.get_events = oracle_get_events;
        owned = std::make_unique<UringAsyncBackend>(
            UringConfig{capacity, depth}, hooks);
    } else {
        owned = std::make_unique<UringAsyncBackend>(UringConfig{capacity, depth});
    }
    UringAsyncBackend* backend = owned.get();
    Harness harness;
    harness.ctx = sluice::async::detail::make_context(std::move(owned));
    harness.backend = backend;
    harness.oracle = oracle;
    t.check(harness.backend != nullptr, "the harness exposes the uring backend");
    return harness;
}

std::optional<Harness> make_scripted_harness(Tracker& t, std::size_t capacity, unsigned depth,
                                             ScriptedKernel& kernel) {
    UringBackendSubmitTestHooks hooks;
    hooks.context = &kernel;
    hooks.submit = scripted_submit;
    auto owned = std::make_unique<UringAsyncBackend>(UringConfig{capacity, depth}, hooks);
    UringAsyncBackend* backend = owned.get();
    Harness harness;
    harness.ctx = sluice::async::detail::make_context(std::move(owned));
    harness.backend = backend;
    harness.kernel = &kernel;
    t.check(harness.backend != nullptr, "the harness exposes the uring backend");
    return harness;
}

std::vector<ReadOp> file_ops(const Fixture& fixture, std::vector<std::vector<std::byte>>& buffers,
                             const std::vector<std::uint64_t>& offsets) {
    std::vector<ReadOp> ops;
    ops.reserve(offsets.size());
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        ReadOp op;
        op.file = NativeFileRef(*fixture.file);
        op.dst = buffers[i].data();
        op.len = buffers[i].size();
        op.offset = offsets[i];
        ops.push_back(op);
    }
    return ops;
}

std::vector<ReadOp> pipe_ops(int fd, std::vector<std::vector<std::byte>>& buffers,
                             std::size_t count, std::uint64_t offset = 0) {
    std::vector<ReadOp> ops;
    ops.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        ReadOp op;
        op.file = NativeFileRef(fd, sluice::FileAccess::read_only);
        op.dst = buffers[i].data();
        op.len = buffers[i].size();
        op.offset = offset;
        ops.push_back(op);
    }
    return ops;
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

UringAsyncBackend* backend_of(AsyncIoContext& ctx) {
    return dynamic_cast<UringAsyncBackend*>(TestAccess::context_backend(ctx));
}

bool drive_request(AsyncIoContext& ctx, Request<std::size_t>& request,
                   std::chrono::milliseconds budget = kDriveBudget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        (void)ctx.poll();
        if (request.ready())
            return true;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return request.ready();
        const auto remaining = deadline - now;
        const auto slice =
            std::min<std::chrono::nanoseconds>(remaining, std::chrono::milliseconds(20));
        if (!ctx.wait_one(slice).has_value())
            return request.ready();
    }
}

bool drive_any_ready(AsyncIoContext& ctx,
                     const std::vector<Request<std::size_t>>& requests,
                     std::chrono::milliseconds budget = kDriveBudget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        (void)ctx.poll();
        for (const Request<std::size_t>& request : requests) {
            if (request.ready())
                return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        const auto remaining = deadline - now;
        const auto slice =
            std::min<std::chrono::nanoseconds>(remaining, std::chrono::milliseconds(20));
        if (!ctx.wait_one(slice).has_value())
            return false;
    }
}

bool drive_context(AsyncIoContext& ctx, const std::vector<Request<std::size_t>>& requests,
                   std::chrono::milliseconds budget = kDriveBudget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        (void)ctx.poll();
        if (all_ready(requests))
            return true;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return all_ready(requests);
        const auto remaining = deadline - now;
        const auto slice =
            std::min<std::chrono::nanoseconds>(remaining, std::chrono::milliseconds(20));
        if (!ctx.wait_one(slice).has_value())
            return all_ready(requests);
    }
}

bool drive_backend(UringAsyncBackend& backend,
                   const std::vector<Request<std::size_t>>& requests,
                   std::chrono::milliseconds budget = kDriveBudget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        (void)backend.poll();
        if (all_ready(requests))
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return all_ready(requests);
        std::this_thread::yield();
    }
}

void check_bytes(Tracker& t, const std::vector<std::byte>& buffer, const Fixture& fixture,
                 std::size_t offset, const char* label) {
    if (offset + buffer.size() > fixture.shadow.size()) {
        t.check(false, label);
        return;
    }
    t.check(std::memcmp(buffer.data(), fixture.shadow.data() + offset, buffer.size()) == 0, label);
}

void check_result(Tracker& t, Request<std::size_t>& request, std::size_t bytes, const char* label) {
    const auto observed = request.take_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == bytes,
            label);
}

bool same_token(const detail::ProgressSource::Token& a,
                const detail::ProgressSource::Token& b) noexcept {
    return a.progress == b.progress && a.progress_exhaustion == b.progress_exhaustion;
}

void drain_notification(AsyncIoContext& ctx) {
    detail::ProgressSource* source = TestAccess::context_progress_source(ctx);
    if (source == nullptr)
        return;
    for (int i = 0; i < 4; ++i) {
        source->acknowledge_notification();
        std::uint64_t value = 0;
        const ssize_t n = ::read(source->notification_fd(), &value, sizeof(value));
        if (n < 0 && errno == EAGAIN)
            return;
    }
}

bool notification_readable(AsyncIoContext& ctx) {
    detail::ProgressSource* source = TestAccess::context_progress_source(ctx);
    if (source == nullptr)
        return false;
    std::uint64_t value = 0;
    const ssize_t n = ::read(source->notification_fd(), &value, sizeof(value));
    return n == static_cast<ssize_t>(sizeof(value));
}

bool submit_all_batched(Tracker& t, AsyncIoContext& ctx, UringAsyncBackend& backend,
                        const std::vector<ReadOp>& ops,
                        std::vector<Request<std::size_t>>& requests, const char* label) {
    std::size_t next = 0;
    while (next < ops.size()) {
        const std::size_t room = std::min<std::size_t>(UringAsyncBackend::ready_batch_cap,
                                                       ops.size() - next);
        const auto span = std::span<const ReadOp>(ops.data() + next, room);
        const auto report = backend.submit_ready_reads(span);
        auto accepted = requests_for(ctx, report);
        for (Request<std::size_t>& request : accepted)
            requests.push_back(std::move(request));
        if (report.accepted == 0) {
            t.check(false, label);
            return false;
        }
        next += report.accepted;
        if (report.untouched > 0) {
            for (std::size_t i = report.untouched_begin; i < span.size(); ++i) {
                const auto single = backend.submit_ready_reads(
                    std::span<const ReadOp>(span.data() + i, 1));
                if (single.accepted != 1) {
                    t.check(false, label);
                    return false;
                }
                requests.push_back(
                    TestAccess::make_request<std::size_t>(ctx, single.keys[0]));
                ++next;
            }
        }
    }
    return true;
}

// D2 positive at the W-03 refill burst. The consumer state fixes two
// independent 4 KiB reads before the first submit; A0 and A1 each cost two
// pure submit enters, B costs one enter carrying two data SQEs, and all three
// arms yield two independent 4 KiB results.
void d2_positive_two_ready_reads_one_enter(Tracker& t) {
    constexpr std::size_t kDepth = 2;
    constexpr std::size_t kRequests = 2;

    for (int arm_index = 0; arm_index < 3; ++arm_index) {
        const auto arm = static_cast<bench::r3::Arm>(arm_index);
        KernelOracle oracle;
        auto fixture = make_fixture(t, kRequests * kPage);
        auto harness = make_real_harness(t, 4, static_cast<unsigned>(kDepth), &oracle);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        UringAsyncBackend& backend = *harness->backend;

        const std::size_t window = 0;
        const std::size_t submitted = 0;
        const std::size_t blocks = kRequests;
        const std::size_t group =
            std::min(kDepth - window, kRequests - submitted);
        t.check(group == 2, "the production point predicts a two-member ready group");
        std::vector<std::vector<std::byte>> buffers(kRequests, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> offsets;
        for (std::size_t j = 0; j < group; ++j)
            offsets.push_back(((submitted + j) % blocks) * kPage);
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        for (std::size_t j = 0; j < group; ++j) {
            t.check(ops[j].len == kPage && ops[j].dst == buffers[j].data() &&
                        ops[j].offset == j * kPage,
                    "the predicted member identity matches the ready group");
        }

        bench::r3::Adapter adapter(ctx, arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops.data(), group), requests);
        t.check(!outcome.error.has_value(), "the ready group is accepted without an initiation error");
        t.check(requests.size() == 2, "both ready members produce a request");
        t.check(requests[0].id() != requests[1].id(), "the two members keep distinct request ids");

        const unsigned expected_calls = arm == bench::r3::Arm::b ? 1u : 2u;
        t.check(oracle.count == expected_calls, "the arm costs its documented number of submit enters");
        t.check(oracle.total_sqes == 2, "exactly two data SQEs reach the kernel");
        if (arm == bench::r3::Arm::b) {
            t.check(oracle.count == 1 && oracle.calls[0].ready == 2,
                    "B submits both SQEs in one enter");
            t.check(oracle.calls[0].opcodes[0] == IORING_OP_READ &&
                        oracle.calls[0].opcodes[1] == IORING_OP_READ,
                    "B's enter carries two read SQEs");
            t.check(outcome.counters.group_calls == 1 && outcome.counters.prefix_max == 2 &&
                        outcome.counters.flush_attempts == 1,
                    "B's adapter accounts one group and one flush");
        } else {
            for (unsigned i = 0; i < oracle.count; ++i) {
                t.check(oracle.calls[i].ready == 1 && oracle.calls[i].opcodes[0] == IORING_OP_READ,
                        "A0/A1 submit one read per enter");
            }
            if (arm == bench::r3::Arm::a1) {
                t.check(outcome.counters.entry_calls == 2 && outcome.counters.single_calls == 2 &&
                            outcome.counters.accepted == 2 &&
                            outcome.counters.flush_attempts == 2,
                        "A1's adapter accounts two single submits");
            }
        }

        t.check(drive_context(ctx, requests), "both members terminate");
        check_result(t, requests[0], kPage, "member 0 reports its own 4 KiB result");
        check_result(t, requests[1], kPage, "member 1 reports its own 4 KiB result");
        check_bytes(t, buffers[0], *fixture, 0, "member 0 lands its own bytes");
        check_bytes(t, buffers[1], *fixture, kPage, "member 1 lands its own bytes");
        t.check(backend.transport_ledger_size_for_test() == 0 &&
                    backend.dispatch_size_for_test() == 0,
                "the fully consumed group leaves no dispatch or ledger residue");
    }

    KernelOracle oracle;
    auto fixture = make_fixture(t, kRequests * kPage);
    auto harness = make_real_harness(t, 4, static_cast<unsigned>(kDepth), &oracle);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;
    std::vector<std::vector<std::byte>> buffers(kRequests, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> offsets{0, kPage};
    std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
    const auto report = backend.submit_ready_reads(std::span<const ReadOp>(ops.data(), ops.size()));
    t.check(report.accepted == 2, "B's report accepts the whole ready group");
    t.check(report.untouched_begin == 2 && report.untouched == 0,
            "B's report returns no unaccepted suffix");
    t.check(report.stop == UringAsyncBackend::ReadyBatchStop::input_exhausted,
            "B's report stops at input exhaustion");
    t.check(report.flush_attempts == 1, "B's report attempts exactly one transport flush");
    t.check(!report.residue_advertised, "a fully consumed group advertises no residue");
    t.check(oracle.count == 1 && oracle.calls[0].ready == 2,
            "the batch entry issues one enter with both SQEs");
    auto requests = requests_for(ctx, report);
    t.check(drive_context(ctx, requests), "the reported members terminate");
    check_result(t, requests[0], kPage, "reported member 0 keeps its result");
    check_result(t, requests[1], kPage, "reported member 1 keeps its result");
}

// D2 negative: the W-02 depth-2 loop refills one read while the other is still
// in flight. B takes the immediate path, emits one SQE in one enter, and
// leaves no defer state, residue advertisement or progress notification.
void d2_negative_single_refill_while_in_flight(Tracker& t) {
    constexpr std::size_t kDepth = 2;
    KernelOracle oracle;
    auto fixture = make_fixture(t, 3 * kPage);
    auto harness = make_real_harness(t, kDepth, static_cast<unsigned>(kDepth), &oracle);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    int fds[2];
    if (::pipe(fds) != 0) {
        t.check(false, "pipe fixture is created");
        return;
    }
    std::vector<std::vector<std::byte>> pipe_buffers(2, std::vector<std::byte>(kPage));
    std::vector<ReadOp> initial = pipe_ops(fds[0], pipe_buffers, 2);
    std::vector<Request<std::size_t>> initial_requests;
    for (const ReadOp& op : initial) {
        auto submitted = ctx.submit_read(op);
        t.check(submitted.has_value(), "the depth-2 fill reads are accepted");
        if (submitted.has_value())
            initial_requests.push_back(std::move(submitted).value());
    }
    const std::string first_payload(kPage, 'a');
    t.check(::write(fds[1], first_payload.data(), first_payload.size()) ==
                static_cast<ssize_t>(first_payload.size()),
            "the pipe releases exactly one fill read");
    t.check(drive_any_ready(ctx, initial_requests), "the released fill read terminates");
    t.check(initial_requests[0].ready() != initial_requests[1].ready(),
            "exactly one fill read completed; the other I/O is still in flight");
    Request<std::size_t> still_in_flight;
    if (initial_requests[0].ready()) {
        check_result(t, initial_requests[0], kPage, "the completed fill read keeps its result");
        still_in_flight = std::move(initial_requests[1]);
    } else if (initial_requests[1].ready()) {
        check_result(t, initial_requests[1], kPage, "the completed fill read keeps its result");
        still_in_flight = std::move(initial_requests[0]);
    } else {
        t.check(false, "a completed fill read is identified");
    }

    drain_notification(ctx);
    const auto token_before = TestAccess::context_progress_token(ctx);
    const std::uint64_t flushes_before = backend.submit_flushes_for_test();

    std::vector<std::vector<std::byte>> refill_buffers(1, std::vector<std::byte>(kPage));
    std::vector<ReadOp> refill = pipe_ops(fds[0], refill_buffers, 1);
    const auto report = backend.submit_ready_reads(std::span<const ReadOp>(refill.data(), 1));
    t.check(report.accepted == 1, "the single ready refill is accepted");
    t.check(report.stop == UringAsyncBackend::ReadyBatchStop::input_exhausted,
            "the single ready refill stops at input exhaustion");
    t.check(report.untouched_begin == 1 && report.untouched == 0,
            "the single ready refill has no unaccepted suffix");
    t.check(report.flush_attempts == 1, "the single ready refill attempts exactly one flush");
    t.check(!report.residue_advertised, "the single ready refill leaves no advertised residue");
    t.check(backend.dispatch_size_for_test() == 0 &&
                backend.transport_ledger_size_for_test() == 0,
            "the immediate refill holds no cross-call SQE or dispatch entry");
    t.check(backend.submit_flushes_for_test() == flushes_before + 1,
            "the immediate refill costs exactly one transport flush");
    t.check(oracle.count == 3 && oracle.calls[2].ready == 1 &&
                oracle.calls[2].opcodes[0] == IORING_OP_READ,
            "the refill emits one read SQE in one enter");
    t.check(same_token(token_before, TestAccess::context_progress_token(ctx)),
            "the immediate refill does not signal the progress epoch");
    t.check(!notification_readable(ctx), "the immediate refill writes no progress notification");
    t.check(backend.live_cookies_for_test() == 2 && backend.outstanding() == 2,
            "only the two blocked reads remain in flight");

    auto requests = requests_for(ctx, report);
    t.check(requests.size() == 1, "the refill produces one request");
    const std::string second_payload(2 * kPage, 'b');
    t.check(::write(fds[1], second_payload.data(), second_payload.size()) ==
                static_cast<ssize_t>(second_payload.size()),
            "the pipe releases the remaining two reads");
    std::vector<Request<std::size_t>> remaining;
    remaining.push_back(std::move(still_in_flight));
    remaining.push_back(std::move(requests[0]));
    t.check(drive_context(ctx, remaining), "the remaining reads terminate");
    check_result(t, remaining[0], kPage, "the old in-flight read keeps its result");
    check_result(t, remaining[1], kPage, "the refilled read keeps its result");
    ::close(fds[0]);
    ::close(fds[1]);
}

// D1: the second read depends on the first result and therefore is not in the
// ready group; it must not be admitted, routed or dispatched before the first
// terminal is consumed.
void d1_dependent_second_never_enters_ready_group(Tracker& t) {
    constexpr std::size_t kDepth = 2;
    auto fixture = make_fixture(t, 2 * kPage);
    auto harness = make_real_harness(t, kDepth, static_cast<unsigned>(kDepth), nullptr);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    int fds[2];
    if (::pipe(fds) != 0) {
        t.check(false, "pipe fixture is created");
        return;
    }
    std::vector<std::vector<std::byte>> first_buffer(1, std::vector<std::byte>(kPage));
    std::vector<ReadOp> first_ops = pipe_ops(fds[0], first_buffer, 1);
    const auto first = backend.submit_ready_reads(
        std::span<const ReadOp>(first_ops.data(), first_ops.size()));
    t.check(first.accepted == 1 && first.untouched == 0,
            "only the independent first read enters the batch");
    t.check(first.flush_attempts == 1, "the first read is flushed immediately");
    t.check(backend.outstanding() == 1, "only one accepted request exists before the dependency resolves");
    t.check(!backend.live_cookie_for_offset_for_test(kPage).has_value(),
            "no route exists for the dependent second read");

    const std::vector<std::byte> second_sentinel(kPage, std::byte{0x3c});
    std::vector<std::vector<std::byte>> second_buffer(1, second_sentinel);
    std::optional<AsyncIoContext::ProgressWaitOutcome> wait_outcome;
    std::thread waiter([&ctx, &wait_outcome] {
        const auto outcome = ctx.wait_one(std::chrono::seconds(2));
        if (outcome.has_value())
            wait_outcome = outcome.value();
    });
    const std::string payload(kPage, 'd');
    t.check(::write(fds[1], payload.data(), payload.size()) ==
                static_cast<ssize_t>(payload.size()),
            "the first read's data is released while the waiter is parked");
    waiter.join();
    t.check(wait_outcome.has_value() &&
                wait_outcome->kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress &&
                wait_outcome->completed >= 1,
            "waiting for the first read reports progress, not a deadline");

    auto first_request = TestAccess::make_request<std::size_t>(ctx, first.keys[0]);
    t.check(first_request.ready(), "the first read reaches its terminal");
    const auto observed = first_request.take_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == kPage,
            "the first read's result is consumable");
    t.check(std::memcmp(second_buffer[0].data(), second_sentinel.data(), kPage) == 0,
            "the dependent buffer was not touched before its own submit");

    const std::uint64_t dependent_offset = observed.result.value() == kPage ? kPage : 0;
    t.check(dependent_offset == kPage, "the second read's offset is derived from the first result");
    std::vector<std::vector<std::byte>> dependent_buffers(1, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> dependent_offsets{dependent_offset};
    std::vector<ReadOp> dependent_ops = file_ops(*fixture, dependent_buffers, dependent_offsets);
    t.check(!backend.live_cookie_for_offset_for_test(dependent_offset).has_value(),
            "the pending dependency occupies no route before it is submitted");
    const auto second = backend.submit_ready_reads(
        std::span<const ReadOp>(dependent_ops.data(), dependent_ops.size()));
    t.check(second.accepted == 1 && second.untouched == 0,
            "the dependency-resolved read is admitted only after the first terminal");
    auto second_request = TestAccess::make_request<std::size_t>(ctx, second.keys[0]);
    const detail::RequestKey first_key = first.keys[0];
    const detail::RequestKey second_key = second.keys[0];
    t.check(!(first_key.slot.value == second_key.slot.value &&
              first_key.generation.value == second_key.generation.value),
            "the dependent read never reuses the consumed request identity");
    if (first_key.slot.value == second_key.slot.value) {
        t.check(second_key.generation.value > first_key.generation.value,
                "a reused slot advances its generation");
    }
    t.check(TestAccess::context_core(ctx)->lookup(first_key) == detail::PublicLookup::not_found,
            "the consumed key is stale after slot reuse");
    std::vector<Request<std::size_t>> requests;
    requests.push_back(std::move(second_request));
    t.check(drive_backend(backend, requests), "the dependency-resolved read terminates");
    check_result(t, requests[0], kPage, "the dependency-resolved read keeps its result");
    check_bytes(t, dependent_buffers[0], *fixture, dependent_offset,
                "the dependency-resolved read lands the derived range");
    ::close(fds[0]);
    ::close(fds[1]);
}

// Single, real multi-ready group, group larger than the cap, and group larger
// than the ring budget. Every member is accounted individually; the
// unaccepted suffix is returned and only the suffix takes the single path.
void d3_d16_budget_prefix_and_suffix_accounting(Tracker& t) {
    {
        KernelOracle oracle;
        auto fixture = make_fixture(t, kPage);
        auto harness = make_real_harness(t, 1, 1, &oracle);
        if (fixture.has_value() && harness.has_value()) {
            AsyncIoContext& ctx = *harness->ctx;
            UringAsyncBackend& backend = *harness->backend;
            std::vector<std::vector<std::byte>> buffers(1, std::vector<std::byte>(kPage));
            std::vector<std::uint64_t> offsets{0};
            std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
            const auto report = backend.submit_ready_reads(std::span<const ReadOp>(ops.data(), 1));
            t.check(report.accepted == 1 && report.untouched_begin == 1 && report.untouched == 0,
                    "a single ready read is accepted with no suffix");
            t.check(report.stop == UringAsyncBackend::ReadyBatchStop::input_exhausted,
                    "a single ready read stops at input exhaustion");
            t.check(report.flush_attempts == 1, "a single ready read flushes once");
            t.check(oracle.count == 1 && oracle.calls[0].ready == 1,
                    "a single ready read takes the immediate path with one SQE");
            auto requests = requests_for(ctx, report);
            t.check(drive_context(ctx, requests), "the single read terminates");
            check_result(t, requests[0], kPage, "the single read keeps its result");
        }
    }

    {
        KernelOracle oracle;
        auto fixture = make_fixture(t, 3 * kPage);
        auto harness = make_real_harness(t, 4, 3, &oracle);
        if (fixture.has_value() && harness.has_value()) {
            AsyncIoContext& ctx = *harness->ctx;
            UringAsyncBackend& backend = *harness->backend;
            std::vector<std::vector<std::byte>> buffers(3, std::vector<std::byte>(kPage));
            std::vector<std::uint64_t> offsets{0, kPage, 2 * kPage};
            std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
            const auto report =
                backend.submit_ready_reads(std::span<const ReadOp>(ops.data(), ops.size()));
            t.check(report.accepted == 3 && report.untouched == 0,
                    "a three-member ready group is accepted whole");
            t.check(report.stop == UringAsyncBackend::ReadyBatchStop::input_exhausted,
                    "the three-member group stops at input exhaustion");
            t.check(oracle.count == 1 && oracle.calls[0].ready == 3,
                    "the three-member group costs one enter with three SQEs");
            auto requests = requests_for(ctx, report);
            t.check(drive_context(ctx, requests), "the three-member group terminates");
            for (std::size_t i = 0; i < requests.size(); ++i)
                check_result(t, requests[i], kPage, "each group member keeps its own result");
            for (std::size_t i = 0; i < buffers.size(); ++i)
                check_bytes(t, buffers[i], *fixture, i * kPage,
                            "each group member lands its own bytes");
        }
    }

    {
        constexpr std::size_t kCount = 16;
        KernelOracle oracle;
        auto fixture = make_fixture(t, kCount * kPage);
        auto harness = make_real_harness(t, kCount + 2, static_cast<unsigned>(kCount), &oracle);
        if (fixture.has_value() && harness.has_value()) {
            AsyncIoContext& ctx = *harness->ctx;
            UringAsyncBackend& backend = *harness->backend;
            std::vector<std::vector<std::byte>> buffers(kCount, std::vector<std::byte>(kPage));
            std::vector<std::uint64_t> offsets;
            for (std::size_t i = 0; i < kCount; ++i)
                offsets.push_back(i * kPage);
            std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
            const auto report =
                backend.submit_ready_reads(std::span<const ReadOp>(ops.data(), ops.size()));
            t.check(report.accepted == UringAsyncBackend::ready_batch_cap,
                    "a group larger than the cap accepts exactly the cap prefix");
            t.check(report.stop == UringAsyncBackend::ReadyBatchStop::cap,
                    "the cap stop is reported");
            t.check(report.untouched_begin == report.accepted,
                    "the untouched boundary follows the accepted prefix at the cap");
            t.check(report.untouched == kCount - report.accepted,
                    "the cap returns every unaccepted member in the suffix");
            t.check(oracle.count == 1 && oracle.calls[0].ready == 8,
                    "the capped prefix costs one enter with eight SQEs");
            std::vector<Request<std::size_t>> requests = requests_for(ctx, report);
            std::vector<RequestId> ids;
            for (const Request<std::size_t>& request : requests)
                ids.push_back(request.id());
            for (std::size_t index = report.untouched_begin; index < kCount; ++index) {
                const auto single = backend.submit_ready_reads(
                    std::span<const ReadOp>(&ops[index], 1));
                t.check(single.accepted == 1 && single.untouched == 0,
                        "a suffix member is submitted alone exactly once");
                requests.push_back(
                    TestAccess::make_request<std::size_t>(ctx, single.keys[0]));
                ids.push_back(requests.back().id());
            }
            t.check(requests.size() == kCount, "cap prefix and suffix cover every input once");
            bool duplicate = false;
            for (std::size_t i = 0; i < ids.size(); ++i)
                for (std::size_t j = i + 1; j < ids.size(); ++j)
                    duplicate = duplicate || ids[i] == ids[j];
            t.check(!duplicate, "no accepted identity appears twice across prefix and suffix");
            t.check(oracle.max_sqes <= UringAsyncBackend::ready_batch_cap,
                    "no enter exceeds the ready-batch cap");
            t.check(drive_context(ctx, requests), "all sixteen members terminate");
            for (std::size_t i = 0; i < requests.size(); ++i)
                check_result(t, requests[i], kPage, "every capped-group member keeps its result");
            for (std::size_t i = 0; i < kCount; ++i)
                check_bytes(t, buffers[i], *fixture, i * kPage,
                            "every capped-group member lands its own bytes");
        }
    }

    {
        constexpr std::size_t kCount = 6;
        constexpr unsigned kRing = 4;
        KernelOracle oracle;
        auto fixture = make_fixture(t, kCount * kPage);
        auto harness = make_real_harness(t, 8, kRing, &oracle);
        if (fixture.has_value() && harness.has_value()) {
            AsyncIoContext& ctx = *harness->ctx;
            UringAsyncBackend& backend = *harness->backend;
            std::vector<std::vector<std::byte>> buffers(kCount, std::vector<std::byte>(kPage));
            std::vector<std::uint64_t> offsets;
            for (std::size_t i = 0; i < kCount; ++i)
                offsets.push_back(i * kPage);
            std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
            const auto report =
                backend.submit_ready_reads(std::span<const ReadOp>(ops.data(), ops.size()));
            t.check(report.accepted == kRing,
                    "a group larger than the SQ budget accepts only the dispatchable prefix");
            t.check(report.stop == UringAsyncBackend::ReadyBatchStop::sq_budget,
                    "the SQ-budget stop is reported when the prefix is below the cap");
            t.check(report.untouched_begin == report.accepted && report.untouched == kCount - kRing,
                    "the SQ-budget boundary returns the unaccepted suffix");
            t.check(oracle.count == 1 && oracle.calls[0].ready == kRing,
                    "the SQ-budget prefix costs one enter with the ring budget");
            std::vector<Request<std::size_t>> requests = requests_for(ctx, report);
            for (std::size_t index = report.untouched_begin; index < kCount; ++index) {
                const auto single = backend.submit_ready_reads(
                    std::span<const ReadOp>(&ops[index], 1));
                t.check(single.accepted == 1, "an SQ-budget suffix member is submitted alone");
                requests.push_back(
                    TestAccess::make_request<std::size_t>(ctx, single.keys[0]));
            }
            t.check(drive_context(ctx, requests), "all budget-limited members terminate");
            for (std::size_t i = 0; i < requests.size(); ++i)
                check_result(t, requests[i], kPage, "every budget-limited member keeps its result");
        }
    }
}

// A full SQ with unproven transport residue is not a ready transport: the next
// batch accepts nothing and returns the whole group untouched. This is the
// named target of SLUICE_R3_MUTANT_BATCH_INCLUDES_NOT_READY_TRANSPORT.
void sq_full_transport_residue_rejects_batch(Tracker& t) {
    ScriptedKernel kernel;
    kernel.store(ScriptedKernel::Mode::hold);
    auto fixture = make_fixture(t, 6 * kPage);
    auto harness = make_scripted_harness(t, 8, 4, kernel);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> held_buffers(4, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> held_offsets{0, kPage, 2 * kPage, 3 * kPage};
    std::vector<ReadOp> held_ops = file_ops(*fixture, held_buffers, held_offsets);
    const auto held = backend.submit_ready_reads(
        std::span<const ReadOp>(held_ops.data(), held_ops.size()));
    t.check(held.accepted == 4, "the first batch fills the SQ ring");
    t.check(backend.sq_ready_for_test() == 4,
            "the SQ is full and its SQEs are unproven by the kernel");
    t.check(backend.transport_ledger_size_for_test() == 4,
            "the held batch keeps every prepared SQE in the transport ledger");
    t.check(held.residue_advertised, "the held batch advertises its transport residue");
    auto held_requests = requests_for(ctx, held);

    std::vector<std::vector<std::byte>> later_buffers(2, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> later_offsets{4 * kPage, 5 * kPage};
    std::vector<ReadOp> later_ops = file_ops(*fixture, later_buffers, later_offsets);
    const std::uint64_t flushes_before = backend.submit_flushes_for_test();
    const auto later = backend.submit_ready_reads(
        std::span<const ReadOp>(later_ops.data(), later_ops.size()));
    t.check(later.accepted == 0,
            "a batch at SQ-full transport residue accepts nothing");
    t.check(later.stop == UringAsyncBackend::ReadyBatchStop::recovery,
            "the transport-residue boundary is reported as recovery");
    t.check(later.untouched_begin == 0 && later.untouched == 2,
            "the whole group stays untouched at the transport-residue boundary");
    t.check(later.flush_attempts == 0, "the rejected batch attempts no flush");
    t.check(backend.submit_flushes_for_test() == flushes_before,
            "the rejected batch adds no transport submit");
    t.check(backend.transport_ledger_size_for_test() == 4,
            "the rejected batch leaves the transport ledger unchanged");
    t.check(backend.dispatch_size_for_test() == 0,
            "the rejected batch leaves the dispatch queue empty");

    auto later_requests = requests_for(ctx, later);
    for (Request<std::size_t>& request : later_requests)
        held_requests.push_back(std::move(request));

    kernel.store(ScriptedKernel::Mode::consume_all);
    (void)ctx.poll();
    for (std::size_t i = 0; i < 6; ++i) {
        const auto cookie = backend.live_cookie_for_offset_for_test(i * kPage);
        if (cookie.has_value())
            backend.inject_cqe_for_test(*cookie, static_cast<int>(kPage));
    }
    t.check(drive_context(ctx, held_requests), "every held or rejected member reaches a terminal");
    for (Request<std::size_t>& request : held_requests)
        check_result(t, request, kPage, "the held member keeps its own result");
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    t.check(outcome.has_value() && outcome.value() == ShutdownOutcome::completed,
            "the SQ-full residue boundary still settles");
}

// A multi-SQE flush that the kernel consumes only partially leaves a real
// ledger residue. The report must advertise it and the next pass must retry
// exactly the remaining prefix. This is the named target of
// SLUICE_R3_MUTANT_BATCH_RESIDUE_NOT_ADVERTISED.
void partial_submit_advertises_and_retries_residue(Tracker& t) {
    ScriptedKernel kernel;
    kernel.store(ScriptedKernel::Mode::partial_first);
    auto fixture = make_fixture(t, 2 * kPage);
    auto harness = make_scripted_harness(t, 4, 4, kernel);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> offsets{0, kPage};
    std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
    const auto report = backend.submit_ready_reads(std::span<const ReadOp>(ops.data(), ops.size()));
    t.check(report.accepted == 2, "both members are accepted before the partial flush");
    t.check(report.flush_attempts == 1, "the batch attempts exactly one flush");
    t.check(report.residue_advertised,
            "partial kernel consumption advertises dispatch residue");
    t.check(kernel.first_ready.load() == 2 && kernel.first_rc.load() == 1,
            "the kernel consumed one of the two pending SQEs");
    t.check(backend.transport_ledger_size_for_test() == 1,
            "one prepared SQE stays in the transport ledger");
    t.check(backend.submit_flushes_for_test() == 1, "exactly one transport flush has run");

    (void)ctx.poll();
    t.check(backend.transport_ledger_size_for_test() == 0,
            "the next pass consumes the remaining ledger prefix");
    t.check(backend.submit_flushes_for_test() == 2,
            "the residue costs exactly one further transport flush");
    t.check(kernel.second_ready.load() == 1 && kernel.second_rc.load() == 1,
            "the retry submits exactly the remaining one SQE");

    const auto cookie0 = backend.live_cookie_for_offset_for_test(0);
    const auto cookie1 = backend.live_cookie_for_offset_for_test(kPage);
    t.check(cookie0.has_value() && cookie1.has_value(), "both members keep a live route");
    if (cookie0.has_value())
        backend.inject_cqe_for_test(*cookie0, static_cast<int>(kPage));
    if (cookie1.has_value())
        backend.inject_cqe_for_test(*cookie1, static_cast<int>(kPage));
    auto requests = requests_for(ctx, report);
    t.check(drive_context(ctx, requests), "both partially submitted members terminate");
    check_result(t, requests[0], kPage, "the first retried member keeps its result");
    check_result(t, requests[1], kPage, "the second retried member keeps its result");
}

// Cancel and CQE ordering across a batch: a non-cancelled member's CQE is
// reaped before and after a cancel SQE is issued; a control CQE may arrive
// before the cancelled original CQE; a cancel may be issued after the CQE was
// produced but before it was reaped. The original terminal stays authoritative
// in every order.
void cancel_and_cqe_ordering(Tracker& t) {
    ScriptedKernel kernel;
    kernel.store(ScriptedKernel::Mode::hold);
    auto fixture = make_fixture(t, 4 * kPage);
    auto harness = make_scripted_harness(t, 6, 4, kernel);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> buffers(4, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> offsets{0, kPage, 2 * kPage, 3 * kPage};
    std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
    const auto report = backend.submit_ready_reads(std::span<const ReadOp>(ops.data(), ops.size()));
    t.check(report.accepted == 4, "the four-member batch is accepted");
    auto requests = requests_for(ctx, report);

    const auto cookie0 = backend.live_cookie_for_offset_for_test(0);
    t.check(cookie0.has_value(), "member 0 has a live route");
    if (cookie0.has_value())
        backend.inject_cqe_for_test(*cookie0, static_cast<int>(kPage));

    const auto disposition = requests[3].cancel();
    t.check(disposition.has_value(), "cancelling a batched member returns a disposition");
    t.check(disposition.has_value() &&
                disposition.value() == CancelDisposition::requested,
            "a running batched member reports the requested disposition");
    t.check(backend.pending_control_snapshot_for_test().authoritative_pending != 0,
            "a cancel without SQ space leaves a sticky control intent");

    kernel.store(ScriptedKernel::Mode::consume_all);
    (void)ctx.poll();
    t.check(requests[0].ready(), "the CQE produced before the cancel is reaped and published");
    const auto prepared = backend.pending_control_snapshot_for_test();
    t.check(prepared.prepared_controls == 1 && prepared.authoritative_pending == 0,
            "the sticky intent becomes one prepared cancel SQE");
    t.check(backend.live_control_sqes_for_test() == 0,
            "the prepared cancel SQE is not yet proven consumed");
    (void)ctx.poll();
    const auto submitted = backend.pending_control_snapshot_for_test();
    t.check(submitted.submitted_controls == 1 && backend.live_control_sqes_for_test() == 1,
            "the next flush consumes the cancel SQE and marks it submitted");

    const auto cookie3 = backend.live_cookie_for_offset_for_test(3 * kPage);
    t.check(cookie3.has_value(), "the cancelled member keeps its live route");
    if (cookie3.has_value())
        backend.inject_cqe_for_test(kControlTag | *cookie3, 0);
    (void)ctx.poll();
    t.check(!requests[3].ready(),
            "a control CQE before the original CQE does not create a terminal");
    if (cookie3.has_value())
        backend.inject_cqe_for_test(*cookie3, -ECANCELED);
    const auto cookie1 = backend.live_cookie_for_offset_for_test(kPage);
    const auto cookie2 = backend.live_cookie_for_offset_for_test(2 * kPage);
    if (cookie1.has_value())
        backend.inject_cqe_for_test(*cookie1, static_cast<int>(kPage));
    if (cookie2.has_value())
        backend.inject_cqe_for_test(*cookie2, static_cast<int>(kPage));
    (void)ctx.poll();

    t.check(drive_context(ctx, requests), "every batch member reaches a terminal");
    check_result(t, requests[0], kPage, "the CQE-before-cancel member keeps its real result");
    check_result(t, requests[1], kPage, "the first untouched member keeps its result");
    check_result(t, requests[2], kPage, "the second untouched member keeps its result");
    const auto cancelled = requests[3].take_result();
    t.check(cancelled.readiness == RequestReadiness::ready && !cancelled.result.has_value() &&
                cancelled.result.error().code == IoError::Code::canceled,
            "the cancelled member reports cancellation, not a fabricated result");
    const auto snapshot = backend.pending_control_snapshot_for_test();
    t.check(snapshot.live_routes == 0 && snapshot.authoritative_pending == 0 &&
                snapshot.prepared_controls == 0 && snapshot.submitted_controls == 0,
            "control and route accounting retire with the cancelled member");
    t.check(backend.live_cookies_for_test() == 0, "no live route survives the batch");
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    t.check(outcome.has_value() && outcome.value() == ShutdownOutcome::completed,
            "the cancel path settles");
}

// A completed batch wakes an owner parked at the pre-park handshake: the
// kernel completion is produced while the owner is stopped after the final
// revalidation and before poll(2); resuming must observe progress.
void prepark_batch_completion_is_not_lost(Tracker& t) {
    auto harness = make_real_harness(t, 6, 4, nullptr);
    if (!harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;

    int fds[2];
    if (::pipe(fds) != 0) {
        t.check(false, "pipe fixture is created");
        return;
    }
    std::vector<std::vector<std::byte>> buffers(4, std::vector<std::byte>(kPage));
    std::vector<ReadOp> ops = pipe_ops(fds[0], buffers, 4);
    const auto report = backend_of(ctx)->submit_ready_reads(
        std::span<const ReadOp>(ops.data(), ops.size()));
    t.check(report.accepted == 4 && !report.residue_advertised,
            "the parked batch is fully submitted with no residue");
    auto requests = requests_for(ctx, report);

    detail::ProgressSource::PauseGate gate;
    TestAccess::context_set_prepark_pause_gate(ctx, &gate);
    std::optional<AsyncIoContext::ProgressWaitOutcome> wait_outcome;
    std::thread owner([&ctx, &wait_outcome] {
        const auto outcome = ctx.wait_one(std::chrono::seconds(2));
        if (outcome.has_value())
            wait_outcome = outcome.value();
    });
    bool paused = gate.paused.load(std::memory_order_acquire);
    while (!paused) {
        gate.paused.wait(paused, std::memory_order_acquire);
        paused = gate.paused.load(std::memory_order_acquire);
    }
    t.check(paused, "the owner is stopped at the pre-park handshake");
    const std::string payload(4 * kPage, 'w');
    t.check(::write(fds[1], payload.data(), payload.size()) ==
                static_cast<ssize_t>(payload.size()),
            "the batch completion is produced while the owner is pre-park");
    TestAccess::context_set_prepark_pause_gate(ctx, nullptr);
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
    owner.join();
    t.check(wait_outcome.has_value() &&
                wait_outcome->kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress &&
                wait_outcome->completed >= 4,
            "the pre-park completion wakes the owner as progress");
    t.check(all_ready(requests), "every batch member is published after the wake");
    for (Request<std::size_t>& request : requests)
        check_result(t, request, kPage, "the pre-park member keeps its result");
    ::close(fds[0]);
    ::close(fds[1]);
}

// CQ overflow from many batch completions is flushed, not lost: the overflow
// flush runs and every member keeps its own result.
void overflow_flush_keeps_every_batch_completion(Tracker& t) {
    constexpr std::size_t kCount = 32;
    KernelOracle oracle;
    auto fixture = make_fixture(t, kCount * kPage);
    auto harness = make_real_harness(t, kCount, 8, &oracle);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> buffers(kCount, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> offsets;
    for (std::size_t i = 0; i < kCount; ++i)
        offsets.push_back(i * kPage);
    std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
    std::vector<Request<std::size_t>> requests;
    t.check(submit_all_batched(t, ctx, backend, ops, requests, "every overflow input is accepted"),
            "all overflow inputs are admitted without polling");
    t.check(requests.size() == kCount, "every submitted input produces one request");
    t.check(drive_context(ctx, requests), "every overflow member termins");
    t.check(oracle.get_events_calls >= 1, "the overflow list is flushed by the reap path");
    for (std::size_t i = 0; i < requests.size(); ++i)
        check_result(t, requests[i], kPage, "every overflow member keeps its own result");
    for (std::size_t i = 0; i < kCount; ++i)
        check_bytes(t, buffers[i], *fixture, i * kPage,
                    "every overflow member lands its own bytes");
}

// A batch whose flush partially reaches the kernel and then poisons it: the
// kernel-visible member keeps its real completion path, the invisible member
// is retired as a physical failure and never as success.
void poison_after_partial_batch_never_fabricates_success(Tracker& t) {
    ScriptedKernel kernel;
    kernel.store(ScriptedKernel::Mode::partial_then_poison);
    auto fixture = make_fixture(t, 2 * kPage);
    auto harness = make_scripted_harness(t, 4, 2, kernel);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> offsets{0, kPage};
    std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
    const auto report = backend.submit_ready_reads(std::span<const ReadOp>(ops.data(), ops.size()));
    t.check(report.accepted == 2, "both members are accepted before the poisoned flush");
    t.check(report.residue_advertised, "the poisoned flush advertises its residue");
    t.check(kernel.first_ready.load() == 2 && kernel.first_rc.load() == -EIO,
            "the kernel saw one SQE and reported the hard flush failure");
    t.check(backend.transport_ledger_size_for_test() == 1,
            "only the provably kernel-invisible SQE remains in the ledger");
    auto requests = requests_for(ctx, report);

    const auto visible_cookie = backend.live_cookie_for_offset_for_test(0);
    t.check(visible_cookie.has_value(),
            "the kernel-visible member keeps its route after poison");
    t.check(!backend.live_cookie_for_offset_for_test(kPage).has_value(),
            "the invisible member has no route to fabricate a completion");
    if (visible_cookie.has_value())
        backend.inject_cqe_for_test(*visible_cookie, static_cast<int>(kPage));
    t.check(drive_context(ctx, requests), "both poisoned-batch members reach a terminal");
    check_result(t, requests[0], kPage,
                 "the kernel-visible member keeps its real 4 KiB result");
    const auto failed = requests[1].take_result();
    t.check(failed.readiness == RequestReadiness::ready && !failed.result.has_value() &&
                failed.result.error().code == IoError::Code::backend_error,
            "the invisible member reports the physical failure, never success");
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    t.check(outcome.has_value() && outcome.value() == ShutdownOutcome::completed,
            "the poisoned batch settles without unresolved accepted work");
}

// Shutdown drains a batch that was accepted but never polled; every member is
// published with its own result, and a later batch entry on the retired ring
// returns the whole group untouched.
void shutdown_drain_settles_batch(Tracker& t) {
    constexpr std::size_t kCount = 4;
    KernelOracle oracle;
    auto fixture = make_fixture(t, kCount * kPage);
    auto harness = make_real_harness(t, kCount + 2, static_cast<unsigned>(kCount), &oracle);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> buffers(kCount, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> offsets;
    for (std::size_t i = 0; i < kCount; ++i)
        offsets.push_back(i * kPage);
    std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
    const auto report = backend.submit_ready_reads(std::span<const ReadOp>(ops.data(), ops.size()));
    t.check(report.accepted == kCount && report.untouched == 0,
            "the shutdown batch is accepted whole before any poll");
    auto requests = requests_for(ctx, report);
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    t.check(outcome.has_value() && outcome.value() == ShutdownOutcome::completed,
            "drain shutdown settles the accepted batch");
    t.check(ctx.execution_closed(), "execution is closed after the successful drain");
    t.check(all_ready(requests), "shutdown publication makes every member ready");
    for (std::size_t i = 0; i < requests.size(); ++i) {
        check_result(t, requests[i], kPage, "every drained member keeps its result");
        check_bytes(t, buffers[i], *fixture, i * kPage,
                    "every drained member lands its own bytes");
    }

    std::vector<std::vector<std::byte>> later_buffers(2, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> later_offsets{0, kPage};
    std::vector<ReadOp> later_ops = file_ops(*fixture, later_buffers, later_offsets);
    const auto later = backend.submit_ready_reads(
        std::span<const ReadOp>(later_ops.data(), later_ops.size()));
    t.check(later.accepted == 0, "a batch entry after shutdown accepts nothing");
    t.check(later.stop == UringAsyncBackend::ReadyBatchStop::recovery,
            "a retired ring is reported as the recovery boundary");
    t.check(later.error.code == IoError::Code::backend_error,
            "a retired ring reports backend_error");
    t.check(later.untouched_begin == 0 && later.untouched == 2,
            "a retired ring leaves the whole group untouched");
}

// The borrow of every member survives until its own CQE: two pipe reads in one
// batch stay pending with untouched buffers, one payload release completes
// exactly one member, and the last member completes only on its own release.
void borrow_and_last_member_are_independent(Tracker& t) {
    auto harness = make_real_harness(t, 4, 2, nullptr);
    if (!harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    int fds[2];
    if (::pipe(fds) != 0) {
        t.check(false, "pipe fixture is created");
        return;
    }
    const std::byte sentinel{0x3c};
    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(kPage, sentinel));
    std::vector<ReadOp> ops = pipe_ops(fds[0], buffers, 2);
    const auto report = backend.submit_ready_reads(std::span<const ReadOp>(ops.data(), ops.size()));
    t.check(report.accepted == 2, "both pipe reads are accepted as independent members");
    auto requests = requests_for(ctx, report);
    t.check(backend.live_cookies_for_test() == 2,
            "both members hold a live kernel route");
    t.check(!requests[0].ready() && !requests[1].ready(),
            "no member is published while its borrow is live");
    t.check(buffers[0][0] == sentinel && buffers[1][0] == sentinel,
            "no member's buffer is touched before its own completion");

    const std::string first_payload(kPage, 'e');
    t.check(::write(fds[1], first_payload.data(), first_payload.size()) ==
                static_cast<ssize_t>(first_payload.size()),
            "the first member's payload is released");
    t.check(drive_any_ready(ctx, requests), "one batch member reaches its terminal");
    const std::size_t first_done = requests[0].ready() ? 0 : 1;
    const std::size_t last_done = 1 - first_done;
    t.check(requests[first_done].ready() != requests[last_done].ready(),
            "only the member whose payload arrived is published");
    t.check(std::memcmp(buffers[first_done].data(), first_payload.data(), kPage) == 0 &&
                buffers[last_done][0] == sentinel,
            "the completed member writes its own buffer; the last member's borrow is untouched");

    const std::string last_payload(kPage, 'f');
    t.check(::write(fds[1], last_payload.data(), last_payload.size()) ==
                static_cast<ssize_t>(last_payload.size()),
            "the last member's payload is released");
    t.check(drive_request(ctx, requests[last_done]), "the last member reaches its terminal");
    check_result(t, requests[first_done], kPage, "the first completed member keeps its 4 KiB result");
    check_result(t, requests[last_done], kPage, "the last member keeps its 4 KiB result");
    t.check(std::memcmp(buffers[last_done].data(), last_payload.data(), kPage) == 0,
            "the last member writes its own buffer");
    ::close(fds[0]);
    ::close(fds[1]);
}

// A consumed batch releases its slots; the next batch reuses them with
// advanced generations, the old keys are stale, and no consumed SQE is
// re-emitted.
void stale_generation_after_consumed_batch(Tracker& t) {
    constexpr std::size_t kCount = 2;
    KernelOracle oracle;
    auto fixture = make_fixture(t, 4 * kPage);
    auto harness = make_real_harness(t, kCount + 2, static_cast<unsigned>(kCount), &oracle);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> first_buffers(kCount, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> first_offsets{0, kPage};
    std::vector<ReadOp> first_ops = file_ops(*fixture, first_buffers, first_offsets);
    const auto first = backend.submit_ready_reads(
        std::span<const ReadOp>(first_ops.data(), first_ops.size()));
    t.check(first.accepted == 2, "the first batch is accepted");
    auto first_requests = requests_for(ctx, first);
    t.check(drive_context(ctx, first_requests), "the first batch terminates");
    for (Request<std::size_t>& request : first_requests)
        check_result(t, request, kPage, "the first batch member is consumed");

    const unsigned enters_before = oracle.count;
    const unsigned sqes_before = oracle.total_sqes;
    std::vector<std::vector<std::byte>> second_buffers(kCount, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> second_offsets{2 * kPage, 3 * kPage};
    std::vector<ReadOp> second_ops = file_ops(*fixture, second_buffers, second_offsets);
    const auto second = backend.submit_ready_reads(
        std::span<const ReadOp>(second_ops.data(), second_ops.size()));
    t.check(second.accepted == 2, "the second batch is accepted into the reclaimed slots");
    t.check(!second.residue_advertised,
            "the reclaimed-slot batch emits no duplicate SQE or residue");
    t.check(oracle.count == enters_before + 1 &&
                oracle.total_sqes == sqes_before + 2,
            "the reclaimed-slot batch emits exactly two fresh SQEs in one enter");
    bool advanced = true;
    for (std::size_t i = 0; i < kCount; ++i) {
        const bool reused = first.keys[i].slot.value == second.keys[i].slot.value;
        if (reused)
            advanced = advanced &&
                       second.keys[i].generation.value > first.keys[i].generation.value;
        advanced = advanced && !(first.keys[i].slot.value == second.keys[i].slot.value &&
                                 first.keys[i].generation.value == second.keys[i].generation.value);
    }
    t.check(advanced, "every reused slot advances its generation");
    for (std::size_t i = 0; i < kCount; ++i)
        t.check(TestAccess::context_core(ctx)->lookup(first.keys[i]) ==
                    detail::PublicLookup::not_found,
                "every consumed key is stale after the batch");
    auto second_requests = requests_for(ctx, second);
    t.check(drive_context(ctx, second_requests), "the second batch terminates");
    for (std::size_t i = 0; i < kCount; ++i) {
        check_result(t, second_requests[i], kPage,
                     "the reclaimed-slot member keeps its own result");
        check_bytes(t, second_buffers[i], *fixture, second_offsets[i],
                    "the reclaimed-slot member lands its own bytes");
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* only = nullptr;
    for (int i = 1; i < argc; ++i) {
        static constexpr char kPrefix[] = "--only=";
        if (std::strncmp(argv[i], kPrefix, sizeof(kPrefix) - 1) == 0)
            only = argv[i] + sizeof(kPrefix) - 1;
    }
    const auto selected = [only](const char* scenario) {
        return only == nullptr || std::strcmp(only, scenario) == 0;
    };

    Tracker tracker{"uring_ready_batch_deterministic"};
    if (selected("d2_positive")) d2_positive_two_ready_reads_one_enter(tracker);
    if (selected("d2_negative")) d2_negative_single_refill_while_in_flight(tracker);
    if (selected("d1_dependency")) d1_dependent_second_never_enters_ready_group(tracker);
    if (selected("d3_budget")) d3_d16_budget_prefix_and_suffix_accounting(tracker);
    if (selected("sq_full_residue")) sq_full_transport_residue_rejects_batch(tracker);
    if (selected("partial_submit")) partial_submit_advertises_and_retries_residue(tracker);
    if (selected("cancel_ordering")) cancel_and_cqe_ordering(tracker);
    if (selected("prepark_wake")) prepark_batch_completion_is_not_lost(tracker);
    if (selected("overflow")) overflow_flush_keeps_every_batch_completion(tracker);
    if (selected("poison")) poison_after_partial_batch_never_fabricates_success(tracker);
    if (selected("shutdown")) shutdown_drain_settles_batch(tracker);
    if (selected("borrow")) borrow_and_last_member_are_independent(tracker);
    if (selected("stale_generation")) stale_generation_after_consumed_batch(tracker);
    if (tracker.failures != 0) {
        std::fprintf(stderr, "%s: %d failure(s)\n", tracker.name, tracker.failures);
        return 1;
    }
    std::printf("%s: all deterministic ready-batch oracles passed\n", tracker.name);
    return 0;
}
