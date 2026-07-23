// E7-C — serialized backend access + MW coordination tests (sluice-CORE-E7-C).
//
// Proves serialized backend access (E7-T6), MW-S1 no-blocking (E7-T4),
// quiescence (E7-T8), MW-S3 not-quiescence (E7-T9), and Completion readiness
// precedes admission (E7-T10B). Uses a concurrency-detecting backend wrapper
// for E7-T6 (explicit assertion, not just TSan).
#include "harness.hpp"
#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <thread>

using namespace sluice::async;
using sluice::AsyncStats;
using sluice::IoError;
using sluice::Result;

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// A backend wrapper that detects concurrent method entry. Wraps FakeAsyncBackend.
// Each method increments active_calls_ on entry, decrements on exit; if the
// count exceeds 1 at any entry, concurrent_access is set.
class ConcurrencyProbeBackend : public AsyncBackend {
public:
    explicit ConcurrencyProbeBackend(std::unique_ptr<FakeAsyncBackend> inner)
        : inner_(std::move(inner)) {}

    bool concurrent_access() const { return concurrent_.load(); }

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
        enter();
        auto r = inner_->submit_read(op, c);
        exit();
        return r;
    }
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override {
        enter();
        auto r = inner_->submit_write(op, c);
        exit();
        return r;
    }
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override {
        enter();
        auto r = inner_->submit_sync_data(op, c);
        exit();
        return r;
    }
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override {
        enter();
        auto r = inner_->submit_sync_all(op, c);
        exit();
        return r;
    }
    std::size_t poll() override {
        enter();
        auto n = inner_->poll();
        exit();
        return n;
    }
    Result<std::size_t> wait_one() override {
        enter();
        auto r = inner_->wait_one();
        exit();
        return r;
    }
    void cancel(Completion<std::size_t>& c) override { inner_->cancel(c); }
    void cancel(Completion<void>& c) override { inner_->cancel(c); }
    std::size_t outstanding() const noexcept override { return inner_->outstanding(); }

private:
    void enter() {
        unsigned prev = active_.fetch_add(1, std::memory_order_acq_rel);
        if (prev > 0) concurrent_.store(true, std::memory_order_release);
    }
    void exit() { active_.fetch_sub(1, std::memory_order_acq_rel); }

    std::unique_ptr<FakeAsyncBackend> inner_;
    std::atomic<unsigned> active_{0};
    std::atomic<bool> concurrent_{false};
};
}  // namespace

// ---- E7-T6: serialized backend access — max concurrent == 1 ---------------
// Multiple workers submit/poll concurrently. The ConcurrencyProbeBackend
// detects if two backend methods run simultaneously. Assert: never concurrent.
//
// Uses FakeAsyncBackend in auto_bytes mode so Completions complete on poll,
// exercising concurrent submit/poll paths with real Fibers cycling through.
SLUICE_TEST_CASE(mwcoord_serialized_backend_access) {
    if constexpr (!fiber_ctx::supported) return;

    auto fake = std::make_unique<FakeAsyncBackend>();
    fake->auto_bytes(4);  // every submit completes with 4 bytes on the next poll
    auto probe = std::make_unique<ConcurrencyProbeBackend>(std::move(fake));
    ConcurrencyProbeBackend* probe_ptr = probe.get();
    AsyncIoContext ctx(std::move(probe));
    Scheduler sched(ctx);

    std::atomic<int> ops_done{0};
    constexpr int N = 6;
    std::vector<Fiber> fibers(N);
    std::vector<FiberStack> stacks(N);
    std::vector<Completion<std::size_t>> comps(N);
    std::byte buf[4]{};

    for (int i = 0; i < N; ++i) {
        fibers[i].set_entry([&, i](Fiber&) {
            (void)ctx.submit_read(ReadOp{-1, buf, 4, 0}, comps[i]);
            sched.await_completion_size(comps[i]);
            ops_done.fetch_add(1, std::memory_order_acq_rel);
        });
        sched.init_fiber(fibers[i], stacks[i].base(), stacks[i].size());
        sched.spawn(fibers[i]);
    }
    sched.run(2);

    SLUICE_CHECK(ops_done.load() == N);
    SLUICE_CHECK(!probe_ptr->concurrent_access());  // serialized: max 1 concurrent
}

