#include <sluice/async/stackful_io_host.hpp>

#include <sluice/async/request_scope.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#if defined(SLUICE_HAS_LIBURING)
#include <sluice/async/uring_backend.hpp>
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "threadpool_test_seams.hpp"

namespace {

using namespace sluice::async;
using sluice::AsyncStats;
using sluice::blocking::CompositionEnd;
using sluice::blocking::CompositionOutcome;
using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::Result;

int g_failures = 0;

void check(bool ok, const char* label) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_host_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0) {
        return {};
    }
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

bool open_fd_content_is(int fd, const std::string& expected) {
    const std::string path = "/proc/self/fd/" + std::to_string(fd);
    const int probe = ::open(path.c_str(), O_RDONLY);
    if (probe < 0) {
        return false;
    }
    std::string got(expected.size() + 1, '\0');
    const ssize_t n = ::read(probe, got.data(), got.size());
    ::close(probe);
    return n == static_cast<ssize_t>(expected.size()) &&
           std::memcmp(got.data(), expected.data(), expected.size()) == 0;
}

template <class Gate> void wait_gate_paused(Gate& gate) {
    gate.paused.wait(false, std::memory_order_acquire);
}

template <class Gate> void resume_gate(Gate& gate) {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

FileOpen writable() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    return mode;
}

struct OwnedBackend {
    std::unique_ptr<ThreadPoolBackend> backend;
    ThreadPoolBackend* raw;
};

OwnedBackend make_threadpool(std::size_t worker_count = 4) {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{64, worker_count});
    ThreadPoolBackend* raw = backend.get();
    return OwnedBackend{std::move(backend), raw};
}

// The sequential pipeline body: fill a buffer from one file through repeated
// primitive awaits, then write it to another file and sync.
struct PipelineCopyBody {
    File* src;
    File* dst;
    std::vector<std::byte> buffer;
    std::size_t copied = 0;
    bool sync_done = false;

    PipelineCopyBody(File* s, File* d, std::size_t chunk) : src(s), dst(d), buffer(chunk) {}

    void operator()(IoTaskContext& task) {
        std::uint64_t offset = 0;
        for (;;) {
            auto r = task.read(NativeFileRef{*src}, buffer, offset);
            check(r.has_value(), "tracer read completes");
            if (!r.has_value()) {
                return;
            }
            if (r.value() == 0) {
                break;
            }
            std::size_t written = 0;
            while (written < r.value()) {
                auto w = task.write(NativeFileRef{*dst},
                                    std::span<const std::byte>(buffer.data() + written,
                                                               r.value() - written),
                                    offset + written);
                check(w.has_value(), "tracer write completes");
                if (!w.has_value()) {
                    return;
                }
                written += w.value();
            }
            offset += r.value();
            copied += r.value();
        }
        auto s = task.sync_data(NativeFileRef{*dst});
        check(s.has_value(), "tracer sync completes");
        sync_done = s.has_value();
    }
};

bool pipeline_tracer_threadpool() {
    const std::string payload(7000, 'x');
    const std::string src_path = make_temp_file(payload);
    const std::string dst_path = make_temp_file("");
    auto src_open = File::open(src_path);
    auto dst_open = File::open(dst_path, writable());
    ::unlink(src_path.c_str());
    ::unlink(dst_path.c_str());
    check(src_open.has_value() && dst_open.has_value(), "tracer opens");
    if (!src_open.has_value() || !dst_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();
    File dst = std::move(dst_open).value();

    OwnedBackend owned = make_threadpool();
    AsyncIoContext ctx{std::move(owned.backend)};
    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "host constructs");
    if (!host_r.has_value()) {
        return false;
    }
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    PipelineCopyBody body(&src, &dst, 512);
    check(host->spawn([&body](IoTaskContext& task) { body(task); }).has_value(),
          "tracer task admitted");

    auto run = host->run();
    check(run.has_value(), "tracer run succeeds");
    check(body.copied == payload.size(), "tracer copied byte count");
    check(body.sync_done, "tracer sync performed");
    check(host->test_live_task_count() == 0, "tracer no live tasks after run");
    check(host->test_slots_with_await_link() == 0, "tracer no await links after run");
    check(ctx.outstanding() == 0, "tracer no outstanding requests after run");

    auto reclaim = ctx.claim_progress_owner();
    check(reclaim.has_value(), "tracer progress owner released after run");

    const bool content_ok = open_fd_content_is(dst.native_handle(), payload);
    check(content_ok, "tracer destination content");
    return body.copied == payload.size() && body.sync_done && content_ok;
}

#if defined(SLUICE_HAS_LIBURING)

bool uring_backend_available() {
    try {
        UringAsyncBackend probe;
        return probe.available();
    } catch (...) {
        return false;
    }
}

bool pipeline_tracer_uring() {
    if (!uring_backend_available()) {
        std::printf("NOT RUN: pipeline_tracer_uring (io_uring unavailable)\n");
        return true;
    }
    const std::string payload(3000, 'u');
    const std::string src_path = make_temp_file(payload);
    const std::string dst_path = make_temp_file("");
    auto src_open = File::open(src_path);
    auto dst_open = File::open(dst_path, writable());
    ::unlink(src_path.c_str());
    ::unlink(dst_path.c_str());
    check(src_open.has_value() && dst_open.has_value(), "uring tracer opens");
    if (!src_open.has_value() || !dst_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();
    File dst = std::move(dst_open).value();

    AsyncIoContext ctx{std::make_unique<UringAsyncBackend>()};
    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "uring host constructs");
    if (!host_r.has_value()) {
        return false;
    }
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    PipelineCopyBody body(&src, &dst, 256);
    check(host->spawn([&body](IoTaskContext& task) { body(task); }).has_value(),
          "uring task admitted");
    auto run = host->run();
    check(run.has_value(), "uring run succeeds");
    check(body.copied == payload.size(), "uring copied byte count");
    check(ctx.outstanding() == 0, "uring no outstanding after run");
    const bool content_ok = open_fd_content_is(dst.native_handle(), payload);
    check(content_ok, "uring destination content");
    return body.copied == payload.size() && content_ok;
}

#endif

// One task owns accepted I/O and suspends; another task fails. The failing
// task's cleanup completes, the suspended task remains valid and settles,
// the host stays coherent. The failing task drives the pause-gate release
// itself, so no external scheduling enters the trace.
bool task_failure_while_other_io_outstanding() {
    const std::string src_path = make_temp_file(std::string(200, 'a'));
    const std::string dst_path = make_temp_file("");
    auto src_open = File::open(src_path);
    auto dst_open = File::open(dst_path, writable());
    ::unlink(src_path.c_str());
    ::unlink(dst_path.c_str());
    check(src_open.has_value() && dst_open.has_value(), "pipeline opens");
    if (!src_open.has_value() || !dst_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();
    File dst = std::move(dst_open).value();

    OwnedBackend owned = make_threadpool();
    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    owned.raw->set_worker_claimed_pause_gate(&gate);
    AsyncIoContext ctx{std::move(owned.backend)};

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    PipelineCopyBody body_a(&src, &dst, 64);
    check(host->spawn([&](IoTaskContext& task) { body_a(task); }).has_value(),
          "copy task admitted");

    check(host->spawn([&](IoTaskContext&) {
              wait_gate_paused(gate);
              resume_gate(gate);
              throw std::system_error(ENOENT, std::generic_category());
          }).has_value(),
          "failing task admitted");

    auto run = host->run();
    check(!run.has_value(), "run reports the task error");
    check(run.error().code == IoError::Code::not_found, "error maps ENOENT to not_found");
    check(body_a.copied == 200, "copy task still settles and completes");
    check(ctx.outstanding() == 0, "nothing outstanding after run");
    check(host->test_live_task_count() == 0, "no live tasks");
    check(host->test_slots_with_await_link() == 0, "no await links");
    return body_a.copied == 200 && run.error().code == IoError::Code::not_found;
}

// A task is suspended with an accepted Request; stop arrives while the
// physical operation is claimed but not terminal. The request keeps its
// responsibility: the host neither cancels nor settles it, the helper
// returns the operation's real outcome, and the task observes the stop token
// before retiring. No stack destruction; settlement and retirement complete
// before the host returns.
bool stop_during_suspension_settles() {
    const std::string src_path = make_temp_file(std::string(64, 's'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "stop-settlement open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    OwnedBackend owned = make_threadpool();
    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    owned.raw->set_worker_claimed_pause_gate(&gate);
    AsyncIoContext ctx{std::move(owned.backend)};

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> task_saw_token{false};
    std::atomic<bool> task_got_terminal{false};
    std::atomic<bool> real_outcome{false};

    std::thread stopper([&] {
        wait_gate_paused(gate);
        host->request_stop();
        resume_gate(gate);
    });

    std::vector<std::byte> buffer(64, std::byte{0});
    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read(NativeFileRef{src}, buffer, 0);
              task_got_terminal = true;
              task_saw_token = task.token().is_requested();
              real_outcome = r.has_value() && r.value() == 64;
          }).has_value(),
          "task admitted");

    auto run = host->run();
    stopper.join();
    check(run.has_value(), "run settles and returns after stop");

    check(task_got_terminal, "suspended request reached a public terminal");
    check(real_outcome, "stop did not cancel the accepted request");
    check(task_saw_token, "task observed the stop token");
    check(ctx.outstanding() == 0, "nothing outstanding after settlement");
    check(host->test_live_task_count() == 0, "task storage retired");
    check(host->test_slots_with_await_link() == 0, "await link retired with the task");
    check(host->stop_requested(), "host stop is observable");

    auto after = host->spawn([](IoTaskContext&) {});
    check(!after.has_value() && after.error().code == IoError::Code::canceled,
          "spawn admission closed after stop");
    return task_got_terminal && task_saw_token && real_outcome;
}

