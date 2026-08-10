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

// ---------------------------------------------------------------------------
// Evidence-meta (G2): exactly one [evidence-meta] line per gate-driven run.
//
// This case MUST be registered in BOTH build modes. It sits OUTSIDE the outer
// `#if defined(SLUICE_HAS_LIBURING)` guard so a stub build also emits its
// mode=stub line: every gate-driven target run emits exactly one evidence
// metadata line (G2 protocol). The stub line is NOT a PASS — it lets the
// aggregate gate attribute the run to mode=stub and classify it INCOMPLETE via
// required_modes=("real",), instead of an accidental INCOMPLETE from a missing
// case. That distinction is load-bearing: the classification reason must be
// "disallowed mode", never "required case disappeared".
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
// own terminal through a REAL kernel CQE.
//
// Deterministic construction (Issue #85): B is a REAL kernel-blocked pipe read
// (empty pipe, write end open), NOT a fast tmpfs write. A blocked pipe read
// cannot legitimately terminalize before its write end closes, so the detector
// never depends on unrelated real-kernel completion timing (a fast B write
// legitimately completing between submit and poll() must not break the
// proof). The stale-cookie theorem is B's authoritative state BEFORE and
// AFTER the injection — never a poll()==0 count that a legitimate B CQE could
// break.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2b_stale_cookie_cqe_dropped) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());
    PipePair pipe;
    SLUICE_CHECK(pipe.valid());

    // A: ordinary real operation used to obtain Slot(S,N) / cookie A. The
    // cookie comes from the authoritative router counter, not a magic constant.
    const auto cookie_a = backend.peek_next_cookie_for_test();
    std::byte buf_a[8]{std::byte{0x43}};
    Completion<std::size_t> ca;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf_a, 8, 0}, ca).has_value());
    auto hA = backend.handle_for_completion_for_test(&ca);
    SLUICE_CHECK(hA.has_value());
    const auto slot = hA->slot;
    const auto gen_a = hA->generation;

    // Synthetic A terminal + reap + caller release: slot S released (generation
    // incremented), A's router cookie retired.
    backend.inject_cqe_for_test(cookie_a, 8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(ca.ready());
    SLUICE_CHECK(backend.sink_deliveries() == 1); // A published exactly once
    ca.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);

    // B: REAL pipe read on the same physical slot S at generation N+1 with a
    // NEW cookie (cookie values are never recycled within the backend
    // lifetime). The pipe write end stays OPEN and empty: B is
    // kernel-owned/running and CANNOT legitimately terminalize until the
    // write end closes.
    const auto cookie_b = backend.peek_next_cookie_for_test();
    SLUICE_CHECK(cookie_b != cookie_a); // distinct, non-recycled cookie
    std::byte buf_b[4]{};
    Completion<std::size_t> cb;
    SLUICE_CHECK(backend.submit_read(ReadOp{pipe.read_fd(), buf_b, 4, 0}, cb).has_value());
    auto hB = backend.handle_for_completion_for_test(&cb);
    SLUICE_CHECK(hB.has_value());
    SLUICE_CHECK(hB->slot.value == slot.value);
    SLUICE_CHECK(hB->generation.value == gen_a.value + 1);
    // Flush the SQE so the kernel actually blocks the read. poll() returns 0
    // deterministically: B cannot complete, and A's own real late CQE (if it
    // already arrived) is dropped as a stale cookie and publishes nothing.
    SLUICE_CHECK(backend.poll() == 0);

    // B is genuinely live: running/ring-owned, no terminal, borrow active with
    // the pipe read metadata, Completion not ready, own router cookie live.
    const auto before = backend.observe_for_test(*hB);
    SLUICE_CHECK(before.has_value());
    SLUICE_CHECK(before->state == detail::RequestState::running);
    SLUICE_CHECK(before->terminal_stored == false);
    const auto before_borrow = backend.borrow_for_test(*hB);
    SLUICE_CHECK(before_borrow.has_value());
    SLUICE_CHECK(before_borrow->active);
    SLUICE_CHECK(before_borrow->fd == pipe.read_fd());
    SLUICE_CHECK(before_borrow->address == static_cast<const void*>(buf_b));
    SLUICE_CHECK(before_borrow->length == 4);
    SLUICE_CHECK(!cb.ready());
    SLUICE_CHECK(backend.live_cookies_for_test() == 1); // B's own entry live

    // Late/stale A cookie: no live router entry carries cookie_a (A's entry
    // was retired; the router array slot was recycled by B but the COOKIE
    // VALUE is never reused). handle_one_cqe drops it — it must not resolve
    // to B. NO poll() follows the injection: poll() advances the REAL ring
    // and unrelated kernel CQEs must not serve as the proof. The proof is B's
    // authoritative state, read again immediately.
    backend.inject_cqe_for_test(cookie_a, /*res=*/999); // would-be stale terminal
    const auto after = backend.observe_for_test(*hB);
    SLUICE_CHECK(after.has_value());
    SLUICE_CHECK(after->state == before->state); // B state unchanged
    SLUICE_CHECK(after->handle.generation.value == before->handle.generation.value);
    SLUICE_CHECK(after->terminal_stored == false); // B terminal not overwritten
    const auto after_borrow = backend.borrow_for_test(*hB);
    SLUICE_CHECK(after_borrow.has_value());
    SLUICE_CHECK(after_borrow->active == before_borrow->active);
    SLUICE_CHECK(after_borrow->fd == before_borrow->fd);
    SLUICE_CHECK(after_borrow->address == before_borrow->address);
    SLUICE_CHECK(after_borrow->length == before_borrow->length);
    SLUICE_CHECK(!cb.ready()); // B Completion not spuriously ready
    SLUICE_CHECK(backend.live_cookies_for_test() == 1); // B's entry still live

    // B completes through its OWN real kernel CQE: closing the write end gives
    // the blocked read a deterministic 0-byte EOF terminal. wait_one() flushes,
    // reaps, and blocks in the kernel until the real CQE arrives — no
    // synthetic terminal is injected for B.
    pipe.close_write();
    auto waited = backend.wait_one();
    SLUICE_CHECK(waited.has_value());
    SLUICE_CHECK(waited.value() == 1); // exactly one publication: B's real terminal
    SLUICE_CHECK(cb.ready());
    const auto resb = cb.result();
    SLUICE_CHECK(resb.has_value());
    SLUICE_CHECK(resb.value() == 0); // 0-byte EOF — NOT the stale 999, not canceled
    SLUICE_CHECK(backend.sink_deliveries() == 2); // A's 1 + B's exactly-once
    SLUICE_CHECK(backend.live_cookies_for_test() == 0); // B router entry retired
    // Reap published but did NOT release the slot: it stays bound until the
    // caller resets the ready Completion.
    SLUICE_CHECK(backend.arena_slot_in_use() == 1);
    cb.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    SLUICE_CHECK(backend.outstanding() == 0);
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
// The read completes (0 bytes) before the appended cancel SQE is resolved, so
// the original terminal is stored in bounded router scratch while the control
// reference is live, published only after the control retires. The control
// result may reflect not-found / already-completed / raced cancellation
// according to kernel timing (per the io_uring LOTI: 0 / -ENOENT / -EALREADY
// are all possible); it is informational and cannot author the request
// terminal. The detector asserts only the invariant it owns: the original
// result is verbatim, exactly one arena terminal, one reap publication,
// router entry retired, control state zero.
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
// C2b §7 — portable cancel-control-authority detector (kernel-portable).
//
// A REAL kernel-blocked pipe read is submitted; the request reaches `running`.
// A cancel is then requested through the REAL production cancel path. Whether
// IORING_OP_ASYNC_CANCEL effectively interrupts a kernel-blocked pipe read is
// kernel-version/timing-dependent (per the io_uring LOTI: socket-style I/O is
// interruptible, disk I/O is not; a blocked pipe read is in between). This
// detector does NOT assume effectiveness. It drives the transport with
// deterministic state observations and a bounded hang-watchdog (NEVER sleep as
// ordering proof) and branches on the REAL kernel outcome:
//
//   Path A — effective cancellation: the ORIGINAL operation CQE arrives with
//     -ECANCELED (a legal original-CQE terminal, ADR Decision 12). The
//     cancel-control CQE is informational only. result == canceled; control
//     state retired; router entry retired; exactly one publication.
//
//   Path B — ineffective / raced cancellation: the control attempt retires
//     (control result one of 0 / -ENOENT / -EALREADY per the io_uring LOTI —
//     informational, not asserted) while the original operation is still
//     kernel-owned. Completion NOT ready; original router/cookie still live;
//     control state retired; the control CQE did NOT choose a terminal; the
//     canceled tally remains zero. Closing the pipe's write end then delivers
//     a deterministic REAL original-operation EOF CQE (0 bytes): result is
//     verbatim success — no cancel rewrite — exactly one publication.
//
// In BOTH paths the load-bearing invariant is the SAME: the ORIGINAL operation
// CQE owns the request terminal; the cancel-control CQE is informational and
// can never independently author or overwrite the terminal; exactly one reap
// publication; the slot is released only after the caller resets the ready
// Completion. This is NOT a tautology — the control-authority mutant (D3-M6:
// the control CQE fabricates the terminal) is RED against this detector.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2b_cancel_control_never_authors_terminal) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    PipePair pipe;
    SLUICE_CHECK(pipe.valid());
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    // 1. Submit a REAL kernel-blocked pipe read.
    std::byte buf[4]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_read(ReadOp{pipe.read_fd(), buf, 4, 0}, c).has_value());
    SLUICE_CHECK(backend.poll() == 0); // flush: kernel blocks the read

    // 2. Confirm RequestState == running.
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());
    {
        auto obs = backend.observe_for_test(*h);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(obs->state == detail::RequestState::running);
    }
    SLUICE_CHECK(backend.live_cookies_for_test() == 1); // original router entry live

    // 3. Request cancel through the REAL production cancel path.
    SLUICE_CHECK(backend.cancel_handle_for_test(*h) ==
                  detail::CancelDisposition::intent_recorded);

    // 4. Confirm: disposition was intent-only; the Completion is NOT ready from
    //    the cancel intent alone; exactly one bounded control execution
    //    reference (the per-slot bit) and one prepared (not yet flushed)
    //    control SQE.
    SLUICE_CHECK(!c.ready());
    SLUICE_CHECK(stats.canceled_ops == 0);
    SLUICE_CHECK(backend.live_control_entries_for_test() == 1);
    // The control SQE is PREPARED but NOT yet flushed by transport: there is
    // no live submitted control SQE yet (it is counted once submit consumes
    // it; a CQE retires it back to zero). This mirrors the running-cancel
    // detector: intent sets the per-slot control bit; a transport flush
    // submits it; its control CQE retires the bit.
    SLUICE_CHECK(backend.live_control_sqes_for_test() == 0);

    // 5. Drive the transport until the cancel-control attempt itself has
    //    retired (control bit clears) OR the original request already
    //    terminalized — a persistent-state predicate (the per-slot control
    //    entry), not a sleep-based ordering proof. The bounded deadline is a
    //    hang-watchdog only.
    SLUICE_CHECK(wait_until([&] {
        (void)backend.poll(); // flush transport + reap any kernel-ready CQEs
        return c.ready() || backend.live_control_entries_for_test() == 0;
    }));
    SLUICE_CHECK(backend.live_control_entries_for_test() == 0); // control retired

    // 6. Branch on the REAL kernel outcome.
    if (c.ready()) {
        // Path A — effective cancellation: the ORIGINAL operation CQE reported
        // -ECANCELED (a legal original-CQE terminal). The control CQE never
        // chose or overwrote it; reap published exactly once.
        auto res = c.result();
        SLUICE_CHECK(!res.has_value());
        SLUICE_CHECK(res.error().code == IoError::Code::canceled);
        SLUICE_CHECK(stats.canceled_ops == 1); // effective cancellation tallied once
        SLUICE_CHECK(backend.live_control_entries_for_test() == 0);
        SLUICE_CHECK(backend.live_control_sqes_for_test() == 0);
        SLUICE_CHECK(backend.live_cookies_for_test() == 0); // router entry retired
    } else {
        // Path B — ineffective / raced cancellation: the control attempt
        // retired but the original operation is still kernel-owned. The
        // control CQE did NOT choose a terminal.
        SLUICE_CHECK(stats.canceled_ops == 0);                // no canceled tally
        SLUICE_CHECK(backend.live_control_entries_for_test() == 0); // control retired
        SLUICE_CHECK(backend.live_control_sqes_for_test() == 0);
        SLUICE_CHECK(backend.live_cookies_for_test() == 1);   // original cookie still live
        {
            auto obs = backend.observe_for_test(*h);
            SLUICE_CHECK(obs.has_value());
            SLUICE_CHECK(obs->state == detail::RequestState::running); // still running
            SLUICE_CHECK(obs->terminal_stored == false);               // no terminal
        }
        SLUICE_CHECK(!c.ready()); // control CQE published nothing

        // Close the pipe's write end so the original read receives a
        // deterministic REAL EOF CQE (0 bytes).
        pipe.close_write();
        SLUICE_CHECK(wait_until([&] { return c.ready(); }));

        // The original operation CQE owns the terminal: verbatim success, no
        // cancel rewrite. Exactly one publication; router finally retires.
        SLUICE_CHECK(c.ready());
        auto res = c.result();
        SLUICE_CHECK(res.has_value());
        SLUICE_CHECK(res.value() == 0); // original result verbatim
        SLUICE_CHECK(stats.canceled_ops == 0); // still zero — no cancel rewrite
        SLUICE_CHECK(backend.live_control_entries_for_test() == 0);
        SLUICE_CHECK(backend.live_control_sqes_for_test() == 0);
        SLUICE_CHECK(backend.live_cookies_for_test() == 0); // router entry retired
    }

    // Both paths: slot released only after the caller resets the ready
    // Completion; exactly one publication throughout.
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
SLUICE_TEST_CASE(uring_c2b_cancel_control_never_authors_terminal) {}
SLUICE_TEST_CASE(uring_c2b_publication_boundary_reap_gates_ready) {}

#endif // SLUICE_HAS_LIBURING

SLUICE_MAIN()
