// Phase D3 — Uring C2b identity / generation / cancel integration evidence.
//
// Real mode exercises the authoritative production uring_backend.cpp with only
// read-only SLUICE_ASYNC_INTERNAL_TESTING observations and the two
// deterministic pause gates (pending window: AfterCommitBeforeEnqueuePauseGate;
// enqueued window: BeforeDispatchTransferPauseGate — the ThreadPool Gate-B
// analogue that releases dispatch_mtx_ while paused). Every seam delegates to
// REAL production authority: RequestArena (full SlotHandle validation,
// generation, terminal winner), ReferenceReadySink (reap publication), and the
// production cancel core (cancel_handle_). No test-side identity map, no second
// generation counter, no reimplemented state machine.
//
// Stub mode proves build and API honesty only; the manifest requires evidence
// mode=real before this target can satisfy the uring_c2b_identity_integration
// record (G2 discipline: the pinned case set must run exactly once each).
#include "harness.hpp"

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
#include <unistd.h>

#include <cstring>
#include <vector>
#endif

using namespace sluice::async;
using sluice::IoError;

#if defined(SLUICE_HAS_LIBURING)

namespace {

// Bounded deadline helper: spins on a predicate (pause-gate observation) with a
// hang-watchdog bound. A deadline may prevent an infinite hang; it NEVER
// establishes ordering — all ordering claims come from the deterministic pause
// gates / kernel-visible states.
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

// RAII: resume a pause gate on scope exit (normal AND failure paths) and wait
// for the paused production path to exit the gate. Mirrors the ThreadPool
// ScopedGateResume discipline: a test that fails mid-window must still release
// the paused production path or the backend destructor would hang.
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
            std::fprintf(stderr, "uring_c2b: pause gate exit timeout\n");
            std::fflush(stderr);
            std::terminate();
        }
        done = true;
    }
};

