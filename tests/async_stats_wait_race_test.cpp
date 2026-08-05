// Issue #67 P1 follow-up regression: AsyncStats fields are plain std::uint64_t
// (caller-owned, never atomic — see include/sluice/measurement.hpp), and
// access_mtx_ is their single serialized accounting domain (AGENTS.md §4.1,
// §13.1 leaf domain). The split-wait fix moved the PARK out of access_mtx_ but
// a follow-on review found that stats accounting had also leaked out:
//   - wait_calls was bumped BEFORE acquiring access_mtx_ in wait_one();
//   - completed_ops was bumped AFTER releasing access_mtx_ on every reap.
// Two wait_one() callers (or a wait_one() and a poll()) therefore raced on the
// same field with no lock. The existing drain-starvation regression proved the
// park/reachability contract but never produced a non-zero completed_ops under
// real concurrency — the producing poll ran after both participants had
// serialized — so TSan stayed green. This test forces the actual race classes:
//   (A) multiple wait_one() callers concurrently, all sharing one AsyncStats;
//   (B) wait_one() racing against concurrent poll() on the same context.
// Under the pre-fix code this is a C++ data race on wait_calls/completed_ops
// (TSan flags it). Under the fix every accounting access is inside access_mtx_
// and the final counters are exact.
//
// Cleanup discipline: EVERY exit path — success and every failure — routes
// through a single teardown that:
//   1. sets the shared stop flag,
//   2. resumes any armed ThreadPool pause gate,
//   3. calls ctx.interrupt_backend_waiters() so any parked wait_one() returns,
//   4. poll-drains ctx.outstanding() to zero (quiescent teardown requires it),
//      re-interrupting each round so a parked waiter re-loops and exits,
//   5. joins every participant thread,
//   6. resets every ready Completion (releases slots),
//   7. only then reports failure / leaves scope.
// If drain or a join cannot complete within a bounded deadline, the helper
// prints a precise diagnostic and std::abort()s — it NEVER lets a non-quiescent
// context destruction or a joinable-thread destructor take over, because that
// would surface as an opaque abort / UAF instead of the test's own message. No
// path detaches a thread (detached threads here still reference ctx /
// completions / stats / done-flags and would be a use-after-free).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace sluice::async;
using sluice::AsyncStats;
using sluice::IoError;
using sluice::Result;

namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(10);

class TempPath {
public:
    explicit TempPath(const char* tag) {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_stats_race_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(counter_++) + ".tmp"))
                    .string();
    }
    ~TempPath() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
    const std::string& path() const { return path_; }

private:
    std::string path_;
    static inline long counter_ = 0;
};

int open_temp(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "open_temp failed\n");
        std::exit(1);
    }
    return fd;
}

// Seed N bytes at offset 0 so each submitted read returns 1 real byte.
void seed_file(int fd, std::size_t n) {
    std::vector<std::byte> buf(n, std::byte{0xA7});
    while (!buf.empty()) {
        ssize_t w = ::pwrite(fd, buf.data(), buf.size(), 0);
        if (w <= 0) {
            std::fprintf(stderr, "seed_file pwrite failed\n");
            std::exit(1);
        }
        buf.erase(buf.begin(), buf.begin() + w);
    }
}

// Bounded join of one thread: polls a done flag with short yields until the
// deadline, then blocks in join() once the flag is observed set. The thread is
// REQUIRED to set its done flag on every path (including stop-signal exit);
// the helper then joins. Never detaches — see the file header.
bool join_bounded(std::thread& t, std::atomic<bool>& done,
                  std::chrono::steady_clock::time_point deadline) {
    if (!t.joinable()) return true;
    while (!done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    t.join();
    return true;
}

// Quiescent teardown for a context that MAY still have outstanding work /
// parked waiters. Drains outstanding to zero by poll()ing and re-interrupting
// (a parked waiter needs an interrupt to re-loop, observe stop, and exit;
// poll() alone cannot wake it). If drain cannot complete within the deadline,
// the failure is unrecoverable: print a precise diagnostic and abort rather
// than let a non-quiescent context destructor or a joinable-thread destructor
// surface as an opaque abort. Returns true on a clean drain.
bool drain_to_quiescent(AsyncIoContext& ctx,
                        std::chrono::steady_clock::time_point deadline) {
    while (ctx.outstanding() != 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        ctx.poll();
        // A waiter parked between interrupt rounds would otherwise never
        // re-check stop; re-interrupt each round so it re-loops and exits.
        ctx.interrupt_backend_waiters();
        std::this_thread::yield();
    }
    return true;
}

// Hard-fail helper: when teardown cannot complete safely (drain or join
// wedged), print a precise diagnostic and abort so the CI log shows the exact
// reason instead of an opaque non-quiescent-destruction abort / UAF.
void abort_with(const char* why) {
    std::fprintf(stderr, "async_stats_wait_race_test: %s; aborting\n", why);
    std::abort();
}

}  // namespace

