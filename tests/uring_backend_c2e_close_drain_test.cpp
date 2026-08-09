// Phase D4 — Uring C2e close / drain / destruction evidence.
//
// Real mode exercises the authoritative production uring_backend.cpp (plus
// async_io_context.cpp for the split-phase wait) with guarded
// SLUICE_ASYNC_INTERNAL_TESTING observations and deterministic pause gates.
// Covers: close while pending/enqueued/running/backend_ready, the submit-vs-
// close acceptance LP in both orderings, post-close precedence, close-then-
// cancel, parked-waiter wake (one-shot, multi-waiter, no busy-spin), the
// interrupt-vs-final-ready race, drained != releasable, and poison + close.
//
// Stub mode proves build and API honesty only; the manifest requires evidence
// mode=real (G2: the pinned case set must run exactly once each).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/error.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

#if defined(SLUICE_HAS_LIBURING)
#include <liburing.h>

#include <cerrno>
#include <cstring>
#include <vector>
#endif

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

#if defined(SLUICE_HAS_LIBURING)

namespace {

template <class Fn> bool wait_until(Fn&& fn, int timeout_ms = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!fn()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

// Bounded drain: poll() flushes transport + reaps until no accepted work
// remains (the ONLY way a real Uring op reaches its terminal). Deadline is a
// hang watchdog only; ordering claims come from the deterministic windows.
void drain_bounded(UringAsyncBackend& backend, int timeout_ms = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (backend.outstanding() != 0) {
        (void)backend.poll();
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "uring_c2e: drain deadline exceeded "
                                 "(outstanding=%zu)\n", backend.outstanding());
            std::fflush(stderr);
            std::terminate();
        }
        std::this_thread::yield();
    }
}

struct ScopedGateResume {
    std::atomic<bool>& resume;
    std::atomic<bool>& exited;
    bool done = false;
    explicit ScopedGateResume(std::atomic<bool>& r, std::atomic<bool>& e)
        : resume(r), exited(e) {}
    ~ScopedGateResume() {
        if (!done) {
            resume.store(true, std::memory_order_release);
            (void)wait_until([&] { return exited.load(std::memory_order_acquire); });
            done = true;
        }
    }
    void release() {
        resume.store(true, std::memory_order_release);
        if (!wait_until([&] { return exited.load(std::memory_order_acquire); })) {
            std::fprintf(stderr, "uring_c2e: pause gate exit timeout\n");
            std::fflush(stderr);
            std::terminate();
        }
        done = true;
    }
};

class TempFile {
  public:
    TempFile() {
        char path[] = "/tmp/sluice_uring_d4_c2e_XXXXXX";
        fd_ = ::mkstemp(path);
        if (fd_ >= 0)
            (void)::unlink(path);
    }
    ~TempFile() {
        if (fd_ >= 0)
            (void)::close(fd_);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    int fd() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

  private:
    int fd_ = -1;
};

class PipePair {
  public:
    PipePair() { valid_ = ::pipe(fds_) == 0; }
    ~PipePair() {
        if (fds_[0] >= 0)
            (void)::close(fds_[0]);
        if (fds_[1] >= 0)
            (void)::close(fds_[1]);
    }
    PipePair(const PipePair&) = delete;
    PipePair& operator=(const PipePair&) = delete;
    bool valid() const noexcept { return valid_; }
    int read_fd() const noexcept { return fds_[0]; }
    int write_fd() const noexcept { return fds_[1]; }
    void close_write() noexcept {
        if (fds_[1] >= 0) {
            (void)::close(fds_[1]);
            fds_[1] = -1;
        }
    }

  private:
    int fds_[2] = {-1, -1};
    bool valid_ = false;
};

} // namespace

// ---------------------------------------------------------------------------
// Evidence-meta (G2): exactly one [evidence-meta] line per gate-driven run.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_d4_c2e_evidence_mode) {
#if defined(SLUICE_HAS_LIBURING)
    UringAsyncBackend backend{UringConfig{4, 4}};
    std::printf("[evidence-meta] evidence=uring_c2e_close_drain mode=real\n");
    SLUICE_CHECK(backend.available());
#else
    std::printf("[evidence-meta] evidence=uring_c2e_close_drain mode=stub\n");
#endif
}

