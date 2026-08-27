// dst_pv1_schedule_driver_test — DST-PV-1 proof-of-value experiments.
//
// Exercises the TEST-ONLY deterministic next-runnable-choice seam (the
// schedule script installed over the worker_loop pop site) with manually
// supplied replay vectors. NO search, NO seeds, NO random scheduler, NO
// permutation exploration, NO trace database — the question is whether a
// small explicit decision vector, combined with the EXISTING controllable
// seams (fake I/O completion, advance_clock, cancel_wait), yields materially
// better deterministic schedule expression than manual choreography.
//
// Cases:
//   T1  exact runnable selection: 3 legal runnable participants; the script
//       controls the order; repeated 20x with the identical semantic trace.
//   T2  replay: the same vector rerun on a fresh fixture yields the identical
//       semantic trace; a one-step-different vector yields a different (but
//       itself reproducible) trace.
//   T3  illegal decision: a script requesting a non-runnable participant
//       aborts with the deterministic diagnostic package (death child with
//       stderr capture; the parent asserts the package fields), and never
//       falls back to another runnable. Control child: a legal script exits 0.
//   T4  cross-domain: one vector spanning scheduler choice + logical deadline
//       + fake I/O completion + wait cancellation; transposing ONE step pair
//       flips the cancel-vs-expiry precedence deterministically, both ways.
//   T5  AC-2d-relevant falsification: the AsyncQueue admission paths. Forces
//       every legal interleaving around the registration -> suspend window
//       (close-before-admit, resource-before-admit, already-due, documented
//       reconcilers after suspend, close after suspend) and — the sharpest
//       probe — a PARKED PRODUCER vs a blocking pop's inline success path
//       (the AC-2a matrix documents try_pop/close as the ONLY producer
//       reconcilers; whether queue_pop_admit's inline path also reconciles
//       is exactly the drift specimen this campaign makes executable). The
//       V1 assertion is a KNOWN-DRIFT CHARACTERIZATION WITNESS: it proves
//       the as-built defect exists today; a future Queue repair slice must
//       consciously flip or replace that expectation. NO production Queue
//       change is made in this campaign.
//   T6  harness contract (review P1-2): an Invoke action may re-enter the
//       test control surface (uninstall) without self-deadlock. Plus the
//       review P1-4 death children inside T3: out-of-range driver ids and
//       unregistered actions abort loudly instead of corrupting memory.
//
// All ordering evidence is script steps + scheduler semantic state (node
// outcomes, Completion results, queue result statuses, event lists). NO
// sleep_for ordering, NO coordinator threads, NO in-fiber atomic
// choreography: run(1) executes inline on the calling thread.
#include "harness.hpp"
#include "async_test_control.hpp"
#include "dst_schedule_driver.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/async_queue.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <string>
#include <vector>
#include <vector>

#if defined(__unix)
#include "death_test_runner_posix.hpp"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#endif

using namespace sluice::async;
using sluice::Result;
using sluice_async_test::ControllerGuard;
using sluice_async_test::TimerTestControl;

namespace {

// 64 KiB scratch stack for a fiber. 16-byte aligned.
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// A backend that never completes anything by itself (outstanding stays 0), so
// primitive-wait runs classify MW-S3 (STALLED, drain mode returns) rather
// than MW-S2; run(1) returns cleanly when a script leaves a fiber suspended.
class IdleBackend : public AsyncBackend {
public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override {
        return {};
    }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override {
        return {};
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override {
        return {};
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override {
        return {};
    }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return 0; }
    void cancel(Completion<std::size_t>&) override {}
    void cancel(Completion<void>&) override {}
    std::size_t outstanding() const noexcept override { return 0; }
};

}  // namespace

// ============================================================================
// T1 — exact runnable selection among 3 legal participants, repeated.
// ============================================================================
SLUICE_TEST_CASE(dst_t1_exact_runnable_selection) {
    if constexpr (!fiber_ctx::supported) return;

    // Default FIFO spawn order would trace "ABC". The script demands "BCA"
    // and, on alternating repeats, "CBA" — proving the order is CHOSEN, not
    // inherited from spawn order, and identical across 20 repetitions.
    for (int rep = 0; rep < 20; ++rep) {
        const bool forward = (rep % 2 == 0);
        AsyncIoContext ctx(std::make_unique<IdleBackend>());
        Scheduler sched(ctx);
        ControllerGuard ctrl(sched);
        sluice_dst::DstScheduleDriver driver(
            sched, "dst_t1_exact_runnable_selection");

        std::string trace;
        Fiber fa, fb, fc;
        fa.set_entry([&](Fiber&) { trace += "A"; });
        fb.set_entry([&](Fiber&) { trace += "B"; });
        fc.set_entry([&](Fiber&) { trace += "C"; });
        FiberStack sa, sb, sc;
        SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
        SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));
        SLUICE_CHECK(sched.init_fiber(fc, sc.base(), sc.size()));

        driver.bind(0, fa).bind(1, fb).bind(2, fc);
        if (forward) {
            driver.run(1).run(2).run(0);  // B, C, A
        } else {
            driver.run(2).run(1).run(0);  // C, B, A
        }
        driver.arm();

        sched.spawn(fa);
        sched.spawn(fb);
        sched.spawn(fc);
        sched.run(1);