// The wake/observation state is reserved for the task's whole lifetime, so
// no observation-setup step exists between acceptance and suspension. A
// sibling task observes the reservation while the first task is suspended;
// the drop-responsibility arms are discriminated by the mutation builds.
bool await_state_reserved_before_suspension() {
    const std::string src_path = make_temp_file(std::string(32, 'h'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "reservation open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    OwnedBackend owned = make_threadpool();
    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    owned.raw->set_worker_claimed_pause_gate(&gate);
    AsyncIoContext ctx{std::move(owned.backend)};

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> saw_reserved_link{false};
    std::atomic<bool> a_settled{false};
    std::vector<std::byte> buffer(32, std::byte{0});

    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read(NativeFileRef{src}, buffer, 0);
              a_settled = r.has_value() && r.value() == 32;
          }).has_value(),
          "reading task admitted");

    check(host->spawn([&](IoTaskContext&) {
              saw_reserved_link = host->test_slots_with_await_link() == 1;
              resume_gate(gate);
          }).has_value(),
          "sibling task admitted");

    auto run = host->run();
    check(run.has_value(), "run succeeds");
    check(saw_reserved_link, "sibling observes the reserved await state");
    check(a_settled, "gated read settles with data");
    check(ctx.outstanding() == 0, "nothing outstanding");
    return saw_reserved_link && a_settled;
}

// A wait deadline expires while the awaited operation is still queued
// undispatched behind a claimed worker. The deadline bounds the driver's
// initial park window only: it neither cancels nor settles the request, and
// the helper returns the operation's real terminal result once the queued
// operation runs. The one-worker pool plus the occupier task makes the
// discriminator deterministic: a reinstated expiry-cancel would win before
// execution and return canceled instead of the data asserted below.
bool deadline_expiry_does_not_cancel() {
    const std::string src_path = make_temp_file(std::string(48, 't'));
    const std::string occ_path = make_temp_file(std::string(8, 'o'));
    auto src_open = File::open(src_path);
    auto occ_open = File::open(occ_path);
    ::unlink(src_path.c_str());
    ::unlink(occ_path.c_str());
    check(src_open.has_value() && occ_open.has_value(), "deadline opens");
    if (!src_open.has_value() || !occ_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();
    File occ = std::move(occ_open).value();

    OwnedBackend owned = make_threadpool(1);
    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    owned.raw->set_worker_claimed_pause_gate(&gate);
    AsyncIoContext ctx{std::move(owned.backend)};

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> occupier_done{false};
    std::atomic<bool> got_terminal{false};
    std::atomic<bool> real_outcome{false};

    // Orders the gate release after the 50ms wait deadline has fired; it
    // inflates no retry and gates no assertion.
    std::thread releaser([&] {
        wait_gate_paused(gate);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        resume_gate(gate);
    });

    check(host->spawn([&](IoTaskContext& task) {
              std::vector<std::byte> scratch(8, std::byte{0});
              auto r = task.read(NativeFileRef{occ}, scratch, 0);
              occupier_done = r.has_value() && r.value() == 8;
          }).has_value(),
          "occupier task admitted");

    std::vector<std::byte> buffer(48, std::byte{0});
    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read_for(NativeFileRef{src}, buffer, 0,
                                     std::chrono::milliseconds(50));
              got_terminal = true;
              real_outcome = r.has_value() && r.value() == 48;
          }).has_value(),
          "deadline task admitted");

    auto run = host->run();
    releaser.join();
    check(run.has_value(), "run settles after the deadline");
    check(occupier_done, "occupier read settled");
    check(got_terminal, "helper returned a terminal outcome, not a timeout escape");
    check(real_outcome, "deadline expiry did not cancel: real result returned");
    check(ctx.outstanding() == 0, "nothing outstanding after settlement");
    check(host->test_live_task_count() == 0, "tasks retired");
    return got_terminal && real_outcome && occupier_done;
}

// The request publishes, the fiber resumes, the task consumes the result
// and then throws. The original exception is preserved and reaches the run
// boundary; the consumed request leaves nothing outstanding.
bool exception_during_resumed_processing() {
    const std::string src_path = make_temp_file(std::string(16, 'e'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "resumed-throw open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> consumed_before_throw{false};
    std::vector<std::byte> buffer(16, std::byte{0});

    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read(NativeFileRef{src}, buffer, 0);
              consumed_before_throw = r.has_value() && r.value() == 16;
              throw std::runtime_error("resumed-processing failure");
          }).has_value(),
          "task admitted");

    auto run = host->run();
    check(!run.has_value(), "task error reaches the run boundary");
    check(run.error().code == IoError::Code::backend_error,
          "original exception maps unchanged (runtime_error -> backend_error)");
    check(consumed_before_throw, "result consumed before the exception");
    check(ctx.outstanding() == 0, "nothing outstanding");
    check(host->test_slots_with_await_link() == 0, "await state retired");
    return consumed_before_throw && run.error().code == IoError::Code::backend_error;
}

