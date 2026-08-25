// phase_g_closeout_test — Phase G final closeout: deterministic causal proofs
// for the commit→park wake protocol (Cases A–D) and the ThreadPool TP-G1..G7
// race matrix (docs/history/implementation-plans/phase-g-backend-progress-wake.md §5).
//
// The audit question (closeout §2): is the `backend_wait_active_` gate in
// Scheduler::signal_wake_locked merely an optimization, and can an external
// Scheduler wake published while the gate is still false be LOST once the
// MW-S2 participant arms, sets the gate, and parks in ctx_.wait_one()?
//
// Closure argument under test (one mechanism per interval):
//
//   [decide .. arm]      external payloads serialize under global_mtx_
//                        (route/flag/waitq/deadline publications) — a payload
//                        is either visible to the Phase-B re-drain (the
//                        participant never parks as MW-S2) or its publisher's
//                        signal reads gate==true (the bridge fires). Case A
//                        proves the re-drain half deterministically: the
//                        notify lands with the gate still false and the
//                        control epoch provably UNCHANGED (bridge skipped) —
//                        the wake is consumed by the re-drain, not the bridge.
//   [arm .. wait_one]    D4-RM14 armed control floor. Case B: notify lands
//                        after arm+gate, before wait_one entry — the armed
//                        floor makes the first wait observe the bump instead
//                        of rebaselining it into a past event (pre-park
//                        return, re-park observed via the prepark counter).
//   [wait_one entry .. ] ReadyWaitSource predicate: control or progress epoch
//                        advance releases the cv park in EVERY sub-state.
//                        Case C: notify after the park-entry marker.
//   [ready vs notify]    Case D: backend terminal racing the control
//                        interrupt in both orders — exactly-once reap, no
//                        lost readiness, no lost wake, no double route.
//
// Level-triggered ready-flag stores (a bare flag store is a legal producer)
// are closed by the E5-A2 bounded observation interval, NOT by the bridge —
// they are deliberately outside this suite's unbounded-park constructions
// (see phase-g design §3.5 / SW E5-A2 capture at the Phase-B commit).
//
// Determinism policy (production-test-plan.md §1): NO sleep_for as ordering
// proof and NO wall-clock deadline as a correctness verdict (issue #123).
// Every correctness observation is a BLOCKING handshake — atomic::wait on a
// store+notify latch (wait_phase_entered / prepark_entries / resumed /
// progress-seam paused), the controller's cv wait_paused, or the
// ReadyWaitSource test-only epoch observer (which re-reads the ACTUAL epoch,
// so no second source of truth) — none of which depends on scheduler
// latency. The ONE bounded element per case is a case-level watchdog: a
// deadlock safety net that aborts fail-closed (rc 70) only on a genuine
// no-progress stall (progress frozen for >= the full budget — the issue #101
// model), printing case, phase, gate state, park domain, backend token,
// outstanding, backend-ready, prepark count, and pid. Mutation detectors
// (closeout M1/M2) hang in these constructions and exit fail-closed (rc 70)
// via the watchdog.
//
// Gated to x86_64 (fiber_ctx::supported) for parity with the rest of E13.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/select.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include "async_test_control.hpp"
#include "harness.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace sa = sluice::async;
namespace stest = sluice_async_test;

using AsyncTestAccess = sa::Scheduler::AsyncTestAccess;
using Scheduler = sa::Scheduler;
using Event = sa::Event;
using Fiber = sa::Fiber;
using FiberState = sa::FiberState;
using SelectKind = sa::SelectKind;

namespace {

// The case-level watchdog budget. This is a deadlock safety net ONLY (see
// CloseoutWatchdog): every correctness observation in this suite is a
// blocking handshake, so a correct case converges in milliseconds even under
// host-scheduler starvation; the budget exists solely to convert a GENUINE
// protocol stall (progress frozen for the entire budget) into a bounded
// fail-closed abort. It is deliberately generous so starvation — which pauses
// progress for seconds and resumes — never reaches a full-budget freeze.
constexpr auto kWatchdogSeconds = std::chrono::seconds(30);

struct PGFixture;  // defined below; CloseoutProbe holds only a pointer.

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
    std::fprintf(stderr, "PHASE-G-CLOSEOUT FAIL-CLOSED: %s (%s); pid=%d\n",
                 msg, tag, static_cast<int>(::getpid()));
    std::_Exit(70);
}

// ---------------------------------------------------------------------------
// Case/phase attribution (see CloseoutWatchdog below, defined after
// PGFixture/SelectWaiter so its forensics dump can dereference the fixture).
// ---------------------------------------------------------------------------
struct CloseoutProbe {
    const char* name = nullptr;
    PGFixture* fx = nullptr;
    std::atomic<const char*> phase{nullptr};
    std::atomic<std::uint64_t> progress_epoch{0};
    const std::atomic<bool>* gate_paused = nullptr;
    const std::atomic<bool>* gate_resume = nullptr;
    const std::atomic<bool>* gate_exited = nullptr;

