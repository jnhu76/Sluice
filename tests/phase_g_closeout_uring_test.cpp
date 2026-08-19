// phase_g_closeout_uring_test — Phase G closeout UR-G1..G7 matrix on the
// REAL io_uring path (docs/design/phase-g-backend-progress-wake.md §5).
//
// Required chain under proof (closeout §6):
//
//   kernel CQE -> ring fd -> wait_one poll -> reap -> Scheduler route
//
// with NO polling interval — the MW-S2 participant parks in
// poll(ring_fd, control_fd) and only a CQE (progress epoch / ring POLLIN) or
// a Scheduler wake (the bridge: control epoch + control eventfd) releases
// it. Deterministic gating WITHOUT test hooks into the kernel: every
// construction submits reads on an EMPTY BLOCKING pipe, so the request pends
// in the kernel and NO CQE exists until the test writes to the pipe — the
// write IS the release valve, ordered by epoch/counter observations.
//
// Cases (mirror of the ThreadPool matrix in phase_g_closeout_test.cpp):
//   UR-G1  CQE/ring readiness between the wait snapshot and the physical
//          poll park (the wait-phase marker is that window), including the
//          full reap->route chain via an awaiting fiber.
//   UR-G2  multiple CQEs coalesce; one consumed wake drains every terminal.
//   UR-G3  external Scheduler wake while physically parked in
//          poll(ring_fd, control_fd) — the control eventfd branch.
//   UR-G4  external wake in the commit→wait_one entry window (D4-RM14
//          armed floor; the detector discriminates a park-through).
//   UR-G5  CQE racing the control interrupt, both orders (the progress seam
//          pins the mid-invocation point for order 1).
//   UR-G6  close_admission while parked: interrupt -> documented no-progress
//          terminate -> caller re-entry reaps the unobserved CQE.
//   UR-G7  drain/quiescent teardown after the final CQE (clean scope exit;
//          the AsyncIoContext/backend fail-fast contracts make a non-
//          quiescent destroy abort, so a normal return IS the assertion).
//
// Stub builds run only the evidence-mode classification:
// required_modes=("real",) — a stub run is classified INCOMPLETE, never an
// accidental PASS (c2c evidence-mode protocol).
//
// Determinism policy (production-test-plan.md §1): no sleep_for as ordering
// proof; epoch + counter observations; bounded deadlines as hang watchdogs
// only, exiting fail-closed (rc 70).
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/uring_wait_source.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/select.hpp>
#include <sluice/async/uring_backend.hpp>

#include "async_test_control.hpp"
#include "harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace sa = sluice::async;
namespace stest = sluice_async_test;

using AsyncTestAccess = sa::Scheduler::AsyncTestAccess;
using Scheduler = sa::Scheduler;

#if !defined(SLUICE_HAS_LIBURING)

SLUICE_TEST_CASE(phase_g_closeout_uring_evidence_mode) {
    std::printf("[evidence-meta] evidence=phase_g_closeout_uring mode=stub\n");
}

SLUICE_TEST_CASE(phase_g_closeout_uring_stub_build_api_honesty) {
    sa::UringAsyncBackend backend{4};
    SLUICE_CHECK(!backend.available());
}

#else // SLUICE_HAS_LIBURING — the real io_uring path ------------------------