// The physical outcome exists but publication is paused; stop races the
// publication/retirement boundary. The request keeps its responsibility —
// the already-computed outcome publishes unchanged — the task body runs
// exactly once, and nothing touches retired storage.
bool stop_races_publication_and_retirement() {
    const std::string src_path = make_temp_file(std::string(24, 'r'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "stop-race open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    OwnedBackend owned = make_threadpool();
    ThreadPoolBackend::WorkerOutcomePreTerminalPauseGate gate;
    owned.raw->set_worker_outcome_pre_terminal_pause_gate(&gate);
    AsyncIoContext ctx{std::move(owned.backend)};

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<int> body_entries{0};
    std::atomic<bool> real_terminal{false};
    std::vector<std::byte> buffer(24, std::byte{0});

    std::thread stopper([&] {
        wait_gate_paused(gate);
        host->request_stop();
        resume_gate(gate);
    });

    check(host->spawn([&](IoTaskContext& task) {
              ++body_entries;
              auto r = task.read(NativeFileRef{src}, buffer, 0);
              real_terminal = r.has_value() && r.value() == 24;
          }).has_value(),
          "task admitted");

    auto run = host->run();
    stopper.join();
    check(run.has_value(), "run settles after the race");
    check(body_entries == 1, "task body entered exactly once");
    check(real_terminal, "real outcome published under the stop race");
    check(ctx.outstanding() == 0, "nothing outstanding");
    check(host->test_live_task_count() == 0, "task retired once");
    check(host->test_slots_with_await_link() == 0, "no delivery touches retired storage");
    return body_entries == 1 && real_terminal;
}

// The host is the context's only driver: an external owner blocks run()
// before any task executes; a RequestScope inside a host task cannot steal
// the owner; a concurrent claim during run() fails; the owner is released
// again after run() returns.
bool progress_owner_composition() {
    OwnedBackend owned = make_threadpool();
    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    owned.raw->set_worker_claimed_pause_gate(&gate);
    AsyncIoContext ctx{std::move(owned.backend)};
    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "owner-composition host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> task_ran{false};
    check(host->spawn([&](IoTaskContext&) { task_ran = true; }).has_value(),
          "owner-composition probe task admitted");

    {
        auto external = ctx.claim_progress_owner();
        check(external.has_value(), "external owner claims first");
        auto rejected = host->run();
        check(!rejected.has_value() && rejected.error().code == IoError::Code::invalid_state,
              "run on externally-owned context fails before task execution");
        check(!task_ran, "no task ran while the owner was external");
    }

    std::atomic<bool> scope_rejected{false};
    check(host->spawn([&](IoTaskContext&) {
              try {
                  RequestScope scope{ctx, 4, ScopeCleanupPolicy::drain};
              } catch (const std::runtime_error&) {
                  scope_rejected = true;
              }
          }).has_value(),
          "scope-composition task admitted");

    std::atomic<bool> gated_read_settled{false};
    check(host->spawn([&](IoTaskContext& task) {
              std::vector<std::byte> scratch(8, std::byte{0});
              auto src_open = File::open(make_temp_file(std::string(8, 'p')));
              if (src_open.has_value()) {
                  File src = std::move(src_open).value();
                  auto r = task.read(NativeFileRef{src}, scratch, 0);
                  check(r.has_value() && r.value() == 8, "owner-composition gated read settles");
                  gated_read_settled = r.has_value() && r.value() == 8;
              }
          }).has_value(),
          "owner-composition gated task admitted");

    // The claimer attempts while the host provably holds the owner (the
    // worker is paused on the gated read submitted from inside run()), and
    // only then releases the gate so the trace cannot complete early.
    std::atomic<bool> concurrent_claim_rejected{false};
    std::thread claimer([&] {
        wait_gate_paused(gate);
        auto attempt = ctx.claim_progress_owner();
        concurrent_claim_rejected = !attempt.has_value();
        resume_gate(gate);
    });

    auto run = host->run();
    claimer.join();
    check(run.has_value(), "owner-composition run succeeds");
    check(task_ran, "tasks ran after the external owner released");
    check(scope_rejected, "RequestScope cannot steal the host-owned driver");
    check(concurrent_claim_rejected, "concurrent claim during run fails");
    check(gated_read_settled, "gated read settled under the host owner");

    auto reclaimed = ctx.claim_progress_owner();
    check(reclaimed.has_value(), "owner released after run");
    return task_ran && scope_rejected && concurrent_claim_rejected && gated_read_settled;
}

// Task admission is bounded by the configured capacity and the bound is
// visible; rejected spawns leave no slot taken.
bool bounds_task_capacity_rejects() {
    AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
    StackfulHostConfig config;
    config.task_capacity = 2;
    auto host_r = StackfulIoHost::create(ctx, config);
    check(host_r.has_value(), "bounded host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    check(host->task_capacity() == 2, "capacity visible");

    auto a = host->spawn([](IoTaskContext&) {});
    auto b = host->spawn([](IoTaskContext&) {});
    check(a.has_value() && b.has_value(), "first two spawns admitted");
    auto c = host->spawn([](IoTaskContext&) {});
    check(!c.has_value() && c.error().code == IoError::Code::no_space,
          "third spawn rejected by the task bound");

    check(host->run().has_value(), "bounded run succeeds");

    auto d = host->spawn([](IoTaskContext&) {});
    check(d.has_value(), "slots recycle after retirement");
    check(host->run().has_value(), "second run succeeds");

    auto empty = host->spawn(std::function<void(IoTaskContext&)>{});
    check(!empty.has_value(), "null task rejected");

    host->request_stop();
    auto stopped = host->spawn([](IoTaskContext&) {});
    check(!stopped.has_value() && stopped.error().code == IoError::Code::canceled,
          "spawn closed after stop");
    return true;
}

bool invalid_configuration_rejected() {
    AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};

    StackfulHostConfig zero_tasks;
    zero_tasks.task_capacity = 0;
    check(!StackfulIoHost::create(ctx, zero_tasks).has_value(), "zero capacity rejected");

    StackfulHostConfig tiny_stack;
    tiny_stack.stack_bytes = 4096;
    check(!StackfulIoHost::create(ctx, tiny_stack).has_value(), "tiny stack rejected");
    return true;
}

// A backend that cannot signal physical progress cannot drive the host's
// wait protocol; admitting it would park the driver into an undrivable
// dead-end after the first suspension, so construction must refuse it.
bool create_rejects_non_signaling_backend() {
    class NullBackend final : public AsyncBackend {
      public:
        std::size_t poll() override { return 0; }
        std::size_t outstanding() const noexcept override { return 0; }
        bool internal_work_retired() const noexcept override { return true; }
        std::size_t slot_capacity() const noexcept override { return 0; }
        detail::PublicCancel cancel_identity(detail::RequestKey) override {
            return detail::PublicCancel::not_found;
        }
        Result<detail::RequestKey> submit_read(ReadOp, Completion<std::size_t>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_write(WriteOp, Completion<std::size_t>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_sync_data(SyncDataOp, Completion<void>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_sync_all(SyncAllOp, Completion<void>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_file_info(FileInfoOp, Completion<sluice::FileInfo>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
        Result<detail::RequestKey> submit_size(SizeOp, Completion<sluice::FileSize>*) override {
            return sluice::make_unexpected<detail::RequestKey>(
                IoError{IoError::Code::not_supported});
        }
    };

    AsyncIoContext ctx{std::make_unique<NullBackend>()};
    auto host = StackfulIoHost::create(ctx, StackfulHostConfig{});
    const bool rejected =
        !host.has_value() && host.error().code == IoError::Code::not_supported;
    check(rejected, "create rejects a backend that cannot drive the wait protocol");
    return rejected;
}

bool destroying_host_with_live_task_fails_fast() {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        ::alarm(30);
        AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
        auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
        if (!host_r.has_value()) {
            std::_Exit(0);
        }
        std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();
        (void)host->spawn([](IoTaskContext&) {});
        host.reset();
        std::_Exit(0);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        return false;
    }
    const bool aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    check(aborted, "destroying the host with a spawned, unretired task fails fast");
    return aborted;
}

bool nested_run_from_a_task_rejected() {
    AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "nested-run host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> inner_rejected{false};
    check(host->spawn([&](IoTaskContext&) {
              auto nested = host->run();
              inner_rejected = !nested.has_value();
          }).has_value(),
          "nested-run task admitted");
    auto run = host->run();
    check(run.has_value(), "outer run completes");
    check(inner_rejected, "nested run from a task rejected");
    return inner_rejected;
}

// After the host stop is requested, a task's await must reject before
// acceptance, never accept and then abandon. The gate proves no physical
// claim ever happened.
bool await_after_stop_rejects_before_acceptance() {
    const std::string src_path = make_temp_file(std::string(8, 'o'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "stop-await open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    OwnedBackend owned = make_threadpool();
    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    owned.raw->set_worker_claimed_pause_gate(&gate);
    AsyncIoContext ctx{std::move(owned.backend)};

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "stop-await host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    check(host->spawn([&](IoTaskContext&) { host->request_stop(); }).has_value(),
          "stop task admitted");

    std::atomic<bool> rejected_before_acceptance{false};
    std::vector<std::byte> buffer(8, std::byte{0});
    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read(NativeFileRef{src}, buffer, 0);
              rejected_before_acceptance =
                  !r.has_value() && r.error().code == IoError::Code::canceled;
          }).has_value(),
          "await task admitted");

    auto run = host->run();
    check(run.has_value(), "stop-await run settles");
    check(rejected_before_acceptance, "await after stop rejects with canceled");
    check(!gate.paused.load(std::memory_order_acquire),
          "no physical claim happened for the rejected await");
    check(ctx.outstanding() == 0, "stop-await nothing outstanding");
    return rejected_before_acceptance;
}

// A failed run must not poison the next run: the first-error accumulator is
// per-run state, reset at each successful run entry before any task executes.
bool run_failure_does_not_poison_the_next_run() {
    AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "run-reuse host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    check(host->spawn([](IoTaskContext&) {
              throw std::runtime_error("first run failure");
          }).has_value(),
          "run-reuse failing task admitted");
    auto run1 = host->run();
    check(!run1.has_value() && run1.error().code == IoError::Code::backend_error,
          "run-reuse run1 reports its own task error");

    std::atomic<bool> second_ran{false};
    check(host->spawn([&](IoTaskContext&) { second_ran = true; }).has_value(),
          "run-reuse run2 task admitted");
    auto run2 = host->run();
    check(second_ran, "run-reuse run2 task executed");
    check(run2.has_value(), "run-reuse run2 succeeds: run1's error does not persist");
    return run2.has_value() && second_ran;
}

// Host stop must not write the context's control plane: a stop that lands
// while the driver is executing a task (so the run never waits) plants
// nothing in the notification domain, and no unacknowledged control is left
// for the next progress owner. Readability of the borrowed notification fd
// at stop time is the direct observation of a planted control.
bool host_stop_does_not_plant_context_control() {
    OwnedBackend owned = make_threadpool();
    AsyncIoContext ctx{std::move(owned.backend)};
    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "control-plane host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> task_started{false};
    std::atomic<bool> release_task{false};
    std::atomic<bool> stop_wrote_notification{false};
    std::thread stopper([&] {
        task_started.wait(false, std::memory_order_acquire);
        host->request_stop();
        struct pollfd pfd;
        pfd.fd = ctx.progress_notification_fd();
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int readable = ::poll(&pfd, 1, 0);
        stop_wrote_notification.store(readable == 1 && (pfd.revents & POLLIN) != 0,
                                      std::memory_order_release);
        ctx.detach_progress_host();
        release_task.store(true, std::memory_order_release);
        release_task.notify_all();
    });

    check(host->spawn([&](IoTaskContext&) {
              task_started.store(true, std::memory_order_release);
              task_started.notify_all();
              release_task.wait(false, std::memory_order_acquire);
          }).has_value(),
          "control-plane task admitted");

    auto run = host->run();
    stopper.join();
    check(run.has_value(), "control-plane run settles after stop");
    check(!stop_wrote_notification.load(std::memory_order_acquire),
          "control-plane: host stop writes nothing to the notification domain");

    auto owner = ctx.claim_progress_owner();
    check(owner.has_value(), "control-plane owner reclaimable after run");
    auto wait = ctx.wait_one();
    check(wait.has_value(), "control-plane wait_one succeeds after the stopped run");
    check(wait.has_value() &&
              wait.value().kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress,
          "control-plane: no stale control from the host stop reaches the next owner");
    return !stop_wrote_notification.load(std::memory_order_acquire) && wait.has_value() &&
           wait.value().kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress;
}

// A victim request is accepted while the only worker is occupied inside
// another operation, so the victim is queued and undispatched when host stop
// lands; the occupier then releases. A stop path that reintroduced
// cancellation of suspended awaits would win before execution and return
// canceled; the victim must instead execute naturally and report the real
// result. The accepted-pre-dispatch pause proves the stop follows the
// victim's acceptance and precedes its dispatch (so also its execution
// claim); the worker-claimed pause proves the only worker never left the
// occupier before the stop.
bool stop_leaves_undispatched_accepted_io_uncanceled() {
    const std::string src_path = make_temp_file(std::string(48, 'v'));
    const std::string occ_path = make_temp_file(std::string(8, 'o'));
    auto src_open = File::open(src_path);
    auto occ_open = File::open(occ_path);
    ::unlink(src_path.c_str());
    ::unlink(occ_path.c_str());
    check(src_open.has_value() && occ_open.has_value(), "undispatched-stop opens");
    if (!src_open.has_value() || !occ_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();
    File occ = std::move(occ_open).value();

    OwnedBackend owned = make_threadpool(1);
    ThreadPoolBackend::WorkerClaimedPauseGate claimed_gate;
    owned.raw->set_worker_claimed_pause_gate(&claimed_gate);
    AsyncIoContext ctx{std::move(owned.backend)};

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "undispatched-stop host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::vector<std::byte> occ_buffer(8, std::byte{0});
    auto occ_submit =
        ctx.submit_read(ReadOp{NativeFileRef{occ}, occ_buffer.data(), occ_buffer.size(), 0});
    check(occ_submit.has_value(), "occupier accepted");
    if (!occ_submit.has_value()) {
        return false;
    }
    Request<std::size_t> occupier = std::move(occ_submit).value();

    wait_gate_paused(claimed_gate);

    ThreadPoolBackend::AcceptedPreDispatchPauseGate accepted_gate;
    owned.raw->set_accepted_pre_dispatch_pause_gate(&accepted_gate);

    std::atomic<bool> victim_real_outcome{false};
    std::vector<std::byte> buffer(48, std::byte{0});
    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read(NativeFileRef{src}, buffer, 0);
              victim_real_outcome = r.has_value() && r.value() == 48;
          }).has_value(),
          "undispatched-stop victim admitted");

    std::thread stopper([&] {
        wait_gate_paused(accepted_gate);
        host->request_stop();
        resume_gate(accepted_gate);
        resume_gate(claimed_gate);
    });

    auto run = host->run();
    stopper.join();

    check(run.has_value(), "undispatched-stop run settles");
    check(victim_real_outcome, "undispatched accepted request executed naturally after stop");
    const auto observed = occupier.take_result();
    check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
              observed.result.value() == 8,
          "occupier settled with its real result");
    check(ctx.outstanding() == 0, "undispatched-stop nothing outstanding");
    check(host->test_live_task_count() == 0, "undispatched-stop tasks retired");
    check(host->test_slots_with_await_link() == 0, "undispatched-stop await links retired");
    check(host->stop_requested(), "undispatched-stop stop observed");
    return run.has_value() && victim_real_outcome && observed.readiness == RequestReadiness::ready;
}