    void set_phase(const char* p) noexcept {
        phase.store(p, std::memory_order_release);
        progress_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    void bind_gate(const std::atomic<bool>& paused,
                   const std::atomic<bool>& resume,
                   const std::atomic<bool>& exited) noexcept {
        gate_paused = &paused;
        gate_resume = &resume;
        gate_exited = &exited;
    }
};

// ---------------------------------------------------------------------------
// Blocking observation helpers (issue #123). All of them are zero-CPU
// handshakes on state that is published with a matching notify — none depends
// on scheduler latency, and none carries a wall-clock deadline. A genuine
// stall (the state never changes) is bounded by the case watchdog, never by
// a correctness deadline.
// ---------------------------------------------------------------------------

// One-way latch published with store+notify_all (the ready wait source's
// wait_phase_flag and the progress seam's paused flag both do exactly that).
void wait_flag(std::atomic<bool>& flag) {
    while (!flag.load(std::memory_order_acquire)) {
        flag.wait(false, std::memory_order_acquire);
    }
}

// Monotonic counter incremented WITH a matching notify_all (prepark_entries
// in ReadyWaitSource, resumed in the test fiber below).
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

// Direct-Scheduler fixture over the production split-wait backend. The
// WorkerRunningPauseGate freezes the first accepted read mid-flight (the
// syscall never completes until resume), so `outstanding` is stable and the
// MW-S2 park is UNBOUNDED (no deadline, no ready-flag wait) — every wake in
// these cases must arrive through the mechanism under test, never a timeout.
struct PGFixture {
    sa::ThreadPoolBackend::WorkerRunningPauseGate gate;
    std::atomic<bool> wait_phase_entered{false};
    std::atomic<int> prepark_entries{0};
    sa::ThreadPoolBackend* raw = nullptr;
    sa::AsyncIoContext ctx{make_backend(this)};
    Scheduler sched;
    stest::ControllerGuard ctrl;
    sa::SchedulerWakeHandle wh;

    PGFixture()
        : sched(ctx), ctrl(sched), wh(sched.make_wake_handle()) {}

    ~PGFixture() {
        // Disarm the backend test seams while `raw` still points at the
        // backend owned by ctx (member destruction order: wh, ctrl, sched,
        // ctx — raw outlives them as a pointer, but the backend dies with
        // ctx, so this is the last valid moment).
        raw->set_running_pause_gate(nullptr);
        raw->set_wait_phase_flag_for_test(nullptr);
        raw->set_wait_prepark_counter_for_test(nullptr);
    }

    PGFixture(const PGFixture&) = delete;
    PGFixture& operator=(const PGFixture&) = delete;

  private:
    static std::unique_ptr<sa::ThreadPoolBackend> make_backend(PGFixture* self) {
        auto backend = std::make_unique<sa::ThreadPoolBackend>(
            sa::ThreadPoolConfig{/*request_capacity=*/4, /*worker_count=*/2});
        self->raw = backend.get();
        backend->set_running_pause_gate(&self->gate);
        backend->set_wait_phase_flag_for_test(&self->wait_phase_entered);
        backend->set_wait_prepark_counter_for_test(&self->prepark_entries);
        return backend;
    }
};

// One ReadyWaitSource epoch field advancing past `observed`. The blocking
// channel is the ready-wait source's test-only epoch observer — it parks on
// the SAME mtx_ + ready_cv_ domain that interrupt_all()/signal_progress()
// use to advance the ACTUAL epochs (single source of truth; no second
// counter or notification channel — sharing the domain is what makes a
// lost wake impossible). If the epoch moves on the other field first, the
// observer wakes, the loop re-checks our field, and it re-blocks — still
// zero CPU.
template <class T>
void wait_token(PGFixture& f, T sa::BackendWaitToken::*field, T observed) {
    for (;;) {
        const auto token = f.raw->wait_source()->snapshot();
        if (token.*field > observed) return;
        f.raw->wait_epoch_changed_for_test(token);
    }
}

// A one-byte seeded temp file; the gated read on it never completes until
// resume_threadpool_gate. The path is unique per process and per instance
// (pid + monotonic counter), following the Phase-G forensics temp-path
// pattern — isolation hardening against concurrent suite processes sharing
// the default-gate temp files (issue #123 requirement 6; the closeout
// constructions are single-process deterministic proofs, but a shared fixed
// path under parallel execution is a hygiene hazard).
struct TempFile {
    int fd = -1;
    TempFile() {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_phase_g_closeout_" + std::to_string(::getpid()) +
                  "_" + std::to_string(counter_++) + ".tmp"))
                    .string();
        fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            std::fprintf(stderr, "phase_g_closeout: temp open failed\n");
            std::exit(1);
        }
        const std::byte seed[1] = {std::byte{0x71}};
        if (::pwrite(fd, seed, 1, 0) != 1) {
            std::fprintf(stderr, "phase_g_closeout: temp seed failed\n");
            std::exit(1);
        }
    }
    ~TempFile() {
        ::close(fd);
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

  private:
    std::string path_;
    static inline long counter_ = 0;
};

