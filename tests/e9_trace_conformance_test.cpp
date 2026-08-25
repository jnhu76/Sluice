// e9_trace_conformance_test — Issue #196 V2 trace-conformance pilot.
//
// Captures MINIMAL semantic traces of the E9 park/wake protocol from REAL
// deterministic C++ executions (the SLUICE_ASYNC_INTERNAL_TESTING seams + the
// #196 controller recorder), asserts each trace's deterministic shape, and —
// when SLUICE_E9_TRACE_OUT names a directory — writes each trace as a JSON
// artifact bound to the revision in SLUICE_E9_TRACE_REVISION. The same-revision
// model-side validation (a generated TLC replay wrapper over the PRISTINE
// spec/tla/e9_park_wake/E9ParkWake.tla) is scripts/formal/e9_trace_validate.py,
// driven by scripts/formal/verify-e9-trace-conformance.sh.
//
// Corpus (each case's trace shape is asserted IN THIS FILE; the checked-in
// canonical fixtures under spec/tla/e9_park_wake/traces/ mirror these shapes
// — the assertions here are the freshness link):
//
//   T1  split (ThreadPoolBackend), Live, Event wait: the single worker's
//       UNARMED park returns on an external wake-handle notify published
//       strictly while the worker is inside cv.wait. Determinism: the notify
//       thread polls the recorder for ParkEntered; record_entered and the
//       cv.wait entry run under ONE continuous wake_mtx_ hold, so a notifier
//       serializing on wake_mtx_ cannot land between them — the return is
//       provably a blocking-park return (immediate=false).
//         [ParkCommitted, ParkEntered, WakePublished(external),
//          ParkReturned(blocked, {epoch})]
//   T2  split, Live, Event wait: the notify is published strictly inside the
//       commit→wait window (post-baseline seam hold) — the wake-epoch advance
//       is visible ONLY to the cv predicate. The predicate is true at wait
//       entry: an immediate return (the model's EnterPhysicalPark
//       predicate-true branch).
//         [ParkCommitted, WakePublished(external), ParkEntered,
//          ParkReturned(immediate, {epoch})]
//   T3  reference (FakeAsyncBackend), Live, ready-flag wait: the ENTRY-ARMED
//       MW-S3 park (E5-A2 observation, the #185 observationArmed shape)
//       returns on the 2 ms observation timeout with NO wake at all.
//         [ParkCommitted(armed), ParkEntered, ParkReturned({timeout})]
//   T4  reference, Drain, one never-completed submitted op: the unarmed
//       MW-S2 NON-participant park returns on the participant's no-progress
//       terminate publication. The parked return and the retiring
//       participant's epilogue wake race after the terminate signal — BOTH
//       orders are legal and both validate against the model; this test
//       asserts the trace is one of the two legal orders.
//         [ParkCommitted, ParkEntered, WakePublished(terminate),
//          (ParkReturned({epoch,terminate}) + WakePublished(retire)) in
//          either order]
//   T5  reference, Live: a runnable publication (spawn_on onto the parking
//       worker's own inbox) lands strictly BEFORE the park baseline (seam B
//       hold); the G1 recheck REFUSES the park and signals (the #115
//       runnable-first law).
//         [WakePublished(runnable_route), ParkRefused,
//          WakePublished(park_refuse)]
//
// The negative controls (forbidden MUTANT traces — never emitted by this
// binary) live as checked-in fixtures: neg_a (causeless return of T1's
// shape; LeavePark has no enabled disjunct under SplitWait=TRUE) and neg_b
// (the pre-#185-style unconditional-escape claim on T4's un-armed reference
// park; the faithful escape requires observationArmed). #185 was MODEL
// drift, never a C++ defect — neg_b is a forbidden semantic mutant trace.
//
// Every capture is a bounded, seam-controlled window: the recorder is
// enabled before the first in-window event and disabled after the last
// (with the emitting worker held at a seam where needed), so the recorded
// content is deterministic. Bounded watchdogs fail closed (issue115
// discipline); sleeps are never ordering evidence.
#include "harness.hpp"
#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/async/wait_node.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace sa = sluice::async;
namespace stest = sluice_async_test;

using Scheduler = sa::Scheduler;
using Fiber = sa::Fiber;
using FiberState = sa::FiberState;
using TraceEvent = stest::TraceEvent;