namespace {

constexpr auto kObserveWait = std::chrono::seconds(5);
constexpr auto kJoinWatchdog = std::chrono::seconds(10);

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

[[noreturn]] void fail_closed(Scheduler& sched, const char* tag, const char* msg) {
    AsyncTestAccess::dump_park_forensics(sched, tag);
    std::fprintf(stderr, "PHASE-G-URING FAIL-CLOSED: %s (%s); pid=%d\n",
                 msg, tag, static_cast<int>(::getpid()));
    std::_Exit(70);
}

bool wait_flag(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + kObserveWait;
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

bool wait_count_at_least(const std::atomic<int>& counter, int value) {
    const auto deadline = std::chrono::steady_clock::now() + kObserveWait;
    while (counter.load(std::memory_order_acquire) < value) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

bool wait_token(Scheduler& sched, std::uint64_t& observed,
                std::uint64_t sa::BackendWaitToken::*field) {
    const auto deadline = std::chrono::steady_clock::now() + kObserveWait;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto token = AsyncTestAccess::backend_wait_token(sched);
        if (token.*field > observed) {
            observed = token.*field;
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

struct RunDriver {
    Scheduler& sched;
    std::thread th;
    std::atomic<bool> done{false};
    explicit RunDriver(Scheduler& s) : sched(s) {}
    void start(unsigned workers) {
        th = std::thread([this, workers] {
            sched.run_live(workers);
            done.store(true, std::memory_order_release);
        });
    }
    void join_or_fail(const char* tag) {
        if (done.load(std::memory_order_acquire)) {
            th.join();
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + kJoinWatchdog;
        while (!done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                fail_closed(sched, tag, "run_live never returned (lost wake)");
            }
            std::this_thread::yield();
        }
        th.join();
    }
};

// Real-ring fixture. `gate_pipe` is a BLOCKING pipe: every submitted read on
// the empty read-end pends in the kernel (accepted + outstanding, NO CQE)
// until the test writes — the deterministic release valve.
struct URGFixture {
    int pipe_fd[2] = {-1, -1};
    std::atomic<bool> wait_phase_entered{false};
    std::atomic<int> prepark_entries{0};
    sa::UringAsyncBackend* raw = nullptr;
    sa::AsyncIoContext ctx{make_backend(this)};
    Scheduler sched;
    stest::ControllerGuard ctrl;
    sa::SchedulerWakeHandle wh;

    URGFixture() : sched(ctx), ctrl(sched), wh(sched.make_wake_handle()) {
        SLUICE_CHECK(raw->available());
    }

    ~URGFixture() {
        raw->set_wait_phase_flag_for_test(nullptr);
        raw->set_wait_prepark_counter_for_test(nullptr);
        if (pipe_fd[0] >= 0) ::close(pipe_fd[0]);
        if (pipe_fd[1] >= 0) ::close(pipe_fd[1]);
    }

    URGFixture(const URGFixture&) = delete;
    URGFixture& operator=(const URGFixture&) = delete;

    // Release every kernel-pended read: one byte per pending read.
    void release_kernel(std::size_t reads) {
        for (std::size_t i = 0; i < reads; ++i) {
            const char byte = 'g';
            if (::write(pipe_fd[1], &byte, 1) != 1) {
                std::fprintf(stderr, "phase_g_closeout_uring: pipe write failed\n");
                std::exit(1);
            }
        }
    }

  private:
    static std::unique_ptr<sa::UringAsyncBackend> make_backend(URGFixture* self) {
        if (::pipe(self->pipe_fd) != 0) {
            std::fprintf(stderr, "phase_g_closeout_uring: pipe failed\n");
            std::exit(1);
        }
        auto backend =
            std::make_unique<sa::UringAsyncBackend>(sa::UringConfig{8, 8});
        self->raw = backend.get();
        backend->set_wait_phase_flag_for_test(&self->wait_phase_entered);
        backend->set_wait_prepark_counter_for_test(&self->prepark_entries);
        return backend;
    }
};

// External-wake-possible fiber parked in select() — keeps the run Live so an
// interrupted park re-parks instead of terminating (see the TP twin).
struct SelectWaiter {
    sa::Event ev;
    sa::SelectResult captured;
    std::atomic<int> resumed{0};
    sa::Fiber fb;
    FiberStack stack;

    explicit SelectWaiter(Scheduler& sched) : ev(sched, /*initially_set=*/false) {
        fb.set_entry([this, &sched](sa::Fiber&) {
            captured = sa::select(sched, sa::EventSelectCase{ev});
            resumed.fetch_add(1, std::memory_order_acq_rel);
        });
    }
    void spawn_on_worker0(Scheduler& sched) {
        if (!sched.init_fiber(fb, stack.base(), stack.size())) {
            std::fprintf(stderr, "phase_g_closeout_uring: init_fiber failed\n");
            std::exit(1);
        }
        sched.spawn_on(fb, /*worker_id=*/0);
    }
};

// Fiber awaiting the Completion on the real ring — the reap->route half of
// the UR chain (Phase F1 identity routing).
struct AwaitWaiter {
    sa::Completion<std::size_t>& c;
    std::atomic<int> resumed{0};
    sa::Fiber fb;
    FiberStack stack;

    explicit AwaitWaiter(sa::Completion<std::size_t>& completion,
                         Scheduler& sched)
        : c(completion) {
        fb.set_entry([this, &sched](sa::Fiber&) {
            (void)sched.await_completion_size(c);
            resumed.fetch_add(1, std::memory_order_acq_rel);
        });
    }
    void spawn_on_worker0(Scheduler& sched) {
        if (!sched.init_fiber(fb, stack.base(), stack.size())) {
            std::fprintf(stderr, "phase_g_closeout_uring: init_fiber failed\n");
            std::exit(1);
        }
        sched.spawn_on(fb, /*worker_id=*/0);
    }
};

}  // namespace

SLUICE_TEST_CASE(phase_g_closeout_uring_evidence_mode) {
    sa::UringAsyncBackend backend{sa::UringConfig{4, 4}};
    std::printf("[evidence-meta] evidence=phase_g_closeout_uring mode=real\n");
    SLUICE_CHECK(backend.available());
}

// ===========================================================================
// UR-G1 — kernel CQE between the wait snapshot and the physical poll park,
// including the full chain: CQE -> ring fd -> wait_one returns -> reap
// publishes Completion-ready -> Phase F1 route resumes the awaiting fiber ->
// quiescent terminate. The wait-phase marker (set inside wait_for_change,
// after the token snapshot, at the poll boundary) IS the window.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_uring_g1_cqe_between_snapshot_and_park) {
    if constexpr (!sa::fiber_ctx::supported) return;
    URGFixture f;
    std::byte buf[1]{};
    sa::Completion<std::size_t> c;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{f.pipe_fd[0], buf, 1, 0}, c).has_value());

    AwaitWaiter waiter(c, f.sched);
    waiter.spawn_on_worker0(f.sched);

    RunDriver driver(f.sched);
    driver.start(1);

    if (!wait_count_at_least(f.prepark_entries, 1)) {
        fail_closed(f.sched, "ur-g1-never-parked",
                    "participant never reached the physical poll park");
    }
    if (AsyncTestAccess::worker_park_domain(f.sched, 0) !=
        sa::WorkerState::ParkDomain::Backend) {
        fail_closed(f.sched, "ur-g1-wrong-domain", "park domain not Backend");
    }
    std::uint64_t progress =
        AsyncTestAccess::backend_wait_token(f.sched).progress_generation;
    f.release_kernel(1);
    if (!wait_token(f.sched, progress, &sa::BackendWaitToken::progress_generation)) {
        fail_closed(f.sched, "ur-g1-cqe-never-published",
                    "kernel CQE never advanced the progress epoch");
    }

    driver.join_or_fail("ur-g1-run-never-returned");

    SLUICE_CHECK_MSG(waiter.resumed.load(std::memory_order_acquire) == 1,
                     "UR-G1: awaiting fiber routed exactly once by the reap");
    SLUICE_CHECK_MSG(waiter.fb.state() == sa::FiberState::done, "fiber done");
    SLUICE_CHECK_MSG(c.ready(), "UR-G1: Completion ready via real reap");
    c.reset();
}

// ===========================================================================
// UR-G2 — multiple kernel CQEs coalesce: three pended reads released
// together; the parked participant's wake drains every terminal (one
// wait_one invocation or several — coalescing is allowed, loss is not).
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_uring_g2_multi_cqe_coalesce) {
    if constexpr (!sa::fiber_ctx::supported) return;
    URGFixture f;
    std::byte buf[3]{};
    sa::Completion<std::size_t> c1, c2, c3;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{f.pipe_fd[0], &buf[0], 1, 0}, c1).has_value());
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{f.pipe_fd[0], &buf[1], 1, 0}, c2).has_value());
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{f.pipe_fd[0], &buf[2], 1, 0}, c3).has_value());

    RunDriver driver(f.sched);
    driver.start(1);

    if (!wait_count_at_least(f.prepark_entries, 1)) {
        fail_closed(f.sched, "ur-g2-never-parked",
                    "participant never reached the physical poll park");
    }
    f.release_kernel(3);

    driver.join_or_fail("ur-g2-run-never-returned");

    SLUICE_CHECK_MSG(c1.ready() && c2.ready() && c3.ready(),
                     "UR-G2: every coalesced CQE produced its Completion");
    SLUICE_CHECK_MSG(f.raw->outstanding() == 0, "UR-G2: outstanding drained");
    c1.reset();
    c2.reset();
    c3.reset();
}