// The external-wake-possible fiber: parked in select() on an Event, which
// registers a waiting_select_count_ wait (external wake possible) WITHOUT a
// waiting_ready_ entry, so the MW-S2 backend park stays UNBOUNDED (E5-A2
// caps apply only to level-triggered ready-flag waits). ev.set() from an
// external thread is the canonical G-serialized external publication.
struct SelectWaiter {
    Event ev;
    sa::SelectResult captured;
    std::atomic<int> resumed{0};
    Fiber fb;
    FiberStack stack;

    explicit SelectWaiter(Scheduler& sched) : ev(sched, /*initially_set=*/false) {
        fb.set_entry([this, &sched](Fiber&) {
            captured = sa::select(sched, sa::EventSelectCase{ev});
            resumed.fetch_add(1, std::memory_order_acq_rel);
            // atomic::wait consumers: notify after the increment so the test
            // can block zero-CPU on this counter (blocking handshake).
            resumed.notify_all();
        });
    }
    void spawn_on_worker0(Scheduler& sched) {
        if (!sched.init_fiber(fb, stack.base(), stack.size())) {
            std::fprintf(stderr, "phase_g_closeout: init_fiber failed\n");
            std::exit(1);
        }
        sched.spawn_on(fb, /*worker_id=*/0);
    }
};

// ---------------------------------------------------------------------------
// The single bounded watchdog per case (issue #123).
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
// reads — it must never block behind the defect it is diagnosing (a stalled
// worker may hold global_mtx_ at a causal seam).
// ---------------------------------------------------------------------------
class CloseoutWatchdog {
  public:
    explicit CloseoutWatchdog(std::chrono::seconds budget,
                              const CloseoutProbe& probe)
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
                         "phase_g_closeout: watchdog thread creation failed; "
                         "aborting (fail-closed: no unbounded blocking wait)\n");
            std::abort();
        }
    }
    ~CloseoutWatchdog() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            done_.store(true, std::memory_order_release);
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    CloseoutWatchdog(const CloseoutWatchdog&) = delete;
    CloseoutWatchdog& operator=(const CloseoutWatchdog&) = delete;

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
                     "PHASE-G-CLOSEOUT WATCHDOG: genuine no-progress stall "
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
        if (probe_->gate_resume != nullptr) {
            std::fprintf(stderr, "  gate: paused=%d resume=%d exited=%d\n",
                         probe_->gate_paused->load(std::memory_order_acquire),
                         probe_->gate_resume->load(std::memory_order_acquire),
                         probe_->gate_exited->load(std::memory_order_acquire));
        }
        if (probe_->fx != nullptr) {
            PGFixture& f = *probe_->fx;
            // Watchdog rule (issue #128 review): every read below is
            // lock-free or a try-read — the watchdog must never block
            // behind the stall it is diagnosing, so a contended leaf domain
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
                std::snprintf(tok_buf, sizeof tok_buf, "(ready=%llu,ctrl=%llu)",
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

    const CloseoutProbe* probe_;
    std::chrono::milliseconds budget_ms_;
    std::chrono::steady_clock::time_point start_{
        std::chrono::steady_clock::now()};
    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> done_{false};
};

}  // namespace