// An expired wait deadline must stop constraining the driver's park: after
// expiry the driver parks unboundedly instead of spinning zero-duration
// waits. The one-worker pool holds physical completion far past the
// deadline; the context's existing poll/wait counters bound the driver's
// activity well below any spin rate, and the victim still settles on its
// real result.
bool expired_deadline_parks_instead_of_spinning() {
    const std::string src_path = make_temp_file(std::string(48, 't'));
    const std::string occ_path = make_temp_file(std::string(8, 'o'));
    auto src_open = File::open(src_path);
    auto occ_open = File::open(occ_path);
    ::unlink(src_path.c_str());
    ::unlink(occ_path.c_str());
    check(src_open.has_value() && occ_open.has_value(), "deadline-spin opens");
    if (!src_open.has_value() || !occ_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();
    File occ = std::move(occ_open).value();

    OwnedBackend owned = make_threadpool(1);
    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    owned.raw->set_worker_claimed_pause_gate(&gate);
    AsyncStats stats;
    AsyncIoContext ctx{std::move(owned.backend), &stats};

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "deadline-spin host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> occupier_done{false};
    std::atomic<bool> real_outcome{false};

    std::thread releaser([&] {
        wait_gate_paused(gate);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        resume_gate(gate);
    });

    check(host->spawn([&](IoTaskContext& task) {
              std::vector<std::byte> scratch(8, std::byte{0});
              auto r = task.read(NativeFileRef{occ}, scratch, 0);
              occupier_done = r.has_value() && r.value() == 8;
          }).has_value(),
          "deadline-spin occupier admitted");

    std::vector<std::byte> buffer(48, std::byte{0});
    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read_for(NativeFileRef{src}, buffer, 0,
                                     std::chrono::milliseconds(50));
              real_outcome = r.has_value() && r.value() == 48;
          }).has_value(),
          "deadline-spin victim admitted");

    auto run = host->run();
    releaser.join();

    check(run.has_value(), "deadline-spin run settles");
    check(occupier_done, "deadline-spin occupier settled");
    check(real_outcome, "deadline-spin expiry still returns the real result");
    check(stats.wait_calls <= 64, "deadline-spin wait calls stay bounded after expiry");
    check(stats.poll_calls <= 256, "deadline-spin poll calls stay bounded after expiry");
    check(ctx.outstanding() == 0, "deadline-spin nothing outstanding");
    check(host->test_live_task_count() == 0, "deadline-spin tasks retired");
    return run.has_value() && real_outcome && occupier_done && stats.wait_calls <= 64 &&
           stats.poll_calls <= 256;
}

// External progress-source control must be consumed by the host as the
// context's progress owner: observed, acknowledged, and not left behind.
// The worker-claimed pause holds the task's operation; the prepark pause
// proves the driver committed to a park with the notification fd drained,
// so the control planted next cannot be missed. Two legal completions exist
// — the next wait reports the control, or a reaped completion returns
// progress first and the control is retired at run exit — and both must
// leave the next owner clean, which the post-run new-owner wait observes
// directly.
bool external_control_acknowledged_before_next_owner() {
    const std::string src_path = make_temp_file(std::string(32, 'c'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "control-ack open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    OwnedBackend owned = make_threadpool(1);
    ThreadPoolBackend::WorkerClaimedPauseGate claimed_gate;
    owned.raw->set_worker_claimed_pause_gate(&claimed_gate);
    AsyncStats stats;
    AsyncIoContext ctx{std::move(owned.backend), &stats};
    detail::ProgressSource::PauseGate prepark_gate;
    ctx.set_progress_prepark_pause_gate_for_test(&prepark_gate);

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "control-ack host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> real_outcome{false};
    std::vector<std::byte> buffer(32, std::byte{0});
    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read(NativeFileRef{src}, buffer, 0);
              real_outcome = r.has_value() && r.value() == 32;
          }).has_value(),
          "control-ack task admitted");

    std::thread controller([&] {
        wait_gate_paused(claimed_gate);
        wait_gate_paused(prepark_gate);
        ctx.interrupt_progress_waiters();
        resume_gate(prepark_gate);
        resume_gate(claimed_gate);
    });

    auto run = host->run();
    controller.join();

    check(run.has_value(), "control-ack run settles");
    check(real_outcome, "control-ack task settled on its real result");
    check(stats.wait_calls <= 64, "control-ack wait calls stay bounded");
    check(stats.poll_calls <= 256, "control-ack poll calls stay bounded");
    check(ctx.outstanding() == 0, "control-ack nothing outstanding");
    check(host->test_live_task_count() == 0, "control-ack tasks retired");

    auto owner = ctx.claim_progress_owner();
    check(owner.has_value(), "control-ack next owner claims the context");
    auto probe = ctx.wait_one();
    check(probe.has_value() &&
              probe.value().kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress,
          "control-ack: no stale control reaches the next owner");
    return run.has_value() && real_outcome && probe.has_value() &&
           probe.value().kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress;
}

// The wait path may reap a completion in the pass that follows a control
// wake and return progress without ever reporting the control. Holding the
// driver at its prepark pause while the worker publishes forces exactly
// that interleaving — the pass after the control wake reaps the published
// completion first — so the run-exit observation is the only thing standing
// between that control and the next owner.
bool control_consumed_by_progress_return_is_retired_at_exit() {
    const std::string src_path = make_temp_file(std::string(32, 'k'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "exit-control open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    OwnedBackend owned = make_threadpool(1);
    ThreadPoolBackend::WorkerClaimedPauseGate claimed_gate;
    owned.raw->set_worker_claimed_pause_gate(&claimed_gate);
    detail::ProgressSource::PauseGate prepark_gate;
    AsyncIoContext ctx{std::move(owned.backend)};
    ctx.set_progress_prepark_pause_gate_for_test(&prepark_gate);

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "exit-control host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> real_outcome{false};
    std::vector<std::byte> buffer(32, std::byte{0});
    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read(NativeFileRef{src}, buffer, 0);
              real_outcome = r.has_value() && r.value() == 32;
          }).has_value(),
          "exit-control task admitted");

    std::thread controller([&] {
        wait_gate_paused(claimed_gate);
        wait_gate_paused(prepark_gate);
        ctx.interrupt_progress_waiters();
        const std::uint64_t progress_before = ctx.progress_token_for_test().progress;
        resume_gate(claimed_gate);
        while (ctx.progress_token_for_test().progress == progress_before) {
            std::this_thread::yield();
        }
        resume_gate(prepark_gate);
    });

    auto run = host->run();
    controller.join();

    check(run.has_value(), "exit-control run settles");
    check(real_outcome, "exit-control task settled on its real result");
    check(ctx.outstanding() == 0, "exit-control nothing outstanding");
    check(host->test_live_task_count() == 0, "exit-control tasks retired");

    auto owner = ctx.claim_progress_owner();
    check(owner.has_value(), "exit-control next owner claims the context");
    auto probe = ctx.wait_one();
    check(probe.has_value() &&
              probe.value().kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress,
          "exit-control: control consumed by a progress return is retired at exit");
    return run.has_value() && real_outcome && probe.has_value() &&
           probe.value().kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress;
}