class TempFile {
  public:
    TempFile() {
        char path[] = "/tmp/sluice_uring_d3_c2b_XXXXXX";
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
    // Close the write end: a blocked read on this pipe then completes with
    // 0 bytes (EOF) — a kernel-visible deterministic terminal.
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
SLUICE_TEST_CASE(uring_d3_c2b_evidence_mode) {
#if defined(SLUICE_HAS_LIBURING)
    UringAsyncBackend backend{UringConfig{4, 4}};
    std::printf("[evidence-meta] evidence=uring_c2b_identity_integration mode=real\n");
    SLUICE_CHECK(backend.available());
#else
    std::printf("[evidence-meta] evidence=uring_c2b_identity_integration mode=stub\n");
#endif
}

// ---------------------------------------------------------------------------
// C2b row 3 — full SlotHandle identity chain on a REAL accepted op:
// Completion -> RequestArena binding -> SlotHandle{slot, full generation,
// context provenance}; kernel routing cookie -> bounded router -> full
// SlotHandle -> RequestArena generation validation.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2b_full_slothandle_identity_chain) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    // Fresh backend: the next allocated cookie is 1 (no-wrap counter starts at
    // 1; high bit reserved for tagged control identity).
    SLUICE_CHECK(backend.peek_next_cookie_for_test() == 1);

    std::byte buf[8]{std::byte{0x41}};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c).has_value());

    // Completion -> arena binding -> full SlotHandle.
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());

    // observe_for_test validates slot + FULL generation + context provenance
    // against the arena and returns the authoritative handle: the observed
    // identity must be exactly the handle the binding carries.
    auto obs = backend.observe_for_test(*h);
    SLUICE_CHECK(obs.has_value());
    SLUICE_CHECK(obs->handle.slot.value == h->slot.value);
    SLUICE_CHECK(obs->handle.generation.value == h->generation.value);
    // The request was dispatched inside submit (SQE installed): running.
    SLUICE_CHECK(obs->state == detail::RequestState::running);

    // Kernel routing chain: op_cookie -> bounded router -> full SlotHandle ->
    // arena generation validation. Inject the PREDICTED cookie (1) through the
    // same handle_one_cqe path a real CQE takes: it must resolve through the
    // live router entry, reach record_terminal through the arena's full
    // generation validation, and reap publishes exactly once.
    SLUICE_CHECK(backend.live_cookies_for_test() == 1);
    backend.inject_cqe_for_test(1, 8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    const auto res = c.result();
    SLUICE_CHECK(res.has_value() && res.value() == 8);
    c.reset();
    // The real write CQE (flushed by poll) arrives later and is dropped as a
    // stale cookie; nothing double-publishes.
    SLUICE_CHECK(backend.poll() == 0);
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2b row 4a — generation reuse: A(S,N) -> terminal -> reap -> Completion
// reset -> slot release -> B(S,N+1). A.generation + 1 == B.generation, via the
// authoritative RequestArena observation.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2b_generation_reuse_plus_one) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    std::byte buf[8]{std::byte{0x42}};
    Completion<std::size_t> ca;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, ca).has_value());
    auto hA = backend.handle_for_completion_for_test(&ca);
    SLUICE_CHECK(hA.has_value());
    const auto slot = hA->slot;
    const auto gen_a = hA->generation;

    // A terminal + reap + caller release (Completion::reset -> slot release).
    backend.inject_cqe_for_test(1, 8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(ca.ready());
    ca.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);

    // B reuses the same physical slot with generation + 1 (the arena free-list
    // is LIFO; the released slot is the next reserve).
    Completion<std::size_t> cb;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, cb).has_value());
    auto hB = backend.handle_for_completion_for_test(&cb);
    SLUICE_CHECK(hB.has_value());
    SLUICE_CHECK(hB->slot.value == slot.value);
    SLUICE_CHECK(hB->generation.value == gen_a.value + 1);

    // B reaches its own terminal.
    backend.inject_cqe_for_test(2, 8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(cb.ready());
    const auto resb = cb.result();
    SLUICE_CHECK(resb.has_value() && resb.value() == 8);
    cb.reset();
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2b row 4a + P0-B — stale operation CQE / cookie after slot reuse: A retired,
// slot reused by B; a late/stale A cookie must be DROPPED at the router, B
// untouched (state, generation, Completion, terminal, borrow), B reaches its
// own terminal.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2b_stale_cookie_cqe_dropped) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    std::byte buf[8]{std::byte{0x43}};
    Completion<std::size_t> ca;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, ca).has_value());
    auto hA = backend.handle_for_completion_for_test(&ca);
    SLUICE_CHECK(hA.has_value());
    const auto slot = hA->slot;
    const auto gen_a = hA->generation;

    backend.inject_cqe_for_test(1, 8); // A terminal (cookie 1)
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(ca.ready());
    ca.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);

    // B occupies the same physical slot at generation N+1 with cookie 2.
    Completion<std::size_t> cb;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, cb).has_value());
    auto hB = backend.handle_for_completion_for_test(&cb);
    SLUICE_CHECK(hB.has_value());
    SLUICE_CHECK(hB->slot.value == slot.value);
    SLUICE_CHECK(hB->generation.value == gen_a.value + 1);

    // Late/stale A cookie: no live router entry carries cookie 1 (A's entry
    // was retired; the array slot was recycled by B but the COOKIE VALUE is
    // never reused). The CQE is dropped — it must not resolve to B.
    const auto before = backend.observe_for_test(*hB);
    SLUICE_CHECK(before.has_value());
    backend.inject_cqe_for_test(1, /*res=*/999); // would-be stale terminal
    SLUICE_CHECK(backend.poll() == 0);           // dropped: nothing published
    const auto after = backend.observe_for_test(*hB);
    SLUICE_CHECK(after.has_value());
    SLUICE_CHECK(after->state == before->state);              // B state unchanged
    SLUICE_CHECK(after->handle.generation.value == before->handle.generation.value);
    SLUICE_CHECK(after->terminal_stored == false);            // B terminal not overwritten
    SLUICE_CHECK(!cb.ready());                                // B Completion not spuriously ready
    SLUICE_CHECK(backend.live_cookies_for_test() == 1);       // B's own entry still live

    // B reaches its own terminal with its own result.
    backend.inject_cqe_for_test(2, 8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(cb.ready());
    const auto resb = cb.result();
    SLUICE_CHECK(resb.has_value() && resb.value() == 8);
    cb.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2b row 4a — stale full SlotHandle cancel: A(S,N) released, B(S,N+1) live;
// cancel_handle_for_test(A) must resolve not_found with ZERO side effect on B.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2b_stale_slothandle_cancel_harmless) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    std::byte buf[8]{std::byte{0x44}};
    Completion<std::size_t> ca;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, ca).has_value());
    auto hA = backend.handle_for_completion_for_test(&ca);
    SLUICE_CHECK(hA.has_value());
    const auto gen_a = hA->generation;

    backend.inject_cqe_for_test(1, 8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(ca.ready());
    ca.reset();

    Completion<std::size_t> cb;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, cb).has_value());
    auto hB = backend.handle_for_completion_for_test(&cb);
    SLUICE_CHECK(hB.has_value());
    SLUICE_CHECK(hB->generation.value == gen_a.value + 1);

    // Drive the CAPTURED generation-N handle through the REAL cancel core
    // (dispatch remove_exact + arena.cancel). The arena rejects the stale
    // generation: not_found, no dispatch removal, no tally, no terminal.
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);
    const auto disp = backend.cancel_handle_for_test(*hA);
    SLUICE_CHECK(disp == detail::CancelDisposition::not_found);
    SLUICE_CHECK(stats.canceled_ops == 0);
    const auto after = backend.observe_for_test(*hB);
    SLUICE_CHECK(after.has_value());
    SLUICE_CHECK(after->handle.generation.value == hB->generation.value);
    SLUICE_CHECK(after->terminal_stored == false);
    SLUICE_CHECK(!cb.ready());
    SLUICE_CHECK(backend.live_cookies_for_test() == 1); // B's dispatch linkage intact

    backend.inject_cqe_for_test(2, 8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(cb.ready());
    cb.reset();
}

