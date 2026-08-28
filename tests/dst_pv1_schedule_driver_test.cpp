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
//   T6  harness contract (review P1-2 / R3): an Invoke action may re-enter
//       SUPPORTED NON-REPLACING test-control surfaces; uninstall is
//       explicitly supported, re-install / re-arm is fail-closed (T7).
//       Plus the review P1-4 death children inside T3: out-of-range driver
//       ids and unregistered actions abort loudly instead of corrupting
//       memory.
//   T7  fail-closed re-arm (review R3 P1): an Invoke action attempting to
//       install / re-arm a schedule script aborts with the named diagnostic
//       (death child) — the old script's epilogue can never advance a
//       replaced replay vector.
//   T8  watchdog pacing (review R3 P2): a death child that closes its
//       stderr pipe and then hangs must be bounded by the parent's
//       watchdog — no busy spin on the HUP'd pipe, SIGKILL at the deadline,
//       timed_out reported (parent CPU stays far below wall time).
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
#include <sys/resource.h>
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

// V1 — THE SHARP PROBE (Q-LIV-1 REGRESSION): parked producer vs blocking
// pop's inline success. Ring pre-filled from the main thread (try_push, no
// fiber needed); P parks (push onto a full ring); C's BLOCKING pop succeeds
// inline and frees a ring slot. The Q-LIV-1 repair (scheduler_queue.cpp
// queue_pop_admit) reconciles the opposite-role FIFO head at that inline
// success — the same queue_grant_producer_locked authority try_pop's
// FastPopCommit has always performed (queue_port.cpp).
//
// HISTORY: this case originated as the DST-PV-1 KNOWN-DRIFT CHARACTERIZATION
// WITNESS and became the post-fix regression. Pre-fix it proved the as-built
// defect: the inline pop success left the parked producer stranded (no
// reconcile; only a later try_pop/close released it). The Q-LIV-1 Queue
// repair consciously FLIPPED the expectation: the producer now commits from
// the pop's freed slot, and no close is required merely to release it.
// Structure: run -> capture -> dispose -> assert (a failing check returns
// from the case, so the queue's ring-empty destruction contract must be
// satisfied BEFORE any assertion).
SLUICE_TEST_CASE(dst_t5_v1_parked_producer_vs_inline_pop) {
    if constexpr (!fiber_ctx::supported) return;

    QueueFixture fx("dst_t5_v1");
    fx.build_fibers();
    SLUICE_CHECK(fx.q.try_push(7).status() == QueuePushStatus::committed);
    fx.driver.run(0).run(1);  // P parks, C pops inline (and reconciles P)
    fx.driver.arm();
    fx.sched.spawn(fx.fp);
    fx.sched.spawn(fx.fc);
    fx.sched.run(1);

    const std::string ev = join(fx.events);
    const FiberState p_state_at_run_end = fx.fp.state();
    const FiberState c_state = fx.fc.state();
    fx.q.close();
    fx.sched.run(1);
    const std::string ev_after_cleanup = join(fx.events);
    fx.dispose();

    // Exactly-once resource invariant: C received THE pre-filled item, and
    // P's lease committed exactly once into the slot the pop freed (no item
    // duplication, no capacity-1 slot double occupancy).
    SLUICE_CHECK_MSG(ev.find("C:item:7") != std::string::npos,
                     ("consumer must get the pre-filled item (events: " + ev +
                      ")")
                         .c_str());
    SLUICE_CHECK(c_state == FiberState::done);
    // Q-LIV-1 REGRESSION (flipped witness): the blocking pop's inline success
    // must reconcile the parked producer — the drain run finishes BOTH
    // fibers, with P committing BEFORE any close.
    SLUICE_CHECK_MSG(
        p_state_at_run_end == FiberState::done,
        ("Q-LIV-1: blocking pop inline success must reconcile the parked "
         "producer instead of stranding it (events: " +
         ev + ")")
            .c_str());
    SLUICE_CHECK_MSG(ev.find("P:committed") != std::string::npos,
                     ("producer must commit from the pop's inline reconcile "
                      "without a close (events: " +
                      ev + ")")
                         .c_str());
    SLUICE_CHECK_MSG(ev.find("P:closed") == std::string::npos,
                     "close must not be needed merely to release the producer");
}