// ===========================================================================
// Case A (TP-G3 "before arm") — external wake publication lands while the
// gate is still false; the bridge provably skips (control epoch unchanged);
// the Phase-B re-drain must consume the payload instead.
//
// Deterministic construction:
//   1. The worker elects MW-S2 and pauses at the mw_admission_phase_b seam —
//      BEFORE the Phase-B global_mtx_ section, so the gate is false and no
//      commit/arm has happened.
//   2. ev.set() from the test thread: payload (route under global_mtx_) +
//      signal_wake_locked. OBSERVE: the control generation is UNCHANGED
//      across the call — the bridge skipped exactly as in the audited race.
//   3. Release the seam. The participant's Phase-B re-drain MUST see the
//      routed runnable (global_mtx_ serialization), abandon the admission,
//      and run the fiber — the wake is consumed by re-evaluation of the
//      persistent state, NOT by the park transport.
//   4. The worker then re-classifies MW-S2 (read still gated) and parks in
//      the backend domain; releasing the gate completes the read, the park
//      wakes on real progress, the Completion is reaped, and the run
//      converges.
// Verdict: fiber resumed exactly once with the Event winner; the run
// returned; the Completion ready via a real reap. A participant that slept
// through the pre-arm wake would stall the blocking handshakes and the case
// watchdog fires (rc 70).
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_case_a_notify_before_arm) {
    if constexpr (!sa::fiber_ctx::supported) return;
    PGFixture f;
    TempFile tmp;
    std::byte buf[1]{};
    sa::Completion<std::size_t> c;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{tmp.fd, buf, 1, 0}, c).has_value());

    SelectWaiter waiter(f.sched);
    waiter.spawn_on_worker0(f.sched);
    stest::MwAdmissionSeam::arm(f.sched);

    CloseoutProbe probe;
    probe.name = "case-a-notify-before-arm";
    probe.fx = &f;
    probe.bind_gate(f.gate.paused, f.gate.resume, f.gate.exited);
    CloseoutWatchdog wd(kWatchdogSeconds, probe);

    probe.set_phase("run");
    RunDriver driver(f.sched);
    driver.start(1);

    // Blocking seam observation (zero-CPU controller-cv handshake): the
    // worker is held at the MW-S2 Phase-B commit boundary, gate false, no
    // commit. A construction failure (seam never paused) is bounded by the
    // case watchdog, never by a correctness deadline.
    probe.set_phase("seam-pause");
    stest::MwAdmissionSeam::wait_paused(f.sched);

    // The bridge skips: the gate is false (no commit), so ev.set()'s signal
    // must NOT bump the control epoch.
    probe.set_phase("publish-pre-arm");
    const auto control_before = f.raw->wait_source()->snapshot();
    waiter.ev.set();
    const auto control_after = f.raw->wait_source()->snapshot();
    SLUICE_CHECK_MSG(control_after.control_generation ==
                         control_before.control_generation,
                     "Case A: bridge must skip before the arm (gate false)");

    stest::MwAdmissionSeam::release(f.sched);

    // The re-drain consumes the wake: the fiber runs exactly once.
    probe.set_phase("observe-fiber-resumed");
    wait_count_at_least(waiter.resumed, 1);
    // The worker re-classifies MW-S2 (read still gated) and parks unbounded
    // in the backend domain; only the gate release can complete the read.
    probe.set_phase("observe-repark");
    wait_count_at_least(f.prepark_entries, 1);
    sa::resume_threadpool_gate(f.gate);

    probe.set_phase("join");
    driver.join();
    probe.set_phase("verify");

    SLUICE_CHECK_MSG(waiter.resumed.load(std::memory_order_acquire) == 1,
                     "fiber resumed exactly once");
    SLUICE_CHECK_MSG(waiter.captured.has_winner(), "select produced a winner");
    SLUICE_CHECK_MSG(waiter.captured.kind() == SelectKind::event,
                     "Event winner");
    SLUICE_CHECK_MSG(waiter.fb.state() == FiberState::done, "fiber done");
    SLUICE_CHECK_MSG(c.ready(), "Completion ready via real reap");
    c.reset();
}

// ===========================================================================
// Case B (TP-G3 "arm→wait") — external notify lands after the commit's
// arm+gate, before wait_one() entry. D4-RM14: the armed control floor must
// make the first wait observe the interrupt (pre-park return) instead of
// rebaselining the bump into the entry snapshot.
//
//   1. The participant pauses at the mw_s2_committed_before_wait_one seam —
//      after arm_backend_wait_commit() + gate store, before wait_one().
//   2. SchedulerWakeHandle::notify() from the test thread: the gate is TRUE,
//      so the bridge fires. OBSERVE the control bump (deterministic bridge
//      evidence — mutation M1 never bumps and the case fails below).
//   3. Release. The first wait_for_change enters (prepark 1) and its
//      predicate fires on the armed floor WITHOUT consuming a real park;
//      the participant re-parks (prepark 2). A rebaselining implementation
//      parks through the interrupt and prepark never reaches 2 (M2).
//   4. ev.set() resolves the select through the SAME bridge; releasing the
//      gate completes the read; the run converges.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_case_b_notify_after_arm_before_wait) {
    if constexpr (!sa::fiber_ctx::supported) return;
    PGFixture f;
    TempFile tmp;
    std::byte buf[1]{};
    sa::Completion<std::size_t> c;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{tmp.fd, buf, 1, 0}, c).has_value());

    SelectWaiter waiter(f.sched);
    waiter.spawn_on_worker0(f.sched);
    stest::arm(f.sched, stest::PhaseTag::mw_s2_committed_before_wait_one);

    CloseoutProbe probe;
    probe.name = "case-b-notify-after-arm-before-wait";
    probe.fx = &f;
    probe.bind_gate(f.gate.paused, f.gate.resume, f.gate.exited);
    CloseoutWatchdog wd(kWatchdogSeconds, probe);

    probe.set_phase("run");
    RunDriver driver(f.sched);
    driver.start(1);

    probe.set_phase("seam-pause");
    stest::wait_paused(f.sched, stest::PhaseTag::mw_s2_committed_before_wait_one);

    // The bridge fires: gate==true at the notify, so the control epoch
    // advances BEFORE wait_one() ever runs.
    probe.set_phase("publish-after-commit");
    const auto token_before = f.raw->wait_source()->snapshot();
    f.wh.notify();
    wait_token(f, &sa::BackendWaitToken::control_generation,
               token_before.control_generation);

    stest::release(f.sched, stest::PhaseTag::mw_s2_committed_before_wait_one);

    // The armed floor releases the first wait; the participant re-parks.
    probe.set_phase("observe-repark");
    wait_count_at_least(f.prepark_entries, 2);

    // A second external publication through the same bridge resolves the
    // select; then real progress completes the scenario.
    probe.set_phase("resolve-select");
    waiter.ev.set();
    wait_count_at_least(waiter.resumed, 1);
    sa::resume_threadpool_gate(f.gate);

    probe.set_phase("join");
    driver.join();
    probe.set_phase("verify");

    SLUICE_CHECK_MSG(waiter.resumed.load(std::memory_order_acquire) == 1,
                     "fiber resumed exactly once");
    SLUICE_CHECK_MSG(waiter.fb.state() == FiberState::done, "fiber done");
    SLUICE_CHECK_MSG(c.ready(), "Completion ready via real reap");
    c.reset();
}

