#include <sluice/async/stackful_io_host.hpp>

#include <sluice/async/request_scope.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>
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
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "threadpool_test_seams.hpp"

namespace {

using namespace sluice::async;
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
    char path[] = "/tmp/sluice_d2_host_XXXXXX";
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

OwnedBackend make_threadpool() {
    auto backend = std::make_unique<ThreadPoolBackend>();
    ThreadPoolBackend* raw = backend.get();
    return OwnedBackend{std::move(backend), raw};
}

// The sequential-looking W-04 body: fill a buffer from one file through
// repeated primitive awaits, then write it to another file and sync.
struct W04CopyBody {
    File* src;
    File* dst;
    std::vector<std::byte> buffer;
    std::size_t copied = 0;
    bool sync_done = false;

    W04CopyBody(File* s, File* d, std::size_t chunk) : src(s), dst(d), buffer(chunk) {}

    void operator()(IoTaskContext& task) {
        std::uint64_t offset = 0;
        for (;;) {
            auto r = task.read(NativeFileRef{*src}, buffer, offset);
            check(r.has_value(), "w04 read completes");
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
                check(w.has_value(), "w04 write completes");
                if (!w.has_value()) {
                    return;
                }
                written += w.value();
            }
            offset += r.value();
            copied += r.value();
        }
        auto s = task.sync_data(NativeFileRef{*dst});
        check(s.has_value(), "w04 sync completes");
        sync_done = s.has_value();
    }
};

bool w04_tracer_threadpool() {
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

    W04CopyBody body(&src, &dst, 512);
    check(host->spawn([&body](IoTaskContext& task) { body(task); }).has_value(),
          "w04 task admitted");

    auto run = host->run();
    check(run.has_value(), "w04 run succeeds");
    check(body.copied == payload.size(), "w04 copied byte count");
    check(body.sync_done, "w04 sync performed");
    check(host->test_live_task_count() == 0, "w04 no live tasks after run");
    check(host->test_slots_with_await_link() == 0, "w04 no await links after run");
    check(ctx.outstanding() == 0, "w04 no outstanding requests after run");

    auto reclaim = ctx.claim_progress_owner();
    check(reclaim.has_value(), "w04 progress owner released after run");

    const bool content_ok = open_fd_content_is(dst.native_handle(), payload);
    check(content_ok, "w04 destination content");
    return body.copied == payload.size() && body.sync_done && content_ok;
}

#if defined(SLUICE_HAS_LIBURING)

bool uring_backend_available() {
    UringAsyncBackend probe;
    return probe.available();
}

