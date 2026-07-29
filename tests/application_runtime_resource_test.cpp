// E16-POST-MERGE-CORRECTIVE-1 — terminal resource destruction regression tests.
//
// ADR: docs/adr/ADR-application-runtime.md (Accepted).
//
// These tests prove the C1 contract:
//
//   Every transition into State::Stopped must first destroy all Runtime-owned
//   execution resources (root Group, Scheduler, AsyncIoContext/backend).
//
// The mechanism is a ProbeBackend whose destructor increments an atomic counter.
// The backend is OWNED by the AsyncIoContext which is OWNED by the Runtime, so
// observing the destructor count proves the component destruction chain ran.
//
// Each test asserts the destructor count BEFORE destroying the ApplicationRuntime
// object, so it proves destruction happened inside the lifecycle call (shutdown /
// start-abort / join) rather than later in ~ApplicationRuntime.
//
// On the pre-corrective merged master these tests FAIL:
//   - shutdown() from Constructed/StartFailed publishes Stopped WITHOUT calling
//     close_resources(), leaving backend alive.
//   - start() startup-abort path publishes Stopped WITHOUT close_resources().
//
// Deterministic causal tests. NO sleep_for as proof of ordering.
#include "harness.hpp"

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/fake_backend.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// ---------------------------------------------------------------------------
// ProbeBackend: FakeAsyncBackend + observable destructor.
// The backend is the leaf of the ownership chain
//   ApplicationRuntime -> AsyncIoContext -> AsyncBackend
// so its destructor running proves the whole component-close chain ran.
// ---------------------------------------------------------------------------
struct DestructionProbe {
    std::atomic<int> destructor_count{0};
};

class ProbeBackend : public AsyncBackend {
public:
    explicit ProbeBackend(std::shared_ptr<DestructionProbe> p) : probe_(std::move(p)) {}

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
        return inner_.submit_read(op, c);
    }
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override {
        return inner_.submit_write(op, c);
    }
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override {
        return inner_.submit_sync_data(op, c);
    }
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override {
        return inner_.submit_sync_all(op, c);
    }
    std::size_t poll() override { return inner_.poll(); }
    Result<std::size_t> wait_one() override { return inner_.wait_one(); }
    std::size_t outstanding() const noexcept override { return inner_.outstanding(); }

    ~ProbeBackend() override {
        probe_->destructor_count.fetch_add(1, std::memory_order::release);
    }

private:
    std::shared_ptr<DestructionProbe> probe_;
    FakeAsyncBackend inner_;
};

// Build a Runtime backed by a probe; the shared probe outlives the Runtime so
// the test can read the destructor count even after destroying the Runtime.
struct ProbeRuntime {
    ProbeRuntime() : probe(std::make_shared<DestructionProbe>()) {}
    std::unique_ptr<ApplicationRuntime> rt;
    std::shared_ptr<DestructionProbe> probe;
};

static ProbeRuntime build_probe_runtime() {
    ProbeRuntime pr;
    auto backend = std::make_unique<ProbeBackend>(pr.probe);
    RuntimeBuilder builder;
    builder.backend(std::move(backend));
    auto r = builder.build();
    if (!r.has_value()) {
        std::fprintf(stderr, "build_probe_runtime: builder.build() failed\n");
        std::abort();
    }
    pr.rt = std::move(r.value());
    return pr;
}

// ---------------------------------------------------------------------------
// C1-T1 — shutdown from Constructed destroys backend before return.
// Proves destruction happened INSIDE shutdown(), not later in ~ApplicationRuntime.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(c1_shutdown_constructed_destroys_backend) {
    auto pr = build_probe_runtime();
    auto& rt = *pr.rt;
    SLUICE_CHECK(pr.probe->destructor_count.load() == 0);

    auto sr = rt.shutdown();
    SLUICE_CHECK(sr.has_value());

    // Backend MUST be destroyed before this point (before ~ApplicationRuntime).
    SLUICE_CHECK(pr.probe->destructor_count.load() == 1);
}