// ===========================================================================
// Case C (TP-G3 "physically parked") — external notify while the participant
// is at/inside the backend park. The ReadyWaitSource predicate closes every
// sub-state (pre-block, blocked, between): the control bump + notify_all
// release the park; the epoch guarantees no lost wake regardless of where
// the participant was inside wait_for_change.
//
//   1. Observe the park-entry marker (wait_phase_entered — set inside
//      wait_for_change, after the snapshot, at the cv park) and the Backend
//      park domain (the commit marker cleared only when wait_one returns).
//   2. notify() from the test thread → bridge (gate true) → control bump.
//   3. The interrupted wait returns; with the select wait still registered
//      the run stays Live and the participant re-parks (prepark 2).
//   4. ev.set() through the same bridge; gate release completes the read;
//      the run converges.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_case_c_notify_while_parked) {
    if constexpr (!sa::fiber_ctx::supported) return;
    PGFixture f;
    TempFile tmp;
    std::byte buf[1]{};
    sa::Completion<std::size_t> c;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{tmp.fd, buf, 1, 0}, c).has_value());

    SelectWaiter waiter(f.sched);
    waiter.spawn_on_worker0(f.sched);

    CloseoutProbe probe;
    probe.name = "case-c-notify-while-parked";
    probe.fx = &f;
    probe.bind_gate(f.gate.paused, f.gate.resume, f.gate.exited);
    CloseoutWatchdog wd(kWatchdogSeconds, probe);

    probe.set_phase("run");
    RunDriver driver(f.sched);
    driver.start(1);

    probe.set_phase("observe-park");
    wait_flag(f.wait_phase_entered);
    if (AsyncTestAccess::worker_park_domain(f.sched, 0) !=
        sa::WorkerState::ParkDomain::Backend) {
        fail_closed(f.sched, "case-c-wrong-domain", "park domain not Backend");
    }

    const auto token_before = f.raw->wait_source()->snapshot();
    f.wh.notify();
    wait_token(f, &sa::BackendWaitToken::control_generation,
               token_before.control_generation);
    // The wake is consumed: the interrupted wait returns and the Live run
    // re-parks (external-wake-possible select wait still registered).
    probe.set_phase("observe-repark");
    wait_count_at_least(f.prepark_entries, 2);

    probe.set_phase("resolve-select");
    waiter.ev.set();
    wait_count_at_least(waiter.resumed, 1);
    sa::resume_threadpool_gate(f.gate);

    probe.set_phase("join");
    driver.join();
    probe.set_phase("verify");

    SLUICE_CHECK_MSG(waiter.resumed.load(std::memory_order_acquire) == 1,
                     "fiber resumed exactly once");
    SLUICE_CHECK_MSG(waiter.fb.state() == FiberState::done, "fiber done");
    SLUICE_CHECK_MSG(c.ready(), "Completion ready via real reap");
    c.reset();
}