bool w04_tracer_uring() {
    if (!uring_backend_available()) {
        std::printf("NOT RUN: w04_tracer_uring (io_uring unavailable)\n");
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

    W04CopyBody body(&src, &dst, 256);
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

// H1: task A owns accepted I/O and suspends; task B fails. B's cleanup
// completes, A remains valid and settles, the host stays coherent. B drives
// the pause-gate release itself, so no external scheduling enters the trace.
bool h1_task_failure_while_other_io_outstanding() {
    const std::string src_path = make_temp_file(std::string(200, 'a'));
    const std::string dst_path = make_temp_file("");
    auto src_open = File::open(src_path);
    auto dst_open = File::open(dst_path, writable());
    ::unlink(src_path.c_str());
    ::unlink(dst_path.c_str());
    check(src_open.has_value() && dst_open.has_value(), "h1 opens");
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
    check(host_r.has_value(), "h1 host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    W04CopyBody body_a(&src, &dst, 64);
    check(host->spawn([&](IoTaskContext& task) { body_a(task); }).has_value(),
          "h1 task A admitted");

    check(host->spawn([&](IoTaskContext&) {
              wait_gate_paused(gate);
              resume_gate(gate);
              throw std::system_error(ENOENT, std::generic_category());
          }).has_value(),
          "h1 task B admitted");

    auto run = host->run();
    check(!run.has_value(), "h1 run reports the task error");
    check(run.error().code == IoError::Code::not_found, "h1 error maps ENOENT to not_found");
    check(body_a.copied == 200, "h1 task A still settles and completes");
    check(ctx.outstanding() == 0, "h1 no outstanding after run");
    check(host->test_live_task_count() == 0, "h1 no live tasks");
    check(host->test_slots_with_await_link() == 0, "h1 no await links");
    return body_a.copied == 200 && run.error().code == IoError::Code::not_found;
}

// H2: a task is suspended with an accepted Request; stop arrives while the
// physical operation is claimed but not terminal. No stack destruction; the
// responsibility is retained; settlement and retirement complete before the
// host returns.
bool h2_stop_during_suspension_settles() {
    const std::string src_path = make_temp_file(std::string(64, 's'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "h2 open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    OwnedBackend owned = make_threadpool();
    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    owned.raw->set_worker_claimed_pause_gate(&gate);
    AsyncIoContext ctx{std::move(owned.backend)};

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "h2 host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> task_saw_token{false};
    std::atomic<bool> task_got_terminal{false};

    std::thread stopper([&] {
        wait_gate_paused(gate);
        host->request_stop();
        resume_gate(gate);
    });

    std::vector<std::byte> buffer(64, std::byte{0});
    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read(NativeFileRef{src}, buffer, 0);
              task_got_terminal = true;
              if (task.token().is_requested()) {
                  task_saw_token = true;
              }
              check(r.has_value() || r.error().code == IoError::Code::canceled,
                    "h2 terminal outcome is honest");
          }).has_value(),
          "h2 task admitted");

    auto run = host->run();
    stopper.join();
    check(run.has_value(), "h2 run settles and returns after stop");

    check(task_got_terminal, "h2 suspended request reached a public terminal");
    check(task_saw_token, "h2 task observed the stop token");
    check(ctx.outstanding() == 0, "h2 nothing outstanding after settlement");
    check(host->test_live_task_count() == 0, "h2 task storage retired");
    check(host->test_slots_with_await_link() == 0, "h2 await link retired with the task");
    check(host->stop_requested(), "h2 host stop is observable");

    auto after = host->spawn([](IoTaskContext&) {});
    check(!after.has_value() && after.error().code == IoError::Code::canceled,
          "h2 spawn admission closed after stop");
    return task_got_terminal && task_saw_token;
}

// H3 arm: the wake/observation state is reserved for the task's whole
// lifetime, so no observation-setup step exists between acceptance and
// suspension. A sibling task observes the reservation while the first task
// is suspended; the drop-responsibility arms are discriminated by the
// M4/M5 mutation builds.
bool h3_await_state_reserved_before_suspension() {
    const std::string src_path = make_temp_file(std::string(32, 'h'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "h3 open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    OwnedBackend owned = make_threadpool();
    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    owned.raw->set_worker_claimed_pause_gate(&gate);
    AsyncIoContext ctx{std::move(owned.backend)};

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "h3 host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> saw_reserved_link{false};
    std::atomic<bool> a_settled{false};
    std::vector<std::byte> buffer(32, std::byte{0});

    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read(NativeFileRef{src}, buffer, 0);
              a_settled = r.has_value() && r.value() == 32;
          }).has_value(),
          "h3 task A admitted");

    check(host->spawn([&](IoTaskContext&) {
              saw_reserved_link = host->test_slots_with_await_link() == 1;
              resume_gate(gate);
          }).has_value(),
          "h3 task B admitted");

    auto run = host->run();
    check(run.has_value(), "h3 run succeeds");
    check(saw_reserved_link, "h3 sibling observes the reserved await state");
    check(a_settled, "h3 gated read settles with data");
    check(ctx.outstanding() == 0, "h3 nothing outstanding");
    return saw_reserved_link && a_settled;
}

// H4: a wait deadline expires while the physical operation is still claimed.
// The deadline bounds initial waiting only: the helper initiates best-effort
// cancel and settles before returning the honest terminal outcome.
bool h4_wait_timeout_settles_before_return() {
    const std::string src_path = make_temp_file(std::string(48, 't'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "h4 open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    OwnedBackend owned = make_threadpool();
    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    owned.raw->set_worker_claimed_pause_gate(&gate);
    AsyncIoContext ctx{std::move(owned.backend)};

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "h4 host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> got_terminal{false};
    std::atomic<bool> honest_outcome{false};

    // Orders the gate release after the 50ms wait deadline has fired; it
    // inflates no retry and gates no assertion.
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        resume_gate(gate);
    });

    std::vector<std::byte> buffer(48, std::byte{0});
    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read_for(NativeFileRef{src}, buffer, 0, std::chrono::milliseconds(50));
              got_terminal = true;
              honest_outcome = (r.has_value() && r.value() == 48) ||
                               (!r.has_value() && r.error().code == IoError::Code::canceled);
          }).has_value(),
          "h4 task admitted");

    auto run = host->run();
    releaser.join();
    check(run.has_value(), "h4 run settles after deadline");

    check(got_terminal, "h4 helper returned a terminal outcome, not a timeout escape");
    check(honest_outcome, "h4 outcome is honest data or honest cancel");
    check(ctx.outstanding() == 0, "h4 nothing outstanding after settlement");
    check(host->test_live_task_count() == 0, "h4 task retired");
    return got_terminal && honest_outcome;
}