// ---------------------------------------------------------------------------
// C2e row 15 — close while the accepted request is still `pending` (after the
// accept LP, before enqueue): close does NOT retroactively reject/cancel/
// discard; the request reaches its REAL terminal; reap works after close.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_close_while_pending_preserves_accepted) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    UringAsyncBackend::AfterCommitBeforeEnqueuePauseGate gate;
    backend.set_after_commit_before_enqueue_pause_gate(&gate);

    std::byte buf[8]{std::byte{0x61}};
    Completion<std::size_t> c;
    std::thread submitter([&] {
        (void)backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c);
    });
    SLUICE_CHECK(wait_until([&] { return gate.paused.load(std::memory_order_acquire); }));
    ScopedGateResume resume(gate.resume, gate.exited);
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());
    auto obs = backend.observe_for_test(*h);
    SLUICE_CHECK(obs.has_value());
    SLUICE_CHECK(obs->state == detail::RequestState::pending);

    // Close while pending: admission barrier only.
    backend.close_admission();
    SLUICE_CHECK(backend.outstanding() == 1); // accepted work untouched

    resume.release();
    submitter.join();

    // The accepted request still reaches its real terminal.
    drain_bounded(backend);
    SLUICE_CHECK(c.ready());
    const auto res = c.result();
    SLUICE_CHECK(res.has_value() && res.value() == 8);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2e row 15 — close while the request is `enqueued` (dispatch ring, no SQE
// yet): the dispatch linkage is preserved; the request still executes and
// reaches its real terminal.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_close_while_enqueued_preserves_dispatch) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    UringAsyncBackend::BeforeDispatchTransferPauseGate gate;
    backend.set_before_dispatch_transfer_pause_gate(&gate);

    std::byte buf[8]{std::byte{0x62}};
    Completion<std::size_t> c;
    std::thread submitter([&] {
        (void)backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c);
    });
    SLUICE_CHECK(wait_until([&] { return gate.paused.load(std::memory_order_acquire); }));
    ScopedGateResume resume(gate.resume, gate.exited);
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());
    auto obs = backend.observe_for_test(*h);
    SLUICE_CHECK(obs.has_value());
    SLUICE_CHECK(obs->state == detail::RequestState::enqueued);
    SLUICE_CHECK(backend.dispatch_size_for_test() == 1);

    // Close while enqueued: the dispatch ring is NOT discarded.
    backend.close_admission();
    SLUICE_CHECK(backend.dispatch_size_for_test() == 1);

    resume.release();
    submitter.join();

    drain_bounded(backend);
    SLUICE_CHECK(c.ready());
    const auto res = c.result();
    SLUICE_CHECK(res.has_value() && res.value() == 8);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2e row 15 — close while the request is `running` (ring-owned, real
// kernel-blocked read): close is not a cancel — the original operation CQE
// decides VERBATIM.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_close_while_running_result_verbatim) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    PipePair pipe;
    SLUICE_CHECK(pipe.valid());
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    std::byte buf[4]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_read(ReadOp{pipe.read_fd(), buf, 4, 0}, c).has_value());
    SLUICE_CHECK(backend.poll() == 0); // kernel blocks the read
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());
    auto obs = backend.observe_for_test(*h);
    SLUICE_CHECK(obs.has_value());
    SLUICE_CHECK(obs->state == detail::RequestState::running);

    backend.close_admission();
    SLUICE_CHECK(backend.outstanding() == 1);

    pipe.close_write();
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    const auto res = c.result();
    SLUICE_CHECK(res.has_value() && res.value() == 0); // EOF verbatim
    SLUICE_CHECK(stats.canceled_ops == 0);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2e row 15 — close while the request is `backend_ready` but not yet reaped:
// reap remains legal after close and publishes exactly once.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_close_while_backend_ready) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    std::byte buf[8]{std::byte{0x63}};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c).has_value());
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());
    backend.inject_cqe_for_test(1, 8);
    auto obs = backend.observe_for_test(*h);
    SLUICE_CHECK(obs.has_value());
    SLUICE_CHECK(obs->state == detail::RequestState::backend_ready);
    SLUICE_CHECK(!c.ready());

    backend.close_admission();
    SLUICE_CHECK(backend.poll() == 1); // reap after close is legal
    SLUICE_CHECK(c.ready());
    const auto res = c.result();
    SLUICE_CHECK(res.has_value() && res.value() == 8);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2e row 15 — void submit after close: invalid_state, Completion idle, zero