// A running task may spawn a successor, bounded by the same task capacity:
// at capacity 1 the nested spawn is refused while the parent occupies the
// only slot and the host still retires cleanly; at capacity 2 the child is
// admitted, runs exactly once after the parent completes, and both retire.
bool nested_spawn_bounded_by_capacity() {
    bool cap1_ok = false;
    {
        AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
        StackfulHostConfig config;
        config.task_capacity = 1;
        auto host_r = StackfulIoHost::create(ctx, config);
        check(host_r.has_value(), "nested capacity-1 host constructs");
        std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

        std::atomic<bool> child_refused{false};
        std::atomic<bool> child_ran{false};
        std::atomic<bool> parent_completed{false};
        check(host->spawn([&](IoTaskContext&) {
                  auto child = host->spawn([&](IoTaskContext&) { child_ran = true; });
                  child_refused = !child.has_value() &&
                                  child.error().code == IoError::Code::no_space;
                  parent_completed = true;
              }).has_value(),
              "nested capacity-1 parent admitted");

        auto run = host->run();
        check(run.has_value(), "nested capacity-1 run succeeds");
        check(child_refused, "nested spawn at capacity reports no_space");
        check(!child_ran, "refused nested spawn never runs");
        check(parent_completed, "nested capacity-1 parent completed");
        check(host->test_live_task_count() == 0, "nested capacity-1 no live tasks");

        auto recycled = host->spawn([](IoTaskContext&) {});
        check(recycled.has_value(), "slot recycles after the refused nested spawn retires");
        check(host->run().has_value(), "nested capacity-1 second run succeeds");
        cap1_ok = run.has_value() && child_refused && !child_ran && recycled.has_value();
    }
    {
        AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
        StackfulHostConfig config;
        config.task_capacity = 2;
        auto host_r = StackfulIoHost::create(ctx, config);
        check(host_r.has_value(), "nested capacity-2 host constructs");
        std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

        std::atomic<int> child_entries{0};
        std::atomic<bool> child_admitted{false};
        std::atomic<bool> parent_done{false};
        std::atomic<bool> parent_done_before_child{false};
        std::atomic<bool> saw_two_live_tasks{false};
        std::atomic<bool> saw_child_queued{false};
        check(host->spawn([&](IoTaskContext&) {
                  auto child = host->spawn(
                      [&](IoTaskContext&) {
                          ++child_entries;
                          parent_done_before_child = parent_done.load();
                      });
                  child_admitted = child.has_value();
                  saw_two_live_tasks = host->test_live_task_count() == 2;
                  saw_child_queued = host->test_ready_ring_size() == 1;
                  parent_done = true;
              }).has_value(),
              "nested capacity-2 parent admitted");

        auto run = host->run();
        check(run.has_value(), "nested capacity-2 run succeeds");
        check(child_admitted, "nested capacity-2 child admitted");
        check(child_entries == 1, "nested child runs exactly once");
        check(parent_done_before_child, "child runs only after the parent completes");
        check(saw_two_live_tasks, "parent observes both live tasks");
        check(saw_child_queued, "parent observes the queued child within the bound");
        check(host->test_live_task_count() == 0, "nested capacity-2 no live tasks");
        check(host->test_slots_with_await_link() == 0, "nested capacity-2 no await links");
        return cap1_ok && run.has_value() && child_admitted && child_entries == 1 &&
               parent_done_before_child && saw_two_live_tasks && saw_child_queued;
    }
}

// run() reports the first task error in execution order, not the last and
// not an admission-order accident: immediate failures in both admission
// orders pick their own first error, and a task that fails only after a
// suspended read loses to a later-admitted task that fails first.
bool first_task_error_selected_by_execution_order() {
    {
        AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
        auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
        check(host_r.has_value(), "first-error not-found host constructs");
        std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();
        check(host->spawn([](IoTaskContext&) {
                  throw std::system_error(ENOENT, std::generic_category());
              }).has_value(),
              "first-error ENOENT-first admitted");
        check(host->spawn([](IoTaskContext&) {
                  throw std::system_error(EACCES, std::generic_category());
              }).has_value(),
              "first-error EACCES-second admitted");
        auto run = host->run();
        check(!run.has_value() && run.error().code == IoError::Code::not_found,
              "first admitted error wins when it executes first");
        if (run.has_value() || run.error().code != IoError::Code::not_found) {
            return false;
        }
    }
    {
        AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
        auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
        check(host_r.has_value(), "first-error denied host constructs");
        std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();
        check(host->spawn([](IoTaskContext&) {
                  throw std::system_error(EACCES, std::generic_category());
              }).has_value(),
              "first-error EACCES-first admitted");
        check(host->spawn([](IoTaskContext&) {
                  throw std::system_error(ENOENT, std::generic_category());
              }).has_value(),
              "first-error ENOENT-second admitted");
        auto run = host->run();
        check(!run.has_value() && run.error().code == IoError::Code::permission_denied,
              "reversed admission picks the reversed first error");
        if (run.has_value() || run.error().code != IoError::Code::permission_denied) {
            return false;
        }
    }
    {
        const std::string src_path = make_temp_file(std::string(16, 'f'));
        auto src_open = File::open(src_path);
        ::unlink(src_path.c_str());
        check(src_open.has_value(), "first-error interleaved open");
        if (!src_open.has_value()) {
            return false;
        }
        File src = std::move(src_open).value();

        OwnedBackend owned = make_threadpool();
        ThreadPoolBackend::WorkerClaimedPauseGate gate;
        owned.raw->set_worker_claimed_pause_gate(&gate);
        AsyncIoContext ctx{std::move(owned.backend)};
        auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
        check(host_r.has_value(), "first-error interleaved host constructs");
        std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

        std::atomic<bool> suspended_read_settled{false};
        std::vector<std::byte> buffer(16, std::byte{0});
        check(host->spawn([&](IoTaskContext& task) {
                  auto r = task.read(NativeFileRef{src}, buffer, 0);
                  suspended_read_settled = r.has_value() && r.value() == 16;
                  throw std::system_error(ENOENT, std::generic_category());
              }).has_value(),
              "first-error gated task admitted first");

        check(host->spawn([](IoTaskContext&) {
                  throw std::system_error(EACCES, std::generic_category());
              }).has_value(),
              "first-error immediate task admitted second");

        std::thread releaser([&] {
            wait_gate_paused(gate);
            resume_gate(gate);
        });

        auto run = host->run();
        releaser.join();
        check(!run.has_value() && run.error().code == IoError::Code::permission_denied,
              "the error that lands first in execution wins, not the last");
        check(suspended_read_settled, "the later-failing task still settles its read first");
        check(ctx.outstanding() == 0, "first-error interleaved nothing outstanding");
        check(host->test_live_task_count() == 0, "first-error interleaved tasks retired");
        return !run.has_value() && run.error().code == IoError::Code::permission_denied &&
               suspended_read_settled;
    }
}

// The host exact/all conveniences compose primitive awaits with the shared
// composition rule: a full request reports completion with the confirmed
// byte count, a short-then-EOF read reports EOF-before-full with the
// confirmed prefix, and a full write reports completion with the payload
// on disk.
bool host_composition_reports_canonical_outcomes() {
    {
        const std::string path = make_temp_file("abcdefgh");
        auto open = File::open(path);
        ::unlink(path.c_str());
        check(open.has_value(), "composition full-read open");
        if (!open.has_value()) {
            return false;
        }
        File src = std::move(open).value();

        AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
        auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
        std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

        std::atomic<bool> full_ok{false};
        std::vector<std::byte> dst(8, std::byte{0});
        check(host->spawn([&](IoTaskContext& task) {
                  auto composed = task.read_exact(NativeFileRef{src}, dst, 0);
                  full_ok = composed.has_value() && composed.value().complete() &&
                            composed.value().confirmed_bytes == 8 && dst[0] == std::byte{'a'} &&
                            dst[7] == std::byte{'h'};
              }).has_value(),
              "composition full-read task admitted");
        check(host->run().has_value(), "composition full-read run succeeds");
        check(full_ok, "read_exact completes a full request");
        check(ctx.outstanding() == 0, "composition full-read nothing outstanding");
        if (!full_ok) {
            return false;
        }
    }
    {
        const std::string path = make_temp_file("abcde");
        auto open = File::open(path);
        ::unlink(path.c_str());
        check(open.has_value(), "composition eof-read open");
        if (!open.has_value()) {
            return false;
        }
        File src = std::move(open).value();

        AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
        auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
        std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

        std::atomic<bool> eof_ok{false};
        std::vector<std::byte> dst(16, std::byte{0});
        check(host->spawn([&](IoTaskContext& task) {
                  auto composed = task.read_exact(NativeFileRef{src}, dst, 0);
                  eof_ok = composed.has_value() &&
                           composed.value().end == CompositionEnd::eof_before_full &&
                           composed.value().confirmed_bytes == 5 && dst[4] == std::byte{'e'};
              }).has_value(),
              "composition eof-read task admitted");
        check(host->run().has_value(), "composition eof-read run succeeds");
        check(eof_ok, "read_exact reports EOF before full with the confirmed prefix");
        if (!eof_ok) {
            return false;
        }
    }
    {
        const std::string path = make_temp_file("");
        auto open = File::open(path, writable());
        ::unlink(path.c_str());
        check(open.has_value(), "composition write open");
        if (!open.has_value()) {
            return false;
        }
        File dst_file = std::move(open).value();

        AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
        auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
        std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

        const std::string payload = "host composition payload";
        std::vector<std::byte> src(payload.size());
        for (std::size_t i = 0; i < payload.size(); ++i) {
            src[i] = static_cast<std::byte>(payload[i]);
        }
        std::atomic<bool> write_ok{false};
        check(host->spawn([&](IoTaskContext& task) {
                  auto composed =
                      task.write_all(NativeFileRef{dst_file},
                                     std::span<const std::byte>(src.data(), src.size()), 0);
                  write_ok = composed.has_value() && composed.value().complete() &&
                             composed.value().confirmed_bytes == payload.size();
              }).has_value(),
              "composition write task admitted");
        check(host->run().has_value(), "composition write run succeeds");
        check(write_ok, "write_all completes a full request");
        const bool content_ok = open_fd_content_is(dst_file.native_handle(), payload);
        check(content_ok, "write_all payload reaches the file");
        check(ctx.outstanding() == 0, "composition write nothing outstanding");
        return write_ok && content_ok;
    }
}