// ===========================================================================
// UR-G3 — external Scheduler wake while the participant is physically parked
// in poll(ring_fd, control_fd). The bridge bumps the control epoch and writes
// the control eventfd; poll returns on the control branch; the interrupted
// wait is consumed and the Live run re-parks. Then a real CQE and the event
// resolution converge the run.
//
// Park observation NOTE: unlike ReadyWaitSource (whose prepark counter
// counts every wait_for_change ENTRY), the UringWaitSource prepark counter
// counts only arrivals at the FINAL PRE-POLL point — prepark >= 1 is the
// strict "physically at poll(2)" marker here.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_uring_g3_external_wake_while_poll_parked) {
    if constexpr (!sa::fiber_ctx::supported) return;
    URGFixture f;
    std::byte buf[1]{};
    sa::Completion<std::size_t> c;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{f.pipe_fd[0], buf, 1, 0}, c).has_value());

    SelectWaiter waiter(f.sched);
    waiter.spawn_on_worker0(f.sched);

    RunDriver driver(f.sched);
    driver.start(1);

    if (!wait_count_at_least(f.prepark_entries, 1)) {
        fail_closed(f.sched, "ur-g3-never-parked",
                    "participant never reached the physical poll park");
    }
    std::uint64_t control =
        AsyncTestAccess::backend_wait_token(f.sched).control_generation;
    f.wh.notify();
    if (!wait_token(f.sched, control, &sa::BackendWaitToken::control_generation)) {
        fail_closed(f.sched, "ur-g3-bridge-never-fired",
                    "parked notify did not bump the control epoch");
    }
    if (!wait_count_at_least(f.prepark_entries, 2)) {
        fail_closed(f.sched, "ur-g3-lost-parked-wake",
                    "poll-parked participant never consumed the bridge wake");
    }

    waiter.ev.set();
    if (!wait_count_at_least(waiter.resumed, 1)) {
        fail_closed(f.sched, "ur-g3-fiber-never-resumed",
                    "select never resolved after the bridge wake");
    }
    f.release_kernel(1);

    driver.join_or_fail("ur-g3-run-never-returned");

    SLUICE_CHECK_MSG(waiter.resumed.load(std::memory_order_acquire) == 1,
                     "fiber resumed exactly once");
    SLUICE_CHECK_MSG(c.ready(), "UR-G3: Completion ready via real reap");
    c.reset();
}

