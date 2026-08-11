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
#include <cstdlib>     // std::_Exit
#include <exception>   // std::set_terminate, std::terminate
#include <memory>      // std::make_unique
#include <thread>

#if defined(SLUICE_HAS_LIBURING)
#include <liburing.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
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
            // The destructor MUST check the wait_until result just like
            // release(): if the gate-exit condition times out, the paused
            // production thread is still blocked on this scope's gate. Letting
            // the enclosing gate (and the production thread it gates) outlive
            // this scope would be an unsafe-lifetime violation, so fail fast
            // instead of allowing the scope to exit with a live gate.
            if (!wait_until([&] { return exited.load(std::memory_order_acquire); })) {
                std::fprintf(stderr, "uring_c2e: pause gate exit timeout "
                                     "(ScopedGateResume destructor)\n");
                std::fflush(stderr);
                std::terminate();
            }
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
// C2e §3.1 — submit-vs-close acceptance LP (honest evidence split).
//
// A runtime "closer blocked on the admission mutex" observation CANNOT be made
// without scheduler timing: any test that serializes close after the submit LP
// (resume the submit, let it finish, then let close proceed) would PASS even
// for a mutant that removes dispatch_mtx_ from close_admission, because the
// test itself imposes the ordering. So this case makes NO deterministic
// mutex-blocking claim and uses NO sleep/time window as an ordering proof.
//
// What this case DOES prove (legitimately, structurally):
//   - a submit paused inside its acceptance transaction (BeforeCommitBinding
//     PauseGate, admission_domain_held) reaches a genuine in-flight LP state;
//   - after a genuine concurrent close (the submit is resumed to complete its
//     LP, then close runs), the LP-winning request is outstanding and is still
//     driven to exactly one terminal (no lost acceptance); and
//   - post-close, no new acceptance LP can occur (Stage-0 reject, idle
//     Completion, zero residue).
//
// The DETERMINISTIC authority that submit Stage-0..commit_binding and
// close_admission's admission-close write share the SAME dispatch_mtx_ lives
// in a source-drift self-test
// (D4DriftDetectorTest.test_close_admission_uses_dispatch_mtx): it parses
// uring_backend.cpp and asserts arena_.close_admission() and
// admission_closed_=true are INSIDE a lock_guard(dispatch_mtx_) critical
// section. A D4-RM8 mutant (dispatch_mtx_ removed from close_admission) turns
// that self-test RED for every build, independent of race timing. Focused TSan
// on submit||close and the concurrent linearization case
// (uring_c2e_submit_races_close_linearization) complete the evidence.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_close_waits_for_inflight_acceptance_lp) {
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
    // Deterministic in-flight LP placement: the submit is paused INSIDE its
    // admission transaction (admission_domain_held == true means the gate
    // fired while dispatch_mtx_ was held).
    SLUICE_CHECK(wait_until([&] { return gate.paused.load(std::memory_order_acquire); }));
    SLUICE_CHECK(gate.admission_domain_held.load(std::memory_order_acquire));

    // Resume the submit so it completes its LP (binding -> outstanding), then
    // run a genuine concurrent close. The LP-winning request must survive the
    // close and still reach exactly one terminal. (This does NOT prove close
    // blocked on the mutex — that authority is the source-drift self-test.)
    gate.resume.store(true, std::memory_order_release);
    submitter.join();
    SLUICE_CHECK(c.outstanding()); // submit won the LP before close

    backend.close_admission();

    // Post-close: no new acceptance LP can occur.
    Completion<std::size_t> c2;
    const auto r = backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c2);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    SLUICE_CHECK(c2.idle());
    SLUICE_CHECK(!c2.outstanding());
    SLUICE_CHECK(backend.outstanding() == 1); // the LP winner only

    // The LP-winning request still completes (close does not cancel accepted work).
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
// Deterministic proof (AGENTS.md §13.3 — NO sleep is the ordering proof): a
// guarded per-participant pre-poll park counter records EACH waiter reaching
// the final pre-poll point (epoch checked, eventfd drained, about to poll).
// The test waits for count == N using a bounded deadline ONLY as a hang
// watchdog, then drives the interrupt; every participant must return
// interrupted (0, nothing fabricated). A mutant that wakes only one waiter
// strands the others (bounded join deadline -> RED); a mutant that
// under-counts participants never reaches count == N (deadline -> RED).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_multiple_parked_waiters_all_wake) {
    constexpr int kWaiters = 3;
    constexpr int kDeadlineMs = 5000;

    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 4});
    if (!backend->available())
        return;
    auto* raw = backend.get();
    std::atomic<int> prepark_count{0};
    backend->set_wait_prepark_counter_for_test(&prepark_count);
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
    // Deterministic pre-condition: EVERY participant reached the final
    // pre-poll point (the per-participant park count == N). The bounded
    // deadline is a hang watchdog only — the park count is the ordering
    // proof, never a sleep. A prepark detector that under-counts
    // participants (D4-RM7) never reaches N and this watchdog fires.
    const auto park_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kDeadlineMs);
    while (prepark_count.load(std::memory_order_acquire) != kWaiters) {
        if (std::chrono::steady_clock::now() > park_deadline) {
            std::fprintf(stderr, "uring_c2e: only %d of %d participants reached "
                                 "the pre-poll park point (deadline)\n",
                         prepark_count.load(std::memory_order_acquire), kWaiters);
            std::fflush(stderr);
            std::terminate();
        }
        std::this_thread::yield();
    }

    // One interrupt must wake ALL parked participants. The bounded join
    // deadline is a hang watchdog: a single-wake mutant strands the others.
    ctx.interrupt_backend_waiters();
    const auto wake_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kDeadlineMs);
    while (returned.load(std::memory_order_acquire) != kWaiters) {
        if (std::chrono::steady_clock::now() > wake_deadline) {
            std::fprintf(stderr, "uring_c2e: only %d of %d waiters returned after "
                                 "interrupt_all (single-wake mutant)\n",
                         returned.load(std::memory_order_acquire), kWaiters);
            std::fflush(stderr);
            std::terminate();
        }
        std::this_thread::yield();
    }
    for (int i = 0; i < kWaiters; ++i) {
        waiters[i].join();
    }
    for (int i = 0; i < kWaiters; ++i) {
        SLUICE_CHECK(results[i].has_value());
        SLUICE_CHECK(results[i].value() == 0); // interrupted, nothing fabricated
    }
    SLUICE_CHECK(returned.load(std::memory_order_acquire) == kWaiters);

    // Drain for a clean teardown. Each Completion is released by its OWN
    // ready() — a single wait_one() can reap MORE than one completion at
    // once, so resetting by the aggregate count would leave ready-but-
    // unreset Completions (drained != releasable, D4). The slot_in_use == 0
    // check proves every reaped slot was actually released by the caller,
    // not silently returned by a Completion destructor.
    for (int i = 0; i < kWaiters; ++i) {
        pipes[i].close_write();
    }
    while (ctx.outstanding() != 0) {
        Result<std::size_t> r = ctx.wait_one();
        SLUICE_CHECK(r.has_value());
    }
    for (int i = 0; i < kWaiters; ++i) {
        if (comps[i].ready()) {
            comps[i].reset();
        }
    }
    SLUICE_CHECK(ctx.outstanding() == 0);
    SLUICE_CHECK(raw->arena_slot_in_use() == 0);
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