// ===========================================================================
// Case D / TP-G4 — backend terminal racing the control interrupt, both
// orders. No fabricated Completion, no lost readiness, no lost wake, no
// double route; exactly-once reap through the interrupt-vs-final-poll
// closure (wait_one's interrupted path performs one final poll whose count
// is returned — the control interrupt can never swallow the last ready).
//
//   Order 1 (progress → notify): the terminal publishes first (progress
//   epoch observed advanced); the notify then races the participant's wake
//   classification; whichever reason wins, the reap count is consumed.
//   Order 2 (notify → progress): the interrupt wins the reason; the final
//   poll reaps the subsequent terminal; the Live run re-parks and the later
//   progress is consumed by that park.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_case_d_ready_vs_notify_race) {
    if constexpr (!sa::fiber_ctx::supported) return;
    // Order 1 (progress → notify), deterministic via the context's
    // wait-source progress seam: the participant wakes on progress and is
    // held AFTER the progress report, BEFORE the reaping poll — the notify
    // then lands mid-invocation (its control baseline predates the bump),
    // and the resumed iteration must still return the reaped terminal
    // (RM13: the bump can neither swallow the reap nor be rebaselined into
    // a forever park).
    {
        PGFixture f;
        sa::AsyncIoContext::WaitSourceProgressPauseGate pgate;
        TempFile tmp;
        std::byte buf[1]{};
        sa::Completion<std::size_t> c;
        SLUICE_CHECK(
            f.ctx.submit_read(sa::ReadOp{tmp.fd, buf, 1, 0}, c).has_value());

        SelectWaiter waiter(f.sched);
        waiter.spawn_on_worker0(f.sched);

        CloseoutProbe probe;
        probe.name = "case-d1-ready-vs-notify-progress-first";
        probe.fx = &f;
        probe.bind_gate(f.gate.paused, f.gate.resume, f.gate.exited);
        CloseoutWatchdog wd(kWatchdogSeconds, probe);

        probe.set_phase("run");
        RunDriver driver(f.sched);
        driver.start(1);

        probe.set_phase("observe-park");
        wait_flag(f.wait_phase_entered);
        f.ctx.set_wait_source_progress_pause_gate_for_test(&pgate);
        // Baseline BEFORE the gate release: the release is the ONLY trigger
        // for the terminal publication, so the advance is strictly after
        // this baseline (the old code read the baseline AFTER the release —
        // the same baseline-inversion race as TP-G5, issue #123).
        const auto progress_before = f.raw->wait_source()->snapshot();
        sa::resume_threadpool_gate(f.gate);
        wait_token(f, &sa::BackendWaitToken::progress_generation,
                   progress_before.progress_generation);
        // The participant is held AFTER the progress report, BEFORE the
        // reaping poll (the seam's paused flag is published with notify —
        // blocking observation).
        wait_flag(pgate.paused);
        const auto control_before = f.raw->wait_source()->snapshot();
        f.wh.notify();
        wait_token(f, &sa::BackendWaitToken::control_generation,
                   control_before.control_generation);
        sa::AsyncIoContext::resume_wait_source_progress_gate_for_test(pgate);

        probe.set_phase("resolve-select");
        waiter.ev.set();
        wait_count_at_least(waiter.resumed, 1);
        probe.set_phase("join");
        driver.join();
        probe.set_phase("verify");
        SLUICE_CHECK_MSG(c.ready(),
                         "D1: terminal consumed exactly once despite the race");
        SLUICE_CHECK_MSG(f.raw->backend_ready_count_for_test() == 0,
                         "D1: no unreaped backend-ready residue");
        f.ctx.set_wait_source_progress_pause_gate_for_test(nullptr);
        c.reset();
    }
    // Order 2: notify first, progress second.
    {
        PGFixture f;
        TempFile tmp;
        std::byte buf[1]{};
        sa::Completion<std::size_t> c;
        SLUICE_CHECK(
            f.ctx.submit_read(sa::ReadOp{tmp.fd, buf, 1, 0}, c).has_value());

        SelectWaiter waiter(f.sched);
        waiter.spawn_on_worker0(f.sched);

        CloseoutProbe probe;
        probe.name = "case-d2-ready-vs-notify-notify-first";
        probe.fx = &f;
        probe.bind_gate(f.gate.paused, f.gate.resume, f.gate.exited);
        CloseoutWatchdog wd(kWatchdogSeconds, probe);

        probe.set_phase("run");
        RunDriver driver(f.sched);
        driver.start(1);

        probe.set_phase("observe-park");
        wait_flag(f.wait_phase_entered);
        const auto control_before = f.raw->wait_source()->snapshot();
        f.wh.notify();
        wait_token(f, &sa::BackendWaitToken::control_generation,
                   control_before.control_generation);
        wait_count_at_least(f.prepark_entries, 2);
        // The interrupt returned 0 (gated read: the final poll reaps
        // nothing); the Live run re-parked. NOW the terminal publishes —
        // the re-park must consume it (no lost readiness). Baseline BEFORE
        // the release (same discipline as D1/TP-G5).
        const auto progress_before = f.raw->wait_source()->snapshot();
        sa::resume_threadpool_gate(f.gate);
        wait_token(f, &sa::BackendWaitToken::progress_generation,
                   progress_before.progress_generation);

        probe.set_phase("resolve-select");
        waiter.ev.set();
        wait_count_at_least(waiter.resumed, 1);
        probe.set_phase("join");
        driver.join();
        probe.set_phase("verify");
        SLUICE_CHECK_MSG(c.ready(),
                         "D2: re-parked participant consumed the terminal");
        SLUICE_CHECK_MSG(f.raw->backend_ready_count_for_test() == 0,
                         "D2: no unreaped backend-ready residue");
        c.reset();
    }
}

// ===========================================================================
// TP-G1 — backend-ready between the park snapshot and the physical cv park.
// The wait_phase marker IS that window (set inside wait_for_change after
// the token snapshot, before/at the cv block). A terminal published in the
// window advances the progress epoch past the observed token, so the
// predicate releases the park (or never blocks) and wait_one's next poll
// reaps it — the park cannot swallow readiness published after its snapshot.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_tp_g1_ready_between_snapshot_and_park) {
    if constexpr (!sa::fiber_ctx::supported) return;
    PGFixture f;
    TempFile tmp;
    std::byte buf[1]{};
    sa::Completion<std::size_t> c;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{tmp.fd, buf, 1, 0}, c).has_value());

    CloseoutProbe probe;
    probe.name = "tp-g1-ready-between-snapshot-and-park";
    probe.fx = &f;
    probe.bind_gate(f.gate.paused, f.gate.resume, f.gate.exited);
    CloseoutWatchdog wd(kWatchdogSeconds, probe);

    probe.set_phase("run");
    RunDriver driver(f.sched);
    driver.start(1);

    // No fiber: pure backend-only MW-S2, unbounded park.
    probe.set_phase("observe-park");
    wait_flag(f.wait_phase_entered);
    if (AsyncTestAccess::worker_park_domain(f.sched, 0) !=
        sa::WorkerState::ParkDomain::Backend) {
        fail_closed(f.sched, "tp-g1-wrong-domain", "park domain not Backend");
    }
    // The terminal lands INSIDE the snapshot→park window. Baseline BEFORE
    // the release (the publication is strictly after it).
    const auto progress_before = f.raw->wait_source()->snapshot();
    sa::resume_threadpool_gate(f.gate);
    wait_token(f, &sa::BackendWaitToken::progress_generation,
               progress_before.progress_generation);

    probe.set_phase("join");
    driver.join();
    probe.set_phase("verify");

    SLUICE_CHECK_MSG(c.ready(), "TP-G1: in-window readiness consumed");
    SLUICE_CHECK_MSG(f.raw->backend_ready_count_for_test() == 0,
                     "TP-G1: no unreaped residue");
    c.reset();
}