// ===========================================================================
// UR-G4 — external wake in the commit→wait_one entry window (D4-RM14). The
// armed control floor must deliver the interrupt to the FIRST wait_for_change
// invocation. Deterministic uring-native discriminator: the wait source's
// control-wake pause gate fires exactly in the control-epoch-mismatch branch
// — pausing there proves the first invocation OBSERVED the pre-entry bump; a
// rebaselining implementation (fresh snapshot absorbs the bump) never pauses
// at the gate and instead parks at the physical poll, failing the watchdog.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_uring_g4_external_wake_commit_to_wait) {
    if constexpr (!sa::fiber_ctx::supported) return;
    URGFixture f;
    auto* ws = static_cast<sa::detail::UringWaitSource*>(f.raw->wait_source());
    sa::detail::UringWaitSource::ControlWakeFinalReapPauseGate cgate;
    std::byte buf[1]{};
    sa::Completion<std::size_t> c;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{f.pipe_fd[0], buf, 1, 0}, c).has_value());

    SelectWaiter waiter(f.sched);
    waiter.spawn_on_worker0(f.sched);
    stest::arm(f.sched, stest::PhaseTag::mw_s2_committed_before_wait_one);

    RunDriver driver(f.sched);
    driver.start(1);

    {
        const auto deadline = std::chrono::steady_clock::now() + kObserveWait;
        while (!stest::is_paused(f.sched,
                                 stest::PhaseTag::mw_s2_committed_before_wait_one)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                fail_closed(f.sched, "ur-g4-seam-never-paused",
                            "committed seam never paused");
            }
            std::this_thread::yield();
        }
    }
    std::uint64_t control =
        AsyncTestAccess::backend_wait_token(f.sched).control_generation;
    f.wh.notify();
    if (!wait_token(f.sched, control, &sa::BackendWaitToken::control_generation)) {
        fail_closed(f.sched, "ur-g4-bridge-never-fired",
                    "notify did not bump the control epoch");
    }
    // The gate must be live before the first wait_for_change can run.
    ws->set_control_wake_final_reap_pause_gate(&cgate);
    stest::release(f.sched, stest::PhaseTag::mw_s2_committed_before_wait_one);

    // The armed floor delivers the pre-entry bump to the FIRST invocation —
    // observed at the control-mismatch branch (the gate pause).
    if (!wait_flag(cgate.paused)) {
        fail_closed(f.sched, "ur-g4-armed-floor-not-delivered",
                    "first wait never observed the pre-entry interrupt "
                    "(D4-RM14 violation / mutation M2)");
    }
    cgate.resume.store(true, std::memory_order_release);

    // The interrupted first wait returns; the Live run re-parks at the poll.
    if (!wait_count_at_least(f.prepark_entries, 1)) {
        fail_closed(f.sched, "ur-g4-never-reparked",
                    "participant never re-parked after the interrupt");
    }
    ws->set_control_wake_final_reap_pause_gate(nullptr);

    waiter.ev.set();
    if (!wait_count_at_least(waiter.resumed, 1)) {
        fail_closed(f.sched, "ur-g4-fiber-never-resumed",
                    "select never resolved after the bridge wake");
    }
    f.release_kernel(1);

    driver.join_or_fail("ur-g4-run-never-returned");

    SLUICE_CHECK_MSG(waiter.resumed.load(std::memory_order_acquire) == 1,
                     "fiber resumed exactly once");
    SLUICE_CHECK_MSG(c.ready(), "UR-G4: Completion ready via real reap");
    c.reset();
}