// Host stop ends an exact/all composition at the next primitive boundary:
// the already-confirmed prefix survives, the next primitive rejects before
// acceptance with canceled, and no additional physical operation runs after
// the stop. The worker-claimed pause holds the first primitive's physical
// completion across the stop so the boundary is deterministic; the backend
// syscall count proves exactly one primitive reached the kernel.
bool host_composition_stops_at_new_acceptance_under_stop() {
    const std::string src_path = make_temp_file(std::string(24, 'p'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "composition stop open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    OwnedBackend owned = make_threadpool();
    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    owned.raw->set_worker_claimed_pause_gate(&gate);
    AsyncIoContext ctx{std::move(owned.backend)};
    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "composition stop host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::thread stopper([&] {
        wait_gate_paused(gate);
        host->request_stop();
        resume_gate(gate);
    });

    std::atomic<bool> stop_outcome_ok{false};
    std::vector<std::byte> dst(48, std::byte{0});
    check(host->spawn([&](IoTaskContext& task) {
              auto composed = task.read_exact(NativeFileRef{src}, dst, 0);
              stop_outcome_ok = composed.has_value() &&
                                composed.value().end == CompositionEnd::primitive_error &&
                                composed.value().error.has_value() &&
                                composed.value().error->code == IoError::Code::canceled &&
                                composed.value().confirmed_bytes == 24;
          }).has_value(),
          "composition stop task admitted");

    auto run = host->run();
    stopper.join();
    check(run.has_value(), "composition stop run settles");
    check(stop_outcome_ok, "composition under stop keeps the prefix and rejects at the boundary");
    check(owned.raw->syscall_count_for_test() == 1,
          "no second primitive dispatched after the stop");
    check(ctx.outstanding() == 0, "composition stop nothing outstanding");
    check(host->test_live_task_count() == 0, "composition stop tasks retired");
    return run.has_value() && stop_outcome_ok && owned.raw->syscall_count_for_test() == 1;
}

bool same_composition_result(const Result<CompositionOutcome>& direct,
                             const Result<CompositionOutcome>& host_result) {
    if (direct.has_value() != host_result.has_value()) {
        return false;
    }
    if (!direct.has_value()) {
        return direct.error().code == host_result.error().code;
    }
    const CompositionOutcome& a = direct.value();
    const CompositionOutcome& b = host_result.value();
    return a.confirmed_bytes == b.confirmed_bytes && a.end == b.end &&
           a.remaining == b.remaining && a.error.has_value() == b.error.has_value() &&
           (!a.error.has_value() || a.error->code == b.error->code);
}

// The host exact/all conveniences refine the canonical direct invocation
// semantics at their boundary: whole-invocation rejections (closed file,
// illegal access, invalid range, stopped host) surface as outer result
// errors with nothing accepted and no kernel operation, matching the direct
// forms called with identical inputs. A zero-length invocation still crosses
// admission as one no-op request and completes without a data operation.
// Failures inside an established composition stay in the outcome with the
// confirmed prefix — including a backend error after a confirmed step, whose
// request never reached the kernel.
bool host_convenience_parity_with_direct_invocation_semantics() {
    {
        const std::string read_path = make_temp_file("abcdefgh");
        const std::string short_path = make_temp_file("abcde");
        const std::string closed_path = make_temp_file("xy");
        const std::string write_only_path = make_temp_file("zz");
        const std::string rw_path = make_temp_file("");
        auto read_open = File::open(read_path);
        auto short_open = File::open(short_path);
        auto closed_open = File::open(closed_path);
        FileOpen write_only_mode;
        write_only_mode.access = FileAccess::write_only;
        auto write_only_open = File::open(write_only_path, write_only_mode);
        auto rw_open = File::open(rw_path, writable());
        ::unlink(read_path.c_str());
        ::unlink(short_path.c_str());
        ::unlink(closed_path.c_str());
        ::unlink(write_only_path.c_str());
        ::unlink(rw_path.c_str());
        const bool opens_ok = read_open.has_value() && short_open.has_value() &&
                              closed_open.has_value() && write_only_open.has_value() &&
                              rw_open.has_value();
        check(opens_ok, "parity opens");
        if (!opens_ok) {
            return false;
        }
        File read_file = std::move(read_open).value();
        File short_file = std::move(short_open).value();
        File closed_file = std::move(closed_open).value();
        (void)closed_file.close();
        File write_only_file = std::move(write_only_open).value();
        File rw_file = std::move(rw_open).value();

        std::vector<std::byte> empty_dst;
        std::vector<std::byte> empty_src;
        std::vector<std::byte> dst8(8, std::byte{0});
        std::vector<std::byte> src8(8, std::byte{'s'});
        std::vector<std::byte> full_dst(8, std::byte{0});
        std::vector<std::byte> eof_dst(16, std::byte{0});
        const std::uint64_t invalid_offset = std::numeric_limits<std::uint64_t>::max();

        const Result<CompositionOutcome> d_closed_empty =
            sluice::blocking::read_exact_at(closed_file, 0, empty_dst);
        const Result<CompositionOutcome> d_write_only_empty =
            sluice::blocking::read_exact_at(write_only_file, 0, empty_dst);
        const Result<CompositionOutcome> d_closed_nonempty =
            sluice::blocking::read_exact_at(closed_file, 0, dst8);
        const Result<CompositionOutcome> d_write_only_nonempty =
            sluice::blocking::read_exact_at(write_only_file, 0, dst8);
        const Result<CompositionOutcome> d_range_invalid =
            sluice::blocking::read_exact_at(read_file, invalid_offset, dst8);
        const Result<CompositionOutcome> d_read_only_empty_write =
            sluice::blocking::write_all_at(read_file, 0, empty_src);
        const Result<CompositionOutcome> d_read_only_nonempty_write =
            sluice::blocking::write_all_at(read_file, 0, src8);
        const Result<CompositionOutcome> d_valid_empty_read =
            sluice::blocking::read_exact_at(read_file, 0, empty_dst);
        const Result<CompositionOutcome> d_valid_empty_write =
            sluice::blocking::write_all_at(rw_file, 0, empty_src);
        const Result<CompositionOutcome> d_full =
            sluice::blocking::read_exact_at(read_file, 0, full_dst);
        const Result<CompositionOutcome> d_eof =
            sluice::blocking::read_exact_at(short_file, 0, eof_dst);

        OwnedBackend owned = make_threadpool();
        AsyncIoContext ctx{std::move(owned.backend)};
        auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
        check(host_r.has_value(), "parity host constructs");
        std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

        const auto unset = [] {
            return make_unexpected<CompositionOutcome>(
                IoError{.code = IoError::Code::backend_error});
        };
        Result<CompositionOutcome> h_closed_empty = unset(), h_write_only_empty = unset(),
                                    h_closed_nonempty = unset(),
                                    h_write_only_nonempty = unset(),
                                    h_range_invalid = unset(),
                                    h_read_only_empty_write = unset(),
                                    h_read_only_nonempty_write = unset(),
                                    h_valid_empty_read = unset(),
                                    h_valid_empty_write = unset(), h_full = unset(),
                                    h_eof = unset();
        check(host->spawn([&](IoTaskContext& task) {
                  h_closed_empty = task.read_exact(NativeFileRef{closed_file}, empty_dst, 0);
                  h_write_only_empty =
                      task.read_exact(NativeFileRef{write_only_file}, empty_dst, 0);
                  h_closed_nonempty = task.read_exact(NativeFileRef{closed_file}, dst8, 0);
                  h_write_only_nonempty =
                      task.read_exact(NativeFileRef{write_only_file}, dst8, 0);
                  h_range_invalid =
                      task.read_exact(NativeFileRef{read_file}, dst8, invalid_offset);
                  h_read_only_empty_write = task.write_all(NativeFileRef{read_file}, empty_src, 0);
                  h_read_only_nonempty_write =
                      task.write_all(NativeFileRef{read_file}, src8, 0);
                  h_valid_empty_read = task.read_exact(NativeFileRef{read_file}, empty_dst, 0);
                  h_valid_empty_write = task.write_all(NativeFileRef{rw_file}, empty_src, 0);
                  h_full = task.read_exact(NativeFileRef{read_file}, full_dst, 0);
                  h_eof = task.read_exact(NativeFileRef{short_file}, eof_dst, 0);
              }).has_value(),
              "parity task admitted");
        check(host->run().has_value(), "parity run settles");

        bool parity_ok = true;
        const auto verify = [&](const char* label, const Result<CompositionOutcome>& direct,
                                const Result<CompositionOutcome>& host_result) {
            const bool ok = same_composition_result(direct, host_result);
            check(ok, label);
            parity_ok = parity_ok && ok;
        };
        verify("parity closed + empty read", d_closed_empty, h_closed_empty);
        verify("parity illegal access + empty read", d_write_only_empty, h_write_only_empty);
        verify("parity closed + nonempty read", d_closed_nonempty, h_closed_nonempty);
        verify("parity illegal access + nonempty read", d_write_only_nonempty,
               h_write_only_nonempty);
        verify("parity invalid range nonempty read", d_range_invalid, h_range_invalid);
        verify("parity read-only + empty write", d_read_only_empty_write,
               h_read_only_empty_write);
        verify("parity read-only + nonempty write", d_read_only_nonempty_write,
               h_read_only_nonempty_write);
        verify("parity valid + empty read", d_valid_empty_read, h_valid_empty_read);
        verify("parity valid + empty write", d_valid_empty_write, h_valid_empty_write);
        verify("parity full read", d_full, h_full);
        verify("parity EOF before full", d_eof, h_eof);
        const bool content_ok = h_full.has_value() && full_dst[0] == std::byte{'a'} &&
                                full_dst[7] == std::byte{'h'};
        check(content_ok, "parity full read payload");
        check(owned.raw->syscall_count_for_test() == 3,
              "parity rows ran exactly the full and EOF data operations");
        check(ctx.outstanding() == 0, "parity nothing outstanding");
        check(host->test_live_task_count() == 0, "parity tasks retired");
        if (!parity_ok || !content_ok) {
            return false;
        }
    }
    {
        const std::string path = make_temp_file("abcdefgh");
        auto open = File::open(path);
        ::unlink(path.c_str());
        check(open.has_value(), "parity stopped open");
        if (!open.has_value()) {
            return false;
        }
        File src = std::move(open).value();

        AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
        auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
        std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

        std::vector<std::byte> empty_dst;
        std::vector<std::byte> dst8(8, std::byte{0});
        const Result<CompositionOutcome> stopped_unset = make_unexpected<CompositionOutcome>(
            IoError{.code = IoError::Code::backend_error});
        Result<CompositionOutcome> stopped_empty = stopped_unset;
        Result<CompositionOutcome> stopped_nonempty = stopped_unset;
        check(host->spawn([&](IoTaskContext& task) {
                  stopped_empty = task.read_exact(NativeFileRef{src}, empty_dst, 0);
                  stopped_nonempty = task.read_exact(NativeFileRef{src}, dst8, 0);
              }).has_value(),
              "parity stopped task admitted");
        host->request_stop();
        check(host->run().has_value(), "parity stopped run settles");
        const bool stopped_ok =
            !stopped_empty.has_value() &&
            stopped_empty.error().code == IoError::Code::canceled &&
            !stopped_nonempty.has_value() &&
            stopped_nonempty.error().code == IoError::Code::canceled;
        check(stopped_ok, "stopped host rejects a composition invocation before acceptance");
        check(ctx.outstanding() == 0, "parity stopped nothing outstanding");
        if (!stopped_ok) {
            return false;
        }
    }
    {
        const std::string path = make_temp_file(std::string(24, 'q'));
        auto open = File::open(path);
        ::unlink(path.c_str());
        check(open.has_value(), "parity fault open");
        if (!open.has_value()) {
            return false;
        }
        File src = std::move(open).value();

        OwnedBackend owned = make_threadpool();
        ThreadPoolBackend::WorkerClaimedPauseGate worker_gate;
        owned.raw->set_worker_claimed_pause_gate(&worker_gate);
        ThreadPoolBackend::AcceptedPreDispatchPauseGate accept_gate;
        ThreadPoolBackend::DispatchFailureInjection injection;
        AsyncIoContext ctx{std::move(owned.backend)};
        auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
        check(host_r.has_value(), "parity fault host constructs");
        std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

        std::thread choreography([&] {
            wait_gate_paused(worker_gate);
            owned.raw->set_accepted_pre_dispatch_pause_gate(&accept_gate);
            owned.raw->set_dispatch_failure_injection(&injection);
            injection.armed.store(true, std::memory_order_release);
            resume_gate(worker_gate);
            wait_gate_paused(accept_gate);
            resume_gate(accept_gate);
        });

        std::atomic<bool> fault_ok{false};
        std::vector<std::byte> dst(48, std::byte{0});
        check(host->spawn([&](IoTaskContext& task) {
                  auto composed = task.read_exact(NativeFileRef{src}, dst, 0);
                  fault_ok = composed.has_value() &&
                             composed.value().end == CompositionEnd::primitive_error &&
                             composed.value().error.has_value() &&
                             composed.value().error->code == IoError::Code::backend_error &&
                             composed.value().confirmed_bytes == 24;
              }).has_value(),
              "parity fault task admitted");

        auto run = host->run();
        choreography.join();
        check(run.has_value(), "parity fault run settles");
        check(fault_ok, "primitive failure after a confirmed prefix keeps the prefix");
        check(injection.fired.load(std::memory_order_acquire) == 1,
              "dispatch failure injected exactly once");
        check(owned.raw->syscall_count_for_test() == 1, "failed step never reached the kernel");
        check(ctx.outstanding() == 0, "parity fault nothing outstanding");
        return run.has_value() && fault_ok &&
               injection.fired.load(std::memory_order_acquire) == 1 &&
               owned.raw->syscall_count_for_test() == 1;
    }
}

