// #595 R4 ready-reuse / state-aware adapter oracles. Each scenario names the
// invariant or failure mode it distinguishes for the R (cap re-entry) and S
// (state-aware budget) arms and their combinations with A0/A1/B. Seams reuse
// the R3 deterministic framework: pipe holds, scripted kernel submits,
// injected CQEs and the kernel oracle; no sleep is concurrency evidence.

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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

#ifndef SLUICE_HAS_LIBURING
#error "the ready-reuse correctness oracle must receive SLUICE_HAS_LIBURING"
#endif
#ifndef SLUICE_ASYNC_INTERNAL_TESTING
#error "the ready-reuse correctness oracle must receive SLUICE_ASYNC_INTERNAL_TESTING"
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
    char path[] = "/tmp/sluice_595_r4_reuse_XXXXXX";
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
    std::array<EnterCall, 1024> calls{};
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
                static_cast<long long>(ring->sq.sqes[index].off), std::memory_order_relaxed);
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
        owned = std::make_unique<UringAsyncBackend>(UringConfig{capacity, depth}, hooks);
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
requests_from(TestAccess::ReadyReadsBatch& batch) {
    std::vector<Request<std::size_t>> requests;
    for (std::size_t i = 0; i < batch.accepted; ++i)
        requests.push_back(std::move(batch.requests[i]));
    return requests;
}