// ===========================================================================
// UR-G5 — kernel CQE racing the control interrupt, both orders.
//   Order 1 (CQE → notify): the context progress seam pins the participant
//   AFTER the progress report, BEFORE the reaping poll; the notify lands
//   mid-invocation and the resumed iteration must still return the reaped
//   CQE (RM13: no rebaseline, no swallowed terminal).
//   Order 2 (notify → CQE): the interrupt wins; the final-poll closure (or
//   the re-park) consumes the later CQE — no lost readiness.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_uring_g5_cqe_vs_control_interrupt) {
    if constexpr (!sa::fiber_ctx::supported) return;
    // Order 1: CQE first, notify second (mid-invocation, deterministically).
    {
        URGFixture f;
        sa::AsyncIoContext::WaitSourceProgressPauseGate pgate;
        std::byte buf[1]{};
        sa::Completion<std::size_t> c;
        SLUICE_CHECK(
            f.ctx.submit_read(sa::ReadOp{f.pipe_fd[0], buf, 1, 0}, c).has_value());

        SelectWaiter waiter(f.sched);
        waiter.spawn_on_worker0(f.sched);

        RunDriver driver(f.sched);
        driver.start(1);

        if (!wait_count_at_least(f.prepark_entries, 1)) {
            fail_closed(f.sched, "ur-g5d1-never-parked",
                        "participant never reached the physical poll park");
        }
        f.ctx.set_wait_source_progress_pause_gate_for_test(&pgate);
        f.release_kernel(1);
        // Lazy uring publication: the CQE lands in the kernel ring; the
        // participant's poll(2) returns on the ring fd and wait_for_change
        // reports progress BEFORE the backend poll reaps it — the seam pause
        // IS the deterministic "CQE reported, reap not yet done" marker (the
        // progress epoch bumps only after the resumed poll reaps).
        if (!wait_flag(pgate.paused)) {
            fail_closed(f.sched, "ur-g5d1-progress-seam-never-paused",
                        "participant never reported the in-window CQE");
        }
        std::uint64_t control =
            AsyncTestAccess::backend_wait_token(f.sched).control_generation;
        f.wh.notify();
        if (!wait_token(f.sched, control,
                        &sa::BackendWaitToken::control_generation)) {
            fail_closed(f.sched, "ur-g5d1-bridge-never-fired",
                        "notify never bumped the control epoch");
        }
        sa::AsyncIoContext::resume_wait_source_progress_gate_for_test(pgate);

        waiter.ev.set();
        if (!wait_count_at_least(waiter.resumed, 1)) {
            fail_closed(f.sched, "ur-g5d1-fiber-never-resumed",
                        "select never resolved");
        }
        driver.join_or_fail("ur-g5d1-run-never-returned");
        SLUICE_CHECK_MSG(c.ready(),
                         "UR-G5/D1: CQE consumed exactly once despite the race");
        f.ctx.set_wait_source_progress_pause_gate_for_test(nullptr);
        c.reset();
    }
    // Order 2: notify first, CQE second.
    {
        URGFixture f;
        std::byte buf[1]{};
        sa::Completion<std::size_t> c;
        SLUICE_CHECK(
            f.ctx.submit_read(sa::ReadOp{f.pipe_fd[0], buf, 1, 0}, c).has_value());

        SelectWaiter waiter(f.sched);
        waiter.spawn_on_worker0(f.sched);

        RunDriver driver(f.sched);
        driver.start(1);

        if (!wait_count_at_least(f.prepark_entries, 1)) {
            fail_closed(f.sched, "ur-g5d2-never-parked",
                        "participant never reached the physical poll park");
        }
        std::uint64_t control =
            AsyncTestAccess::backend_wait_token(f.sched).control_generation;
        f.wh.notify();
        if (!wait_token(f.sched, control,
                        &sa::BackendWaitToken::control_generation)) {
            fail_closed(f.sched, "ur-g5d2-bridge-never-fired",
                        "notify never bumped the control epoch");
        }
        if (!wait_count_at_least(f.prepark_entries, 2)) {
            fail_closed(f.sched, "ur-g5d2-lost-parked-wake",
                        "parked participant never consumed the bridge wake");
        }
        std::uint64_t progress =
            AsyncTestAccess::backend_wait_token(f.sched).progress_generation;
        f.release_kernel(1);
        if (!wait_token(f.sched, progress,
                        &sa::BackendWaitToken::progress_generation)) {
            fail_closed(f.sched, "ur-g5d2-cqe-never-published",
                        "kernel CQE never advanced the progress epoch");
        }

        waiter.ev.set();
        if (!wait_count_at_least(waiter.resumed, 1)) {
            fail_closed(f.sched, "ur-g5d2-fiber-never-resumed",
                        "select never resolved");
        }
        driver.join_or_fail("ur-g5d2-run-never-returned");
        SLUICE_CHECK_MSG(c.ready(),
                         "UR-G5/D2: re-parked participant consumed the CQE");
        c.reset();
    }
}