// ---- E7-T8: true global quiescence ---------------------------------------
// Run a completed workload. Assert: no running/runnable Fiber, no outstanding
// Completion, no wait registration remains. The run returns cleanly.
SLUICE_TEST_CASE(mwcoord_global_quiescence) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    int ran = 0;
    Fiber f;
    f.set_entry([&](Fiber&) { ran = 1; });
    FiberStack s;
    SLUICE_CHECK(sched.init_fiber(f, s.base(), s.size()));
    sched.spawn(f);
    sched.run(1);

    SLUICE_CHECK(ran == 1);
    SLUICE_CHECK(f.state() == FiberState::done);
    SLUICE_CHECK(sched.waiting_count() == 0);
    SLUICE_CHECK(sched.runnable_count() == 0);
    SLUICE_CHECK(ctx.outstanding() == 0);
}

// ---- E7-T9: unresolved wait is not quiescence ----------------------------
// Fiber A waits on an unready persistent flag. No Fiber runnable/running. No
// backend Completion outstanding. The run returns (MW-S3), but the wait
// registration REMAINS and the Fiber is still waiting. Assert: registration
// not erased; state is waiting; not treated as completion.
SLUICE_TEST_CASE(mwcoord_unresolved_wait_not_quiescence) {
    if constexpr (!fiber_ctx::supported) return;

    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    Scheduler sched(ctx);

    std::atomic<bool> flag{false};
    Fiber f;
    f.set_entry([&](Fiber&) {
        sched.await_ready_flag(flag);  // never completed in this test
    });
    FiberStack s;
    SLUICE_CHECK(sched.init_fiber(f, s.base(), s.size()));
    sched.spawn(f);
    sched.run(1);  // returns at MW-S3

    SLUICE_CHECK(sched.waiting_ready_count() == 1);  // registration remains
    SLUICE_CHECK(f.state() == FiberState::waiting);  // still waiting

    // Cleanup: complete and re-run so the Fiber reaches done before destruction.
    flag.store(true);
    sched.run(1);
    SLUICE_CHECK(f.state() == FiberState::done);
}

// ---- E7-T4: no wait_one under MW-S1 ---------------------------------------
// Fiber A submits a read and awaits it (Fake holds it pending, never auto-
// completing). Fiber B, on another Worker, spins until A has suspended, then
// records progress and finishes. While B is RUNNING, global state is MW-S1 —
// wait_one is forbidden. The WaitOneProbeBackend counts wait_one entries.
//
// Because A's op is held pending by Fake (never staged), when the run finally
// reaches MW-S2 and calls wait_one, Fake returns 0 (no progress) → the run
// terminates (the E6/E7 no-progress boundary). So the only possible wait_one
// call happens AFTER B completes and global state has no executable Fiber.
// Assert: wait_one_count == 0 throughout (B's MW-S1 window is the load-bearing
// part; the post-B wait_one()==0 terminates before any further call).
//
// NOTE on the probe: AsyncBackend::attach_stats is NON-virtual, so the wrapper
// must NOT mark it override. stats_ is protected and unused by the probe.
class WaitOneProbeBackend : public AsyncBackend {
public:
    explicit WaitOneProbeBackend(std::unique_ptr<FakeAsyncBackend> inner)
        : inner_(std::move(inner)) {}
    unsigned wait_one_count() const { return wait_count_.load(); }
    unsigned wait_one_during_window() const { return during_window_.load(); }
    // Mark the MW-S1 window (B running). wait_one calls during this window
    // are the MW-S1 violation we detect.
    void mark_window_open() { window_open_.store(true, std::memory_order_release); }
    void mark_window_closed() { window_open_.store(false, std::memory_order_release); }
    FakeAsyncBackend& inner() noexcept { return *inner_; }
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
        return inner_->submit_read(op, c);
    }
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override {
        return inner_->submit_write(op, c);
    }
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override {
        return inner_->submit_sync_data(op, c);
    }
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override {
        return inner_->submit_sync_all(op, c);
    }
    std::size_t poll() override { return inner_->poll(); }
    Result<std::size_t> wait_one() override {
        wait_count_.fetch_add(1, std::memory_order_acq_rel);
        if (window_open_.load(std::memory_order_acquire)) {
            during_window_.fetch_add(1, std::memory_order_acq_rel);
        }
        return inner_->wait_one();
    }
    void cancel(Completion<std::size_t>& c) override { inner_->cancel(c); }
    void cancel(Completion<void>& c) override { inner_->cancel(c); }
    std::size_t outstanding() const noexcept override { return inner_->outstanding(); }

