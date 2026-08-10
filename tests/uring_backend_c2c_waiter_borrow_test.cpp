// Phase D3 — Uring C2c borrow / waiter / lease integration evidence.
//
// Real mode exercises the authoritative production uring_backend.cpp with only
// read-only SLUICE_ASYNC_INTERNAL_TESTING observations and the two
// deterministic pause gates. Every waiter/borrow seam forwards verbatim to the
// REAL RequestArena authorities (register_waiter / cancel_waiter /
// borrow_for_test / waiter_for_test) and the REAL ReferenceReadySink — there
// is no Uring-specific waiter storage, no side-band waiter map, and no
// reimplemented state machine (C2c rows 11-14a).
//
// Stub mode proves build and API honesty only; the manifest requires evidence
// mode=real before this target can satisfy the uring_c2c_borrow_waiter_
// integration record (G2: the pinned case set must run exactly once each).
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
using detail::WaiterRegistration;
using detail::WaiterToken;

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
SLUICE_TEST_CASE(uring_d3_c2c_evidence_mode) {
#if defined(SLUICE_HAS_LIBURING)
    UringAsyncBackend backend{UringConfig{4, 4}};
    std::printf("[evidence-meta] evidence=uring_c2c_borrow_waiter_integration mode=real\n");
    SLUICE_CHECK(backend.available());
#else
    std::printf("[evidence-meta] evidence=uring_c2c_borrow_waiter_integration mode=stub\n");
#endif
}

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
            std::fprintf(stderr, "uring_c2c: pause gate exit timeout\n");
            std::fflush(stderr);
            std::terminate();
        }
        done = true;
    }
};

class TempFile {
  public:
    TempFile() {
        char path[] = "/tmp/sluice_uring_d3_c2c_XXXXXX";
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
// C2c row 11 — borrow lifetime: active at `pending` (commit began the borrow)
// with the EXACT submitted fd/address/length; still active through enqueued,
// running, and backend_ready-before-reap; ends ONLY at reap.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2c_borrow_active_through_lifecycle) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    std::byte buf[8]{std::byte{0x51}};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c).has_value());

    // Running (dispatched inside submit): borrow active with exact metadata.
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());
    auto running_borrow = backend.borrow_for_test(*h);
    SLUICE_CHECK(running_borrow.has_value());
    SLUICE_CHECK(running_borrow->active);
    SLUICE_CHECK(running_borrow->fd == file.fd());
    SLUICE_CHECK(running_borrow->address == static_cast<const void*>(buf));
    SLUICE_CHECK(running_borrow->length == 8);

    // backend_ready-before-reap (inject the terminal through the routing
    // layer): the borrow is STILL active — the CQE / record_terminal is NOT
    // the borrow lifetime end (ADR Decision 8; I7/I18).
    backend.inject_cqe_for_test(1, 8);
    auto ready_borrow = backend.borrow_for_test(*h);
    SLUICE_CHECK(ready_borrow.has_value());
    SLUICE_CHECK(ready_borrow->active);
    SLUICE_CHECK(ready_borrow->fd == file.fd());
    SLUICE_CHECK(ready_borrow->address == static_cast<const void*>(buf));
    SLUICE_CHECK(ready_borrow->length == 8);

    // Reap ends the borrow: the ready observer sees the ended borrow (I18
    // publish_seq > borrow_end_seq).
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    auto after_borrow = backend.borrow_for_test(*h);
    SLUICE_CHECK(after_borrow.has_value());
    SLUICE_CHECK(!after_borrow->active);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2c row 11 — borrow at the `pending` window (after commit, before enqueue):
// commit began the borrow BEFORE any execution ownership.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2c_borrow_active_pending_window) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    UringAsyncBackend::AfterCommitBeforeEnqueuePauseGate gate;
    backend.set_after_commit_before_enqueue_pause_gate(&gate);

    std::byte buf[8]{std::byte{0x52}};
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