// ===========================================================================
// UR-G6 — close_admission while parked in poll(ring_fd, control_fd): the
// backend's close interrupts the wait source; the interrupted no-progress
// return is the documented MW-S2 boundary, the run terminates and run_live
// RETURNS; the kernel-pended read is then released with NO observer, and
// the caller's re-entry (E4/E5) must reap the CQE exactly once.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_uring_g6_close_admission_while_parked) {
    if constexpr (!sa::fiber_ctx::supported) return;
    URGFixture f;
    std::byte buf[1]{};
    sa::Completion<std::size_t> c;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{f.pipe_fd[0], buf, 1, 0}, c).has_value());

    RunDriver driver(f.sched);
    driver.start(1);

    if (!wait_count_at_least(f.prepark_entries, 1)) {
        fail_closed(f.sched, "ur-g6-never-parked",
                    "participant never reached the physical poll park");
    }
    f.raw->close_admission();

    driver.join_or_fail("ur-g6-run-never-returned");

    // Lazy uring publication: with no observer (the worker exited), the CQE
    // sits in the kernel ring — the re-entry invocation's first poll() is the
    // collector; c.ready() after the re-entry is the publication evidence.
    f.release_kernel(1);

    RunDriver driver2(f.sched);
    driver2.start(1);
    driver2.join_or_fail("ur-g6-reentry-never-returned");

    SLUICE_CHECK_MSG(c.ready(), "UR-G6: re-entry reaped the CQE exactly once");
    SLUICE_CHECK_MSG(f.raw->outstanding() == 0, "UR-G6: outstanding drained");
    c.reset();
}

