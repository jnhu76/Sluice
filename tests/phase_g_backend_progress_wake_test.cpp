// phase_g_backend_progress_wake_test — Phase G backend-ready progress wake
// integration (docs/design/phase-g-backend-progress-wake.md).
//
// Phase G replaces the E9 2ms bounded wake-domain observation with signal-
// driven progress: split-wait backends park the MW-S2 progress participant in
// ctx_.wait_one() (backend domain) with Scheduler wake publications bridged
// through interrupt_backend_waiters, and the wake-domain park is unbounded
// without an active deadline (no periodic wake, no CPU tax).
//
// Determinism policy (production-test-plan.md §1): NO sleep_for as
// correctness proof. Causal seams (PhaseTag pauses) + state observations only.
// Bounded deadlines appear solely as hang watchdogs.
//
// Gated to x86_64 (fiber_ctx::supported) for parity with the rest of E13.
#include <sluice/async/application_runtime.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/detail/select_registration.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/select.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include "async_test_control.hpp"
#include "harness.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace sa = sluice::async;
namespace sad = sluice::async::detail;
namespace stest = sluice_async_test;

using AsyncTestAccess = sa::Scheduler::AsyncTestAccess;
using Scheduler = sa::Scheduler;
using Event = sa::Event;
using SelectResult = sa::SelectResult;
using SelectKind = sa::SelectKind;
using EventSelectCase = sa::EventSelectCase;
using Fiber = sa::Fiber;
using FiberState = sa::FiberState;

namespace {

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

struct MWFixture {
    sa::AsyncIoContext ctx{std::make_unique<sa::FakeAsyncBackend>()};
    Scheduler sched;
    stest::ControllerGuard ctrl;
    MWFixture() : sched(ctx), ctrl(sched) {
        stest::TimerTestControl::enable_test_clock(sched);
    }
};

}  // namespace

