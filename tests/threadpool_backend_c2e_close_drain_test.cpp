// Phase C2e ThreadPoolBackend close/drain/destruction deterministic tests
// (Issue #68 rows 15-16; ADR Decision 15).
//
// These cases close the C2e evidence gaps on the REAL blocking backend that the
// shared observable suite cannot reach deterministically. Each case drives a
// real accepted request through a SLUICE_ASYNC_INTERNAL_TESTING pause gate to
// pin the exact window, calls close_admission() inside that window, and proves
// the ADR Decision-15 contract for that state:
//
//   C1  close while the submit path is paused between commit and enqueue (slot
//       `pending`): close rejects nothing retroactively, the resumed enqueue +
//       dispatch runs the real syscall, and the request completes with its REAL
//       result verbatim — close never rewrites an accepted request.
//   C2  close while the request is `enqueued` on the dispatch ring (worker
//       paused before dequeue): the ring is not discarded, the worker dequeues
//       and runs the syscall, real result verbatim.
//   D   close while the worker is `running` the syscall: the real result wins
//       verbatim — close must NOT turn an ordinary success into canceled.
//   D2  void path (sync_data): same close-while-running contract.
//   A2  void submit after close: invalid_state, Completion idle (the shared
//       case A covers the size path; this pins the void path).
//   A3  close then malformed descriptor: the Reserve-stage admission decision
//       (closed -> invalid_state, Decision 15; capacity full -> would_block,
//       Decision 13) wins over the Prepare-stage malformed-descriptor probe
//       (invalid_argument, Decision 6) — descriptor validation runs INSIDE
//       the admission transaction AFTER reserve (ADR Decision-5 stage order;
//       review P1). Zero residue: idle Completion, outstanding == 0,
//       slot_in_use == 0. Size + void paths, and the capacity-full case.
//   C1b close || acceptance LP, submit wins: the submit pauses INSIDE the
//       backend admission transaction, after the slot commit (Step 4) and
//       before the `binding -> outstanding` release-store (ADR Step 5 — the
//       commit/accept linearization point). close_admission() must BLOCK:
//       after close returns no new acceptance LP may occur (Decision 15 +
//       ADR :453-462 "the winning submit ... retaining its own
//       context/admission lock"). The resumed submit completes its LP (submit
//       wins), close returns after, the request completes verbatim, and a new
//       submit after close rejects invalid_state.
//   C1c close || acceptance, close wins: the submit pauses BEFORE taking the
//       admission transaction lock; close_admission() completes with no
//       contention; the resumed submit observes admission closed at reserve
//       and rejects synchronously with invalid_state — Completion idle, zero
//       residue.
//   E1  close then pending cancel: cancel still WINS the canceled terminal
//       (Scheme B) after close; enqueue no-ops; reap publishes canceled once;
//       canceled_ops == 1; no dispatch linkage.
//   E2  close then running cancel: intent only; the real syscall result wins
//       verbatim (not rewritten to canceled).
//   F1  close wakes a parked wait_one as a ONE-SHOT control wake (returns 0,
//       no fabricated completion), and a FUTURE wait_one parks normally again
//       and is woken by real progress (returns 1) — the control wake is not a
//       sticky "never park" state, so an admission-closed runtime never
//       busy-spins (issue #67 split-wait protocol).
//   G1  close || final record_terminal, close first: the interrupted wait_one
//       returns 0 after its final reap, and the LATER record_terminal +
//       progress wake is reaped by the NEXT wait_one — the control interrupt
//       must not swallow the final ready (the interrupt-vs-final-ready race
//       is closed by the caller's next reap, not by dropping the request).
//   G2  close || final record_terminal, terminal first: progress wins before
//       close; wait_one reaps the ready request with its real result; close
//       does not affect an already-stored terminal.
//   G3  invariant-only race: N requests submitted, close_admission races the
//       running workers, then a bounded drain loop — every accepted request
//       reaches exactly one verbatim terminal, accounting reaches zero, no
//       lost wake (the bounded deadline is failure protection only, never an
//       ordering claim).
//   I   submit || close concurrent linearization: a submitter loops while a
//       closer closes admission mid-stream. Every submit is either accepted
//       (the op later reaches exactly one terminal) or synchronously rejected
//       with invalid_state (Completion idle) — never a half-accepted state.
//
// All waits are bounded; on timeout or assertion failure the test resumes
// every armed gate and joins every created thread before reporting failure.
// Links sluice_async_internal_testing (the gates are guarded by
// SLUICE_ASYNC_INTERNAL_TESTING; production sluice_async has no seams).
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(5);
// C2e (B1): failure-protection bound for the deterministic negative probe in
// tp_c2e_close_waits_for_inflight_acceptance_lp (and the Fake driver case).
// The probe observes "the closer's read must not complete while the submit is
// paused": under the admission-transaction mutation the closer's close returns
// in microseconds, so this window is >=1000x the defect latency; the ordering
// proof itself is structural (the admission lock + the pause gate), not this
// window.
constexpr auto kCloseProbeTimeout = std::chrono::seconds(2);