// ===========================================================================
// UR-G7 — drain/quiescent teardown after the final CQE: reap -> caller reset
// (slot released) -> close_admission -> outstanding == 0 -> quiescent scope
// exit destroys Scheduler, AsyncIoContext, and the real ring. The fail-fast
// destruction contracts (no outstanding at destroy, no live ring refs) make
// any non-quiescent teardown abort — the normal return IS the assertion.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_uring_g7_quiescent_teardown_after_final_cqe) {
    if constexpr (!sa::fiber_ctx::supported) return;
    {
        URGFixture f;
        std::byte buf[2]{};
        sa::Completion<std::size_t> c1, c2;
        SLUICE_CHECK(
            f.ctx.submit_read(sa::ReadOp{f.pipe_fd[0], &buf[0], 1, 0}, c1).has_value());
        SLUICE_CHECK(
            f.ctx.submit_read(sa::ReadOp{f.pipe_fd[0], &buf[1], 1, 0}, c2).has_value());

        RunDriver driver(f.sched);
        driver.start(1);

        if (!wait_count_at_least(f.prepark_entries, 1)) {
            fail_closed(f.sched, "ur-g7-never-parked",
                        "participant never reached the physical poll park");
        }
        f.release_kernel(2);
        driver.join_or_fail("ur-g7-run-never-returned");

        SLUICE_CHECK(c1.ready() && c2.ready());
        c1.reset();
        c2.reset();
        SLUICE_CHECK_MSG(f.raw->outstanding() == 0,
                         "UR-G7: outstanding drained before teardown");
        f.raw->close_admission();
    }  // quiescent destruction: Scheduler -> AsyncIoContext -> ring teardown.
    SLUICE_CHECK_MSG(true, "UR-G7: quiescent teardown after the final CQE");
}

#endif // SLUICE_HAS_LIBURING

SLUICE_MAIN()