SLUICE_MAIN()

// Race class A: many wait_one() callers concurrently, sharing one AsyncStats.
// Each caller submits one read first (so wait_one has real work to reap), then
// waits. The pre-fix code bumped wait_calls outside access_mtx_, so concurrent
// wait_one() callers raced on wait_calls. The fix puts every accounting access
// inside access_mtx_; the final counters are exact.
SLUICE_TEST_CASE(wait_one_concurrent_callers_no_stats_race) {
    constexpr std::size_t kCallers = 4;
    constexpr std::size_t kOps = kCallers;

    TempPath tp("A");
    int fd = open_temp(tp.path());
    seed_file(fd, kOps);

    // workers == kOps so every submitted read can be reaped without needing a
    // parked waiter to first wake and poll — keeps the liveness proof
    // unambiguous while still exercising the wait_calls/completed_ops race.
    ThreadPoolConfig cfg;
    cfg.request_capacity = 8;
    cfg.worker_count = kOps;
    AsyncStats stats;
    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>(cfg), &stats);

    std::vector<std::byte> buf(kOps, std::byte{});
    std::vector<Completion<std::size_t>> completions(kOps);

    // The stop flag is the single orderly-shutdown signal for every caller.
    // On ANY exit path (success or failure) the teardown sets it +
    // interrupt_backend_waiters() so parked wait_one() calls return; callers
    // observe stop and exit, so join() always succeeds and no thread outlives
    // the scope (no detach anywhere).
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> submitted{0};
    std::vector<std::atomic<bool>> done_flags(kCallers);
    for (auto& f : done_flags) f.store(false, std::memory_order_release);

    std::vector<std::thread> callers;
    callers.reserve(kCallers);
    for (std::size_t i = 0; i < kCallers; ++i) {
        callers.emplace_back([&, i] {
            ReadOp op{fd, buf.data() + i, 1, static_cast<std::uint64_t>(i)};
            if (ctx.submit_read(op, completions[i]).has_value()) {
                submitted.fetch_add(1, std::memory_order_release);
                // Each caller calls wait_one() at least once UNCONDITIONALLY —
                // BEFORE the ready() check. This is what makes
                // wait_calls >= kCallers a CAUSAL guarantee: without it, a
                // caller whose Completion was reaped by another participant
                // before it first entered the loop would observe ready() and
                // skip wait_one() entirely, making the lower bound
                // schedule-dependent. After the unconditional first call, the
                // loop only re-enters while not-yet-ready and not-stopped.
                if (!stop.load(std::memory_order_acquire)) {
                    auto first = ctx.wait_one();
                    if (first.has_value()) {
                        while (!stop.load(std::memory_order_acquire) &&
                               !completions[i].ready()) {
                            auto r = ctx.wait_one();
                            if (!r.has_value()) break;
                        }
                    }
                }
            }
            done_flags[i].store(true, std::memory_order_release);
        });
    }

    const char* fail_msg = nullptr;
    const auto join_deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    for (std::size_t i = 0; i < kCallers; ++i) {
        if (!join_bounded(callers[i], done_flags[i], join_deadline)) {
            fail_msg = "wait_one caller did not finish in time";
            break;
        }
    }

    if (fail_msg == nullptr) {
        SLUICE_CHECK(submitted.load(std::memory_order_acquire) == kOps);

        // Drain any straggler via poll so completed_ops reflects every reap.
        // poll() is the other writer of completed_ops; doing this here also
        // exercises the wait_one-vs-poll accounting consistency under the same
        // lock.
        const auto drain_deadline =
            std::chrono::steady_clock::now() + kWaitTimeout;
        if (!drain_to_quiescent(ctx, drain_deadline)) {
            fail_msg = "did not drain to zero in time";
        }
    }

    // SINGLE teardown for every path. On failure we MUST drain + join before
    // reporting: otherwise SLUICE_FAIL returns, the locals destruct, and the
    // context destruction sees outstanding != 0 / a joinable thread and either
    // fail-fasts opaquely or UAFs. On the success path the callers are already
    // joined and outstanding is 0, so this is cheap.
    if (fail_msg != nullptr) {
        stop.store(true, std::memory_order_release);
        ctx.interrupt_backend_waiters();
        if (!drain_to_quiescent(ctx,
                                std::chrono::steady_clock::now() +
                                    kWaitTimeout)) {
            abort_with("case A failure path: could not drain to quiescent");
        }
        for (auto& t : callers) {
            if (t.joinable()) t.join();
        }
        SLUICE_FAIL(fail_msg);
    }

    for (std::size_t i = 0; i < kOps; ++i) {
        SLUICE_CHECK(completions[i].ready());
        SLUICE_CHECK(completions[i].result().has_value());
        SLUICE_CHECK(completions[i].result().value() == 1);
    }

    // Exact-value assertion: completed_ops is THE counter the split-wait race
    // raced on (reap-path bump raced vs concurrent poll()). The fix puts the
    // bump inside access_mtx_, so completed_ops MUST be exactly kOps — never
    // a lost increment, never a double count. This is the load-bearing
    // correctness assertion for the race class the fix targets.
    //
    // wait_calls is asserted as a lower bound, not exact: the loop shape makes
    // the call count schedule-dependent (a caller re-enters wait_one when its
    // Completion was reaped by another participant). The fix makes wait_calls
    // correct (bumped exactly once per wait_one call, under the lock); the TSan
    // run of THIS test proves the wait_calls race is gone (on the pre-fix code
    // TSan flagged it at async_io_context.cpp:181). An exact schedule-dependent
    // value would make the test flaky for reasons unrelated to the race.
    SLUICE_CHECK(stats.submitted_ops == kOps);
    SLUICE_CHECK(stats.completed_ops == kOps);
    // wait_calls >= kCallers is a CAUSAL guarantee here, not schedule-dependent:
    // each caller calls wait_one() at least once unconditionally before it ever
    // checks ready() / stop. A regression that undercounts wait_calls (e.g.
    // re-introducing the lock-free bump) would still trip TSan; this bound is
    // the runtime backstop.
    SLUICE_CHECK(stats.wait_calls >= kCallers);

    // Release slots before the context goes out of scope (quiescent teardown
    // requires slot_in_use == 0).
    for (std::size_t i = 0; i < kOps; ++i) {
        completions[i].reset();
    }
    ::close(fd);
}

