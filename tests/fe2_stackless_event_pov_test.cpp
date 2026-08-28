// fe2_stackless_event_pov_test — FE-2 minimal stackless frontend
// proof-of-value (FE campaign; compliance gate
// docs/architecture/fe2-frontend-seam-compliance-gate.md Gate 4).
//
// Purpose: prove that ONE stackless (C++20 coroutine) continuation passes
// through the SAME Event semantic authorities as the stackful Fiber
// frontend — the shared admission ladder (event_wait_admit_locked), the
// WaitNode resolve_ terminal winner, the ordinary deadline authority, the
// Event cancellation closure, and the publication edge
// (publish_wait_winner_locked switching on the WaitResume kind) — WITHOUT
// creating any second terminal/timer/cancel/admission authority.
//
// What is test-only (AGENTS.md §15 / FE-1c scope): the tiny coroutine task,
// the awaiter, and the FeDeferredRecord continuation record. Everything the
// proof depends on semantically is PRODUCTION code: the shared ladder, the
// token seam, defer/take delivery split, and the winner tails.
//
// §21 publication-eligibility race matrix:
//   A. event already set before admission      -> fe2_pov_inline_already_set
//   B. set during admission, before arm        -> fe2_pov_set_across_admission_boundary
//   C. set after eligibility commit            -> fe2_pov_async_set_exactly_once
//   D. cancel at the equivalent boundary       -> fe2_pov_cancel_after_suspend
//   E. deadline already due                    -> fe2_pov_deadline_already_due
//   F. deadline due after eligibility commit   -> fe2_pov_deadline_due_after_suspend
// §22 no-user-code-under-lock witness         -> fe2_pov_no_user_code_under_lock
// Publication guard fail-closed direction     -> fe2_pov_record_guard_unit
//
// Determinism: causal phase seams + the test clock only. NO sleep proves
// any ordering (AGENTS.md §13.3).
#include "harness.hpp"

#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>

#include <coroutine>
#include <atomic>
#include <thread>
#include <utility>

using namespace sluice::async;
using AsyncTestAccess = Scheduler::AsyncTestAccess;
using FeRecord = AsyncTestAccess::FeDeferredRecord;

namespace {

// Minimal test-local void coroutine task. NOT a task library: no allocation
// customization, no exception propagation, no continuation chaining. The
// frame is the WaitEpoch storage the FE-1a lifetime audit requires
// (address-stable while suspended).
class FeTask {
public:
    struct promise_type {
        FeTask get_return_object() noexcept {
            return FeTask{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };

    explicit FeTask(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
    FeTask(FeTask&& other) noexcept : h_(std::exchange(other.h_, {})) {}
    FeTask(const FeTask&) = delete;
    FeTask& operator=(const FeTask&) = delete;
    ~FeTask() {
        if (h_) h_.destroy();
    }

    void start() { h_.resume(); }
    bool done() const noexcept { return h_.done(); }

private:
    std::coroutine_handle<promise_type> h_;
};

// The stackless awaiter over the shared Event admission ladder (via the
// internal-testing seam; the ladder itself is production code). Awaiting it:
//   - await_suspend returns false for rejected/inline-resolved admissions
//     (FE-1b L6: the caller never suspends; outcome is on the node);
//   - await_suspend returns true after the record was ARMED inside the
//     resolver-excluded admission critical section (FE-1b L7).
struct FeEventWaiter {
    Scheduler& sched;
    Event& ev;
    WaitNode& node;
    FeRecord& rec;

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        // FE-1b L2: bind the resume target BEFORE the epoch becomes
        // resolver-observable (this runs before the admission CS).
        rec.handle_address = h.address();
        return AsyncTestAccess::event_wait_deferred_for_test(sched, ev, node,
                                                             rec);
    }
    void await_resume() const noexcept {}
};

struct FeEventWaiterTimed {
    Scheduler& sched;
    Event& ev;
    WaitNode& node;
    FeRecord& rec;
    Scheduler::deadline_t deadline;

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        rec.handle_address = h.address();
        return AsyncTestAccess::event_wait_deferred_deadline_for_test(
            sched, ev, node, rec, deadline);
    }
    void await_resume() const noexcept {}
};

// Drain helper: discharge ALL pending deferred publications. Chunked take
// (under G) + resume with NO lock held (FE-1b L9). Returns the number of
// records consumed-and-resumed.
std::size_t drain_all(Scheduler& sched, std::atomic<int>&,
                      std::atomic<int>& guard_failures) {
    std::size_t total = 0;
    void* buf[16];
    while (true) {
        const std::size_t n =
            AsyncTestAccess::take_deferred_for_test(sched, buf, 16);
        if (n == 0) break;
        for (std::size_t i = 0; i < n; ++i) {
            auto* rec = static_cast<FeRecord*>(buf[i]);
            if (!rec->try_consume()) {
                // Unarmed consume = resume-before-armed (L8 broken);
                // double-consume = double resume. Both are loud failures.
                guard_failures.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::coroutine_handle<>::from_address(rec->handle_address)
                .resume();
            // The resumed BODY counts itself (the semantic resume count);
            // drain_all only returns how many records it discharged.
            total += 1;
        }
    }
    return total;
}

}  // namespace

// ---- §21-A: event already set before admission -> inline, no publication --
SLUICE_TEST_CASE(fe2_pov_inline_already_set) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    Event ev(sched, false);
    WaitNode node;  // coroutine-frame stand-in: this scope
    FeRecord rec;