// V1-SYM — Q-LIV-1 SYMMETRIC DIRECTION: parked consumer vs blocking push's
// inline success. Ring starts EMPTY; C parks (pop on an empty ring); P's
// BLOCKING push commits inline and fills a ring slot. The Q-LIV-1 repair
// (scheduler_queue.cpp queue_push_admit) reconciles the parked consumer at
// that inline success — the same queue_grant_consumer_locked authority
// try_push's FastPushCommit has always performed (queue_port.cpp). The
// consumer pops the just-committed item; no close is required merely to
// release it. Exactly-once item accounting: the single item goes to exactly
// one role (P commits it, C consumes it).
SLUICE_TEST_CASE(dst_t5_v1s_parked_consumer_vs_inline_push) {
    if constexpr (!fiber_ctx::supported) return;

    QueueFixture fx("dst_t5_v1s");
    fx.build_fibers();
    fx.driver.run(1).run(0);  // C parks (empty ring), P commits inline
    fx.driver.arm();
    fx.sched.spawn(fx.fp);
    fx.sched.spawn(fx.fc);
    fx.sched.run(1);

    const std::string ev = join(fx.events);
    const FiberState p_state_at_run_end = fx.fp.state();
    const FiberState c_state = fx.fc.state();
    fx.q.close();
    fx.sched.run(1);
    const std::string ev_after_cleanup = join(fx.events);
    fx.dispose();

    SLUICE_CHECK_MSG(ev.find("P:committed") != std::string::npos,
                     ("producer must commit inline (events: " + ev + ")")
                         .c_str());
    SLUICE_CHECK_MSG(
        ev.find("C:item:9") != std::string::npos,
        ("Q-LIV-1 symmetric: blocking push inline success must reconcile the "
         "parked consumer, which consumes THE committed item (events: " +
         ev + ")")
            .c_str());
    SLUICE_CHECK(p_state_at_run_end == FiberState::done);
    SLUICE_CHECK(c_state == FiberState::done);
    SLUICE_CHECK_MSG(ev.find("C:closed") == std::string::npos,
                     "close must not be needed merely to release the consumer");
}

// V1-T — Q-LIV-1 x AC-2b: TIMED producer parked on a full ring; C's BLOCKING
// pop succeeds inline and the grant reconciles P. The grant seam must retire
// P's ordinary deadline EXACTLY ONCE (retire_timer_for_node_locked inside
// queue_grant_producer_locked): the active count returns to 0 and a later
// clock advance past the deadline produces no second terminal (no P:expired
// after P:committed — the terminal-winner law absorbs the stale timer).
SLUICE_TEST_CASE(dst_t5_v1t_timed_producer_inline_pop_reconcile) {
    if constexpr (!fiber_ctx::supported) return;

    QueueFixture fx("dst_t5_v1t");
    fx.timed_producer = true;
    fx.build_fibers();
    SLUICE_CHECK(fx.q.try_push(7).status() == QueuePushStatus::committed);
    fx.driver.run(0).run(1);  // P parks (timed), C pops inline (and grants P)
    fx.driver.arm();
    fx.sched.spawn(fx.fp);
    fx.sched.spawn(fx.fc);
    fx.sched.run(1);

    const std::string ev = join(fx.events);
    const FiberState p_state_at_run_end = fx.fp.state();
    const unsigned active_timers =
        TimerTestControl::active_deadline_count(fx.sched);
    fx.q.close();
    fx.sched.run(1);
    fx.dispose();

    SLUICE_CHECK_MSG(ev.find("C:item:7") != std::string::npos,
                     ("consumer must get the pre-filled item (events: " + ev +
                      ")")
                         .c_str());
    SLUICE_CHECK_MSG(ev.find("P:committed") != std::string::npos,
                     ("Q-LIV-1: inline pop must reconcile the parked TIMED "
                      "producer (events: " +
                      ev + ")")
                         .c_str());
    SLUICE_CHECK(p_state_at_run_end == FiberState::done);
    // AC-2b closure: the grant retired the winner's timer exactly once.
    SLUICE_CHECK_MSG(active_timers == 0,
                     "grant-side retire must drain the parked producer's timer");
    // Exactly-once terminal: a stale clock advance finds no live registration.
    fx.sched.advance_clock(Scheduler::deadline_t{60});
    fx.sched.run(1);
    SLUICE_CHECK_MSG(join(fx.events).find("P:expired") == std::string::npos,
                     "terminal winner law: no second terminal after commit");
}