// A first primitive whose submission transaction is rejected — here because
// request capacity is exhausted by a held occupier — accepted nothing, so
// the invocation itself is rejected through the outer result. Folding that
// rejection into the composition outcome would erase the accepted-versus-
// never-accepted distinction the result contract keeps.
bool first_primitive_admission_rejection_surfaces_as_outer_error() {
    const std::string src_path = make_temp_file("abcdefgh");
    const std::string occ_path = make_temp_file(std::string(8, 'o'));
    auto src_open = File::open(src_path);
    auto occ_open = File::open(occ_path);
    ::unlink(src_path.c_str());
    ::unlink(occ_path.c_str());
    check(src_open.has_value() && occ_open.has_value(), "first-rejection opens");
    if (!src_open.has_value() || !occ_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();
    File occ = std::move(occ_open).value();

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{1, 1});
    ThreadPoolBackend* raw = backend.get();
    ThreadPoolBackend::WorkerClaimedPauseGate claimed_gate;
    raw->set_worker_claimed_pause_gate(&claimed_gate);
    AsyncIoContext ctx{std::move(backend)};
    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "first-rejection host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::vector<std::byte> occ_buffer(8, std::byte{0});
    auto occ_submit =
        ctx.submit_read(ReadOp{NativeFileRef{occ}, occ_buffer.data(), occ_buffer.size(), 0});
    check(occ_submit.has_value(), "first-rejection occupier accepted");
    if (!occ_submit.has_value()) {
        return false;
    }
    Request<std::size_t> occupier = std::move(occ_submit).value();
    wait_gate_paused(claimed_gate);

    const Result<CompositionOutcome> rejected_unset = make_unexpected<CompositionOutcome>(
        IoError{.code = IoError::Code::backend_error});
    Result<CompositionOutcome> composed = rejected_unset;
    std::vector<std::byte> buffer(8, std::byte{0});
    check(host->spawn([&](IoTaskContext& task) {
              composed = task.read_exact(NativeFileRef{src}, buffer, 0);
          }).has_value(),
          "first-rejection task admitted");

    auto run = host->run();
    check(run.has_value(), "first-rejection run settles");
    const bool rejected_ok = !composed.has_value() &&
                             composed.error().code == IoError::Code::would_block;
    check(rejected_ok, "capacity-rejected first primitive rejects the invocation, nothing accepted");
    check(ctx.outstanding() == 1, "first-rejection only the occupier is outstanding");
    check(raw->syscall_count_for_test() == 0, "first-rejection dispatched no kernel operation");
    const bool rejected_clean = rejected_ok && ctx.outstanding() == 1 &&
                                raw->syscall_count_for_test() == 0;

    resume_gate(claimed_gate);
    auto owner = ctx.claim_progress_owner();
    check(owner.has_value(), "first-rejection owner reclaimable");
    auto reap = ctx.wait_one();
    check(reap.has_value() && reap.value().completed == 1, "occupier settles after release");
    const auto observed = occupier.take_result();
    check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
              observed.result.value() == 8,
          "first-rejection occupier keeps its real result");
    return run.has_value() && rejected_clean && observed.readiness == RequestReadiness::ready;
}

// The paired discriminator: a first primitive that WAS accepted and then
// failed terminally before any byte reports through the composition outcome
// (primitive error, confirmed zero) inside a successful outer result — the
// request was accepted, so this is an operation result, not an invocation
// rejection. Together with the capacity rejection above, the two pin the
// classification; either one alone would admit a wrong rule.
bool first_primitive_terminal_error_keeps_outcome_shape() {
    const std::string path = make_temp_file("abcdefgh");
    auto open = File::open(path);
    ::unlink(path.c_str());
    check(open.has_value(), "first-terminal open");
    if (!open.has_value()) {
        return false;
    }
    File src = std::move(open).value();

    OwnedBackend owned = make_threadpool();
    ThreadPoolBackend::DispatchFailureInjection injection;
    owned.raw->set_dispatch_failure_injection(&injection);
    injection.armed.store(true, std::memory_order_release);
    AsyncIoContext ctx{std::move(owned.backend)};
    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "first-terminal host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> shape_ok{false};
    std::vector<std::byte> buffer(8, std::byte{0});
    check(host->spawn([&](IoTaskContext& task) {
              auto composed = task.read_exact(NativeFileRef{src}, buffer, 0);
              shape_ok = composed.has_value() &&
                         composed.value().end == CompositionEnd::primitive_error &&
                         composed.value().error.has_value() &&
                         composed.value().error->code == IoError::Code::backend_error &&
                         composed.value().confirmed_bytes == 0;
          }).has_value(),
          "first-terminal task admitted");

    auto run = host->run();
    check(run.has_value(), "first-terminal run settles");
    check(shape_ok, "accepted first primitive terminal error is an operation result, not a rejection");
    check(injection.fired.load(std::memory_order_acquire) == 1,
          "first-terminal failure injected exactly once");
    check(owned.raw->syscall_count_for_test() == 0, "first-terminal never reached the kernel");
    check(ctx.outstanding() == 0, "first-terminal nothing outstanding");
    check(host->test_live_task_count() == 0, "first-terminal tasks retired");
    return run.has_value() && shape_ok &&
           injection.fired.load(std::memory_order_acquire) == 1 &&
           owned.raw->syscall_count_for_test() == 0;
}