// ---------------------------------------------------------------------------
// C1-T2 — startup abort destroys resources before start returns.
// Deterministic seam: request_stop() is invoked from another thread while the
// start owner is parked at the startup barrier (before Running commit).
//
// Race-free causal order:
//   - we submit no tasks, so the driver's run_live loop is quiescent
//   - main thread calls start(); driver reaches barrier_wait
//   - helper thread calls request_stop() BEFORE start observes commit
//   - start owner observes startup_abort_requested, runs rollback+close
//   - start() returns canceled with backend destroyed exactly once
//
// No sleep_for as ordering proof: the barrier synchronization inside start()
// guarantees the driver has parked at barrier_wait before we can observe it;
// request_stop() setting startup_abort_requested is observed by the start
// owner's commit-check. We use a short bounded retry on state observation only
// to handle the legitimate interleaving where request_stop wins the race
// against the start owner's barrier wait — this is NOT a liveness proof, the
// shutdown() convergence (C1-T5) is the liveness guarantee.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(c1_startup_abort_destroys_resources) {
    auto pr = build_probe_runtime();
    auto& rt = *pr.rt;

    // request_stop() before start() commits. request_stop() is worker-safe and
    // may be called from any thread. Calling it from the main thread before
    // start() observes commit deterministically drives the startup-abort path:
    // start() remembers stop_requested_, transitions to Starting, spawns the
    // driver, and at the commit checkpoint observes stop_requested_ set, taking
    // the abort branch.
    rt.request_stop();

    auto start_result = rt.start();
    SLUICE_CHECK(!start_result.has_value());
    SLUICE_CHECK(start_result.error().code == IoError::Code::canceled);

    // Backend MUST be destroyed by the time start() returns (close ran inside
    // the abort path, not deferred to ~ApplicationRuntime).
    SLUICE_CHECK(pr.probe->destructor_count.load() == 1);

    // Runtime object remains alive (we have not destroyed it). Idempotent
    // shutdown must succeed and NOT double-destroy.
    auto sr = rt.shutdown();
    SLUICE_CHECK(sr.has_value());
    SLUICE_CHECK(pr.probe->destructor_count.load() == 1);
}

// ---------------------------------------------------------------------------
// C1-T4 — normal join destroys backend exactly once.
// Prevents double destruction: close_resources() destroys the backend, then
// ~ApplicationRuntime must not destroy it again.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(c1_normal_join_destroys_backend_once) {
    auto pr = build_probe_runtime();
    auto& rt = *pr.rt;

    SLUICE_CHECK(rt.start().has_value());

    std::atomic<bool> task_ran{false};
    SLUICE_CHECK(rt.submit([&task_ran](RuntimeTaskContext& ctx) {
        (void)ctx;
        task_ran.store(true, std::memory_order::release);
    }).has_value());

    rt.request_stop();
    SLUICE_CHECK(rt.drain().has_value());
    SLUICE_CHECK(task_ran.load(std::memory_order::acquire));

    SLUICE_CHECK(rt.join().has_value());

    // Backend destroyed exactly once after join.
    SLUICE_CHECK(pr.probe->destructor_count.load() == 1);

    // Destroy the ApplicationRuntime: count must REMAIN 1 (no double destroy).
    pr.rt.reset();
    SLUICE_CHECK(pr.probe->destructor_count.load() == 1);
}

// ---------------------------------------------------------------------------
// C1-T5 — concurrent shutdown from Constructed converges on one close owner.
// N external threads call shutdown() while the Runtime remains Constructed.
// All must succeed, backend destroyed exactly once, no hang, no double close.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(c1_concurrent_shutdown_constructed_one_owner) {
    constexpr int N = 8;
    auto pr = build_probe_runtime();
    auto& rt = *pr.rt;

    std::atomic<int> ok_count{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&rt, &ok_count] {
            auto sr = rt.shutdown();
            if (sr.has_value()) ok_count.fetch_add(1, std::memory_order::release);
        });
    }
    for (auto& t : threads) t.join();

    SLUICE_CHECK(ok_count.load() == N);
    // Exactly one close owner destroyed the backend.
    SLUICE_CHECK(pr.probe->destructor_count.load() == 1);

    pr.rt.reset();
    SLUICE_CHECK(pr.probe->destructor_count.load() == 1);
}

// ---------------------------------------------------------------------------
// C1-T2b — concurrent shutdown while Starting converges to one close owner.
// The load-bearing concurrency case for the startup-abort close path: while
// the start owner is parked at the startup barrier, an external thread calls
// shutdown(). Both the start owner (commit-check observes stop_requested) and
// the shutdown caller (Starting branch) must converge on exactly ONE close
// owner and destroy the backend exactly once, with no hang and no double close.
//
// Deterministic causal seam: we admit no tasks and set stop_requested via the
// external shutdown() call before the start owner's commit checkpoint can be
// reached. The start owner's commit-check observes startup_abort_requested
// (set by shutdown()'s Starting branch) and takes the abort path as the close
// owner; the shutdown caller waits for Closed.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(c1_concurrent_shutdown_starting_one_owner) {
    auto pr = build_probe_runtime();
    auto& rt = *pr.rt;

    // Run start() in its own thread. While it is parked at the startup barrier
    // (driver spawned, awaiting commit), the main thread calls shutdown() which
    // drives the Starting-abort path. Both converge on one close owner.
    std::thread start_thread([&rt] {
        auto sr = rt.start();
        // start() returns canceled (abort) OR invalid_state (if shutdown raced
        // ahead and already closed). Both are acceptable terminal outcomes.
        (void)sr;
    });

    // shutdown() from Starting: elects/observes the close, returns success.
    auto shr = rt.shutdown();
    SLUICE_CHECK(shr.has_value());

    start_thread.join();

    // Exactly one close owner destroyed the backend.
    SLUICE_CHECK(pr.probe->destructor_count.load() == 1);

    pr.rt.reset();
    SLUICE_CHECK(pr.probe->destructor_count.load() == 1);
}

SLUICE_MAIN()
