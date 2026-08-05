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
// Cleanup discipline: the failure path NEVER detaches a participant thread.
// Detached threads here still hold references to ctx / completions / stats /
// the done-flags; once SLUICE_FAIL returns and the locals begin destructing,
// a detached thread still calling ctx.wait_one() would be a use-after-free,
// and destroying a context with a parked waiter would trip the backend's
// non-quiescent-destruction fail-fast — turning a clean bounded failure into
// an unexplained abort. Instead every failure/teardown path:
//   1. sets a shared stop flag,
//   2. calls ctx.interrupt_backend_waiters() so any parked wait_one() returns,
//   3. drains outstanding work via poll() so accepted requests reach terminal
//      and slots release (quiescent teardown requires outstanding == 0),
//   4. joins every thread,
//   5. only then reports failure / leaves scope.
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
    // On the failure path we set it and interrupt_backend_waiters() so any
    // parked wait_one() returns; callers observe stop and exit, so join()
    // always succeeds and no thread outlives the scope (no detach anywhere).
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
                // Each caller calls wait_one() at least once UNCONDITIONALLY,
                // so wait_calls >= kCallers is a causal guarantee, not a
                // schedule-dependent hope. Without this, a caller whose
                // Completion was reaped by another participant before it
                // re-checked ready() could skip the loop entirely and never
                // call wait_one() at all.
                for (;;) {
                    if (stop.load(std::memory_order_acquire)) break;
                    if (completions[i].ready()) break;
                    auto r = ctx.wait_one();
                    if (!r.has_value()) break;
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
        while (ctx.outstanding() != 0) {
            if (std::chrono::steady_clock::now() >= drain_deadline) {
                fail_msg = "did not drain to zero in time";
                break;
            }
            ctx.poll();
            std::this_thread::yield();
        }
    }

    // Orderly shutdown on ANY failure: set stop, wake parked waiters, drain
    // accepted work so the context can be destroyed quiescently, then JOIN
    // every thread. NEVER detach — a detached caller still references ctx /
    // completions / stats / done_flags and would be a use-after-free once the
    // locals destruct. (On the success path stop is still set + interrupt
    // issued here as the single teardown point — callers have already set
    // done_flags, so join is immediate.)
    if (fail_msg != nullptr) {
        stop.store(true, std::memory_order_release);
        ctx.interrupt_backend_waiters();
        const auto drain_deadline =
            std::chrono::steady_clock::now() + kWaitTimeout;
        while (ctx.outstanding() != 0) {
            if (std::chrono::steady_clock::now() >= drain_deadline) break;
            ctx.poll();
            std::this_thread::yield();
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
    // each caller calls wait_one() at least once unconditionally before
    // observing its own ready() / stop. A regression that undercounts wait_calls
    // (e.g. re-introducing the lock-free bump) would still trip TSan; this bound
    // is the runtime backstop.
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
// the waiter is parked: only after the waiter is observed parked do we release
// the gate, so the worker's completion wakes the parked waiter (progress wake)
// and the waiter's wait_one() returns >0. That makes waiter_reaped > 0 a
// deterministic guarantee, not a race between waiter / poller / main.
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

    // Wait for the first op to reach the running gate (paused pre-syscall), so
    // no readiness signal can arrive before the waiter parks.
    const char* fail_msg = nullptr;
    const auto gate_deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (!gate.paused.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= gate_deadline) {
            fail_msg = "op did not reach the running gate in time";
            break;
        }
        std::this_thread::yield();
    }
    if (fail_msg != nullptr) {
        backend_raw->set_running_pause_gate(nullptr);
        backend_raw->set_wait_phase_flag_for_test(nullptr);
        gate.resume.store(true, std::memory_order_release);
        SLUICE_FAIL(fail_msg);
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
    std::atomic<bool> stop{false};
    std::atomic<bool> poller_may_start{false};
    std::atomic<std::size_t> waiter_reaped{0};
    std::atomic<bool> waiter_done{false};
    std::atomic<bool> poller_done{false};
    std::thread waiter([&] {
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
    std::thread poller([&] {
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
                fail_msg = "waiter never parked (wait-phase flag never fired)";
                break;
            }
            std::this_thread::yield();
        }
    }
    // Disarm both seams and release the gate so real syscalls run. op0's reap
    // goes to the parked waiter; the poller starts once waiter_reaped > 0.
    backend_raw->set_running_pause_gate(nullptr);
    backend_raw->set_wait_phase_flag_for_test(nullptr);
    gate.resume.store(true, std::memory_order_release);

    if (fail_msg == nullptr) {
        // First: wait for the waiter to deterministically reap op0 BEFORE the
        // main thread polls. Otherwise the main thread (or poller) wins the
        // race for op0 and waiter_reaped stays 0 — defeating the whole point
        // of the deterministic handoff.
        const auto handoff_deadline =
            std::chrono::steady_clock::now() + kWaitTimeout;
        while (waiter_reaped.load(std::memory_order_acquire) == 0) {
            if (std::chrono::steady_clock::now() >= handoff_deadline) {
                fail_msg = "waiter did not reap op0 after gate release";
                break;
            }
            std::this_thread::yield();
        }
    }

    if (fail_msg == nullptr) {
        const auto reap_deadline =
            std::chrono::steady_clock::now() + kWaitTimeout;
        while (ctx.outstanding() != 0) {
            if (std::chrono::steady_clock::now() >= reap_deadline) {
                fail_msg = "did not reap all ops in time";
                break;
            }
            ctx.poll();
            std::this_thread::yield();
        }
    }

    // Orderly shutdown: set stop, interrupt parked waiters, then join both
    // participants. NEVER detach — they reference ctx / completions / stats.
    stop.store(true, std::memory_order_release);
    ctx.interrupt_backend_waiters();
    const auto join_deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    bool waiter_joined = join_bounded(waiter, waiter_done, join_deadline);
    bool poller_joined = join_bounded(poller, poller_done, join_deadline);
    if (fail_msg == nullptr && (!waiter_joined || !poller_joined)) {
        fail_msg = "participant did not finish after stop signal";
    }
    // If a join still failed after stop + interrupt, the participant is wedged
    // in a way the test cannot recover from safely — surface it loudly. (Under
    // the contract this never happens: interrupt wakes every parked waiter.)
    if (!waiter_joined && waiter.joinable()) {
        std::fprintf(stderr,
                     "async_stats_wait_race_test B: waiter unjoinable after "
                     "stop+interrupt; aborting to avoid scoped-use-after-free\n");
        std::abort();
    }
    if (!poller_joined && poller.joinable()) {
        std::fprintf(stderr,
                     "async_stats_wait_race_test B: poller unjoinable after "
                     "stop; aborting to avoid scoped-use-after-free\n");
        std::abort();
    }
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);

    for (std::size_t i = 0; i < kOps; ++i) {
        SLUICE_CHECK(completions[i].ready());
        SLUICE_CHECK(completions[i].result().has_value());
        SLUICE_CHECK(completions[i].result().value() == 1);
        completions[i].reset();
    }

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