    auto borrow = backend.borrow_for_test(*h);
    SLUICE_CHECK(borrow.has_value());
    SLUICE_CHECK(borrow->active);
    SLUICE_CHECK(borrow->fd == file.fd());
    SLUICE_CHECK(borrow->address == static_cast<const void*>(buf));
    SLUICE_CHECK(borrow->length == 8);
    resume.release();
    submitter.join();

    backend.inject_cqe_for_test(1, 8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    c.reset();
}

// ---------------------------------------------------------------------------
// C2c row 11 — borrow at the `enqueued` window (dispatch ring, no SQE yet).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2c_borrow_active_enqueued_window) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    UringAsyncBackend::BeforeDispatchTransferPauseGate gate;
    backend.set_before_dispatch_transfer_pause_gate(&gate);

    std::byte buf[8]{std::byte{0x53}};
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

    auto borrow = backend.borrow_for_test(*h);
    SLUICE_CHECK(borrow.has_value());
    SLUICE_CHECK(borrow->active);
    SLUICE_CHECK(borrow->fd == file.fd());
    SLUICE_CHECK(borrow->address == static_cast<const void*>(buf));
    SLUICE_CHECK(borrow->length == 8);
    resume.release();
    submitter.join();

    backend.inject_cqe_for_test(1, 8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    c.reset();
}

// ---------------------------------------------------------------------------
// C2c row 11 — sync (void) ops carry the expected NO-BUFFER borrow shape:
// fd only, null address, zero length.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2c_borrow_sync_no_buffer_shape) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    Completion<void> c;
    SLUICE_CHECK(backend.submit_sync_data(SyncDataOp{file.fd()}, c).has_value());
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());
    auto borrow = backend.borrow_for_test(*h);
    SLUICE_CHECK(borrow.has_value());
    SLUICE_CHECK(borrow->active);
    SLUICE_CHECK(borrow->fd == file.fd());
    SLUICE_CHECK(borrow->address == nullptr);
    SLUICE_CHECK(borrow->length == 0);

    backend.inject_cqe_for_test(1, 0);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2c row 12a — waiter registration while `enqueued` (orthogonal to execution
// state, ADR Decision 10); registration observation shows the exact token and
// lease.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2c_waiter_registration_enqueued) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    UringAsyncBackend::BeforeDispatchTransferPauseGate gate;
    backend.set_before_dispatch_transfer_pause_gate(&gate);

    std::byte buf[8]{std::byte{0x54}};
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

    // Register through the REAL arena authority while enqueued.
    auto reg = backend.register_waiter_for_test(c, WaiterToken{1, 7, 3}, detail::RoutingLease{99});
    SLUICE_CHECK(reg.has_value());
    auto w = backend.waiter_for_test(*h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK((w->token == WaiterToken{1, 7, 3}));
    SLUICE_CHECK(w->lease_id == 99);
    resume.release();
    submitter.join();

    backend.inject_cqe_for_test(1, 8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    SLUICE_CHECK(backend.sink_last_has_waiter());
    SLUICE_CHECK((backend.sink_last_token() == WaiterToken{1, 7, 3}));
    SLUICE_CHECK(backend.sink_last_lease_id() == 99);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2c row 12a — waiter registration while `running` (real kernel-blocked op).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2c_waiter_registration_running) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    PipePair pipe;
    SLUICE_CHECK(pipe.valid());

    std::byte buf[4]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_read(ReadOp{pipe.read_fd(), buf, 4, 0}, c).has_value());
    SLUICE_CHECK(backend.poll() == 0); // kernel blocks the read
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());
    auto obs = backend.observe_for_test(*h);
    SLUICE_CHECK(obs.has_value());
    SLUICE_CHECK(obs->state == detail::RequestState::running);

    auto reg = backend.register_waiter_for_test(c, WaiterToken{5, 4, 2}, detail::RoutingLease{77});
    SLUICE_CHECK(reg.has_value());
    auto w = backend.waiter_for_test(*h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK((w->token == WaiterToken{5, 4, 2}));
    SLUICE_CHECK(w->lease_id == 77);

    pipe.close_write();
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    SLUICE_CHECK(backend.sink_last_has_waiter());
    SLUICE_CHECK((backend.sink_last_token() == WaiterToken{5, 4, 2}));
    SLUICE_CHECK(backend.sink_last_lease_id() == 77);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2c row 12a — waiter registration while `backend_ready` before reap: the
// terminal winner does NOT close registration; reap delivers.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2c_waiter_registration_backend_ready) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    std::byte buf[8]{std::byte{0x55}};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c).has_value());
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());

    // backend_ready (injected), not yet reaped.
    backend.inject_cqe_for_test(1, 8);
    auto obs = backend.observe_for_test(*h);
    SLUICE_CHECK(obs.has_value());
    SLUICE_CHECK(obs->state == detail::RequestState::backend_ready);

    auto reg = backend.register_waiter_for_test(c, WaiterToken{6, 5, 3}, detail::RoutingLease{88});
    SLUICE_CHECK(reg.has_value());
    auto w = backend.waiter_for_test(*h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK((w->token == WaiterToken{6, 5, 3}));
    SLUICE_CHECK(w->lease_id == 88);

    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(backend.sink_last_has_waiter());
    SLUICE_CHECK((backend.sink_last_token() == WaiterToken{6, 5, 3}));
    SLUICE_CHECK(backend.sink_last_lease_id() == 88);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2c row 12a — second waiter registration is rejected by the shared contract
// (single-waiter cardinality); the FIRST registration and its lease survive.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2c_second_waiter_registration_rejected) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    PipePair pipe;
    SLUICE_CHECK(pipe.valid());

    std::byte buf[4]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_read(ReadOp{pipe.read_fd(), buf, 4, 0}, c).has_value());
    SLUICE_CHECK(backend.poll() == 0);
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());

    auto reg = backend.register_waiter_for_test(c, WaiterToken{2, 8, 4}, detail::RoutingLease{111});
    SLUICE_CHECK(reg.has_value());
    auto second = backend.register_waiter_for_test(c, WaiterToken{3, 9, 5},
                                                   detail::RoutingLease{222});
    SLUICE_CHECK(!second.has_value());
    SLUICE_CHECK(second.error().code == IoError::Code::invalid_state);
    // The first registration is untouched (no overwrite).
    auto w = backend.waiter_for_test(*h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK((w->token == WaiterToken{2, 8, 4}));
    SLUICE_CHECK(w->lease_id == 111);

    pipe.close_write();
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK((backend.sink_last_token() == WaiterToken{2, 8, 4}));
    SLUICE_CHECK(backend.sink_last_lease_id() == 111);
    c.reset();
}