// ===========================================================================
// TP-G2 — multiple backend-ready publications coalesce into one consumed
// wake: three gated reads complete (epoch advances per terminal, coalescing
// allowed), the participant's park wakes once and its reap loop drains ALL
// ready terminals; every Completion is ready exactly once with no residue.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_tp_g2_multi_ready_coalesce) {
    if constexpr (!sa::fiber_ctx::supported) return;
    PGFixture f;
    TempFile tmp;
    std::byte buf[3]{};
    sa::Completion<std::size_t> c1, c2, c3;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{tmp.fd, &buf[0], 1, 0}, c1).has_value());
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{tmp.fd, &buf[1], 1, 1}, c2).has_value());
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{tmp.fd, &buf[2], 1, 2}, c3).has_value());

    CloseoutProbe probe;
    probe.name = "tp-g2-multi-ready-coalesce";
    probe.fx = &f;
    probe.bind_gate(f.gate.paused, f.gate.resume, f.gate.exited);
    CloseoutWatchdog wd(kWatchdogSeconds, probe);

    probe.set_phase("run");
    RunDriver driver(f.sched);
    driver.start(1);

    probe.set_phase("observe-park");
    wait_flag(f.wait_phase_entered);
    sa::resume_threadpool_gate(f.gate);

    probe.set_phase("join");
    driver.join();
    probe.set_phase("verify");

    SLUICE_CHECK_MSG(c1.ready() && c2.ready() && c3.ready(),
                     "TP-G2: every coalesced terminal produced its Completion");
    SLUICE_CHECK_MSG(f.raw->backend_ready_count_for_test() == 0,
                     "TP-G2: no unreaped residue");
    SLUICE_CHECK_MSG(f.raw->outstanding() == 0, "TP-G2: outstanding drained");
    c1.reset();
    c2.reset();
    c3.reset();
}

// ===========================================================================
// TP-G5 — close_admission while the participant is parked. The backend's
// close_admission interrupts the wait source (control epoch); the
// interrupted no-progress return is the documented MW-S2 no-progress
// boundary for a backend-only park, so the run terminates and run_live
// RETURNS (no implicit drain). The gated read then completes with no
// observer; the caller's re-entry (E4/E5 model) must reap it exactly once
// and converge.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_tp_g5_close_admission_while_parked) {
    if constexpr (!sa::fiber_ctx::supported) return;
    PGFixture f;
    TempFile tmp;
    std::byte buf[1]{};
    sa::Completion<std::size_t> c;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{tmp.fd, buf, 1, 0}, c).has_value());

    CloseoutProbe probe;
    probe.name = "tp-g5-close-admission-while-parked";
    probe.fx = &f;
    probe.bind_gate(f.gate.paused, f.gate.resume, f.gate.exited);
    CloseoutWatchdog wd(kWatchdogSeconds, probe);

    probe.set_phase("run");
    RunDriver driver(f.sched);
    driver.start(1);

    probe.set_phase("observe-park");
    wait_flag(f.wait_phase_entered);
    f.raw->close_admission();

    // The interrupted no-progress park terminates the run (no select wait —
    // external wake not possible). A parked-forever run is the failure
    // (bounded by the case watchdog).
    probe.set_phase("join");
    driver.join();

    // The read completes with no observer; the caller re-enters and the
    // first poll must reap the terminal. Baseline BEFORE the gate release:
    // the release is the ONLY trigger for the terminal publication, so the
    // advance is strictly after this baseline. (The old code read the
    // baseline AFTER the release — a publication landing in between made
    // the observation wait for a second, never-coming advance: the
    // reproduced TP-G5 flake under CPU contention, issue #123.)
    probe.set_phase("observe-terminal-published");
    const auto progress_before = f.raw->wait_source()->snapshot();
    sa::resume_threadpool_gate(f.gate);
    wait_token(f, &sa::BackendWaitToken::progress_generation,
               progress_before.progress_generation);

    probe.set_phase("reentry");
    RunDriver driver2(f.sched);
    driver2.start(1);
    driver2.join();
    probe.set_phase("verify");

    SLUICE_CHECK_MSG(c.ready(), "TP-G5: re-entry reaped the terminal exactly once");
    SLUICE_CHECK_MSG(f.raw->backend_ready_count_for_test() == 0,
                     "TP-G5: no unreaped residue");
    c.reset();
}