namespace {

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// Test-side rendezvous (issue115 pattern): blocks a worker INSIDE user fiber
// code without busy-spinning.
struct Rendezvous {
    std::mutex mtx;
    std::condition_variable cv;
    bool released = false;
    void wait() {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [this] { return released; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            released = true;
        }
        cv.notify_all();
    }
};

constexpr auto kWatchdog = std::chrono::milliseconds(10000);

bool wait_flag(const std::atomic<bool>& flag, std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// ---- trace window helper ----------------------------------------------------
// Polls the recorder until `pred` holds over the snapshot (bounded watchdog;
// polling observes only RECORDED state — never a sleep-based ordering proof;
// the deterministic order comes from the seams, the wake_mtx_ hold, and the
// per-event recording points).
template <typename Pred>
std::vector<TraceEvent> wait_trace(Scheduler& s, Pred pred,
                                   const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + kWatchdog;
    for (;;) {
        std::vector<TraceEvent> ev = stest::E9TraceRecorder::events(s);
        if (pred(ev)) return ev;
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "e9-trace: timed out waiting for %s\n", what);
            return ev;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

const char* kind_name(const TraceEvent& ev) {
    switch (static_cast<stest::TraceEventKind>(ev.kind)) {
        case stest::TraceEventKind::park_committed: return "ParkCommitted";
        case stest::TraceEventKind::park_entered: return "ParkEntered";
        case stest::TraceEventKind::park_refused: return "ParkRefused";
        case stest::TraceEventKind::wake_published: return "WakePublished";
        case stest::TraceEventKind::park_returned: return "ParkReturned";
    }
    return "?";
}

const char* cause_name(const TraceEvent& ev) {
    switch (static_cast<stest::WakeCause>(ev.cause)) {
        case stest::WakeCause::none: return "none";
        case stest::WakeCause::external_notify: return "external";
        case stest::WakeCause::runnable_route: return "runnable_route";
        case stest::WakeCause::park_refuse: return "refuse";
        case stest::WakeCause::terminate: return "terminate";
        case stest::WakeCause::retire_epilogue: return "retire";
        case stest::WakeCause::idle_dance: return "idle_dance";
    }
    return "?";
}

// ---- #210 fail-safe runner cleanup -----------------------------------------
// SLUICE_CHECK/SLUICE_FAIL record the failure and RETURN from the testcase
// (harness.hpp); they do not throw. Any such early return between runner
// construction and runner.join() used to destroy a JOINABLE std::thread, and
// ~thread() on a joinable thread calls std::terminate() — a SECONDARY
// cleanup failure that replaced the real report with
// "terminate called without an active exception" (issue #210). The guard is
// declared immediately AFTER the runner (so it runs BEFORE the thread's
// destructor) and makes every early exit fail-safe:
//   1. pre_join: release exactly the seams this test armed (release is
//      idempotent and state-based: a worker paused under the arm wakes, a
//      worker arriving after the release never pauses) and close the trace
//      window — the releases let the coordinated run converge on its own
//      termination semantics (the MW-S2 participant's FakeAsyncBackend
//      wait_one() is non-blocking by contract and publishes
//      global_terminate_, which every parked/looping worker observes);
//   2. join the runner (no joinable std::thread is ever destroyed);
//   3. post_join: drain test-held backend state so the Completion and
//      AsyncIoContext destruction contracts hold.
// The harness then reports the ORIGINAL failure. Not std::jthread (a
// destructor join could deadlock on an armed seam), not detach (a Scheduler/
// context lifetime race), not catch(...) (the harness does not throw).
struct RunnerCleanup {
    std::thread& runner;
    std::function<void()> pre_join;
    std::function<void()> post_join;
    explicit RunnerCleanup(std::thread& r,
                           std::function<void()> pre = nullptr,
                           std::function<void()> post = nullptr)
        : runner(r), pre_join(std::move(pre)), post_join(std::move(post)) {}
    ~RunnerCleanup() {
        if (!runner.joinable()) return;  // testcase joined on the normal path
        if (pre_join) pre_join();
        runner.join();
        if (post_join) post_join();
    }
    RunnerCleanup(const RunnerCleanup&) = delete;
    RunnerCleanup& operator=(const RunnerCleanup&) = delete;
};

// Bounded variant of the controller's unbounded wait_paused for call sites
// reachable AFTER a bounded-watchdog failure observation (#210): the fail
// path must stay bounded — never convert a crash into a hang. Polling
// observes the same persistent `paused` state the cv wait would; the 1 ms
// granularity matches wait_trace/wait_flag and proves nothing about
// ordering (the seams do).
bool wait_paused_bounded(Scheduler& s, stest::PhaseTag tag, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + kWatchdog;
    while (!stest::is_paused(s, tag)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "e9-trace: timed out waiting for pause %s\n",
                         what);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// ---- #210 fail-path forensics (test-only stderr diagnostics) ---------------
// Print the trace snapshot and the seam states AT the moment a bounded
// watchdog failed, so the PRIMARY failure (which wait timed out, how far the
// protocol actually got, which worker held which role) reaches the report
// instead of being masked by a secondary terminate/hang.
void dump_trace(const char* what, const std::vector<TraceEvent>& evs) {
    std::fprintf(stderr, "e9-trace: %s — observed %zu event(s):\n", what,
                 evs.size());
    for (std::size_t i = 0; i < evs.size(); ++i) {
        const TraceEvent& ev = evs[i];
        std::fprintf(stderr, "  [%zu] %s w=%u", i, kind_name(ev),
                     static_cast<unsigned>(ev.worker));
        if (static_cast<stest::TraceEventKind>(ev.kind) ==
            stest::TraceEventKind::wake_published) {
            std::fprintf(stderr, " cause=%s epoch=%llu", cause_name(ev),
                         static_cast<unsigned long long>(ev.epoch));
        }
        if (static_cast<stest::TraceEventKind>(ev.kind) ==
            stest::TraceEventKind::park_committed) {
            std::fprintf(stderr, " armed=%d epoch=%llu", ev.armed ? 1 : 0,
                         static_cast<unsigned long long>(ev.epoch));
        }
        if (static_cast<stest::TraceEventKind>(ev.kind) ==
            stest::TraceEventKind::park_returned) {
            std::fprintf(stderr, " immediate=%d causes=0x%x",
                         ev.immediate ? 1 : 0,
                         static_cast<unsigned>(ev.return_causes));
        }
        std::fprintf(stderr, "\n");
    }
}

void dump_seam_states(Scheduler& s) {
    const struct {
        const char* name;
        stest::PhaseTag tag;
    } seams[] = {
        {"mw_s2_committed_before_wait_one",
         stest::PhaseTag::mw_s2_committed_before_wait_one},
        {"scheduler_park_candidate", stest::PhaseTag::scheduler_park_candidate},
        {"scheduler_park_commit", stest::PhaseTag::scheduler_park_commit},
        {"scheduler_park_baseline_recorded",
         stest::PhaseTag::scheduler_park_baseline_recorded},
        {"worker_park_returned", stest::PhaseTag::worker_park_returned},
    };
    std::fprintf(stderr, "e9-trace: seam states at failure:\n");
    for (const auto& e : seams) {
        std::fprintf(stderr, "  %-36s reached=%d paused=%d\n", e.name,
                     stest::is_reached(s, e.tag) ? 1 : 0,
                     stest::is_paused(s, e.tag) ? 1 : 0);
    }
}

// Emits the captured window as a #196 trace JSON artifact (bound to the
// revision from SLUICE_E9_TRACE_REVISION; absent env -> "unbound", which the
// validator fail-closes on). Returns false when no output was requested.
bool write_trace_json(const char* test, bool split_wait, const char* run_mode,
                      const char* prehistory,
                      const std::vector<TraceEvent>& events) {
    const char* dir = std::getenv("SLUICE_E9_TRACE_OUT");
    if (dir == nullptr || *dir == '\0') return false;
    const char* rev = std::getenv("SLUICE_E9_TRACE_REVISION");
    if (rev == nullptr) rev = "unbound";
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s.json", dir, test);
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) return false;
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"schema\": 1,\n");
    std::fprintf(f, "  \"suite\": \"e9_park_wake\",\n");
    std::fprintf(f, "  \"test\": \"%s\",\n", test);
    std::fprintf(f, "  \"cpp_revision\": \"%s\",\n", rev);
    std::fprintf(f, "  \"model_revision\": \"%s\",\n", rev);
    std::fprintf(f, "  \"split_wait\": %s,\n", split_wait ? "true" : "false");
    std::fprintf(f, "  \"run_mode\": \"%s\",\n", run_mode);
    std::fprintf(f, "  \"prehistory\": \"%s\",\n", prehistory);
    std::fprintf(f, "  \"events\": [\n");
    for (std::size_t i = 0; i < events.size(); ++i) {
        const TraceEvent& ev = events[i];
        std::fprintf(f, "    {\"seq\": %zu, \"event\": \"%s\"", i + 1,
                     kind_name(ev));
        if (ev.worker != stest::kTraceNoWorker) {
            std::fprintf(f, ", \"worker\": %u",
                         static_cast<unsigned>(ev.worker));
        }
        if (static_cast<stest::TraceEventKind>(ev.kind) ==
            stest::TraceEventKind::wake_published) {
            std::fprintf(f, ", \"epoch\": %llu, \"cause\": \"%s\"",
                         static_cast<unsigned long long>(ev.epoch),
                         cause_name(ev));
        }
        if (static_cast<stest::TraceEventKind>(ev.kind) ==
            stest::TraceEventKind::park_committed) {
            std::fprintf(f, ", \"epoch\": %llu, \"armed\": %s",
                         static_cast<unsigned long long>(ev.epoch),
                         ev.armed ? "true" : "false");
        }
        if (static_cast<stest::TraceEventKind>(ev.kind) ==
            stest::TraceEventKind::park_returned) {
            std::fprintf(f, ", \"immediate\": %s, \"causes\": [",
                         ev.immediate ? "true" : "false");
            bool first = true;
            auto bit = [&](std::uint16_t m, const char* n) {
                if (ev.return_causes & m) {
                    std::fprintf(f, "%s\"%s\"", first ? "" : ", ", n);
                    first = false;
                }
            };
            bit(stest::kReturnCauseEpoch, "epoch");
            bit(stest::kReturnCauseTerminate, "terminate");
            bit(stest::kReturnCauseRunnable, "runnable");
            bit(stest::kReturnCauseTimeout, "timeout");
            std::fprintf(f, "]");
        }
        std::fprintf(f, "}%s\n", i + 1 < events.size() ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
    return true;
}

// Shape matchers (the deterministic-sequence self-checks that anchor the
// canonical fixtures).
bool is_commit(const TraceEvent& ev, unsigned worker, bool armed) {
    return static_cast<stest::TraceEventKind>(ev.kind) ==
               stest::TraceEventKind::park_committed &&
           ev.worker == worker && (ev.armed != 0) == armed;
}
bool is_entered(const TraceEvent& ev, unsigned worker) {
    return static_cast<stest::TraceEventKind>(ev.kind) ==
               stest::TraceEventKind::park_entered &&
           ev.worker == worker;
}
bool is_wake(const TraceEvent& ev, stest::WakeCause cause) {
    return static_cast<stest::TraceEventKind>(ev.kind) ==
               stest::TraceEventKind::wake_published &&
           ev.cause == static_cast<unsigned char>(cause);
}
bool is_return(const TraceEvent& ev, unsigned worker, bool immediate,
               std::uint16_t causes) {
    return static_cast<stest::TraceEventKind>(ev.kind) ==
               stest::TraceEventKind::park_returned &&
           ev.worker == worker && (ev.immediate != 0) == immediate &&
           ev.return_causes == causes;
}
bool is_refused(const TraceEvent& ev, unsigned worker) {
    return static_cast<stest::TraceEventKind>(ev.kind) ==
               stest::TraceEventKind::park_refused &&
           ev.worker == worker;
}

std::size_t count_kind(const std::vector<TraceEvent>& ev,
                       stest::TraceEventKind k) {
    std::size_t n = 0;
    for (const TraceEvent& e : ev) {
        if (static_cast<stest::TraceEventKind>(e.kind) == k) ++n;
    }
    return n;
}

}  // namespace

// ---- T1: unarmed park, external wake while blocked (split-wait) -----------
SLUICE_TEST_CASE(e9_trace_t1_unarmed_park_external_wake) {
    if constexpr (!sa::fiber_ctx::supported) return;

    sa::AsyncIoContext ctx(std::make_unique<sa::ThreadPoolBackend>());
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);
    sa::SchedulerWakeHandle wh = sched.make_wake_handle();

    sa::Event ev(sched, /*initially_set=*/false);
    sa::WaitNode node;
    std::atomic<bool> fiber_done{false};

    Fiber f;
    f.set_entry([&](Fiber&) {
        ev.wait(node);  // waitq registration: external-wake-capable, UNARMED park
        fiber_done.store(true, std::memory_order_release);
    });
    FiberStack fs;
    SLUICE_CHECK(sched.init_fiber(f, fs.base(), fs.size()));
    sched.spawn(f);  // pre-run: pending_spawn_, no wake publication

    // Hold the worker right AFTER its post-park return (before it can re-park)
    // so the capture window closes deterministically.
    stest::WorkerParkReturnSeam::arm(sched);

    stest::E9TraceRecorder::enable(sched);
    std::thread runner([&] { sched.run_live(1); });
    // #210: wh.notify() below is checked while the runner is joinable — the
    // guard makes that (and any other) early exit release the armed
    // post-park seam, resolve the Event, and join instead of terminating.
    RunnerCleanup cleanup(runner, [&] {
        stest::WorkerParkReturnSeam::release(sched);
        stest::E9TraceRecorder::disable(sched);
        ev.set();
    });

    // The single worker: runs f -> suspends on the Event -> classifies MW-S3
    // (Live + external-wake-capable) -> commits + enters the UNARMED park.
    std::vector<TraceEvent> evs = wait_trace(
        sched,
        [](const std::vector<TraceEvent>& v) {
            return v.size() >= 2 &&
                   static_cast<stest::TraceEventKind>(v[1].kind) ==
                       stest::TraceEventKind::park_entered;
        },
        "T1 ParkCommitted+ParkEntered");

    // Publish the external wake. record_entered and cv.wait run under ONE
    // continuous wake_mtx_ hold, so this notify — which takes wake_mtx_ —
    // serializes strictly after the worker is inside the wait: the return is
    // a blocking-park return by construction, not by timing.
    SLUICE_CHECK(wh.notify());

    evs = wait_trace(
        sched,
        [](const std::vector<TraceEvent>& v) {
            return !v.empty() &&
                   static_cast<stest::TraceEventKind>(v.back().kind) ==
                       stest::TraceEventKind::park_returned;
        },
        "T1 ParkReturned");
    stest::E9TraceRecorder::disable(sched);
    stest::WorkerParkReturnSeam::release(sched);  // disarm; the re-park is free

    // Resolve the Event from the coordinator thread; the run converges.
    ev.set();
    runner.join();

    // Deterministic shape assertion (the canonical fixture mirror).
    SLUICE_CHECK(!stest::E9TraceRecorder::overflow(sched));
    SLUICE_CHECK(evs.size() == 4);
    if (evs.size() == 4) {
        SLUICE_CHECK(is_commit(evs[0], /*worker=*/0, /*armed=*/false));
        SLUICE_CHECK(is_entered(evs[1], 0));
        SLUICE_CHECK(is_wake(evs[2], stest::WakeCause::external_notify));
        SLUICE_CHECK(is_return(evs[3], 0, /*immediate=*/false,
                               stest::kReturnCauseEpoch));
        SLUICE_CHECK(evs[2].epoch > evs[0].epoch);  // the notify advanced it
    }
    SLUICE_CHECK(fiber_done.load(std::memory_order_acquire));
    SLUICE_CHECK(f.state() == FiberState::done);
    (void)write_trace_json("t1_unarmed_park_external_wake", /*split_wait=*/true,
                           "live", "external_wait_registered", evs);
}

// ---- T2: wake inside the commit→wait window (post-baseline seam) ----------
SLUICE_TEST_CASE(e9_trace_t2_wake_races_park_commit) {
    if constexpr (!sa::fiber_ctx::supported) return;

    sa::AsyncIoContext ctx(std::make_unique<sa::ThreadPoolBackend>());
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);
    sa::SchedulerWakeHandle wh = sched.make_wake_handle();

    sa::Event ev(sched, false);
    sa::WaitNode node;
    Fiber f;
    f.set_entry([&](Fiber&) { ev.wait(node); });
    FiberStack fs;
    SLUICE_CHECK(sched.init_fiber(f, fs.base(), fs.size()));
    sched.spawn(f);

    // Hold 1: strictly post-baseline, pre-cv.wait (the #115 seam).
    stest::SchedulerParkBaselineSeam::arm(sched);
    // Hold 2: post-return, pre-re-park (window close).
    stest::WorkerParkReturnSeam::arm(sched);

    stest::E9TraceRecorder::enable(sched);
    std::thread runner([&] { sched.run_live(1); });
    // #210: wh.notify() below is checked while the runner is joinable — the
    // guard makes that (and any other) early exit release both armed seams,
    // resolve the Event, and join instead of terminating.
    RunnerCleanup cleanup(runner, [&] {
        stest::SchedulerParkBaselineSeam::release(sched);
        stest::WorkerParkReturnSeam::release(sched);
        stest::E9TraceRecorder::disable(sched);
        ev.set();
    });

    // The worker committed its park: baseline recorded, cv NOT yet entered.
    stest::SchedulerParkBaselineSeam::wait_paused(sched);

    // THE LOAD-BEARING PUBLICATION: strictly inside the commit→wait window.
    // Its only possible transport is the cv predicate (the baseline already
    // absorbed everything earlier).
    SLUICE_CHECK(wh.notify());

    // Release: the predicate is true at wait entry -> immediate return.
    stest::SchedulerParkBaselineSeam::release(sched);
    stest::WorkerParkReturnSeam::wait_paused(sched);
    std::vector<TraceEvent> evs = stest::E9TraceRecorder::events(sched);
    stest::E9TraceRecorder::disable(sched);
    stest::WorkerParkReturnSeam::release(sched);

    ev.set();
    runner.join();

    SLUICE_CHECK(!stest::E9TraceRecorder::overflow(sched));
    SLUICE_CHECK(evs.size() == 4);
    if (evs.size() == 4) {
        SLUICE_CHECK(is_commit(evs[0], 0, false));
        SLUICE_CHECK(is_wake(evs[1], stest::WakeCause::external_notify));
        SLUICE_CHECK(is_entered(evs[2], 0));
        SLUICE_CHECK(is_return(evs[3], 0, /*immediate=*/true,
                               stest::kReturnCauseEpoch));
    }
    SLUICE_CHECK(f.state() == FiberState::done);
    (void)write_trace_json("t2_wake_races_park_commit", /*split_wait=*/true,
                           "live", "external_wait_registered", evs);
}

// ---- T3: entry-armed reference park, observation-timeout return -----------
SLUICE_TEST_CASE(e9_trace_t3_armed_park_observation_return) {
    if constexpr (!sa::fiber_ctx::supported) return;

    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);
    sa::SchedulerWakeHandle wh = sched.make_wake_handle();