class TempPath {
public:
    TempPath(const char* tag) {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_tp_c2e_" + std::string(tag) + "_" +
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
    if (fd < 0) { std::fprintf(stderr, "open_temp failed\n"); std::exit(1); }
    return fd;
}

// Wait for a gate's paused flag with a bounded deadline. Returns true on success.
template <class Gate>
bool wait_paused(Gate& gate, std::chrono::steady_clock::time_point deadline) {
    while (!gate.paused.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// Drain outstanding ops through the real reaper with a bounded total time.
// Uses only poll() + interrupt_backend_waiters() + yield() — never a blocking
// wait_one(), which has no timeout and could hang the test forever if a
// terminal or ready-wake were lost (the deadline is failure protection only).
bool drain_bounded(ThreadPoolBackend& backend,
                   std::chrono::steady_clock::time_point deadline) {
    while (backend.outstanding() > 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (backend.poll() == 0) {
            std::this_thread::yield();
        }
    }
    return true;
}

// RAII: resume a paused gate and join a thread on scope exit, then wait for
// the production path to leave the gate. The gate object must outlive the
// backend (declared before it in the test), so no disarm is needed. Guarantees
// the test never leaves a gate armed or a thread joinable when an assertion
// fails. (Same shape as threadpool_backend_scheme_b_race_test.)
template <class Gate>
class ScopedGateAndThread {
public:
    ScopedGateAndThread(Gate& gate, std::thread& t)
        : gate_(&gate), thread_(&t) {}
    void join() {
        if (joined_) return;
        gate_->resume.store(true, std::memory_order_release);
        thread_->join();
        joined_ = true;
    }
    ~ScopedGateAndThread() { cleanup(); }
private:
    void cleanup() {
        join();
        wait_for_exit();
    }
    void wait_for_exit() noexcept {
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (!gate_->exited.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr,
                             "ThreadPool C2e test gate failed to exit before timeout\n");
                std::abort();
            }
            std::this_thread::yield();
        }
    }
    Gate* gate_;
    std::thread* thread_;
    bool joined_ = false;
};

// Bounded wait on an atomic flag. Returns true when the flag became true.
bool wait_flag(std::atomic<bool>& flag,
               std::chrono::steady_clock::time_point deadline) {
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// Join a thread with a bounded deadline (wait_drain_deadlock_test shape): the
// thread publishes `done` when finished; the joiner waits for the flag
// (bounded), then really joins. Returns false on timeout; the caller must
// unblock the thread before the scope ends.
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

// Observe the slot state of a bound Completion (generation-validated).
bool state_of(ThreadPoolBackend& backend, const void* completion,
              detail::RequestState expected) {
    auto h = backend.handle_for_completion_for_test(completion);
    if (!h.has_value()) return false;
    auto obs = backend.observe_for_test(*h);
    return obs.has_value() && obs->state == expected;
}

// Seed a 1-byte temp file so a 1-byte read has a real result to return.
int seeded_fd(const TempPath& tp, std::byte seed) {
    int fd = open_temp(tp.path());
    if (::pwrite(fd, &seed, 1, 0) != 1) { std::fprintf(stderr, "pwrite failed\n"); std::exit(1); }
    return fd;
}

}  // namespace

// ---- C1: close while the submit path is paused between commit and enqueue --
// The slot is `pending` (committed, enqueue pin live, Completion outstanding)
// when close_admission() runs. close must NOT retroactively reject or cancel
// the accepted request: the resumed enqueue + dispatch run the real syscall
// and the request completes with its REAL result verbatim.
SLUICE_TEST_CASE(tp_c2e_close_while_pending_preserves_accepted_request) {
    ThreadPoolBackend::BeforeEnqueueLockPauseGate gate;  // outlives backend
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_enqueue_lock_pause_gate(&gate);

    TempPath tp("pending");
    int fd = seeded_fd(tp, std::byte{0x11});
    std::byte buf[1]{};
    Completion<std::size_t> c;
    std::atomic<bool> submit_done{false};
    std::atomic<bool> submit_ok{false};

    std::thread submitter([&] {
        auto r = backend.submit_read(ReadOp{fd, buf, 1, 0}, c);
        submit_ok.store(r.has_value(), std::memory_order_release);
        submit_done.store(true, std::memory_order_release);
    });
    ScopedGateAndThread<ThreadPoolBackend::BeforeEnqueueLockPauseGate> guard(gate, submitter);

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    const char* fail_msg = nullptr;
    if (!wait_paused(gate, deadline)) {
        fail_msg = "before-enqueue gate did not pause in time";
    } else if (submit_done.load(std::memory_order_acquire)) {
        fail_msg = "submitter must still be paused";
    } else if (!state_of(backend, &c, detail::RequestState::pending)) {
        fail_msg = "slot must be `pending` at the before-enqueue pause";
    }

    // close while the accepted request is `pending`.
    if (fail_msg == nullptr) {
        backend.close_admission();
        if (!state_of(backend, &c, detail::RequestState::pending)) {
            fail_msg = "close must not change a pending accepted request's state";
        } else if (c.ready()) {
            fail_msg = "close must not fabricate a completion";
        } else if (backend.outstanding() != 1) {
            fail_msg = "close must not change accepted-outstanding";
        }
    }

    // Resume: enqueue + dispatch run the real syscall; result verbatim.
    if (fail_msg == nullptr) {
        guard.join();
        if (!submit_done.load(std::memory_order_acquire) || !submit_ok.load(std::memory_order_acquire)) {
            fail_msg = "submit must have succeeded";
        } else if (!drain_bounded(backend, deadline)) {
            fail_msg = "accepted request never drained after close";
        } else if (!c.ready()) {
            fail_msg = "Completion must be ready after drain";
        } else if (!c.result().has_value() || c.result().value() != 1) {
            fail_msg = "result must be the real 1-byte read verbatim (not rewritten by close)";
        } else if (backend.outstanding() != 0) {
            fail_msg = "outstanding must reach zero";
        } else if (backend.backend_ready_count_for_test() != 0) {
            fail_msg = "backend_ready must reach zero";
        }
    }
    if (fail_msg != nullptr) {
        // cleanup path: resume the paused submitter FIRST (join is idempotent;
        // the guard dtor resumes again as a no-op), then drain + reset so the
        // backend destructor sees quiescence and the failure cause is the case.
        guard.join();
        if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
            std::fprintf(stderr, "cleanup drain failed\n");
        }
        if (c.ready()) c.reset();
        ::close(fd);
        SLUICE_FAIL(fail_msg);
    }
    c.reset();
    if (backend.arena_slot_in_use() != 0) {
        SLUICE_FAIL("slot must be released after reset");
    }
    ::close(fd);
}

// ---- C2: close while the request is `enqueued` on the dispatch ring --------
// The worker is paused BEFORE dequeue (the handle sits on the ring). close
// must not discard the dispatch linkage: the worker dequeues after resume and
// runs the real syscall; result verbatim.
SLUICE_TEST_CASE(tp_c2e_close_while_enqueued_preserves_dispatch) {
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;  // outlives backend
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_dequeue_pause_gate(&gate);

    TempPath tp("enqueued");
    int fd = seeded_fd(tp, std::byte{0x22});
    std::byte buf[1]{};
    Completion<std::size_t> c;

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
        SLUICE_FAIL("submit failed");
    }
    const char* fail_msg = nullptr;
    if (!wait_paused(gate, deadline)) {
        fail_msg = "before-dequeue gate did not pause in time";
    } else if (!state_of(backend, &c, detail::RequestState::enqueued)) {
        fail_msg = "slot must be `enqueued` at the before-dequeue pause";
    } else if (backend.dispatch_size_for_test() != 1) {
        fail_msg = "the handle must still be on the dispatch ring";
    }

    // close while the request is enqueued on the ring.
    if (fail_msg == nullptr) {
        backend.close_admission();
        if (backend.dispatch_size_for_test() != 1) {
            fail_msg = "close must not discard the dispatch linkage";
        } else if (c.ready()) {
            fail_msg = "close must not fabricate a completion";
        }
    }

    // Resume: the worker dequeues and runs the syscall; result verbatim.
    if (fail_msg == nullptr) {
        gate.resume.store(true, std::memory_order_release);
        if (!drain_bounded(backend, deadline)) {
            fail_msg = "enqueued request never drained after close";
        } else if (!c.ready() || !c.result().has_value() || c.result().value() != 1) {
            fail_msg = "result must be the real 1-byte read verbatim";
        } else if (backend.syscall_count_for_test() != 1) {
            fail_msg = "exactly one syscall must have executed";
        }
    }
    // cleanup path (runs on both success and failure): resume, drain, reset.
    gate.resume.store(true, std::memory_order_release);
    backend.set_before_dequeue_pause_gate(nullptr);
    if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
        std::fprintf(stderr, "cleanup drain failed\n");
    }
    if (c.ready()) c.reset();
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- D: close while the worker is `running` the syscall --------------------
// The running syscall's REAL result wins verbatim — close MUST NOT rewrite an
// ordinary success into canceled (ADR Decision 11/15: running cancel records
// intent only; close is not a cancel at all).
SLUICE_TEST_CASE(tp_c2e_close_while_running_result_verbatim) {
    ThreadPoolBackend::WorkerRunningPauseGate gate;  // outlives backend
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_running_pause_gate(&gate);

    TempPath tp("running");
    int fd = seeded_fd(tp, std::byte{0x33});
    std::byte buf[1]{};
    Completion<std::size_t> c;

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
        SLUICE_FAIL("submit failed");
    }
    const char* fail_msg = nullptr;
    if (!wait_paused(gate, deadline)) {
        fail_msg = "running gate did not pause in time";
    } else if (!state_of(backend, &c, detail::RequestState::running)) {
        fail_msg = "slot must be `running` at the running pause";
    } else if (backend.active_workers_for_test() != 1) {
        fail_msg = "the worker must be active";
    }

    // close while the worker is inside the syscall.
    if (fail_msg == nullptr) {
        backend.close_admission();
        if (!state_of(backend, &c, detail::RequestState::running)) {
            fail_msg = "close must not change a running request's state";
        } else if (backend.active_workers_for_test() != 1) {
            fail_msg = "close must not interrupt the running worker";
        }
    }

    // Resume: the syscall returns its REAL result; verbatim (not canceled).
    if (fail_msg == nullptr) {
        gate.resume.store(true, std::memory_order_release);
        if (!drain_bounded(backend, deadline)) {
            fail_msg = "running request never drained after close";
        } else if (!c.ready()) {
            fail_msg = "Completion must be ready";
        } else if (!c.result().has_value() || c.result().value() != 1) {
            fail_msg = "running result must be the real 1-byte read verbatim "
                       "(close must not rewrite success into canceled)";
        } else if (backend.syscall_count_for_test() != 1) {
            fail_msg = "exactly one syscall must have executed";
        }
    }
    gate.resume.store(true, std::memory_order_release);
    backend.set_running_pause_gate(nullptr);
    if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
        std::fprintf(stderr, "cleanup drain failed\n");
    }
    if (c.ready()) c.reset();
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- D2: void path — close while running sync_data; success verbatim --------
SLUICE_TEST_CASE(tp_c2e_close_while_running_void_result_verbatim) {
    ThreadPoolBackend::WorkerRunningPauseGate gate;  // outlives backend
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_running_pause_gate(&gate);

    TempPath tp("void");
    int fd = open_temp(tp.path());
    Completion<void> c;

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    if (!backend.submit_sync_data(SyncDataOp{fd}, c).has_value()) {
        SLUICE_FAIL("submit failed");
    }
    const char* fail_msg = nullptr;
    if (!wait_paused(gate, deadline)) {
        fail_msg = "running gate did not pause in time";
    } else if (!state_of(backend, &c, detail::RequestState::running)) {
        fail_msg = "slot must be `running` at the running pause";
    }

    if (fail_msg == nullptr) {
        backend.close_admission();
        if (!state_of(backend, &c, detail::RequestState::running)) {
            fail_msg = "close must not change a running void request's state";
        }
    }

    if (fail_msg == nullptr) {
        gate.resume.store(true, std::memory_order_release);
        if (!drain_bounded(backend, deadline)) {
            fail_msg = "running void request never drained after close";
        } else if (!c.ready()) {
            fail_msg = "void Completion must be ready";
        } else if (!c.result().has_value()) {
            fail_msg = "void result must be the real fdatasync success verbatim";
        }
    }
    gate.resume.store(true, std::memory_order_release);
    backend.set_running_pause_gate(nullptr);
    if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
        std::fprintf(stderr, "cleanup drain failed\n");
    }
    if (c.ready()) c.reset();
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- A2: void submit after close -> invalid_state, Completion idle ---------
// The shared case A covers the size path; this pins the void path on the real
// backend.
SLUICE_TEST_CASE(tp_c2e_void_submit_after_close_rejected) {
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/2, /*workers=*/1});
    backend.close_admission();

    TempPath tp("voidreject");
    int fd = open_temp(tp.path());
    Completion<void> c;
    auto r = backend.submit_sync_all(SyncAllOp{fd}, c);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    SLUICE_CHECK(c.idle());
    SLUICE_CHECK(!c.outstanding());
    SLUICE_CHECK(!c.ready());
    SLUICE_CHECK(backend.outstanding() == 0);
    ::close(fd);
}