// Other threads may submit context operations while the host drives
// (THREAD-01), so a completion of such a request can be reaped by the
// run-exit observation before the pending control is ever checked — with a
// single exit wait the owner then releases with the control unobserved and
// the next owner inherits it. The choreography pins that interleaving
// exactly: after the task's final await settles, its epilogue (still on the
// driver thread, so no progress pass can run) lets an external thread
// publish a zero-length request and plant the control before returning, so
// the exit observation's first pass reaps the external completion and
// returns progress without the control check. The release must keep
// observing until a pass reaps nothing with no control pending.
bool external_completion_preceding_control_does_not_leak_to_next_owner() {
    const std::string src_path = make_temp_file(std::string(32, 'k'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "exit-interleave open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    OwnedBackend owned = make_threadpool(1);
    ThreadPoolBackend::WorkerClaimedPauseGate claimed_gate;
    owned.raw->set_worker_claimed_pause_gate(&claimed_gate);
    AsyncIoContext ctx{std::move(owned.backend)};
    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "exit-interleave host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> real_outcome{false};
    std::atomic<bool> epilogue_running{false};
    std::atomic<bool> external_published{false};
    std::atomic<bool> control_planted{false};
    std::atomic<bool> external_may_finish{false};

    std::vector<std::byte> buffer(32, std::byte{0});
    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read(NativeFileRef{src}, buffer, 0);
              real_outcome = r.has_value() && r.value() == 32;
              epilogue_running.store(true, std::memory_order_release);
              epilogue_running.notify_all();
              external_published.wait(false, std::memory_order_acquire);
              control_planted.wait(false, std::memory_order_acquire);
          }).has_value(),
          "exit-interleave task admitted");

    std::thread sequencer([&] {
        wait_gate_paused(claimed_gate);
        resume_gate(claimed_gate);
    });

    std::thread external([&] {
        epilogue_running.wait(false, std::memory_order_acquire);
        auto e = ctx.submit_read(ReadOp{NativeFileRef{src}, nullptr, 0, 0});
        check(e.has_value(), "exit-interleave external no-op accepted");
        external_published.store(true, std::memory_order_release);
        external_published.notify_all();
        ctx.interrupt_progress_waiters();
        control_planted.store(true, std::memory_order_release);
        control_planted.notify_all();
        external_may_finish.wait(false, std::memory_order_acquire);
    });

    auto run = host->run();
    sequencer.join();
    check(run.has_value(), "exit-interleave run settles");
    check(real_outcome, "exit-interleave task settled on its real result");

    external_may_finish.store(true, std::memory_order_release);
    external_may_finish.notify_all();
    external.join();
    check(ctx.outstanding() == 0, "exit-interleave external request retired");

    auto owner = ctx.claim_progress_owner();
    check(owner.has_value(), "exit-interleave next owner claims the context");
    auto probe = ctx.wait_one();
    check(probe.has_value() &&
              probe.value().kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress,
          "exit-interleave: an external completion preceding control does not leak it");
    return run.has_value() && real_outcome && probe.has_value() &&
           probe.value().kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress;
}

bool zero_length_convenience_crosses_request_admission() {
    const std::string path = make_temp_file("abcd");
    auto open = File::open(path, writable());
    ::unlink(path.c_str());
    check(open.has_value(), "empty-admission open");
    if (!open.has_value()) {
        return false;
    }
    File rw = std::move(open).value();

    OwnedBackend owned = make_threadpool(1);
    AsyncStats stats;
    AsyncIoContext ctx{std::move(owned.backend), &stats};
    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "empty-admission host constructs");
    if (!host_r.has_value()) {
        return false;
    }
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::vector<std::byte> empty_dst;
    std::vector<std::byte> empty_src;
    const auto unsett = [] {
        return make_unexpected<CompositionOutcome>(
            IoError{.code = IoError::Code::backend_error});
    };
    Result<CompositionOutcome> empty_read = unsett();
    Result<CompositionOutcome> empty_write = unsett();
    const std::uint64_t submits_before = stats.submit_calls;
    check(host->spawn([&](IoTaskContext& task) {
              empty_read = task.read_exact(NativeFileRef{rw}, empty_dst, 0);
              empty_write = task.write_all(NativeFileRef{rw}, empty_src, 0);
          }).has_value(),
          "empty-admission task admitted");
    check(host->run().has_value(), "empty-admission run settles");
    const bool read_ok = empty_read.has_value() &&
                         empty_read.value().end == CompositionEnd::complete &&
                         empty_read.value().confirmed_bytes == 0;
    const bool write_ok = empty_write.has_value() &&
                          empty_write.value().end == CompositionEnd::complete &&
                          empty_write.value().confirmed_bytes == 0;
    check(read_ok, "empty-admission read completes as an admitted no-op");
    check(write_ok, "empty-admission write completes as an admitted no-op");
    check(stats.submit_calls == submits_before + 2,
          "each zero-length convenience crosses exactly one request admission");
    check(owned.raw->syscall_count_for_test() == 0,
          "empty-admission rows dispatch no data syscall");
    check(ctx.outstanding() == 0, "empty-admission nothing outstanding");
    return read_ok && write_ok && stats.submit_calls == submits_before + 2 &&
           owned.raw->syscall_count_for_test() == 0 && ctx.outstanding() == 0;
}

bool exit_window_health_failure_fails_fast() {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        ::alarm(30);
        const std::string path = make_temp_file("abcdefgh");
        auto open = File::open(path);
        ::unlink(path.c_str());
        if (!open.has_value()) {
            std::_Exit(0);
        }
        File src = std::move(open).value();
        OwnedBackend owned = make_threadpool(1);
        AsyncIoContext ctx{std::move(owned.backend)};
        auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
        if (!host_r.has_value()) {
            std::_Exit(0);
        }
        std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();
        std::vector<std::byte> dst(8, std::byte{0});
        (void)host->spawn([&](IoTaskContext& task) {
            auto r = task.read(NativeFileRef{src}, dst, 0);
            if (!r.has_value()) {
                std::_Exit(0);
            }
            ctx.set_wait_health_failed_for_test();
        });
        auto run = host->run();
        (void)run;
        std::_Exit(0);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        return false;
    }
    const bool aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    check(aborted, "exit-window health failure fails fast instead of returning success");
    return aborted;
}

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"pipeline_tracer_threadpool", pipeline_tracer_threadpool},
#if defined(SLUICE_HAS_LIBURING)
        {"pipeline_tracer_uring", pipeline_tracer_uring},
#endif
        {"task_failure_while_other_io_outstanding", task_failure_while_other_io_outstanding},
        {"stop_during_suspension_settles", stop_during_suspension_settles},
        {"await_state_reserved_before_suspension", await_state_reserved_before_suspension},
        {"deadline_expiry_does_not_cancel", deadline_expiry_does_not_cancel},
        {"exception_during_resumed_processing", exception_during_resumed_processing},
        {"stop_races_publication_and_retirement", stop_races_publication_and_retirement},
        {"progress_owner_composition", progress_owner_composition},
        {"bounds_task_capacity_rejects", bounds_task_capacity_rejects},
        {"invalid_configuration_rejected", invalid_configuration_rejected},
        {"create_rejects_non_signaling_backend", create_rejects_non_signaling_backend},
        {"destroying_host_with_live_task_fails_fast", destroying_host_with_live_task_fails_fast},
        {"nested_run_from_a_task_rejected", nested_run_from_a_task_rejected},
        {"await_after_stop_rejects_before_acceptance", await_after_stop_rejects_before_acceptance},
        {"run_failure_does_not_poison_the_next_run", run_failure_does_not_poison_the_next_run},
        {"host_stop_does_not_plant_context_control", host_stop_does_not_plant_context_control},
        {"stop_leaves_undispatched_accepted_io_uncanceled",
         stop_leaves_undispatched_accepted_io_uncanceled},
        {"expired_deadline_parks_instead_of_spinning", expired_deadline_parks_instead_of_spinning},
        {"external_control_acknowledged_before_next_owner",
         external_control_acknowledged_before_next_owner},
        {"control_consumed_by_progress_return_is_retired_at_exit",
         control_consumed_by_progress_return_is_retired_at_exit},
        {"nested_spawn_bounded_by_capacity", nested_spawn_bounded_by_capacity},
        {"first_task_error_selected_by_execution_order",
         first_task_error_selected_by_execution_order},
        {"host_composition_reports_canonical_outcomes",
         host_composition_reports_canonical_outcomes},
        {"host_composition_stops_at_new_acceptance_under_stop",
         host_composition_stops_at_new_acceptance_under_stop},
        {"host_convenience_parity_with_direct_invocation_semantics",
         host_convenience_parity_with_direct_invocation_semantics},
        {"first_primitive_admission_rejection_surfaces_as_outer_error",
         first_primitive_admission_rejection_surfaces_as_outer_error},
        {"first_primitive_terminal_error_keeps_outcome_shape",
         first_primitive_terminal_error_keeps_outcome_shape},
        {"external_completion_preceding_control_does_not_leak_to_next_owner",
         external_completion_preceding_control_does_not_leak_to_next_owner},
        {"zero_length_convenience_crosses_request_admission",
         zero_length_convenience_crosses_request_admission},
        {"exit_window_health_failure_fails_fast", exit_window_health_failure_fails_fast},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d assertion(s)\n", g_failures);
        return 1;
    }
    std::printf("stackful_host_test: all cases passed\n");
    return 0;
}