// residue.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_void_submit_after_close_rejected) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());
    backend.close_admission();

    Completion<void> c;
    const auto r = backend.submit_sync_data(SyncDataOp{file.fd()}, c);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    SLUICE_CHECK(c.idle());
    SLUICE_CHECK(!c.outstanding() && !c.ready());
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    SLUICE_CHECK(backend.dispatch_size_for_test() == 0);
    SLUICE_CHECK(backend.live_cookies_for_test() == 0);
    SLUICE_CHECK(backend.poll() == 0);
}

// ---------------------------------------------------------------------------
// C2e row 15 — post-close descriptor precedence: a MALFORMED submit after
// close rejects with invalid_state (admission beats descriptor validation),
// Completion idle, zero residue.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_malformed_submit_after_close_rejected) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    backend.close_admission();

    std::byte buf[4]{};
    Completion<std::size_t> c;
    const auto r = backend.submit_read(ReadOp{/*fd=*/-1, buf, 4, 0}, c);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    SLUICE_CHECK(c.idle());
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);

    Completion<void> cs;
    const auto rs = backend.submit_sync_all(SyncAllOp{/*fd=*/-1}, cs);
    SLUICE_CHECK(!rs.has_value());
    SLUICE_CHECK(rs.error().code == IoError::Code::invalid_state);
    SLUICE_CHECK(cs.idle());
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2e §3.1 — submit-vs-close LP: the winning submit retains the admission
// transaction lock through the `binding -> outstanding` release-store.
// close_admission() must BLOCK while the submit is paused inside the
// transaction and only return AFTER the LP completed (submit wins).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_close_waits_for_inflight_acceptance_lp) {
    constexpr int kCloseProbeTimeoutMs = 1500;

    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    UringAsyncBackend::BeforeCommitBindingPauseGate gate;
    backend.set_before_commit_binding_pause_gate(&gate);

    std::byte buf[8]{std::byte{0x64}};
    Completion<std::size_t> c;
    std::thread submitter([&] {
        (void)backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c);
    });
    SLUICE_CHECK(wait_until([&] { return gate.paused.load(std::memory_order_acquire); }));
    SLUICE_CHECK(gate.admission_domain_held.load(std::memory_order_acquire));

    // The closer must block on the in-flight admission transaction.
    std::atomic<bool> close_returned{false};
    std::thread closer([&] {
        backend.close_admission();
        close_returned.store(true, std::memory_order_release);
    });
    // Negative probe: close must NOT return while the submit holds the LP.
    std::this_thread::sleep_for(std::chrono::milliseconds(kCloseProbeTimeoutMs));
    SLUICE_CHECK(!close_returned.load(std::memory_order_acquire));

    // Resume: the submit completes its LP, then close acquires the lock and
    // returns with admission closed.
    gate.resume.store(true, std::memory_order_release);
    submitter.join();
    closer.join();
    SLUICE_CHECK(close_returned.load(std::memory_order_acquire));
    SLUICE_CHECK(c.outstanding()); // submit won the LP

    // Post-close: no new acceptance LP can occur.
    Completion<std::size_t> c2;
    const auto r = backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c2);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);

    // The LP-winning request still completes.
    drain_bounded(backend);
    SLUICE_CHECK(c.ready());
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2e §3.1 — close wins: a submit that has NOT yet taken the admission lock
// when close completes observes admission closed at Stage 0 and rejects
// synchronously with invalid_state (idle Completion, zero residue).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_close_wins_submit_started_before_close_rejected) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    UringAsyncBackend::BeforeAdmissionLockPauseGate gate;
    backend.set_before_admission_lock_pause_gate(&gate);

    std::byte buf[8]{std::byte{0x65}};
    Completion<std::size_t> c;
    std::thread submitter([&] {
        (void)backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c);
    });
    SLUICE_CHECK(wait_until([&] { return gate.paused.load(std::memory_order_acquire); }));
    ScopedGateResume resume(gate.resume, gate.exited);

    // Close completes with no contention (the submit has not taken the lock).
    backend.close_admission();

    resume.release();
    submitter.join();
    // The resumed submit must observe admission closed at Stage 0.
    SLUICE_CHECK(c.idle());
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    SLUICE_CHECK(backend.dispatch_size_for_test() == 0);
}