// ---------------------------------------------------------------------------
// C2c row 13 — wait-cancel independence: cancel_waiter() removes ONLY the
// waiter and returns/moves the RoutingLease to the caller; the I/O continues
// and its real result is the terminal; the sink delivers NO waiter.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2c_wait_cancel_keeps_io) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    PipePair pipe;
    SLUICE_CHECK(pipe.valid());

    std::byte buf[4]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_read(ReadOp{pipe.read_fd(), buf, 4, 0}, c).has_value());
    SLUICE_CHECK(backend.poll() == 0); // kernel blocks the read
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());

    auto reg = backend.register_waiter_for_test(c, WaiterToken{7, 6, 1}, detail::RoutingLease{55});
    SLUICE_CHECK(reg.has_value());
    // Wait-cancel: the lease moves to the caller; the registration reopens
    // with no waiter; the I/O is untouched.
    auto lease = backend.cancel_waiter_for_test(c);
    SLUICE_CHECK(lease.has_value());
    SLUICE_CHECK(lease.value().id() == 55);
    auto w = backend.waiter_for_test(*h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_no_waiter);
    auto borrow = backend.borrow_for_test(*h);
    SLUICE_CHECK(borrow.has_value());
    SLUICE_CHECK(borrow->active); // borrow unaffected by wait-cancel

    // The I/O still runs and its real result wins.
    pipe.close_write();
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    const auto res = c.result();
    SLUICE_CHECK(res.has_value() && res.value() == 0);
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    SLUICE_CHECK(!backend.sink_last_has_waiter()); // no waiter delivered
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2c row 13 — I/O-cancel independence: cancel(operation) does NOT delete the
// waiter; an enqueued cancel win delivers the canceled terminal AND the
// registered waiter at reap.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2c_io_cancel_keeps_waiter) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    UringAsyncBackend::BeforeDispatchTransferPauseGate gate;
    backend.set_before_dispatch_transfer_pause_gate(&gate);

    std::byte buf[8]{std::byte{0x56}};
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
    auto reg = backend.register_waiter_for_test(c, WaiterToken{4, 3, 8}, detail::RoutingLease{123});
    SLUICE_CHECK(reg.has_value());

    // Enqueued cancel wins: the waiter is KEPT, not deleted.
    const auto disp = backend.cancel_handle_for_test(*h);
    SLUICE_CHECK(disp == detail::CancelDisposition::terminal_won);
    SLUICE_CHECK(stats.canceled_ops == 1);
    auto w = backend.waiter_for_test(*h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK((w->token == WaiterToken{4, 3, 8}));
    SLUICE_CHECK(w->lease_id == 123);

    resume.release();
    submitter.join();

    // Reap delivers the canceled terminal AND the waiter exactly once.
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    const auto res = c.result();
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == IoError::Code::canceled);
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    SLUICE_CHECK(backend.sink_last_has_waiter());
    SLUICE_CHECK((backend.sink_last_token() == WaiterToken{4, 3, 8}));
    SLUICE_CHECK(backend.sink_last_lease_id() == 123);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2c rows 12a/14a — waiter delivery exactly once: the sink receives the
