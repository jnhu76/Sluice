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
// Determinism policy (production-test-plan.md §1): NO sleep_for and NO
// yield-spin as ordering proof and NO wall-clock deadline as a correctness
// verdict (issue #123; migrated to blocking handshakes by issue #129). Every
// correctness observation is a BLOCKING handshake — atomic::wait on a
// store+notify latch (prepark_entries / fiber resumed / pause-gate paused),
// the controller's cv wait_paused, or the UringWaitSource test-only epoch
// observer (which parks on the wait source's OWN mtx_ + cv_ domain and
// re-reads the ACTUAL epoch, so no second source of truth) — none of which
// depends on scheduler latency. The ONE bounded element per case is a
// case-level watchdog: a deadlock safety net that aborts fail-closed (rc 70)
// only on a genuine no-progress stall (progress frozen for >= the full
// budget — the issue #101 model), printing case, phase, gate state, park
// domain, backend token, outstanding, backend-ready, prepark count, and pid.
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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
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

// The case-level watchdog budget. This is a deadlock safety net ONLY (see
// UringWatchdog): every correctness observation in this suite is a blocking
// handshake, so a correct case converges in milliseconds even under
// host-scheduler starvation; the budget exists solely to convert a GENUINE
// protocol stall (progress frozen for the entire budget) into a bounded
// fail-closed abort. It is deliberately generous so starvation — which
// pauses progress for seconds and resumes — never reaches a full-budget
// freeze.
constexpr auto kWatchdogSeconds = std::chrono::seconds(30);

struct URGFixture;  // defined below; UringProbe holds only a pointer.

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// Deterministic construction-verdict helper (test thread only, NOT the
// watchdog): a wrong state at an observation point (e.g. a park domain that
// is not Backend) is the FAILURE verdict. dump_park_forensics is safe here —
// it runs on the test thread at a point where the workers are parked in
// domains that hold no Scheduler global lock, so it cannot deadlock.
[[noreturn]] void fail_closed(Scheduler& sched, const char* tag, const char* msg) {
    AsyncTestAccess::dump_park_forensics(sched, tag);
    std::fprintf(stderr, "PHASE-G-URING FAIL-CLOSED: %s (%s); pid=%d\n",
                 msg, tag, static_cast<int>(::getpid()));
    std::_Exit(70);
}

// ---------------------------------------------------------------------------
// Case/phase attribution (see UringWatchdog below, defined after
// URGFixture/SelectWaiter so its forensics dump can dereference the fixture).
// ---------------------------------------------------------------------------
struct UringProbe {
    const char* name = nullptr;
    URGFixture* fx = nullptr;
    std::atomic<const char*> phase{nullptr};
    std::atomic<std::uint64_t> progress_epoch{0};
    const std::atomic<bool>* gate_paused = nullptr;
    const std::atomic<bool>* gate_exited = nullptr;