bool all_ready(const std::vector<Request<std::size_t>>& requests) {
    for (const Request<std::size_t>& request : requests) {
        if (!request.ready())
            return false;
    }
    return true;
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

bool wait_for_cq_overflow(UringAsyncBackend& backend) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!backend.cq_overflow_for_test()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

struct RequestKeyOrder {
    bool operator()(const sluice::async::detail::RequestKey& a,
                    const sluice::async::detail::RequestKey& b) const noexcept {
        if (a.context.value != b.context.value)
            return a.context.value < b.context.value;
        if (a.slot.value != b.slot.value)
            return a.slot.value < b.slot.value;
        return a.generation.value < b.generation.value;
    }
};

using KeySet = std::set<sluice::async::detail::RequestKey, RequestKeyOrder>;

sluice::async::detail::RequestKey id_key(const RequestId& id) {
    return TestAccess::request_key_of(id);
}

bool ids_all_distinct(const std::vector<Request<std::size_t>>& requests) {
    KeySet seen;
    for (const Request<std::size_t>& request : requests) {
        if (!seen.insert(id_key(request.id())).second)
            return false;
    }
    return true;
}

using Arm = bench::r3::Arm;

constexpr std::array<Arm, 10> kAllArms{
    Arm::a0,     Arm::a1,    Arm::b,           Arm::r,          Arm::s,
    Arm::s_no_inflight, Arm::s_no_pressure, Arm::q2, Arm::q8, Arm::shadow};

struct ArmShape {
    std::size_t presentations = 0;
    std::size_t cap_reentries = 0;
    std::size_t chunk_reentries = 0;
    std::size_t suffix_singles = 0;
    std::size_t enters = 0;
    std::size_t sqes = 0;
};

// Presentation/enter shape of one arm on an idle pipeline with an empty SQ:
// the adapter's documented decomposition of an N-op ready span. S/SNP grow
// from quantum 2 to 8 as the accepted set raises outstanding(); Q2/Q8 pin
// their quantum; R/SHADOW present the full remaining span and re-enter on
// cap stops; B stops at the cap and defers the suffix to singles.
ArmShape idle_shape(Arm arm, std::size_t n) {
    ArmShape shape;
    shape.sqes = n;
    const std::size_t caps = (n + 7) / 8;
    switch (arm) {
    case Arm::a0:
    case Arm::a1:
        shape.enters = n;
        return shape;
    case Arm::b:
        shape.presentations = 1;
        shape.suffix_singles = n > 8 ? n - 8 : 0;
        shape.enters = 1 + shape.suffix_singles;
        return shape;
    case Arm::r:
    case Arm::shadow:
        shape.presentations = caps;
        shape.cap_reentries = caps - 1;
        shape.enters = caps;
        return shape;
    case Arm::s:
    case Arm::s_no_pressure:
        if (n <= 2) {
            shape.presentations = 1;
            shape.enters = 1;
        } else {
            const std::size_t rest = n - 2;
            const std::size_t chunks = (rest + 7) / 8;
            shape.presentations = 1 + chunks;
            shape.chunk_reentries = chunks;
            shape.enters = shape.presentations;
        }
        return shape;
    case Arm::s_no_inflight:
    case Arm::q8:
        shape.presentations = caps;
        shape.chunk_reentries = caps - 1;
        shape.enters = caps;
        return shape;
    case Arm::q2:
        shape.presentations = (n + 1) / 2;
        shape.chunk_reentries = shape.presentations - 1;
        shape.enters = shape.presentations;
        return shape;
    }
    return shape;
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

// An empty ready span is inert on every arm: no acceptance, no presentation,
// no kernel enter, no error.
void empty_span_is_inert_on_every_arm(Tracker& t) {
    for (const Arm arm : kAllArms) {
        KernelOracle oracle;
        auto harness = make_real_harness(t, 8, 4, &oracle);
        if (!harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        bench::r3::Adapter adapter(ctx, arm);
        bench::r3::Adapter::TraceSink trace;
        adapter.set_trace_sink(&trace);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(), requests);
        t.check(!outcome.error.has_value(), "an empty span reports no error");
        t.check(requests.empty(), "an empty span accepts nothing");
        t.check(outcome.counters.r4_batch_presentations == 0 &&
                    outcome.counters.suffix_single_calls == 0 &&
                    outcome.counters.entry_calls == 0 && outcome.counters.accepted == 0,
                "an empty span presents nothing");
        t.check(trace.size() == 0, "an empty span forms no decision");
        t.check(oracle.count == 0, "an empty span reaches no kernel submit");
    }
}

// A single ready read keeps the frozen immediate shape on every arm: exactly
// one accepted request, one kernel SQE, its own result and bytes.
void single_read_settles_with_own_identity_on_every_arm(Tracker& t) {
    for (const Arm arm : kAllArms) {
        KernelOracle oracle;
        auto fixture = make_fixture(t, kPage);
        auto harness = make_real_harness(t, 4, 4, &oracle);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        std::vector<std::vector<std::byte>> buffers(1, std::vector<std::byte>(kPage));
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, {0});
        bench::r3::Adapter adapter(ctx, arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(!outcome.error.has_value(), "a single read is accepted without error");
        t.check(requests.size() == 1, "a single read yields exactly one request");
        t.check(oracle.count == 1 && oracle.total_sqes == 1,
                "a single read costs exactly one kernel enter with one SQE");
        if (arm == Arm::a0 || arm == Arm::a1) {
            t.check(outcome.counters.r4_batch_presentations == 0,
                    "A0/A1 keep the frozen per-op path for a single read");
        } else {
            t.check(outcome.counters.r4_batch_presentations == 1 &&
                        outcome.counters.suffix_single_calls == 0,
                    "a batching arm presents the single member exactly once");
        }
        t.check(drive_context(ctx, requests), "the single read terminates");
        check_result(t, requests[0], kPage, "the single read reports its own 4 KiB result");
        check_bytes(t, buffers[0], *fixture, 0, "the single read lands its own bytes");
    }
}

// For spans 2/8/9/16/64 every arm accepts exactly N members with N distinct
// request ids and exactly N kernel SQEs (no member is accepted or submitted
// twice), and each arm keeps its documented presentation shape including cap
// re-entries and chunk re-entries. All members settle with their own bytes
// and leave no transport residue. This is the named target of
// SLUICE_R4_ADAPTER_MUTANT_DUPLICATE_ACCEPT.
void span_shapes_and_reentry_counts_are_exact(Tracker& t) {
    const std::size_t spans[] = {2, 8, 9, 16, 64};
    for (const std::size_t n : spans) {
        for (const Arm arm : kAllArms) {
            KernelOracle oracle;
            auto fixture = make_fixture(t, n * kPage);
            auto harness = make_real_harness(t, n + 8, 8, &oracle);
            if (!fixture.has_value() || !harness.has_value())
                return;
            AsyncIoContext& ctx = *harness->ctx;
            UringAsyncBackend& backend = *harness->backend;
            std::vector<std::vector<std::byte>> buffers(n, std::vector<std::byte>(kPage));
            std::vector<std::uint64_t> offsets;
            for (std::size_t i = 0; i < n; ++i)
                offsets.push_back(i * kPage);
            std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
            bench::r3::Adapter adapter(ctx, arm);
            bench::r3::Adapter::TraceSink trace;
            adapter.set_trace_sink(&trace);
            std::vector<Request<std::size_t>> requests;
            const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
            const ArmShape want = idle_shape(arm, n);
            t.check(!outcome.error.has_value(), "the whole span is accepted without error");
            t.check(requests.size() == n, "exactly one request per member is accepted");
            t.check(ids_all_distinct(requests), "every member keeps a distinct request id");
            t.check(oracle.total_sqes == n, "exactly one kernel SQE per member");
            t.check(oracle.count == want.enters, "the arm costs its documented kernel enters");
            t.check(outcome.counters.r4_batch_presentations == want.presentations,
                    "the arm makes its documented presentations");
            t.check(outcome.counters.r4_cap_reentries == want.cap_reentries,
                    "cap re-entries are exact");
            t.check(outcome.counters.r4_chunk_reentries == want.chunk_reentries,
                    "chunk re-entries are exact");
            t.check(outcome.counters.suffix_single_calls == want.suffix_singles,
                    "suffix singles are exact");
            if (arm != Arm::a0) {
                t.check(outcome.counters.accepted == n,
                        "the accepted accounting covers every member");
            }
            t.check(drive_context(ctx, requests), "every member terminates");
            for (std::size_t i = 0; i < n; ++i)
                check_result(t, requests[i], kPage, "every member keeps its own result");
            for (std::size_t i = 0; i < n; ++i)
                check_bytes(t, buffers[i], *fixture, i * kPage, "every member lands its own bytes");
            t.check(backend.transport_ledger_size_for_test() == 0 &&
                        backend.dispatch_size_for_test() == 0,
                    "the consumed span leaves no dispatch or ledger residue");
            t.check(trace.size() == want.presentations,
                    "every presentation logs exactly one decision");
        }
    }
}

// A cap stop that also leaves unproven transport residue: R re-enters once
// and defers the stuck remainder to the frozen single path, while the
// state-aware arms exit to authority at the residue boundary instead of
// presenting another group span. No member is lost or duplicated; the owner
// pass retries exactly the residue. This is the named target of
// SLUICE_R4_ADAPTER_MUTANT_BOUNDARY_IGNORED.
void cap_stop_with_transport_residue_defers_to_frozen_authority(Tracker& t) {
    struct Variant {
        Arm arm;
        std::size_t presentations;
        std::size_t cap_reentries;
        std::size_t authority_exits;
        std::size_t suffix_singles;
    };
    const Variant variants[] = {
        {Arm::r, 2, 1, 0, 8},
        {Arm::b, 1, 0, 0, 8},
        {Arm::shadow, 2, 1, 0, 8},
        {Arm::s_no_inflight, 1, 0, 1, 8},
        {Arm::q8, 1, 0, 1, 8},
        {Arm::s, 1, 0, 1, 14},
        {Arm::s_no_pressure, 1, 0, 1, 14},
        {Arm::q2, 1, 0, 1, 14},
    };
    for (const Variant& variant : variants) {
        ScriptedKernel kernel;
        kernel.store(ScriptedKernel::Mode::hold);
        auto fixture = make_fixture(t, 16 * kPage);
        auto harness = make_scripted_harness(t, 24, 8, kernel);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        UringAsyncBackend& backend = *harness->backend;
        std::vector<std::vector<std::byte>> buffers(16, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> offsets;
        for (std::size_t i = 0; i < 16; ++i)
            offsets.push_back(i * kPage);
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        bench::r3::Adapter adapter(ctx, variant.arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(!outcome.error.has_value(), "the residue boundary still accepts every member");
        t.check(requests.size() == 16, "no member is lost at the residue boundary");
        t.check(ids_all_distinct(requests), "residue-boundary members keep distinct ids");
        t.check(outcome.counters.r4_batch_presentations == variant.presentations,
                "presentations at the residue boundary are exact");
        t.check(outcome.counters.r4_cap_reentries == variant.cap_reentries,
                "cap re-entries at the residue boundary are exact");
        t.check(outcome.counters.r4_authority_exits == variant.authority_exits,
                "the state-aware arm exits to authority at the residue boundary");
        t.check(outcome.counters.suffix_single_calls == variant.suffix_singles,
                "the deferred remainder takes the frozen single path");
        t.check(backend.transport_ledger_size_for_test() != 0,
                "the boundary leaves a real transport residue");
        t.check(backend.transport_ledger_size_for_test() +
                        backend.dispatch_size_for_test() ==
                    16,
                "every accepted member is either kernel-pending or queued");
        kernel.store(ScriptedKernel::Mode::consume_all);
        (void)ctx.poll();
        t.check(backend.transport_ledger_size_for_test() == 0,
                "the owner pass retries the whole residue");
        for (std::size_t i = 0; i < 16; ++i) {
            const auto cookie = backend.live_cookie_for_offset_for_test(i * kPage);
            if (cookie.has_value())
                backend.inject_cqe_for_test(*cookie, static_cast<int>(kPage));
        }
        t.check(drive_context(ctx, requests), "every member terminates after the residue retry");
        for (std::size_t i = 0; i < 16; ++i)
            check_result(t, requests[i], kPage, "every member keeps its own result");
        t.check(backend.transport_ledger_size_for_test() == 0 &&
                    backend.dispatch_size_for_test() == 0,
                "the residue retry drains ledger and dispatch");
    }
}

// An SQ that is already full with unproven transport accepts nothing at the
// group gate: the adapter presents once, does not loop on the zero-accept
// stop, defers every member to the frozen single path, and the whole set
// still settles. This is the named target of
// SLUICE_R4_ADAPTER_MUTANT_ZERO_PROGRESS_CONTINUE.
void prefilled_sq_full_residue_stops_group_without_spinning(Tracker& t) {
    for (const Arm arm : {Arm::s, Arm::b, Arm::q2, Arm::r}) {
        ScriptedKernel kernel;
        kernel.store(ScriptedKernel::Mode::hold);
        auto fixture = make_fixture(t, 12 * kPage);
        auto harness = make_scripted_harness(t, 16, 4, kernel);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        UringAsyncBackend& backend = *harness->backend;

        std::vector<std::vector<std::byte>> held_buffers(4, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> held_offsets{0, kPage, 2 * kPage, 3 * kPage};
        std::vector<ReadOp> held_ops = file_ops(*fixture, held_buffers, held_offsets);
        auto held = TestAccess::context_submit_ready_reads(ctx, held_ops);
        t.check(held.accepted == 4 && held.residue_advertised,
                "the held prefill fills the SQ with unproven transport");
        std::vector<Request<std::size_t>> held_requests = requests_from(held);

        std::vector<std::vector<std::byte>> buffers(8, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> offsets;
        for (std::size_t i = 0; i < 8; ++i)
            offsets.push_back((4 + i) * kPage);
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        bench::r3::Adapter adapter(ctx, arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(!outcome.error.has_value(), "the SQ-full boundary still accepts every member");
        t.check(requests.size() == 8, "every member is accepted through the single path");
        t.check(outcome.counters.r4_batch_presentations == 1,
                "the zero-accept stop is presented exactly once (no spin)");
        t.check(outcome.counters.r4_authority_exits == 0,
                "the arm returns without re-deciding past the stuck gate");
        t.check(outcome.counters.r4_cap_reentries == 0 && outcome.counters.r4_chunk_reentries == 0,
                "no re-entry happens without progress");
        t.check(outcome.counters.suffix_single_calls == 8,
                "the whole span defers to the frozen single path");

        kernel.store(ScriptedKernel::Mode::consume_all);
        (void)ctx.poll();
        for (std::size_t i = 0; i < 12; ++i) {
            const auto cookie = backend.live_cookie_for_offset_for_test(i * kPage);
            if (cookie.has_value())
                backend.inject_cqe_for_test(*cookie, static_cast<int>(kPage));
        }
        std::vector<Request<std::size_t>> all = std::move(held_requests);
        for (Request<std::size_t>& request : requests)
            all.push_back(std::move(request));
        t.check(drive_context(ctx, all), "held and deferred members all terminate");
        for (std::size_t i = 0; i < all.size(); ++i)
            check_result(t, all[i], kPage, "every held or deferred member keeps its result");
    }
}

// A flush the kernel consumes only partially: the consumed prefix belongs to
// the kernel, the residue is advertised and stays queued after the adapter
// returns, and the owner pass re-submits exactly the unconsumed SQEs in
// order (never a consumed key). Member identity survives the retry. The
// spans are chosen so no suffix single runs: the retry is attributable to
// the owner pass alone.
void partial_flush_residue_retry_preserves_member_identity(Tracker& t) {
    struct Variant {
        Arm arm;
        ScriptedKernel::Mode mode;
        std::size_t span;
        int first_rc;
        std::size_t first_consumed;
    };
    const Variant variants[] = {
        {Arm::r, ScriptedKernel::Mode::partial_then_eagain, 8, -EAGAIN, 2},
        {Arm::b, ScriptedKernel::Mode::partial_then_eagain, 8, -EAGAIN, 2},
        {Arm::s, ScriptedKernel::Mode::partial_first, 2, 1, 1},
    };
    for (const Variant& variant : variants) {
        ScriptedKernel kernel;
        kernel.store(variant.mode);
        auto fixture = make_fixture(t, variant.span * kPage);
        auto harness = make_scripted_harness(t, variant.span + 8, 8, kernel);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        UringAsyncBackend& backend = *harness->backend;
        std::vector<std::vector<std::byte>> buffers(variant.span,
                                                    std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> offsets;
        for (std::size_t i = 0; i < variant.span; ++i)
            offsets.push_back(i * kPage);
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        bench::r3::Adapter adapter(ctx, variant.arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(!outcome.error.has_value(), "the partial flush still accepts every member");
        t.check(requests.size() == variant.span && ids_all_distinct(requests),
                "partial transport keeps one distinct request per member");
        t.check(outcome.counters.suffix_single_calls == 0,
                "the partial-flush span needs no suffix singles");
        t.check(kernel.first_rc.load() == variant.first_rc,
                "the scripted kernel reported the partial consumption");
        t.check(backend.transport_ledger_size_for_test() == variant.span - variant.first_consumed,
                "exactly the unconsumed SQEs stay in the transport ledger");
        kernel.store(ScriptedKernel::Mode::consume_all);
        (void)ctx.poll();
        t.check(backend.transport_ledger_size_for_test() == 0 &&
                    backend.dispatch_size_for_test() == 0,
                "the owner pass retries the whole residue");
        t.check(kernel.second_ready.load() == variant.span - variant.first_consumed &&
                    kernel.second_rc.load() ==
                        static_cast<int>(kernel.second_ready.load()),
                "the retry submits exactly the pending set");
        t.check(kernel.second_first_offset.load() ==
                    static_cast<long long>(variant.first_consumed * kPage),
                "the retry restarts at the first unconsumed member");
        for (std::size_t i = 0; i < variant.span; ++i) {
            const auto cookie = backend.live_cookie_for_offset_for_test(i * kPage);
            if (cookie.has_value())
                backend.inject_cqe_for_test(*cookie, static_cast<int>(kPage));
        }
        t.check(drive_context(ctx, requests), "every member terminates after the retry");
        for (std::size_t i = 0; i < variant.span; ++i)
            check_result(t, requests[i], kPage, "every member keeps its own result");
    }
}

// A precheck-rejected member in the middle of the span: the accepted prefix
// before it settles with its own results, the rejected member itself is
// attempted but never accepted, and the suffix after it is never submitted to
// the kernel through any arm.
void middle_member_rejection_settles_prefix_and_leaves_suffix_untouched(Tracker& t) {
    for (const Arm arm : {Arm::a0, Arm::a1, Arm::b, Arm::r, Arm::s}) {
        KernelOracle oracle;
        auto fixture = make_fixture(t, 16 * kPage);
        auto harness = make_real_harness(t, 24, 8, &oracle);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        std::vector<std::vector<std::byte>> buffers(16, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> offsets;
        for (std::size_t i = 0; i < 16; ++i)
            offsets.push_back(i * kPage);
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        ops[5].file = NativeFileRef(ops[5].file.fd, sluice::FileAccess::write_only);

        bench::r3::Adapter adapter(ctx, arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(outcome.error.has_value() &&
                    outcome.error->code == IoError::Code::invalid_argument,
                "the rejected member surfaces its access error");
        t.check(requests.size() == 5, "only the valid prefix before the rejection is accepted");
        t.check(oracle.total_sqes == 5, "only the valid prefix reaches the kernel");
        t.check(drive_context(ctx, requests), "the accepted prefix terminates");
        for (std::size_t i = 0; i < 5; ++i) {
            check_result(t, requests[i], kPage, "a prefix member keeps its own result");
            check_bytes(t, buffers[i], *fixture, i * kPage, "a prefix member lands its own bytes");
        }
        for (std::size_t i = 5; i < 16; ++i) {
            bool untouched = true;
            for (std::size_t k = 0; k < kPage; ++k)
                untouched = untouched && buffers[i][k] == std::byte{0};
            t.check(untouched, "a suffix member's buffer is never touched");
        }
        const auto outcome_shutdown = ctx.shutdown(ShutdownPolicy::drain);
        t.check(outcome_shutdown.has_value() &&
                    outcome_shutdown.value() == ShutdownOutcome::completed,
                "the rejected span settles at shutdown");
    }
}

// Ineligible members inside an otherwise batch-shaped span: the group stops
// at the first ineligible member, every ineligible member is deferred to the
// frozen immediate path, and the state-aware budget's mirrored shape agrees
// with the entry's actual eligibility (front-ineligible presents one member).
void ineligible_mix_defers_each_ineligible_member_to_immediate_path(Tracker& t) {
    struct Variant {
        Arm arm;
        std::size_t ineligible_fallbacks;
        std::size_t suffix_singles;
        std::size_t presentations;
        std::size_t immediate_decisions;
    };
    const Variant variants[] = {
        {Arm::b, 1, 3, 1, 0},
        {Arm::r, 1, 3, 1, 0},
        {Arm::s, 0, 0, 3, 1},
    };
    for (const Variant& variant : variants) {
        KernelOracle oracle;
        auto fixture = make_fixture(t, 6 * kPage);
        auto harness = make_real_harness(t, 12, 8, &oracle);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        std::vector<std::vector<std::byte>> buffers{
            std::vector<std::byte>(kPage), std::vector<std::byte>(kPage),
            std::vector<std::byte>(512), std::vector<std::byte>(kPage),
            std::vector<std::byte>(kPage)};
        std::vector<std::uint64_t> offsets{0, kPage, 2 * kPage, 2 * kPage + 512, 3 * kPage + 512};
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        bench::r3::Adapter adapter(ctx, variant.arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(!outcome.error.has_value(), "the mixed span is accepted without error");
        t.check(requests.size() == 5, "every mixed-span member is accepted");
        t.check(ids_all_distinct(requests), "mixed-span members keep distinct ids");
        t.check(outcome.counters.ineligible_fallbacks == variant.ineligible_fallbacks,
                "the arm accounts its ineligible boundary exactly");
        t.check(outcome.counters.suffix_single_calls == variant.suffix_singles,
                "the arm's single-path deferral is exact");
        t.check(outcome.counters.r4_batch_presentations == variant.presentations,
                "the arm's presentations are exact");
        t.check(outcome.counters.r4_immediate_decisions == variant.immediate_decisions,
                "the state-aware arm routes the ineligible front to the immediate row");
        t.check(oracle.total_sqes == 5, "every member costs exactly one SQE");
        t.check(drive_context(ctx, requests), "every mixed-span member terminates");
        for (std::size_t i = 0; i < 5; ++i)
            check_result(t, requests[i], buffers[i].size(),
                         "a mixed-span member keeps its own sized result");
        for (std::size_t i = 0; i < 5; ++i)
            check_bytes(t, buffers[i], *fixture, offsets[i],
                        "a mixed-span member lands its own bytes");
    }

    for (const Arm arm : {Arm::r, Arm::s}) {
        KernelOracle oracle;
        auto fixture = make_fixture(t, 4 * kPage);
        auto harness = make_real_harness(t, 12, 8, &oracle);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        std::vector<std::vector<std::byte>> buffers{
            std::vector<std::byte>(512), std::vector<std::byte>(kPage),
            std::vector<std::byte>(kPage)};
        std::vector<std::uint64_t> offsets{0, 512, 512 + kPage};
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        bench::r3::Adapter adapter(ctx, arm);
        bench::r3::Adapter::TraceSink trace;
        adapter.set_trace_sink(&trace);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(!outcome.error.has_value() && requests.size() == 3,
                "the front-ineligible span is accepted whole");
        t.check(outcome.counters.single_calls >= 1,
                "the front-ineligible member takes the immediate path");
        if (arm == Arm::s && trace.size() >= 1) {
            t.check(std::string(trace[0].action_row) == "front_ineligible" &&
                        trace[0].present_len == 1 &&
                        outcome.counters.r4_immediate_decisions == 1,
                    "the budget's mirrored shape matches the entry's front eligibility");
        }
        t.check(oracle.total_sqes == 3, "every front-ineligible member costs one SQE");
        t.check(drive_context(ctx, requests), "every front-ineligible member terminates");
        for (std::size_t i = 0; i < 3; ++i)
            check_bytes(t, buffers[i], *fixture, offsets[i],
                        "a front-ineligible member lands its own bytes");
    }
}

// Slot-capacity exhaustion mid re-entry: the arm reports would_block at the
// authoritative reserve rejection, the already accepted prefix settles, and a
// later refill of the remainder completes the whole workload with no
// duplicate or lost member.
void capacity_exhaustion_would_block_then_refill_completes(Tracker& t) {
    for (const Arm arm : {Arm::a0, Arm::b, Arm::r, Arm::s, Arm::q2}) {
        KernelOracle oracle;
        auto fixture = make_fixture(t, 16 * kPage);
        auto harness = make_real_harness(t, 8, 8, &oracle);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        std::vector<std::vector<std::byte>> buffers(16, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> offsets;
        for (std::size_t i = 0; i < 16; ++i)
            offsets.push_back(i * kPage);
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        bench::r3::Adapter adapter(ctx, arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(outcome.error.has_value() && outcome.error->code == IoError::Code::would_block,
                "capacity exhaustion reports would_block");
        t.check(requests.size() == 8, "exactly the capacity-sized prefix is accepted");
        t.check(ids_all_distinct(requests), "the accepted prefix keeps distinct ids");
        t.check(drive_context(ctx, requests), "the accepted prefix terminates");
        for (std::size_t i = 0; i < 8; ++i)
            check_result(t, requests[i], kPage, "a prefix member keeps its own result");

        std::vector<ReadOp> rest(ops.begin() + 8, ops.end());
        std::vector<Request<std::size_t>> refill;
        const auto second = adapter.submit(std::span<const ReadOp>(rest), refill);
        t.check(!second.error.has_value() && refill.size() == 8,
                "the freed slots accept the remainder");
        t.check(drive_context(ctx, refill), "the remainder terminates");
        for (std::size_t i = 0; i < 8; ++i) {
            check_result(t, refill[i], kPage, "a remainder member keeps its own result");
            check_bytes(t, buffers[8 + i], *fixture, (8 + i) * kPage,
                        "a remainder member lands its own bytes");
        }
        t.check(oracle.total_sqes == 16, "the two-phase refill costs exactly one SQE per member");
    }
}

// A sticky cancel intent between refills stops the next group presentation
// before any member is accepted; the adapter presents once, defers to the
// frozen single path (which the control gate does not block), and the
// cancelled member keeps exactly one authoritative terminal.
void cancel_intent_between_refills_stops_group_not_singles(Tracker& t) {
    for (const Arm arm : {Arm::r, Arm::s}) {
        ScriptedKernel kernel;
        kernel.store(ScriptedKernel::Mode::hold);
        auto fixture = make_fixture(t, 12 * kPage);
        auto harness = make_scripted_harness(t, 16, 4, kernel);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        UringAsyncBackend& backend = *harness->backend;

        std::vector<std::vector<std::byte>> held_buffers(4, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> held_offsets{0, kPage, 2 * kPage, 3 * kPage};
        std::vector<ReadOp> held_ops = file_ops(*fixture, held_buffers, held_offsets);
        auto held = TestAccess::context_submit_ready_reads(ctx, held_ops);
        t.check(held.accepted == 4, "the held prefill is accepted");
        std::vector<Request<std::size_t>> held_requests = requests_from(held);
        const auto cancel = held_requests.front().cancel();
        t.check(cancel.has_value() && cancel.value() == CancelDisposition::requested,
                "the held member reports a requested cancellation");
        t.check(backend.pending_control_snapshot_for_test().authoritative_pending != 0,
                "the full SQ leaves a sticky control intent");

        std::vector<std::vector<std::byte>> buffers(8, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> offsets;
        for (std::size_t i = 0; i < 8; ++i)
            offsets.push_back((4 + i) * kPage);
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        bench::r3::Adapter adapter(ctx, arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(!outcome.error.has_value() && requests.size() == 8,
                "the control boundary still accepts every member via singles");
        t.check(outcome.counters.r4_batch_presentations == 1,
                "the control stop is presented exactly once (no spin)");
        t.check(outcome.counters.gate_stops == 1, "the control gate is accounted");
        t.check(outcome.counters.suffix_single_calls == 8,
                "the whole span defers to the frozen single path");

        kernel.store(ScriptedKernel::Mode::consume_all);
        (void)ctx.poll();
        (void)ctx.poll();
        t.check(backend.live_control_sqes_for_test() == 1,
                "the sticky cancel intent becomes one submitted control SQE");
        const auto cookie0 = backend.live_cookie_for_offset_for_test(0);
        if (cookie0.has_value()) {
            backend.inject_cqe_for_test(kControlTag | *cookie0, 0);
            backend.inject_cqe_for_test(*cookie0, -ECANCELED);
        }
        for (std::size_t i = 1; i < 12; ++i) {
            const auto cookie = backend.live_cookie_for_offset_for_test(i * kPage);
            if (cookie.has_value())
                backend.inject_cqe_for_test(*cookie, static_cast<int>(kPage));
        }
        std::vector<Request<std::size_t>> all = std::move(held_requests);
        for (Request<std::size_t>& request : requests)
            all.push_back(std::move(request));
        t.check(drive_context(ctx, all), "held, cancelled and deferred members all terminate");
        const auto cancelled = all.front().take_result();
        t.check(cancelled.readiness == RequestReadiness::ready && !cancelled.result.has_value() &&
                    cancelled.result.error().code == IoError::Code::canceled,
                "the cancelled member keeps exactly one canceled terminal");
        const auto again = all.front().take_result();
        t.check(again.readiness != RequestReadiness::ready,
                "the cancelled terminal is consumable exactly once");
        for (std::size_t i = 1; i < all.size(); ++i)
            check_result(t, all[i], kPage, "a non-cancelled member keeps its own result");
    }
}

// A poisoned transport between refills: the next presentation of every arm
// observes the fatal boundary, accepts nothing new, and the previously
// accepted members keep exactly one terminal each — the kernel-visible one
// its real result, the invisible one its physical failure, never success.
void poison_between_refills_rejects_new_and_settles_exactly_once(Tracker& t) {
    for (const Arm arm : {Arm::r, Arm::s}) {
        ScriptedKernel kernel;
        kernel.store(ScriptedKernel::Mode::hold);
        auto fixture = make_fixture(t, 10 * kPage);
        auto harness = make_scripted_harness(t, 16, 4, kernel);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        UringAsyncBackend& backend = *harness->backend;

        std::vector<std::vector<std::byte>> held_buffers(2, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> held_offsets{0, kPage};
        std::vector<ReadOp> held_ops = file_ops(*fixture, held_buffers, held_offsets);
        bench::r3::Adapter held_adapter(ctx, arm);
        std::vector<Request<std::size_t>> held_requests;
        const auto held_outcome = held_adapter.submit(std::span<const ReadOp>(held_ops),
                                                      held_requests);
        t.check(!held_outcome.error.has_value() && held_requests.size() == 2,
                "the pre-poison refill is accepted whole");

        kernel.store(ScriptedKernel::Mode::partial_then_poison);
        kernel.calls.store(0, std::memory_order_relaxed);
        (void)ctx.poll();
        t.check(backend.live_cookie_for_offset_for_test(0).has_value(),
                "the kernel-visible member keeps its route after the poison");

        std::vector<std::vector<std::byte>> buffers(8, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> offsets;
        for (std::size_t i = 0; i < 8; ++i)
            offsets.push_back((2 + i) * kPage);
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        bench::r3::Adapter adapter(ctx, arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(outcome.error.has_value() && outcome.error->code == IoError::Code::backend_error,
                "the poisoned boundary rejects the new presentation");
        t.check(requests.empty(), "the poisoned boundary accepts no new member");

        const auto visible = backend.live_cookie_for_offset_for_test(0);
        if (visible.has_value())
            backend.inject_cqe_for_test(*visible, static_cast<int>(kPage));
        t.check(drive_context(ctx, held_requests), "the poisoned members reach their terminals");
        check_result(t, held_requests[0], kPage, "the kernel-visible member keeps its real result");
        const auto failed = held_requests[1].take_result();
        t.check(failed.readiness == RequestReadiness::ready && !failed.result.has_value() &&
                    failed.result.error().code == IoError::Code::backend_error,
                "the invisible member reports the physical failure, never success");
        const auto second_take = held_requests[1].take_result();
        t.check(second_take.readiness != RequestReadiness::ready,
                "the failure terminal is consumable exactly once");
        const auto outcome_shutdown = ctx.shutdown(ShutdownPolicy::drain);
        t.check(outcome_shutdown.has_value() &&
                    outcome_shutdown.value() == ShutdownOutcome::completed,
                "the poisoned boundary settles at shutdown");
    }
}

// Concurrent submitters on one context through the re-entry arms: every op is
// accepted exactly once with a context-unique request id, a second round
// reuses freed slots under advanced generations (old keys never reappear),
// and exactly one kernel SQE per op is emitted across all interleavings.
void concurrent_submitters_unique_ids_and_generation_advancing_reuse(Tracker& t) {
    constexpr std::size_t kThreads = 2;
    constexpr std::size_t kPerThread = 16;
    constexpr std::size_t kTotal = kThreads * kPerThread;
    KernelOracle oracle;
    auto fixture = make_fixture(t, kTotal * kPage);
    auto harness = make_real_harness(t, kTotal + 8, 16, &oracle);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    const Fixture& shared_fixture = *fixture;

    std::vector<std::vector<std::byte>> buffers(kTotal, std::vector<std::byte>(kPage));
    std::vector<std::vector<Request<std::size_t>>> per_thread(kThreads);
    std::vector<sluice::async::detail::RequestKey> round_keys;
    const auto run_round = [&](Arm arm) {
        round_keys.clear();
        for (std::size_t th = 0; th < kThreads; ++th) {
            per_thread[th].clear();
        }
        std::vector<std::thread> threads;
        for (std::size_t th = 0; th < kThreads; ++th) {
            threads.emplace_back([&ctx, arm, th, &shared_fixture, &buffers, &per_thread]() {
                std::vector<ReadOp> ops;
                ops.reserve(kPerThread);
                for (std::size_t i = 0; i < kPerThread; ++i) {
                    ReadOp op;
                    op.file = NativeFileRef(*shared_fixture.file);
                    op.dst = buffers[th * kPerThread + i].data();
                    op.len = kPage;
                    op.offset = (th * kPerThread + i) * kPage;
                    ops.push_back(op);
                }
                bench::r3::Adapter adapter(ctx, arm);
                std::vector<Request<std::size_t>> requests;
                const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
                if (!outcome.error.has_value() && requests.size() == kPerThread)
                    per_thread[th] = std::move(requests);
            });
        }
        for (std::thread& thread : threads)
            thread.join();
        std::vector<Request<std::size_t>> all;
        for (std::size_t th = 0; th < kThreads; ++th) {
            t.check(per_thread[th].size() == kPerThread,
                    "a concurrent submitter accepts its whole span");
            for (const Request<std::size_t>& request : per_thread[th])
                round_keys.push_back(id_key(request.id()));
            for (Request<std::size_t>& request : per_thread[th])
                all.push_back(std::move(request));
        }
        t.check(drive_context(ctx, all), "every concurrently submitted member terminates");
        for (Request<std::size_t>& request : all)
            check_result(t, request, kPage,
                         "a concurrently submitted member keeps its own result");
    };

    run_round(Arm::r);
    KeySet round_a(round_keys.begin(), round_keys.end());
    t.check(round_a.size() == kTotal, "round A ids are context-unique across threads");
    for (std::size_t i = 0; i < kTotal; ++i)
        check_bytes(t, buffers[i], *fixture, i * kPage,
                    "a round-A member lands its own bytes");

    run_round(Arm::s);
    for (const sluice::async::detail::RequestKey& key : round_keys) {
        t.check(round_a.find(key) == round_a.end(),
                "slot reuse advances the generation so no old key reappears");
    }
    t.check(oracle.total_sqes == 2 * kTotal,
            "both concurrent rounds cost exactly one kernel SQE per op");
    for (std::size_t i = 0; i < kTotal; ++i)
        check_bytes(t, buffers[i], *fixture, i * kPage,
                    "a concurrently submitted member lands its own bytes");
    t.check(ctx.outstanding() == 0, "the concurrent rounds leave nothing outstanding");
}

// CQ overflow across cap re-entries: 64 members submitted without an
// intervening reap park completions in the overflow list; the reap path
// flushes it and every member keeps its own published result and bytes.
void cq_overflow_across_reentry_is_flushed_not_lost(Tracker& t) {
    for (const Arm arm : {Arm::r, Arm::s}) {
        KernelOracle oracle;
        auto fixture = make_fixture(t, 64 * kPage);
        auto harness = make_real_harness(t, 72, 8, &oracle);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        UringAsyncBackend& backend = *harness->backend;
        std::vector<std::vector<std::byte>> buffers(64, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> offsets;
        for (std::size_t i = 0; i < 64; ++i)
            offsets.push_back(i * kPage);
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        bench::r3::Adapter adapter(ctx, arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(!outcome.error.has_value() && requests.size() == 64,
                "the overflow workload is accepted whole");
        t.check(wait_for_cq_overflow(backend),
                "the kernel parks completions in the overflow list before the reap");
        const unsigned flushes_before = oracle.get_events_calls;
        t.check(drive_context(ctx, requests), "every overflow member terminates");
        t.check(oracle.get_events_calls > flushes_before,
                "the overflow list is flushed by the reap path");
        for (std::size_t i = 0; i < 64; ++i) {
            check_result(t, requests[i], kPage,
                         "an overflow member keeps its own published result");
            check_bytes(t, buffers[i], *fixture, i * kPage,
                        "an overflow member lands its own bytes");
        }
        t.check(oracle.total_sqes == 64, "the overflow workload costs one SQE per member");
    }
}

// The per-member borrow survives across a cap re-entry: 16 pipe reads stay
// pending with untouched buffers, and each payload release completes exactly
// one more member (whichever the kernel picks), leaving every still-pending
// member's borrow untouched until its own release. Completion order across
// equal pipe readers is not an invariant, so the oracle counts published
// members instead of assuming FIFO order.
void borrow_lifetime_is_per_member_across_reentry(Tracker& t) {
    for (const Arm arm : {Arm::r, Arm::s}) {
        auto harness = make_real_harness(t, 24, 16, nullptr);
        if (!harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        UringAsyncBackend& backend = *harness->backend;

        int fds[2];
        if (::pipe(fds) != 0) {
            t.check(false, "pipe fixture is created");
            return;
        }
        const std::byte sentinel{0x5a};
        std::vector<std::vector<std::byte>> buffers(16, std::vector<std::byte>(kPage, sentinel));
        std::vector<ReadOp> ops = pipe_ops(fds[0], buffers, 16);
        bench::r3::Adapter adapter(ctx, arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(!outcome.error.has_value() && requests.size() == 16,
                "the pipe batch is accepted whole across re-entries");
        t.check(backend.live_cookies_for_test() == 16,
                "every member holds its own live kernel route");
        bool all_sentinel = true;
        for (const std::vector<std::byte>& buffer : buffers)
            all_sentinel = all_sentinel && buffer[0] == sentinel;
        t.check(all_sentinel, "no member's buffer is touched before its own completion");

        const std::string payload(kPage, 'p');
        const auto ready_count = [&requests]() {
            return static_cast<std::size_t>(
                std::count_if(requests.begin(), requests.end(),
                              [](const Request<std::size_t>& request) {
                                  return request.ready();
                              }));
        };
        std::size_t published = 0;
        for (std::size_t release = 0; release < 16; ++release) {
            t.check(::write(fds[1], payload.data(), payload.size()) ==
                        static_cast<ssize_t>(payload.size()),
                    "one member's payload is released");
            const auto deadline = std::chrono::steady_clock::now() + kDriveBudget;
            while (ready_count() == published) {
                if (std::chrono::steady_clock::now() >= deadline)
                    break;
                (void)ctx.poll();
            }
            const std::size_t now_published = ready_count();
            t.check(now_published == published + 1,
                    "exactly one more member is published per payload release");
            published = now_published;
            std::size_t completed_index = requests.size();
            std::size_t completed_count = 0;
            for (std::size_t i = 0; i < requests.size(); ++i) {
                if (!requests[i].ready())
                    continue;
                ++completed_count;
                completed_index = i;
            }
            if (completed_count != published) {
                t.check(false, "the published set is consistent with the ready count");
                break;
            }
            t.check(std::memcmp(buffers[completed_index].data(), payload.data(), kPage) == 0,
                    "the newly completed member writes its own buffer");
            bool others_untouched = true;
            for (std::size_t i = 0; i < requests.size(); ++i) {
                if (!requests[i].ready())
                    others_untouched = others_untouched && buffers[i][0] == sentinel;
            }
            t.check(others_untouched,
                    "every still-pending member's borrow stays untouched");
        }
        t.check(published == 16, "sixteen releases publish all sixteen members");
        for (std::size_t k = 0; k < 16; ++k)
            check_result(t, requests[k], kPage, "a pipe member keeps its own result");
        ::close(fds[0]);
        ::close(fds[1]);
    }
}

// The state-aware budget's quantum follows the pipeline state through real
// passes: a legal completion backlog with one member still in flight clamps
// the quantum to 2 via the pass-fact channel, while the ablations prove each
// hint is what moves its own row.
void state_aware_quantum_tracks_pipeline_state(Tracker& t) {
    ScriptedKernel kernel;
    kernel.store(ScriptedKernel::Mode::hold);
    auto fixture = make_fixture(t, 12 * kPage);
    auto harness = make_scripted_harness(t, 16, 8, kernel);
    if (!fixture.has_value() || !harness.has_value())
        return;
    AsyncIoContext& ctx = *harness->ctx;
    UringAsyncBackend& backend = *harness->backend;

    std::vector<std::vector<std::byte>> held_buffers(2, std::vector<std::byte>(kPage));
    std::vector<std::uint64_t> held_offsets{0, kPage};
    std::vector<ReadOp> held_ops = file_ops(*fixture, held_buffers, held_offsets);
    auto held = TestAccess::context_submit_ready_reads(ctx, held_ops);
    t.check(held.accepted == 2, "the held pair is accepted to pin outstanding data");
    kernel.store(ScriptedKernel::Mode::consume_all);
    (void)ctx.poll();
    t.check(backend.live_cookie_for_offset_for_test(0).has_value() &&
                backend.live_cookie_for_offset_for_test(kPage).has_value(),
            "both held members keep live routes");
    const auto cookie0 = backend.live_cookie_for_offset_for_test(0);
    if (cookie0.has_value())
        backend.inject_cqe_for_test(*cookie0, static_cast<int>(kPage));
    auto pass = ctx.poll_progress();
    t.check(pass.has_value() && pass.value().completed == 1,
            "one pass reaps exactly the completed member");
    t.check(ctx.outstanding() == 1, "one accepted member stays outstanding after the pass");

    bench::r3::DecisionFacts facts;
    facts.last_pass_completed = pass.value().completed;
    facts.last_pass_immediate_work = pass.value().immediate_work_remains;
    facts.pressure_known = true;

    const struct Variant {
        Arm arm;
        bool active_read;
        bool pressure_read;
        std::size_t quantum;
        const char* row;
        std::uint64_t base_offset;
    } variants[] = {
        {Arm::s, true, true, 2, "completion_pressure", 2 * kPage},
        {Arm::s_no_inflight, false, true, 2, "completion_pressure", 4 * kPage},
        {Arm::s_no_pressure, true, false, 8, "quantum_base", 6 * kPage},
        {Arm::q8, false, false, 8, "quantum_pinned", 8 * kPage},
    };
    std::vector<std::vector<Request<std::size_t>>> probe_requests;
    for (const Variant& variant : variants) {
        std::vector<std::vector<std::byte>> probe_buffers(2, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> probe_offsets{variant.base_offset, variant.base_offset + kPage};
        std::vector<ReadOp> probe_ops = file_ops(*fixture, probe_buffers, probe_offsets);
        bench::r3::Adapter adapter(ctx, variant.arm);
        bench::r3::Adapter::TraceSink trace;
        adapter.set_trace_sink(&trace);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(probe_ops), requests, facts);
        t.check(!outcome.error.has_value() && requests.size() == 2,
                "the state probe is accepted whole");
        t.check(trace.size() >= 1, "the state probe forms its decision");
        if (trace.size() >= 1) {
            t.check(trace[0].quantum == variant.quantum &&
                        std::string(trace[0].action_row) == variant.row,
                    "the budget picks its documented row at this state");
            t.check(trace[0].active_read == variant.active_read &&
                        trace[0].pressure_read == variant.pressure_read,
                    "the ablation reads exactly its own hints");
            if (variant.active_read)
                t.check(trace[0].active_data >= 1,
                        "the inflight hint sees the pinned member");
            if (variant.pressure_read)
                t.check(trace[0].pressure && trace[0].last_pass_completed == 1,
                        "the pressure hint sees the completed pass");
        }
        (void)outcome;
        probe_requests.push_back(std::move(requests));
    }

    std::vector<Request<std::size_t>> held_requests = requests_from(held);
    const auto cookie1 = backend.live_cookie_for_offset_for_test(kPage);
    if (cookie1.has_value())
        backend.inject_cqe_for_test(*cookie1, static_cast<int>(kPage));
    for (std::size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); ++v) {
        for (std::size_t m = 0; m < 2; ++m) {
            const auto cookie = backend.live_cookie_for_offset_for_test(
                variants[v].base_offset + m * kPage);
            if (cookie.has_value())
                backend.inject_cqe_for_test(*cookie, static_cast<int>(kPage));
        }
    }
    t.check(drive_context(ctx, held_requests), "the pinned members terminate");
    check_result(t, held_requests[0], kPage, "the reaped member keeps its result");
    check_result(t, held_requests[1], kPage, "the held member keeps its result");
    for (std::vector<Request<std::size_t>>& requests : probe_requests) {
        t.check(drive_context(ctx, requests), "a probe member terminates");
        for (Request<std::size_t>& request : requests)
            check_result(t, request, kPage, "a probe member keeps its result");
    }
}

// An empty pipeline (outstanding()==0, no pass facts) clamps only the
// state-aware arms' first presentation to 2 members; the hint-free and pinned
// arms present 8, and R/B present the full span — the arms are distinguished
// by their own decision records at the same state.
void empty_pipeline_clamps_only_the_state_aware_arms(Tracker& t) {
    struct Variant {
        Arm arm;
        std::size_t present_len;
        bool budgeted;
        std::size_t quantum;
    };
    const Variant variants[] = {
        {Arm::s, 2, true, 2},
        {Arm::s_no_pressure, 2, true, 2},
        {Arm::s_no_inflight, 8, true, 8},
        {Arm::q2, 2, true, 2},
        {Arm::q8, 8, true, 8},
        {Arm::r, 16, false, 0},
        {Arm::b, 16, false, 0},
    };
    for (const Variant& variant : variants) {
        KernelOracle oracle;
        auto fixture = make_fixture(t, 16 * kPage);
        auto harness = make_real_harness(t, 24, 16, &oracle);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        std::vector<std::vector<std::byte>> buffers(16, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> offsets;
        for (std::size_t i = 0; i < 16; ++i)
            offsets.push_back(i * kPage);
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        bench::r3::Adapter adapter(ctx, variant.arm);
        bench::r3::Adapter::TraceSink trace;
        adapter.set_trace_sink(&trace);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(!outcome.error.has_value() && requests.size() == 16,
                "the empty-pipeline probe is accepted whole");
        t.check(trace.size() >= 1, "the empty-pipeline probe forms its decision");
        if (trace.size() >= 1) {
            t.check(trace[0].present_len == variant.present_len,
                    "the arm presents its documented first span");
            if (variant.budgeted) {
                t.check(trace[0].quantum == variant.quantum,
                        "the budget arm's quantum matches its policy at the empty state");
                t.check(trace[0].active_read == (variant.arm == Arm::s ||
                                                 variant.arm == Arm::s_no_pressure),
                        "only the inflight-reading arms observe the empty pipeline");
            } else {
                t.check(std::string(trace[0].action_row) == "full_span",
                        "the unbudgeted arm presents the full span without hint reads");
                t.check(!trace[0].active_read && !trace[0].pressure_read,
                        "R/B read no hints at the empty state");
            }
        }
        (void)outcome;
        t.check(drive_context(ctx, requests), "the empty-pipeline probe terminates");
        for (std::size_t i = 0; i < 16; ++i)
            check_bytes(t, buffers[i], *fixture, i * kPage,
                        "an empty-pipeline member lands its own bytes");
    }
}

// A shutdown between refills closes admission: the adapter's next
// presentation is rejected with the frozen entry's invalid_state and accepts
// nothing, while the drained members keep exactly one terminal each.
void shutdown_between_refills_closes_admission_for_the_adapter(Tracker& t) {
    for (const Arm arm : {Arm::r, Arm::s}) {
        KernelOracle oracle;
        auto fixture = make_fixture(t, 10 * kPage);
        auto harness = make_real_harness(t, 16, 8, &oracle);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;

        std::vector<std::vector<std::byte>> buffers(8, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> offsets;
        for (std::size_t i = 0; i < 8; ++i)
            offsets.push_back(i * kPage);
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        bench::r3::Adapter adapter(ctx, arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(!outcome.error.has_value() && requests.size() == 8,
                "the pre-shutdown refill is accepted whole");
        const auto drain = ctx.shutdown(ShutdownPolicy::drain);
        t.check(drain.has_value() && drain.value() == ShutdownOutcome::completed,
                "drain shutdown settles the accepted refill");
        t.check(all_ready(requests), "shutdown publication makes every member ready");
        for (std::size_t i = 0; i < 8; ++i) {
            check_result(t, requests[i], kPage, "a drained member keeps its own result");
            check_bytes(t, buffers[i], *fixture, i * kPage, "a drained member lands its own bytes");
        }

        std::vector<std::vector<std::byte>> later_buffers(2, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> later_offsets{8 * kPage, 9 * kPage};
        std::vector<ReadOp> later_ops = file_ops(*fixture, later_buffers, later_offsets);
        const unsigned enters_before = oracle.count;
        std::vector<Request<std::size_t>> later_requests;
        const auto later = adapter.submit(std::span<const ReadOp>(later_ops), later_requests);
        t.check(later.error.has_value() && later.error->code == IoError::Code::invalid_state,
                "the closed execution rejects the adapter's next presentation");
        t.check(later_requests.empty(), "the closed execution accepts nothing");
        t.check(oracle.count == enters_before, "the closed execution reaches no kernel submit");
    }
}

// Every member of a re-entered span publishes exactly once: the accumulated
// pass completions equal the span, no pass publishes more members than
// remain, and bounded passes publish every member deterministically.
void reentered_span_publishes_exactly_once(Tracker& t) {
    for (const Arm arm : {Arm::r, Arm::s}) {
        auto fixture = make_fixture(t, 16 * kPage);
        auto harness = make_real_harness(t, 24, 16, nullptr);
        if (!fixture.has_value() || !harness.has_value())
            return;
        AsyncIoContext& ctx = *harness->ctx;
        std::vector<std::vector<std::byte>> buffers(16, std::vector<std::byte>(kPage));
        std::vector<std::uint64_t> offsets;
        for (std::size_t i = 0; i < 16; ++i)
            offsets.push_back(i * kPage);
        std::vector<ReadOp> ops = file_ops(*fixture, buffers, offsets);
        bench::r3::Adapter adapter(ctx, arm);
        std::vector<Request<std::size_t>> requests;
        const auto outcome = adapter.submit(std::span<const ReadOp>(ops), requests);
        t.check(!outcome.error.has_value() && requests.size() == 16,
                "the publication probe is accepted whole");

        std::size_t published = 0;
        bool all_published = false;
        for (std::size_t guard = 0; guard < 100000; ++guard) {
            auto pass = ctx.poll_progress();
            if (!pass.has_value()) {
                t.check(false, "a publication pass never fails on a healthy context");
                break;
            }
            published += pass.value().completed;
            t.check(published <= 16, "a pass never publishes more members than remain");
            if (all_ready(requests)) {
                all_published = true;
                break;
            }
        }
        t.check(all_published, "bounded passes publish every member");
        t.check(published == 16, "exactly sixteen publications are counted in total");
        for (std::size_t i = 0; i < 16; ++i) {
            check_result(t, requests[i], kPage, "a published member keeps its own result");
            check_bytes(t, buffers[i], *fixture, i * kPage,
                        "a published member lands its own bytes");
        }
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

    Tracker tracker{"uring_ready_reuse_correctness"};
    if (selected("empty_span")) empty_span_is_inert_on_every_arm(tracker);
    if (selected("single_read")) single_read_settles_with_own_identity_on_every_arm(tracker);
    if (selected("span_shapes")) span_shapes_and_reentry_counts_are_exact(tracker);
    if (selected("residue_after_cap"))
        cap_stop_with_transport_residue_defers_to_frozen_authority(tracker);
    if (selected("prefilled_residue"))
        prefilled_sq_full_residue_stops_group_without_spinning(tracker);
    if (selected("partial_retry")) partial_flush_residue_retry_preserves_member_identity(tracker);
    if (selected("middle_rejection"))
        middle_member_rejection_settles_prefix_and_leaves_suffix_untouched(tracker);
    if (selected("ineligible_mix"))
        ineligible_mix_defers_each_ineligible_member_to_immediate_path(tracker);
    if (selected("capacity")) capacity_exhaustion_would_block_then_refill_completes(tracker);
    if (selected("control_between"))
        cancel_intent_between_refills_stops_group_not_singles(tracker);
    if (selected("poison_between"))
        poison_between_refills_rejects_new_and_settles_exactly_once(tracker);
    if (selected("concurrent"))
        concurrent_submitters_unique_ids_and_generation_advancing_reuse(tracker);
    if (selected("overflow")) cq_overflow_across_reentry_is_flushed_not_lost(tracker);
    if (selected("borrow")) borrow_lifetime_is_per_member_across_reentry(tracker);
    if (selected("state_aware")) state_aware_quantum_tracks_pipeline_state(tracker);
    if (selected("empty_pipeline")) empty_pipeline_clamps_only_the_state_aware_arms(tracker);
    if (selected("shutdown_between"))
        shutdown_between_refills_closes_admission_for_the_adapter(tracker);
    if (selected("publish_once")) reentered_span_publishes_exactly_once(tracker);
    if (tracker.failures != 0) {
        std::fprintf(stderr, "%s: %d failure(s)\n", tracker.name, tracker.failures);
        return 1;
    }
    std::printf("%s: all ready-reuse correctness oracles passed\n", tracker.name);
    return 0;
}