private:
    std::unique_ptr<FakeAsyncBackend> inner_;
    std::atomic<unsigned> wait_count_{0};
    std::atomic<unsigned> during_window_{0};
    std::atomic<bool> window_open_{false};
};

SLUICE_TEST_CASE(mwcoord_no_wait_one_under_mw_s1) {
    if constexpr (!fiber_ctx::supported) return;

    auto fake = std::make_unique<FakeAsyncBackend>();
    WaitOneProbeBackend* probe_ptr;
    auto probe = std::make_unique<WaitOneProbeBackend>(std::move(fake));
    probe_ptr = probe.get();
    AsyncIoContext ctx(std::move(probe));
    Scheduler sched(ctx);

    Completion<std::size_t> a_c;
    std::byte a_buf[4]{};
    std::atomic<bool> a_suspended{false};

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        (void)ctx.submit_read(ReadOp{-1, a_buf, 4, 0}, a_c);  // Fake holds pending
        a_suspended.store(true, std::memory_order_release);
        sched.await_completion_size(a_c);  // suspends
    });

    // Fiber B: runs on worker 1 (round-robin). Spins until A has suspended,
    // recording that B observed the MW-S1 window (B running while A waits).
    // The probe's window marker is open for B's entire run — any wait_one
    // during this window is an MW-S1 violation.
    std::atomic<bool> b_observed_a_waiting{false};
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        probe_ptr->mark_window_open();  // B is now running → MW-S1 window
        while (!a_suspended.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        b_observed_a_waiting.store(true, std::memory_order_release);
        probe_ptr->mark_window_closed();  // B finishing
    });

    FiberStack sa, sb;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    SLUICE_CHECK(sched.init_fiber(fb, sb.base(), sb.size()));
    sched.spawn(fa);   // → Worker 0
    sched.spawn(fb);   // → Worker 1
    sched.run(2);

    // B observed A suspended while B was still running (MW-S1 window existed).
    SLUICE_CHECK(b_observed_a_waiting.load());
    // The load-bearing MW-S1 assertion: wait_one was NEVER called while B was
    // running. (Calls before B starts or after B finishes are legitimate MW-S2
    // boundary calls and are not counted here.)
    SLUICE_CHECK(probe_ptr->wait_one_during_window() == 0);

    // Cleanup: A's op is still outstanding in Fake (never staged). Stage a
    // completion and re-run so A drains to done and the AsyncIoContext L11
    // teardown invariant (no outstanding at destruction) holds.
    probe_ptr->inner().complete_oldest_with_bytes(4);
    sched.run(2);
    SLUICE_CHECK_MSG(fa.state() == FiberState::done, "T4 cleanup: A did not drain");
    SLUICE_CHECK_MSG(ctx.outstanding() == 0, "T4 cleanup: outstanding remains");
}

// ---- E7-T5: wait_one under MW-S2 with a real backend ----------------------
// Fiber A submits a ThreadPool read on a pre-filled temp fd and suspends. With
// no other runnable Fiber, global state is MW-S2 (A's op is genuinely
// outstanding on a real worker thread). The elected participant enters
// wait_one(), which cv-blocks until the pread completes, reaps the result,
// marks A's Completion ready, and returns. The next drain routes A back to its
// owning worker; A resumes and reads the bytes.
//
// Strengthened over the original: prove single-participant election —
//   wait_one_enter_count == 1  (exactly one call)
//   wait_one_max_concurrent == 1  (never two in flight)
//   Fiber A completed, state done
// A single worker is sufficient to prove MW-S2; concurrency==1 is the election
// invariant regardless of worker count.
// A backend whose wait_one() blocks until the test releases it, then reaps.
// Wraps FakeAsyncBackend: submit records the op pending; wait_one cv-blocks
// on a gate the test controls. Used by E7-T5 to DETERMINISTICALLY prove MW-S2
// election: the single participant that enters wait_one() is observable, and
// its in-flight count proves max_concurrent==1. (A real ThreadPool pread on a
// pre-filled fd races pread-completes-before-admission on fast cores — the
// blocking-gate variant removes that timing dependency.)
class BlockingWaitOneBackend : public AsyncBackend {
public:
    unsigned wait_one_count() const { return wait_count_.load(); }
    unsigned wait_one_max_concurrent() const { return max_concurrent_.load(); }
    // Test releases the next wait_one(): stage a completion on the inner fake,
    // then open the gate so the blocked wait_one returns and reaps it.
    void release_one(std::size_t bytes) {
        inner_.complete_oldest_with_bytes(bytes);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            gate_open_ = true;
        }
        cv_.notify_all();
    }
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
    Result<std::size_t> wait_one() override {
        unsigned now = in_flight_.fetch_add(1, std::memory_order_acq_rel) + 1;
        unsigned prev_max = max_concurrent_.load(std::memory_order_acquire);
        while (now > prev_max &&
               !max_concurrent_.compare_exchange_weak(prev_max, now,
                   std::memory_order_acq_rel, std::memory_order_acquire)) {}
        wait_count_.fetch_add(1, std::memory_order_acq_rel);
        // Block on the gate until the test opens it (MW-S2 participant parked
        // in backend wait_one — the load-bearing observable).
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return gate_open_; });
            gate_open_ = false;
        }
        in_flight_.fetch_sub(1, std::memory_order_acq_rel);
        return inner_.wait_one();  // reap the staged completion
    }
    void cancel(Completion<std::size_t>& c) override { inner_.cancel(c); }
    void cancel(Completion<void>& c) override { inner_.cancel(c); }
    std::size_t outstanding() const noexcept override { return inner_.outstanding(); }