    void set_phase(const char* p) noexcept {
        phase.store(p, std::memory_order_release);
        progress_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    void bind_gate(const std::atomic<bool>& paused,
                   const std::atomic<bool>& exited) noexcept {
        gate_paused = &paused;
        gate_exited = &exited;
    }
};

// ---------------------------------------------------------------------------
// Blocking observation helpers (issues #123/#129). All of them are zero-CPU
// handshakes on state that is published with a matching notify — none depends
// on scheduler latency, and none carries a wall-clock deadline. A genuine
// stall (the state never changes) is bounded by the case watchdog, never by a
// correctness deadline.
// ---------------------------------------------------------------------------

// One-way latch published with store+notify_all (the wait source's
// wait_phase_flag, the control-wake gate's paused flag, and the context
// progress seam's paused flag all do exactly that).
void wait_flag(std::atomic<bool>& flag) {
    while (!flag.load(std::memory_order_acquire)) {
        flag.wait(false, std::memory_order_acquire);
    }
}

// Monotonic counter incremented WITH a matching notify_all (prepark_entries
// in UringWaitSource, resumed in the test fibers below).
void wait_count_at_least(std::atomic<int>& counter, int value) {
    int cur = counter.load(std::memory_order_acquire);
    while (cur < value) {
        counter.wait(cur, std::memory_order_acquire);
        cur = counter.load(std::memory_order_acquire);
    }
}

// run_live driver. A run that never returns IS the mutant/pre-fix verdict —
// the case-level watchdog bounds that and aborts fail-closed (rc 70) with
// the forensic state; join() itself is a blocking handshake, never a
// deadline.
struct RunDriver {
    Scheduler& sched;
    std::thread th;
    std::atomic<bool> done{false};
    explicit RunDriver(Scheduler& s) : sched(s) {}
    void start(unsigned workers) {
        th = std::thread([this, workers] {
            sched.run_live(workers);
            done.store(true, std::memory_order_release);
            done.notify_all();
        });
    }
    void join() {
        while (!done.load(std::memory_order_acquire)) {
            done.wait(false, std::memory_order_acquire);
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
    sa::detail::UringWaitSource* ws = nullptr;
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
        self->ws = static_cast<sa::detail::UringWaitSource*>(
            backend->wait_source());
        backend->set_wait_phase_flag_for_test(&self->wait_phase_entered);
        backend->set_wait_prepark_counter_for_test(&self->prepark_entries);
        return backend;
    }
};

// One UringWaitSource epoch field advancing past `observed`. The blocking
// channel is the wait source's test-only epoch observer — it parks on the
// SAME mtx_ + cv_ domain that interrupt_all()/signal_progress() use to
// advance the ACTUAL epochs (single source of truth; no second counter or
// notification channel — sharing the domain is what makes a lost wake
// impossible). The cv is shared with the durable-broadcast gate: each parked
// waiter re-checks its own predicate, so a gate release that wakes this
// observer is spurious, never lost. If the epoch moves on the other field
// first, the observer wakes, the loop re-checks our field, and it re-blocks
// — still zero CPU.
template <class T>
void wait_token(URGFixture& f, T sa::BackendWaitToken::*field, T observed) {
    for (;;) {
        const auto token = f.ws->snapshot();
        if (token.*field > observed) return;
        f.raw->wait_epoch_changed_for_test(token);
    }
}

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
            // atomic::wait consumers: notify after the increment so the test
            // can block zero-CPU on this counter (blocking handshake).
            resumed.notify_all();
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
            // atomic::wait consumers: notify after the increment so the test
            // can block zero-CPU on this counter (blocking handshake).
            resumed.notify_all();
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

// ---------------------------------------------------------------------------
// The single bounded watchdog per case (issues #123/#129; the Uring twin of
// the closeout CloseoutWatchdog).
//
// The case advances `phase` and bumps `progress_epoch` at every transition;
// the watchdog treats a progress_epoch frozen for >= the full budget as a
// genuine no-progress stall — the ONLY abort trigger (issue #101 model: a
// case-total wall-clock deadline is NOT a liveness oracle; only a real
// freeze is). It is a deadlock safety net ONLY: every correctness
// observation in this suite is a blocking handshake (zero CPU, no
// scheduler-latency dependency), so a correct case converges in
// milliseconds even under host-scheduler starvation, and starvation — which
// pauses progress for seconds and resumes — never reaches a full-budget
// freeze. The watchdog reads ONLY lock-free atomics and non-blocking (try)
// reads — it must never block behind the defect it is diagnosing (a paused
// control-wake gate holds the wait-source leaf mutex; a stalled worker may
// hold Scheduler global_mtx_ at a causal seam).
// ---------------------------------------------------------------------------
class UringWatchdog {
  public:
    explicit UringWatchdog(std::chrono::seconds budget, const UringProbe& probe)
        : probe_(&probe),
          budget_ms_(std::chrono::duration_cast<std::chrono::milliseconds>(
              budget)) {
        try {
            thread_ = std::thread([this] { run(); });
        } catch (...) {
            // Fail-closed: the watchdog is the only bound on the blocking
            // handshakes, so a thread-creation failure must fail the test
            // rather than leave a genuine stall unbounded.
            std::fprintf(stderr,
                         "phase_g_closeout_uring: watchdog thread creation "
                         "failed; aborting (fail-closed: no unbounded "
                         "blocking wait)\n");
            std::abort();
        }
    }
    ~UringWatchdog() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            done_.store(true, std::memory_order_release);
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    UringWatchdog(const UringWatchdog&) = delete;
    UringWatchdog& operator=(const UringWatchdog&) = delete;