    std::atomic<bool> flag{false};
    std::atomic<bool> fiber_done{false};
    Fiber f;
    f.set_entry([&](Fiber&) {
        // A ready-flag wait registers waiting_ready_ -> the MW-S3 park is
        // ENTRY-ARMED (E5-A2 observation, scheduler.cpp:1188's
        // ready_flag_observation) — the #185 observationArmed shape.
        sched.await_ready_flag(flag);
        fiber_done.store(true, std::memory_order_release);
    });
    FiberStack fs;
    SLUICE_CHECK(sched.init_fiber(f, fs.base(), fs.size()));
    sched.spawn(f);

    stest::WorkerParkReturnSeam::arm(sched);

    stest::E9TraceRecorder::enable(sched);
    std::thread runner([&] { sched.run_live(1); });

    // The armed park blocks at the 2 ms observation interval with NOTHING
    // else able to wake it; the timeout expires deterministically (no wake
    // event ever fires in this window).
    stest::WorkerParkReturnSeam::wait_paused(sched);
    std::vector<TraceEvent> evs = stest::E9TraceRecorder::events(sched);
    stest::E9TraceRecorder::disable(sched);
    stest::WorkerParkReturnSeam::release(sched);

    // Resolve the flag + notify; the run converges.
    flag.store(true, std::memory_order_release);
    (void)wh.notify();
    runner.join();