// ---- A3: close then malformed descriptor -> invalid_state (Reserve wins) ---
// ADR Decision-5 stage order: descriptor validation (Prepare, Decision 6
// invalid_argument) runs INSIDE the admission transaction AFTER reserve, so
// the Reserve-stage admission decisions — closed -> invalid_state (Decision
// 15) and capacity full -> would_block (Decision 13) — take precedence. A
// post-close malformed submit (negative fd, nonzero length) must reject
// invalid_state — NOT invalid_argument — with a completely idle Completion
// and zero slot/borrow residue (review P1; size path).
SLUICE_TEST_CASE(tp_c2e_close_then_malformed_read_rejected_invalid_state) {
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/2, /*workers=*/1});
    backend.close_admission();

    Completion<std::size_t> c;
    std::byte buf[1]{};
    auto r = backend.submit_read(ReadOp{-1, buf, 1, 0}, c);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    SLUICE_CHECK(c.idle());
    SLUICE_CHECK(!c.outstanding());
    SLUICE_CHECK(!c.ready());
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---- A3b: same Reserve-over-Prepare precedence on the void path ------------
SLUICE_TEST_CASE(tp_c2e_close_then_malformed_sync_rejected_invalid_state) {
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/2, /*workers=*/1});
    backend.close_admission();

    Completion<void> c;
    auto r = backend.submit_sync_data(SyncDataOp{-1}, c);  // negative fd
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    SLUICE_CHECK(c.idle());
    SLUICE_CHECK(!c.outstanding());
    SLUICE_CHECK(!c.ready());
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---- A3c: capacity full + malformed descriptor -> would_block (Reserve wins
// over Prepare validation) ----------------------------------------------------
// The first submit holds the only slot (accepted, unreaped/unreset), so the
// second submit's reserve fails would_block BEFORE the malformed descriptor
// is even probed. Pins the ADR Decision-5 precedence: a Reserve-stage
// rejection beats the Prepare-stage invalid_argument (review P1).
SLUICE_TEST_CASE(tp_c2e_capacity_full_malformed_rejected_would_block) {
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    TempPath tp("fullmalformed");
    int fd = seeded_fd(tp, std::byte{0xC0});

    std::byte buf[1]{};
    Completion<std::size_t> c1;
    auto r1 = backend.submit_read(ReadOp{fd, buf, 1, 0}, c1);
    SLUICE_CHECK(r1.has_value());

    // The only slot is held by the accepted, unreaped c1 — reserve must fail
    // would_block even though this descriptor is malformed (negative fd).
    Completion<std::size_t> c2;
    auto r2 = backend.submit_read(ReadOp{-1, buf, 1, 0}, c2);
    SLUICE_CHECK(!r2.has_value());
    SLUICE_CHECK(r2.error().code == IoError::Code::would_block);
    SLUICE_CHECK(c2.idle());
    SLUICE_CHECK(!c2.outstanding());
    SLUICE_CHECK(!c2.ready());

    // Drain + reset so destruction is quiescent.
    SLUICE_CHECK(
        drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout));
    SLUICE_CHECK(c1.ready());
    SLUICE_CHECK(c1.result().has_value());
    c1.reset();
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    ::close(fd);
}
// The submit path pauses INSIDE the backend admission transaction, AFTER the
// slot commit (Step 4: prepared -> pending) and BEFORE the `binding ->
// outstanding` release-store (ADR Step 5 — the commit/accept linearization
// point). close_admission() must BLOCK on the transaction: after close
// returns, no new acceptance LP may occur (ADR Decision 15 + §"Commit /
// accept" :453-462 — "the winning submit ... retaining its own
// context/admission lock"). The resumed submit completes its LP (submit
// wins), close returns after, and the accepted request completes with its
// real result. This is the deterministic detector for the "close drops the
// admission transaction" mutation (M11): without the transaction, close
// returns while the submit is paused before its LP — a new acceptance LP
// after close (the Decision-15 violation this case must catch).
SLUICE_TEST_CASE(tp_c2e_close_waits_for_inflight_acceptance_lp) {
    ThreadPoolBackend::BeforeCommitBindingPauseGate gate;  // outlives backend
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_commit_binding_pause_gate(&gate);

    TempPath tp("lpwin");
    int fd = seeded_fd(tp, std::byte{0xB1});
    std::byte buf[1]{};
    Completion<std::size_t> c;
    Completion<std::size_t> c2;
    std::atomic<bool> submit_done{false};
    std::atomic<bool> submit_ok{false};
    std::atomic<bool> close_done{false};

    std::thread submitter([&] {
        auto r = backend.submit_read(ReadOp{fd, buf, 1, 0}, c);
        submit_ok.store(r.has_value(), std::memory_order_release);
        submit_done.store(true, std::memory_order_release);
    });
    ScopedGateAndThread<ThreadPoolBackend::BeforeCommitBindingPauseGate> guard(gate, submitter);

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    const char* fail_msg = nullptr;
    if (!wait_paused(gate, deadline)) {
        fail_msg = "before-commit-binding gate did not pause in time";
    } else if (submit_done.load(std::memory_order_acquire)) {
        fail_msg = "submitter must still be paused";
    } else if (c.outstanding()) {
        fail_msg = "the Completion must still be `binding` at the pause "
                   "(the LP is the binding -> outstanding release-store)";
    }

    // close_admission() must wait for the in-flight acceptance protocol.
    std::atomic<bool> close_saw_outstanding{false};
    std::thread closer;
    if (fail_msg == nullptr) {
        closer = std::thread([&] {
            backend.close_admission();
            // Decision-15 observable AT the close return: a submit that
            // entered the protocol before close has already passed its
            // acceptance LP (`binding -> outstanding` release-store, ADR
            // Step 5). Under the fix the admission-lock handoff makes the
            // submit's LP release-store visible to this read (mutex acquire
            // after the submit's mutex release); under the mutation the read
            // completes while the submitter is still paused and sees
            // `binding` — the violation this case must catch.
            close_saw_outstanding.store(c.outstanding(), std::memory_order_release);
            close_done.store(true, std::memory_order_release);
        });
        // Deterministic negative probe (the 2 s window is failure protection
        // only — see kCloseProbeTimeout): while the submit is paused before
        // its LP, the closer's read must NOT complete — a completed read means
        // close returned before the LP (the Decision-15 violation; mutant M11
        // detector). The submitter cannot advance until the test resumes it
        // (it spins on the gate), and under the fix the closer is blocked on
        // the admission lock the paused submitter holds, so the probe can only
        // fire under the mutation.
        const auto probe_deadline = std::chrono::steady_clock::now() + kCloseProbeTimeout;
        while (std::chrono::steady_clock::now() < probe_deadline) {
            if (close_done.load(std::memory_order_acquire)) {
                fail_msg = "close_admission returned before the in-flight "
                           "acceptance LP (admission transaction violated)";
                break;
            }
            std::this_thread::yield();
        }
    }

    // Resume: the submit completes its LP (binding -> outstanding), submit
    // returns success; then close acquires the admission lock and returns.
    if (fail_msg == nullptr) {
        guard.join();
        if (!submit_done.load(std::memory_order_acquire) ||
            !submit_ok.load(std::memory_order_acquire)) {
            fail_msg = "submit must have succeeded (the in-flight LP wins)";
        } else if (!wait_flag(close_done, deadline)) {
            fail_msg = "close_admission must return after the in-flight submit's LP";
        } else if (!close_saw_outstanding.load(std::memory_order_acquire)) {
            fail_msg = "close_admission returned before the in-flight acceptance "
                       "LP (admission transaction violated)";
        } else if (!drain_bounded(backend,
                                  std::chrono::steady_clock::now() + kWaitTimeout)) {
            fail_msg = "the accepted request never drained after close";
        } else if (!c.ready() || !c.result().has_value() || c.result().value() != 1) {
            fail_msg = "result must be the real 1-byte read verbatim";
        }
    }

    // After close returned, a NEW submit rejects synchronously (Decision 15).
    if (fail_msg == nullptr) {
        auto r2 = backend.submit_read(ReadOp{fd, buf, 1, 0}, c2);
        if (r2.has_value()) {
            fail_msg = "a submit after close returned must be rejected";
        } else if (r2.error().code != IoError::Code::invalid_state) {
            fail_msg = "the post-close rejection must be invalid_state";
        } else if (!c2.idle() || c2.outstanding() || c2.ready()) {
            fail_msg = "the rejected Completion must be idle (zero residue)";
        }
    }
    // cleanup (both paths): resume + join the submitter FIRST (the closer may
    // be blocked on the admission lock behind the paused submit), then join
    // the closer; drain + reset so the backend destructor sees quiescence.
    guard.join();
    if (closer.joinable()) closer.join();
    gate.resume.store(true, std::memory_order_release);
    if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
        std::fprintf(stderr, "cleanup drain failed\n");
    }
    if (c.ready()) c.reset();
    if (c2.ready()) c2.reset();
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- C1c: close || acceptance — close wins; submit rejects at reserve ------
// The submit path pauses BEFORE taking the backend admission transaction
// lock. close_admission() completes with no contention (close wins); the
// resumed submit acquires the lock, reserve observes admission closed and
// rejects synchronously with invalid_state — Completion idle, zero residue
// (ADR Decision 15).
SLUICE_TEST_CASE(tp_c2e_close_wins_submit_started_before_close_rejected) {
    ThreadPoolBackend::BeforeAdmissionLockPauseGate gate;  // outlives backend
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_admission_lock_pause_gate(&gate);

    TempPath tp("lpclose");
    int fd = seeded_fd(tp, std::byte{0xB2});
    std::byte buf[1]{};
    Completion<std::size_t> c;
    std::atomic<bool> submit_done{false};
    std::atomic<bool> submit_accepted{false};
    std::atomic<int> submit_code{0};

    std::thread submitter([&] {
        auto r = backend.submit_read(ReadOp{fd, buf, 1, 0}, c);
        if (r.has_value()) {
            submit_accepted.store(true, std::memory_order_release);
        } else {
            submit_code.store(static_cast<int>(r.error().code), std::memory_order_release);
        }
        submit_done.store(true, std::memory_order_release);
    });
    ScopedGateAndThread<ThreadPoolBackend::BeforeAdmissionLockPauseGate> guard(gate, submitter);

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    const char* fail_msg = nullptr;
    if (!wait_paused(gate, deadline)) {
        fail_msg = "before-admission-lock gate did not pause in time";
    } else if (submit_done.load(std::memory_order_acquire)) {
        fail_msg = "submitter must still be paused";
    } else if (backend.outstanding() != 0 || backend.arena_slot_in_use() != 0) {
        fail_msg = "no acceptance may have started before the admission lock";
    }

    // close wins the admission transaction: it returns while the submit is
    // still outside the lock (no contention).
    if (fail_msg == nullptr) {
        backend.close_admission();
    }

    // Resume: the submit acquires the lock; reserve observes admission closed
    // -> synchronous invalid_state; Completion idle; zero residue.
    if (fail_msg == nullptr) {
        guard.join();
        if (!submit_done.load(std::memory_order_acquire)) {
            fail_msg = "submitter must finish";
        } else if (submit_accepted.load(std::memory_order_acquire)) {
            fail_msg = "a submit that entered after close must be rejected";
        } else if (submit_code.load(std::memory_order_acquire) !=
                   static_cast<int>(IoError::Code::invalid_state)) {
            fail_msg = "the post-close rejection must be invalid_state";
        } else if (!c.idle() || c.outstanding() || c.ready()) {
            fail_msg = "the rejected Completion must be idle (zero residue)";
        } else if (backend.outstanding() != 0) {
            fail_msg = "no accepted request may exist";
        } else if (backend.arena_slot_in_use() != 0) {
            fail_msg = "no slot may be reserved";
        }
    }
    guard.join();
    // cleanup (both paths): if the submitter was NOT paused in time (gate
    // timeout) it may have been accepted before close — drain + reset so the
    // backend destructor sees quiescence and the failure stays this case, not
    // a destructor fail-fast.
    if (c.outstanding() || c.ready()) {
        if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
            std::fprintf(stderr, "cleanup drain failed\n");
        }
        if (c.ready()) c.reset();
    }
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- E1: close then pending cancel — cancel still WINS the terminal ---------
// Scheme B after close: the pending request's cancel stores the canceled
// terminal; the resumed enqueue observes backend_ready and no-ops; reap
// publishes canceled exactly once; canceled_ops == 1; no dispatch linkage.
SLUICE_TEST_CASE(tp_c2e_close_then_pending_cancel_wins) {
    ThreadPoolBackend::BeforeEnqueueLockPauseGate gate;  // outlives backend
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_enqueue_lock_pause_gate(&gate);

    TempPath tp("cancelwins");
    int fd = seeded_fd(tp, std::byte{0x44});
    std::byte buf[1]{};
    Completion<std::size_t> c;
    std::atomic<bool> submit_done{false};
    std::atomic<bool> submit_ok{false};

    std::thread submitter([&] {
        auto r = backend.submit_read(ReadOp{fd, buf, 1, 0}, c);
        submit_ok.store(r.has_value(), std::memory_order_release);
        submit_done.store(true, std::memory_order_release);
    });
    ScopedGateAndThread<ThreadPoolBackend::BeforeEnqueueLockPauseGate> guard(gate, submitter);

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    const char* fail_msg = nullptr;
    if (!wait_paused(gate, deadline)) {
        fail_msg = "before-enqueue gate did not pause in time";
    } else if (!state_of(backend, &c, detail::RequestState::pending)) {
        fail_msg = "slot must be `pending` at the before-enqueue pause";
    }

    if (fail_msg == nullptr) {
        backend.close_admission();
        // Operation cancellation remains legal after close (ADR Decision 15).
        // The pending request's cancel WINS the canceled terminal (Scheme B).
        backend.cancel(c);
        auto h = backend.handle_for_completion_for_test(&c);
        if (!h.has_value()) {
            fail_msg = "accepted request must still resolve after close+cancel";
        } else {
            auto obs = backend.observe_for_test(*h);
            if (!obs.has_value() || obs->state != detail::RequestState::backend_ready) {
                fail_msg = "cancel must have won backend_ready after close";
            } else if (backend.backend_ready_count_for_test() != 1) {
                fail_msg = "exactly one backend-ready terminal must be stored";
            }
        }
    }

    // Resume: enqueue observes backend_ready -> terminal no-op, no dispatch
    // linkage; reap publishes the canceled terminal exactly once.
    if (fail_msg == nullptr) {
        guard.join();
        if (!submit_ok.load(std::memory_order_acquire)) {
            fail_msg = "submit must have succeeded";
        } else if (backend.dispatch_size_for_test() != 0) {
            fail_msg = "cancel winner must leave NO dispatch linkage";
        } else if (backend.syscall_count_for_test() != 0) {
            fail_msg = "the canceled request must never execute a syscall";
        } else if (!drain_bounded(backend, deadline)) {
            fail_msg = "canceled request never drained after close";
        } else if (!c.ready()) {
            fail_msg = "Completion must be ready after reap";
        } else if (c.result().has_value()) {
            fail_msg = "cancel winner must publish canceled, not success";
        } else if (c.result().error().code != IoError::Code::canceled) {
            fail_msg = "terminal must be canceled";
        }
    }
    if (fail_msg != nullptr) {
        // cleanup path: resume the paused submitter FIRST (join is idempotent),
        // then drain + reset so the backend destructor sees quiescence.
        guard.join();
        if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
            std::fprintf(stderr, "cleanup drain failed\n");
        }
        if (c.ready()) c.reset();
        ::close(fd);
        SLUICE_FAIL(fail_msg);
    }
    c.reset();
    ::close(fd);
}

