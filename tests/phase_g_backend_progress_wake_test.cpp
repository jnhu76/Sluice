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
// Phase G G1 park-window stall — DETERMINISTIC reproducer (PR #108 review
// P2b: causal-seam construction replacing the yield-ordered high-probability
// canary; NO behavioral fix in this change).
//
// The stall (design doc §8): the MW-S2 participant — pre-repair, the only
// worker allowed to elect (ws->id == 0) — parks in ctx_.wait_one (backend
// domain). request_stop interrupts it; the still-gated read yields
// 0 progress, so it takes the mw_s2_no_progress_terminate exit and
// its thread DIES. The gate is then released; the backend publishes
// backend-ready to a wait source with no observer. The survivor (held at
// its post-park recheck seam) re-drains, REAPS the request, and
// routes the task continuation to its OWNER — the dead participant. The
// route clears global_terminate_ and its wake signal is absorbed by the
// survivor's own park baseline; the survivor classifies mw_s1 ("someone
// will run it"), trusts it, and parks UNBOUNDED in the wake domain. The
// runnable is stranded on a dead worker's queue, no wake source remains,
// and drain() waits forever. Every step below is causally observed — no
// yield, no sleep-as-ordering (production-test-plan §1). The construction
// is ROLE-BASED (participant/survivor identified by park_domain at step
// 1b): the run-entry seam places the fiber on worker 0's inbox, but worker
// 1 may still steal it before worker 0's first pop, so the participant is
// whichever worker ran the task fiber. PRE-REPAIR the stolen-fiber variant
// fails closed at step (1) (a non-zero-id worker cannot elect — the
// no-participant unguarded-park manifestation, also deterministic RED),
// and POST-REPAIR the same construction must converge (GREEN) in every
// interleaving: the strand either never forms (survivor == owner) or is
// resolved by the park-commit recheck (the survivor steals it).
//
// Deterministic construction:
//   0. Hold run_impl at the run-entry seam (topology published, NO worker
//      thread started), start the runtime, submit the task while held, then
//      release. The submit routes the fiber onto worker 0's inbox under the
//      published topology, so the FIRST invocation's workers start with the
//      ticket already queued — the run can never converge to a worker-exiting
//      last-idle terminate before the task runs (a plain submit-after-start
//      loses exactly that race in Release builds and the parked pair below
//      cannot form).
//   1. The backend worker pauses mid-read at the running gate. The worker
//      running the task fiber elects MW-S2 and parks in ctx_.wait_one
//      (wait_phase_entered fires). OBSERVE both flags. (Worker 1 may still
//      steal the fiber before worker 0's first pop — the roles below are
//      identified by park_domain, not by id.)
//   1b. Pin the parked pair BEFORE the stop: park_domain(participant)==Backend
//      (set at the MW-S2 commit, cleared only when wait_one returns) AND
//      park_domain(survivor)==Scheduler (inside park_on_wake_source). Both
//      are stable while the read is gated, so the stop wake is GUARANTEED to
//      route the survivor through its park return and into the held seam —
//      without this pin a mid-cycle survivor instead exits on
//      global_terminate_, the driver re-enters with fresh workers, and the
//      scenario heals nondeterministically (observed as the old rc 124/134
//      flake mix). A pin timeout is a construction violation and fails
//      closed with its own dump tag.
//   2. ONLY NOW arm the worker_park_returned seam (arming earlier would
//      catch the pre-submit quiescence park-returns of run #1 and wedge the
//      setup; after the participant park the system is quiescent — the only
//      future publications are this test's).
//   3. rt.request_stop(). The participant's ARMED baseline observes the
//      interrupt; the gated read reaps 0; the participant exits
//      mw_s2_no_progress_terminate (loop_exited[participant] becomes true —
//      causally dead, its inbox residue is stranded unless a live
//      participant re-seeds/steals it). The survivor wakes from its
//      wake-domain park and is HELD at the post-park recheck seam (excluded
//      from release_all_phases — the participant's exit path must not
//      destroy the hold). OBSERVE both.
//   4. Release the backend gate; the read completes and publishes
//      backend-ready (ready epoch advances). OBSERVE the ready publication
//      (backend_wait_token) — no observer exists for it: the participant is
//      dead, the survivor is held.
//   5. Release the seam: the survivor's loop-top drain reaps the request and
//      routes the continuation to the dead participant (owner). OBSERVE the
//      stranded state (backend-ready count 0, participant runnable depth 1).
//   6. Verdict: rt.drain() must converge within the watchdog. PRE-FIX it
//      never does (deterministic RED): the watchdog dumps the park ledger +
//      live state and exits fail-closed (rc 70). The G1 repair makes the
//      drain converge (GREEN): the task reaches terminal, the Completion is
//      ready, and the backend counters — snapshotted BEFORE rt.join()
//      (join destroys the backend; touching `raw` afterwards is the UAF the
//      PR #108 review flagged as P1a) — are all zero.
// ===========================================================================
SLUICE_TEST_CASE(phase_g_g1_stranded_runnable_park_stall_reproducer) {
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
        ForensicsRtGuard rt_guard{&rt};
        Scheduler& sched = rt.test_scheduler_for_worker_topology();
        // Seam controller: registered before any worker can reach the
        // worker_park_returned call site; destroyed (unregistered) before
        // rt_guard's shutdown joins the workers (reverse declaration order).
        stest::ControllerGuard ctrl_guard{sched};
        // Park ledger on for the stall dump (its snapshot locks shift park
        // timing — acceptable here, the scenario is seam-driven).
        AsyncTestAccess::set_park_forensics(sched, true);

        ForensicsTempPath tp("G1");
        int fd = forensics_open_temp(tp.path());
        const std::byte seed[1] = {std::byte{0x67}};
        SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

        std::byte buf[1]{};
        sa::Completion<std::size_t> c;
        std::atomic<bool> task_done{false};

        // Fail-closed forensics exit: dump + gdb attach window + _Exit(70).
        // EVERY construction violation and stall verdict takes this path.
        // The scenario is deliberately NOT recovered: a mid-stall teardown
        // either aborts via joinable-thread destruction (the old canary's
        // rc 134) or hangs in shutdown (rc 124) — both mask the evidence and
        // made two failure modes randomly compete for the exit code (PR #108
        // review P2b). Pre-fix this case therefore ALWAYS exits rc 70 with a
        // dump naming the exact stalled state.
        auto fail_closed = [&sched](const char* tag, const char* msg) {
            AsyncTestAccess::dump_park_forensics(sched, tag);
            std::fprintf(stderr,
                         "FORENSICS: %s; gdb-attach window (pid=%d, 20s), "
                         "then exit fail-closed\n",
                         msg, static_cast<int>(::getpid()));
            std::this_thread::sleep_for(std::chrono::seconds(20));
            std::_Exit(70);
        };

        const auto setup_deadline =
            std::chrono::steady_clock::now() + kForensicsWait;

        // (0) Deterministic work placement: hold run_impl at the run-entry
        // seam — the invocation topology is published (active_worker_count_
        // == 2, terminate cleared, next_spawn_worker_ reset) but NO worker
        // thread has started — then start, submit the task while held, and
        // release. The submit routes the fiber onto worker 0's inbox under
        // the published topology (next_spawn_worker_ == 0), so the first
        // invocation's workers start with the ticket already queued and the
        // run can NEVER converge to a worker-exiting last-idle terminate
        // before the task runs. A plain submit-after-start loses exactly
        // that race in Release builds (observed: both workers idle, one
        // exits last_idle_terminate, the late submit leaves the fiber for
        // the single survivor, and the parked pair pinned below can never
        // form — rc 70 no-participant-parked-pair).
        stest::TopologyReadySeam::arm(sched);
        SLUICE_CHECK(rt.start().has_value());
        {
            bool topology_paused = false;
            while (std::chrono::steady_clock::now() < setup_deadline) {
                if (stest::TopologyReadySeam::is_paused(sched)) {
                    topology_paused = true;
                    break;
                }
                std::this_thread::yield();
            }
            if (!topology_paused) {
                fail_closed("topology-ready-never-paused",
                            "run-entry seam never paused (driver did not "
                            "reach the pre-start boundary)");
            }
        }
        SLUICE_CHECK(rt.submit([&](sa::RuntimeTaskContext& rctx) {
            auto sr = rctx.submit_read(sa::ReadOp{fd, buf, 1, 0}, c);
            if (sr.has_value()) {
                (void)rctx.await_completion(c);
            }
            task_done.store(true, std::memory_order_release);
        }).has_value());
        stest::TopologyReadySeam::release(sched);

        // (1) The backend worker is paused mid-read AND the MW-S2
        // participant is parked in ctx_.wait_one (backend domain). "wait
        // phase never entered" is itself a G1 manifestation: pre-repair,
        // when the task fiber lands on a worker that cannot elect
        // (ws->id != 0), its mw_s2 classify cannot elect and BOTH workers
        // park unguarded in the wake domain with no observer for the gated
        // read — the same unguarded-park defect (design §8.3), reported
        // fail-closed like the primary one.
        if (!forensics_wait_flag(gate.paused, setup_deadline)) {
            fail_closed("gate-never-paused",
                        "running gate did not pause in time");
        }
        if (!forensics_wait_flag(wait_phase_entered, setup_deadline)) {
            fail_closed("no-wait-phase",
                        "no MW-S2 participant entered the backend ready wait "
                        "(unguarded-park G1 manifestation)");
        }

        // (1b) Pin the parked configuration causally BEFORE the stop: ONE
        // worker is INSIDE its backend-domain park (park_domain == Backend,
        // set at the MW-S2 commit and cleared only when wait_one returns)
        // and the OTHER is INSIDE park_on_wake_source (park_domain ==
        // Scheduler, cleared only when the park returns). Both are stable
        // while the read is gated, so the upcoming request_stop wake is
        // GUARANTEED to route the survivor through its park return and into
        // the held seam. ROLE-BASED (post-G1-repair): with the transferable
        // election and the spawn/steal placement race, the participant is
        // whichever worker ran the task fiber — the strand forms on THAT
        // worker when it dies (pre-fix variant A: participant == owner ==
        // worker 0; the pre-fix stolen-fiber variant B fails closed earlier,
        // at the step-(1) wait-phase check).
        // Without this pin a mid-cycle survivor instead exits on
        // global_terminate_, the driver re-enters with fresh workers, and
        // the scenario heals nondeterministically (observed as the old
        // rc 124/134 flake mix).
        unsigned participant = 0;
        unsigned survivor = 1;
        {
            const auto parked_deadline =
                std::chrono::steady_clock::now() + kForensicsWait;
            bool parked_pair = false;
            while (std::chrono::steady_clock::now() < parked_deadline) {
                const auto d0 = AsyncTestAccess::worker_park_domain(sched, 0);
                const auto d1 = AsyncTestAccess::worker_park_domain(sched, 1);
                if (d0 == sa::WorkerState::ParkDomain::Backend &&
                    d1 == sa::WorkerState::ParkDomain::Scheduler) {
                    participant = 0;
                    survivor = 1;
                    parked_pair = true;
                    break;
                }
                if (d1 == sa::WorkerState::ParkDomain::Backend &&
                    d0 == sa::WorkerState::ParkDomain::Scheduler) {
                    participant = 1;
                    survivor = 0;
                    parked_pair = true;
                    break;
                }
                std::this_thread::yield();
            }
            if (!parked_pair) {
                fail_closed(
                    "no-participant-parked-pair",
                    "participant/survivor park pair never established "
                    "(no-participant or mid-cycle survivor G1 manifestation)");
            }
        }

        // (2) Arm the post-park recheck seam ONLY now — arming earlier would
        // catch the pre-submit quiescence park-returns of the first run
        // invocation and wedge the setup; after the participant park the
        // system is quiescent, so the only future publications are this
        // test's.
        stest::WorkerParkReturnSeam::arm(sched);
        // Baseline the backend wait token before the completion gate is
        // released, so the ready publication below is unambiguous.
        const auto token_before = AsyncTestAccess::backend_wait_token(sched);

        // (3) request_stop: the participant's ARMED baseline observes the
        // interrupt; the still-gated read reaps 0, so it takes the
        // no-progress terminate and its thread dies (loop_exited — causally
        // dead; its inbox residue is stranded unless a live participant
        // re-seeds or steals it). The survivor's park returns on the stop
        // wake and is held at the seam.
        rt.request_stop();
        {
            const auto death_deadline =
                std::chrono::steady_clock::now() + kForensicsWait;
            bool participant_dead = false;
            while (std::chrono::steady_clock::now() < death_deadline) {
                if (AsyncTestAccess::worker_loop_exited(sched, participant)) {
                    participant_dead = true;
                    break;
                }
                std::this_thread::yield();
            }
            if (!participant_dead) {
                fail_closed("participant-never-died",
                            "the participant worker did not reach the "
                            "no-progress terminate");
            }
        }

        // Hold the survivor at its post-park recheck. BOUNDED: if the flow
        // diverged (e.g. a repair that never parks the survivor), the seam
        // never pauses — disarm it and let the drain verdict below measure
        // convergence instead of wedging here.
        {
            const auto seam_deadline =
                std::chrono::steady_clock::now() + kForensicsWait;
            while (!stest::WorkerParkReturnSeam::is_paused(sched)) {
                if (std::chrono::steady_clock::now() >= seam_deadline) break;
                std::this_thread::yield();
            }
            if (stest::WorkerParkReturnSeam::is_paused(sched)) {
                // (4) Complete the read and observe the backend-ready
                // publication — which has NO observer: the participant is
                // dead, the survivor is held.
                sa::resume_threadpool_gate(gate);
                const auto ready_deadline =
                    std::chrono::steady_clock::now() + kForensicsWait;
                bool ready_published = false;
                while (std::chrono::steady_clock::now() < ready_deadline) {
                    if (AsyncTestAccess::backend_wait_token(sched)
                            .progress_generation >
                        token_before.progress_generation) {
                        ready_published = true;
                        break;
                    }
                    std::this_thread::yield();
                }
                if (!ready_published) {
                    fail_closed("ready-never-published",
                                "backend-ready publication not observed");
                }

                // (5) Release the survivor: its loop-top drain reaps the
                // completed request and routes the task continuation to its
                // OWNER — the dead worker. The route clears
                // global_terminate_ and its wake signal is absorbed by the
                // survivor's own park baseline; it classifies mw_s1 and
                // parks unbounded (the G1 stall).
                stest::WorkerParkReturnSeam::release(sched);
                const auto strand_deadline =
                    std::chrono::steady_clock::now() + kForensicsWait;
                bool stranded = false;
                while (std::chrono::steady_clock::now() < strand_deadline) {
                    if (raw->backend_ready_count_for_test() == 0 &&
                        AsyncTestAccess::worker_local_runnable(sched, 0) +
                                AsyncTestAccess::worker_local_runnable(sched, 1) ==
                            1) {
                        stranded = true;
                        break;
                    }
                    std::this_thread::yield();
                }
                // Forensic marker, not a verdict: pre-fix the strand is
                // permanent (the drain watchdog below fires); post-fix the
                // repair may resolve it before this observation completes.
                std::fprintf(stderr,
                             "FORENSICS: stranded-runnable observed=%d "
                             "(participant=%u survivor=%u ready_count=%zu "
                             "w0_runnable=%zu w1_runnable=%zu)\n",
                             stranded ? 1 : 0, participant, survivor,
                             raw->backend_ready_count_for_test(),
                             AsyncTestAccess::worker_local_runnable(sched, 0),
                             AsyncTestAccess::worker_local_runnable(sched, 1));
            } else {
                // Divergent flow: no survivor was held. Complete the read and
                // let the drain verdict measure convergence.
                std::fprintf(stderr,
                             "FORENSICS: park-return seam never paused "
                             "(construction diverged; drain verdict decides)\n");
                stest::WorkerParkReturnSeam::release(sched);
                sa::resume_threadpool_gate(gate);
            }
        }

        // Disarm the backend test seams BEFORE drain/join (join destroys the
        // backend; `raw` must not be touched afterwards — P1a).
        raw->set_running_pause_gate(nullptr);
        raw->set_wait_phase_flag_for_test(nullptr);

        // (6) THE VERDICT — watchdog-bounded drain. Pre-fix it never
        // converges (fail-closed above); the G1 repair must make it
        // converge with no other change to this construction.
        {
            std::atomic<bool> drain_done{false};
            std::thread drainer([&] {
                (void)rt.drain();
                drain_done.store(true, std::memory_order_release);
            });
            const auto wd =
                std::chrono::steady_clock::now() + kForensicsWatchdog;
            while (!drain_done.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= wd) {
                    fail_closed("g1-stranded-runnable-park-stall",
                                "G1 stall — drain never converged");
                }
                std::this_thread::yield();
            }
            drainer.join();
        }

        // P1a (PR #108 review): snapshot the backend counters while the
        // backend is still alive — rt.join() destroys the runtime's
        // IoContext and with it the ThreadPoolBackend, so any read via
        // `raw` after the join is a use-after-free.
        const std::size_t ready_at_drain = raw->backend_ready_count_for_test();
        const std::size_t outstanding_at_drain = raw->outstanding();

        // Post-drain caller lifecycle (ADR Decision 15, same discipline as
        // final_backend_ready_request_drains_at_shutdown): reap published
        // Completion-ready, but the arena slot is released only by the
        // CALLER's reset — quiescent teardown (join -> close_resources ->
        // ~ThreadPoolBackend) requires slot_in_use == 0 BEFORE join.
        SLUICE_CHECK_MSG(task_done.load(std::memory_order_acquire),
                         "task never reached terminal (park-window violation)");
        SLUICE_CHECK_MSG(c.ready(), "Completion not ready after drain");
        SLUICE_CHECK_MSG(ready_at_drain == 0,
                         "backend-ready count nonzero after drain");
        SLUICE_CHECK_MSG(outstanding_at_drain == 0,
                         "outstanding nonzero after drain");
        c.reset();
        const std::size_t slot_in_use_at_drain = raw->arena_slot_in_use();
        SLUICE_CHECK_MSG(slot_in_use_at_drain == 0,
                         "caller reset must release the slot before join");

        // Watchdog-bounded join (same fail-closed pattern as the drain).
        {
            std::atomic<bool> join_done{false};
            std::thread joiner([&] {
                (void)rt.join();
                join_done.store(true, std::memory_order_release);
            });
            const auto wd2 =
                std::chrono::steady_clock::now() + kForensicsWatchdog;
            while (!join_done.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= wd2) {
                    fail_closed("join-stall", "join never converged");
                }
                std::this_thread::yield();
            }
            joiner.join();
        }
        (void)::close(fd);
    }
}

SLUICE_MAIN()