        const std::string expect = forward ? "BCA" : "CBA";
        SLUICE_CHECK_MSG(trace == expect,
                         ("rep " + std::to_string(rep) +
                          ": script chose the exact order (trace=" + trace +
                          ")")
                             .c_str());
        SLUICE_CHECK(fa.state() == FiberState::done);
        SLUICE_CHECK(fb.state() == FiberState::done);
        SLUICE_CHECK(fc.state() == FiberState::done);
    }
}

// ============================================================================
// T2/T4 fixture — fiber A: fake read + await; fiber B: deadline wait at 50.
//
// Driver vocabulary over the EXISTING seams:
//   Run(A)/Run(B) — next-runnable choice (the ONLY new primitive)
//   Clock(t)      — sched.advance_clock(t) (logical clock; pumps internally)
//   Io            — fake complete_oldest_with_bytes(4) (staged; applied by
//                   the scheduler's next drain)
//   Cancel(B)     — sched.cancel_wait(qb, nb) (existing cancel seam)
// ============================================================================
namespace {

struct CrossFixture {
    explicit CrossFixture(const char* test_name)
        : backend_up(std::make_unique<FakeAsyncBackend>()),
          backend(backend_up.get()),
          ctx(std::move(backend_up)),
          sched(ctx),
          ctrl(sched),
          driver(sched, test_name) {
        TimerTestControl::enable_test_clock(sched);
    }

    std::unique_ptr<FakeAsyncBackend> backend_up;
    FakeAsyncBackend* backend;
    AsyncIoContext ctx;
    Scheduler sched;
    ControllerGuard ctrl;
    sluice_dst::DstScheduleDriver driver;

    Fiber fa, fb;
    FiberStack sa, sb;
    Completion<std::size_t> a_c;
    std::byte a_buf[4]{};
    WaitQueue qb;
    WaitNode nb;
    bool cancel_result = false;

    // Semantic events recorded by the fibers (assertion evidence).
    std::vector<std::string> events;

    void build_fibers() {
        fa.set_entry([this](Fiber&) {
            events.push_back("A:submit");
            (void)ctx.submit_read(ReadOp{-1, a_buf, 4, 0}, a_c);
            (void)sched.await_completion_size(a_c);
            const std::size_t n =
                a_c.ready() ? a_c.result().value_or(0) : 0;
            events.push_back("A:got:" + std::to_string(n));
        });
        fb.set_entry([this](Fiber&) {
            events.push_back("B:wait");
            (void)sched.await_wait_deadline(qb, nb,
                                            Scheduler::deadline_t{50});
            if (nb.was_expired()) {
                events.push_back("B:expired");
            } else if (nb.was_cancelled()) {
                events.push_back("B:cancelled");
            } else if (nb.was_woken()) {
                events.push_back("B:woken");
            } else {
                events.push_back("B:???");
            }
        });
        SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
        SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));
        driver.bind(0, fa).bind(1, fb);
    }

    void arm_common_actions() {
        Scheduler* sp = &sched;
        FakeAsyncBackend* bp = backend;
        WaitQueue* qp = &qb;
        WaitNode* np = &nb;
        bool* cancel_seen = &cancel_result;
        driver.on_action(0, "Clock(60)", [sp](Scheduler&) {
            sp->advance_clock(Scheduler::deadline_t{60});
        });
        driver.on_action(1, "Io(4)", [bp](Scheduler&) {
            bp->complete_oldest_with_bytes(4);
        });
        driver.on_action(2, "Cancel(B)", [sp, qp, np, cancel_seen](Scheduler&) {
            *cancel_seen = sp->cancel_wait(*qp, *np);
        });
        driver.on_action(3, "Clock(45)", [sp](Scheduler&) {
            sp->advance_clock(Scheduler::deadline_t{45});
        });
    }
};

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) {
        if (!out.empty()) out += ",";
        out += s;
    }
    return out;
}

}  // namespace