// ---------------------------------------------------------------------------
// C2b row 5 — pending cancellation (Scheme B). Deterministic window: commit/
// accept complete, Completion outstanding, slot `pending`, BEFORE enqueue.
// cancel() wins: terminal = canceled, no SQE, no router cookie, no transport
// ledger entry, no syscall; after resume enqueue() observes terminal_noop and
// creates no execution linkage; reap publishes once.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2b_pending_cancel_wins_no_sqe) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    UringAsyncBackend::AfterCommitBeforeEnqueuePauseGate gate;
    backend.set_after_commit_before_enqueue_pause_gate(&gate);

    std::byte buf[8]{std::byte{0x46}};
    Completion<std::size_t> c;
    std::thread submitter([&] {
        (void)backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c);
    });
    SLUICE_CHECK(wait_until([&] { return gate.paused.load(std::memory_order_acquire); }));

    // Exact pending window: accepted (outstanding), not yet enqueued, no SQE.
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());
    auto obs = backend.observe_for_test(*h);
    SLUICE_CHECK(obs.has_value());
    SLUICE_CHECK(obs->state == detail::RequestState::pending);
    SLUICE_CHECK(c.outstanding());
    SLUICE_CHECK(backend.dispatch_size_for_test() == 0);
    SLUICE_CHECK(backend.live_cookies_for_test() == 0);
    SLUICE_CHECK(backend.transport_ledger_size_for_test() == 0);
    SLUICE_CHECK(backend.submit_flushes_for_test() == 0);

    // Cancel wins while pending (Scheme B): terminal stored, no execution
    // linkage, Completion NOT ready merely because cancel() was called.
    const auto disp = backend.cancel_handle_for_test(*h);
    SLUICE_CHECK(disp == detail::CancelDisposition::terminal_won);
    auto obs2 = backend.observe_for_test(*h);
    SLUICE_CHECK(obs2.has_value());
    SLUICE_CHECK(obs2->state == detail::RequestState::backend_ready);
    SLUICE_CHECK(obs2->terminal_stored);
    SLUICE_CHECK(stats.canceled_ops == 1);
    SLUICE_CHECK(!c.ready());

    // Resume: enqueue() observes terminal_noop; no SQE / cookie / ledger / syscall.
    gate.resume.store(true, std::memory_order_release);
    submitter.join();
    SLUICE_CHECK(backend.live_cookies_for_test() == 0);
    SLUICE_CHECK(backend.transport_ledger_size_for_test() == 0);
    SLUICE_CHECK(backend.submit_flushes_for_test() == 0);
    SLUICE_CHECK(backend.dispatch_size_for_test() == 0);

    // Reap publishes the canceled terminal exactly once.
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    auto res = c.result();
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == IoError::Code::canceled);
    c.reset();
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2b row 6 — enqueued cancellation (Scheme B). Deterministic window: request
// in the local dispatch ring (enqueued), no SQE installed; the pause gate
// releases dispatch_mtx_ while paused (ThreadPool Gate-B discipline) so
// cancel() can run remove_exact FIRST, then arena.cancel terminal_won.
// No future SQE, no router entry, no CQE, canceled terminal once.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2b_enqueued_cancel_wins_no_sqe) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    UringAsyncBackend::BeforeDispatchTransferPauseGate gate;
    backend.set_before_dispatch_transfer_pause_gate(&gate);

    std::byte buf[8]{std::byte{0x47}};
    Completion<std::size_t> c;
    std::thread submitter([&] {
        (void)backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c);
    });
    SLUICE_CHECK(wait_until([&] { return gate.paused.load(std::memory_order_acquire); }));
    SLUICE_CHECK(gate.dispatch_domain_released.load(std::memory_order_acquire));

    // Exact enqueued window: on the dispatch ring, no SQE / cookie / ledger.
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());
    auto obs = backend.observe_for_test(*h);
    SLUICE_CHECK(obs.has_value());
    SLUICE_CHECK(obs->state == detail::RequestState::enqueued);
    SLUICE_CHECK(backend.dispatch_size_for_test() == 1);
    SLUICE_CHECK(backend.live_cookies_for_test() == 0);
    SLUICE_CHECK(backend.transport_ledger_size_for_test() == 0);
    SLUICE_CHECK(backend.submit_flushes_for_test() == 0);

    // Cancel wins: remove_exact FIRST (the request leaves the dispatch ring),
    // then the arena stores the canceled terminal. No SQE was ever installed.
    const auto disp = backend.cancel_handle_for_test(*h);
    SLUICE_CHECK(disp == detail::CancelDisposition::terminal_won);
    auto obs2 = backend.observe_for_test(*h);
    SLUICE_CHECK(obs2.has_value());
    SLUICE_CHECK(obs2->state == detail::RequestState::backend_ready);
    SLUICE_CHECK(obs2->terminal_stored);
    SLUICE_CHECK(stats.canceled_ops == 1);
    SLUICE_CHECK(!c.ready());
    SLUICE_CHECK(backend.dispatch_size_for_test() == 0);

    // Resume: the drain re-reads the queue (empty) — nothing is dispatched.
    gate.resume.store(true, std::memory_order_release);
    submitter.join();
    SLUICE_CHECK(backend.live_cookies_for_test() == 0);
    SLUICE_CHECK(backend.transport_ledger_size_for_test() == 0);
    SLUICE_CHECK(backend.submit_flushes_for_test() == 0);

    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    auto res = c.result();
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == IoError::Code::canceled);
    c.reset();
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2b row 7 — running cancellation: intent only. A REAL kernel-blocked pipe
// read is running; cancel() must NOT terminalize, NOT release the slot, NOT
// make the Completion ready. The terminal comes only from the original
// operation CQE (0 bytes after the pipe's write end closes) — never from the
// cancel intent or the control CQE.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2b_running_cancel_intent_real_result) {
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
    // Flush the SQE so the kernel actually blocks the read on the empty pipe.
    SLUICE_CHECK(backend.poll() == 0);

    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());
    auto obs = backend.observe_for_test(*h);
    SLUICE_CHECK(obs.has_value());
    SLUICE_CHECK(obs->state == detail::RequestState::running);
    SLUICE_CHECK(backend.live_cookies_for_test() == 1);

    // cancel() on a running request: intent only.
    const auto disp = backend.cancel_handle_for_test(*h);
    SLUICE_CHECK(disp == detail::CancelDisposition::intent_recorded);
    auto obs2 = backend.observe_for_test(*h);
    SLUICE_CHECK(obs2.has_value());
    SLUICE_CHECK(obs2->state == detail::RequestState::running); // slot remains bound
    SLUICE_CHECK(obs2->terminal_stored == false);               // no terminal from intent
    SLUICE_CHECK(stats.canceled_ops == 0);                      // no canceled tally
    SLUICE_CHECK(!c.ready());                                   // no publication from cancel()
    // At most one bounded control execution reference (the per-slot bit).
    SLUICE_CHECK(backend.live_control_entries_for_test() == 1);

    // The original operation CQE decides: closing the write end completes the
    // blocked read with 0 bytes (EOF) — the result is VERBATIM, not rewritten
    // to canceled by the cancel intent.
    pipe.close_write();
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    auto res = c.result();
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
    SLUICE_CHECK(stats.canceled_ops == 0);
    SLUICE_CHECK(backend.live_control_entries_for_test() == 0); // control retired
    SLUICE_CHECK(backend.live_control_sqes_for_test() == 0);
    c.reset();
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2b §7 Order B — original operation CQE FIRST, cancel-control CQE second.
// The read completes (0 bytes) before the appended cancel SQE is flushed, so
// the kernel retires the cancel with -ENOENT. The original terminal is stored
// in bounded router scratch while the control reference is live, published
// only after the control retires. Exactly one arena terminal, one reap
// publication, router entry retired, control state zero.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2b_original_cqe_before_control_cqe) {
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
    SLUICE_CHECK(backend.cancel_handle_for_test(*h) ==
                  detail::CancelDisposition::intent_recorded);
    SLUICE_CHECK(backend.live_control_entries_for_test() == 1); // prepared, not flushed

    // Original completes BEFORE the cancel SQE is flushed: closing the pipe
    // delivers the original CQE (0 bytes) while the control is still live.
    pipe.close_write();
    SLUICE_CHECK(backend.poll() == 1); // flush + reap: original deferred, control retires it
    SLUICE_CHECK(c.ready());
    auto res = c.result();
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0); // original result verbatim — control never chose it
    SLUICE_CHECK(stats.canceled_ops == 0);
    SLUICE_CHECK(backend.live_cookies_for_test() == 0);        // router entry retired
    SLUICE_CHECK(backend.live_control_entries_for_test() == 0);
    SLUICE_CHECK(backend.live_control_sqes_for_test() == 0);
    c.reset();
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2b §7 Order A — cancel-control CQE FIRST, original operation CQE second.
// The cancel SQE is flushed while the read is kernel-blocked: on this kernel
// the cancel is EFFECTIVE — the kernel interrupts the blocked read, so the
// original operation CQE arrives with -ECANCELED (a legal original-CQE
// terminal, ADR Decision 12) and the control CQE is control-informational
// only. Invariants asserted: the terminal is produced by the ORIGINAL CQE
// (effective cancellation), never chosen or overwritten by the control CQE;
// exactly one arena terminal, one reap publication, control state zero.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2b_control_cqe_before_original_cqe) {
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
    SLUICE_CHECK(backend.cancel_handle_for_test(*h) ==
                  detail::CancelDisposition::intent_recorded);

    // Flush the cancel SQE while the read is still blocked: the control CQE
    // retires with a control-only result and the original operation CQE
    // reports the effective cancellation (-ECANCELED). The control CQE never
    // chooses or overwrites the terminal; the slot-bound publication happens
    // exactly once through reap.
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    auto res = c.result();
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == IoError::Code::canceled);
    SLUICE_CHECK(stats.canceled_ops == 1); // effective cancellation tallied once
    SLUICE_CHECK(backend.live_control_entries_for_test() == 0);
    SLUICE_CHECK(backend.live_control_sqes_for_test() == 0);
    SLUICE_CHECK(backend.live_cookies_for_test() == 0); // router entry retired
    c.reset();
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2b row 8 — identity-bearing reap / publication boundary: a backend_ready
// slot is NOT ready until reap publishes; poll() publishes exactly once
// through the slot binding. Also proves a late real CQE for a retired cookie
// is dropped (no double publication).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2b_publication_boundary_reap_gates_ready) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    std::byte buf[8]{std::byte{0x45}};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c).has_value());
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());

    // Record the terminal WITHOUT reaping (inject through the routing layer):
    // the slot is backend_ready but the Completion is NOT ready — only reap
    // publishes (ADR Decision 9 / I11).
    backend.inject_cqe_for_test(1, 8);
    auto obs = backend.observe_for_test(*h);
    SLUICE_CHECK(obs.has_value());
    SLUICE_CHECK(obs->state == detail::RequestState::backend_ready);
    SLUICE_CHECK(obs->terminal_stored);
    SLUICE_CHECK(!c.ready());
    SLUICE_CHECK(backend.backend_ready_count_for_test() == 1);
    SLUICE_CHECK(backend.sink_deliveries() == 0);

    // Reap publishes exactly once through the slot binding.
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    const auto res = c.result();
    SLUICE_CHECK(res.has_value() && res.value() == 8);
    SLUICE_CHECK(backend.backend_ready_count_for_test() == 0);
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    c.reset();

    // The real write CQE (the SQE was flushed by the reap above) arrives
    // later: its cookie was retired, so it is dropped — exactly-once stands.
    (void)backend.poll();
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

#else // !SLUICE_HAS_LIBURING — stub mode: build/API honesty only.

SLUICE_TEST_CASE(uring_c2b_full_slothandle_identity_chain) {}
SLUICE_TEST_CASE(uring_c2b_generation_reuse_plus_one) {}
SLUICE_TEST_CASE(uring_c2b_stale_cookie_cqe_dropped) {}
SLUICE_TEST_CASE(uring_c2b_stale_slothandle_cancel_harmless) {}
SLUICE_TEST_CASE(uring_c2b_pending_cancel_wins_no_sqe) {}
SLUICE_TEST_CASE(uring_c2b_enqueued_cancel_wins_no_sqe) {}
SLUICE_TEST_CASE(uring_c2b_running_cancel_intent_real_result) {}
SLUICE_TEST_CASE(uring_c2b_original_cqe_before_control_cqe) {}
SLUICE_TEST_CASE(uring_c2b_control_cqe_before_original_cqe) {}
SLUICE_TEST_CASE(uring_c2b_publication_boundary_reap_gates_ready) {}

#endif // SLUICE_HAS_LIBURING

SLUICE_MAIN()
