// issue116_interrupt_reevaluation_regression_test — deterministic merge-gate
// regression for the invocation-boundary lost re-entry liveness defect
// (Issue #116; docs/investigations/issue-116-runtime-reentry-liveness.md).
//
// Failing interleaving (proven on master ff003fd by in-process diagnostic
// trace + gdb state dump of a live-hung process):
//
//   driver : run_live invocation enters MW-S2 with one accepted, gated op
//            (outstanding == 1), the task Fiber awaits, and the elected
//            participant parks in ctx_.wait_one() (unbounded — no deadline,
//            no external-wake wait).
//   tester : an external Scheduler wake publication (wake handle notify)
//            carries NO ApplicationRuntime control_epoch_ advance. The
//            signal_wake_locked bridge interrupts the parked participant.
//   driver : wait_one returns 0 (interrupted; final poll empty — the op is
//            still mid-flight). The MW-S2 no-progress boundary terminates
//            the invocation (TP-G5 caller-owned re-entry contract).
//   PRE-FIX: ApplicationRuntime::driver_main parks on runtime_cv_ because
//            control_epoch_ == observed_epoch_ — the Runtime's re-entry
//            signal cannot observe the transferred obligation. Releasing the
//            op afterwards publishes a terminal into a ready-ring with no
//            observer: the task never resumes, done is never published.
//            (Deterministic 1/1 hang; watchdog exits 70 with forensics.)
//   POST-FIX: the driver re-enters run_live immediately while
//            io_ctx_->outstanding() > 0, re-parks as the MW-S2 participant,
//            and the released op's progress wakes it: the task completes and
//            the Runtime tears down cleanly.
//
// Determinism policy: NO sleep_for as ordering proof. The startup ordering
// (the ~7.5% CI race) is forced by holding the first, empty run_live at the
// run_impl tail seam until it has provably returned; the mid-flight op is
// frozen by the backend running-pause gate; the participant park is proven
// by the wait-source park-entry latch. All three are persistent-state
// observations, and the parked wait_one is UNBOUNDED, so the participant
// provably remains parked until the notify. Bounded waits appear solely as
// hang watchdogs.
//
// Gated to fiber-capable targets (fiber_ctx::supported), matching the rest
// of the async internal-testing suite.
#include <sluice/async/application_runtime.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/op_helpers.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include "async_test_control.hpp"
#include "harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace sa = sluice::async;