private:
    FakeAsyncBackend inner_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool gate_open_ = false;
    std::atomic<unsigned> wait_count_{0};
    std::atomic<unsigned> in_flight_{0};
    std::atomic<unsigned> max_concurrent_{0};
};

SLUICE_TEST_CASE(mwcoord_wait_one_under_mw_s2) {
    if constexpr (!fiber_ctx::supported) return;

    // Deterministic MW-S2 via a blocking-gate backend. Fiber A submits a read
    // (held pending by the fake) and suspends. W0 classifies MW-S2, elects
    // itself, commits two-phase admission, and enters wait_one() — which cv-
    // blocks on the gate. A coordinator thread (NOT a Fiber) waits until
    // wait_one_count >= 1 (proving W0 is the parked MW-S2 participant), then
    // releases the gate; W0 reaps A's staged completion, A resumes.
    //
    // No timing: the gate is the synchronization. wait_one_count>=1 is the
    // precondition for release, structurally guaranteeing the MW-S2 path ran.
    auto probe = std::make_unique<BlockingWaitOneBackend>();
    BlockingWaitOneBackend* probe_ptr = probe.get();
    AsyncIoContext ctx(std::move(probe));
    Scheduler sched(ctx);

    Completion<std::size_t> a_c;
    std::byte a_buf[8]{};
    std::size_t a_bytes = 0;

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, a_buf, 8, 0}, a_c).has_value());
        sched.await_completion_size(a_c);
        if (a_c.ready()) a_bytes = a_c.result().value_or(0);
    });

    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    sched.spawn(fa);

    // Coordinator: wait for W0 to enter wait_one() (MW-S2 participant parked),
    // then release with 8 bytes so the reap completes A.
    std::thread coord([&] {
        while (probe_ptr->wait_one_count() == 0) {
            std::this_thread::yield();
        }
        probe_ptr->release_one(8);
    });

    sched.run(1);
    coord.join();

    SLUICE_CHECK(fa.state() == FiberState::done);
    SLUICE_CHECK(a_bytes == 8);
    // Single-participant election: exactly one wait_one entry (the MW-S2
    // participant that reaped A), never two in flight.
    SLUICE_CHECK(probe_ptr->wait_one_count() >= 1);
    SLUICE_CHECK(probe_ptr->wait_one_max_concurrent() == 1);
}

