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
    enum class Mode : int {
        consume_all,
        partial_first,
        hold,
        partial_then_poison,
        partial_then_eagain,
        partial_then_eintr,
    };

    std::atomic<int> mode{static_cast<int>(Mode::consume_all)};
    std::atomic<int> calls{0};
    std::atomic<unsigned> first_ready{0};
    std::atomic<unsigned> second_ready{0};
    std::atomic<long long> second_first_offset{-1};
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
        if (ready > 0) {
            const unsigned index = ring->sq.array != nullptr
                                       ? ring->sq.array[(*ring->sq.khead) & *ring->sq.kring_mask]
                                       : ((*ring->sq.khead) & *ring->sq.kring_mask);
            kernel->second_first_offset.store(
                static_cast<long long>(ring->sq.sqes[index].off),
                std::memory_order_relaxed);
        }
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
    case ScriptedKernel::Mode::partial_then_eagain:
        if (call == 1 && ready > 2) {
            *ring->sq.khead += 2;
            kernel->store(ScriptedKernel::Mode::consume_all);
            rc = -EAGAIN;
        } else {
            *ring->sq.khead += ready;
            rc = static_cast<int>(ready);
        }
        break;
    case ScriptedKernel::Mode::partial_then_eintr:
        if (call == 1 && ready > 2) {
            *ring->sq.khead += 2;
            kernel->store(ScriptedKernel::Mode::consume_all);
            rc = -EINTR;
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

std::vector<Request<std::size_t>>
requests_from(TestAccess::ReadyReadsBatch& batch) {
    std::vector<Request<std::size_t>> requests;
    for (std::size_t i = 0; i < batch.accepted; ++i)
        requests.push_back(std::move(batch.requests[i]));
    return requests;
}

template <class Gate> void wait_gate_paused(Gate& gate) {
    bool paused = gate.paused.load(std::memory_order_acquire);
    while (!paused) {
        gate.paused.wait(paused, std::memory_order_acquire);
        paused = gate.paused.load(std::memory_order_acquire);
    }
}

template <class Gate> void release_gate(Gate& gate) {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

bool wait_for_core_admission_closed(AsyncIoContext& ctx) {
    detail::RequestCore* core = TestAccess::context_core(ctx);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (core->admission_open()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

bool wait_for_cq_overflow(UringAsyncBackend& backend) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!backend.cq_overflow_for_test()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

bool submit_all_batched(Tracker& t, AsyncIoContext& ctx, UringAsyncBackend& backend,
                        const std::vector<ReadOp>& ops,
                        std::vector<Request<std::size_t>>& requests, const char* label) {
    (void)backend;
    std::size_t next = 0;
    while (next < ops.size()) {
        const std::size_t room = std::min<std::size_t>(UringAsyncBackend::ready_batch_cap,
                                                       ops.size() - next);
        const std::size_t base = next;
        auto batch = TestAccess::context_submit_ready_reads(
            ctx, std::span<const ReadOp>(ops.data() + base, room));
        for (std::size_t i = 0; i < batch.accepted; ++i)
            requests.push_back(std::move(batch.requests[i]));
        if (batch.stop == UringAsyncBackend::ReadyBatchStop::rejected) {
            t.check(false, label);
            return false;
        }
        next = base + batch.accepted;
        if (batch.accepted == 0) {
            t.check(false, label);
            return false;
        }
        for (std::size_t i = batch.untouched_begin; i < room; ++i) {
            auto single = TestAccess::context_submit_ready_reads(
                ctx, std::span<const ReadOp>(&ops[base + i], 1));
            if (single.accepted != 1) {
                t.check(false, label);
                return false;
            }
            requests.push_back(std::move(single.requests[0]));
            ++next;
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
// named target of SLUICE_READY_BATCH_MUTANT_INCLUDES_PENDING_TRANSPORT.
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
    t.check(later.error.code == IoError::Code::would_block,
            "a non-fatal transport residue reports the retryable error, not eof");
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
// SLUICE_READY_BATCH_MUTANT_RESIDUE_UNADVERTISED.
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
    auto batch = TestAccess::context_submit_ready_reads(ctx, ops);
    t.check(batch.accepted == 4, "the four-member batch is accepted");
    auto requests = requests_from(batch);

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
    auto batch = TestAccess::context_submit_ready_reads(ctx, ops);
    t.check(batch.accepted == 4 && !batch.residue_advertised,
            "the parked batch is fully submitted with no residue");
    auto requests = requests_from(batch);

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
    t.check(wait_for_cq_overflow(backend),
            "the kernel parks completions in the overflow list before the first reap");
    t.check(oracle.get_events_calls == 0, "no overflow flush ran before the reap observed it");
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
    auto batch = TestAccess::context_submit_ready_reads(ctx, ops);
    t.check(batch.accepted == kCount && batch.untouched == 0,
            "the shutdown batch is accepted whole before any poll");
    auto requests = requests_from(batch);
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
    auto batch = TestAccess::context_submit_ready_reads(ctx, ops);
    t.check(batch.accepted == 2, "both pipe reads are accepted as independent members");
    auto requests = requests_from(batch);
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

    std::vector<std::vector<std::byte>> first_buffers(kCount, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> first_offsets{0, kPage};
    std::vector<ReadOp> first_ops = file_ops(*fixture, first_buffers, first_offsets);
    auto first = TestAccess::context_submit_ready_reads(ctx, first_ops);
    t.check(first.accepted == 2, "the first batch is accepted");
    std::array<detail::RequestKey, kCount> first_keys{};
    for (std::size_t i = 0; i < kCount; ++i)
        first_keys[i] = TestAccess::request_key(first.requests[i]);
    auto first_requests = requests_from(first);
    t.check(drive_context(ctx, first_requests), "the first batch terminates");
    for (Request<std::size_t>& request : first_requests)
        check_result(t, request, kPage, "the first batch member is consumed");

    const unsigned enters_before = oracle.count;
    const unsigned sqes_before = oracle.total_sqes;
    std::vector<std::vector<std::byte>> second_buffers(kCount, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> second_offsets{2 * kPage, 3 * kPage};
    std::vector<ReadOp> second_ops = file_ops(*fixture, second_buffers, second_offsets);
    auto second = TestAccess::context_submit_ready_reads(ctx, second_ops);
    t.check(second.accepted == 2, "the second batch is accepted into the reclaimed slots");
    t.check(!second.residue_advertised,
            "the reclaimed-slot batch emits no duplicate SQE or residue");
    t.check(oracle.count == enters_before + 1 &&
                oracle.total_sqes == sqes_before + 2,
            "the reclaimed-slot batch emits exactly two fresh SQEs in one enter");
    std::array<detail::RequestKey, kCount> second_keys{};
    for (std::size_t i = 0; i < kCount; ++i)
        second_keys[i] = TestAccess::request_key(second.requests[i]);
    bool advanced = true;
    for (std::size_t i = 0; i < kCount; ++i) {
        const bool reused = first_keys[i].slot.value == second_keys[i].slot.value;
        if (reused)
            advanced = advanced &&
                       second_keys[i].generation.value > first_keys[i].generation.value;
        advanced = advanced &&
                   !(first_keys[i].slot.value == second_keys[i].slot.value &&
                     first_keys[i].generation.value == second_keys[i].generation.value);
    }
    t.check(advanced, "every reused slot advances its generation");
    for (std::size_t i = 0; i < kCount; ++i)
        t.check(TestAccess::context_core(ctx)->lookup(first_keys[i]) ==
                    detail::PublicLookup::not_found,
                "every consumed key is stale after the batch");
    auto second_requests = requests_from(second);
    t.check(drive_context(ctx, second_requests), "the second batch terminates");
    for (std::size_t i = 0; i < kCount; ++i) {
        check_result(t, second_requests[i], kPage,
                     "the reclaimed-slot member keeps its own result");
        check_bytes(t, second_buffers[i], *fixture, second_offsets[i],
                    "the reclaimed-slot member lands its own bytes");
    }
}

// A batch is not one atomic acceptance: with a hold at the second member's
// accept, the first member is already in the accepted set when the owner
// closes admission; the second member is rejected and the third is untouched.
void mixed_acceptance_before_close(Tracker& t) {
    KernelOracle oracle;
    auto fixture = make_fixture(t, 3 * kPage);
    auto harness = make_real_harness(t, 4, 4, &oracle);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> buffers(3, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> offsets{0, kPage, 2 * kPage};
    std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);

    detail::ProgressSource::PauseGate settlement_gate;
    TestAccess::context_set_settlement_start_pause_gate(ctx, &settlement_gate);
    bool owner_failed = false;
    ShutdownOutcome owner_outcome = ShutdownOutcome::health_failed;
    std::thread owner([&] {
        const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
        if (outcome.has_value()) {
            owner_outcome = outcome.value();
        } else {
            owner_failed = true;
        }
    });
    wait_gate_paused(settlement_gate);

    UringAsyncBackend::PreAcceptCommitPauseGate accept_gate;
    accept_gate.target.store(2, std::memory_order_release);
    backend.set_pre_accept_commit_pause_gate(&accept_gate);
    std::optional<TestAccess::ReadyReadsBatch> batch_result;
    std::thread producer([&] {
        batch_result = TestAccess::context_submit_ready_reads(ctx, ops);
    });
    wait_gate_paused(accept_gate);
    const detail::CoreOccupancy during = TestAccess::context_core(ctx)->occupancy();
    t.check(during.accepted_live == 1,
            "the first member is accepted before the second member's accept hold");
    t.check(TestAccess::context_core(ctx)->admission_open(),
            "admission is still open when the batch pauses at the second member");

    release_gate(settlement_gate);
    t.check(wait_for_core_admission_closed(ctx),
            "the owner closes admission between the two member acceptances");
    backend.set_pre_accept_commit_pause_gate(nullptr);
    release_gate(accept_gate);
    producer.join();
    owner.join();
    t.check(!owner_failed && owner_outcome == ShutdownOutcome::completed,
            "the owner settles the partially accepted batch");
    t.check(batch_result.has_value(), "the producer observed a batch report");
    if (batch_result.has_value()) {
        t.check(batch_result->accepted == 1,
                "the batch reports only the member accepted before the close");
        t.check(batch_result->stop == UringAsyncBackend::ReadyBatchStop::rejected,
                "the close rejection stops the batch at the second member");
        t.check(batch_result->error.code == IoError::Code::invalid_state,
                "the rejected member reports admission closed");
        t.check(batch_result->untouched_begin == 2 && batch_result->untouched == 1,
                "the rejected member is attempted and the third stays untouched");
        t.check(batch_result->requests[0].valid() && !batch_result->requests[1].valid(),
                "only the accepted member materializes a request");
        auto accepted = requests_from(*batch_result);
        t.check(drive_request(ctx, accepted[0]), "the accepted member reaches its terminal");
        check_result(t, accepted[0], kPage, "the accepted member keeps its own result");
    }
    t.check(ctx.execution_closed(), "the owner retired the execution");
    const detail::CoreOccupancy settled = TestAccess::context_core(ctx)->occupancy();
    t.check(settled.accepted_live == 0 && settled.public_bindings == 0 &&
                settled.outstanding == 0,
            "the mixed acceptance leaves no live accepted slot or binding");
    TestAccess::context_set_settlement_start_pause_gate(ctx, nullptr);
}

// A span larger than the batch cap is still fully accounted: the backend sees
// the whole input, accepts the cap prefix and reports the rest as untouched.
void context_entry_cap_reports_suffix(Tracker& t) {
    constexpr std::size_t kCount = 16;
    KernelOracle oracle;
    auto fixture = make_fixture(t, kCount * kPage);
    auto harness = make_real_harness(t, kCount + 2, static_cast<unsigned>(kCount), &oracle);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;

    std::vector<std::vector<std::byte>> buffers(kCount, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> offsets;
    for (std::size_t i = 0; i < kCount; ++i)
        offsets.push_back(i * kPage);
    std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
    auto batch = TestAccess::context_submit_ready_reads(ctx, ops);
    t.check(batch.batch_backend, "the cap probe uses the batch backend");
    t.check(batch.accepted == UringAsyncBackend::ready_batch_cap,
            "a span larger than the cap accepts exactly the cap prefix");
    t.check(batch.stop == UringAsyncBackend::ReadyBatchStop::cap,
            "the cap stop is reported for the oversized span");
    t.check(batch.untouched_begin == batch.accepted && batch.untouched == kCount - batch.accepted,
            "every member beyond the cap is reported as untouched");
    std::vector<Request<std::size_t>> requests = requests_from(batch);
    for (std::size_t index = batch.untouched_begin; index < kCount; ++index) {
        auto single = TestAccess::context_submit_ready_reads(
            ctx, std::span<const ReadOp>(&ops[index], 1));
        t.check(single.accepted == 1, "an oversized-span suffix member is accepted alone");
        requests.push_back(std::move(single.requests[0]));
    }
    t.check(requests.size() == kCount, "cap prefix and suffix cover every input once");
    t.check(drive_context(ctx, requests), "every oversized-span member terminates");
    for (std::size_t i = 0; i < requests.size(); ++i) {
        check_result(t, requests[i], kPage, "every oversized-span member keeps its result");
        check_bytes(t, buffers[i], *fixture, i * kPage,
                    "every oversized-span member lands its own bytes");
    }
}

// An inert (moved-from) context cannot reach a backend. The entry reports the
// same rejection submit_read does instead of a success-shaped empty batch.
void inert_context_entry_rejects(Tracker& t) {
    auto fixture = make_fixture(t, kPage);
    auto harness = make_real_harness(t, 2, 2, nullptr);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext moved = std::move(*harness->ctx);
    std::vector<std::vector<std::byte>> buffers(1, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> offsets{0};
    std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
    auto batch = TestAccess::context_submit_ready_reads(*harness->ctx, ops);
    t.check(!batch.batch_backend, "an inert context has no batch backend");
    t.check(batch.accepted == 0 &&
                batch.stop == UringAsyncBackend::ReadyBatchStop::rejected &&
                batch.error.code == IoError::Code::invalid_state,
            "an inert context entry reports invalid_state, not a successful empty batch");
    t.check(batch.untouched_begin == 1 && batch.untouched == 0,
            "the inert context rejects the attempted member");
    const auto direct = harness->ctx->submit_read(ops[0]);
    t.check(!direct.has_value() && direct.error().code == IoError::Code::invalid_state,
            "submit_read rejects the same inert context with invalid_state");
}

// The group gate is a batch-eligibility decision, not a per-request rejection:
// under the same held/control state a span=1 call takes the frozen immediate
// path and is accepted, while a span=2 group stops untouched; both paths then
// accept the same members (group via suffix span=1) and settle all of them.
void group_gate_defers_same_members_to_suffix(Tracker& t) {
    constexpr std::size_t kHeld = 4;
    constexpr std::size_t kLater = 2;
    const auto run = [&](bool grouped) {
        ScriptedKernel kernel;
        kernel.store(ScriptedKernel::Mode::hold);
        auto fixture = make_fixture(t, (kHeld + kLater) * kPage);
        auto harness = make_scripted_harness(t, kHeld + kLater, 4, kernel);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        UringAsyncBackend& backend = *harness->backend;

        std::vector<std::vector<std::byte>> held_buffers(kHeld, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> held_offsets;
        for (std::size_t i = 0; i < kHeld; ++i)
            held_offsets.push_back(i * kPage);
        std::vector<ReadOp> held_ops = file_ops(*fixture, held_buffers, held_offsets);
        auto held = TestAccess::context_submit_ready_reads(ctx, held_ops);
        t.check(held.accepted == kHeld, "the held group is accepted");
        auto requests = requests_from(held);
        const auto cancel = requests.front().cancel();
        t.check(cancel.has_value() && cancel.value() == CancelDisposition::requested,
                "the held member reports a running cancellation");
        t.check(backend.pending_control_snapshot_for_test().authoritative_pending != 0,
                "the full SQ leaves a sticky control intent");

        std::vector<std::vector<std::byte>> later_buffers(kLater, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> later_offsets{kHeld * kPage, (kHeld + 1) * kPage};
        std::vector<ReadOp> later_ops = file_ops(*fixture, later_buffers, later_offsets);
        if (grouped) {
            auto group = TestAccess::context_submit_ready_reads(ctx, later_ops);
            t.check(group.accepted == 0, "a group at the control boundary accepts nothing");
            t.check(group.stop == UringAsyncBackend::ReadyBatchStop::control,
                    "the group reports the control boundary");
            t.check(group.untouched == kLater, "the whole group is reported untouched");
            for (std::size_t i = 0; i < kLater; ++i) {
                auto single = TestAccess::context_submit_ready_reads(
                    ctx, std::span<const ReadOp>(&later_ops[i], 1));
                t.check(single.accepted == 1 && single.stop == UringAsyncBackend::ReadyBatchStop::input_exhausted,
                        "a group-gate suffix member takes the immediate path");
                requests.push_back(std::move(single.requests[0]));
            }
        } else {
            for (std::size_t i = 0; i < kLater; ++i) {
                auto single = TestAccess::context_submit_ready_reads(
                    ctx, std::span<const ReadOp>(&later_ops[i], 1));
                t.check(single.accepted == 1 && single.stop == UringAsyncBackend::ReadyBatchStop::input_exhausted,
                        "the single path accepts the member in the same state");
                requests.push_back(std::move(single.requests[0]));
            }
        }
        t.check(requests.size() == kHeld + kLater,
                grouped ? "the group path accepts every member via its suffix"
                        : "the single path accepts every member directly");

        kernel.store(ScriptedKernel::Mode::consume_all);
        (void)ctx.poll();
        (void)ctx.poll();
        for (std::size_t i = 0; i < kHeld + kLater; ++i) {
            const auto cookie = backend.live_cookie_for_offset_for_test(i * kPage);
            if (i == 0) {
                if (cookie.has_value()) {
                    backend.inject_cqe_for_test(*cookie, -ECANCELED);
                    backend.inject_cqe_for_test(kControlTag | *cookie, 0);
                }
            } else if (cookie.has_value()) {
                backend.inject_cqe_for_test(*cookie, static_cast<int>(kPage));
            }
        }
        t.check(drive_context(ctx, requests), "every member of both paths settles");
        const auto cancelled = requests.front().take_result();
        t.check(cancelled.readiness == RequestReadiness::ready && !cancelled.result.has_value() &&
                    cancelled.result.error().code == IoError::Code::canceled,
                "the cancelled member is terminal in both paths");
        for (std::size_t i = 1; i < requests.size(); ++i)
            check_result(t, requests[i], kPage, "every non-cancelled member keeps its result");
        const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
        t.check(outcome.has_value() && outcome.value() == ShutdownOutcome::completed,
                "both paths settle");
    };
    run(false);
    run(true);
}

// A single submit paused before admission must observe a poison that the owner
// published while it was parked, and reject without accepting or binding.
void single_submit_after_poison_rejects(Tracker& t) {
    ScriptedKernel kernel;
    kernel.store(ScriptedKernel::Mode::hold);
    auto fixture = make_fixture(t, 3 * kPage);
    auto harness = make_scripted_harness(t, 4, 4, kernel);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> held_buffers(1, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> held_offsets{0};
    std::vector<ReadOp> held_ops = file_ops(*fixture, held_buffers, held_offsets);
    auto held = TestAccess::context_submit_ready_reads(ctx, held_ops);
    t.check(held.accepted == 1, "the held read is accepted before the poison");
    auto held_requests = requests_from(held);

    UringAsyncBackend::SubmitEntryPauseGate gate;
    backend.set_submit_entry_pause_gate(&gate);
    std::optional<sluice::Result<Request<std::size_t>>> parked;
    std::vector<std::vector<std::byte>> parked_buffers(1, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> parked_offsets{kPage};
    std::vector<ReadOp> parked_ops = file_ops(*fixture, parked_buffers, parked_offsets);
    std::thread submitter([&] { parked = ctx.submit_read(parked_ops[0]); });
    wait_gate_paused(gate);

    kernel.store(ScriptedKernel::Mode::partial_then_poison);
    kernel.calls.store(0, std::memory_order_relaxed);
    (void)backend.poll();
    t.check(backend.live_cookie_for_offset_for_test(0).has_value(),
            "the held route keeps its real completion path after the poison");
    backend.set_submit_entry_pause_gate(nullptr);
    release_gate(gate);
    submitter.join();
    t.check(parked.has_value() && !parked->has_value() &&
                parked->error().code == IoError::Code::backend_error,
            "the parked submit observes the poison and rejects before accept");
    t.check(TestAccess::context_core(ctx)->occupancy().accepted_live == 1,
            "the rejected submit leaves only the previously accepted member");
    auto poisoned_entry = TestAccess::context_submit_ready_reads(ctx, parked_ops);
    t.check(poisoned_entry.accepted == 0 &&
                poisoned_entry.stop == UringAsyncBackend::ReadyBatchStop::rejected &&
                poisoned_entry.error.code == IoError::Code::backend_error &&
                poisoned_entry.flush_attempts == 0,
            "a poisoned immediate entry rejects without attempting a flush");

    const auto visible = backend.live_cookie_for_offset_for_test(0);
    if (visible.has_value())
        backend.inject_cqe_for_test(*visible, static_cast<int>(kPage));
    t.check(drive_context(ctx, held_requests), "the held member reaches its terminal");
    check_result(t, held_requests[0], kPage, "the held member keeps its own result");
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    t.check(outcome.has_value() && outcome.value() == ShutdownOutcome::completed,
            "the poison transition settles");
}

// A control intent that already owns a slot stops the next context entry
// before any member is accepted, even when the SQ still looks usable.
void cancel_intent_stops_context_entry(Tracker& t) {
    constexpr std::size_t kHeld = 4;
    constexpr std::size_t kLater = 2;
    ScriptedKernel kernel;
    kernel.store(ScriptedKernel::Mode::hold);
    auto fixture = make_fixture(t, (kHeld + kLater) * kPage);
    auto harness = make_scripted_harness(t, kHeld + kLater, 4, kernel);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> held_buffers(kHeld, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> held_offsets;
    for (std::size_t i = 0; i < kHeld; ++i)
        held_offsets.push_back(i * kPage);
    std::vector<ReadOp> held_ops = file_ops(*fixture, held_buffers, held_offsets);
    auto held = TestAccess::context_submit_ready_reads(ctx, held_ops);
    t.check(held.accepted == kHeld, "the held group is accepted through the context entry");
    auto requests = requests_from(held);

    const auto cancel = requests.front().cancel();
    t.check(cancel.has_value() && cancel.value() == CancelDisposition::requested,
            "the running member reports a requested cancellation");
    t.check(backend.pending_control_snapshot_for_test().authoritative_pending != 0,
            "the full SQ leaves a sticky control intent");

    std::vector<std::vector<std::byte>> later_buffers(kLater, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> later_offsets{kHeld * kPage, (kHeld + 1) * kPage};
    std::vector<ReadOp> later_ops = file_ops(*fixture, later_buffers, later_offsets);
    const std::uint64_t flushes_before = backend.submit_flushes_for_test();
    auto stopped = TestAccess::context_submit_ready_reads(ctx, later_ops);
    t.check(stopped.accepted == 0, "an owed control intent accepts no batch member");
    t.check(stopped.stop == UringAsyncBackend::ReadyBatchStop::control,
            "the context entry reports the control boundary");
    t.check(stopped.untouched_begin == 0 && stopped.untouched == kLater,
            "the whole later group stays untouched at the control boundary");
    t.check(backend.submit_flushes_for_test() == flushes_before,
            "the control-stopped group adds no transport submit");

    kernel.store(ScriptedKernel::Mode::consume_all);
    (void)ctx.poll();
    (void)ctx.poll();
    for (std::size_t i = 0; i < kHeld; ++i) {
        const auto cookie = backend.live_cookie_for_offset_for_test(i * kPage);
        if (i == 0) {
            if (cookie.has_value()) {
                backend.inject_cqe_for_test(*cookie, -ECANCELED);
                backend.inject_cqe_for_test(kControlTag | *cookie, 0);
            }
        } else if (cookie.has_value()) {
            backend.inject_cqe_for_test(*cookie, static_cast<int>(kPage));
        }
    }
    t.check(drive_context(ctx, requests), "every held member reaches a terminal");
    const auto cancelled = requests.front().take_result();
    t.check(cancelled.readiness == RequestReadiness::ready && !cancelled.result.has_value() &&
                cancelled.result.error().code == IoError::Code::canceled,
            "the cancelled member reports cancellation, not a fabricated result");
    for (std::size_t i = 1; i < kHeld; ++i)
        check_result(t, requests[i], kPage, "each untouched member keeps its own result");
    const auto snapshot = backend.pending_control_snapshot_for_test();
    t.check(snapshot.live_routes == 0 && snapshot.authoritative_pending == 0,
            "control and route accounting retire with the cancelled member");
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    t.check(outcome.has_value() && outcome.value() == ShutdownOutcome::completed,
            "the control-stopped batch settles");
}

// The context entry applies the submit_read precheck before any backend
// reservation; a rejected member is attempted but neither accepted nor
// untouched, and the valid prefix is still admitted per member.
void context_entry_precheck_matches_submit_read(Tracker& t) {
    KernelOracle oracle;
    auto fixture = make_fixture(t, 4 * kPage);
    auto harness = make_real_harness(t, 6, 4, &oracle);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;

    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> offsets{0, kPage};
    std::vector<ReadOp> invalid = file_ops(*fixture, buffers, offsets);
    invalid[0].file = NativeFileRef(-1, sluice::FileAccess::read_only);
    auto rejected = TestAccess::context_submit_ready_reads(ctx, invalid);
    t.check(rejected.batch_backend, "the context entry resolves the uring backend");
    t.check(rejected.accepted == 0 && rejected.stop == UringAsyncBackend::ReadyBatchStop::rejected,
            "a closed file is rejected before any member is accepted");
    t.check(rejected.error.code == IoError::Code::invalid_state,
            "the context precheck keeps the submit_read error code");
    t.check(rejected.untouched_begin == 1 && rejected.untouched == 1,
            "the precheck-rejected member is attempted, not untouched");
    t.check(oracle.count == 0, "a precheck rejection reaches no kernel submit");
    t.check(TestAccess::context_core(ctx)->occupancy().accepted_live == 0,
            "a precheck rejection leaves no accepted slot");
    const auto direct = ctx.submit_read(invalid[0]);
    t.check(!direct.has_value() && direct.error().code == IoError::Code::invalid_state,
            "submit_read rejects the same member with the same code");
    t.check(oracle.count == 0, "the submit_read rejection also reaches no kernel submit");

    std::vector<ReadOp> mixed = file_ops(*fixture, buffers, offsets);
    mixed[1].file =
        NativeFileRef(mixed[1].file.fd, sluice::FileAccess::write_only);
    auto batch = TestAccess::context_submit_ready_reads(ctx, mixed);
    t.check(batch.accepted == 1, "the valid prefix before the precheck rejection is admitted");
    t.check(batch.stop == UringAsyncBackend::ReadyBatchStop::rejected,
            "the precheck rejection stops the batch at that member");
    t.check(batch.error.code == IoError::Code::invalid_argument,
            "an access-inconsistent read keeps invalid_argument");
    t.check(batch.untouched_begin == 2 && batch.untouched == 0,
            "the rejected member is accounted after the accepted prefix");
    t.check(oracle.count == 1 && oracle.calls[0].ready == 1,
            "only the accepted prefix emits one SQE");
    auto requests = requests_from(batch);
    t.check(requests.size() == 1, "only the accepted prefix materializes a request");
    t.check(drive_context(ctx, requests), "the accepted prefix terminates");
    check_result(t, requests[0], kPage, "the accepted prefix keeps its own result");
    t.check(TestAccess::context_core(ctx)->occupancy().public_bindings == 0,
            "consuming the accepted prefix leaves no binding");
}

// Capacity is reserved and accepted per member: the first member joins the
// accepted set, the second is rejected with would_block and is not part of the
// untouched suffix.
void capacity_boundary_is_per_request(Tracker& t) {
    auto fixture = make_fixture(t, 3 * kPage);
    auto harness = make_real_harness(t, 2, 2, nullptr);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;

    std::vector<std::vector<std::byte>> held(1, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> held_offsets{0};
    std::vector<ReadOp> held_ops = file_ops(*fixture, held, held_offsets);
    auto held_submitted = ctx.submit_read(held_ops[0]);
    t.check(held_submitted.has_value(), "the first capacity slot is occupied through the context");

    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> offsets{kPage, 2 * kPage};
    std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
    auto batch = TestAccess::context_submit_ready_reads(ctx, ops);
    t.check(batch.accepted == 1, "the batch accepts the one remaining slot");
    t.check(batch.stop == UringAsyncBackend::ReadyBatchStop::rejected,
            "capacity exhaustion stops at the authoritative reserve rejection");
    t.check(batch.error.code == IoError::Code::would_block,
            "capacity exhaustion keeps the would_block mapping");
    t.check(batch.untouched_begin == 2 && batch.untouched == 0,
            "the rejected member is attempted, not part of the untouched suffix");
    auto requests = requests_from(batch);
    t.check(requests.size() == 1, "only the accepted member materializes a request");
    std::vector<Request<std::size_t>> all;
    all.push_back(std::move(held_submitted).value());
    all.push_back(std::move(requests[0]));
    t.check(drive_context(ctx, all), "the held and accepted members terminate");
    check_result(t, all[0], kPage, "the held member keeps its result");
    check_result(t, all[1], kPage, "the accepted member keeps its result");
    t.check(TestAccess::context_core(ctx)->occupancy().accepted_live == 0,
            "the capacity boundary leaves no live accepted slot");
}

// Deterministic acceptance barrier: the owner is held at the settlement entry
// before admission closes, the producer is held after reserve and before
// accept. The close then wins the core's serialized admission check for that
// member: it is rejected with no effect, and the owner settles without any
// accepted work.
void acceptance_barrier_close_rejects_member(Tracker& t) {
    KernelOracle oracle;
    auto fixture = make_fixture(t, 4 * kPage);
    auto harness = make_real_harness(t, 4, 4, &oracle);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> offsets{kPage, 2 * kPage};
    std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);

    detail::ProgressSource::PauseGate settlement_gate;
    TestAccess::context_set_settlement_start_pause_gate(ctx, &settlement_gate);
    bool owner_failed = false;
    ShutdownOutcome owner_outcome = ShutdownOutcome::health_failed;
    std::thread owner([&] {
        const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
        if (outcome.has_value()) {
            owner_outcome = outcome.value();
        } else {
            owner_failed = true;
        }
    });
    wait_gate_paused(settlement_gate);
    t.check(true, "the owner is held at the settlement entry before admission closes");

    UringAsyncBackend::PreAcceptCommitPauseGate accept_gate;
    backend.set_pre_accept_commit_pause_gate(&accept_gate);
    std::optional<TestAccess::ReadyReadsBatch> batch_result;
    std::thread producer([&] {
        batch_result = TestAccess::context_submit_ready_reads(ctx, ops);
    });
    wait_gate_paused(accept_gate);
    t.check(TestAccess::context_core(ctx)->admission_open(),
            "the producer reserved a slot while admission was still open");

    release_gate(settlement_gate);
    t.check(wait_for_core_admission_closed(ctx),
            "the owner closes admission while the producer waits before accept");
    backend.set_pre_accept_commit_pause_gate(nullptr);
    release_gate(accept_gate);
    producer.join();
    owner.join();
    t.check(!owner_failed && owner_outcome == ShutdownOutcome::completed,
            "the owner settles after rejecting the racing member");
    t.check(batch_result.has_value(), "the producer observed a batch report");
    if (batch_result.has_value()) {
        t.check(batch_result->accepted == 0,
                "the racing member is not accepted when close wins the serialized check");
        t.check(batch_result->stop == UringAsyncBackend::ReadyBatchStop::rejected,
                "the close race reports an authoritative rejection");
        t.check(batch_result->error.code == IoError::Code::invalid_state,
                "the admission-close rejection maps to invalid_state");
        t.check(batch_result->untouched_begin == 1 && batch_result->untouched == 1,
                "the rejected member is attempted and not counted as untouched");
        t.check(!batch_result->requests[0].valid(),
                "the rejected member materializes no request");
    }
    t.check(ctx.execution_closed(), "the owner retired the execution");
    const detail::CoreOccupancy occupancy = TestAccess::context_core(ctx)->occupancy();
    t.check(occupancy.accepted_live == 0 && occupancy.public_bindings == 0 &&
                occupancy.outstanding == 0,
            "the rejected race leaves no accepted or public binding");

    const unsigned submits_before = oracle.count;
    auto after = TestAccess::context_submit_ready_reads(ctx, ops);
    t.check(after.accepted == 0 &&
                after.stop == UringAsyncBackend::ReadyBatchStop::rejected &&
                after.error.code == IoError::Code::invalid_state,
            "a context entry after execution closed reports invalid_state, not backend_error");
    t.check(oracle.count == submits_before,
            "a post-close context entry performs no kernel submit");
    TestAccess::context_set_settlement_start_pause_gate(ctx, nullptr);
}

// Deterministic accepted-before-close race: the owner is held at the settlement
// entry, the producer admits and dispatches the whole group, then the owner
// closes admission and drains the finite accepted set.
void shutdown_owner_entry_settles_accepted_batch(Tracker& t) {
    auto harness = make_real_harness(t, 4, 4, nullptr);
    if (!harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;

    int fds[2];
    if (::pipe(fds) != 0) {
        t.check(false, "pipe fixture is created");
        return;
    }
    std::vector<std::vector<std::byte>> buffers(2, std::vector<std::byte>(kPage));
    std::vector<ReadOp> ops = pipe_ops(fds[0], buffers, 2);

    detail::ProgressSource::PauseGate settlement_gate;
    TestAccess::context_set_settlement_start_pause_gate(ctx, &settlement_gate);
    bool owner_failed = false;
    ShutdownOutcome owner_outcome = ShutdownOutcome::health_failed;
    std::thread owner([&] {
        const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
        if (outcome.has_value()) {
            owner_outcome = outcome.value();
        } else {
            owner_failed = true;
        }
    });
    wait_gate_paused(settlement_gate);

    auto batch = TestAccess::context_submit_ready_reads(ctx, ops);
    t.check(batch.accepted == 2 && batch.untouched == 0,
            "the group is accepted while the owner is held before admission closes");
    t.check(batch.flush_attempts == 1, "the accepted group flushes once");
    t.check(!batch.residue_advertised, "the accepted group leaves no transport residue");
    const detail::CoreOccupancy admitted = TestAccess::context_core(ctx)->occupancy();
    t.check(admitted.accepted_live == 2 && admitted.public_bindings == 2,
            "both members are in the finite accepted set before the close");
    auto requests = requests_from(batch);

    release_gate(settlement_gate);
    t.check(wait_for_core_admission_closed(ctx),
            "the owner closes admission after the group was accepted");
    const std::string payload(2 * kPage, 's');
    t.check(::write(fds[1], payload.data(), payload.size()) ==
                static_cast<ssize_t>(payload.size()),
            "the accepted pipe reads are released during settlement");
    owner.join();
    t.check(!owner_failed && owner_outcome == ShutdownOutcome::completed,
            "the owner drains the accepted batch to completion");
    t.check(ctx.execution_closed(), "execution is closed after the drain");
    t.check(all_ready(requests), "every accepted member is published by the drain");
    for (Request<std::size_t>& request : requests)
        check_result(t, request, kPage, "every drained member keeps its own result");
    for (std::size_t i = 0; i < buffers.size(); ++i)
        t.check(std::memcmp(buffers[i].data(), payload.data(), kPage) == 0,
                "every drained member lands its own bytes");
    const detail::CoreOccupancy settled = TestAccess::context_core(ctx)->occupancy();
    t.check(settled.accepted_live == 0 && settled.public_bindings == 0 &&
                settled.outstanding == 0 && settled.execution_refs == 0,
            "the drained accepted set is fully settled");
    TestAccess::context_set_settlement_start_pause_gate(ctx, nullptr);
    ::close(fds[0]);
    ::close(fds[1]);
}

// Partial/zero/retryable transport results through the context entry: the
// consumed prefix belongs to the kernel, the remainder stays in the ledger,
// the residue is advertised, and the next pass submits exactly the remainder
// (no consumed key is re-emitted).
void partial_submit_error_modes(Tracker& t) {
    struct ModeCase {
        ScriptedKernel::Mode mode;
        unsigned consumed;
        const char* label;
    };
    const ModeCase cases[] = {
        {ScriptedKernel::Mode::consume_all, 8, "consume_all"},
        {ScriptedKernel::Mode::hold, 0, "zero_consumed"},
        {ScriptedKernel::Mode::partial_first, 1, "partial_one"},
        {ScriptedKernel::Mode::partial_then_eagain, 2, "partial_eagain"},
        {ScriptedKernel::Mode::partial_then_eintr, 2, "partial_eintr"},
    };
    for (const ModeCase& test_case : cases) {
        constexpr std::size_t kCount = 8;
        ScriptedKernel kernel;
        kernel.store(test_case.mode);
        auto fixture = make_fixture(t, kCount * kPage);
        auto harness = make_scripted_harness(t, kCount, static_cast<unsigned>(kCount), kernel);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        UringAsyncBackend& backend = *harness->backend;

        std::vector<std::vector<std::byte>> buffers(kCount, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> offsets;
        for (std::size_t i = 0; i < kCount; ++i)
            offsets.push_back(i * kPage);
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        auto batch = TestAccess::context_submit_ready_reads(ctx, ops);
        t.check(batch.accepted == kCount, test_case.label);
        t.check(batch.flush_attempts == 1, test_case.label);
        t.check(kernel.first_ready.load() == kCount,
                "the first flush carries every accepted SQE");
        const bool retryable = test_case.mode != ScriptedKernel::Mode::consume_all;
        t.check(batch.residue_advertised == retryable,
                "only a transport left outside the kernel advertises residue");
        t.check(backend.transport_ledger_size_for_test() == kCount - test_case.consumed,
                "the ledger keeps exactly the unconsumed suffix");
        const std::uint64_t cookie_before = backend.peek_next_cookie_for_test();
        const std::size_t routes_before = backend.live_cookies_for_test();

        if (test_case.mode == ScriptedKernel::Mode::hold)
            kernel.store(ScriptedKernel::Mode::consume_all);
        const unsigned submits_before = kernel.calls.load(std::memory_order_relaxed);
        (void)ctx.poll();
        const unsigned expected_second = retryable ? kCount - test_case.consumed : 0;
        t.check(kernel.second_ready.load() == expected_second,
                "the retry submits exactly the remaining prefix");
        t.check(static_cast<unsigned>(kernel.calls.load(std::memory_order_relaxed)) ==
                    submits_before + (retryable ? 1u : 0u),
                "a fully consumed flush schedules no redundant transport attempt");
        if (retryable) {
            t.check(kernel.first_ready.load() - kernel.second_ready.load() ==
                        test_case.consumed,
                    "the consumed prefix is exactly the shrink of the pending SQ");
            t.check(kernel.second_first_offset.load() ==
                        static_cast<long long>(test_case.consumed * kPage),
                    "the retry's first pending SQE is the first unconsumed member");
        }
        t.check(backend.peek_next_cookie_for_test() == cookie_before &&
                    backend.live_cookies_for_test() == routes_before,
                "the retry re-presents pending SQEs without preparing new ones");
        t.check(backend.transport_ledger_size_for_test() == 0,
                "the retry leaves no transport ledger entry");
        t.check(backend.dispatch_size_for_test() == 0,
                "the retry leaves no dispatch entry");

        auto requests = requests_from(batch);
        for (std::size_t i = 0; i < kCount; ++i) {
            const auto cookie = backend.live_cookie_for_offset_for_test(i * kPage);
            t.check(cookie.has_value(), "every accepted member keeps its own route");
            if (cookie.has_value())
                backend.inject_cqe_for_test(*cookie, static_cast<int>(kPage));
        }
        t.check(drive_context(ctx, requests), "every partially submitted member terminates");
        for (Request<std::size_t>& request : requests)
            check_result(t, request, kPage, "every member keeps its own 4 KiB result");
        const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
        t.check(outcome.has_value() && outcome.value() == ShutdownOutcome::completed,
                "the partial-submit mode settles");
    }
}

// A poisoned multi-member flush is reported as a recovery boundary with the
// backend error, not as a normal input-exhausted completion. The kernel-visible
// prefix keeps its real completion; only provably invisible members are retired
// as physical failures.
void poison_report_is_recovery(Tracker& t) {
    constexpr std::size_t kCount = 8;
    ScriptedKernel kernel;
    kernel.store(ScriptedKernel::Mode::partial_then_poison);
    auto fixture = make_fixture(t, kCount * kPage);
    auto harness = make_scripted_harness(t, kCount, static_cast<unsigned>(kCount), kernel);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> buffers(kCount, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> offsets;
    for (std::size_t i = 0; i < kCount; ++i)
        offsets.push_back(i * kPage);
    std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
    auto batch = TestAccess::context_submit_ready_reads(ctx, ops);
    t.check(batch.accepted == kCount, "every member is accepted before the poisoned flush");
    t.check(batch.stop == UringAsyncBackend::ReadyBatchStop::recovery,
            "a poisoned flush reports the recovery boundary, not input_exhausted");
    t.check(batch.error.code == IoError::Code::backend_error,
            "the poisoned flush carries the backend error");
    t.check(batch.residue_advertised, "the poisoned flush still advertises its residue");
    t.check(batch.flush_attempts == 1, "the poisoned flush is counted");
    t.check(kernel.first_ready.load() == kCount && kernel.first_rc.load() == -EIO,
            "the kernel saw the whole group and reported the hard failure");
    t.check(backend.live_cookie_for_offset_for_test(0).has_value(),
            "the kernel-visible member keeps its route");
    for (std::size_t i = 1; i < kCount; ++i)
        t.check(!backend.live_cookie_for_offset_for_test(i * kPage).has_value(),
                "an invisible member has no route to fabricate a completion");

    const auto visible = backend.live_cookie_for_offset_for_test(0);
    if (visible.has_value())
        backend.inject_cqe_for_test(*visible, static_cast<int>(kPage));
    auto requests = requests_from(batch);
    t.check(drive_context(ctx, requests), "every poisoned-batch member reaches a terminal");
    check_result(t, requests[0], kPage,
                 "the kernel-visible member keeps its real 4 KiB result");
    for (std::size_t i = 1; i < kCount; ++i) {
        const auto failed = requests[i].take_result();
        t.check(failed.readiness == RequestReadiness::ready && !failed.result.has_value() &&
                    failed.result.error().code == IoError::Code::backend_error,
                "an invisible member reports the physical failure, never success");
    }
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    t.check(outcome.has_value() && outcome.value() == ShutdownOutcome::completed,
            "the poisoned batch settles");
}

// A partially free SQ with unproven transport residue is not ready transport:
// the next context entry accepts nothing and returns the whole group untouched,
// even though the ring still has room.
void sq_partial_free_transport_residue_stops_batch(Tracker& t) {
    constexpr std::size_t kHeld = 6;
    constexpr std::size_t kLater = 2;
    ScriptedKernel kernel;
    kernel.store(ScriptedKernel::Mode::hold);
    auto fixture = make_fixture(t, (kHeld + kLater) * kPage);
    auto harness = make_scripted_harness(t, kHeld + kLater, 8, kernel);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> held_buffers(kHeld, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> held_offsets;
    for (std::size_t i = 0; i < kHeld; ++i)
        held_offsets.push_back(i * kPage);
    std::vector<ReadOp> held_ops = file_ops(*fixture, held_buffers, held_offsets);
    auto held = TestAccess::context_submit_ready_reads(ctx, held_ops);
    t.check(held.accepted == kHeld && held.residue_advertised,
            "the first group leaves unproven transport");
    t.check(backend.transport_ledger_size_for_test() == kHeld,
            "the whole first group stays in the ledger");
    t.check(backend.sq_ready_for_test() == kHeld,
            "the ring still has free SQ slots, so this is not an SQ-full stop");

    std::vector<std::vector<std::byte>> later_buffers(kLater, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> later_offsets{kHeld * kPage, (kHeld + 1) * kPage};
    std::vector<ReadOp> later_ops = file_ops(*fixture, later_buffers, later_offsets);
    const std::uint64_t flushes_before = backend.submit_flushes_for_test();
    auto later = TestAccess::context_submit_ready_reads(ctx, later_ops);
    t.check(later.accepted == 0, "a batch at transport residue accepts nothing");
    t.check(later.stop == UringAsyncBackend::ReadyBatchStop::recovery,
            "the transport-residue boundary is reported as recovery");
    t.check(later.error.code == IoError::Code::would_block,
            "a non-fatal transport residue reports the retryable error, not eof");
    t.check(later.untouched_begin == 0 && later.untouched == kLater,
            "the whole later group stays untouched");
    t.check(later.flush_attempts == 0, "the stopped batch attempts no flush");
    t.check(backend.submit_flushes_for_test() == flushes_before,
            "the stopped batch adds no transport submit");
    t.check(backend.transport_ledger_size_for_test() == kHeld,
            "the rejected batch leaves the transport ledger unchanged");

    auto requests = requests_from(held);
    auto later_requests = requests_from(later);
    kernel.store(ScriptedKernel::Mode::consume_all);
    (void)ctx.poll();
    for (std::size_t i = 0; i < kHeld + kLater; ++i) {
        const auto cookie = backend.live_cookie_for_offset_for_test(i * kPage);
        if (cookie.has_value())
            backend.inject_cqe_for_test(*cookie, static_cast<int>(kPage));
    }
    t.check(drive_context(ctx, requests), "every held member terminates");
    if (!later_requests.empty())
        t.check(drive_context(ctx, later_requests), "a wrongly accepted later member terminates");
    for (Request<std::size_t>& request : requests)
        check_result(t, request, kPage, "every held member keeps its result");
    for (Request<std::size_t>& request : later_requests)
        (void)request.take_result();
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    t.check(outcome.has_value() && outcome.value() == ShutdownOutcome::completed,
            "the partial-SQ residue boundary settles");
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
    if (selected("context_precheck")) context_entry_precheck_matches_submit_read(tracker);
    if (selected("context_cap_suffix")) context_entry_cap_reports_suffix(tracker);
    if (selected("inert_context")) inert_context_entry_rejects(tracker);
    if (selected("capacity_boundary")) capacity_boundary_is_per_request(tracker);
    if (selected("acceptance_barrier")) acceptance_barrier_close_rejects_member(tracker);
    if (selected("mixed_acceptance")) mixed_acceptance_before_close(tracker);
    if (selected("shutdown_accepted"))
        shutdown_owner_entry_settles_accepted_batch(tracker);
    if (selected("partial_modes")) partial_submit_error_modes(tracker);
    if (selected("poison_report")) poison_report_is_recovery(tracker);
    if (selected("control_stop")) cancel_intent_stops_context_entry(tracker);
    if (selected("gate_defer")) group_gate_defers_same_members_to_suffix(tracker);
    if (selected("post_poison_single")) single_submit_after_poison_rejects(tracker);
    if (selected("sq_partial_residue"))
        sq_partial_free_transport_residue_stops_batch(tracker);
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