namespace {

constexpr auto kObserveWait = std::chrono::seconds(10);

bool wait_flag(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + kObserveWait;
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// One-byte temp file; the gated read on it stays mid-flight until
// resume_threadpool_gate.
struct TempFile {
    int fd = -1;
    TempFile() {
        fd = ::open("/tmp/sluice_issue116_regression.tmp",
                    O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            std::fprintf(stderr, "issue116: temp open failed\n");
            std::exit(1);
        }
        const std::byte seed[1] = {std::byte{0x71}};
        if (::pwrite(fd, seed, 1, 0) != 1) {
            std::fprintf(stderr, "issue116: temp seed failed\n");
            std::exit(1);
        }
    }
    ~TempFile() { ::close(fd); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

}  // namespace

SLUICE_TEST_CASE(issue116_interrupt_is_reevaluation_not_quiescence) {
    if constexpr (!sa::fiber_ctx::supported) return;

    // Fixture: a split-wait backend whose FIRST accepted read freezes in the
    // running state (syscall not executed) until the test resumes it, plus
    // the wait-source park-entry latch (fires at wait_for_change entry, i.e.
    // the MW-S2 participant is committing to the backend-domain park).
    sa::ThreadPoolBackend::WorkerRunningPauseGate gate;
    std::atomic<bool> wait_phase_entered{false};
    auto backend = std::make_unique<sa::ThreadPoolBackend>(
        sa::ThreadPoolConfig{/*request_capacity=*/4, /*worker_count=*/2});
    sa::ThreadPoolBackend* raw = backend.get();
    raw->set_running_pause_gate(&gate);
    raw->set_wait_phase_flag_for_test(&wait_phase_entered);
    TempFile tmp;

    sa::RuntimeBuilder builder;
    builder.backend(std::move(backend));
    builder.workers(1);  // the captured hang configuration: single worker

    auto build_r = builder.build();
    SLUICE_CHECK_MSG(build_r.has_value(), "issue116: runtime build failed");
    auto rt = std::move(build_r.value());

    // Force the fatal startup ordering deterministically (the ~7.5% CI race
    // condensed to one schedule). The defect needs the driver to have
    // CONSUMED the submit's control-epoch delta before the invocation that
    // parks dies — i.e. the first (empty) run_live must RETURN before the
    // task is submitted. Hold that first invocation at the run_impl tail
    // seam (all workers joined, run provably terminated), then release and
    // submit: the driver's re-entry records observed_epoch_ = post-submit
    // control_epoch_ BEFORE the task's op parks, which is the exact state
    // the captured hang was found in (epoch == observed == 2).
    sluice_async_test::ControllerGuard controller(
        rt->test_scheduler_for_worker_topology());
    sluice_async_test::arm(
        rt->test_scheduler_for_worker_topology(),
        sluice_async_test::PhaseTag::worker_topology_joined_before_unpublish);
    SLUICE_CHECK_MSG(rt->start().has_value(), "issue116: start failed");
    sluice_async_test::wait_paused(
        rt->test_scheduler_for_worker_topology(),
        sluice_async_test::PhaseTag::worker_topology_joined_before_unpublish);
    sluice_async_test::release(
        rt->test_scheduler_for_worker_topology(),
        sluice_async_test::PhaseTag::worker_topology_joined_before_unpublish);

    // The external wake handle on the runtime's own Scheduler: notify() is a
    // Scheduler wake-domain publication with NO Runtime control_epoch_
    // advance — the exact component the #116 fatal interleaving used (the
    // submit-path wake whose epoch delta the driver had already consumed).
    sa::SchedulerWakeHandle wh =
        rt->test_scheduler_for_worker_topology().make_wake_handle();

    std::atomic<bool> done{false};
    auto sub_r = rt->submit([&](sa::RuntimeTaskContext& ctx) {
        std::byte buf[1]{};
        sa::Completion<std::size_t> c;
        auto sr = ctx.submit_read(sa::ReadOp{tmp.fd, buf, 1, 0}, c);
        if (!sr.has_value()) return;
        (void)ctx.await_completion(c);
        (void)c.result();
        c.reset();
        done.store(true, std::memory_order_release);
    });
    SLUICE_CHECK_MSG(sub_r.has_value(), "issue116: submit rejected");

    // Rendezvous 1: the accepted op is provably mid-flight (running, no
    // terminal) — `outstanding` is stable at 1 for the whole scenario.
    SLUICE_CHECK_MSG(wait_flag(gate.paused),
                     "issue116: gated op never reached the running state");
    // Rendezvous 2: the MW-S2 participant is provably inside the backend
    // park. The park is unbounded and no interrupt source exists yet, so the
    // participant REMAINS parked until the notify below: the interleaving is
    // forced, not sampled.
    SLUICE_CHECK_MSG(wait_flag(wait_phase_entered),
                     "issue116: participant never entered the backend park");

    // The fatal component: an external wake publication while the
    // participant is parked. The bridge interrupts wait_one; the MW-S2
    // no-progress boundary terminates the invocation and transfers the
    // observation obligation to the runtime driver (TP-G5 contract).
    wh.notify();

    // Release the op: the terminal is recorded and progress is signaled.
    // PRE-FIX: no observer remains — the driver is parked on runtime_cv_
    // (control_epoch_ == observed_epoch_) and `done` never fires. POST-FIX:
    // the re-entered invocation's participant wakes, reaps, delivers, and
    // the task resumes.
    sa::resume_threadpool_gate(gate);

    // The assertion: the task MUST complete. Bounded wait is the watchdog
    // escape hatch only; ordering is already fixed by the rendezvous above.
    if (!wait_flag(done)) {
        rt->test_dump_forensics("issue116-regression");
        std::fprintf(stderr,
                     "ISSUE116 FAIL-CLOSED: task never completed after the "
                     "interrupt (lost re-entry); pid=%d\n",
                     static_cast<int>(::getpid()));
        std::_Exit(70);
    }

    // Disarm the backend test seams while the backend is still alive:
    // rt->join() destroys it via close_resources, so disarming after the
    // teardown would dereference a freed ThreadPoolBackend (TSan proves).
    raw->set_running_pause_gate(nullptr);
    raw->set_wait_phase_flag_for_test(nullptr);

    // Clean teardown proves no stranded obligation remained: drain requires
    // all tasks terminal AND outstanding == 0; join destroys the backend
    // (whose destructor fail-fasts on outstanding != 0).
    rt->request_stop();
    SLUICE_CHECK_MSG(rt->drain().has_value(), "issue116: drain failed");
    SLUICE_CHECK_MSG(rt->join().has_value(), "issue116: join failed");
}

SLUICE_MAIN()