// V1-TS — Q-LIV-1 symmetric x AC-2b: TIMED consumer parked on an empty ring;
// P's BLOCKING push succeeds inline and the grant reconciles C. The grant
// seam retires C's ordinary deadline exactly once; a later clock advance
// produces no second terminal (no C:expired after C:item).
SLUICE_TEST_CASE(dst_t5_v1ts_timed_consumer_inline_push_reconcile) {
    if constexpr (!fiber_ctx::supported) return;

    QueueFixture fx("dst_t5_v1ts");
    fx.timed_consumer = true;
    fx.build_fibers();
    fx.driver.run(1).run(0);  // C parks (timed), P commits inline (grants C)
    fx.driver.arm();
    fx.sched.spawn(fx.fp);
    fx.sched.spawn(fx.fc);
    fx.sched.run(1);

    const std::string ev = join(fx.events);
    const FiberState p_state_at_run_end = fx.fp.state();
    const FiberState c_state = fx.fc.state();
    const unsigned active_timers =
        TimerTestControl::active_deadline_count(fx.sched);
    fx.q.close();
    fx.sched.run(1);
    fx.dispose();

    SLUICE_CHECK_MSG(ev.find("P:committed") != std::string::npos,
                     ("producer must commit inline (events: " + ev + ")")
                         .c_str());
    SLUICE_CHECK_MSG(ev.find("C:item:9") != std::string::npos,
                     ("Q-LIV-1 symmetric: inline push must reconcile the "
                      "parked TIMED consumer (events: " +
                      ev + ")")
                         .c_str());
    SLUICE_CHECK(p_state_at_run_end == FiberState::done);
    SLUICE_CHECK(c_state == FiberState::done);
    // AC-2b closure: the grant retired the winner's timer exactly once.
    SLUICE_CHECK_MSG(active_timers == 0,
                     "grant-side retire must drain the parked consumer's timer");
    // Exactly-once terminal: a stale clock advance finds no live registration.
    fx.sched.advance_clock(Scheduler::deadline_t{60});
    fx.sched.run(1);
    SLUICE_CHECK_MSG(join(fx.events).find("C:expired") == std::string::npos,
                     "terminal winner law: no second terminal after grant");
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

// T6 — review P1-2 harness contract (review R3 wording): an Invoke action
// MAY re-enter SUPPORTED NON-REPLACING test-control surfaces;
// uninstall_schedule_script is explicitly supported, re-install / re-arm is
// fail-closed (T7). Pre-fix the action ran while holding the script mutex, so
// this call locked the same mutex and hung; post-fix the action runs with NO
// script mutex held, the seam deactivates mid-run, the epilogue records the
// executed step into the old state without reactivating, and the run
// continues on the plain FIFO pop (a free run).
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

// Child (review R3 P1): an Invoke action attempting to ARM a SECOND script
// (re-install / re-arm) must abort with the re-install diagnostic — the OLD
// script's post-action epilogue must never advance a replaced replay vector.
// The abort fires inside install_schedule_script under the same-thread
// invoke-active guard; reaching here means the re-arm was silently allowed.
void dst_t7_child_reinstall_inside_invoke() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    ControllerGuard ctrl(sched);
    sluice_dst::DstScheduleDriver driver_a(sched, "dst_t7_a");
    sluice_dst::DstScheduleDriver driver_b(sched, "dst_t7_b");

    WaitQueue qa;
    WaitNode na;
    Fiber fa, fb;
    fa.set_entry([&](Fiber&) { (void)sched.await_wait(qa, na); });
    fb.set_entry([&](Fiber&) {});
    FiberStack sa, sb;
    if (!sched.init_fiber(fa, sa.base(), sa.size())) std::_Exit(88);
    if (!sched.init_fiber(fb, sb.base(), sb.size())) std::_Exit(88);

    // B is a COMPLETE, legal plan: the guard must reject the re-arm ATTEMPT,
    // not some half-built script (no silent B-step skipping, no fallback).
    driver_b.bind(0, fa).bind(1, fb);
    driver_b.run(0).run(1);

    sluice_dst::DstScheduleDriver* b = &driver_b;
    driver_a.bind(0, fa).bind(1, fb);
    driver_a.on_action(0, "ReArmB", [b](sluice::async::Scheduler&) {
        b->arm();  // re-install inside Invoke -> fail-loud abort (T7)
    });
    driver_a.invoke(0);  // the first pop-site visit executes the action
    driver_a.arm();
    sched.spawn(fa);
    sched.spawn(fb);
    sched.run(1);
    // Reaching here means the re-arm was silently allowed — must not happen.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// Child (review R3 P2): close stderr (the captured pipe) at a known point,
// then hang forever. The parent must NOT busy-spin on the HUP'd pipe: the
// watchdog paces, reaches its (test-local, short) deadline, SIGKILLs the
// child, and reports timed_out.
void dst_t8_child_close_stderr_then_hang() {
    std::fprintf(stderr, "dst_t8: closing stderr; hanging\n");
    std::fflush(stderr);
    ::close(STDERR_FILENO);
    for (;;) ::pause();
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

// Watchdog pacing interval (ms). This paces ONLY a bounded watchdog
// observation (review R3 P2) — it must never be used to order a schedule;
// sleep-for-ordering is forbidden repository-wide, watchdog pacing is
// observation, not causality.
inline constexpr int kWatchdogPacingMs = 10;

CapturedChild run_child_captured_with_timeout(const std::string& case_name,
                                              std::chrono::milliseconds timeout) {
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
    // Parent: ONE bounded capture loop — drain the pipe AND reap the child under
    // ONE deadline. A child that hangs with its stderr pipe still open must
    // reach the watchdog: a blocking read-to-EOF performed before waitpid
    // would never return (review P1-3), so the pipe is polled with the
    // remaining budget and the child is reaped with waitpid(WNOHANG) on every
    // iteration. Once EOF is seen while the child still lives, the HUP'd
    // pipe would make poll return IMMEDIATELY — bounded but CPU-burning
    // (review R3 P2) — so the loop then switches to a fixed
    // kWatchdogPacingMs pacing interval. The watchdog is the only time bound
    // in this harness; the pacing never orders a schedule.
    ::close(pipefd[1]);
    std::string text;
    char buf[512];
    const auto deadline = std::chrono::steady_clock::now() + timeout;
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
        if (eof) {
            // EOF while the child still lives: a poll on the HUP'd pipe would
            // return immediately (busy spin), so pace the watchdog with a
            // fixed small interval instead. Bounded-CPU observation only.
            ::poll(nullptr, 0, kWatchdogPacingMs);
            continue;
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
        if (pr > 0) {
            const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
            if (n > 0) {
                text.append(buf, static_cast<std::size_t>(n));
            } else if (n == 0) {
                eof = true;  // child closed stderr (or died)
            } else if (errno != EINTR) {
                eof = true;  // read error: no further data
            }
        } else if (pr < 0 && errno != EINTR) {
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

CapturedChild run_child_captured(const std::string& case_name) {
    // Repository default watchdog (60s). The T8 regression uses a short
    // TEST-LOCAL timeout through the _with_timeout form; there is no public
    // or production timeout API.
    return run_child_captured_with_timeout(case_name, std::chrono::seconds{60});
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

// T7 — review R3 P1: re-install / re-arm is FAIL-CLOSED inside Invoke. The
// child arms script A whose sole step is Invoke(action); the action attempts
// to arm a second complete script B. The guard aborts inside
// install_schedule_script (same-thread invoke-active flag) with the named
// diagnostic — no silent replacement, no skipped B step, no hang.
SLUICE_TEST_CASE(dst_t7_reinstall_inside_invoke_aborts) {
    const CapturedChild bad = run_child_captured("dst_t7_reinstall");
    SLUICE_CHECK(bad.status != -1);
    SLUICE_CHECK_MSG(!bad.timed_out, "re-install child must not hang");
    SLUICE_CHECK_MSG(WIFSIGNALED(bad.status) && WTERMSIG(bad.status) == SIGABRT,
                     "re-install inside Invoke must abort loudly");
    SLUICE_CHECK_MSG(contains(bad.stderr_text, "SCHEDULE SCRIPT FAILURE"),
                     "re-install diagnostic header present");
    SLUICE_CHECK_MSG(
        contains(bad.stderr_text, "install/re-arm forbidden inside Invoke"),
        "re-install diagnostic names the violation");
    SLUICE_CHECK_MSG(contains(bad.stderr_text, "current script: dst_t7_a"),
                     "re-install diagnostic names the CURRENT script");
    SLUICE_CHECK_MSG(contains(bad.stderr_text, "step index:"),
                     "re-install diagnostic names the step");
}

// T8 — review R3 P2: the death watchdog must not busy-spin once a child
// closes its stderr (pipe EOF) and then hangs. The child case closes stderr
// at a known point and hangs forever; the parent uses a short TEST-LOCAL
// timeout (no public/production timeout API), must WAIT for the deadline
// (no early return), SIGKILL the child, report timed_out, and burn parent
// CPU far below the wall time (a spin loop would burn ~100% of a core; the
// 10ms pacing makes the loop bounded regardless of the timeout).
SLUICE_TEST_CASE(dst_t8_watchdog_hang_after_stderr_close) {
    struct rusage ru_before{};
    ::getrusage(RUSAGE_SELF, &ru_before);
    const auto wall_before = std::chrono::steady_clock::now();
    constexpr auto kShortWatchdog = std::chrono::milliseconds{500};
    const CapturedChild c = run_child_captured_with_timeout(
        "dst_t8_close_stderr_then_hang", kShortWatchdog);
    const auto wall_after = std::chrono::steady_clock::now();
    struct rusage ru_after{};
    ::getrusage(RUSAGE_SELF, &ru_after);

    SLUICE_CHECK_MSG(contains(c.stderr_text, "dst_t8: closing stderr"),
                     "the pre-hang diagnostic was captured before the close");
    SLUICE_CHECK_MSG(c.timed_out,
                     "EOF + live child must reach the watchdog deadline");
    SLUICE_CHECK_MSG(WIFSIGNALED(c.status) && WTERMSIG(c.status) == SIGKILL,
                     "the hung child is SIGKILLed at the deadline");
    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        wall_after - wall_before);
    SLUICE_CHECK_MSG(wall_ms >= std::chrono::milliseconds{400},
                     "the parent WAITED for the deadline (no early return)");
    // No-busy-spin proof: the parent's CPU in the window must stay far below
    // the wall time. A spin loop burns ~100% of one core; the 10ms watchdog
    // pacing costs a few microseconds per iteration even under TSan.
    const double cpu_s =
        static_cast<double>(ru_after.ru_utime.tv_sec -
                            ru_before.ru_utime.tv_sec) +
        static_cast<double>(ru_after.ru_utime.tv_usec -
                            ru_before.ru_utime.tv_usec) /
            1e6 +
        static_cast<double>(ru_after.ru_stime.tv_sec -
                            ru_before.ru_stime.tv_sec) +
        static_cast<double>(ru_after.ru_stime.tv_usec -
                            ru_before.ru_stime.tv_usec) /
            1e6;
    const double wall_s = wall_ms.count() / 1000.0;
    SLUICE_CHECK_MSG(cpu_s < wall_s * 0.5,
                     "watchdog pacing keeps parent CPU far below wall time "
                     "(no busy spin)");
}

#else
SLUICE_TEST_CASE(dst_t3_illegal_decision_aborts) {
    // POSIX-only death harness; the seam itself is exercised by T1/T2/T4/T5.
}
SLUICE_TEST_CASE(dst_t7_reinstall_inside_invoke_aborts) {
    // POSIX-only death probe; the fail-closed re-install guard is compiled
    // into the controller on every platform.
}
SLUICE_TEST_CASE(dst_t8_watchdog_hang_after_stderr_close) {
    // POSIX-only death probe (watchdog pacing).
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
    if (child == "dst_t7_reinstall") {
        dst_t7_child_reinstall_inside_invoke();
        std::_Exit(0);
    }
    if (child == "dst_t8_close_stderr_then_hang") {
        dst_t8_child_close_stderr_then_hang();
        std::_Exit(0);
    }
#else
    (void)argc;
    (void)argv;
#endif
    return ::sluice_test::run_all();
}
