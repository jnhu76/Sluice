// Tests for sluice::async::Future (sluice-CORE-028, T2).
//
// Single-task awaitable derived from Zig std.Io Future (Io.zig:1176-1206).
// Each case asserts ONE Future semantic, TDD-vertical: RED -> GREEN -> next.
#include "harness.hpp"

#include <sluice/async/cancel.hpp>
#include <sluice/async/future.hpp>
#include <sluice/async/wait_policy.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// ---- Slice 1 (tracer): inline producer -> await returns the result --------
// The simplest path: complete_with on the same thread, then await returns.
// Proves the value channel works end-to-end.
SLUICE_TEST_CASE(future_inline_complete_then_await_returns_result) {
    Future<int> f;
    SLUICE_CHECK(!f.ready());
    f.complete_with(Result<int>{42});
    SLUICE_CHECK(f.ready());
    auto r = f.await();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 42);
}

// ---- Slice 2: thread-driven producer — await blocks until complete --------
// The producer runs on a worker thread; await() on the consumer blocks until
// the worker publishes. Mirrors Zig Future where await(io) drives the backend
// (here: blocks the calling thread, the Threaded-equivalent shape). Exactly-
// once: the worker publishes once; await returns once ready.
SLUICE_TEST_CASE(future_await_blocks_until_worker_completes) {
    Future<long> f;
    std::thread worker([&f] {
        // Simulate work; then publish.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        f.complete_with(Result<long>{12345L});
    });
    auto r = f.await();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 12345L);
    worker.join();
}

// ---- Slice 3: idempotent await/cancel -------------------------------------
// Once the result is materialized, both await() and cancel() return the cached
// result without re-blocking or re-driving (Zig Io.zig:1190, 1198: idempotent).
SLUICE_TEST_CASE(future_await_and_cancel_are_idempotent) {
    Future<int> f;
    f.complete_with(Result<int>{7});
    SLUICE_CHECK(f.ready());

    auto r1 = f.await();
    auto r2 = f.await();   // idempotent — returns cached, no re-block
    auto r3 = f.cancel();  // idempotent — cancel of an already-ready future
    SLUICE_CHECK(r1.value() == 7);
    SLUICE_CHECK(r2.value() == 7);
    SLUICE_CHECK(r3.value() == 7);
}

// ---- Slice 4: cooperative cancel honored by the producer -----------------
// The producer observes the Future's cancel token at its cancel points; when
// cancel() is requested, the producer sees it and publishes IoError::canceled.
// This is the cooperative contract: cancel is best-effort (the producer MAY
// ignore it and publish the real result) but HONORED here (Zig Cancelable +
// the ADR §7 X3 "best-effort and asynchronous" semantics).
//
// Deterministic by construction: the producer waits on the token (polling with
// a short sleep) BEFORE doing its real work, so cancel() always lands while the
// producer is still in its pre-work cancel point. No timing race.
SLUICE_TEST_CASE(future_cancel_honored_by_cooperative_producer) {
    Future<int> f;
    CancelToken& tok = f.cancel_token();
    std::thread worker([&f, &tok] {
        // Pre-work cancel point: wait until the consumer requests cancel OR a
        // long timeout (so the test fails closed rather than hanging).
        for (int i = 0; i < 1000 && !tok.is_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (tok.is_requested()) {
            f.complete_with(sluice::make_unexpected<int>(
                IoError{IoError::Code::canceled}));
            return;
        }
        f.complete_with(Result<int>{999});  // not canceled in time (test bug)
    });
    // Give the worker a moment to reach its pre-work cancel point, then cancel.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto r = f.cancel();
    worker.join();
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::canceled);
}

// ---- Slice 5 (E0A): await delegates to the injected WaitPolicy seam -------
// The E0 ADR §3 boundary: Future must NOT embed the physical wait mechanism.
// This slice proves the seam — a custom WaitPolicy records that it was invoked,
// and still satisfies the wait (using the Threaded mechanism internally) so the
// result is returned correctly. When E5 lands, an Evented policy replaces the
// mechanism with a fiber yield; this test's shape is the contract proof.
namespace {
class RecordingWaitPolicy : public sluice::async::WaitPolicy {
public:
    void wait_until_ready(const std::atomic<bool>& ready,
                          std::mutex& mtx,
                          std::condition_variable& cv) override {
        ++invocations;
        // Use the Threaded mechanism (the contract is "wait until ready"); the
        // point of this test is the SEAM, not a different mechanism.
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [&] { return ready.load(std::memory_order::acquire); });
    }
    std::atomic<int> invocations{0};
};
}  // namespace

SLUICE_TEST_CASE(future_await_delegates_to_wait_policy_seam) {
    RecordingWaitPolicy policy;
    Future<int> f{policy};  // inject the custom policy
    SLUICE_CHECK(policy.invocations.load() == 0);

    // await on an un-ready Future: the policy MUST be invoked.
    std::thread worker([&f] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        f.complete_with(Result<int>{42});
    });
    auto r = f.await();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 42);
    SLUICE_CHECK(policy.invocations.load() == 1);  // seam was used, not bypassed
    worker.join();

    // Second await: ready already; the policy must NOT be re-invoked (the
    // fast-path skips the wait when ready, preserving idempotency).
    auto r2 = f.await();
    SLUICE_CHECK(r2.value() == 42);
    SLUICE_CHECK(policy.invocations.load() == 1);
}

SLUICE_MAIN()