// ============================================================================
// T2 — replay: identical vectors produce identical semantic traces; a
//      one-step-different vector produces a reproducibly different trace.
// ============================================================================
SLUICE_TEST_CASE(dst_t2_replay) {
    if constexpr (!fiber_ctx::supported) return;

    // Vector R1: Run(B) -> Run(A) -> Clock(60) -> Io(4)
    //   B registers its deadline wait and suspends; A submits and suspends;
    //   the clock pump expires B (deadline 50 <= 60) and B resumes Expired
    //   via FIFO; the staged I/O is applied by the drain and A resumes 4.
    // Vector R2 (one different step): Run(B) -> Run(A) -> Io(4) -> Clock(45)
    //   The I/O completes BEFORE any expiry; the clock stays below B's
    //   deadline, so B remains suspended when the drain-mode run STALLS; the
    //   test then cancels B post-run and reruns — itself fully scripted.
    std::vector<std::string> r1_first, r1_second, r2_first, r2_second;
    {
        CrossFixture fx("dst_t2_replay_r1_a");
        fx.build_fibers();
        fx.arm_common_actions();
        fx.driver.run(1).run(0).invoke(0).invoke(1);  // Run(B),Run(A),Clock,Io
        fx.driver.arm();
        fx.sched.spawn(fx.fa);
        fx.sched.spawn(fx.fb);
        fx.sched.run(1);
        r1_first = fx.events;
        SLUICE_CHECK(fx.fa.state() == FiberState::done);
        SLUICE_CHECK(fx.fb.state() == FiberState::done);
    }
    {
        CrossFixture fx("dst_t2_replay_r1_b");
        fx.build_fibers();
        fx.arm_common_actions();
        fx.driver.run(1).run(0).invoke(0).invoke(1);
        fx.driver.arm();
        fx.sched.spawn(fx.fa);
        fx.sched.spawn(fx.fb);
        fx.sched.run(1);
        r1_second = fx.events;
    }
    SLUICE_CHECK_MSG(r1_first == r1_second,
                     ("R1 replay diverged: " + join(r1_first) + " vs " +
                      join(r1_second))
                         .c_str());
    SLUICE_CHECK_MSG(
        join(r1_first) == "B:wait,A:submit,B:expired,A:got:4",
        ("R1 semantic trace: " + join(r1_first)).c_str());

    for (int rep = 0; rep < 2; ++rep) {
        CrossFixture fx("dst_t2_replay_r2");
        fx.build_fibers();
        fx.arm_common_actions();
        // Run(B) -> Run(A) -> Io(4) -> Clock(45): the I/O completes before
        // any expiry and the clock stays below B's deadline, so B remains
        // suspended when the drain-mode run STALLS; the post-run segment
        // (cancel + rerun) is itself deterministic main-thread scripting.
        fx.driver.run(1).run(0).invoke(1).invoke(3);
        fx.driver.arm();
        fx.sched.spawn(fx.fa);
        fx.sched.spawn(fx.fb);
        fx.sched.run(1);
        // B (deadline 50) is still suspended: clock 45 < 50 and no resolver.
        SLUICE_CHECK(fx.fb.state() == FiberState::waiting);
        SLUICE_CHECK(fx.fa.state() == FiberState::done);
        // Post-run segment (still deterministic, main-thread scripted):
        // cancel B and rerun.
        (void)fx.sched.cancel_wait(fx.qb, fx.nb);
        fx.sched.run(1);
        SLUICE_CHECK(fx.fb.state() == FiberState::done);
        if (rep == 0) {
            r2_first = fx.events;
        } else {
            r2_second = fx.events;
        }
    }    SLUICE_CHECK_MSG(r2_first == r2_second,
                     ("R2 replay diverged: " + join(r2_first) + " vs " +
                      join(r2_second))
                         .c_str());
    SLUICE_CHECK_MSG(join(r2_first) != join(r1_first),
                     "one-step difference must change the semantic trace");
}

// ============================================================================
// T4 — cross-domain precedence flip: Clock <-> Cancel transposition.
// ============================================================================
SLUICE_TEST_CASE(dst_t4_cross_domain_precedence) {
    if constexpr (!fiber_ctx::supported) return;

    // EXPIRY wins: the pump fires before any cancel; the later cancel is a
    // terminal no-op (exactly-once terminal winner) and reports false.
    std::vector<std::string> ev_expiry;
    bool cancel_after_expiry = true;
    {
        CrossFixture fx("dst_t4_expiry_wins");
        fx.build_fibers();
        fx.arm_common_actions();
        fx.driver.run(1).run(0).invoke(0).invoke(1).invoke(2);
        fx.driver.arm();
        fx.sched.spawn(fx.fa);
        fx.sched.spawn(fx.fb);
        fx.sched.run(1);
        ev_expiry = fx.events;
        cancel_after_expiry = fx.cancel_result;
        SLUICE_CHECK(fx.fa.state() == FiberState::done);
        SLUICE_CHECK(fx.fb.state() == FiberState::done);
    }
    SLUICE_CHECK_MSG(join(ev_expiry) == "B:wait,A:submit,B:expired,A:got:4",
                     ("expiry-wins trace: " + join(ev_expiry)).c_str());
    SLUICE_CHECK_MSG(!cancel_after_expiry,
                     "cancel after a terminal expiry is a no-op (false)");

    // CANCEL wins: the cancel resolves B first; the later clock advance finds
    // the registration already RETIRED (nothing due; no second terminal).
    std::vector<std::string> ev_cancel;
    {
        CrossFixture fx("dst_t4_cancel_wins");
        fx.build_fibers();
        fx.arm_common_actions();
        fx.driver.run(1).run(0).invoke(2).invoke(1).invoke(0);
        fx.driver.arm();
        fx.sched.spawn(fx.fa);
        fx.sched.spawn(fx.fb);
        fx.sched.run(1);
        ev_cancel = fx.events;
        SLUICE_CHECK(fx.fa.state() == FiberState::done);
        SLUICE_CHECK(fx.fb.state() == FiberState::done);
        SLUICE_CHECK_MSG(
            TimerTestControl::active_deadline_count(fx.sched) == 0,
            "cancel-side retire decremented the ACTIVE count exactly once");
    }
    SLUICE_CHECK_MSG(join(ev_cancel) == "B:wait,A:submit,B:cancelled,A:got:4",
                     ("cancel-wins trace: " + join(ev_cancel)).c_str());
}

// ============================================================================
// T5 — AC-2d falsification: AsyncQueue admission-path interleavings.
// ============================================================================
namespace {

struct QueueFixture {
    explicit QueueFixture(const char* test_name)
        : ctx(std::make_unique<IdleBackend>()),
          sched(ctx),
          ctrl(sched),
          driver(sched, test_name),
          q(sched, 1) {
        TimerTestControl::enable_test_clock(sched);
    }