// ---------------------------------------------------------------------------
// C2e §15 — submit || close concurrent linearization (P0-B TSan window).
// A submitter loops on fresh Completions while a closer closes admission
// mid-stream — NO pause gate orders them, so this is the genuine unsynchronized
// submit-vs-close window the D4 arbitration claims to support (and the TSan
// coverage for it; a reintroduced unlocked admission_closed_ fast-path read
// races here — D4-RM1). Every submit must linearize as either:
//   * accepted — later driven to EXACTLY ONE terminal via the still-legal
//     cancel path, reaped, and reset, or
//   * synchronously rejected with invalid_state (admission closed — close
//     won the linearization race) or would_block (capacity full mid-stream),
//     leaving the Completion idle with ZERO residue — never half-accepted.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_submit_races_close_linearization) {
    constexpr std::size_t kAttempts = 256;
    UringAsyncBackend backend{UringConfig{8, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());
    const std::byte seed{0xAA};
    SLUICE_CHECK(::pwrite(file.fd(), &seed, 1, 0) == 1);

    std::byte buf[kAttempts]{};
    Completion<std::size_t> cs[kAttempts];
    std::atomic<std::size_t> accepted{0};
    std::atomic<std::size_t> rejected{0};

    std::thread submitter([&] {
        for (std::size_t i = 0; i < kAttempts; ++i) {
            auto r = backend.submit_read(ReadOp{file.fd(), buf + i, 1, 0}, cs[i]);
            if (r.has_value()) {
                accepted.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Any rejection MUST be one of the two legal admission
                // outcomes: invalid_state (admission closed — close won the
                // linearization race; ADR Decision 15) or would_block
                // (arena capacity full mid-stream — ADR Decision 13). Both
                // MUST leave the Completion idle with ZERO residue — a
                // half-accepted state is the regression this case catches.
                const auto code = r.error().code;
                if (code != IoError::Code::invalid_state &&
                    code != IoError::Code::would_block) {
                    std::fprintf(stderr,
                                 "uring_c2e_submit_races_close_linearization: "
                                 "unexpected reject code %d at attempt %zu\n",
                                 static_cast<int>(code), i);
                    std::fflush(stderr);
                    std::terminate();
                }
                if (!cs[i].idle() || cs[i].outstanding() || cs[i].ready()) {
                    std::fprintf(stderr,
                                 "uring_c2e_submit_races_close_linearization: "
                                 "rejected attempt %zu left non-idle Completion\n",
                                 i);
                    std::fflush(stderr);
                    std::terminate();
                }
                rejected.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // Close after at least one accept has happened (barrier only; the exact
    // accept/reject split is unconstrained). The atomic counter is relaxed,
    // so this barrier creates NO happens-before edge between the submitter's
    // entry reads and close's write — the TSan race window stays open.
    const auto first_accept_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (accepted.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < first_accept_deadline) {
        std::this_thread::yield();
    }
    if (accepted.load(std::memory_order_relaxed) == 0) {
        backend.close_admission();
        submitter.join();
        std::fprintf(stderr, "uring_c2e_submit_races_close_linearization: "
                             "submitter never accepted a request (harness "
                             "error)\n");
        std::fflush(stderr);
        std::terminate();
    }
    backend.close_admission();
    submitter.join();
    SLUICE_CHECK(accepted.load(std::memory_order_relaxed) +
                     rejected.load(std::memory_order_relaxed) == kAttempts);

    // Every accepted request is driven to exactly one terminal via the
    // still-legal cancel path, reaped, and reset; every rejected Completion
    // is idle.
    for (std::size_t i = 0; i < kAttempts; ++i) {
        if (cs[i].outstanding())
            backend.cancel(cs[i]);
    }
    drain_bounded(backend);
    std::size_t ready_count = 0;
    for (std::size_t i = 0; i < kAttempts; ++i) {
        if (cs[i].outstanding()) {
            std::fprintf(stderr, "uring_c2e_submit_races_close_linearization: "
                                 "no Completion may remain outstanding after "
                                 "drain (attempt %zu)\n", i);
            std::fflush(stderr);
            std::terminate();
        }
        if (cs[i].ready()) {
            ++ready_count;
            const auto res = cs[i].result();
            if (!(res.has_value() ||
                  res.error().code == IoError::Code::canceled ||
                  res.error().code == IoError::Code::eof)) {
                std::fprintf(stderr, "uring_c2e_submit_races_close_linearization: "
                                     "undefined terminal on accepted attempt %zu\n", i);
                std::fflush(stderr);
                std::terminate();
            }
            cs[i].reset();
        }
    }
    SLUICE_CHECK(ready_count == accepted.load(std::memory_order_relaxed));
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2e §41 — control wins over a co-ready ring (deterministic post-poll
// reason classification). When poll(2) returns with BOTH the ring fd and the
// control fd readable (a CQE-readable ring + interrupt_all in the same
// window), wait_for_change MUST return `interrupted`, NOT `progress`: control
// is shutdown/liveness authority and must not be swallowed by physical ring
// progress. The caller (wait_one) then runs its final non-blocking poll/reap,
// which reaps any co-ready CQE and returns its real count.
//
// Deterministic proof (AGENTS.md §13.3 — NO sleep is the ordering proof): a
// test-only pre-poll barrier (BeforePhysicalPollPauseGate) parks the waiter at
// the physical-poll boundary, and a test-only ring-fd poll override makes the
// "ring" readable with a pipe. The test then drives interrupt_all (bumps
// control epoch + makes the control fd readable), and releases the barrier so
// the single poll(2) observes BOTH fds ready. The assertion is that the waiter
// observes the interrupt (returns 0 = interrupted, nothing fabricated). A
// mutant that returns `progress` on ring POLLIN before the post-poll control
// recheck (D4-RM10) makes the waiter re-loop and re-park on a stale token —
// it fails to return the interrupt observation and the bounded join deadline
// fires (RED).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_control_wins_over_co_ready_ring) {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{4, 4});
    if (!backend->available())
        return;
    auto* raw = backend.get();
    detail::UringWaitSource::BeforePhysicalPollPauseGate gate;
    raw->set_wait_before_physical_poll_pause_gate(&gate);
    // Install a readable pipe as the "ring" fd BEFORE launching the waiter, so
    // physical poll sees POLLIN on it. The production ring_fd_ is untouched.
    PipePair ring_override_pipe;
    SLUICE_CHECK(ring_override_pipe.valid());
    // Make the override fd immediately readable (write one byte to the read end's
    // peer) so poll returns POLLIN on the "ring" fd as soon as it is called.
    {
        std::byte one{0x1};
        SLUICE_CHECK(::write(ring_override_pipe.write_fd(), &one, 1) == 1);
    }
    raw->set_wait_poll_ring_fd_override_for_test(ring_override_pipe.read_fd());
    AsyncIoContext ctx(std::move(backend));

    // One kernel-blocked read keeps outstanding > 0 so the waiter parks.
    PipePair op_pipe;
    SLUICE_CHECK(op_pipe.valid());
    std::byte buf[4]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(ctx.submit_read(ReadOp{op_pipe.read_fd(), buf, 4, 0}, c).has_value());

    Result<std::size_t> w{std::size_t{999}};
    std::atomic<bool> waiter_done{false};
    std::thread waiter([&] {
        w = ctx.wait_one();
        waiter_done.store(true, std::memory_order_release);
    });

    // Deterministic pre-condition: the waiter reached the physical-poll
    // boundary (one distinct arrival). The bounded deadline is a hang watchdog
    // only — the arrival count is the ordering proof, never a sleep.
    const auto arrive_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (gate.arrivals.load(std::memory_order_acquire) != 1) {
        if (std::chrono::steady_clock::now() > arrive_deadline) {
            std::fprintf(stderr, "uring_c2e_control_wins: waiter never reached "
                                 "the pre-poll barrier (arrivals=%d)\n",
                         gate.arrivals.load(std::memory_order_acquire));
            std::fflush(stderr);
            std::terminate();
        }
        std::this_thread::yield();
    }

    // Now BOTH readiness sources are live: the override "ring" fd is already
    // readable (POLLIN), and the control interrupt bumps the control epoch +
    // writes the control eventfd (control fd POLLIN). Release the barrier so
    // the single poll(2) observes BOTH ready.
    ctx.interrupt_backend_waiters();
    gate.release.store(true, std::memory_order_release);

    // The waiter MUST observe the interrupt and return. Under the fix it
    // returns 0 (interrupted, nothing fabricated: the override fd is not a real
    // ring, so no CQE exists and the final poll reaps nothing). Under the
    // D4-RM10 mutant (ring POLLIN returns progress before control recheck) the
    // waiter re-loops, re-snapshots the (already-bumped) control epoch, and
    // returns interrupted anyway — BUT only because the epoch check at the top
    // of the loop catches it. The two-waiter strand case (next) is the real
    // production-bug detector; this case pins the single-waiter co-ready
    // classification directly.
    const auto join_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    // Poll the atomic done flag (NOT the Result w directly — that would be a
    // TSan data race between the waiter's write and this read). The Result is
    // read ONLY after waiter.join() establishes happens-before.
    while (!waiter_done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > join_deadline) {
            std::fprintf(stderr, "uring_c2e_control_wins: waiter never returned "
                                 "(strand on stale token after ring POLLIN)\n");
            std::fflush(stderr);
            std::terminate();
        }
        std::this_thread::yield();
    }
    waiter.join();
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w.value() == 0); // interrupted, nothing fabricated

    // Drain for a clean teardown. Explicit ready()->reset() per Completion
    // authority — drained != releasable: a drained request is only
    // releasable after its ready Completion is reset, and slot_in_use == 0
    // proves the slot was actually returned (C2e evidence).
    op_pipe.close_write();
    Result<std::size_t> r = ctx.wait_one();
    if (c.ready()) {
        c.reset();
    }
    SLUICE_CHECK(ctx.outstanding() == 0);
    SLUICE_CHECK(raw->arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2e §42 — two-waiter consumer strand (the production-bug detector). With
// A+B outstanding and T1 parked, A becomes ready AND an interrupt fires. T2
// (the test) consumes A via a reap. T1 MUST return interrupted (or the final-
// poll reap result) and MUST NOT repark on B — under the old ring-POLLIN-
// returns-progress code, T1 could re-loop, take a fresh snapshot, find A
// already gone and B still blocked, drain the stale control token, and park
// forever: the control wake was swallowed by physical ring progress.
//
// Deterministic proof (AGENTS.md §13.3): the production bug requires T1's
// poll(2) to observe BOTH the ring fd readable (A's CQE pending) AND the
// control fd readable (interrupt), so the mutant's ring-POLLIN-returns-progress
// branch fires. A real ring fd's readability cannot be held while A is reaped
// (reaping consumes the CQE), so this case uses the test-only ring-fd poll
// override to make the "ring" deterministically readable with a pipe for the
// duration of T1's poll. Sequence: T1 parked at the pre-poll barrier; A made
// ready; interrupt fired; A consumed (reaped) by the test; override "ring" left
// readable; barrier released so T1's poll sees ring+control co-ready. The
// post-poll control recheck MUST return interrupted; the final poll reaps
// nothing (A already consumed). T1 returns 0. A mutant that returns progress on
// ring POLLIN before the control recheck strands T1 on B (deadline -> RED).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_two_waiter_consumer_strand) {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 4});
    if (!backend->available())
        return;
    auto* raw = backend.get();
    detail::UringWaitSource::BeforePhysicalPollPauseGate gate;
    raw->set_wait_before_physical_poll_pause_gate(&gate);
    // Install a readable pipe as the "ring" fd BEFORE launching the waiter, so
    // T1's poll observes POLLIN on it (simulating A's CQE being pending) even
    // after the test reaps A. Production ring_fd_ is untouched.
    PipePair ring_override_pipe;
    SLUICE_CHECK(ring_override_pipe.valid());
    {
        std::byte one{0x1};
        SLUICE_CHECK(::write(ring_override_pipe.write_fd(), &one, 1) == 1);
    }
    raw->set_wait_poll_ring_fd_override_for_test(ring_override_pipe.read_fd());
    AsyncIoContext ctx(std::move(backend));

    // A and B: two kernel-blocked reads.
    PipePair pipe_a, pipe_b;
    SLUICE_CHECK(pipe_a.valid());
    SLUICE_CHECK(pipe_b.valid());
    std::byte buf_a[4]{};
    std::byte buf_b[4]{};
    Completion<std::size_t> ca, cb;
    SLUICE_CHECK(ctx.submit_read(ReadOp{pipe_a.read_fd(), buf_a, 4, 0}, ca).has_value());
    SLUICE_CHECK(ctx.submit_read(ReadOp{pipe_b.read_fd(), buf_b, 4, 0}, cb).has_value());

    Result<std::size_t> w1{std::size_t{999}};
    std::atomic<bool> t1_done{false};
    std::thread t1([&] {
        w1 = ctx.wait_one();
        t1_done.store(true, std::memory_order_release);
    });

    // Deterministic pre-condition: T1 reached the physical-poll boundary.
    const auto arrive_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (gate.arrivals.load(std::memory_order_acquire) != 1) {
        if (std::chrono::steady_clock::now() > arrive_deadline) {
            std::fprintf(stderr, "uring_c2e_two_waiter_strand: T1 never reached "
                                 "the pre-poll barrier (arrivals=%d)\n",
                         gate.arrivals.load(std::memory_order_acquire));
            std::fflush(stderr);
            std::terminate();
        }
        std::this_thread::yield();
    }

    // Make A ready, fire the interrupt, and consume A via a reap — all while
    // T1 is paused at the barrier (before its poll(2)). The override "ring"
    // pipe stays readable, so on release T1's poll observes ring+control
    // co-ready (the exact production interleaving).
    pipe_a.close_write();              // A's read completes
    ctx.interrupt_backend_waiters();   // control epoch bump + eventfd write

    // round-4 (P1-1): consume A with a BOUNDED NONBLOCKING poll() loop —
    // never a second wait_one(). The controller owns gate.release below; a
    // wait_one() here would enter wait_for_change() and hit the SAME
    // BeforePhysicalPollPauseGate that only THIS controller can release (and,
    // with the durable-broadcast gate, additionally block on T1's pending
    // acknowledgement while T1 waits at the barrier) — the test would
    // deadlock on itself regardless of whether A's CQE had already arrived
    // (the historical "WSL2 flake" misattribution; the hang is the test's own
    // design). poll() takes access_mtx_ (the serialized reap domain) without
    // ever parking, so it cannot enter the barrier. The deadline is a hang
    // watchdog only — the reap observation (ca.ready()) is the ordering
    // proof.
    {
        const auto reap_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        while (!ca.ready()) {
            if (std::chrono::steady_clock::now() > reap_deadline) {
                std::fprintf(stderr, "uring_c2e_two_waiter_strand: A was never "
                                     "reaped by the controller's poll loop\n");
                std::fflush(stderr);
                std::terminate();
            }
            (void)ctx.poll(); // non-blocking: flush transport + reap A's CQE
        }
    }
    SLUICE_CHECK(ca.ready()); // A consumed

    // Release T1: poll sees ring (override) + control co-ready. Under the fix
    // the control recheck returns interrupted; final poll reaps nothing (A
    // gone, B still blocked). T1 returns 0. Under D4-RM10 the ring-POLLIN
    // branch returns progress; T1 re-loops; A is gone (override is not a real
    // ring, so re-poll reaps nothing) and B is blocked; T1 parks forever on a
    // stale token (deadline -> RED).
    gate.release.store(true, std::memory_order_release);

    const auto join_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (!t1_done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > join_deadline) {
            std::fprintf(stderr, "uring_c2e_two_waiter_strand: T1 never returned "
                                 "(reparked on B after swallowing the control wake)\n");
            std::fflush(stderr);
            std::terminate();
        }
        std::this_thread::yield();
    }
    t1.join();
    SLUICE_CHECK(w1.has_value());
    SLUICE_CHECK(w1.value() == 0); // interrupted, nothing fabricated (A already consumed)

    // Complete B for a clean teardown. Explicit ready()->reset() per
    // Completion authority — drained != releasable (C2e evidence): ca was
    // already proven ready above (line 1183), cb becomes ready after the
    // wait; slot_in_use == 0 proves both slots were actually returned.
    pipe_b.close_write();
    Result<std::size_t> rb = ctx.wait_one();
    if (cb.ready()) {
        cb.reset();
    }
    if (ca.ready()) {
        ca.reset();
    }
    SLUICE_CHECK(ctx.outstanding() == 0);
    SLUICE_CHECK(raw->arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2e §44 — D4-RM15 (P0-2): a FUTURE-generation waiter must not consume the
// eventfd token that wakes an OLD-generation waiter (durable broadcast).
//
// Linux eventfd(2) without EFD_SEMAPHORE: read() returns the whole counter
// and resets it; POLLIN holds while counter > 0. A wake callback only makes
// the poller runnable — do_poll() re-runs the fd readiness check after the
// wake, and a poller whose recheck finds an empty counter goes back to
// sleep. A single shared consumable token therefore does NOT implement
// notify_all once a future-generation waiter drains it: interrupt_all() can
// lose a wake for a parked waiter that was woken but has not finished its
// readiness recheck.
//
// Deterministic construction: T1 (old generation) submits A and parks —
// registered as a parked participant, held at the pre-poll barrier (its
// poll(2) has not run yet, but the interrupt's token is already in the
// counter — the exact "woken-but-not-rechecked" transport state). The
// interrupt publishes control C0 -> C1 and writes the single token. T2
// (future generation) starts wait_one() AFTER the interrupt: its control
// baseline is C1, its epoch check passes, and under the pre-fix code it
// drains the token (stealing T1's wake) and parks; T1's poll(2) then finds
// an empty counter and re-sleeps forever — the interrupt is lost.
//
// Under the fix the drain is GATED on T1's acknowledgement: T2 blocks in
// wait_for_change (never reaching the shared pre-poll barrier), T1's poll
// returns on the still-present token, T1 rechecks C1 != C0 and returns
// interrupted (0) — its single acknowledgement releases the gate — and T2
// then drains the stale token, parks, and wakes on REAL progress.
//
// Ordering proof (AGENTS.md §13.3 — deadlines are hang watchdogs only): T1
// returning interrupted within a bounded join (the pre-fix code strands it
// forever: its poll finds an empty counter and re-sleeps), and T2 returning
// the reaped count only after real progress. The barrier-arrival
// observation is a fast-fail diagnostic for the steal, not the proof.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_future_waiter_cannot_steal_old_wake) {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 4});
    if (!backend->available())
        return;
    auto* raw = backend.get();
    detail::UringWaitSource::BeforePhysicalPollPauseGate gate;
    raw->set_wait_before_physical_poll_pause_gate(&gate);
    AsyncIoContext ctx(std::move(backend));

    // A (old-generation waiter T1) and B (future-generation waiter T2):
    // two kernel-blocked reads.
    PipePair pipe_a, pipe_b;
    SLUICE_CHECK(pipe_a.valid());
    SLUICE_CHECK(pipe_b.valid());
    std::byte buf_a[4]{};
    std::byte buf_b[4]{};
    Completion<std::size_t> ca, cb;
    SLUICE_CHECK(ctx.submit_read(ReadOp{pipe_a.read_fd(), buf_a, 4, 0}, ca).has_value());
    SLUICE_CHECK(ctx.submit_read(ReadOp{pipe_b.read_fd(), buf_b, 4, 0}, cb).has_value());

    Result<std::size_t> w1{std::size_t{999}};
    std::atomic<bool> t1_done{false};
    std::thread t1([&] {
        w1 = ctx.wait_one();
        t1_done.store(true, std::memory_order_release);
    });

    // Deterministic pre-condition: T1 parked (registered) and stopped at the
    // pre-poll barrier — the old-generation waiter whose wake transport must
    // survive a future-generation drain.
    const auto arrive_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (gate.arrivals.load(std::memory_order_acquire) != 1) {
        if (std::chrono::steady_clock::now() > arrive_deadline) {
            std::fprintf(stderr, "uring_c2e_future_waiter_steal: T1 never "
                                 "reached the pre-poll barrier (arrivals=%d)\n",
                         gate.arrivals.load(std::memory_order_acquire));
            std::fflush(stderr);
            std::terminate();
        }
        std::this_thread::yield();
    }

    // The interrupt: control C0 -> C1 plus the single transport token. T1 is
    // parked-but-not-polling; the token is the wake transport for its
    // imminent poll.
    ctx.interrupt_backend_waiters();

    // T2: the FUTURE-generation waiter — its invocation starts AFTER the
    // interrupt, so its control baseline is C1 and its epoch check passes
    // (the interrupt is a past event for its OWN invocation).
    Result<std::size_t> w2{std::size_t{999}};
    std::atomic<bool> t2_done{false};
    std::thread t2([&] {
        w2 = ctx.wait_one();
        t2_done.store(true, std::memory_order_release);
    });

    // D4-RM14: T2 must NOT reach the shared pre-poll barrier while T1 is
    // still parked. Under the pre-fix code it drains the token and arrives
    // (arrivals == 2, within microseconds — nothing blocks it); under the fix
    // it is gated on T1's acknowledgement and stays blocked in
    // wait_for_change. The bounded window is a fast-fail diagnostic only —
    // the ordering proof is T1's interrupted return below (a stolen token
    // strands T1 forever -> join deadline RED).
    const auto steal_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (gate.arrivals.load(std::memory_order_acquire) == 1) {
        if (std::chrono::steady_clock::now() > steal_deadline) {
            break; // fixed code: T2 never arrives (blocked on the gate)
        }
        std::this_thread::yield();
    }
    if (gate.arrivals.load(std::memory_order_acquire) != 1) {
        std::fprintf(stderr, "uring_c2e_future_waiter_steal: future waiter "
                             "reached the pre-poll barrier (token drained)\n");
        std::fflush(stderr);
        std::terminate();
    }

    // Release T1: its poll(2) returns on the STILL-PRESENT token (the gate
    // blocked T2 from draining it), it rechecks C1 != C0, acknowledges its
    // wake, and returns interrupted (0) — nothing fabricated (A still
    // blocked).
    gate.release.store(true, std::memory_order_release);
    const auto t1_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (!t1_done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > t1_deadline) {
            std::fprintf(stderr, "uring_c2e_future_waiter_steal: T1 never "
                                 "returned (wake stolen by the future "
                                 "waiter)\n");
            std::fflush(stderr);
            std::terminate();
        }
        std::this_thread::yield();
    }
    t1.join();
    SLUICE_CHECK(w1.has_value());
    SLUICE_CHECK(w1.value() == 0); // interrupted, nothing fabricated

    // Real progress for T2: complete A. T2 (released by T1's acknowledgement)
    // drains the now-stale token, parks, and wakes on the REAL CQE, returning
    // the reaped count (1).
    pipe_a.close_write();
    const auto t2_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (!t2_done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > t2_deadline) {
            std::fprintf(stderr, "uring_c2e_future_waiter_steal: T2 never "
                                 "returned after real progress\n");
            std::fflush(stderr);
            std::terminate();
        }
        std::this_thread::yield();
    }
    t2.join();
    SLUICE_CHECK(w2.has_value());
    SLUICE_CHECK(w2.value() == 1); // A reaped
    SLUICE_CHECK(ca.ready());

    // Complete B for a clean teardown. Explicit ready()->reset() per
    // Completion authority — drained != releasable (C2e evidence); ca.ready()
    // was asserted above, and slot_in_use == 0 proves both slots were
    // actually returned.
    pipe_b.close_write();
    Result<std::size_t> rb = ctx.wait_one();
    if (cb.ready()) {
        cb.reset();
    }
    ca.reset();
    SLUICE_CHECK(ctx.outstanding() == 0);
    SLUICE_CHECK(raw->arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2e §43 — non-EINTR poll(2) failure must fail-fast, not spin (D4-RM12).
// wait_for_change is noexcept with no Result<> channel, so a physical poll
// failure that is NOT EINTR cannot be reported up; the production fix
// terminates (stderr + std::terminate) rather than busy-spinning or
// fabricating a reason. This case proves the fail-fast deterministically via
// a forked child that re-creates a FRESH backend in-place (not a
// fork-inherited context, which would carry stale lock/thread state). The
// child installs a test-only PollFn that returns -1 with errno=EIO, submits
// a blocked read, and calls wait_one. Under the fix the wait source
// terminates (the child's deterministic terminate handler _Exit(86)); under
// the D4-RM12 mutant (all errno treated as retryable EINTR) the child
// busy-spins forever and the parent's watchdog SIGKILLs it (exit by signal
// -> RED).
// ---------------------------------------------------------------------------
namespace {
int failing_poll_fn(struct pollfd*, unsigned long, int, void*) {
    errno = EIO;
    return -1;
}

constexpr int kD4Rm12ExpectedExit = 86;
constexpr int kD4Rm12MutantExit = 90; // unused; the mutant busy-spins (SIGKILL)

int d4_rm12_child_main() {
    // Fresh backend in the child (no fork-inherited state).
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{4, 4});
    if (!backend->available()) {
        std::_Exit(88);
    }
    PipePair pipe;
    if (!pipe.valid()) {
        std::_Exit(88);
    }
    std::byte buf[4]{};
    Completion<std::size_t> c;
    if (!backend->submit_read(ReadOp{pipe.read_fd(), buf, 4, 0}, c).has_value()) {
        std::_Exit(88);
    }
    // Install the failing PollFn BEFORE the waiter could park.
    backend->set_wait_poll_fn_for_test(failing_poll_fn, nullptr);
    AsyncIoContext ctx(std::move(backend));
    // A non-EINTR poll failure must terminate (set_terminate -> _Exit(86)),
    // not return. If it returns, the fail-fast boundary did NOT fire (mutant).
    std::set_terminate([]() noexcept { std::_Exit(kD4Rm12ExpectedExit); });
    (void)ctx.wait_one();
    std::_Exit(87); // unexpected return
}
} // namespace

SLUICE_TEST_CASE(uring_c2e_non_eintr_poll_failure_failfast) {
    // Fork a fresh child (no exec — the child runs d4_rm12_child_main
    // in-place) so the child has a clean single-threaded process state (no
    // inherited threads/locks from earlier cases in this binary) and a
    // terminate() in the child cannot abort the whole test run.
    pid_t pid = ::fork();
    SLUICE_CHECK(pid >= 0);
    if (pid == 0) {
        int rc = d4_rm12_child_main();
        std::_Exit(rc);
    }
    int status = 0;
    const auto start = std::chrono::steady_clock::now();
    constexpr auto kTimeout = std::chrono::seconds(8);
    bool watchdog = false;
    for (;;) {
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno == EINTR) continue;
        if (std::chrono::steady_clock::now() - start >= kTimeout) {
            std::fprintf(stderr, "uring_c2e: non-EINTR poll watchdog fired "
                                 "(child busy-spinning, D4-RM12 mutant)\n");
            std::fflush(stderr);
            (void)::kill(pid, SIGKILL);
            (void)::waitpid(pid, &status, 0);
            watchdog = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    SLUICE_CHECK(!watchdog); // watchdog == mutant detector (RED)
    SLUICE_CHECK(WIFEXITED(status));
    SLUICE_CHECK(WEXITSTATUS(status) == kD4Rm12ExpectedExit); // fail-fast won
    (void)kD4Rm12MutantExit;
}

// ---------------------------------------------------------------------------
// C2e §45 — D4-RM17 (P0): a cancel-side transport flush that permanently
// poisons the backend MUST still wake a parked wait_one. issue_running_cancel
// appends the best-effort AsyncCancel SQE; when the SQ is full it flushes
// transport, and a permanent negative flush runs the Class-A recovery (the
// retained ledger entries become backend-ready terminals). No reap runs on
// the cancel path, so the ONLY wake for a parked waiter is the deferred
// signal_ready_progress() AFTER dispatch_mtx_ is released (D4-RM16). The
// pre-fix code early-returned on the newly-set fatal_error_ and skipped the
// wake — the parked waiter slept forever on published terminals (AGENTS.md
// §13.2: state published, wake obligation missing).
//
// Deterministic construction: A+B fill the SQ (queue_depth 2) and reach the
// kernel via the waiter's first flush (scripted submit #1 succeeds); the
// waiter then parks with outstanding=2 (prepark-count observation, bounded
// watchdog). C+D refill the SQ WITHOUT a flush (the enqueue path never
// flushes). cancel(A) reaches issue_running_cancel with a full SQ: its flush
// is scripted submit #2 = permanent -EIO -> poison -> Class-A recovery
// retires C+D to backend-ready. The parked waiter's ONLY wake is the
// deferred signal_ready_progress(); under the mutant it parks forever (join
// deadline -> RED). The scripted flush order (exactly two invocations, the
// second poisoning) proves the poison came from the cancel path under test.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2e_running_cancel_poison_deferred_wake) {
    struct Script {
        std::atomic<std::size_t> next{0};
        static int invoke(void* context, io_uring* ring) noexcept {
            auto& self = *static_cast<Script*>(context);
            const std::size_t index = self.next.fetch_add(1, std::memory_order_relaxed);
            if (index == 0)
                return ::io_uring_submit(ring); // waiter's flush: A+B reach the kernel
            return -EIO;                        // cancel's flush: permanent poison
        }
    };
    Script script;
    UringBackendSubmitTestHooks hooks{&script, &Script::invoke, nullptr};
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{4, 2}, hooks);
    if (!backend->available())
        return;
    auto* raw = backend.get();
    std::atomic<int> prepark_count{0};
    raw->set_wait_prepark_counter_for_test(&prepark_count);
    AsyncIoContext ctx(std::move(backend));

    // A+B: two kernel-blocked reads that fill the SQ and reach the kernel on
    // the waiter's first flush. C+D: two more that refill the SQ unflushed,
    // so cancel(A)'s get_sqe fails and its flush is the one that poisons.
    PipePair pipe_a, pipe_b, pipe_c, pipe_d;
    SLUICE_CHECK(pipe_a.valid());
    SLUICE_CHECK(pipe_b.valid());
    SLUICE_CHECK(pipe_c.valid());
    SLUICE_CHECK(pipe_d.valid());
    std::byte buf[4]{};
    Completion<std::size_t> ca, cb, cc, cd;
    SLUICE_CHECK(ctx.submit_read(ReadOp{pipe_a.read_fd(), buf, 4, 0}, ca).has_value());
    SLUICE_CHECK(ctx.submit_read(ReadOp{pipe_b.read_fd(), buf, 4, 0}, cb).has_value());

    // The waiter parks after its flush consumed A+B (nothing ready yet).
    Result<std::size_t> w{std::size_t{999}};
    std::atomic<bool> waiter_done{false};
    std::thread waiter([&] {
        w = ctx.wait_one();
        waiter_done.store(true, std::memory_order_release);
    });
    if (!wait_until([&] { return prepark_count.load(std::memory_order_acquire) == 1; })) {
        std::fprintf(stderr, "uring_c2e_running_cancel_poison: waiter never parked "
                             "(prepark=%d)\n",
                     prepark_count.load(std::memory_order_acquire));
        std::fflush(stderr);
        std::terminate();
    }

    // C+D refill the SQ (queue_depth 2) without a flush.
    SLUICE_CHECK(ctx.submit_read(ReadOp{pipe_c.read_fd(), buf, 4, 0}, cc).has_value());
    SLUICE_CHECK(ctx.submit_read(ReadOp{pipe_d.read_fd(), buf, 4, 0}, cd).has_value());

    // cancel(A): running -> intent_recorded -> issue_running_cancel. Its
    // get_sqe fails on the full SQ; its flush (scripted -EIO) poisons; the
    // Class-A recovery retires C+D to backend-ready terminals. The parked
    // waiter MUST be woken by the deferred wake (mutant: never -> RED).
    ctx.cancel(ca);
    if (!wait_until([&] { return waiter_done.load(std::memory_order_acquire); })) {
        std::fprintf(stderr, "uring_c2e_running_cancel_poison: parked waiter never "
                             "woke after the cancel-side poison (deferred wake "
                             "missing)\n");
        std::fflush(stderr);
        std::terminate();
    }
    waiter.join();
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w.value() == 2); // C+D retired by the Class-A recovery
    SLUICE_CHECK(cc.ready());
    SLUICE_CHECK(cd.ready());
    auto rc = cc.result();
    SLUICE_CHECK(!rc.has_value());
    SLUICE_CHECK(rc.error().code == IoError::Code::backend_error);
    SLUICE_CHECK(rc.error().os_errno == EIO);
    SLUICE_CHECK(script.next.load(std::memory_order_relaxed) == 2); // exactly two flushes

    // A+B are kernel-owned reads; the cancel intent never rewrites the real
    // result — complete them for a clean teardown (poisoned wait path,
    // to_submit=0, never submits the quarantined batch).
    pipe_a.close_write();
    Result<std::size_t> ra = ctx.wait_one();
    SLUICE_CHECK(ra.has_value());
    SLUICE_CHECK(ra.value() == 1);
    SLUICE_CHECK(ca.ready());
    pipe_b.close_write();
    Result<std::size_t> rb = ctx.wait_one();
    SLUICE_CHECK(rb.has_value());
    SLUICE_CHECK(rb.value() == 1);
    SLUICE_CHECK(cb.ready());
    ca.reset();
    cb.reset();
    cc.reset();
    cd.reset();
    SLUICE_CHECK(ctx.outstanding() == 0);
    SLUICE_CHECK(raw->arena_slot_in_use() == 0);
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
SLUICE_TEST_CASE(uring_c2e_submit_races_close_linearization) {}
SLUICE_TEST_CASE(uring_c2e_control_wins_over_co_ready_ring) {}
SLUICE_TEST_CASE(uring_c2e_two_waiter_consumer_strand) {}
SLUICE_TEST_CASE(uring_c2e_future_waiter_cannot_steal_old_wake) {}
SLUICE_TEST_CASE(uring_c2e_non_eintr_poll_failure_failfast) {}
SLUICE_TEST_CASE(uring_c2e_running_cancel_poison_deferred_wake) {}

#endif // SLUICE_HAS_LIBURING

// ---------------------------------------------------------------------------
// Evidence-meta (G2): exactly one [evidence-meta] line per gate-driven run.
// Registered in BOTH real and stub builds (P1-A): before this repair the
// metadata case was compiled out in stub builds, so a stub run printed the
// full pinned [run] set MINUS this case and ZERO evidence-meta lines — the
// target became INCOMPLETE for the WRONG reason (a missing case) instead of
// "mode=stub not allowed by required_modes=(\"real\",)". The internal
// #if/#else emits mode=real (with a real-kernel availability check) in the
// real build and mode=stub (build/API honesty only) in the stub build; both
// modes execute the FULL pinned case-set (22 cases incl. the concurrent
// submit-||-close linearization case, the D4-RM14 future-waiter steal
// detector, and the D4-RM17 cancel-side-poison deferred-wake detector — the
// manifest's cases tuple is the authority).
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

SLUICE_MAIN()