// ---- E2: close then running cancel — intent only, real result verbatim ------
// Running cancel after close records INTENT (best-effort, ADR Decision 11);
// the syscall's REAL result wins verbatim (not rewritten to canceled).
SLUICE_TEST_CASE(tp_c2e_close_then_running_cancel_intent_only) {
    ThreadPoolBackend::WorkerRunningPauseGate gate;  // outlives backend
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_running_pause_gate(&gate);

    TempPath tp("cancelintent");
    int fd = seeded_fd(tp, std::byte{0x55});
    std::byte buf[1]{};
    Completion<std::size_t> c;

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
        SLUICE_FAIL("submit failed");
    }
    const char* fail_msg = nullptr;
    if (!wait_paused(gate, deadline)) {
        fail_msg = "running gate did not pause in time";
    } else if (!state_of(backend, &c, detail::RequestState::running)) {
        fail_msg = "slot must be `running` at the running pause";
    }

    if (fail_msg == nullptr) {
        backend.close_admission();
        backend.cancel(c);  // legal after close; intent only
        auto h = backend.handle_for_completion_for_test(&c);
        if (!h.has_value()) {
            fail_msg = "accepted request must still resolve after close+cancel";
        } else {
            auto obs = backend.observe_for_test(*h);
            if (!obs.has_value() || obs->state != detail::RequestState::running) {
                fail_msg = "running cancel must not change the running state";
            } else if (obs->terminal_stored) {
                fail_msg = "running cancel must record INTENT, not a terminal";
            }
        }
    }

    if (fail_msg == nullptr) {
        gate.resume.store(true, std::memory_order_release);
        if (!drain_bounded(backend, deadline)) {
            fail_msg = "request never drained after close + running cancel";
        } else if (!c.ready()) {
            fail_msg = "Completion must be ready";
        } else if (!c.result().has_value() || c.result().value() != 1) {
            fail_msg = "running cancel after close must yield the REAL result verbatim";
        }
    }
    gate.resume.store(true, std::memory_order_release);
    backend.set_running_pause_gate(nullptr);
    if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
        std::fprintf(stderr, "cleanup drain failed\n");
    }
    if (c.ready()) c.reset();
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- F1: close wakes a parked wait_one (one-shot) and future waits park ----
// Participant A submits op1 (the worker pauses at the running gate, so no real
// readiness can arrive) and parks in wait_one. close_admission wakes A as a
// control wake: wait_one returns 0 with no fabricated completion. Then a
// SECOND participant's wait_one — a FUTURE wait after the close — is verifiably
// PARKED (second wait-phase flag) before the worker is released, and is woken
// by the real progress, reaping op1 (returns 1). This is the deterministic
// no-busy-spin proof: the control wake is ONE-SHOT, not a sticky "never park"
// state — a future wait parks normally and is woken by progress (mutant M5
// detector: a sticky interrupt makes B return 0 / never park).
SLUICE_TEST_CASE(tp_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin) {
    ThreadPoolBackend::WorkerRunningPauseGate gate;  // outlives backend
    std::atomic<bool> a_phase{false};
    std::atomic<bool> b_phase{false};
    ThreadPoolBackend* raw = nullptr;
    {
        auto backend = std::make_unique<ThreadPoolBackend>(
            ThreadPoolConfig{/*request_capacity=*/1, /*worker_count=*/1});
        raw = backend.get();
        raw->set_running_pause_gate(&gate);
        raw->set_wait_phase_flag_for_test(&a_phase);

        AsyncIoContext ctx(std::move(backend));
        TempPath tp("parked");
        int fd = seeded_fd(tp, std::byte{0x66});
        std::byte buf[1]{};
        Completion<std::size_t> c1;

        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        // A: submit op1, then wait_one (parks after the empty reap; the gate
        // holds the worker in `running`, so no real readiness can arrive).
        Result<std::size_t> a_wait{std::size_t{0}};
        std::atomic<bool> a_finished{false};
        std::thread participant_a([&] {
            auto sr = ctx.submit_read(ReadOp{fd, buf, 1, 0}, c1);
            if (sr.has_value()) a_wait = ctx.wait_one();
            a_finished.store(true, std::memory_order_release);
        });

        const char* fail_msg = nullptr;
        if (!wait_paused(gate, deadline)) {
            fail_msg = "running gate did not pause in time";
        } else if (!wait_flag(a_phase, deadline)) {
            fail_msg = "participant A never entered the backend ready wait";
        } else if (a_finished.load(std::memory_order_acquire)) {
            fail_msg = "participant A must still be parked";
        }

        // close wakes the parked waiter as a one-shot control wake.
        if (fail_msg == nullptr) {
            raw->close_admission();
            if (!wait_flag(a_finished, deadline)) {
                fail_msg = "close_admission did not wake the parked participant";
            } else if (!a_wait.has_value() || a_wait.value() != 0) {
                fail_msg = "control wake must return 0 (no completion reaped)";
            } else if (c1.ready()) {
                fail_msg = "control wake must not fabricate a completion";
            } else if (ctx.outstanding() != 1) {
                fail_msg = "control wake must not change request accounting";
            }
        }

        // A FUTURE wait_one (participant B) starts while op1 is still `running`
        // (the gate still holds the worker), so B MUST park — the control wake
        // is one-shot. The second wait-phase flag verifies B entered the ready
        // wait (a sticky interrupt mutant would make B return 0 immediately and
        // never park — the busy-spin defect).
        Result<std::size_t> b_wait{std::size_t{0}};
        std::atomic<bool> b_finished{false};
        std::thread participant_b;
        if (fail_msg == nullptr) {
            raw->set_wait_phase_flag_for_test(&b_phase);
            participant_b = std::thread([&] {
                b_wait = ctx.wait_one();
                b_finished.store(true, std::memory_order_release);
            });
            if (!wait_flag(b_phase, deadline)) {
                fail_msg = "a FUTURE wait_one after close must park normally "
                           "(one-shot control wake; no busy-spin, no sticky "
                           "interrupt)";
            } else if (b_finished.load(std::memory_order_acquire)) {
                fail_msg = "participant B must still be parked before the "
                           "worker is released";
            }
        }

        // Release the worker: op1 records backend-ready + progress signal; the
        // parked B is woken by real progress and reaps op1.
        if (fail_msg == nullptr) {
            raw->set_running_pause_gate(nullptr);
            gate.resume.store(true, std::memory_order_release);
            if (!wait_flag(b_finished, deadline)) {
                fail_msg = "participant B was never woken by real progress";
            } else if (!b_wait.has_value() || b_wait.value() != 1) {
                fail_msg = "the future wait_one must reap the completed request";
            } else if (!c1.ready() || !c1.result().has_value() || c1.result().value() != 1) {
                fail_msg = "op1 must complete with the real 1-byte result";
            }
        }

        // cleanup (both paths): disarm, resume, join A/B, drain, reset.
        raw->set_running_pause_gate(nullptr);
        raw->set_wait_phase_flag_for_test(nullptr);
        gate.resume.store(true, std::memory_order_release);
        (void)join_bounded(participant_a, a_finished,
                           std::chrono::steady_clock::now() + kWaitTimeout);
        if (participant_b.joinable()) {
            (void)join_bounded(participant_b, b_finished,
                               std::chrono::steady_clock::now() + kWaitTimeout);
        }
        {
            const auto drain_deadline = std::chrono::steady_clock::now() + kWaitTimeout;
            while (ctx.outstanding() > 0 && std::chrono::steady_clock::now() < drain_deadline) {
                (void)ctx.poll();
                ctx.interrupt_backend_waiters();
                std::this_thread::yield();
            }
            if (c1.ready()) c1.reset();
        }
        ::close(fd);
        if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
    }
}