// registered token + lease with the FIRST (and only) publication; a second
// poll adds nothing and the slot's registration is closed.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2c_waiter_delivery_exactly_once) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    std::byte buf[8]{std::byte{0x57}};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c).has_value());
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());
    auto reg = backend.register_waiter_for_test(c, WaiterToken{9, 2, 6}, detail::RoutingLease{200});
    SLUICE_CHECK(reg.has_value());

    backend.inject_cqe_for_test(1, 8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    SLUICE_CHECK(backend.sink_last_has_waiter());
    SLUICE_CHECK((backend.sink_last_token() == WaiterToken{9, 2, 6}));
    SLUICE_CHECK(backend.sink_last_lease_id() == 200);
    // Registration closed by reap; no second delivery.
    auto w = backend.waiter_for_test(*h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::closed);
    SLUICE_CHECK(backend.poll() == 0);
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2c row 14a — stale waiter authority: a captured generation-N handle cannot
// register/cancel a waiter on the live N+1 occupant (not_found, zero side
// effect); the live occupant's registration is untouched.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2c_stale_waiter_authority_harmless) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    // A: full lifecycle (generation N), released.
    std::byte buf[8]{std::byte{0x58}};
    Completion<std::size_t> ca;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, ca).has_value());
    auto hA = backend.handle_for_completion_for_test(&ca);
    SLUICE_CHECK(hA.has_value());
    backend.inject_cqe_for_test(1, 8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(ca.ready());
    ca.reset();

    // B: generation N+1 on the same physical slot.
    Completion<std::size_t> cb;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, cb).has_value());
    auto hB = backend.handle_for_completion_for_test(&cb);
    SLUICE_CHECK(hB.has_value());
    SLUICE_CHECK(hB->generation.value == hA->generation.value + 1);

    // Stale A waiter handle: rejected, zero side effect on B.
    auto stale_register =
        backend.register_waiter_handle_for_test(*hA, WaiterToken{1, 1, 1},
                                                detail::RoutingLease{1});
    SLUICE_CHECK(!stale_register.has_value());
    SLUICE_CHECK(stale_register.error().code == IoError::Code::not_found);
    auto stale_cancel = backend.cancel_waiter_handle_for_test(*hA);
    SLUICE_CHECK(!stale_cancel.has_value());
    SLUICE_CHECK(stale_cancel.error().code == IoError::Code::not_found);
    auto w = backend.waiter_for_test(*hB);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_no_waiter);
    auto borrow = backend.borrow_for_test(*hB);
    SLUICE_CHECK(borrow.has_value());
    SLUICE_CHECK(borrow->active); // B borrow unchanged

    // B's OWN registration works and is delivered.
    auto reg = backend.register_waiter_for_test(cb, WaiterToken{4, 2, 2}, detail::RoutingLease{201});
    SLUICE_CHECK(reg.has_value());
    backend.inject_cqe_for_test(2, 8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(cb.ready());
    SLUICE_CHECK((backend.sink_last_token() == WaiterToken{4, 2, 2}));
    SLUICE_CHECK(backend.sink_last_lease_id() == 201);
    cb.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---------------------------------------------------------------------------
// C2c rows 12a/14a — register-waiter-after-record_terminal-before-reap: the
// terminal winner does not close registration; a waiter registered in the
// backend_ready window is still delivered by reap (shared linearization
// rules, deterministic window).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_c2c_register_waiter_after_record_terminal_before_reap) {
    UringAsyncBackend backend{UringConfig{4, 4}};
    if (!backend.available())
        return;
    TempFile file;
    SLUICE_CHECK(file.valid());

    std::byte buf[8]{std::byte{0x59}};
    Completion<std::size_t> c;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), buf, 8, 0}, c).has_value());
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());

    // record_terminal (backend_ready) WITHOUT reaping.
    backend.inject_cqe_for_test(1, 8);
    auto obs = backend.observe_for_test(*h);
    SLUICE_CHECK(obs.has_value());
    SLUICE_CHECK(obs->state == detail::RequestState::backend_ready);
    SLUICE_CHECK(!c.ready());

    // Register in the exact window: allowed, reap delivers.
    auto reg = backend.register_waiter_for_test(c, WaiterToken{3, 1, 7}, detail::RoutingLease{300});
    SLUICE_CHECK(reg.has_value());
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    SLUICE_CHECK(backend.sink_last_has_waiter());
    SLUICE_CHECK((backend.sink_last_token() == WaiterToken{3, 1, 7}));
    SLUICE_CHECK(backend.sink_last_lease_id() == 300);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