    AsyncIoContext ctx;
    Scheduler sched;
    ControllerGuard ctrl;
    sluice_dst::DstScheduleDriver driver;
    AsyncQueue<int> q;

    Fiber fp, fc;
    FiberStack sp_, sc_;
    std::vector<std::string> events;

    // Producer: untimed or timed push of `value`.
    bool timed_producer = false;
    Scheduler::deadline_t producer_deadline = 50;
    int producer_value = 9;

    // Consumer: untimed or timed pop.
    bool timed_consumer = false;
    Scheduler::deadline_t consumer_deadline = 50;

    void build_fibers() {
        fp.set_entry([this](Fiber&) {
            QueuePushResult<int> r =
                timed_producer
                    ? q.push_until(producer_value, producer_deadline)
                    : q.push(producer_value);
            events.push_back("P:" + status_name(r.status()));
        });
        fc.set_entry([this](Fiber&) {
            QueuePopResult<int> r =
                timed_consumer ? q.pop_until(consumer_deadline) : q.pop();
            if (r.status() == QueuePopStatus::item) {
                events.push_back("C:item:" +
                                 std::to_string(std::move(r).take_value()));
            } else {
                events.push_back("C:" + status_name(r.status()));
            }
        });
        SLUICE_CHECK(sched.init_fiber(fp, sp_.base(), sp_.size()));
        SLUICE_CHECK(sched.init_fiber(fc, sc_.base(), sc_.size()));
        driver.bind(0, fp).bind(1, fc);
    }

    static std::string status_name(QueuePushStatus s) {
        switch (s) {
            case QueuePushStatus::committed: return "committed";
            case QueuePushStatus::closed: return "closed";
            case QueuePushStatus::expired: return "expired";
            case QueuePushStatus::would_block: return "would_block";
        }
        return "?";
    }
    static std::string status_name(QueuePopStatus s) {
        switch (s) {
            case QueuePopStatus::item: return "item";
            case QueuePopStatus::closed: return "closed";
            case QueuePopStatus::expired: return "expired";
            case QueuePopStatus::would_block: return "would_block";
        }
        return "?";
    }

    // Destruction contract (queue_port.cpp ~QueuePort): the ring must be
    // empty and the teardown session complete before scope exit.
    void dispose() {
        q.close();
        for (;;) {
            QueuePopResult<int> r = q.try_pop();
            if (r.status() != QueuePopStatus::item) break;
        }
        auto session = q.begin_teardown();
        (void)session;  // born complete on an empty ring
    }
};

}  // namespace

// V1 — THE SHARP PROBE: parked producer vs blocking pop's inline success.
// Ring pre-filled from the main thread (try_push, no fiber needed); P parks
// (push onto a full ring); C's BLOCKING pop succeeds inline. The AC-2a
// matrix documents try_pop/close as the producer reconcilers; whether this
// path also reconciles is executed here.
//
// The assertion below is a KNOWN-DRIFT CHARACTERIZATION WITNESS, NOT a
// statement of desired contract: today it proves the as-built defect exists
// (a parked blocking producer is NOT reconciled by the inline pop success
// path — no queue_grant_producer_locked). A future Queue repair slice must
// consciously FLIP or REPLACE this expectation (the producer would commit
// from the pop); the review P2 agreement is that no Queue change is made in
// this campaign.
// Structure: run -> capture -> dispose -> assert (a failing check returns
// from the case, so the queue's ring-empty destruction contract must be
// satisfied BEFORE any assertion).
SLUICE_TEST_CASE(dst_t5_v1_parked_producer_vs_inline_pop) {
    if constexpr (!fiber_ctx::supported) return;

    QueueFixture fx("dst_t5_v1");
    fx.build_fibers();
    SLUICE_CHECK(fx.q.try_push(7).status() == QueuePushStatus::committed);
    fx.driver.run(0).run(1);  // P parks, C pops inline
    fx.driver.arm();
    fx.sched.spawn(fx.fp);
    fx.sched.spawn(fx.fc);
    fx.sched.run(1);

    // KNOWN-DRIFT CHARACTERIZATION WITNESS: the blocking pop's inline success
    // does NOT reconcile the parked producer — the documented reconciler set
    // is try_pop/close only (queue_port.cpp try_pop's FastPopCommit carries
    // queue_grant_producer_locked; queue_pop_admit's inline path does not).
    // P therefore remains WAITING at run end (the drain-mode run STALLED);
    // close resolves P closed with its lease retained. A Queue repair slice
    // must flip this expectation (P:committed from the pop), not preserve it.
    const std::string ev = join(fx.events);
    const FiberState p_state_at_run_end = fx.fp.state();
    const FiberState c_state = fx.fc.state();
    fx.q.close();
    fx.sched.run(1);
    const std::string ev_after_cleanup = join(fx.events);
    fx.dispose();

    // Exactly-once resource invariant: C received THE pre-filled item.
    SLUICE_CHECK_MSG(ev == "P:parked,C:item:7" || ev == "C:item:7,P:parked" ||
                         ev.find("C:item:7") != std::string::npos,
                     ("consumer must get the pre-filled item (events: " + ev +
                      ")")
                         .c_str());
    SLUICE_CHECK(c_state == FiberState::done);
    SLUICE_CHECK_MSG(
        p_state_at_run_end == FiberState::waiting,
        "KNOWN-DRIFT CHARACTERIZATION WITNESS: the blocking pop's inline "
        "success leaves the parked producer UNRECONCILED (as-built defect, "
        "AC-2a/#234; a Queue repair must flip this expectation)");
    SLUICE_CHECK_MSG(ev.find("P:committed") == std::string::npos,
                     "witness: no inline reconcile under the pop — P:committed "
                     "is the future-repair expectation, not today's behavior");
    SLUICE_CHECK_MSG(
        ev_after_cleanup.find("P:closed") != std::string::npos,
        ("close-cleanup resolves the stranded producer (events: " +
         ev_after_cleanup + ")")
            .c_str());
}