  private:
    static constexpr auto kPollInterval = std::chrono::milliseconds(100);

    void run() noexcept {
        std::unique_lock<std::mutex> lk(mtx_);
        auto last_progress_at = std::chrono::steady_clock::now();
        auto last_epoch =
            probe_->progress_epoch.load(std::memory_order_acquire);
        while (true) {
            const bool done = cv_.wait_until(
                lk, std::chrono::steady_clock::now() + kPollInterval, [this] {
                    return done_.load(std::memory_order_acquire);
                });
            if (done) return;
            const auto now = std::chrono::steady_clock::now();
            const auto epoch =
                probe_->progress_epoch.load(std::memory_order_acquire);
            if (epoch != last_epoch) {
                last_epoch = epoch;
                last_progress_at = now;
            }
            // Genuine stall: no phase transition for the ENTIRE budget.
            if (now - last_progress_at >= budget_ms_) {
                diagnose_and_abort(now, last_progress_at);
            }
        }
    }

    [[noreturn]] void diagnose_and_abort(
        std::chrono::steady_clock::time_point now,
        std::chrono::steady_clock::time_point last_progress_at) noexcept {
        const char* ph = probe_->phase.load(std::memory_order_acquire);
        const auto frozen = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_progress_at);
        const auto total =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start_);
        std::fprintf(stderr,
                     "PHASE-G-URING WATCHDOG: genuine no-progress stall "
                     "(progress frozen for >= the full budget); aborting\n");
        std::fprintf(stderr,
                     "  case=%s phase=%s progress_epoch=%llu frozen=%lldms "
                     "budget=%lldms total=%lldms pid=%d\n",
                     probe_->name ? probe_->name : "?", ph ? ph : "?",
                     static_cast<unsigned long long>(
                         probe_->progress_epoch.load(std::memory_order_acquire)),
                     static_cast<long long>(frozen.count()),
                     static_cast<long long>(budget_ms_.count()),
                     static_cast<long long>(total.count()),
                     static_cast<int>(::getpid()));
        if (probe_->gate_paused != nullptr) {
            std::fprintf(stderr, "  gate: paused=%d exited=%d\n",
                         probe_->gate_paused->load(std::memory_order_acquire),
                         probe_->gate_exited->load(std::memory_order_acquire));
        }
        if (probe_->fx != nullptr) {
            URGFixture& f = *probe_->fx;
            // Watchdog rule (issue #128 review): every read below is
            // lock-free or a try-read — the watchdog must never block
            // behind the stall it is diagnosing, so a contended leaf domain
            // (a paused control-wake gate holds the wait-source mutex)
            // prints "locked" instead of waiting for the mutex.
            const auto tok = f.raw->try_wait_token_for_test();
            const auto outstanding = f.raw->try_outstanding_for_test();
            const auto ready = f.raw->try_backend_ready_count_for_test();
            bool park_available = false;
            const auto park = AsyncTestAccess::worker_park_domain_try(
                f.sched, 0, park_available);
            char tok_buf[64];
            char out_buf[40];
            char ready_buf[48];
            if (tok) {
                std::snprintf(tok_buf, sizeof tok_buf, "(progress=%llu,ctrl=%llu)",
                              static_cast<unsigned long long>(
                                  tok->progress_generation),
                              static_cast<unsigned long long>(
                                  tok->control_generation));
            } else {
                std::snprintf(tok_buf, sizeof tok_buf, "(locked)");
            }
            if (outstanding) {
                std::snprintf(out_buf, sizeof out_buf, "outstanding=%zu",
                              *outstanding);
            } else {
                std::snprintf(out_buf, sizeof out_buf, "outstanding=locked");
            }
            if (ready) {
                std::snprintf(ready_buf, sizeof ready_buf, "backend_ready=%zu",
                              *ready);
            } else {
                std::snprintf(ready_buf, sizeof ready_buf,
                              "backend_ready=locked");
            }
            std::fprintf(stderr,
                         "  wait_phase_entered=%d prepark=%d token=%s %s %s "
                         "park_domain[0]=%d(%s)\n",
                         f.wait_phase_entered.load(std::memory_order_acquire)
                                 ? 1
                                 : 0,
                         f.prepark_entries.load(std::memory_order_acquire),
                         tok_buf, out_buf, ready_buf, static_cast<int>(park),
                         park_available ? "read" : "locked");
        }
        std::fflush(stderr);
        std::_Exit(70);
    }