#else // !SLUICE_HAS_LIBURING — stub mode: build/API honesty only.

SLUICE_TEST_CASE(uring_c2c_borrow_active_through_lifecycle) {}
SLUICE_TEST_CASE(uring_c2c_borrow_active_pending_window) {}
SLUICE_TEST_CASE(uring_c2c_borrow_active_enqueued_window) {}
SLUICE_TEST_CASE(uring_c2c_borrow_sync_no_buffer_shape) {}
SLUICE_TEST_CASE(uring_c2c_waiter_registration_enqueued) {}
SLUICE_TEST_CASE(uring_c2c_waiter_registration_running) {}
SLUICE_TEST_CASE(uring_c2c_waiter_registration_backend_ready) {}
SLUICE_TEST_CASE(uring_c2c_second_waiter_registration_rejected) {}
SLUICE_TEST_CASE(uring_c2c_wait_cancel_keeps_io) {}
SLUICE_TEST_CASE(uring_c2c_io_cancel_keeps_waiter) {}
SLUICE_TEST_CASE(uring_c2c_waiter_delivery_exactly_once) {}
SLUICE_TEST_CASE(uring_c2c_stale_waiter_authority_harmless) {}
SLUICE_TEST_CASE(uring_c2c_register_waiter_after_record_terminal_before_reap) {}

#endif // SLUICE_HAS_LIBURING

SLUICE_MAIN()