// V2 — timed producer push_until(50): expiry / documented-reconciler /
//      close / already-due interleavings around the admission ladder.
SLUICE_TEST_CASE(dst_t5_v2_timed_producer_ladder) {
    if constexpr (!fiber_ctx::supported) return;

    // (a) pump expiry after suspend: P:expired, lease recovered.
    {
        QueueFixture fx("dst_t5_v2a");
        fx.timed_producer = true;
        fx.build_fibers();
        SLUICE_CHECK(fx.q.try_push(7).status() == QueuePushStatus::committed);
        Scheduler* sp = &fx.sched;
        fx.driver.on_action(0, "Clock(60)", [sp](Scheduler&) {
            sp->advance_clock(Scheduler::deadline_t{60});
        });
        fx.driver.run(0).invoke(0);  // P parks; clock pump expires it
        fx.driver.arm();
        fx.sched.spawn(fx.fp);
        fx.sched.run(1);
        std::string ev = join(fx.events);
        unsigned active =
            TimerTestControl::active_deadline_count(fx.sched);
        fx.dispose();
        SLUICE_CHECK_MSG(ev == "P:expired", ("v2a: " + ev).c_str());
        SLUICE_CHECK(active == 0);
    }
    // (b) the DOCUMENTED reconciler: a main-thread try_pop's FastPopCommit
    //     grants the parked producer (winner-before-publication).
    {
        QueueFixture fx("dst_t5_v2b");
        fx.timed_producer = true;
        fx.build_fibers();
        SLUICE_CHECK(fx.q.try_push(7).status() == QueuePushStatus::committed);
        AsyncQueue<int>* qp = &fx.q;
        fx.driver.on_action(0, "TryPop", [qp](Scheduler&) {
            (void)qp->try_pop();
        });
        fx.driver.run(0).invoke(0);  // P parks; try_pop frees + grants
        fx.driver.arm();
        fx.sched.spawn(fx.fp);
        fx.sched.run(1);
        std::string ev = join(fx.events);
        unsigned active =
            TimerTestControl::active_deadline_count(fx.sched);
        fx.dispose();
        SLUICE_CHECK_MSG(ev == "P:committed", ("v2b: " + ev).c_str());
        SLUICE_CHECK(active == 0);
    }
    // (c) close after suspend: grant path resolves P closed (lease retained).
    {
        QueueFixture fx("dst_t5_v2c");
        fx.timed_producer = true;
        fx.build_fibers();
        SLUICE_CHECK(fx.q.try_push(7).status() == QueuePushStatus::committed);
        AsyncQueue<int>* qp = &fx.q;
        fx.driver.on_action(0, "Close", [qp](Scheduler&) { qp->close(); });
        fx.driver.run(0).invoke(0);
        fx.driver.arm();
        fx.sched.spawn(fx.fp);
        fx.sched.run(1);
        std::string ev = join(fx.events);
        unsigned active =
            TimerTestControl::active_deadline_count(fx.sched);
        fx.dispose();
        SLUICE_CHECK_MSG(ev == "P:closed", ("v2c: " + ev).c_str());
        SLUICE_CHECK(active == 0);
    }
    // (d) already-due at admission with a FULL ring (clock pre-set past the
    //     deadline): precedence-1 resource fails, precedence-2 already-due
    //     resolves Expired INLINE — P never suspends, so no
    //     registration->suspend window ever opens.
    {
        QueueFixture fx("dst_t5_v2d");
        fx.timed_producer = true;
        TimerTestControl::set_clock(fx.sched, 100);
        fx.build_fibers();
        SLUICE_CHECK(fx.q.try_push(7).status() == QueuePushStatus::committed);
        fx.driver.run(0);
        fx.driver.arm();
        fx.sched.spawn(fx.fp);
        fx.sched.run(1);
        std::string ev = join(fx.events);
        fx.dispose();
        SLUICE_CHECK_MSG(ev == "P:expired", ("v2d: " + ev).c_str());
    }
    // (d') already-due with an ADMISSIBLE ring (empty): precedence-1
    //     resource WINS over the due deadline — P commits inline. The
    //     ladder's documented precedence order, executed.
    {
        QueueFixture fx("dst_t5_v2d2");
        fx.timed_producer = true;
        TimerTestControl::set_clock(fx.sched, 100);
        fx.build_fibers();
        fx.driver.run(0);
        fx.driver.arm();
        fx.sched.spawn(fx.fp);
        fx.sched.run(1);
        std::string ev = join(fx.events);
        fx.dispose();
        SLUICE_CHECK_MSG(ev == "P:committed", ("v2d2: " + ev).c_str());
    }
    // (e) close BEFORE admission: closed_ is observed under the same
    //     critical section as the resource recheck — inline closed, no
    //     suspend, lease retained.
    {
        QueueFixture fx("dst_t5_v2e");
        fx.timed_producer = true;
        fx.build_fibers();
        fx.q.close();
        fx.driver.run(0);
        fx.driver.arm();
        fx.sched.spawn(fx.fp);
        fx.sched.run(1);
        std::string ev = join(fx.events);
        fx.dispose();
        SLUICE_CHECK_MSG(ev == "P:closed", ("v2e: " + ev).c_str());
    }
}