// ---------------------------------------------------------------------------
// C2e §3.2 — close then pending cancel: cancel may STILL win after close
// (Scheme B: no dispatch linkage, no syscall, canceled_ops == 1).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_close_then_pending_cancel_wins) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    UringAsyncBackend::AfterCommitBeforeEnqueuePauseGate gate;
    backend.set_after_commit_before_enqueue_pause_gate(&gate);

    std::byte buf[8]{std::byte{0x66}};
    Completion<std::size_t> c;
    std::thread submitter([&] {
        (void)backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c);
    });
    SLUICE_CHECK(wait_until([&] { return gate.paused.load(std::memory_order_acquire); }));
    ScopedGateResume resume(gate.resume, gate.exited);
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());

    backend.close_admission();
    const auto disp = backend.cancel_handle_for_test(*h);
    SLUICE_CHECK(disp == detail::CancelDisposition::terminal_won);
    SLUICE_CHECK(stats.canceled_ops == 1);
    SLUICE_CHECK(backend.live_cookies_for_test() == 0); // no SQE ever installed

    resume.release();
    submitter.join();
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    const auto res = c.result();
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == IoError::Code::canceled);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2e §3.2 — close then running cancel: intent only; the original operation
// CQE's result authority is unchanged (close is not an I/O cancellation
// primitive).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_close_then_running_cancel_intent_only) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    PipePair pipe;
    SLUICE_CHECK(pipe.valid());
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    std::byte buf[4]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_read(ReadOp{pipe.read_fd(), buf, 4, 0}, c).has_value());
    SLUICE_CHECK(backend.poll() == 0); // kernel blocks the read
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());

    backend.close_admission();
    const auto disp = backend.cancel_handle_for_test(*h);
    SLUICE_CHECK(disp == detail::CancelDisposition::intent_recorded);
    SLUICE_CHECK(!c.ready());
    SLUICE_CHECK(stats.canceled_ops == 0);

    pipe.close_write();
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    const auto res = c.result();
    SLUICE_CHECK(res.has_value() && res.value() == 0); // verbatim
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2e §3.3 — close wakes a PARKED wait_one as a ONE-SHOT control wake (0, no
// fabricated completion), and a FUTURE wait parks normally again and wakes on
// real progress (no busy-spin).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin) {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{4, 4});
    if (!backend->available())
        return;
    auto* raw = backend.get();
    std::atomic<bool> wait_phase{false};
    raw->set_wait_phase_flag_for_test(&wait_phase);
    AsyncIoContext ctx(std::move(backend));

    PipePair pipe;
    SLUICE_CHECK(pipe.valid());
    std::byte buf[4]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(ctx.submit_read(ReadOp{pipe.read_fd(), buf, 4, 0}, c).has_value());

    Result<std::size_t> w1{std::size_t{999}};
    std::thread waiter([&] { w1 = ctx.wait_one(); });
    // The waiter completes an empty reap and parks in the ring/control wait.
    SLUICE_CHECK(wait_until([&] { return wait_phase.load(std::memory_order_acquire); }));

    // close_admission wakes the parked participant: ONE-SHOT control wake —
    // the parked wait_one returns 0 (nothing reaped, no fabricated
    // completion).
    raw->close_admission();
    waiter.join();
    SLUICE_CHECK(w1.has_value());
    SLUICE_CHECK(w1.value() == 0);
    SLUICE_CHECK(ctx.outstanding() == 1); // the I/O was NOT cancelled by close

    // Future wait parks normally again (the flag re-arms — proving the
    // control wake was not sticky / no busy-spin) and wakes on REAL progress
    // with the reaped count.
    wait_phase.store(false, std::memory_order_release);
    Result<std::size_t> w2{std::size_t{999}};
    std::thread waiter2([&] { w2 = ctx.wait_one(); });
    SLUICE_CHECK(wait_until([&] { return wait_phase.load(std::memory_order_acquire); }));
    pipe.close_write(); // real progress
    waiter2.join();
    SLUICE_CHECK(w2.has_value());
    SLUICE_CHECK(w2.value() == 1);
    SLUICE_CHECK(c.ready());
    c.reset();
    SLUICE_CHECK(ctx.outstanding() == 0);
}