// ===========================================================================
// TP-G6 — the bridge transports a wake WITHOUT fabricating backend
// readiness: repeated notifies while the read is gated must never publish a
// Completion, never fabricate backend-ready, never reap (outstanding
// unchanged), never route the fiber — and the participant must stay
// resident (Live re-park per interrupt, then quiet: one-shot control epoch,
// no busy-spin). The real reap happens only after the gate release.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_tp_g6_bridge_no_fabrication) {
    if constexpr (!sa::fiber_ctx::supported) return;
    PGFixture f;
    TempFile tmp;
    std::byte buf[1]{};
    sa::Completion<std::size_t> c;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{tmp.fd, buf, 1, 0}, c).has_value());

    SelectWaiter waiter(f.sched);
    waiter.spawn_on_worker0(f.sched);

    CloseoutProbe probe;
    probe.name = "tp-g6-bridge-no-fabrication";
    probe.fx = &f;
    probe.bind_gate(f.gate.paused, f.gate.resume, f.gate.exited);
    CloseoutWatchdog wd(kWatchdogSeconds, probe);

    probe.set_phase("run");
    RunDriver driver(f.sched);
    driver.start(1);

    probe.set_phase("observe-park");
    wait_flag(f.wait_phase_entered);
    constexpr int kNotifies = 3;
    for (int i = 0; i < kNotifies; ++i) {
        probe.set_phase("bridge-notify");
        const auto token_before = f.raw->wait_source()->snapshot();
        f.wh.notify();
        wait_token(f, &sa::BackendWaitToken::control_generation,
                   token_before.control_generation);
        // One interrupt = one wake = one re-park (one-shot epoch). The chain
        // of observations orders every interrupt before the next notify.
        probe.set_phase("observe-repark");
        wait_count_at_least(f.prepark_entries, 1 + i + 1);
        SLUICE_CHECK_MSG(!c.ready(), "TP-G6: no fabricated Completion");
        SLUICE_CHECK_MSG(f.raw->backend_ready_count_for_test() == 0,
                         "TP-G6: no fabricated backend-ready");
        SLUICE_CHECK_MSG(f.raw->outstanding() == 1,
                         "TP-G6: no phantom reap while gated");
        SLUICE_CHECK_MSG(waiter.resumed.load(std::memory_order_acquire) == 0,
                         "TP-G6: bridge must not route the fiber");
    }

    // The real progress path is the ONLY Completion publisher.
    probe.set_phase("resolve-select");
    waiter.ev.set();
    wait_count_at_least(waiter.resumed, 1);
    sa::resume_threadpool_gate(f.gate);
    probe.set_phase("join");
    driver.join();
    probe.set_phase("verify");

    SLUICE_CHECK_MSG(c.ready(), "TP-G6: Completion ready via real reap only");
    SLUICE_CHECK_MSG(f.raw->backend_ready_count_for_test() == 0,
                     "TP-G6: no unreaped residue");
    c.reset();
}

// ===========================================================================
// TP-G7 — the accepted-terminal wake path adds no allocation dependency:
// the bridge/interrupt entry points are noexcept by contract (no throwing
// allocation may sit on the wake path), and the end-to-end terminal wake
// (park → progress publication → wake → reap → convergence) completes on
// the noexcept transports alone.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_closeout_tp_g7_wake_noalloc_probe) {
    static_assert(
        noexcept(std::declval<sa::AsyncIoContext&>().interrupt_backend_waiters()),
        "the bridge transport must be noexcept (no allocation dependency)");
    static_assert(
        noexcept(std::declval<sa::AsyncIoContext&>().arm_backend_wait_commit()),
        "the commit-to-park registration must be noexcept");

    if constexpr (!sa::fiber_ctx::supported) return;
    PGFixture f;
    TempFile tmp;
    std::byte buf[1]{};
    sa::Completion<std::size_t> c;
    SLUICE_CHECK(f.ctx.submit_read(sa::ReadOp{tmp.fd, buf, 1, 0}, c).has_value());

    CloseoutProbe probe;
    probe.name = "tp-g7-wake-noalloc-probe";
    probe.fx = &f;
    probe.bind_gate(f.gate.paused, f.gate.resume, f.gate.exited);
    CloseoutWatchdog wd(kWatchdogSeconds, probe);

    probe.set_phase("run");
    RunDriver driver(f.sched);
    driver.start(1);

    probe.set_phase("observe-park");
    wait_flag(f.wait_phase_entered);
    sa::resume_threadpool_gate(f.gate);

    probe.set_phase("join");
    driver.join();
    probe.set_phase("verify");

    SLUICE_CHECK_MSG(c.ready(), "TP-G7: terminal wake path converged");
    c.reset();
}

SLUICE_MAIN()
