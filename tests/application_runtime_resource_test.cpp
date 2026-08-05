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
    // Issue #67: forward the split wait capability (the Runtime rejects
    // backends without it at build).
    BackendWaitSource* wait_source() noexcept override { return inner_.wait_source(); }

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
// Tests the stop-before-start path: request_stop() is called before start(),
// so start() observes stop_requested_ at entry, takes the direct-close path
// without spawning the driver, and returns canceled with backend destroyed.
// The Starting-abort path (driver spawned, then stop requested) is tested
// by C1-T2b (c1_concurrent_shutdown_starting_one_owner).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(c1_startup_abort_destroys_resources) {
    auto pr = build_probe_runtime();
    auto& rt = *pr.rt;

    rt.request_stop();

    auto start_result = rt.start();
    SLUICE_CHECK(!start_result.has_value());
    SLUICE_CHECK(start_result.error().code == IoError::Code::canceled);

    // Backend MUST be destroyed by the time start() returns.
    SLUICE_CHECK(pr.probe->destructor_count.load() == 1);

    // Runtime object remains alive. Idempotent shutdown must succeed and
    // NOT double-destroy.
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
// C1-T2b — concurrent stop while Starting converges to one close owner.
// The load-bearing concurrency case for the startup-abort close path: while
// the start owner is parked at the commit checkpoint, an external thread calls
// request_stop(). The start owner then observes stop_requested_ and takes the
// abort path, returning canceled. Both the start owner (commit-check observes
// stop_requested_) and the shutdown caller (Starting branch) must converge on
// exactly ONE close owner and destroy the backend exactly once, with no hang
// and no double close.
//
// Deterministic causal seam (two-phase, forces start() == canceled):
//   Phase 1: the driver reaches barrier_wait (test_driver_barrier_reached).
//   Phase 2: the start owner reaches the commit checkpoint
//            (test_start_owner_at_commit_checkpoint), immediately before
//            checking stop_requested_. The start owner then parks on a
//            test-only release flag.
//   The test calls request_stop() (non-blocking: sets stop_requested_ = true
//   and returns), then releases the start owner. The start owner observes
//   stop_requested_ and takes the abort path, returning canceled. This
//   deterministically proves the startup-abort close path: stop wins the
//   race, start() returns canceled, backend destroyed exactly once.
//
// Why request_stop() (not shutdown()) for the injection: shutdown() blocks
// until close_state_ == Closed, which cannot happen until the start owner
// proceeds past the commit checkpoint. Using the blocking shutdown() here
// would deadlock the test (shutdown() waits for start owner; start owner
// waits for test release). request_stop() is non-blocking and merely sets
// stop_requested_, which is exactly the signal the start owner checks at the
// abort branch.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(c1_concurrent_stop_starting_one_owner) {
    auto pr = build_probe_runtime();
    auto& rt = *pr.rt;

    // Enable the commit checkpoint pause. This is OFF by default so that tests
    // that do not need the pause (e.g. the suspend/resume identity test) are
    // not blocked. The start owner will park at the commit checkpoint until
    // test_release_start_owner_at_commit_checkpoint() is called.
    rt.test_set_pause_at_commit_checkpoint(true);

    // Run start() in its own thread. It will park at the commit checkpoint
    // (after the driver barrier wait, before checking stop_requested_).
    std::thread start_thread([&rt] {
        auto sr = rt.start();
        // Deterministic: stop wins the race, start() MUST return canceled.
        SLUICE_CHECK(!sr.has_value());
        SLUICE_CHECK(sr.error().code == IoError::Code::canceled);
    });

    // Phase 1: wait for the driver barrier signal, proving the Runtime is in
    // Starting with the driver parked at barrier_wait.
    auto barrier_future = rt.test_driver_barrier_reached();
    barrier_future.wait();

    // Phase 2: wait for the start owner to reach the commit checkpoint
    // (immediately before checking stop_requested_). It is now parked on the
    // test-only release flag.
    auto commit_future = rt.test_start_owner_at_commit_checkpoint();
    commit_future.wait();

    // request_stop() from Starting: non-blocking, sets stop_requested_ = true.
    // The start owner is the close owner; request_stop() returns immediately.
    rt.request_stop();

    // Release the start owner: it observes stop_requested_ and takes the
    // abort path (returns canceled).
    rt.test_release_start_owner_at_commit_checkpoint();

    start_thread.join();

    // Exactly one close owner destroyed the backend.
    SLUICE_CHECK(pr.probe->destructor_count.load() == 1);

    // shutdown() is idempotent: the start owner already closed. It observes
    // close_state_ == Closed and returns success without double-destroying.
    auto shr = rt.shutdown();
    SLUICE_CHECK(shr.has_value());
    SLUICE_CHECK(pr.probe->destructor_count.load() == 1);

    pr.rt.reset();
    SLUICE_CHECK(pr.probe->destructor_count.load() == 1);
}

SLUICE_MAIN()