    SLUICE_CHECK(!stest::E9TraceRecorder::overflow(sched));
    SLUICE_CHECK(evs.size() == 3);
    if (evs.size() == 3) {
        SLUICE_CHECK(is_commit(evs[0], 0, /*armed=*/true));
        SLUICE_CHECK(is_entered(evs[1], 0));
        SLUICE_CHECK(is_return(evs[2], 0, /*immediate=*/false,
                               stest::kReturnCauseTimeout));
    }
    SLUICE_CHECK(count_kind(evs, stest::TraceEventKind::wake_published) == 0);
    SLUICE_CHECK(fiber_done.load(std::memory_order_acquire));
    SLUICE_CHECK(f.state() == FiberState::done);
    (void)write_trace_json("t3_armed_park_observation_return",
                           /*split_wait=*/false, "live",
                           "external_wait_registered", evs);
}

// ---- T4: unarmed MW-S2 non-participant park, terminate-caused return -----
SLUICE_TEST_CASE(e9_trace_t4_unarmed_park_terminate_return) {
    if constexpr (!sa::fiber_ctx::supported) return;

    auto backend = std::make_unique<sa::FakeAsyncBackend>();
    sa::FakeAsyncBackend* backend_ptr = backend.get();
    sa::AsyncIoContext ctx(std::move(backend));
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);

    // One accepted-but-never-completed op: outstanding == 1 -> MW-S2, with a
    // legacy backend whose wait_one returns 0 promptly (no progress).
    sa::Completion<std::size_t> held;
    std::byte buf[4]{};
    SLUICE_CHECK(
        ctx.submit_read(sa::ReadOp{-1, buf, sizeof(buf), 0}, held).has_value());

    // Hold the MW-S2 participant (worker 0) after its commit, before its
    // wait_one. Worker 1 is held at the candidate seam UNTIL worker 0's
    // commit has landed (backend_wait_active_ = the G1 observer): otherwise
    // worker 1's early fall-through park recheck could see unguarded backend
    // progress (admission still none) and refuse — a legal but different
    // schedule. The pilot pins the parking schedule.
    stest::arm(sched, stest::PhaseTag::mw_s2_committed_before_wait_one);
    stest::Issue161CandidateSeam::arm_for_worker(sched, /*worker_id=*/1);
    // Freeze worker 1 right after its parked return (before it can re-loop,
    // break, and run its own retire epilogue — whose wake would race the
    // window close).
    stest::WorkerParkReturnSeam::arm(sched);

    stest::E9TraceRecorder::enable(sched);
    std::thread runner([&] { sched.run(2); });  // Drain mode
    // #210: SLUICE_CHECK(w1_parked) below fires while the runner is joinable;
    // on failure it recorded the primary progress/timeout failure and
    // returned, and ~thread() then called std::terminate() — the secondary
    // cleanup failure that masked the real report. The guard instead
    // releases every seam this case armed (the mw_s2 release lets the
    // participant's non-blocking wait_one() publish global_terminate_ and
    // retire; the park-return release — deliberately NOT covered by
    // release_all_phases — frees a held survivor), joins the runner, and
    // then drains the held op so the Completion/context destruction
    // contracts hold (mirroring the normal path's post-join drain).
    RunnerCleanup cleanup(
        runner,
        [&] {
            stest::release(
                sched, stest::PhaseTag::mw_s2_committed_before_wait_one);
            stest::Issue161CandidateSeam::release(sched);
            stest::WorkerParkReturnSeam::release(sched);
            stest::E9TraceRecorder::disable(sched);
        },
        [&] {
            // Same drain as the normal path: outstanding==0 before context
            // destruction, `held` ends ready.
            backend_ptr->complete_oldest_with_bytes(0);
            (void)ctx.poll();
        });

    // Worker 0 is the committed participant paused pre-wait_one.
    stest::wait_paused(sched, stest::PhaseTag::mw_s2_committed_before_wait_one);
    // Now worker 1 may park (the participant is a standing observer).
    stest::Issue161CandidateSeam::release(sched);
    // Worker 1 (non-participant) parks unarmed at :1188 (MW-S2, no external
    // wait, no ready-flag wait -> UNARMED unbounded park).
    std::vector<TraceEvent> evs = wait_trace(
        sched,
        [](const std::vector<TraceEvent>& v) {
            return v.size() >= 2 &&
                   static_cast<stest::TraceEventKind>(v[1].kind) ==
                       stest::TraceEventKind::park_entered;
        },
        "T4 worker-1 park");
    const bool w1_parked = evs.size() >= 2 &&
                           is_entered(evs[1], /*worker=*/1) &&
                           is_commit(evs[0], 1, /*armed=*/false);
    if (!w1_parked) {
        // #210 fail-path forensics: report WHAT the recorder actually saw
        // (which worker parked / refused / never committed) and where the
        // seams hold the workers, BEFORE the early return below hands over
        // to the fail-safe cleanup guard.
        dump_trace("T4 worker-1 park miss", evs);
        dump_seam_states(sched);
    }
    SLUICE_CHECK(w1_parked);

    // Release the participant: its wait_one returns 0 (no progress), it
    // publishes global_terminate_ + signals, breaks, and its retire epilogue
    // signals again. Worker 1's parked predicate (epoch / terminate) fires.
    stest::release(sched, stest::PhaseTag::mw_s2_committed_before_wait_one);

    // Deterministic window close: wait for BOTH terminal events (the parked
    // return AND the retiring participant's epilogue wake — either order),
    // then confirm worker 1 is frozen at the post-return seam.
    evs = wait_trace(
        sched,
        [](const std::vector<TraceEvent>& v) {
            bool returned = false, retired_w0 = false;
            for (const TraceEvent& e : v) {
                if (static_cast<stest::TraceEventKind>(e.kind) ==
                    stest::TraceEventKind::park_returned) {
                    returned = true;
                }
                if (is_wake(e, stest::WakeCause::retire_epilogue) &&
                    e.worker == 0) {
                    retired_w0 = true;
                }
            }
            return returned && retired_w0;
        },
        "T4 terminate wake + return + retire");
    // #210: this wait is reachable AFTER the bounded wait_trace above timed
    // out (a timeout does NOT early-return) — the unbounded wait_paused here
    // would convert the old std::terminate into a permanent hang. Bounded:
    // a miss dumps the trace + seam states, records the failure, and returns
    // through the fail-safe cleanup guard.
    if (!wait_paused_bounded(sched, stest::PhaseTag::worker_park_returned,
                             "T4 worker-1 post-return")) {
        dump_trace("T4 worker-1 post-return pause miss", evs);
        dump_seam_states(sched);
        SLUICE_FAIL("T4: worker 1 never paused at WorkerParkReturnSeam");
    }
    stest::E9TraceRecorder::disable(sched);
    stest::WorkerParkReturnSeam::release(sched);
    runner.join();

    // Drain the intentionally-held op so the context's L11 outstanding==0
    // assertion holds at destruction (test scaffolding, external_wake_test's
    // HoldingBackend discipline).
    backend_ptr->complete_oldest_with_bytes(0);
    (void)ctx.poll();

    // The parked return and the retiring participant's epilogue wake race
    // after the terminate signal — both orders are legal; both validate
    // against the model (the canonical fixtures carry both).
    SLUICE_CHECK(!stest::E9TraceRecorder::overflow(sched));
    SLUICE_CHECK(evs.size() == 5);
    bool order_a = false, order_b = false;
    if (evs.size() == 5 && w1_parked) {
        SLUICE_CHECK(is_commit(evs[0], 1, false));
        SLUICE_CHECK(is_entered(evs[1], 1));
        if (is_wake(evs[2], stest::WakeCause::terminate)) {
            if (is_return(evs[3], 1, false,
                          stest::kReturnCauseEpoch |
                              stest::kReturnCauseTerminate) &&
                is_wake(evs[4], stest::WakeCause::retire_epilogue) &&
                evs[4].worker == 0) {
                order_a = true;  // return before the epilogue wake
            } else if (is_wake(evs[3], stest::WakeCause::retire_epilogue) &&
                       evs[3].worker == 0 &&
                       is_return(evs[4], 1, false,
                                 stest::kReturnCauseEpoch |
                                     stest::kReturnCauseTerminate)) {
                order_b = true;  // epilogue wake before the return
            }
        }
    }
    SLUICE_CHECK(order_a || order_b);
    const char* name =
        order_a ? "t4a_unarmed_park_terminate_return_retire_after"
                : "t4b_unarmed_park_terminate_return_retire_before";
    (void)write_trace_json(name, /*split_wait=*/false, "drain",
                           "backend_outstanding", evs);
}

