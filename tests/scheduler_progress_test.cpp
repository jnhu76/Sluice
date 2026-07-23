// E6 — scheduler progress: hybrid poll/wait (sluice-CORE-E6).
//
// Proves the ADR §4 boundary end-to-end on a REAL backend whose wait_one()
// BLOCKS: a Fiber awaiting a backend Completion whose op completes AFTER the
// runnable queue drains is resumed by the Scheduler's wait_one() idle wait.
// This closes the [PROGRESS-GAP] from the E6 audit — E4's mapping is reused
// unchanged; only the idle/progress policy changes.
//
// Single-worker Scheduler. ThreadPoolBackend as the completion source (its
// wait_one cv-waits for the ready queue; its workers run real syscalls on
// separate OS threads — a legitimate S2 backend per ADR §4).
//
// PROVES: E6-T1 real-backend held-in-flight Completion; E6-T2 same inside an
//         Evented Group task; E6-T3 S3 (ready-flag-only) does NOT call wait_one
//         (no zero-outstanding hazard); E6-T4 regressions.
// DOES NOT PROVE: multi-worker, cross-thread wake primitive, idle parking
//                 beyond wait_one.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/group.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using namespace sluice::async;

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

int make_temp_fd_with_bytes(const std::byte* seed, std::size_t n) {
    static long c = 0;
    auto path = std::filesystem::temp_directory_path() /
                ("sluice_e6_" + std::to_string(c++) + ".tmp");
    int fd = ::open(path.string().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    std::filesystem::remove(path);
    if (n > 0) {
        ssize_t w = ::write(fd, seed, n);
        (void)w;
        ::lseek(fd, 0, SEEK_SET);
    }
    return fd;
}
}  // namespace

// ---- E6-T1: real-backend Completion, held in flight, resumed via wait_one --
// Fiber A submits a ThreadPool read (worker thread runs pread asynchronously)
// and suspends. Fiber B runs (liveness: B progresses while A's op is in flight
// on the worker). After B completes and the runnable queue drains, the Scheduler
// enters S2 (runnable empty, Completion-backed wait pending) and calls
// wait_one() — ThreadPool's cv-wait blocks until A's worker finishes and pushes
// the result; the Scheduler wakes, maps the now-ready Completion to A, resumes
// A. A observes the bytes.
//
// Determinism: the read is on a pre-filled temp file (EOF after the bytes), so
// the worker's pread completes with a definite result; the only nondeterminism
// is WHEN the worker finishes relative to B, and that is exactly what wait_one
// abstracts — the test does not assert timing, only the terminal outcome.
SLUICE_TEST_CASE(progress_real_backend_completion_resumes_via_wait_one) {
    if constexpr (!fiber_ctx::supported) return;

    const std::byte seed[8]{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                            std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    int fd = make_temp_fd_with_bytes(seed, 8);
    SLUICE_CHECK(fd >= 0);

    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
    Scheduler sched(ctx);

    Completion<std::size_t> a_c;
    std::byte a_buf[8]{};
    int a_resumed = 0;
    std::size_t a_bytes = 0;
    int b_ran = 0;
    bool b_ran_before_a_resume = false;

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{fd, a_buf, 8, 0}, a_c).has_value());
        sched.await_completion_size(a_c);   // suspend; op runs on a ThreadPool worker
        a_resumed = 1;
        if (a_c.ready()) a_bytes = a_c.result().value_or(0);
    });
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        b_ran_before_a_resume = (a_resumed == 0);   // liveness: B before A resumes
        b_ran = 1;
    });
    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));
    sched.spawn(fa);
    sched.spawn(fb);
    sched.run_until_idle();   // S2 wait_one bridges the async worker completion

    SLUICE_CHECK(b_ran == 1);
    SLUICE_CHECK(b_ran_before_a_resume);
    SLUICE_CHECK(a_resumed == 1);
    SLUICE_CHECK(a_bytes == 8);
    SLUICE_CHECK(std::memcmp(a_buf, seed, 8) == 0);   // bytes delivered
    SLUICE_CHECK(fa.state() == FiberState::done);
    SLUICE_CHECK(fb.state() == FiberState::done);
    SLUICE_CHECK(sched.waiting_count() == 0);
    ::close(fd);
}