    const UringProbe* probe_;
    std::chrono::milliseconds budget_ms_;
    std::chrono::steady_clock::time_point start_{
        std::chrono::steady_clock::now()};
    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> done_{false};
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

    UringProbe probe;
    probe.name = "ur-g1-cqe-between-snapshot-and-park";
    probe.fx = &f;
    UringWatchdog wd(kWatchdogSeconds, probe);

    probe.set_phase("run");
    RunDriver driver(f.sched);
    driver.start(1);

    // Blocking handshake (zero-CPU): the participant reached the final
    // pre-poll point — the physical poll(2) park. A construction failure is
    // bounded by the case watchdog, never by a correctness deadline.
    probe.set_phase("observe-park");
    wait_count_at_least(f.prepark_entries, 1);
    if (AsyncTestAccess::worker_park_domain(f.sched, 0) !=
        sa::WorkerState::ParkDomain::Backend) {
        fail_closed(f.sched, "ur-g1-wrong-domain", "park domain not Backend");
    }
    // Baseline BEFORE the release: the pipe write is the ONLY trigger for
    // the CQE, so the progress advance is strictly after this baseline (the
    // baseline-before-trigger discipline, issue #123).
    const auto progress_before = f.ws->snapshot();
    f.release_kernel(1);
    probe.set_phase("observe-cqe-published");
    wait_token(f, &sa::BackendWaitToken::progress_generation,
               progress_before.progress_generation);

    probe.set_phase("join");
    driver.join();
    probe.set_phase("verify");

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

    UringProbe probe;
    probe.name = "ur-g2-multi-cqe-coalesce";
    probe.fx = &f;
    UringWatchdog wd(kWatchdogSeconds, probe);

    probe.set_phase("run");
    RunDriver driver(f.sched);
    driver.start(1);

    probe.set_phase("observe-park");
    wait_count_at_least(f.prepark_entries, 1);
    f.release_kernel(3);

    probe.set_phase("join");
    driver.join();
    probe.set_phase("verify");

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

    UringProbe probe;
    probe.name = "ur-g3-external-wake-while-poll-parked";
    probe.fx = &f;
    UringWatchdog wd(kWatchdogSeconds, probe);

    probe.set_phase("run");
    RunDriver driver(f.sched);
    driver.start(1);

    probe.set_phase("observe-park");
    wait_count_at_least(f.prepark_entries, 1);
    // Baseline BEFORE the notify: the bridge is the ONLY control-epoch
    // publisher here, so the advance is strictly after this baseline.
    const auto control_before = f.ws->snapshot();
    f.wh.notify();
    probe.set_phase("observe-bridge-fired");
    wait_token(f, &sa::BackendWaitToken::control_generation,
               control_before.control_generation);
    // The wake is consumed: the interrupted poll returns and the Live run
    // re-parks (external-wake-possible select wait still registered).
    probe.set_phase("observe-repark");
    wait_count_at_least(f.prepark_entries, 2);