// ---------------------------------------------------------------------------
// C2e §23 — interrupt_all() wakes ALL parked participants (N=3), not one.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_multiple_parked_waiters_all_wake) {
    constexpr int kWaiters = 3;

    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 4});
    if (!backend->available())
        return;
    std::atomic<bool> wait_phase{false};
    backend->set_wait_phase_flag_for_test(&wait_phase);
    AsyncIoContext ctx(std::move(backend));

    // Three kernel-blocked reads, three participants.
    PipePair pipes[kWaiters];
    std::byte bufs[kWaiters][4]{};
    Completion<std::size_t> comps[kWaiters];
    for (int i = 0; i < kWaiters; ++i) {
        SLUICE_CHECK(pipes[i].valid());
        SLUICE_CHECK(ctx.submit_read(ReadOp{pipes[i].read_fd(), bufs[i], 4, 0}, comps[i]).has_value());
    }

    std::atomic<int> returned{0};
    Result<std::size_t> results[kWaiters] = {Result<std::size_t>{std::size_t{999}},
                                             Result<std::size_t>{std::size_t{999}},
                                             Result<std::size_t>{std::size_t{999}}};
    std::thread waiters[kWaiters];
    for (int i = 0; i < kWaiters; ++i) {
        waiters[i] = std::thread([&, i] {
            results[i] = ctx.wait_one();
            returned.fetch_add(1, std::memory_order_release);
        });
    }
    // All three reach the park (the wait-phase flag is set on every park;
    // poll the flag after each thread has had a chance to enter).
    SLUICE_CHECK(wait_until([&] { return wait_phase.load(std::memory_order_acquire); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    SLUICE_CHECK(returned.load(std::memory_order_acquire) == 0); // all parked

    // One interrupt must wake ALL parked participants (bounded join deadline =
    // hang watchdog; a single-wake mutant strands the others -> timeout).
    ctx.interrupt_backend_waiters();
    for (int i = 0; i < kWaiters; ++i) {
        waiters[i].join();
    }
    for (int i = 0; i < kWaiters; ++i) {
        SLUICE_CHECK(results[i].has_value());
        SLUICE_CHECK(results[i].value() == 0); // interrupted, nothing fabricated
    }
    SLUICE_CHECK(returned.load(std::memory_order_acquire) == kWaiters);

    // Drain for a clean teardown.
    for (int i = 0; i < kWaiters; ++i) {
        pipes[i].close_write();
    }
    for (int i = 0; i < kWaiters; ++i) {
        Result<std::size_t> r = ctx.wait_one();
        if (r.has_value() && r.value() > 0) {
            comps[i].reset();
        }
    }
    SLUICE_CHECK(ctx.outstanding() == 0);
}

// ---------------------------------------------------------------------------
// C2e §31 — interrupt-vs-final-ready: a terminal recorded in the exact window
// between the control wake and the context's final poll is REAPED by that
// final poll — wait_one returns the actual count (1), never 0. The control
// interrupt never swallows the last ready completion.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_interrupt_final_reap_closes_ready_race) {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{4, 4});
    if (!backend->available())
        return;
    auto* raw = backend.get();
    std::atomic<bool> wait_phase{false};
    raw->set_wait_phase_flag_for_test(&wait_phase);
    detail::UringWaitSource::ControlWakeFinalReapPauseGate gate;
    raw->set_wait_control_wake_final_reap_pause_gate(&gate);
    AsyncIoContext ctx(std::move(backend));

    PipePair pipe;
    SLUICE_CHECK(pipe.valid());
    std::byte buf[4]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(ctx.submit_read(ReadOp{pipe.read_fd(), buf, 4, 0}, c).has_value());

    Result<std::size_t> w{std::size_t{999}};
    std::thread waiter([&] { w = ctx.wait_one(); });
    SLUICE_CHECK(wait_until([&] { return wait_phase.load(std::memory_order_acquire); }));

    // Control wake; the wait source pauses JUST before reporting interrupted.
    ctx.interrupt_backend_waiters();
    SLUICE_CHECK(wait_until([&] { return gate.paused.load(std::memory_order_acquire); }));
    ScopedGateResume resume(gate.resume, gate.exited);

    // In the exact window, the terminal becomes ready (injected through the
    // routing layer; the waiter's final poll reaps it).
    raw->inject_cqe_for_test(1, 0);

    resume.release();
    waiter.join();
    // The final poll returned the actual completion count — not 0.
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w.value() == 1);
    SLUICE_CHECK(c.ready());
    const auto res = c.result();
    SLUICE_CHECK(res.has_value() && res.value() == 0);
    c.reset();
    SLUICE_CHECK(ctx.outstanding() == 0);
}