// ---- G1: close || final record_terminal — close first ----------------------
// The worker pauses at the terminal-publication gate (bookkeeping done,
// terminal NOT yet stored). A waiter PARKS in wait_one FIRST (the terminal is
// not stored, so no progress wake can arrive); close_admission then wakes it
// as a control wake, and its final reap finds nothing -> returns 0. The LATER
// record_terminal + progress wake is reaped by the NEXT wait_one: the control
// interrupt must NOT swallow the final ready.
SLUICE_TEST_CASE(tp_c2e_close_before_final_terminal_no_lost_ready) {
    ThreadPoolBackend::TerminalPublicationPauseGate gate;  // outlives backend
    std::atomic<bool> wait_phase_entered{false};
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_terminal_publication_pause_gate(&gate);
    backend.set_wait_phase_flag_for_test(&wait_phase_entered);

    TempPath tp("g1");
    int fd = seeded_fd(tp, std::byte{0x77});
    std::byte buf[1]{};
    Completion<std::size_t> c;

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
        SLUICE_FAIL("submit failed");
    }
    const char* fail_msg = nullptr;
    if (!wait_paused(gate, deadline)) {
        fail_msg = "terminal-publication gate did not pause in time";
    } else if (backend.active_workers_for_test() != 0) {
        fail_msg = "bookkeeping must be done at the gate";
    } else if (backend.backend_ready_count_for_test() != 0) {
        fail_msg = "terminal must NOT be stored yet at the gate";
    }

    // A waiter parks FIRST (empty reap; no progress can arrive while the gate
    // holds the worker pre-record_terminal). It MUST be parked with the OLD
    // control generation so close's interrupt can wake it — calling close
    // before the park would make the wait snapshot the advanced generation and
    // park forever (a test-ordering error, not a production defect). The
    // wait-phase flag proves the waiter is inside wait_for_change about to
    // block.
    Result<std::size_t> a_wait{std::size_t{0}};
    std::atomic<bool> a_finished{false};
    std::thread waiter([&] {
        a_wait = backend.wait_one();
        a_finished.store(true, std::memory_order_release);
    });
    if (fail_msg == nullptr) {
        if (!wait_flag(wait_phase_entered, deadline)) {
            fail_msg = "waiter never entered the ready wait";
        } else if (a_finished.load(std::memory_order_acquire)) {
            fail_msg = "waiter must still be parked before close";
        }
    }

    // close wakes the parked waiter as a control wake: interrupted, final reap
    // finds nothing -> 0, no fabricated completion.
    if (fail_msg == nullptr) {
        backend.close_admission();
        if (!wait_flag(a_finished, deadline)) {
            fail_msg = "close_admission did not wake the parked waiter";
        } else if (!a_wait.has_value() || a_wait.value() != 0) {
            fail_msg = "interrupted wait_one must return 0 (no terminal yet)";
        } else if (c.ready()) {
            fail_msg = "control wake must not fabricate a completion";
        }
    }

    // Resume: record_terminal + progress wake land AFTER the interruption.
    // The NEXT wait_one reaps the final ready — the interrupt must not have
    // swallowed it.
    if (fail_msg == nullptr) {
        gate.resume.store(true, std::memory_order_release);
        backend.set_terminal_publication_pause_gate(nullptr);
        auto wr2 = backend.wait_one();
        if (!wr2.has_value() || wr2.value() != 1) {
            fail_msg = "the next wait_one must reap the final ready "
                       "(control interrupt must not swallow the final terminal)";
        } else if (!c.ready() || !c.result().has_value() || c.result().value() != 1) {
            fail_msg = "final result must be the real 1-byte read verbatim";
        }
    }
    // cleanup (both paths): resume, disarm, join the waiter, drain, reset.
    gate.resume.store(true, std::memory_order_release);
    backend.set_terminal_publication_pause_gate(nullptr);
    backend.set_wait_phase_flag_for_test(nullptr);
    (void)join_bounded(waiter, a_finished,
                       std::chrono::steady_clock::now() + kWaitTimeout);
    if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
        std::fprintf(stderr, "cleanup drain failed\n");
    }
    if (c.ready()) c.reset();
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- G1b: interrupt-vs-final-ready race closed by wait_one's final reap ----
// The terminal lands in the EXACT window between the interrupted control wake
// and wait_one's final reap (deterministic via the
// ControlWakeFinalReapPauseGate). wait_one must return the reaped count (1),
// NOT 0 — the control interrupt must not swallow the final ready. This is the
// deterministic detector for the "interrupt path drops the final reap"
// mutation (M4): without the final reap, wait_one returns 0 here.
SLUICE_TEST_CASE(tp_c2e_interrupt_final_reap_closes_ready_race) {
    ThreadPoolBackend::TerminalPublicationPauseGate worker_gate;
    ThreadPoolBackend::ControlWakeFinalReapPauseGate reap_gate;
    std::atomic<bool> wait_phase_entered{false};
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_terminal_publication_pause_gate(&worker_gate);
    backend.set_control_wake_final_reap_pause_gate(&reap_gate);
    backend.set_wait_phase_flag_for_test(&wait_phase_entered);

    TempPath tp("g1b");
    int fd = seeded_fd(tp, std::byte{0x7B});
    std::byte buf[1]{};
    Completion<std::size_t> c;

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
        SLUICE_FAIL("submit failed");
    }
    const char* fail_msg = nullptr;
    if (!wait_paused(worker_gate, deadline)) {
        fail_msg = "terminal-publication gate did not pause in time";
    }

    // The waiter parks (empty reap; the terminal is not stored yet).
    Result<std::size_t> a_wait{std::size_t{0}};
    std::atomic<bool> a_finished{false};
    std::thread waiter([&] {
        a_wait = backend.wait_one();
        a_finished.store(true, std::memory_order_release);
    });
    if (fail_msg == nullptr) {
        if (!wait_flag(wait_phase_entered, deadline)) {
            fail_msg = "waiter never entered the ready wait";
        }
    }

    // close wakes the waiter as a control wake; it pauses at the final-reap
    // gate BEFORE its one final reap.
    if (fail_msg == nullptr) {
        backend.close_admission();
        if (!wait_paused(reap_gate, deadline)) {
            fail_msg = "waiter never paused at the final-reap gate";
        } else if (a_finished.load(std::memory_order_acquire)) {
            fail_msg = "waiter must be paused, not finished";
        }
    }

    // NOW the final terminal is recorded (the worker resumes and stores the
    // real result + progress signal) — inside the interrupt window.
    if (fail_msg == nullptr) {
        worker_gate.resume.store(true, std::memory_order_release);
        backend.set_terminal_publication_pause_gate(nullptr);
        const auto recorded_deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (backend.backend_ready_count_for_test() != 1 &&
               std::chrono::steady_clock::now() < recorded_deadline) {
            std::this_thread::yield();
        }
        if (backend.backend_ready_count_for_test() != 1) {
            fail_msg = "terminal was never recorded in the interrupt window";
        }
    }

    // Release the final reap: wait_one's final reap finds the terminal and
    // returns 1 — the control interrupt never swallows the final ready.
    if (fail_msg == nullptr) {
        reap_gate.resume.store(true, std::memory_order_release);
        if (!wait_flag(a_finished, deadline)) {
            fail_msg = "waiter never finished after the final-reap release";
        } else if (!a_wait.has_value() || a_wait.value() != 1) {
            fail_msg = "wait_one must return the reaped count (1), not 0 — "
                       "the control interrupt must not swallow the final ready";
        } else if (!c.ready() || !c.result().has_value() || c.result().value() != 1) {
            fail_msg = "the reaped result must be the real 1-byte read verbatim";
        }
    }
    // cleanup (both paths): resume both gates, disarm, join, drain, reset.
    worker_gate.resume.store(true, std::memory_order_release);
    reap_gate.resume.store(true, std::memory_order_release);
    backend.set_terminal_publication_pause_gate(nullptr);
    backend.set_control_wake_final_reap_pause_gate(nullptr);
    backend.set_wait_phase_flag_for_test(nullptr);
    (void)join_bounded(waiter, a_finished,
                       std::chrono::steady_clock::now() + kWaitTimeout);
    if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
        std::fprintf(stderr, "cleanup drain failed\n");
    }
    if (c.ready()) c.reset();
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- G2: close || final record_terminal — terminal first -------------------
// record_terminal + progress wake land BEFORE close_admission: the wait_one
// wakes on real progress and reaps the ready request with its REAL result;
// close does not affect an already-stored terminal.
SLUICE_TEST_CASE(tp_c2e_final_terminal_before_close_not_affected) {
    ThreadPoolBackend::TerminalPublicationPauseGate gate;  // outlives backend
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_terminal_publication_pause_gate(&gate);

    TempPath tp("g2");
    int fd = seeded_fd(tp, std::byte{0x88});
    std::byte buf[1]{};
    Completion<std::size_t> c;

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
        SLUICE_FAIL("submit failed");
    }
    const char* fail_msg = nullptr;
    if (!wait_paused(gate, deadline)) {
        fail_msg = "terminal-publication gate did not pause in time";
    }

    // Release the worker: record_terminal + progress wake land FIRST.
    if (fail_msg == nullptr) {
        gate.resume.store(true, std::memory_order_release);
        backend.set_terminal_publication_pause_gate(nullptr);
        const auto ready_deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (backend.backend_ready_count_for_test() != 1 &&
               std::chrono::steady_clock::now() < ready_deadline) {
            std::this_thread::yield();
        }
        if (backend.backend_ready_count_for_test() != 1) {
            fail_msg = "terminal was never recorded";
        }
    }

    // close AFTER the terminal is stored: wait_one reaps the ready request
    // with its real result; close does not rewrite it.
    if (fail_msg == nullptr) {
        backend.close_admission();
        auto wr = backend.wait_one();
        if (!wr.has_value() || wr.value() != 1) {
            fail_msg = "wait_one must reap the already-ready request";
        } else if (!c.ready() || !c.result().has_value() || c.result().value() != 1) {
            fail_msg = "the stored terminal must be the real result verbatim "
                       "(close must not affect an already-stored terminal)";
        }
    }
    gate.resume.store(true, std::memory_order_release);
    backend.set_terminal_publication_pause_gate(nullptr);
    if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
        std::fprintf(stderr, "cleanup drain failed\n");
    }
    if (c.ready()) c.reset();
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- G3: invariant-only race — close vs running workers under load ---------
// N requests submitted, close_admission races the real worker progress, then a
// bounded drain loop: every accepted request reaches EXACTLY ONE verbatim
// terminal, accounting reaches zero, no lost wake, exactly N syscalls. The
// bounded deadline is failure protection only — no ordering is claimed or
// proven by time.
SLUICE_TEST_CASE(tp_c2e_close_races_workers_invariant_drain) {
    constexpr std::size_t kOps = 8;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/kOps, /*workers=*/2});

    TempPath tp("race");
    int fd = seeded_fd(tp, std::byte{0x99});
    std::byte buf[kOps]{};
    Completion<std::size_t> cs[kOps];

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    std::size_t accepted = 0;
    const char* fail_msg = nullptr;
    for (std::size_t i = 0; i < kOps; ++i) {
        auto r = backend.submit_read(ReadOp{fd, buf + i, 1, 0}, cs[i]);
        if (!r.has_value()) {
            fail_msg = "all submits before close must be accepted (capacity fits)";
            break;
        }
        ++accepted;
    }
    // close races the workers (they may be mid-syscall / mid-terminal now).
    if (fail_msg == nullptr) {
        backend.close_admission();
        if (!drain_bounded(backend, deadline)) {
            fail_msg = "accepted requests never all drained";
        }
    }
    if (fail_msg == nullptr) {
        for (std::size_t i = 0; i < accepted; ++i) {
            if (!cs[i].ready()) {
                fail_msg = "every accepted request must be ready";
                break;
            }
            if (!cs[i].result().has_value() || cs[i].result().value() != 1) {
                fail_msg = "every result must be the real 1-byte read verbatim";
                break;
            }
        }
    }
    if (fail_msg == nullptr) {
        if (backend.outstanding() != 0) {
            fail_msg = "outstanding must reach zero";
        } else if (backend.backend_ready_count_for_test() != 0) {
            fail_msg = "backend_ready must reach zero";
        } else if (backend.syscall_count_for_test() != accepted) {
            fail_msg = "exactly one syscall per accepted request";
        } else if (backend.poll() != 0) {
            fail_msg = "no double reap after the drain";
        }
    }
    if (fail_msg == nullptr) {
        for (std::size_t i = 0; i < accepted; ++i) cs[i].reset();
        if (backend.arena_slot_in_use() != 0) {
            fail_msg = "all slots must be released after reset";
        }
    }
    if (fail_msg != nullptr) {
        if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
            std::fprintf(stderr, "cleanup drain failed\n");
        }
        for (std::size_t i = 0; i < accepted; ++i) {
            if (cs[i].ready()) cs[i].reset();
        }
    }
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- I: submit || close concurrent linearization ----------------------------
// A submitter loops on fresh Completions while a closer closes admission
// mid-stream. Every submit must linearize as either:
//   * accepted (the op is later driven to EXACTLY ONE terminal via the still-
//     legal cancel path, then reaped and reset), or
//   * synchronously rejected with invalid_state (Completion idle, zero
//     residue).
// There is never a half-accepted state. The closer's timing is unconstrained
// (the barrier only ensures at least one accept happened before close — no
// ordering claim about the accept/reject outcome itself).
SLUICE_TEST_CASE(tp_c2e_submit_races_close_linearization) {
    constexpr std::size_t kAttempts = 256;
    constexpr std::size_t kCapacity = 8;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/kCapacity, /*workers=*/2});

    TempPath tp("lin");
    int fd = seeded_fd(tp, std::byte{0xAA});
    std::byte buf[kAttempts]{};
    Completion<std::size_t> cs[kAttempts];
    std::atomic<bool> close_called{false};
    std::atomic<std::size_t> accepted{0};
    std::atomic<std::size_t> rejected{0};
    std::atomic<bool> submitter_done{false};

    std::thread submitter([&] {
        for (std::size_t i = 0; i < kAttempts; ++i) {
            auto r = backend.submit_read(ReadOp{fd, buf + i, 1, 0}, cs[i]);
            if (r.has_value()) {
                accepted.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Any rejection MUST be one of the two legal admission outcomes:
                //   invalid_state (admission closed — close won the linearization
                //     race; ADR Decision 15), or
                //   would_block (arena capacity full mid-stream — ADR Decision 13).
                // Both MUST leave the Completion idle with ZERO residue: a
                // half-accepted state (backend binds the Completion then returns
                // an error) is the regression this case must catch.
                const auto code = r.error().code;
                if (code != IoError::Code::invalid_state &&
                    code != IoError::Code::would_block) {
                    std::fprintf(stderr,
                        "tp_c2e_submit_races_close_linearization: unexpected "
                        "reject code %d at attempt %zu\n",
                        static_cast<int>(code), i);
                    std::abort();
                }
                // Every rejection — not only invalid_state — must leave the
                // Completion idle / not outstanding / not ready. A backend that
                // illegally binds before returning an error is the half-accepted
                // defect; cleanup would otherwise mask it.
                if (!cs[i].idle() || cs[i].outstanding() || cs[i].ready()) {
                    std::fprintf(stderr,
                        "tp_c2e_submit_races_close_linearization: rejected "
                        "attempt %zu left non-idle Completion (idle=%d "
                        "outstanding=%d ready=%d)\n",
                        i, (int)cs[i].idle(), (int)cs[i].outstanding(),
                        (int)cs[i].ready());
                    std::abort();
                }
                rejected.fetch_add(1, std::memory_order_relaxed);
            }
        }
        submitter_done.store(true, std::memory_order_release);
    });

    // Close after at least one accept has happened (barrier only; the exact
    // accept/reject split is unconstrained).
    const auto first_accept_deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (accepted.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < first_accept_deadline) {
        std::this_thread::yield();
    }
    if (accepted.load(std::memory_order_relaxed) == 0) {
        backend.close_admission();
        submitter.join();
        SLUICE_FAIL("submitter never accepted a request (harness error)");
    }
    backend.close_admission();
    close_called.store(true, std::memory_order_release);
    submitter.join();

    // Every accepted request is driven to exactly one terminal via the
    // still-legal cancel path, reaped, and reset; every rejected Completion
    // is idle. The drain deadline is computed FRESH here (after the submitter
    // joined and the close + cancel setup finished) so the 256-iteration submit
    // loop and the barrier wait do not eat into the drain's bounded window
    // under a loaded/sanitizer CI (CodeRabbit finding).
    const char* fail_msg = nullptr;
    for (std::size_t i = 0; i < kAttempts; ++i) {
        if (cs[i].outstanding()) backend.cancel(cs[i]);
    }
    if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
        fail_msg = "accepted requests never all drained";
    }
    if (fail_msg == nullptr) {
        for (std::size_t i = 0; i < kAttempts; ++i) {
            if (cs[i].outstanding()) {
                fail_msg = "no Completion may remain outstanding after drain";
                break;
            }
            if (cs[i].ready()) {
                const auto res = cs[i].result();
                if (!(res.has_value() ||
                      res.error().code == IoError::Code::canceled ||
                      res.error().code == IoError::Code::eof ||
                      res.error().code == IoError::Code::backend_error)) {
                    fail_msg = "accepted request must reach a defined terminal";
                    break;
                }
                cs[i].reset();
            }
        }
    }
    if (fail_msg == nullptr) {
        if (accepted.load(std::memory_order_relaxed) + rejected.load(std::memory_order_relaxed) != kAttempts) {
            fail_msg = "every attempt must be accepted or rejected (no half-accepted)";
        } else if (backend.arena_slot_in_use() != 0) {
            fail_msg = "all slots must be released after reset";
        } else if (backend.outstanding() != 0) {
            fail_msg = "outstanding must reach zero";
        }
    }
    if (fail_msg != nullptr) {
        if (!drain_bounded(backend, std::chrono::steady_clock::now() + kWaitTimeout)) {
            std::fprintf(stderr, "cleanup drain failed\n");
        }
        for (std::size_t i = 0; i < kAttempts; ++i) {
            if (cs[i].ready()) cs[i].reset();
        }
    }
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

SLUICE_MAIN()