// ===========================================================================
// Phase G regression — E9-LIFE-8 termination convergence with the unbounded
// wake-domain park (G0 finding during implementation verification).
//
// The Phase G park is unbounded when no deadline is active and no poll-driven
// backend observation is needed. The E9 2ms bounded timeout was LOAD-BEARING
// for termination convergence: it forced a Live-mw_s3 RESIDENT worker to
// re-check the run state periodically. Once the timeout is gone, the resident
// park's idle_workers_ reset can erase the last worker's termination count
// between its quiescent classify and its fetch_add; the not-last worker then
// parks SILENTLY, nobody re-checks, and a run whose final work already
// completed never terminates (deterministic hang: st16_multi_worker_owner_routing
// reproduced 1-in-14 under stress, guard-less unbounded park).
//
// The corrective: a worker that observes quiescence (or mw_s3) and is NOT the
// last idle worker MUST signal the wake domain before parking — every
// quiescence observation then wakes a parked worker, which re-runs the dance,
// and the count converges to the last worker setting global_terminate_.
//
// Deterministic construction (seam-driven, no sleeps):
//   1. Caller F runs on one worker and pauses at select_suspend_before_switch
//      (after G release, suspend_switch_pending RAISED, committed Waiting +
//      Armed + waiting_select_count_ == 1).
//   2. The OTHER worker (idle; Live run, mw_s3 with an external-wake select
//      wait) takes its park decision — RESIDENT (mw_s3 + external wake) or
//      mw_s1 fall-through, EITHER resets idle_workers_ to 0 — and is held at
//      the scheduler_park_candidate boundary BEFORE the physical park. It is
//      the ONLY worker that can reach that boundary while the caller's worker
//      is paused at the suspend seam, so the resolver's seam waits fix the
//      order deterministically.
//   3. Resolver: ev.set() routes F onto the caller's local_runnable and
//      signals the wake domain (epoch advance) — then releases the park
//      seam. The parked worker enters park_on_wake_source AFTER the signal:
//      its observed_epoch absorbs the advance and it sleeps (no re-check).
//   4. Resolver releases the suspend seam: the caller's worker completes the
//      switch, pops F, runs the caller to completion (quiescent), and enters
//      the idle dance. Its fetch_add observes prev == 0 (the other worker's
//      reset erased the count) so it is NOT the last idle worker. Pre-fix it
//      parks silently forever (unbounded park, no timeout, no signal); the
//      run_live join hangs. Post-fix it signals the wake source, the parked
//      worker wakes, re-checks, becomes the last idle worker, sets
//      global_terminate_, and the run converges.
//
// Mechanical proof: run_live returns (termination converged), caller resumed
// exactly once with the Event winner, fiber reached done.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_quiescent_not_last_idle_signals_domain) {
    if constexpr (!sa::fiber_ctx::supported) return;
    MWFixture f;
    Event ev(f.sched, /*initially_set=*/false);

    SelectResult captured;
    std::atomic<bool> resumed{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        captured = sa::select(f.sched, EventSelectCase{ev});
        resumed.store(true, std::memory_order_release);
    });
    FiberStack sw;
    SLUICE_CHECK(f.sched.init_fiber(fb, sw.base(), sw.size()));
    // Deterministic initial placement on worker 0 (E8 may steal before entry;
    // the construction is symmetric — see the comment above).
    f.sched.spawn_on(fb, /*worker_id=*/0);

    // Arm both causal seams before the run starts.
    stest::SelectPublicationSeam::arm_suspend_before_switch(f.sched);
    stest::SchedulerParkSeam::arm_candidate(f.sched);

    std::thread resolver([&] {
        // 1. The caller committed Waiting + Armed and is paused at the
        //    suspend seam (after G release).
        stest::SelectPublicationSeam::wait_suspend_before_switch_paused(f.sched);
        // 2. The idle worker took its resident/mw_s1 park decision (which
        //    resets idle_workers_) and is held BEFORE the physical park.
        stest::SchedulerParkSeam::wait_candidate_paused(f.sched);
        // 3. Route + signal the wake domain, then let the parked worker
        //    enter park_on_wake_source AFTER the signal (its observed_epoch
        //    absorbs the advance; it sleeps with no re-check).
        ev.set();
        stest::SchedulerParkSeam::release_candidate(f.sched);
        // 4. Let the caller's worker complete the switch, run the caller to
        //    completion, and enter the not-last idle park — which must wake
        //    the domain (the corrective) so the run converges.
        stest::SelectPublicationSeam::release_suspend_before_switch(f.sched);
    });

    // run_live returns ONLY when the coordinated run terminates — the pre-fix
    // code parks both workers forever here (deterministic hang).
    f.sched.run_live(2);
    resolver.join();

    SLUICE_CHECK_MSG(resumed.load(), "caller resumed exactly once");
    SLUICE_CHECK_MSG(captured.has_winner(), "winner produced");
    SLUICE_CHECK_MSG(captured.kind() == SelectKind::event, "Event winner");
    SLUICE_CHECK_MSG(fb.state() == FiberState::done,
                     "caller fiber reached done (run terminated after quiescence)");
}

namespace {

constexpr auto kForensicsWait = std::chrono::seconds(5);
constexpr auto kForensicsWatchdog = std::chrono::seconds(10);

class ForensicsTempPath {
public:
    explicit ForensicsTempPath(const char* tag) {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_phase_g_forensics_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(counter_++) + ".tmp"))
                    .string();
    }
    ~ForensicsTempPath() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    ForensicsTempPath(const ForensicsTempPath&) = delete;
    ForensicsTempPath& operator=(const ForensicsTempPath&) = delete;
    const std::string& path() const { return path_; }

private:
    std::string path_;
    static inline long counter_ = 0;
};

int forensics_open_temp(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "forensics_open_temp failed\n");
        std::exit(1);
    }
    return fd;
}