// ---- T5: pre-baseline publication refuses the park (G1, #115 law) --------
SLUICE_TEST_CASE(e9_trace_t5_prebaseline_publication_refuses_park) {
    if constexpr (!sa::fiber_ctx::supported) return;

    sa::AsyncIoContext ctx(std::make_unique<sa::FakeAsyncBackend>());
    Scheduler sched(ctx);
    stest::ControllerGuard ctrl(sched);

    sa::Event ev(sched, false);
    sa::WaitNode node;
    Rendezvous hold_f2;
    std::atomic<bool> f2_started{false};
    std::atomic<bool> f2_ran{false};
    Fiber f_ext, f2;
    f_ext.set_entry([&](Fiber&) { ev.wait(node); });
    f2.set_entry([&](Fiber&) {
        // f2_started proves the REFUSING worker re-looped and popped f2 (the
        // running-observer state that keeps the window stable); the
        // rendezvous then holds it there until the window has closed.
        f2_started.store(true, std::memory_order_release);
        hold_f2.wait();
        f2_ran.store(true, std::memory_order_release);
    });
    FiberStack s0, s2;
    SLUICE_CHECK(sched.init_fiber(f_ext, s0.base(), s0.size()));
    SLUICE_CHECK(sched.init_fiber(f2, s2.base(), s2.size()));
    sched.spawn(f_ext);  // pre-run: f_ext -> worker 0

    // Hold worker 1 BEFORE its park attempt (candidate seam, per-worker), so
    // only worker 0's park/refusal is in the window.
    stest::Issue161CandidateSeam::arm_for_worker(sched, /*worker_id=*/1);
    // Hold worker 0 at the park-commit boundary: strictly PRE-baseline.
    stest::SchedulerParkSeam::arm_commit(sched);

    stest::E9TraceRecorder::enable(sched);
    std::thread runner([&] { sched.run_live(2); });
    // #210: the two wait_flag SLUICE_CHECKs below fire while the runner is
    // joinable — the guard makes those early exits release the (normally
    // already-released) seams, release the f2 rendezvous, resolve the Event,
    // and join instead of terminating.
    RunnerCleanup cleanup(runner, [&] {
        stest::Issue161CandidateSeam::release(sched);
        stest::SchedulerParkSeam::release_commit(sched);
        stest::E9TraceRecorder::disable(sched);
        hold_f2.release();
        ev.set();
    });

    // Worker 0: ran f_ext (suspended on the Event), classified MW-S3
    // (Live + external-wake-capable) and sits at its park-commit boundary.
    stest::SchedulerParkSeam::wait_commit_paused(sched);
    // Worker 1 must have finished its loop-top steal pass and sit at the
    // candidate seam BEFORE the publication — otherwise its steal would
    // consume the ticket and the refusal could not happen (the steal IS the
    // correct production behavior; the pilot pins the refusing schedule).
    stest::Issue161CandidateSeam::wait_paused(sched);

    // THE LOAD-BEARING PUBLICATION: strictly BEFORE the baseline, onto the
    // parking worker's OWN inbox. The G1 recheck is its only transport.
    sched.spawn_on(f2, /*worker_id=*/0);

    // Release: worker 0's G1 recheck sees its own inbox non-empty -> REFUSE
    // (+ refusal signal), re-loop, pop f2, run it (f2 blocks on the
    // rendezvous — no further park events from worker 0 in the window).
    stest::SchedulerParkSeam::release_commit(sched);

    std::vector<TraceEvent> evs = wait_trace(
        sched,
        [](const std::vector<TraceEvent>& v) {
            std::size_t refuses = 0, wakes = 0;
            for (const TraceEvent& e : v) {
                if (static_cast<stest::TraceEventKind>(e.kind) ==
                    stest::TraceEventKind::park_refused) {
                    ++refuses;
                }
                if (is_wake(e, stest::WakeCause::park_refuse)) ++wakes;
            }
            return refuses >= 1 && wakes >= 1;
        },
        "T5 refusal + refusal signal");
    stest::E9TraceRecorder::disable(sched);

    // Close the window BEFORE worker 1 parks (it is held at the candidate
    // seam; its park — a model-unmapped delegation-class park — is
    // intentionally outside the capture).
    stest::Issue161CandidateSeam::release(sched);
    stest::SchedulerParkSeam::release_commit(sched);  // disarm for worker 1

    if (!wait_flag(f2_started, kWatchdog)) {
        // #210 fail-path forensics: f2 never ran — report what the window
        // captured and where the seams hold the workers before returning.
        dump_trace("T5 f2_started miss", evs);
        dump_seam_states(sched);
        SLUICE_FAIL("T5: f2 never started after the refusal window closed");
    }
    hold_f2.release();
    if (!wait_flag(f2_ran, kWatchdog)) {
        dump_trace("T5 f2_ran miss", evs);
        dump_seam_states(sched);
        SLUICE_FAIL("T5: f2 never completed after the rendezvous release");
    }
    ev.set();
    runner.join();

    SLUICE_CHECK(!stest::E9TraceRecorder::overflow(sched));
    SLUICE_CHECK(evs.size() == 3);
    if (evs.size() == 3) {
        SLUICE_CHECK(is_wake(evs[0], stest::WakeCause::runnable_route));
        SLUICE_CHECK(is_refused(evs[1], 0));
        SLUICE_CHECK(is_wake(evs[2], stest::WakeCause::park_refuse));
    }
    SLUICE_CHECK(f2_ran.load(std::memory_order_acquire));
    SLUICE_CHECK(f_ext.state() == FiberState::done);
    SLUICE_CHECK(f2.state() == FiberState::done);
    (void)write_trace_json("t5_prebaseline_publication_refuses_park",
                           /*split_wait=*/false, "live",
                           "external_wait_registered", evs);
}

SLUICE_MAIN()