    ev.set();  // SET before admission

    std::atomic<int> resumed{0};
    struct InlineCase {
        Scheduler& sched;
        Event& ev;
        WaitNode& node;
        FeRecord& rec;
        std::atomic<int>& resumed;
        FeTask run() {
            co_await FeEventWaiter{sched, ev, node, rec};
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    InlineCase ic{sched, ev, node, rec, resumed};
    FeTask t = ic.run();
    t.start(); // runs to completion WITHOUT suspending (await_suspend false)

    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(node.outcome() == WaitOutcome::woken);
    SLUICE_CHECK(node.is_terminal());
    // Inline resolution published nothing (FE-1b L6): transit list empty,
    // record never armed.
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    SLUICE_CHECK(rec.state.load() == FeRecord::State::unarmed);
}

// ---- §21-B/C at the phase-controlled boundary: set ACROSS admission -------
// The admission thread pauses INSIDE the shared ladder (after registration,
// before the SET check, while holding G+W). set() is an external producer
// that takes G — it can only linearize AFTER the admission CS releases (the
// set/reset epoch isolation makes "SET stored inside the admission CS"
// unrepresentable for Event; the observable §21-B schedules reduce to
// §21-A SET-at-check (inline) or this async wake across the boundary).
// Deterministic proof: the eligibility commit (arm) lands inside the paused
// CS; the whole set()+drain linearizes strictly after the release; the
// suspended continuation is published EXACTLY ONCE — no lost wake across
// the exact admission/eligibility boundary.
//
// Ownership note: the FeTask lives in the TEST scope (the admission thread
// only starts it) — a suspended coroutine must outlive its starter thread;
// the drain on the test thread discharges it.
SLUICE_TEST_CASE(fe2_pov_set_across_admission_boundary) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    sluice_async_test::ControllerGuard ctrl(sched);  // event seam registry
    Event ev(sched, false);
    WaitNode node;
    FeRecord rec;
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    struct Case {
        Scheduler& sched;
        Event& ev;
        WaitNode& node;
        FeRecord& rec;
        std::atomic<int>& resumed;
        FeTask run() {
            co_await FeEventWaiter{sched, ev, node, rec};
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    Case c{sched, ev, node, rec, resumed};
    FeTask t = c.run();  // created on the TEST thread; outlives the starter
    sluice_async_test::EventSeam::arm_admission_before_final_check(sched);
    std::thread admission([&] { t.start();});
    sluice_async_test::EventSeam::wait_admission_paused(sched);
    // Paused INSIDE the CS: the admission thread HOLDS G+W here, so only
    // lock-free observations are legal on this thread (a deferred-depth
    // probe would deadlock BY CONSTRUCTION -- it takes G -- which is itself
    // the closed-window property under test).
    SLUICE_CHECK(rec.state.load() == FeRecord::State::unarmed);
    SLUICE_CHECK(node.is_registered());
    sluice_async_test::EventSeam::release_admission(sched);
    admission.join();
    // Released with SET not stored (the setter had no G): the ladder
    // authorized suspension and the record armed INSIDE the CS.
    SLUICE_CHECK(rec.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(node.is_registered());
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    ev.set();  // whole set()+drain linearizes AFTER the admission CS
    SLUICE_CHECK(node.outcome() == WaitOutcome::woken);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    SLUICE_CHECK(resumed.load() == 0);
    const std::size_t drained = drain_all(sched, resumed, guard_failures);
    SLUICE_CHECK(drained == 1);
    SLUICE_CHECK(resumed.load() == 1);  // exactly-once, no lost wake
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(rec.state.load() == FeRecord::State::consumed);
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
}

// ---- §21-C: set after eligibility commit -> exactly-once async publication
SLUICE_TEST_CASE(fe2_pov_async_set_exactly_once) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    Event ev(sched, false);
    WaitNode node;
    FeRecord rec;
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    struct Case {
        Scheduler& sched;
        Event& ev;
        WaitNode& node;
        FeRecord& rec;
        std::atomic<int>& resumed;
        FeTask run() {
            co_await FeEventWaiter{sched, ev, node, rec};
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    Case c{sched, ev, node, rec, resumed};
    FeTask t = c.run();
    t.start(); // suspends inside the co_await (record armed)

    SLUICE_CHECK(rec.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(node.is_registered());
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);  // not yet resolved

    ev.set();  // winner: drain resolves head -> publication edge defers
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    SLUICE_CHECK(node.outcome() == WaitOutcome::woken);
    SLUICE_CHECK(resumed.load() == 0);  // committed, not yet discharged

    const std::size_t drained = drain_all(sched, resumed, guard_failures);
    SLUICE_CHECK(drained == 1);
    SLUICE_CHECK(resumed.load() == 1);  // exactly one resume
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(rec.state.load() == FeRecord::State::consumed);
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);

    // A second set() is a no-op (previous=true): no second publication.
    ev.set();
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
    SLUICE_CHECK(resumed.load() == 1);
}

// ---- §21-D: cancel at the equivalent boundary -> Cancelled outcome ---------
SLUICE_TEST_CASE(fe2_pov_cancel_after_suspend) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    Event ev(sched, false);
    WaitNode node;
    FeRecord rec;
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    struct Case {
        Scheduler& sched;
        Event& ev;
        WaitNode& node;
        FeRecord& rec;
        std::atomic<int>& resumed;
        FeTask run() {
            co_await FeEventWaiter{sched, ev, node, rec};
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    Case c{sched, ev, node, rec, resumed};
    FeTask t = c.run();
    t.start();

    SLUICE_CHECK(rec.state.load() == FeRecord::State::armed);
    // Same production cancellation closure as the fiber frontend.
    SLUICE_CHECK(AsyncTestAccess::event_cancel_deferred_for_test(sched, ev, node));
    SLUICE_CHECK(node.outcome() == WaitOutcome::cancelled);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);

    const std::size_t drained = drain_all(sched, resumed, guard_failures);
    SLUICE_CHECK(drained == 1);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(rec.state.load() == FeRecord::State::consumed);
    SLUICE_CHECK(t.done());

    // Cancel of the now-terminal node is a loser: false, no publication.
    SLUICE_CHECK(!AsyncTestAccess::event_cancel_deferred_for_test(sched, ev, node));
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
}

// ---- §21-E: deadline already due -> inline Expired, no suspension ----------
SLUICE_TEST_CASE(fe2_pov_deadline_already_due) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    Event ev(sched, false);
    WaitNode node;
    FeRecord rec;
    std::atomic<int> resumed{0};

    sluice_async_test::TimerTestControl::enable_test_clock(sched);
    const Scheduler::deadline_t due_now = AsyncTestAccess::clock_now(sched);

    struct Case {
        Scheduler& sched;
        Event& ev;
        WaitNode& node;
        FeRecord& rec;
        Scheduler::deadline_t deadline;
        std::atomic<int>& resumed;
        FeTask run() {
            co_await FeEventWaiterTimed{sched, ev, node, rec, deadline};
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    Case c{sched, ev, node, rec, due_now, resumed};
    FeTask t = c.run();
    t.start(); // already-due closure: await_suspend returned false

    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(t.done());
    SLUICE_CHECK(node.outcome() == WaitOutcome::expired);
    SLUICE_CHECK(rec.state.load() == FeRecord::State::unarmed);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 0);
}

// ---- §21-F: deadline due after eligibility commit -> async Expired ---------
SLUICE_TEST_CASE(fe2_pov_deadline_due_after_suspend) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    Event ev(sched, false);
    WaitNode node;
    FeRecord rec;
    std::atomic<int> resumed{0};
    std::atomic<int> guard_failures{0};

    sluice_async_test::TimerTestControl::enable_test_clock(sched);
    const Scheduler::deadline_t deadline =
        AsyncTestAccess::clock_now(sched) + 50;  // due later

    struct Case {
        Scheduler& sched;
        Event& ev;
        WaitNode& node;
        FeRecord& rec;
        Scheduler::deadline_t deadline;
        std::atomic<int>& resumed;
        FeTask run() {
            co_await FeEventWaiterTimed{sched, ev, node, rec, deadline};
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    Case c{sched, ev, node, rec, deadline, resumed};
    FeTask t = c.run();
    t.start(); // suspends; timer armed through the ordinary deadline authority

    SLUICE_CHECK(rec.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(node.is_registered());

    sched.advance_clock(100);  // 100 >= 50: pump wins Expired -> defers
    SLUICE_CHECK(node.outcome() == WaitOutcome::expired);
    SLUICE_CHECK(AsyncTestAccess::deferred_depth_for_test(sched) == 1);
    SLUICE_CHECK(resumed.load() == 0);

    const std::size_t drained = drain_all(sched, resumed, guard_failures);
    SLUICE_CHECK(drained == 1);
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(guard_failures.load() == 0);
    SLUICE_CHECK(rec.state.load() == FeRecord::State::consumed);
    SLUICE_CHECK(t.done());
}

// ---- §22: no user code under authoritative locks ---------------------------
// The resumed body re-enters Scheduler authority (reset takes G) and probes
// G with a NON-BLOCKING try-lock. If the discharge path held G while
// resuming, the probe deterministically fails (no sleep, no deadlock).
SLUICE_TEST_CASE(fe2_pov_no_user_code_under_lock) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);
    Event ev(sched, false);
    WaitNode node;
    FeRecord rec;
    std::atomic<int> resumed{0};
    std::atomic<bool> g_free_at_resume{false};
    std::atomic<bool> reenter_ok{false};

    struct Case {
        Scheduler& sched;
        Event& ev;
        WaitNode& node;
        FeRecord& rec;
        std::atomic<int>& resumed;
        std::atomic<bool>& g_free_at_resume;
        std::atomic<bool>& reenter_ok;
        FeTask run() {
            co_await FeEventWaiter{sched, ev, node, rec};
            // §22 witness 1: the discharge holds NO authoritative lock.
            const bool free = AsyncTestAccess::try_lock_global_for_test(sched);
            g_free_at_resume.store(free, std::memory_order_release);
            if (free) AsyncTestAccess::unlock_global_for_test(sched);
            // §22 witness 2: the body RE-ENTERS primitive authority
            // (reset takes G) — legal only because discharge is lock-free.
            ev.reset();
            reenter_ok.store(true, std::memory_order_release);
            resumed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
    };
    Case c{sched, ev, node, rec, resumed, g_free_at_resume, reenter_ok};
    FeTask t = c.run();
    t.start();

    ev.set();
    std::atomic<int> guard_failures{0};
    const std::size_t drained = drain_all(sched, resumed, guard_failures);
    SLUICE_CHECK(drained == 1);
    SLUICE_CHECK(g_free_at_resume.load());  // G was FREE inside the resumed body
    SLUICE_CHECK(reenter_ok.load());        // re-entered G cleanly
    SLUICE_CHECK(resumed.load() == 1);
    SLUICE_CHECK(t.done());
}

// ---- Publication guard fail-closed direction (FE-1b L8 sensitivity) -------
SLUICE_TEST_CASE(fe2_pov_record_guard_unit) {
    // A fresh (unarmed) record refuses consumption: a mutated resolver that
    // publishes before the eligibility commit cannot sneak past the guard.
    FeRecord rec;
    SLUICE_CHECK(rec.state.load() == FeRecord::State::unarmed);
    SLUICE_CHECK(!rec.try_consume());
    SLUICE_CHECK(rec.state.load() == FeRecord::State::unarmed);

    // arm -> single consume -> second consume loses (exactly-once).
    rec.arm();
    SLUICE_CHECK(rec.state.load() == FeRecord::State::armed);
    SLUICE_CHECK(rec.try_consume());
    SLUICE_CHECK(rec.state.load() == FeRecord::State::consumed);
    SLUICE_CHECK(!rec.try_consume());
}
SLUICE_MAIN()
