// phase_g_closeout_test — Phase G final closeout: deterministic causal proofs
// for the commit→park wake protocol (Cases A–D) and the ThreadPool TP-G1..G7
// race matrix (docs/design/phase-g-backend-progress-wake.md §5).
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
// proof. Causal seams (PhaseTag pauses), one-way park-entry markers, and
// epoch observations only; bounded deadlines appear solely as hang
// watchdogs. Mutation detectors (closeout M1/M2) hang in these constructions
// and exit fail-closed (rc 70) via the watchdog.
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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
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

constexpr auto kObserveWait = std::chrono::seconds(5);
constexpr auto kJoinWatchdog = std::chrono::seconds(10);

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// Shared construction-violation exit. A stall in a Phase-G closeout case is
// the FAILURE verdict (the pre-fix/mutant manifestation); the scenario is
// deliberately not recovered — teardown with a parked worker would only mask
// the evidence (joinable-thread abort / shutdown hang).
[[noreturn]] void fail_closed(Scheduler& sched, const char* tag, const char* msg) {
    AsyncTestAccess::dump_park_forensics(sched, tag);
    std::fprintf(stderr, "PHASE-G-CLOSEOUT FAIL-CLOSED: %s (%s); pid=%d\n",
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

// run_live driver with a watchdog join. A run that never returns IS the
// mutant/pre-fix verdict — fail closed rather than wedging the harness.
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

// A one-byte seeded temp file; the gated read on it never completes until
// resume_threadpool_gate.
struct TempFile {
    int fd = -1;
    TempFile() {
        fd = ::open(templ(), O_RDWR | O_CREAT | O_TRUNC, 0644);
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
    ~TempFile() { ::close(fd); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

  private:
    static const char* templ() { return "/tmp/sluice_phase_g_closeout.tmp"; }
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
// through the pre-arm wake would hang at the watchdog (rc 70).
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

    RunDriver driver(f.sched);
    driver.start(1);

    // Bounded pause observation (the facade's wait_paused blocks on the
    // controller cv; a construction failure must not wedge the harness).
    {
        const auto deadline = std::chrono::steady_clock::now() + kObserveWait;
        while (!stest::is_paused(f.sched, stest::PhaseTag::mw_admission_phase_b)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                fail_closed(f.sched, "case-a-seam-never-paused",
                            "MW-S2 Phase-B seam never paused");
            }
            std::this_thread::yield();
        }
    }

    // The bridge skips: the gate is false (no commit), so ev.set()'s signal
    // must NOT bump the control epoch.
    const auto control_before =
        AsyncTestAccess::backend_wait_token(f.sched).control_generation;
    waiter.ev.set();
    const auto control_after =
        AsyncTestAccess::backend_wait_token(f.sched).control_generation;
    SLUICE_CHECK_MSG(control_after == control_before,
                     "Case A: bridge must skip before the arm (gate false)");

    stest::MwAdmissionSeam::release(f.sched);

    // The re-drain consumes the wake: the fiber runs exactly once.
    if (!wait_count_at_least(waiter.resumed, 1)) {
        fail_closed(f.sched, "case-a-fiber-never-resumed",
                    "re-drain never consumed the pre-arm wake");
    }
    // The worker re-classifies MW-S2 (read still gated) and parks unbounded
    // in the backend domain; only the gate release can complete the read.
    if (!wait_count_at_least(f.prepark_entries, 1)) {
        fail_closed(f.sched, "case-a-never-reparked",
                    "participant never entered the backend park");
    }
    sa::resume_threadpool_gate(f.gate);

    driver.join_or_fail("case-a-run-never-returned");

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

    RunDriver driver(f.sched);
    driver.start(1);

    {
        const auto deadline = std::chrono::steady_clock::now() + kObserveWait;
        while (!stest::is_paused(f.sched,
                                 stest::PhaseTag::mw_s2_committed_before_wait_one)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                fail_closed(f.sched, "case-b-seam-never-paused",
                            "committed seam never paused");
            }
            std::this_thread::yield();
        }
    }

    // The bridge fires: gate==true at the notify, so the control epoch
    // advances BEFORE wait_one() ever runs.
    std::uint64_t control = AsyncTestAccess::backend_wait_token(f.sched).control_generation;
    f.wh.notify();
    if (!wait_token(f.sched, control, &sa::BackendWaitToken::control_generation)) {
        fail_closed(f.sched, "case-b-bridge-never-fired",
                    "notify did not bump the control epoch (bridge disabled?)");
    }

    stest::release(f.sched, stest::PhaseTag::mw_s2_committed_before_wait_one);

    // The armed floor releases the first wait; the participant re-parks.
    if (!wait_count_at_least(f.prepark_entries, 2)) {
        fail_closed(f.sched, "case-b-parked-through-interrupt",
                    "first wait parked through the armed-floor interrupt "
                    "(D4-RM14 violation / mutation M2)");
    }

    // A second external publication through the same bridge resolves the
    // select; then real progress completes the scenario.
    waiter.ev.set();
    if (!wait_count_at_least(waiter.resumed, 1)) {
        fail_closed(f.sched, "case-b-fiber-never-resumed",
                    "select never resolved after the bridge wake");
    }
    sa::resume_threadpool_gate(f.gate);

    driver.join_or_fail("case-b-run-never-returned");

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

    RunDriver driver(f.sched);
    driver.start(1);

    if (!wait_flag(f.wait_phase_entered)) {
        fail_closed(f.sched, "case-c-never-parked",
                    "participant never entered the backend park");
    }
    if (AsyncTestAccess::worker_park_domain(f.sched, 0) !=
        sa::WorkerState::ParkDomain::Backend) {
        fail_closed(f.sched, "case-c-wrong-domain", "park domain not Backend");
    }

    std::uint64_t control = AsyncTestAccess::backend_wait_token(f.sched).control_generation;
    f.wh.notify();
    if (!wait_token(f.sched, control, &sa::BackendWaitToken::control_generation)) {
        fail_closed(f.sched, "case-c-bridge-never-fired",
                    "parked notify did not bump the control epoch");
    }
    // The wake is consumed: the interrupted wait returns and the Live run
    // re-parks (external-wake-possible select wait still registered).
    if (!wait_count_at_least(f.prepark_entries, 2)) {
        fail_closed(f.sched, "case-c-lost-parked-wake",
                    "parked participant never consumed the bridge wake");
    }

    waiter.ev.set();
    if (!wait_count_at_least(waiter.resumed, 1)) {
        fail_closed(f.sched, "case-c-fiber-never-resumed",
                    "select never resolved after the bridge wake");
    }
    sa::resume_threadpool_gate(f.gate);

    driver.join_or_fail("case-c-run-never-returned");

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

        RunDriver driver(f.sched);
        driver.start(1);

        if (!wait_flag(f.wait_phase_entered)) {
            fail_closed(f.sched, "case-d1-never-parked",
                        "participant never entered the backend park");
        }
        f.ctx.set_wait_source_progress_pause_gate_for_test(&pgate);
        std::uint64_t progress =
            AsyncTestAccess::backend_wait_token(f.sched).progress_generation;
        sa::resume_threadpool_gate(f.gate);
        if (!wait_token(f.sched, progress,
                        &sa::BackendWaitToken::progress_generation)) {
            fail_closed(f.sched, "case-d1-ready-never-published",
                        "backend terminal never published progress");
        }
        if (!wait_flag(pgate.paused)) {
            fail_closed(f.sched, "case-d1-progress-seam-never-paused",
                        "participant never reported the in-window progress");
        }
        std::uint64_t control =
            AsyncTestAccess::backend_wait_token(f.sched).control_generation;
        f.wh.notify();
        if (!wait_token(f.sched, control,
                        &sa::BackendWaitToken::control_generation)) {
            fail_closed(f.sched, "case-d1-bridge-never-fired",
                        "notify never bumped the control epoch");
        }
        pgate.resume.store(true, std::memory_order_release);

        waiter.ev.set();
        if (!wait_count_at_least(waiter.resumed, 1)) {
            fail_closed(f.sched, "case-d1-fiber-never-resumed",
                        "select never resolved");
        }
        driver.join_or_fail("case-d1-run-never-returned");
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

        RunDriver driver(f.sched);
        driver.start(1);

        if (!wait_flag(f.wait_phase_entered)) {
            fail_closed(f.sched, "case-d2-never-parked",
                        "participant never entered the backend park");
        }
        std::uint64_t control =
            AsyncTestAccess::backend_wait_token(f.sched).control_generation;
        f.wh.notify();
        if (!wait_token(f.sched, control,
                        &sa::BackendWaitToken::control_generation)) {
            fail_closed(f.sched, "case-d2-bridge-never-fired",
                        "notify never bumped the control epoch");
        }
        if (!wait_count_at_least(f.prepark_entries, 2)) {
            fail_closed(f.sched, "case-d2-lost-parked-wake",
                        "parked participant never consumed the bridge wake");
        }
        // The interrupt returned 0 (gated read: the final poll reaps
        // nothing); the Live run re-parked. NOW the terminal publishes —
        // the re-park must consume it (no lost readiness).
        std::uint64_t progress =
            AsyncTestAccess::backend_wait_token(f.sched).progress_generation;
        sa::resume_threadpool_gate(f.gate);
        if (!wait_token(f.sched, progress,
                        &sa::BackendWaitToken::progress_generation)) {
            fail_closed(f.sched, "case-d2-ready-never-published",
                        "backend terminal never published progress");
        }

        waiter.ev.set();
        if (!wait_count_at_least(waiter.resumed, 1)) {
            fail_closed(f.sched, "case-d2-fiber-never-resumed",
                        "select never resolved");
        }
        driver.join_or_fail("case-d2-run-never-returned");
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

    RunDriver driver(f.sched);
    driver.start(1);

    // No fiber: pure backend-only MW-S2, unbounded park.
    if (!wait_flag(f.wait_phase_entered)) {
        fail_closed(f.sched, "tp-g1-never-parked",
                    "participant never entered the park window");
    }
    if (AsyncTestAccess::worker_park_domain(f.sched, 0) !=
        sa::WorkerState::ParkDomain::Backend) {
        fail_closed(f.sched, "tp-g1-wrong-domain", "park domain not Backend");
    }
    // The terminal lands INSIDE the snapshot→park window.
    std::uint64_t progress =
        AsyncTestAccess::backend_wait_token(f.sched).progress_generation;
    sa::resume_threadpool_gate(f.gate);
    if (!wait_token(f.sched, progress, &sa::BackendWaitToken::progress_generation)) {
        fail_closed(f.sched, "tp-g1-ready-never-published",
                    "backend terminal never published progress");
    }

    driver.join_or_fail("tp-g1-run-never-returned");

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

    RunDriver driver(f.sched);
    driver.start(1);

    if (!wait_flag(f.wait_phase_entered)) {
        fail_closed(f.sched, "tp-g2-never-parked",
                    "participant never entered the backend park");
    }
    sa::resume_threadpool_gate(f.gate);

    driver.join_or_fail("tp-g2-run-never-returned");

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

    RunDriver driver(f.sched);
    driver.start(1);

    if (!wait_flag(f.wait_phase_entered)) {
        fail_closed(f.sched, "tp-g5-never-parked",
                    "participant never entered the backend park");
    }
    f.raw->close_admission();

    // The interrupted no-progress park terminates the run (no select wait —
    // external wake not possible). A parked-forever run is the failure.
    driver.join_or_fail("tp-g5-run-never-returned");

    // The read completes with no observer; the caller re-enters and the
    // first poll must reap the terminal.
    sa::resume_threadpool_gate(f.gate);
    {
        std::uint64_t progress =
            AsyncTestAccess::backend_wait_token(f.sched).progress_generation;
        const auto deadline = std::chrono::steady_clock::now() + kObserveWait;
        while (AsyncTestAccess::backend_wait_token(f.sched).progress_generation ==
               progress) {
            if (std::chrono::steady_clock::now() >= deadline) {
                fail_closed(f.sched, "tp-g5-ready-never-published",
                            "terminal never published after close");
            }
            std::this_thread::yield();
        }
    }

    RunDriver driver2(f.sched);
    driver2.start(1);
    driver2.join_or_fail("tp-g5-reentry-never-returned");

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

    RunDriver driver(f.sched);
    driver.start(1);

    if (!wait_flag(f.wait_phase_entered)) {
        fail_closed(f.sched, "tp-g6-never-parked",
                    "participant never entered the backend park");
    }
    constexpr int kNotifies = 3;
    for (int i = 0; i < kNotifies; ++i) {
        std::uint64_t control =
            AsyncTestAccess::backend_wait_token(f.sched).control_generation;
        f.wh.notify();
        if (!wait_token(f.sched, control,
                        &sa::BackendWaitToken::control_generation)) {
            fail_closed(f.sched, "tp-g6-bridge-never-fired",
                        "notify did not bump the control epoch");
        }
        // One interrupt = one wake = one re-park (one-shot epoch). The chain
        // of observations orders every interrupt before the next notify.
        if (!wait_count_at_least(f.prepark_entries, 1 + i + 1)) {
            fail_closed(f.sched, "tp-g6-lost-wake",
                        "interrupted participant never re-parked");
        }
        SLUICE_CHECK_MSG(!c.ready(), "TP-G6: no fabricated Completion");
        SLUICE_CHECK_MSG(f.raw->backend_ready_count_for_test() == 0,
                         "TP-G6: no fabricated backend-ready");
        SLUICE_CHECK_MSG(f.raw->outstanding() == 1,
                         "TP-G6: no phantom reap while gated");
        SLUICE_CHECK_MSG(waiter.resumed.load(std::memory_order_acquire) == 0,
                         "TP-G6: bridge must not route the fiber");
    }

    // The real progress path is the ONLY Completion publisher.
    waiter.ev.set();
    if (!wait_count_at_least(waiter.resumed, 1)) {
        fail_closed(f.sched, "tp-g6-fiber-never-resumed",
                    "select never resolved");
    }
    sa::resume_threadpool_gate(f.gate);
    driver.join_or_fail("tp-g6-run-never-returned");

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

    RunDriver driver(f.sched);
    driver.start(1);

    if (!wait_flag(f.wait_phase_entered)) {
        fail_closed(f.sched, "tp-g7-never-parked",
                    "participant never entered the backend park");
    }
    sa::resume_threadpool_gate(f.gate);

    driver.join_or_fail("tp-g7-run-never-returned");

    SLUICE_CHECK_MSG(c.ready(), "TP-G7: terminal wake path converged");
    c.reset();
}

SLUICE_MAIN()