    probe.set_phase("resolve-select");
    waiter.ev.set();
    wait_count_at_least(waiter.resumed, 1);
    f.release_kernel(1);

    probe.set_phase("join");
    driver.join();
    probe.set_phase("verify");

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
    auto* ws = f.ws;
    sa::detail::UringWaitSource::ControlWakeFinalReapPauseGate cgate;
    std::byte buf[1]{};
    sa::Completion<std::size_t> c;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{f.pipe_fd[0], buf, 1, 0}, c).has_value());

    SelectWaiter waiter(f.sched);
    waiter.spawn_on_worker0(f.sched);
    stest::arm(f.sched, stest::PhaseTag::mw_s2_committed_before_wait_one);

    UringProbe probe;
    probe.name = "ur-g4-external-wake-commit-to-wait";
    probe.fx = &f;
    probe.bind_gate(cgate.paused, cgate.exited);
    UringWatchdog wd(kWatchdogSeconds, probe);

    probe.set_phase("run");
    RunDriver driver(f.sched);
    driver.start(1);

    // Blocking seam observation (zero-CPU controller-cv handshake): the
    // participant is held at the commit→wait_one boundary. A construction
    // failure (seam never paused) is bounded by the case watchdog.
    probe.set_phase("seam-pause");
    stest::wait_paused(f.sched, stest::PhaseTag::mw_s2_committed_before_wait_one);
    // Baseline BEFORE the notify (the bridge is the ONLY control-epoch
    // publisher here) and BEFORE the gate install (a paused gate holds the
    // wait-source leaf mutex; the blocking snapshot below must not race it).
    const auto control_before = f.ws->snapshot();
    f.wh.notify();
    probe.set_phase("observe-bridge-fired");
    wait_token(f, &sa::BackendWaitToken::control_generation,
               control_before.control_generation);
    // The gate must be live before the first wait_for_change can run.
    ws->set_control_wake_final_reap_pause_gate(&cgate);
    stest::release(f.sched, stest::PhaseTag::mw_s2_committed_before_wait_one);

    // The armed floor delivers the pre-entry bump to the FIRST invocation —
    // observed at the control-mismatch branch (the gate pause; blocking
    // handshake on the store+notify published paused flag).
    probe.set_phase("observe-gate-paused");
    wait_flag(cgate.paused);
    // The gate consumer POLLS resume (yield-spin under the wait-source leaf
    // mutex — the pre-existing seam transport), so a plain store IS a legal
    // publisher here; atomic::wait wake rules (issue #128 §11.2) apply only
    // to blocking consumers.
    cgate.resume.store(true, std::memory_order_release);

    // The interrupted first wait returns; the Live run re-parks at the poll.
    probe.set_phase("observe-repark");
    wait_count_at_least(f.prepark_entries, 1);
    ws->set_control_wake_final_reap_pause_gate(nullptr);

    probe.set_phase("resolve-select");
    waiter.ev.set();
    wait_count_at_least(waiter.resumed, 1);
    f.release_kernel(1);

    probe.set_phase("join");
    driver.join();
    probe.set_phase("verify");

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

        UringProbe probe;
        probe.name = "ur-g5d1-cqe-vs-control-progress-first";
        probe.fx = &f;
        probe.bind_gate(pgate.paused, pgate.exited);
        UringWatchdog wd(kWatchdogSeconds, probe);

        probe.set_phase("run");
        RunDriver driver(f.sched);
        driver.start(1);

        probe.set_phase("observe-park");
        wait_count_at_least(f.prepark_entries, 1);
        f.ctx.set_wait_source_progress_pause_gate_for_test(&pgate);
        f.release_kernel(1);
        // Lazy uring publication: the CQE lands in the kernel ring; the
        // participant's poll(2) returns on the ring fd and wait_for_change
        // reports progress BEFORE the backend poll reaps it — the seam pause
        // IS the deterministic "CQE reported, reap not yet done" marker (the
        // progress epoch bumps only after the resumed poll reaps). The
        // paused flag is published store+notify — blocking handshake.
        probe.set_phase("observe-progress-seam-paused");
        wait_flag(pgate.paused);
        // Baseline BEFORE the notify, taken while the seam holds NO lock (the
        // context pauses between wait_for_change's return and the reaping
        // poll, outside access_mtx_ and the wait-source mutex).
        const auto control_before = f.ws->snapshot();
        f.wh.notify();
        probe.set_phase("observe-bridge-fired");
        wait_token(f, &sa::BackendWaitToken::control_generation,
                   control_before.control_generation);
        // The ONLY supported resume publisher (store + notify_all — the
        // paused thread blocks in resume.wait(false)).
        sa::AsyncIoContext::resume_wait_source_progress_gate_for_test(pgate);

        probe.set_phase("resolve-select");
        waiter.ev.set();
        wait_count_at_least(waiter.resumed, 1);
        probe.set_phase("join");
        driver.join();
        probe.set_phase("verify");
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

        UringProbe probe;
        probe.name = "ur-g5d2-cqe-vs-control-notify-first";
        probe.fx = &f;
        UringWatchdog wd(kWatchdogSeconds, probe);

        probe.set_phase("run");
        RunDriver driver(f.sched);
        driver.start(1);

        probe.set_phase("observe-park");
        wait_count_at_least(f.prepark_entries, 1);
        const auto control_before = f.ws->snapshot();
        f.wh.notify();
        probe.set_phase("observe-bridge-fired");
        wait_token(f, &sa::BackendWaitToken::control_generation,
                   control_before.control_generation);
        probe.set_phase("observe-repark");
        wait_count_at_least(f.prepark_entries, 2);
        // Baseline BEFORE the release (the pipe write is the ONLY CQE
        // trigger, so the progress advance is strictly after it).
        const auto progress_before = f.ws->snapshot();
        f.release_kernel(1);
        probe.set_phase("observe-cqe-published");
        wait_token(f, &sa::BackendWaitToken::progress_generation,
                   progress_before.progress_generation);

        probe.set_phase("resolve-select");
        waiter.ev.set();
        wait_count_at_least(waiter.resumed, 1);
        probe.set_phase("join");
        driver.join();
        probe.set_phase("verify");
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

    UringProbe probe;
    probe.name = "ur-g6-close-admission-while-parked";
    probe.fx = &f;
    UringWatchdog wd(kWatchdogSeconds, probe);

    probe.set_phase("run");
    RunDriver driver(f.sched);
    driver.start(1);

    probe.set_phase("observe-park");
    wait_count_at_least(f.prepark_entries, 1);
    f.raw->close_admission();

    // The interrupted no-progress park terminates the run (no select wait —
    // external wake not possible). A parked-forever run is the failure
    // (bounded by the case watchdog).
    probe.set_phase("join");
    driver.join();

    // Lazy uring publication: with no observer (the worker exited), the CQE
    // sits in the kernel ring — the re-entry invocation's first poll() is the
    // collector; c.ready() after the re-entry is the publication evidence.
    f.release_kernel(1);

    probe.set_phase("reentry");
    RunDriver driver2(f.sched);
    driver2.start(1);
    driver2.join();
    probe.set_phase("verify");

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

        UringProbe probe;
        probe.name = "ur-g7-quiescent-teardown-after-final-cqe";
        probe.fx = &f;
        UringWatchdog wd(kWatchdogSeconds, probe);

        probe.set_phase("run");
        RunDriver driver(f.sched);
        driver.start(1);

        probe.set_phase("observe-park");
        wait_count_at_least(f.prepark_entries, 1);
        f.release_kernel(2);
        probe.set_phase("join");
        driver.join();

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
