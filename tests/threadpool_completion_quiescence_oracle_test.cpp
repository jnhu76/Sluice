#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/threadpool_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/result.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::FileAccess;
using sluice::async::NativeFileRef;
using sluice::async::ReadOp;
using sluice::async::ThreadPoolBackend;
using sluice::async::ThreadPoolConfig;

constexpr int kBoundedPolls = 200000;
constexpr int kGateWaits = 20000000;

// A failed named assertion exits immediately: under a mutation build the
// ordinary teardown may legitimately wedge (e.g. settlement can never
// converge), and the failing observation must surface before that happens.
void bail(const char* scenario, const char* reason) {
    std::fprintf(stderr, "FAIL: %s: %s\n", scenario, reason);
    std::fflush(stderr);
    std::_Exit(1);
}

int temp_file_fd(const std::string& content) {
    char path[] = "/tmp/sluice_quiescence_oracle_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return -1;
    ::unlink(path);
    const ssize_t written = ::write(fd, content.data(), content.size());
    if (written != static_cast<ssize_t>(content.size())) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool wait_gate_paused(ThreadPoolBackend::WorkerOutcomePreTerminalPauseGate& gate) {
    for (int i = 0; i < kGateWaits && !gate.paused.load(std::memory_order_acquire); ++i)
        std::this_thread::yield();
    return gate.paused.load(std::memory_order_acquire);
}

void resume_gate(ThreadPoolBackend::WorkerOutcomePreTerminalPauseGate& gate) noexcept {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

bool wait_gate_exited(ThreadPoolBackend::WorkerOutcomePreTerminalPauseGate& gate) {
    for (int i = 0; i < kGateWaits && !gate.exited.load(std::memory_order_acquire); ++i)
        std::this_thread::yield();
    return gate.exited.load(std::memory_order_acquire);
}

bool drive_ready(AsyncIoContext& ctx, Request<std::size_t>& request) {
    for (int i = 0; i < kBoundedPolls && request.valid() && !request.ready(); ++i)
        (void)ctx.poll();
    return request.valid() && request.ready();
}

// An abandoned pre-terminal pause would wedge settlement and fail-fast the
// context teardown, so every hold unblocks its worker, drives the held read
// to publication and discards the binding before the enclosing scope ends.
struct GatedRead {
    AsyncIoContext* ctx = nullptr;
    ThreadPoolBackend* backend = nullptr;
    ThreadPoolBackend::WorkerOutcomePreTerminalPauseGate gate;
    Request<std::size_t> request;
    int fd = -1;
    bool released = false;

    ~GatedRead() {
        if (backend != nullptr && !released)
            unblock();
        if (ctx != nullptr && request.valid() && !request.ready())
            (void)drive_ready(*ctx, request);
        if (request.valid())
            request.discard();
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    bool arm(AsyncIoContext& context, ThreadPoolBackend* backend_ptr) {
        ctx = &context;
        backend = backend_ptr;
        fd = temp_file_fd(std::string(64, 'q'));
        if (fd < 0)
            return false;
        backend->set_worker_outcome_pre_terminal_pause_gate(&gate);
        buffer.resize(32, std::byte{0});
        auto submitted = ctx->submit_read(
            ReadOp{NativeFileRef(fd, FileAccess::read_only), buffer.data(), buffer.size(), 0});
        if (!submitted.has_value())
            return false;
        request = std::move(submitted).value();
        return wait_gate_paused(gate);
    }

    void unblock() {
        released = true;
        backend->set_worker_outcome_pre_terminal_pause_gate(nullptr);
        resume_gate(gate);
        (void)wait_gate_exited(gate);
    }

    std::vector<std::byte> buffer;
};

// Design 3.2-2a: while a completed physical op sits gated before its terminal
// handoff, the settlement conjunction must not converge — the unpublished
// window is held core-side (occupancy().outstanding), so neither arm may
// observe convergence, whatever the backend-side retired flag reads. After
// release and publication the same conjunction must flip.
void blocked_conjunction_holds_while_gated_and_flips_after_publication() {
    const char* kScenario = "blocked_conjunction_holds_while_gated_and_flips_after_publication";
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    GatedRead held;
    if (!held.arm(ctx, raw))
        bail(kScenario, "worker never reached the pre-terminal pause");

    const auto* core = raw->request_core_for_test();
    for (int i = 0; i < 200; ++i) {
        (void)ctx.poll();
        const auto occupancy = core->occupancy();
        if (occupancy.accepted_live != 1)
            bail(kScenario, "gated read is not the single accepted request");
        if (occupancy.outstanding == 0)
            bail(kScenario, "core released the unpublished window while gated");
        if (raw->internal_work_retired() && occupancy.outstanding == 0)
            bail(kScenario, "settlement conjunction converged while gated");
        if (held.request.ready())
            bail(kScenario, "request became ready before the terminal handoff");
    }

    held.unblock();
    if (!drive_ready(ctx, held.request))
        bail(kScenario, "read never published after gate release");
    held.request.discard();

    for (int i = 0; i < kBoundedPolls && !raw->internal_work_retired(); ++i)
        (void)ctx.poll();
    const auto settled = core->occupancy();
    if (!raw->internal_work_retired() || settled.outstanding != 0 ||
        settled.accepted_live != 0 || raw->active_workers() != 0 ||
        raw->publication_pending_size_for_test() != 0)
        bail(kScenario, "settlement conjunction did not flip after publication");
}

// Design 3.2-2b: after N physical completions are published and their
// bindings dropped, the backend quiescence face reaches its terminal values
// in both arms.
void quiescence_terminal_predicate_after_drain() {
    const char* kScenario = "quiescence_terminal_predicate_after_drain";
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 4});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx = detail::make_context(std::move(backend));
    const auto* core = raw->request_core_for_test();

    std::vector<Request<std::size_t>> reads;
    std::vector<std::vector<std::byte>> buffers;
    std::vector<int> fds;
    for (int n = 0; n < 4; ++n) {
        const int fd = temp_file_fd(std::string(64, 'd'));
        if (fd < 0)
            bail(kScenario, "temp file creation failed");
        fds.push_back(fd);
        buffers.emplace_back(32, std::byte{0});
        auto submitted = ctx.submit_read(ReadOp{NativeFileRef(fd, FileAccess::read_only),
                                                buffers.back().data(), buffers.back().size(), 0});
        if (!submitted.has_value())
            bail(kScenario, "submit_read refused a plain read");
        reads.push_back(std::move(submitted).value());
    }

    bool all_ready = true;
    for (auto& r : reads)
        all_ready = all_ready && drive_ready(ctx, r);
    for (int fd : fds)
        ::close(fd);
    if (!all_ready)
        bail(kScenario, "a read never published");
    for (auto& r : reads)
        r.discard();

    for (int i = 0; i < kBoundedPolls && !raw->internal_work_retired(); ++i)
        (void)ctx.poll();
    const auto occupancy = core->occupancy();
    if (raw->active_workers() != 0 || !raw->internal_work_retired() ||
        raw->publication_pending_size_for_test() != 0 || occupancy.outstanding != 0 ||
        occupancy.accepted_live != 0)
        bail(kScenario, "quiescence terminal predicate not reached after drain");
}

// Design 3.2-2c: the gated completion holds no queued publication until the
// terminal handoff is done; releasing the gate yields exactly one queued
// entry, and one driver pass publishes it exactly once.
void gated_completion_publishes_exactly_once_after_handoff() {
    const char* kScenario = "gated_completion_publishes_exactly_once_after_handoff";
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    GatedRead held;
    if (!held.arm(ctx, raw))
        bail(kScenario, "worker never reached the pre-terminal pause");
    if (raw->publication_pending_size_for_test() != 0)
        bail(kScenario, "publication queued before the terminal handoff");

    held.unblock();
    bool queued = false;
    for (int i = 0; i < kGateWaits && !queued; ++i) {
        queued = raw->publication_pending_size_for_test() == 1;
        if (!queued)
            std::this_thread::yield();
    }
    if (!queued || raw->publication_pending_size_for_test() != 1)
        bail(kScenario, "exactly one queued publication did not appear after release");

    const auto pass = ctx.poll();
    if (!pass.has_value() || pass.value() != 1 ||
        raw->publication_pending_size_for_test() != 0 ||
        !held.request.ready() || raw->sink_deliveries() != 1)
        bail(kScenario, "one driver pass did not publish the completion exactly once");
    if (!raw->internal_work_retired() || raw->active_workers() != 0)
        bail(kScenario, "backend not quiescent after the publishing pass");
}

// Design 3.2-2d: with every binding drained and dropped, the settlement
// driver converges and the context shuts down cleanly.
void shutdown_settlement_converges_after_drain() {
    const char* kScenario = "shutdown_settlement_converges_after_drain";
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 2});
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    std::vector<Request<std::size_t>> reads;
    std::vector<std::vector<std::byte>> buffers;
    std::vector<int> fds;
    for (int n = 0; n < 2; ++n) {
        const int fd = temp_file_fd(std::string(64, 's'));
        if (fd < 0)
            bail(kScenario, "temp file creation failed");
        fds.push_back(fd);
        buffers.emplace_back(32, std::byte{0});
        auto submitted = ctx.submit_read(ReadOp{NativeFileRef(fd, FileAccess::read_only),
                                                buffers.back().data(), buffers.back().size(), 0});
        if (!submitted.has_value())
            bail(kScenario, "submit_read refused a plain read");
        reads.push_back(std::move(submitted).value());
    }
    for (auto& r : reads) {
        if (!drive_ready(ctx, r))
            bail(kScenario, "a read never published");
    }
    for (int fd : fds)
        ::close(fd);
    for (auto& r : reads)
        r.discard();

    const auto settled = ctx.shutdown(sluice::async::ShutdownPolicy::drain);
    if (!settled.has_value())
        bail(kScenario, "shutdown(drain) did not converge after a full drain");
    const auto refused = ctx.wait_one(std::chrono::milliseconds{0});
    if (refused.has_value() || refused.error().code != sluice::IoError::Code::invalid_state)
        bail(kScenario, "closed context did not refuse wait_one with invalid_state");
}

}

int main() {
    struct NamedTest {
        const char* name;
        void (*fn)();
    };
    const NamedTest tests[] = {
        {"blocked_conjunction_holds_while_gated_and_flips_after_publication",
         blocked_conjunction_holds_while_gated_and_flips_after_publication},
        {"quiescence_terminal_predicate_after_drain", quiescence_terminal_predicate_after_drain},
        {"gated_completion_publishes_exactly_once_after_handoff",
         gated_completion_publishes_exactly_once_after_handoff},
        {"shutdown_settlement_converges_after_drain", shutdown_settlement_converges_after_drain},
    };
    for (const NamedTest& t : tests) {
        std::printf("[case] %s\n", t.name);
        std::fflush(stdout);
        t.fn();
    }
    std::printf("all %d threadpool completion quiescence tests passed\n",
                static_cast<int>(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