// V3 — timed consumer pop_until(50): the consumer-side ladder.
SLUICE_TEST_CASE(dst_t5_v3_timed_consumer_ladder) {
    if constexpr (!fiber_ctx::supported) return;

    // (a) pump expiry after suspend.
    {
        QueueFixture fx("dst_t5_v3a");
        fx.timed_consumer = true;
        fx.build_fibers();
        Scheduler* sp = &fx.sched;
        fx.driver.on_action(0, "Clock(60)", [sp](Scheduler&) {
            sp->advance_clock(Scheduler::deadline_t{60});
        });
        fx.driver.run(1).invoke(0);
        fx.driver.arm();
        fx.sched.spawn(fx.fc);
        fx.sched.run(1);
        std::string ev = join(fx.events);
        fx.dispose();
        SLUICE_CHECK_MSG(ev == "C:expired", ("v3a: " + ev).c_str());
    }
    // (b) documented reconciler: try_push FastPushCommit grants the parked
    //     consumer the OLDEST ring item.
    {
        QueueFixture fx("dst_t5_v3b");
        fx.timed_consumer = true;
        fx.build_fibers();
        AsyncQueue<int>* qp = &fx.q;
        fx.driver.on_action(0, "TryPush(5)", [qp](Scheduler&) {
            (void)qp->try_push(5);
        });
        fx.driver.run(1).invoke(0);
        fx.driver.arm();
        fx.sched.spawn(fx.fc);
        fx.sched.run(1);
        std::string ev = join(fx.events);
        fx.dispose();
        SLUICE_CHECK_MSG(ev == "C:item:5", ("v3b: " + ev).c_str());
    }
    // (c) close after suspend with an EMPTY ring: closed+empty terminal.
    {
        QueueFixture fx("dst_t5_v3c");
        fx.timed_consumer = true;
        fx.build_fibers();
        AsyncQueue<int>* qp = &fx.q;
        fx.driver.on_action(0, "Close", [qp](Scheduler&) { qp->close(); });
        fx.driver.run(1).invoke(0);
        fx.driver.arm();
        fx.sched.spawn(fx.fc);
        fx.sched.run(1);
        std::string ev = join(fx.events);
        fx.dispose();
        SLUICE_CHECK_MSG(ev == "C:closed", ("v3c: " + ev).c_str());
    }
}

// T6 — review P1-2 harness contract: an Invoke action MAY re-enter the test
// control surface (uninstall_schedule_script here) without self-deadlock.
// Pre-fix the action ran while holding the script mutex, so this call locked
// the same mutex and hung; post-fix the action runs with NO script mutex
// held, the seam deactivates mid-run, and the run continues on the plain FIFO
// pop (a free run).
SLUICE_TEST_CASE(dst_t6_action_reenters_control_surface) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    ControllerGuard ctrl(sched);
    sluice_dst::DstScheduleDriver driver(sched, "dst_t6");

    WaitQueue qa;
    WaitNode na;
    Fiber fa, fb;
    fa.set_entry([&](Fiber&) { (void)sched.await_wait(qa, na); });
    fb.set_entry([&](Fiber&) {});
    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));

    driver.on_action(0, "Uninstall", [&](Scheduler& s) {
        sluice_async_test::uninstall_schedule_script(s);  // re-entry
    });
    driver.bind(0, fa).bind(1, fb);
    driver.run(0).invoke(0);  // A runs + suspends; action uninstalls mid-run
    driver.arm();
    sched.spawn(fa);
    sched.spawn(fb);
    sched.run(1);  // script deactivated; B completes via the FIFO pop
    SLUICE_CHECK(fb.state() == FiberState::done);
    SLUICE_CHECK(fa.state() == FiberState::waiting);  // MW-S3 STALLED return
    (void)sched.cancel_wait(qa, na);
    sched.run(1);
    SLUICE_CHECK(fa.state() == FiberState::done);
}

// ============================================================================
// T3 — illegal decision aborts with the deterministic package (death child).
// ============================================================================
#if defined(__unix)

namespace {

// Child: A suspends on a generic wait; the script then requests Run(A) while
// A is WAITING (legal set: {B}). The pick must abort — never fall back.
void dst_t3_child_illegal() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    ControllerGuard ctrl(sched);
    sluice_dst::DstScheduleDriver driver(sched, "dst_t3_illegal");

    WaitQueue qa;
    WaitNode na;
    Fiber fa, fb;
    fa.set_entry([&](Fiber&) { (void)sched.await_wait(qa, na); });
    fb.set_entry([&](Fiber&) {});
    FiberStack sa, sb;
    if (!sched.init_fiber(fa, sa.base(), sa.size())) std::_Exit(88);
    if (!sched.init_fiber(fb, sb.base(), sb.size())) std::_Exit(88);