// H5: the request publishes, the fiber resumes, the task consumes the result
// and then throws. The original exception is preserved and reaches the run
// boundary; the consumed request leaves nothing outstanding.
bool h5_exception_during_resumed_processing() {
    const std::string src_path = make_temp_file(std::string(16, 'e'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "h5 open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    AsyncIoContext ctx{std::make_unique<ThreadPoolBackend>()};
    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "h5 host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<bool> consumed_before_throw{false};
    std::vector<std::byte> buffer(16, std::byte{0});

    check(host->spawn([&](IoTaskContext& task) {
              auto r = task.read(NativeFileRef{src}, buffer, 0);
              consumed_before_throw = r.has_value() && r.value() == 16;
              throw std::runtime_error("h5 resumed-processing failure");
          }).has_value(),
          "h5 task admitted");

    auto run = host->run();
    check(!run.has_value(), "h5 task error reaches the run boundary");
    check(run.error().code == IoError::Code::backend_error,
          "h5 original exception maps unchanged (runtime_error -> backend_error)");
    check(consumed_before_throw, "h5 result consumed before the exception");
    check(ctx.outstanding() == 0, "h5 nothing outstanding");
    check(host->test_slots_with_await_link() == 0, "h5 await state retired");
    return consumed_before_throw && run.error().code == IoError::Code::backend_error;
}

// H6: the physical outcome exists but publication is paused; stop races the
// publication/retirement boundary. Exactly one terminal is observed, the task
// body runs exactly once, and nothing touches retired storage.
bool h6_stop_races_publication_and_retirement() {
    const std::string src_path = make_temp_file(std::string(24, 'r'));
    auto src_open = File::open(src_path);
    ::unlink(src_path.c_str());
    check(src_open.has_value(), "h6 open");
    if (!src_open.has_value()) {
        return false;
    }
    File src = std::move(src_open).value();

    OwnedBackend owned = make_threadpool();
    ThreadPoolBackend::WorkerOutcomePreTerminalPauseGate gate;
    owned.raw->set_worker_outcome_pre_terminal_pause_gate(&gate);
    AsyncIoContext ctx{std::move(owned.backend)};

    auto host_r = StackfulIoHost::create(ctx, StackfulHostConfig{});
    check(host_r.has_value(), "h6 host constructs");
    std::unique_ptr<StackfulIoHost> host = std::move(host_r).value();

    std::atomic<int> body_entries{0};
    std::atomic<bool> honest_terminal{false};
    std::vector<std::byte> buffer(24, std::byte{0});

    std::thread stopper([&] {
        wait_gate_paused(gate);
        host->request_stop();
        resume_gate(gate);
    });

    check(host->spawn([&](IoTaskContext& task) {
              ++body_entries;
              auto r = task.read(NativeFileRef{src}, buffer, 0);
              honest_terminal = (r.has_value() && r.value() == 24) ||
                                (!r.has_value() && r.error().code == IoError::Code::canceled);
          }).has_value(),
          "h6 task admitted");

    auto run = host->run();
    stopper.join();
    check(run.has_value(), "h6 run settles after the race");
    check(body_entries == 1, "h6 task body entered exactly once");
    check(honest_terminal, "h6 honest terminal under the stop race");
    check(ctx.outstanding() == 0, "h6 nothing outstanding");
    check(host->test_live_task_count() == 0, "h6 task retired once");
    check(host->test_slots_with_await_link() == 0, "h6 no delivery touches retired storage");
    return body_entries == 1 && honest_terminal;
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

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"w04_tracer_threadpool", w04_tracer_threadpool},
#if defined(SLUICE_HAS_LIBURING)
        {"w04_tracer_uring", w04_tracer_uring},
#endif
        {"h1_task_failure_while_other_io_outstanding", h1_task_failure_while_other_io_outstanding},
        {"h2_stop_during_suspension_settles", h2_stop_during_suspension_settles},
        {"h3_await_state_reserved_before_suspension", h3_await_state_reserved_before_suspension},
        {"h4_wait_timeout_settles_before_return", h4_wait_timeout_settles_before_return},
        {"h5_exception_during_resumed_processing", h5_exception_during_resumed_processing},
        {"h6_stop_races_publication_and_retirement", h6_stop_races_publication_and_retirement},
        {"progress_owner_composition", progress_owner_composition},
        {"bounds_task_capacity_rejects", bounds_task_capacity_rejects},
        {"invalid_configuration_rejected", invalid_configuration_rejected},
        {"create_rejects_non_signaling_backend", create_rejects_non_signaling_backend},
        {"destroying_host_with_live_task_fails_fast", destroying_host_with_live_task_fails_fast},
        {"nested_run_from_a_task_rejected", nested_run_from_a_task_rejected},
        {"await_after_stop_rejects_before_acceptance", await_after_stop_rejects_before_acceptance},
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
    std::printf("stackful_host_w04_test: all cases passed\n");
    return 0;
}