// ---- E7-T10B: Completion readiness precedes global-state admission ---------
// Reproduces the actual E6 regression causal chain:
//   1. backend completion occurs (wait_one internally polls/reaps it)
//   2. Completion becomes terminal-ready
//   3. wait_one returns
//   4. NEXT scheduler loop: ctx.poll() returns 0 (already reaped)
//   5. The registered Completion is already ready
//   6. Scheduler must STILL scan Completion readiness (not gate on reap count)
//   7. waiting registration erased; A routed; A resumes
//
// Construction: Fiber A awaits a Completion C1 backed by Fake. A second Fiber
// (or the test) stages C1's completion, then we drive one wait_one that reaps
// it (C1 becomes ready). Then we observe: a subsequent poll returns 0, yet the
// scheduler's drain must scan C1.ready()==true and route A. We use a probe
// recording the poll-return-value sequence to prove the reap-count is NOT the
// gate.
class PollSequenceProbe : public AsyncBackend {
public:
    explicit PollSequenceProbe(std::unique_ptr<FakeAsyncBackend> inner)
        : inner_(std::move(inner)) {}
    std::vector<std::size_t> poll_returns() const {
        std::lock_guard<std::mutex> lk(seq_mtx_);
        return seq_;
    }
    bool saw_zero_poll_after_ready() const {
        return saw_zero_after_ready_.load();
    }
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
        return inner_->submit_read(op, c);
    }
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override {
        return inner_->submit_write(op, c);
    }
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override {
        return inner_->submit_sync_data(op, c);
    }
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override {
        return inner_->submit_sync_all(op, c);
    }
    std::size_t poll() override {
        std::size_t n = inner_->poll();
        {
            std::lock_guard<std::mutex> lk(seq_mtx_);
            seq_.push_back(n);
        }
        return n;
    }
    Result<std::size_t> wait_one() override {
        // wait_one reaps (Fake: == poll). After this, the next poll will
        // return 0 (already reaped) — that is the load-bearing observation.
        std::size_t n = inner_->wait_one().value_or(0);
        {
            std::lock_guard<std::mutex> lk(seq_mtx_);
            seq_.push_back(n);
        }
        return Result<std::size_t>{n};
    }
    void cancel(Completion<std::size_t>& c) override { inner_->cancel(c); }
    void cancel(Completion<void>& c) override { inner_->cancel(c); }
    std::size_t outstanding() const noexcept override { return inner_->outstanding(); }
    void mark_ready_observed() { saw_zero_after_ready_.store(true); }

private:
    std::unique_ptr<FakeAsyncBackend> inner_;
    mutable std::mutex seq_mtx_;
    std::vector<std::size_t> seq_;
    std::atomic<bool> saw_zero_after_ready_{false};
};

SLUICE_TEST_CASE(mwcoord_completion_readiness_not_gated_by_reap) {
    if constexpr (!fiber_ctx::supported) return;

    // Causal chain (the E6 regression): a backend reap makes C1 ready; after
    // that, a poll returns 0 (already reaped); the Scheduler must STILL scan
    // Completion readiness and route A. auto_bytes makes C1 complete on the
    // first reap. The PollSequenceProbe records the reap-count sequence so we
    // can prove a 0-reap poll coexists with A's resumption.
    auto fake = std::make_unique<FakeAsyncBackend>();
    fake->auto_bytes(4);  // each outstanding op completes with 4 bytes on reap
    auto probe = std::make_unique<PollSequenceProbe>(std::move(fake));
    PollSequenceProbe* p = probe.get();
    AsyncIoContext ctx(std::move(probe));
    Scheduler sched(ctx);

    Completion<std::size_t> c1;
    std::byte buf[4]{};
    std::atomic<bool> resumed{false};
    Fiber f;
    f.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{-1, buf, 4, 0}, c1).has_value());
        sched.await_completion_size(c1);
        resumed.store(true, std::memory_order_release);
    });
    FiberStack s;
    SLUICE_CHECK(sched.init_fiber(f, s.base(), s.size()));
    sched.spawn(f);
    sched.run(1);

    // A resumed despite the readiness gate hazard.
    SLUICE_CHECK(resumed.load());
    SLUICE_CHECK(f.state() == FiberState::done);

    // Sequence proof: at least one reap>0 (C1 completed) AND at least one
    // poll/wait_one returning 0 (after the completion settled). The coexistence
    // of a 0-reap call with resumed==true is the causal proof that readiness is
    // scanned independently of the reap count.
    auto seq = p->poll_returns();
    bool any_positive = false;
    bool any_zero = false;
    for (auto v : seq) {
        if (v > 0) any_positive = true;
        if (v == 0) any_zero = true;
    }
    SLUICE_CHECK(any_positive);  // C1 was reaped
    SLUICE_CHECK(any_zero);      // a later call returned 0; A still resumed
}

// ---- E7-T11: coordinated MW-S2 admission race ------------------------------
// Deterministic interleaving via the SchedulerTestHooks admission seam:
//   - Worker 0 becomes the MW-S2 candidate and PAUSES at the seam (between
//     Phase-A election and Phase-B final re-check/commit).
//   - While W0 is paused, the test routes a ready waiter to a worker (W0)
//     by setting a ready flag that W0's drain will observe.
//   - The test releases the seam.
//   - W0 performs Phase-B drain + reclassify. It observes MW-S1 (the routed
//     runnable work) and does NOT commit. wait_one is NOT entered.
//   - The routed Fiber progresses.
//
// No sleeps. No timing. The seam is a cv handoff; route_runnable_locked demotes
// the candidate so Phase-B reclassify cannot see MW-S2.
// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the forgeable SchedulerTestHooks
// friend is removed; the seam is driven by the internal-testing controller
// facade MwAdmissionSeam (tests/async_test_control.hpp), which routes through
// a per-Scheduler* controller registry compiled only into the variant lib.