// Race class B: wait_one() racing concurrent poll() on the same context, both
// touching completed_ops. The pre-fix code released access_mtx_ BEFORE bumping
// completed_ops in the split-wait reap path, so a concurrent poll()'s
// completed_ops += n (under access_mtx_) raced with the just-released
// completed_ops += n in wait_one(). The fix does the bump inside the lock.
//
// To actually exercise the wait_one() reap path (the >0-return branch), this
// case uses the ThreadPool running-pause gate to HOLD one op in `running` while
// the waiter is parked: only after the waiter is observed parked (via the
// wait-phase flag) do we release the gate, so the worker's completion wakes the
// parked waiter (progress wake) and the waiter's wait_one() returns >0. That
// makes waiter_reaped > 0 a deterministic guarantee, not a race between
// waiter / poller / main.
SLUICE_TEST_CASE(wait_one_and_poll_concurrent_no_stats_race) {
    constexpr std::size_t kOps = 4;

    TempPath tp("B");
    int fd = open_temp(tp.path());
    seed_file(fd, kOps);

    // 1 worker: with the running-pause gate armed, exactly one op is held in
    // `running` until the gate releases, so the wake goes to the parked waiter
    // (deterministic handoff rather than a 3-way race against poller/main).
    ThreadPoolConfig cfg;
    cfg.request_capacity = 8;
    cfg.worker_count = 1;
    ThreadPoolBackend::WorkerRunningPauseGate gate;
    auto backend_owned = std::make_unique<ThreadPoolBackend>(cfg);
    ThreadPoolBackend* backend_raw = backend_owned.get();
    backend_raw->set_running_pause_gate(&gate);
    // The wait-phase flag fires the instant before the waiter blocks in the
    // ready cv — it is the deterministic proof that the waiter is ACTUALLY
    // parked (not merely between snapshot and poll). Waiting on it before
    // releasing the gate guarantees the worker's signal_progress() wakes a
    // parked waiter, so the waiter's wait_one() returns >0.
    std::atomic<bool> wait_phase_entered{false};
    backend_raw->set_wait_phase_flag_for_test(&wait_phase_entered);
    AsyncStats stats;
    AsyncIoContext ctx(std::move(backend_owned), &stats);

    std::vector<std::byte> buf(kOps, std::byte{});
    std::vector<Completion<std::size_t>> completions(kOps);

    for (std::size_t i = 0; i < kOps; ++i) {
        ReadOp op{fd, buf.data() + i, 1, static_cast<std::uint64_t>(i)};
        SLUICE_CHECK(ctx.submit_read(op, completions[i]).has_value());
    }

    std::atomic<bool> stop{false};
    std::atomic<bool> poller_may_start{false};
    std::atomic<std::size_t> waiter_reaped{0};
    std::atomic<bool> waiter_done{false};
    std::atomic<bool> poller_done{false};

    // Thread objects declared up-front (default-constructed = not-yet-joined)
    // so the teardown lambda below can capture them by reference and be valid
    // on every exit path — including the early-failure paths that fire BEFORE
    // the threads are started (waiter/poller stay non-joinable in that case,
    // and teardown's joinable() guards make that a no-op).
    std::thread waiter;
    std::thread poller;

    // Orderly teardown applied on EVERY exit path (success + every failure).
    // Captures by reference so it can touch stop, the gate, ctx, both threads,
    // and completions. It is idempotent (safe to call from multiple points).
    auto teardown = [&](const char* why) {
        stop.store(true, std::memory_order_release);
        poller_may_start.store(true, std::memory_order_release);
        // Release any armed gate so a paused worker can run and the op can be
        // reaped by the drain below (otherwise the worker is stuck forever).
        gate.resume.store(true, std::memory_order_release);
        backend_raw->set_running_pause_gate(nullptr);
        backend_raw->set_wait_phase_flag_for_test(nullptr);
        // Wake any parked waiter so it observes stop and exits; re-interrupt
        // each drain round so a re-parked waiter re-loops.
        ctx.interrupt_backend_waiters();
        if (!drain_to_quiescent(ctx,
                                std::chrono::steady_clock::now() +
                                    kWaitTimeout)) {
            abort_with("case B teardown: could not drain to quiescent");
        }
        // Join both participants. waiter re-loops on stop via the interrupt;
        // poller observes stop directly. If a join still fails after stop +
        // interrupt, the participant is wedged in a way the test cannot recover
        // from safely — abort loudly (the contract says this never happens).
        const auto join_deadline =
            std::chrono::steady_clock::now() + kWaitTimeout;
        if (waiter.joinable() &&
            !join_bounded(waiter, waiter_done, join_deadline)) {
            abort_with("case B teardown: waiter unjoinable after stop+interrupt");
        }
        if (poller.joinable() &&
            !join_bounded(poller, poller_done, join_deadline)) {
            abort_with("case B teardown: poller unjoinable after stop");
        }
        // Release any ready slots (quiescent teardown requires slot_in_use==0).
        for (auto& c : completions) {
            if (c.ready()) c.reset();
        }
        if (why != nullptr) SLUICE_FAIL(why);
    };

    // Wait for the first op to reach the running gate (paused pre-syscall), so
    // no readiness signal can arrive before the waiter parks. On failure route
    // through the single teardown (which drains + joins before failing).
    {
        const auto gate_deadline =
            std::chrono::steady_clock::now() + kWaitTimeout;
        while (!gate.paused.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= gate_deadline) {
                teardown("op did not reach the running gate in time");
                return;
            }
            std::this_thread::yield();
        }
    }

    // waiter_reaped records the count of completions the waiter's wait_one()
    // ACTUALLY reaped (the >0-return path). To make waiter_reaped > 0 a
    // DETERMINISTIC guarantee (not a race between waiter / poller / main), the
    // first op is handed to the waiter with NO concurrent poller: the gate
    // holds op0 in `running` while the waiter parks (wait-phase flag fires),
    // then the gate releases and the waiter's wait_one() reaps op0
    // uncontested. Only AFTER that proof does the poller start, and the
    // remaining ops are reaped under genuine wait-vs-poll contention (the
    // completed_ops race this test exists to exercise).
    waiter = std::thread([&] {
        while (!stop.load(std::memory_order_acquire)) {
            auto r = ctx.wait_one();
            if (!r.has_value()) break;
            if (r.value() > 0) {
                std::size_t before = waiter_reaped.fetch_add(
                    r.value(), std::memory_order_release);
                // Once the waiter has deterministically reaped op0, let the
                // poller start so the remaining ops race against wait_one().
                if (before == 0) {
                    poller_may_start.store(true, std::memory_order_release);
                }
            }
        }
        waiter_done.store(true, std::memory_order_release);
    });
    poller = std::thread([&] {
        // Do not poll until the waiter has proven it reaped op0 — this keeps
        // waiter_reaped > 0 deterministic without weakening the wait-vs-poll
        // contention for the remaining ops.
        while (!poller_may_start.load(std::memory_order_acquire)) {
            if (stop.load(std::memory_order_acquire)) {
                poller_done.store(true, std::memory_order_release);
                return;
            }
            std::this_thread::yield();
        }
        while (!stop.load(std::memory_order_acquire)) {
            ctx.poll();
            std::this_thread::yield();
        }
        poller_done.store(true, std::memory_order_release);
    });

    // Wait for the wait-phase flag (the waiter is parked in the ready cv), THEN
    // release the gate so op0 completes and its signal_progress() wakes the
    // parked waiter. With the poller gated on poller_may_start, the waiter
    // reaps op0 uncontested — so waiter_reaped > 0 is guaranteed.
    {
        const auto park_deadline =
            std::chrono::steady_clock::now() + kWaitTimeout;
        while (!wait_phase_entered.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= park_deadline) {
                teardown("waiter never parked (wait-phase flag never fired)");
                return;
            }
            std::this_thread::yield();
        }
    }
    // Disarm both seams and release the gate so real syscalls run. op0's reap
    // goes to the parked waiter; the poller starts once waiter_reaped > 0.
    backend_raw->set_running_pause_gate(nullptr);
    backend_raw->set_wait_phase_flag_for_test(nullptr);
    gate.resume.store(true, std::memory_order_release);

    // Wait for the waiter to deterministically reap op0 BEFORE the main thread
    // polls. Otherwise the main thread wins the race for op0 and waiter_reaped
    // stays 0 — defeating the whole point of the deterministic handoff.
    {
        const auto handoff_deadline =
            std::chrono::steady_clock::now() + kWaitTimeout;
        while (waiter_reaped.load(std::memory_order_acquire) == 0) {
            if (std::chrono::steady_clock::now() >= handoff_deadline) {
                teardown("waiter did not reap op0 after gate release");
                return;
            }
            std::this_thread::yield();
        }
    }

    // Drain the remaining ops under genuine wait-vs-poll contention.
    {
        const auto reap_deadline =
            std::chrono::steady_clock::now() + kWaitTimeout;
        while (ctx.outstanding() != 0) {
            if (std::chrono::steady_clock::now() >= reap_deadline) {
                teardown("did not reap all ops in time");
                return;
            }
            ctx.poll();
            std::this_thread::yield();
        }
    }

    // Capture Completion-readiness BEFORE teardown (teardown resets ready
    // slots back to idle). Stats counters, by contrast, MUST be read AFTER
    // teardown: until both participants are joined the poller/waiter may still
    // be writing them (under access_mtx_), so a lock-free read here would race
    // with those writes. After teardown no writer remains.
    bool all_ready_with_one = true;
    for (std::size_t i = 0; i < kOps; ++i) {
        if (!completions[i].ready() || !completions[i].result().has_value() ||
            completions[i].result().value() != 1) {
            all_ready_with_one = false;
            break;
        }
    }

    // Success path teardown: stop both participants, drain any tail, join,
    // release slots. Same routine as failure — single exit discipline. After
    // it returns no participant thread is running, so stats counters are
    // quiescent and can be read without a lock.
    teardown(nullptr);

    // Now assert against post-teardown state. waiter_reaped is atomic; the
    // stats fields are read only after both threads have joined (no writers).
    SLUICE_CHECK(all_ready_with_one);
    // The waiter MUST have reaped at least one completion via wait_one() —
    // otherwise the wait_one() reap-path bump of completed_ops (the exact
    // statement this test exists to prove race-free) was never exercised.
    SLUICE_CHECK(waiter_reaped.load(std::memory_order_acquire) > 0);
    // Exact-value assertion: completed_ops MUST equal the number of reaped
    // ops. Under the pre-fix split-wait code the concurrent poll() race could
    // lose increments (TSan flags the non-atomic write/write); the fix makes
    // this exact because every reap-and-bump is one critical section.
    SLUICE_CHECK(stats.submitted_ops == kOps);
    SLUICE_CHECK(stats.completed_ops == kOps);

    ::close(fd);
}