bool forensics_wait_flag(std::atomic<bool>& flag,
                         std::chrono::steady_clock::time_point deadline) {
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// Teardown guard: any early failure path (a setup SLUICE_CHECK, a fail_msg
// return) must still shut the runtime down — otherwise its joinable driver /
// worker threads abort via std::thread destruction ("terminate called
// without an active exception") and mask the forensic evidence. shutdown()
// is state-dispatched (P1-05): correct in every state, no-op when joined.
struct ForensicsRtGuard {
    sa::ApplicationRuntime* rt;
    ~ForensicsRtGuard() {
        if (rt != nullptr) {
            (void)rt->shutdown();
        }
    }
};

}  // namespace

// ===========================================================================
// Phase G park-window forensics (G1 BLOCKED instrumentation — NO behavioral
// fix in this change).
//
// The clean-rebuild parallel stress reproduced two permanent stalls with the
// Phase G unbounded wake-domain park:
//   - sluice_copy_pipeline_integration_test / pipeline_integration_multi_worker
//     (live gdb): ALL 4 scheduler workers parked unguarded in
//     park_on_wake_source (bounded_backend_observation=false), NO worker in
//     ctx_.wait_one, driver joining, app main waiting forever;
//   - application_runtime_drain_starvation_test /
//     final_backend_ready_request_drains_at_shutdown (coredump): one worker
//     parked unguarded mid-drain, ApplicationRuntime::drain() waiting
//     forever; the test teardown then destroyed a joinable std::thread ->
//     "terminate called without an active exception" (the SIGABRT is a
//     CONSEQUENCE of the stall, not a separate defect).
//
// This case re-runs the drain-shutdown scenario with a FORENSICS WATCHDOG:
// on a bounded timeout it dumps the park ledger + live scheduler state
// (AsyncTestAccess::dump_park_forensics) and exits fail-closed, so a
// repro states exactly which persistent state the parked worker trusted
// (classification, outstanding, backend ready generation) versus which
// publications happened after its baseline — distinguishing stale
// classification from a publication that never crosses the park boundary.
// On the healthy path the case passes like its non-forensics twin.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_park_window_forensics_drain_stall) {
    if constexpr (!sa::fiber_ctx::supported) return;

    sa::ThreadPoolBackend::WorkerRunningPauseGate gate;
    std::atomic<bool> wait_phase_entered{false};

    sa::ThreadPoolBackend* raw = nullptr;
    {
        auto backend = std::make_unique<sa::ThreadPoolBackend>(
            sa::ThreadPoolConfig{/*request_capacity=*/2, /*worker_count=*/2});
        raw = backend.get();
        raw->set_running_pause_gate(&gate);
        raw->set_wait_phase_flag_for_test(&wait_phase_entered);

        sa::RuntimeBuilder builder;
        builder.backend(std::move(backend)).workers(2);
        auto rt_result = builder.build();
        SLUICE_CHECK(rt_result.has_value());
        auto& rt = *rt_result.value();
        SLUICE_CHECK(rt.start().has_value());
        ForensicsRtGuard rt_guard{&rt};
        // Forensics entry point (internal-testing accessor; observation
        // only). Arm the park ledger — off by default because its snapshot
        // locks shift park timing for other tests.
        Scheduler& sched = rt.test_scheduler_for_worker_topology();
        AsyncTestAccess::set_park_forensics(sched, true);

        ForensicsTempPath tp("G");
        int fd = forensics_open_temp(tp.path());
        const std::byte seed[1] = {std::byte{0x67}};
        SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

        std::byte buf[1]{};
        sa::Completion<std::size_t> c;
        std::atomic<bool> task_done{false};
        SLUICE_CHECK(rt.submit([&](sa::RuntimeTaskContext& rctx) {
            auto sr = rctx.submit_read(sa::ReadOp{fd, buf, 1, 0}, c);
            if (sr.has_value()) {
                (void)rctx.await_completion(c);
            }
            task_done.store(true, std::memory_order_release);
        }).has_value());

        const char* fail_msg = nullptr;
        const auto deadline =
            std::chrono::steady_clock::now() + kForensicsWait;
        if (!forensics_wait_flag(gate.paused, deadline)) {
            AsyncTestAccess::dump_park_forensics(sched, "gate-never-paused");
            fail_msg = "running gate did not pause in time (see dump)";
        } else if (!forensics_wait_flag(wait_phase_entered, deadline)) {
            AsyncTestAccess::dump_park_forensics(sched, "no-wait-phase");
            fail_msg = "the MW-S2 participant never entered the backend ready wait (see dump)";
        }
        if (fail_msg == nullptr) {
            rt.request_stop();
            std::this_thread::yield();
        }
        if (fail_msg == nullptr) {
            // Disarm the seams BEFORE drain/join (join destroys the backend;
            // `raw` must not be touched afterwards).
            raw->set_running_pause_gate(nullptr);
            raw->set_wait_phase_flag_for_test(nullptr);
            sa::resume_threadpool_gate(gate);
        }

        // Watchdog-bounded drain + join: on timeout, DUMP and exit
        // fail-closed (std::_Exit — no teardown of the stuck threads, which
        // would abort via joinable-thread destruction and mask the dump).
        if (fail_msg == nullptr) {
            std::atomic<bool> drain_done{false};
            std::thread drainer([&] {
                (void)rt.drain();
                drain_done.store(true, std::memory_order_release);
            });
            const auto wd =
                std::chrono::steady_clock::now() + kForensicsWatchdog;
            while (!drain_done.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= wd) {
                    AsyncTestAccess::dump_park_forensics(sched, "drain-stall");
                    std::fprintf(stderr,
                                 "FORENSICS: drain stalled — park-window "
                                 "violation captured; gdb-attach window "
                                 "(pid=%d, 20s), then exit fail-closed\n",
                                 static_cast<int>(::getpid()));
                    std::this_thread::sleep_for(std::chrono::seconds(20));
                    std::_Exit(70);
                }
                std::this_thread::yield();
            }
            drainer.join();

            std::atomic<bool> join_done{false};
            std::thread joiner([&] {
                (void)rt.join();
                join_done.store(true, std::memory_order_release);
            });
            const auto wd2 =
                std::chrono::steady_clock::now() + kForensicsWatchdog;
            while (!join_done.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= wd2) {
                    AsyncTestAccess::dump_park_forensics(sched, "join-stall");
                    std::fprintf(stderr,
                                 "FORENSICS: join stalled — park-window "
                                 "violation captured; gdb-attach window "
                                 "(pid=%d, 20s), then exit fail-closed\n",
                                 static_cast<int>(::getpid()));
                    std::this_thread::sleep_for(std::chrono::seconds(20));
                    std::_Exit(70);
                }
                std::this_thread::yield();
            }
            joiner.join();
        }

        if (fail_msg == nullptr && !task_done.load(std::memory_order_acquire)) {
            AsyncTestAccess::dump_park_forensics(sched, "task-not-done");
            fail_msg = "task never reached terminal (park-window violation; see dump)";
        }
        if (fail_msg == nullptr && !c.ready()) {
            fail_msg = "Completion not ready after drain";
        }
        if (fail_msg == nullptr && raw->backend_ready_count_for_test() != 0) {
            AsyncTestAccess::dump_park_forensics(sched, "ready-not-reaped");
            fail_msg = "backend-ready count nonzero after drain (see dump)";
        }
        if (fail_msg == nullptr && raw->outstanding() != 0) {
            fail_msg = "outstanding nonzero after drain";
        }
        if (fail_msg == nullptr && raw->arena_slot_in_use() != 0) {
            fail_msg = "slot-in-use nonzero after drain";
        }
        SLUICE_CHECK_MSG(fail_msg == nullptr,
                         fail_msg != nullptr ? fail_msg : "forensics scenario green");
        (void)::close(fd);
    }
}

SLUICE_MAIN()