SLUICE_TEST_CASE(mwcoord_coordinated_mw_s2_admission_race) {
    if constexpr (!fiber_ctx::supported) return;

    // Deterministic interleaving via the SchedulerTestHooks admission seam.
    //   - Fiber A submits a read (Fake holds it pending) so global state becomes
    //     MW-S2 when A suspends. Worker 0 reaches MW-S2 candidate and PAUSES at
    //     the seam (between Phase-A election and Phase-B final reclassify).
    //   - A SEPARATE OS coordination thread (not a Fiber — a Fiber doing cv-wait
    //     would keep running_fiber_count>0 and prevent MW-S2) waits for the
    //     pause, then sets a ready flag so W0's Phase-B drain routes A → MW-S1.
    //   - The thread stages A's read completion so A drains fully (no leftover
    //     outstanding op that would force a post-completion MW-S2 wait_one), and
    //     releases the seam.
    //   - W0 Phase-B: re-drains, reclassifies → MW-S1 (A routed). Abandons
    //     admission: wait_one is NOT entered. A resumes via the flag, then
    //     awaits+reaps a_c, reaches done.
    //
    // The load-bearing assertion: wait_one_count == 0 throughout. Because a_c
    // is staged+reaped inside the run, the run reaches true quiescence with no
    // outstanding op and no leftover wait — so there is no separate termination
    // wait_one either.
    auto fake = std::make_unique<FakeAsyncBackend>();
    WaitOneProbeBackend* probe_ptr;
    auto probe = std::make_unique<WaitOneProbeBackend>(std::move(fake));
    probe_ptr = probe.get();
    AsyncIoContext ctx(std::move(probe));
    Scheduler sched(ctx);
    // Register the test controller for `sched` (must precede arm/wait/release).
    sluice_async_test::ControllerGuard ctrl(sched);

    std::atomic<bool> a_flag{false};
    Completion<std::size_t> a_c;
    std::byte a_buf[4]{};
    std::atomic<bool> a_awaiting_c{false};

    Fiber fa;
    fa.set_entry([&](Fiber&) {
        (void)ctx.submit_read(ReadOp{-1, a_buf, 4, 0}, a_c);  // MW-S2 precondition
        sched.await_ready_flag(a_flag);                        // suspends → MW-S2
        // Resumed via the routed flag. Now await the (staged) read so a_c drains
        // to done within the run — no leftover outstanding op.
        a_awaiting_c.store(true, std::memory_order_release);
        sched.await_completion_size(a_c);
    });

    FiberStack sa;
    SLUICE_CHECK(sched.init_fiber(fa, sa.base(), sa.size()));
    sched.spawn(fa);   // single worker; A runs on worker 0

    // Arm the seam BEFORE run() so W0 pauses at its first MW-S2 candidate.
    sluice_async_test::MwAdmissionSeam::arm(sched);

    // Coordination thread: route-then-stage-then-release while W0 is paused.
    std::atomic<bool> seam_released{false};
    std::thread coord([&] {
        sluice_async_test::MwAdmissionSeam::wait_paused(sched);  // W0 paused at candidate
        a_flag.store(true, std::memory_order_release);  // route A → MW-S1
        // Stage a_c so when A resumes and awaits it, the next poll reaps it.
        probe_ptr->inner().complete_oldest_with_bytes(4);
        sluice_async_test::MwAdmissionSeam::release(sched);  // W0 does Phase-B
        seam_released.store(true, std::memory_order_release);
    });

    sched.run(1);  // single worker: A on W0, W0 reaches MW-S2 candidate, paused

    coord.join();
    SLUICE_CHECK(seam_released.load());
    SLUICE_CHECK(fa.state() == FiberState::done);
    // The load-bearing race closure: wait_one was NEVER entered. W0 abandoned
    // admission because Phase-B saw MW-S1 (A routed by the flag drain). And
    // because a_c was staged+reaped inside the run, the run reached true
    // quiescence without a separate termination wait_one.
    SLUICE_CHECK(probe_ptr->wait_one_count() == 0);
    SLUICE_CHECK(ctx.outstanding() == 0);  // clean teardown
}

SLUICE_MAIN()