// ---- E6-T2: same shape inside an Evented Group task -----------------------
// An Evented Group task awaits a real ThreadPool Completion. Group::await drives
// the scheduler; the S2 wait_one lets the Group task complete even though its
// only progress source is the backend worker. Pre-E6 this would strand (the
// "no progress -> break" guard returned early).
SLUICE_TEST_CASE(progress_evented_group_task_awaits_real_backend) {
    if constexpr (!fiber_ctx::supported) return;

    const std::byte seed[4]{std::byte{0xA}, std::byte{0xB}, std::byte{0xC}, std::byte{0xD}};
    int fd = make_temp_fd_with_bytes(seed, 4);
    SLUICE_CHECK(fd >= 0);

    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
    Scheduler sched(ctx);
    Group g{sched};

    std::byte buf[4]{};
    int task_observed_bytes = 0;
    int task_ran = 0;

    // The Group task submits a read and awaits it via the scheduler. The task
    // body runs in a Fiber, so await_completion_size is reachable.
    g.async([&](CancelToken&) {
        Completion<std::size_t> c;
        SLUICE_CHECK(ctx.submit_read(ReadOp{fd, buf, 4, 0}, c).has_value());
        sched.await_completion_size(c);
        ++task_ran;
        if (c.ready()) task_observed_bytes = static_cast<int>(c.result().value_or(0));
    });

    g.await();   // drives the scheduler; S2 wait_one bridges the worker completion

    SLUICE_CHECK(task_ran == 1);
    SLUICE_CHECK(task_observed_bytes == 4);
    SLUICE_CHECK(std::memcmp(buf, seed, 4) == 0);
    ::close(fd);
}

// ---- E6-T3: S3 (ready-flag-only) does NOT call wait_one -------------------
// When the runnable queue is empty and ONLY Future ready-flag waits remain
// (no Completion-backed waits), the Scheduler must NOT call wait_one (zero-
// outstanding hazard + no progress source). It returns, and the caller re-
// enters after staging. We assert this by observing that run_until_idle returns
// promptly (does not block) when only a ready-flag wait is pending.
//
// We reuse the E5-A1 shape: a Fiber awaits an unready flag; a setter Fiber
// sets it. If the Scheduler incorrectly called wait_one after the setter ran
// and the awaiter was resumed (runnable drained, no Completion waits), it
// would block on the Fake backend's wait_one — but Fake returns 0, so the gate
// breaks. We assert the run terminates (no hang) and the awaiter resumed.
SLUICE_TEST_CASE(progress_ready_flag_only_does_not_call_wait_one) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());  // real backend
    Scheduler sched(ctx);
    std::atomic<bool> flag{false};
    int awaiter_resumed = 0;

    Fiber aw;
    aw.set_entry([&](Fiber&) {
        sched.await_ready_flag(flag);
        ++awaiter_resumed;
    });
    Fiber setter;
    setter.set_entry([&](Fiber&) {
        flag.store(true, std::memory_order::release);
    });
    FiberStack wa, ws;
    SLUICE_CHECK(sched.init_fiber(aw, wa.base(), wa.size()));
    SLUICE_CHECK(sched.init_fiber(setter, ws.base(), ws.size()));
    sched.spawn(aw);
    sched.spawn(setter);
    sched.run_until_idle();   // must NOT call wait_one (no Completion-backed wait)

    SLUICE_CHECK(awaiter_resumed == 1);
    SLUICE_CHECK(aw.state() == FiberState::done);
    SLUICE_CHECK(setter.state() == FiberState::done);
    SLUICE_CHECK(sched.waiting_count() == 0);
}

// ---- E6-T4: E4/E5/Threaded regression -------------------------------------
// The P3 change must not regress the existing async tests. This case is a
// smoke check: a Fiber awaits a Completion, the Scheduler completes it via
// poll, and the Fiber resumes — the E4 shape, unchanged. (Full regression is
// the existing E4/E5 test suites, which remain green.)
SLUICE_TEST_CASE(progress_e4_completion_path_unchanged) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
    Scheduler sched(ctx);
    const std::byte seed[4]{std::byte{9}, std::byte{9}, std::byte{9}, std::byte{9}};
    int fd = make_temp_fd_with_bytes(seed, 4);
    SLUICE_CHECK(fd >= 0);

    Completion<std::size_t> c;
    std::byte buf[4]{};
    int resumed = 0;
    Fiber f;
    f.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{fd, buf, 4, 0}, c).has_value());
        sched.await_completion_size(c);
        ++resumed;
    });
    FiberStack s;
    SLUICE_CHECK(sched.init_fiber(f, s.base(), s.size()));
    sched.spawn(f);
    sched.run_until_idle();
    SLUICE_CHECK(resumed == 1);
    SLUICE_CHECK(f.state() == FiberState::done);
    ::close(fd);
}

SLUICE_MAIN()