    driver.bind(0, fa).bind(1, fb);
    driver.run(0).run(0);  // step 0 legal (A runnable); step 1 illegal (A waiting)
    driver.arm();
    sched.spawn(fa);
    sched.spawn(fb);
    sched.run(1);
    // Reaching here means the pick silently fell back — must not happen.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// Control: the same fixture with a LEGAL script completes and exits 0.
void dst_t3_child_control() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    ControllerGuard ctrl(sched);
    sluice_dst::DstScheduleDriver driver(sched, "dst_t3_control");

    WaitQueue qa;
    WaitNode na;
    Fiber fa, fb;
    fa.set_entry([&](Fiber&) { (void)sched.await_wait(qa, na); });
    fb.set_entry([&](Fiber&) {});
    FiberStack sa, sb;
    if (!sched.init_fiber(fa, sa.base(), sa.size())) std::_Exit(88);
    if (!sched.init_fiber(fb, sb.base(), sb.size())) std::_Exit(88);

    driver.bind(0, fa).bind(1, fb);
    driver.run(0).run(1);  // A suspends; B chosen; script exhausted -> FIFO
    driver.arm();
    sched.spawn(fa);
    sched.spawn(fb);
    sched.run(1);  // B done; A waiting -> MW-S3 STALLED; run returns
    if (fb.state() != FiberState::done) std::_Exit(88);
    // Cleanup: resolve A so the Scheduler/WaitQueue teardown is quiescent.
    (void)sched.cancel_wait(qa, na);
    sched.run(1);
    if (fa.state() != FiberState::done) std::_Exit(88);
    std::_Exit(0);
}

// Child (review P1-4): an out-of-range participant id must fail loudly at
// bind time — never an out-of-bounds write or a silently truncated vector.
void dst_t3_child_driver_bounds() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    ControllerGuard ctrl(sched);
    sluice_dst::DstScheduleDriver driver(sched, "dst_t3_driver_bounds");
    Fiber fb;
    FiberStack sb;
    if (!sched.init_fiber(fb, sb.base(), sb.size())) std::_Exit(88);
    driver.bind(8, fb);  // 8 >= kScheduleMaxFibers -> driver fail-fast abort
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// Child (review P1-4 / P1-2 defense-in-depth): an Invoke step referencing a
// NEVER-registered action aborts at the pick with the configuration package.
void dst_t3_child_unregistered_action() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    ControllerGuard ctrl(sched);
    sluice_dst::DstScheduleDriver driver(sched, "dst_t3_unregistered_action");
    Fiber fb;
    FiberStack sb;
    if (!sched.init_fiber(fb, sb.base(), sb.size())) std::_Exit(88);
    driver.bind(0, fb);
    driver.invoke(0);  // no on_action(0) registered -> config fail at pick
    driver.arm();
    sched.spawn(fb);
    sched.run(1);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// Fork a child re-execing THIS binary with --death-child=<case>, capturing
// the child's stderr through a pipe (the runner's self-exec discipline: the
// child is the same internal-testing binary; post-fork work is restricted to
// close/dup2/execv/_Exit).
struct CapturedChild {
    int status = -1;
    std::string stderr_text;
    bool timed_out = false;
};

CapturedChild run_child_captured(const std::string& case_name) {
    CapturedChild out;
    const std::string self = sluice_death_test::resolve_self_executable_path();
    if (self.empty()) {
        std::perror("dst death: resolve_self_executable_path");
        return out;
    }
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        std::perror("dst death: pipe");
        return out;
    }
    pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("dst death: fork");
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return out;
    }
    if (pid == 0) {
        // Child: stderr -> pipe write end; restricted ops only.
        ::close(pipefd[0]);
        if (::dup2(pipefd[1], STDERR_FILENO) < 0) std::_Exit(88);
        ::close(pipefd[1]);
        const std::string arg1 = sluice_death_test::child_arg(case_name);
        char arg0_buf[4096];
        std::snprintf(arg0_buf, sizeof(arg0_buf), "%s", self.c_str());
        char arg1_buf[512];
        std::snprintf(arg1_buf, sizeof(arg1_buf), "%s", arg1.c_str());
        char* argv[] = {arg0_buf, arg1_buf, nullptr};
        ::execv(argv[0], argv);
        std::_Exit(88);
    }
    // Parent: bounded capture loop — drain the pipe AND reap the child under
    // ONE deadline. A child that hangs with its stderr pipe still open must
    // reach the watchdog: a blocking read-to-EOF performed before waitpid
    // would never return (review P1-3), so the pipe is polled with the
    // remaining budget and the child is reaped with waitpid(WNOHANG) on every
    // iteration. The poll paces the loop even once EOF is seen (a live child
    // with its stderr closed), so there is no busy spin and no unbounded
    // wait. The watchdog is the only time bound in this harness.
    ::close(pipefd[1]);
    std::string text;
    char buf[512];
    constexpr auto kTimeout = std::chrono::seconds{60};
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    bool exited = false;
    bool eof = false;
    bool reap_error = false;
    while ((!exited || !eof) && !reap_error) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        if (!exited) {
            int status = 0;
            const pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                exited = true;
                out.status = status;
            } else if (w < 0 && errno != EINTR) {
                std::perror("dst death: waitpid");
                reap_error = true;  // status stays -1: the caller fails loudly
            }
        }
        struct pollfd pfd;
        pfd.fd = pipefd[0];
        pfd.events = static_cast<short>(POLLIN | POLLHUP);
        pfd.revents = 0;
        const auto remain = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int pr = ::poll(&pfd, 1,
                              static_cast<int>(remain.count() > 0
                                                   ? remain.count()
                                                   : 1));
        if (pr > 0 && !eof) {
            const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
            if (n > 0) {
                text.append(buf, static_cast<std::size_t>(n));
            } else if (n == 0) {
                eof = true;  // child closed stderr (or died)
            } else if (errno != EINTR) {
                eof = true;  // read error: no further data
            }
        } else if (pr < 0 && errno != EINTR && !eof) {
            eof = true;  // poll error: no further data
        }
    }
    ::close(pipefd[0]);
    if (!exited) {
        (void)::kill(pid, SIGKILL);
        if (!reap_error) {
            out.timed_out = true;
            int status2 = 0;
            while (::waitpid(pid, &status2, 0) < 0 && errno == EINTR) {}
            out.status = status2;
        }
    }
    out.stderr_text = std::move(text);
    return out;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