// ---------------------------------------------------------------------------
// C2e §32 — drained != releasable: accepted_outstanding == 0 with a
// completion-ready-but-unreset Completion leaves slot_in_use == 1; only the
// caller's Completion::reset() releases the slot.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_drained_not_releasable) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    std::byte buf[8]{std::byte{0x67}};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c).has_value());
    backend.close_admission();

    // Terminal + reap: drained but NOT releasable.
    drain_bounded(backend);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 1); // ready but unreset

    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0); // caller released it

    // A released slot does not re-open admission.
    Completion<std::size_t> c2;
    const auto r = backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c2);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
}

// ---------------------------------------------------------------------------
// C2e §34 — permanent transport poison + close_admission (P0-D must remain
// valid): new admission rejected, the Class-A request is locally retired with
// backend_error, close never submits the quarantined SQE, and destruction
// still requires full quiescence.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_poison_close_keeps_class_c) {
    struct Script {
        std::atomic<std::size_t> next{0};
        static int invoke(void* context, io_uring* ring) noexcept {
            auto& self = *static_cast<Script*>(context);
            const std::size_t index = self.next.fetch_add(1, std::memory_order_relaxed);
            if (index >= 1)
                return ::io_uring_submit(ring);
            return -EIO;
        }
    };
    Script script;
    UringBackendSubmitTestHooks hooks{&script, &Script::invoke, nullptr};
    UringAsyncBackend backend(UringConfig{2, 2}, hooks);
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    std::byte buf[4]{std::byte{0x68}};
    Completion<std::size_t> a;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 4, 0}, a).has_value());
    // poll() flushes: the scripted permanent -EIO poisons admission and
    // locally retires the Class-A request (its SQE was NEVER kernel-consumed).
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(a.ready());
    auto res = a.result();
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == IoError::Code::backend_error);
    SLUICE_CHECK(res.error().os_errno == EIO);
    SLUICE_CHECK(script.next.load(std::memory_order_relaxed) == 1); // one flush only
    a.reset();

    // close_admission on the poisoned backend: new admission rejected.
    backend.close_admission();
    Completion<std::size_t> b;
    const auto r = backend.submit_write(WriteOp{file.fd(), buf, 4, 0}, b);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error); // poison error
    SLUICE_CHECK(b.idle());
    SLUICE_CHECK(script.next.load(std::memory_order_relaxed) == 1); // nothing submitted

    // Quiescent teardown evidence: slots free; the quarantined Class-A
    // ledger entry remains as teardown evidence (recovery-retired — the
    // destructor preflight accepts exactly this state and the ring teardown
    // discards the never-consumed SQE representation; P0-D §6.3/§4.6).
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    SLUICE_CHECK(backend.transport_ledger_size_for_test() == 1);
    SLUICE_CHECK(backend.outstanding() == 0);
}

#else // !SLUICE_HAS_LIBURING — stub mode: build/API honesty only.

SLUICE_TEST_CASE(uring_c2e_close_while_pending_preserves_accepted) {}
SLUICE_TEST_CASE(uring_c2e_close_while_enqueued_preserves_dispatch) {}
SLUICE_TEST_CASE(uring_c2e_close_while_running_result_verbatim) {}
SLUICE_TEST_CASE(uring_c2e_close_while_backend_ready) {}
SLUICE_TEST_CASE(uring_c2e_void_submit_after_close_rejected) {}
SLUICE_TEST_CASE(uring_c2e_malformed_submit_after_close_rejected) {}
SLUICE_TEST_CASE(uring_c2e_close_waits_for_inflight_acceptance_lp) {}
SLUICE_TEST_CASE(uring_c2e_close_wins_submit_started_before_close_rejected) {}
SLUICE_TEST_CASE(uring_c2e_close_then_pending_cancel_wins) {}
SLUICE_TEST_CASE(uring_c2e_close_then_running_cancel_intent_only) {}
SLUICE_TEST_CASE(uring_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin) {}
SLUICE_TEST_CASE(uring_c2e_multiple_parked_waiters_all_wake) {}
SLUICE_TEST_CASE(uring_c2e_interrupt_final_reap_closes_ready_race) {}
SLUICE_TEST_CASE(uring_c2e_drained_not_releasable) {}
SLUICE_TEST_CASE(uring_c2e_poison_close_keeps_class_c) {}

#endif // SLUICE_HAS_LIBURING

SLUICE_MAIN()