SLUICE_TEST_CASE(dst_t3_illegal_decision_aborts) {
    const CapturedChild bad = run_child_captured("dst_t3_illegal");
    SLUICE_CHECK(bad.status != -1);
    SLUICE_CHECK_MSG(!bad.timed_out, "illegal-pick child must not hang");
    SLUICE_CHECK_MSG(WIFSIGNALED(bad.status) && WTERMSIG(bad.status) == SIGABRT,
                     "illegal pick must abort loudly (no silent fallback)");
    SLUICE_CHECK_MSG(contains(bad.stderr_text, "SCHEDULE SCRIPT FAILURE"),
                     "diagnostic package header present");
    SLUICE_CHECK_MSG(contains(bad.stderr_text, "step index:"),
                     "step index present");
    SLUICE_CHECK_MSG(contains(bad.stderr_text, "requested:"),
                     "requested choice present");
    SLUICE_CHECK_MSG(contains(bad.stderr_text, "legal runnable:"),
                     "legal runnable set present");
    SLUICE_CHECK_MSG(contains(bad.stderr_text, "logical clock:"),
                     "logical clock present");
    SLUICE_CHECK_MSG(contains(bad.stderr_text, "replay vector:"),
                     "replay vector present");

    const CapturedChild ctl = run_child_captured("dst_t3_control");
    SLUICE_CHECK_MSG(!ctl.timed_out, "control child must not hang");
    SLUICE_CHECK_MSG(WIFEXITED(ctl.status) && WEXITSTATUS(ctl.status) == 0,
                     "legal script completes cleanly (control)");

    // Review P1-4: driver capacity violations abort loudly with the driver's
    // own package (id 8 >= the 8-fiber capacity), and an Invoke referencing an
    // action that was never registered aborts with the configuration package.
    const CapturedChild bnd = run_child_captured("dst_t3_driver_bounds");
    SLUICE_CHECK_MSG(!bnd.timed_out, "driver-bounds child must not hang");
    SLUICE_CHECK_MSG(WIFSIGNALED(bnd.status) && WTERMSIG(bnd.status) == SIGABRT,
                     "out-of-range driver ids must abort loudly");
    SLUICE_CHECK_MSG(contains(bnd.stderr_text, "DST SCHEDULE DRIVER FAILURE"),
                     "driver capacity header present");
    SLUICE_CHECK_MSG(contains(bnd.stderr_text, "bind: participant id out of range"),
                     "the violating id is named");

    const CapturedChild ua = run_child_captured("dst_t3_unregistered_action");
    SLUICE_CHECK_MSG(!ua.timed_out, "unregistered-action child must not hang");
    SLUICE_CHECK_MSG(WIFSIGNALED(ua.status) && WTERMSIG(ua.status) == SIGABRT,
                     "unregistered Invoke must abort loudly");
    SLUICE_CHECK_MSG(contains(ua.stderr_text, "DST-PV-1 SCHEDULE SCRIPT FAILURE"),
                     "script failure header present");
    SLUICE_CHECK_MSG(contains(ua.stderr_text, "unregistered action id"),
                     "the missing action is named");
}

#else
SLUICE_TEST_CASE(dst_t3_illegal_decision_aborts) {
    // POSIX-only death harness; the seam itself is exercised by T1/T2/T4/T5.
}
#endif  // __unix

int main(int argc, char** argv) {
#if defined(__unix)
    const std::string child = sluice_death_test::parse_child_case(argc, argv);
    if (child == "dst_t3_illegal") {
        dst_t3_child_illegal();
        std::_Exit(0);
    }
    if (child == "dst_t3_control") {
        dst_t3_child_control();
        std::_Exit(0);
    }
    if (child == "dst_t3_driver_bounds") {
        dst_t3_child_driver_bounds();
        std::_Exit(0);
    }
    if (child == "dst_t3_unregistered_action") {
        dst_t3_child_unregistered_action();
        std::_Exit(0);
    }
#else
    (void)argc;
    (void)argv;
#endif
    return ::sluice_test::run_all();
}
