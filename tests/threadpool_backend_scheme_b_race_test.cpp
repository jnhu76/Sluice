// Phase E ThreadPoolBackend Scheme-B race regressions.
//
// Four deterministic, timing-independent cases that drive the real
// ThreadPoolBackend through the SLUICE_ASYNC_INTERNAL_TESTING pause gates.
// Each case arms a gate, submits a real syscall, waits for the exact pause
// point, manipulates or observes the state, resumes the gate, and then drains.
// All waits are bounded; on timeout or assertion failure the test resumes every
// armed gate and joins every created thread before reporting failure.
//
// Pre-fix / post-fix behavior (honest labels):
//   A  structural lock-domain proof: the Gate-A pause fires INSIDE work_mtx_.
//      The test asserts work_domain_held==true while paused. Pre-fix code
//      pauses outside work_mtx_, so this case FAILS pre-fix and passes after
//      the enqueue/dispatch atomicity fix.
//   B  conformance: enqueued cancel wins before dequeue; the syscall does not
//      run. Likely passes pre-fix; proves the legal cancel/dequeue protocol.
//   C  conformance: running cancel records intent only; the real syscall result
//      wins verbatim. Likely passes pre-fix; proves Decision 11 semantics.
//   D  terminal publication order: while paused, bookkeeping is already done
//      (active_workers==0, syscall_count==1) but poll()==0. Pre-fix code pauses
//      BEFORE bookkeeping and AFTER record_terminal, so this case FAILS pre-fix
//      and passes after the bookkeeping reorder.
//
// Links sluice_async_internal_testing (the seams are guarded by
// SLUICE_ASYNC_INTERNAL_TESTING; production sluice_async has no seams).
#include "harness.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <barrier>
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
#include <type_traits>
#include <unistd.h>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(5);

class TempPath {
public:
    TempPath(const char* tag) {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_tp_scheme_b_" + std::string(tag) + "_" +
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
// Uses only poll() and yield() — never a blocking wait_one(), which has no
// timeout and could hang the test (and ultimately the parent waitpid) forever
// if a terminal or ready-wake were lost.
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

// RAII: resume a paused gate and join a thread on scope exit, then wait for the
// production path to leave the gate.  The gate object must outlive the backend
// (declared before it in the test), so no disarm is needed — lexical scope
// guarantees the gate is destroyed after the backend and its workers.
// Guarantees the test never leaves a gate armed or a thread joinable when an
// assertion fails.
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
                             "ThreadPool test gate failed to exit before timeout\n");
                std::abort();
            }
            std::this_thread::yield();
        }
    }
    Gate* gate_;
    std::thread* thread_;
    bool joined_ = false;
};

// RAII: resume a paused gate on scope exit (for tests without a submitter
// thread) and wait for the production path to leave the gate.  The gate object
// must outlive the backend (declared before it in the test), so no disarm is
// needed — lexical scope guarantees the gate is destroyed after the backend
// and its workers.
template <class Gate>
class ScopedGateResume {
public:
    ScopedGateResume(Gate& gate)
        : gate_(&gate) {}
    void resume() {
        if (resumed_) return;
        gate_->resume.store(true, std::memory_order_release);
        resumed_ = true;
    }
    ~ScopedGateResume() { cleanup(); }
private:
    void cleanup() {
        resume();
        wait_for_exit();
    }
    void wait_for_exit() noexcept {
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (!gate_->exited.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr,
                             "ThreadPool test gate failed to exit before timeout\n");
                std::abort();
            }
            std::this_thread::yield();
        }
    }
    Gate* gate_;
    bool resumed_ = false;
};

}  // namespace

SLUICE_MAIN()

// Gate A: the pause between enqueue and dispatch push fires INSIDE work_mtx_.
// No cancel is issued in this case: a canceled terminal would be a backend
// defect, so only a real success (value==1, exactly one syscall) is accepted.
SLUICE_TEST_CASE(tp_enqueue_push_share_one_work_domain) {
    // Gate must outlive backend (destroyed after it by C++ reverse declaration
    // order), so the worker never accesses a destroyed atomic.
    ThreadPoolBackend::AfterArenaEnqueueBeforeDispatchPushPauseGate gate;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_after_enqueue_before_push_pause_gate(&gate);

    TempPath tp("A");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x11}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    Result<void> submit_result;
    std::thread submitter([&] {
        submit_result = backend.submit_read(ReadOp{fd, buf, 1, 0}, c);
    });
    ScopedGateAndThread arm(gate, submitter);

    const char* fail_msg = nullptr;
    std::uint64_t syscalls_before = 0;

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    if (!wait_paused(gate, deadline)) {
        fail_msg = "Gate A did not pause in time";
    } else if (!gate.work_domain_held.load(std::memory_order_acquire)) {
        fail_msg = "Gate A must fire inside the work_mtx_ critical section";
    } else if (gate.dispatch_push_completed.load(std::memory_order_acquire)) {
        fail_msg = "dispatch push must not have completed while paused";
    } else {
        syscalls_before = backend.syscall_count_for_test();
        auto handle = backend.handle_for_completion_for_test(&c);
        if (!handle.has_value()) {
            fail_msg = "handle_for_completion_for_test must find the bound Completion";
        } else {
            auto obs = backend.observe_for_test(*handle);
            if (!obs.has_value()) {
                fail_msg = "observe_for_test must validate the live handle";
            } else if (obs->state != detail::RequestState::enqueued) {
                fail_msg = "slot must be enqueued while paused";
            } else if (obs->enqueue_pin_live) {
                fail_msg = "enqueue pin must be cleared";
            }
        }
    }

    arm.join();

    if (fail_msg == nullptr) {
        SLUICE_CHECK_MSG(submit_result.has_value(),
                         "submit must succeed (commit already accepted)");
        SLUICE_CHECK(drain_bounded(backend,
                                   std::chrono::steady_clock::now() + kWaitTimeout));
        SLUICE_CHECK(c.ready());
        // No cancel was issued — only a real success is legal; a spurious
        // canceled terminal would be a backend defect.
        SLUICE_CHECK(c.result().has_value());
        SLUICE_CHECK(c.result().value() == 1);
        SLUICE_CHECK(backend.syscall_count_for_test() == syscalls_before + 1);
        SLUICE_CHECK(backend.outstanding() == 0);
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        // Cleanup on failure so the bound Completion can be reset without
        // triggering the Completion authority fail-fast.
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// Gate B: enqueued cancel wins before the worker dequeues; no syscall runs.
SLUICE_TEST_CASE(tp_enqueued_cancel_wins_no_syscall) {
    // Gate must outlive backend (destroyed after it by C++ reverse declaration
    // order), so the worker never accesses a destroyed atomic.
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_dequeue_pause_gate(&gate);
    ScopedGateResume guard(gate);

    TempPath tp("B");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x22}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;

    if (!wait_paused(gate, deadline)) {
        fail_msg = "Gate B did not pause in time";
    } else {
        const std::uint64_t syscalls_before = backend.syscall_count_for_test();
        if (backend.dispatch_size_for_test() != 1) {
            fail_msg = "dispatch ring must hold exactly one enqueued op";
        } else {
            backend.cancel(c);
            guard.resume();
            if (!drain_bounded(backend,
                               std::chrono::steady_clock::now() + kWaitTimeout)) {
                fail_msg = "drain did not complete in time";
            } else if (!c.ready()) {
                fail_msg = "cancel must leave the Completion ready";
            } else if (c.result().has_value()) {
                fail_msg = "canceled op must report an error";
            } else if (c.result().error().code != IoError::Code::canceled) {
                fail_msg = "canceled op must report IoError::canceled";
            } else if (backend.syscall_count_for_test() != syscalls_before) {
                fail_msg = "canceled enqueued op must not execute a syscall";
            } else if (backend.outstanding() != 0) {
                fail_msg = "outstanding must be zero after drain";
            }
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// Gate C: running cancel records intent only; the real syscall result wins verbatim.
SLUICE_TEST_CASE(tp_running_cancel_intent_real_result_verbatim) {
    // Gate must outlive backend (destroyed after it by C++ reverse declaration
    // order), so the worker never accesses a destroyed atomic.
    ThreadPoolBackend::WorkerRunningPauseGate gate;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_running_pause_gate(&gate);
    ScopedGateResume guard(gate);

    TempPath tp("C");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x33}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;

    if (!wait_paused(gate, deadline)) {
        fail_msg = "Gate C did not pause in time";
    } else {
        const std::uint64_t syscalls_before = backend.syscall_count_for_test();
        if (backend.active_workers_for_test() != 1) {
            fail_msg = "exactly one worker must be running";
        } else {
            backend.cancel(c);
            guard.resume();
            if (!drain_bounded(backend,
                               std::chrono::steady_clock::now() + kWaitTimeout)) {
                fail_msg = "drain did not complete in time";
            } else if (!c.ready()) {
                fail_msg = "running-cancel op must still complete";
            } else if (!c.result().has_value()) {
                fail_msg = "real result must win verbatim; cancel must not rewrite";
            } else if (c.result().value() != 1) {
                fail_msg = "read must return the 1 seeded byte";
            } else if (backend.syscall_count_for_test() != syscalls_before + 1) {
                fail_msg = "exactly one syscall must have executed";
            } else if (backend.outstanding() != 0) {
                fail_msg = "outstanding must be zero after drain";
            }
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// Gate D: terminal publication happens after worker bookkeeping is complete.
SLUICE_TEST_CASE(tp_terminal_publication_after_bookkeeping) {
    // Gate must outlive backend (destroyed after it by C++ reverse declaration
    // order), so the worker never accesses a destroyed atomic.
    ThreadPoolBackend::TerminalPublicationPauseGate gate;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_terminal_publication_pause_gate(&gate);
    ScopedGateResume guard(gate);

    TempPath tp("D");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x44}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;

    if (!wait_paused(gate, deadline)) {
        fail_msg = "Gate D did not pause in time";
    } else {
        // Post-fix invariant: bookkeeping is done, but reap has not yet published.
        if (backend.active_workers_for_test() != 0) {
            fail_msg = "active_workers must be zero before publication";
        } else if (backend.syscall_count_for_test() != 1) {
            fail_msg = "exactly one syscall must have executed before publication";
        } else if (backend.poll() != 0) {
            fail_msg = "poll must see nothing ready before publication";
        } else if (c.ready()) {
            fail_msg = "Completion must not be ready before reap publishes";
        } else {
            guard.resume();
            if (!drain_bounded(backend,
                               std::chrono::steady_clock::now() + kWaitTimeout)) {
                fail_msg = "drain did not complete in time";
            } else if (!c.ready()) {
                fail_msg = "op must complete after resume";
            } else if (!c.result().has_value()) {
                fail_msg = "read must succeed";
            } else if (c.result().value() != 1) {
                fail_msg = "read must return the 1 seeded byte";
            } else if (backend.outstanding() != 0) {
                fail_msg = "outstanding must be zero after drain";
            }
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- C2b row 5 (ThreadPool): canceled_ops tallies ONLY on terminal_won ------
// Enqueued cancel wins the terminal (Gate B): exactly one canceled_ops. A late
// cancel on the already-terminal (still bound) Completion is already_terminal
// and never tallies again; the worker runs no syscall.
SLUICE_TEST_CASE(tp_canceled_ops_tallied_only_on_terminal_won) {
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_dequeue_pause_gate(&gate);
    ScopedGateResume guard(gate);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    TempPath tp("E");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x55}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;

    if (!wait_paused(gate, deadline)) {
        fail_msg = "Gate E did not pause in time";
    } else {
        const std::uint64_t syscalls_before = backend.syscall_count_for_test();
        backend.cancel(c);  // enqueued -> terminal_won
        if (stats.canceled_ops != 1) {
            fail_msg = "terminal_won must tally exactly one canceled op";
        } else if (stats.completion_errors != 0) {
            fail_msg = "a canceled winner is not a completion error";
        } else {
            // Late cancel while the canceled terminal is bound:
            // already_terminal -> no second tally.
            backend.cancel(c);
            if (stats.canceled_ops != 1) {
                fail_msg = "late cancel must not tally again";
            } else {
                guard.resume();
                if (!drain_bounded(backend,
                                   std::chrono::steady_clock::now() + kWaitTimeout)) {
                    fail_msg = "drain did not complete in time";
                } else if (!c.ready()) {
                    fail_msg = "canceled op must be ready after drain";
                } else if (c.result().has_value()) {
                    fail_msg = "canceled op must report an error";
                } else if (c.result().error().code != IoError::Code::canceled) {
                    fail_msg = "canceled op must report IoError::canceled";
                } else if (backend.syscall_count_for_test() != syscalls_before) {
                    fail_msg = "canceled enqueued op must not execute a syscall";
                }
            }
        }
    }

    if (fail_msg == nullptr) {
        // Cancel AFTER ready (still bound until reset): already_terminal.
        backend.cancel(c);
        SLUICE_CHECK(stats.canceled_ops == 1);
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- C2b row 5/6 (ThreadPool): running-cancel intent never tallies ---------
// Gate C: cancel on a RUNNING op records intent only — no canceled terminal,
// no canceled_ops tally; the real syscall result wins VERBATIM (never rewritten
// to canceled). A cancel after that ordinary winner is already_terminal.
SLUICE_TEST_CASE(tp_running_cancel_intent_does_not_tally) {
    ThreadPoolBackend::WorkerRunningPauseGate gate;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_running_pause_gate(&gate);
    ScopedGateResume guard(gate);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    TempPath tp("F");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x66}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;

    if (!wait_paused(gate, deadline)) {
        fail_msg = "Gate F did not pause in time";
    } else {
        backend.cancel(c);  // running -> intent_recorded
        if (stats.canceled_ops != 0) {
            fail_msg = "intent_recorded must NOT tally canceled_ops";
        } else if (backend.backend_ready_count_for_test() != 0) {
            fail_msg = "intent must not store a terminal or push the ready ring";
        } else if (c.ready()) {
            fail_msg = "the Completion must stay outstanding on intent";
        } else {
            guard.resume();
            if (!drain_bounded(backend,
                               std::chrono::steady_clock::now() + kWaitTimeout)) {
                fail_msg = "drain did not complete in time";
            } else if (!c.ready()) {
                fail_msg = "running-cancel op must still complete";
            } else if (!c.result().has_value()) {
                fail_msg = "real result must win verbatim; intent must not rewrite";
            } else if (c.result().value() != 1) {
                fail_msg = "read must return the 1 seeded byte";
            } else if (stats.canceled_ops != 0) {
                fail_msg = "an ordinary winner must never tally canceled_ops";
            }
        }
    }

    if (fail_msg == nullptr) {
        // Cancel after the ordinary winner: already_terminal, still no tally.
        backend.cancel(c);
        SLUICE_CHECK(stats.canceled_ops == 0);
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- C2b row 4 (ThreadPool integration): stale-generation events harmless --
// After the Completion is reset (slot released, generation advanced) and the
// SAME physical slot is reused by a NEW request, stale-generation attempts
// cannot act on the new occupant: the stale SlotHandle fails observe_for_test,
// the stale pointer-keyed cancel resolves nothing, and the new request's
// result/counters/state stay exactly intact. All identity is pointer-free
// (SlotHandle/RequestKey) — no Completion reverse map.
SLUICE_TEST_CASE(tp_stale_generation_event_harmless) {
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    TempPath tp("G");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x77}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    const char* fail_msg = nullptr;

    // Generation N: full lifecycle; capture the slot+generation identity.
    if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
        fail_msg = "first submit must succeed";
    }
    std::optional<detail::SlotHandle> h0;
    if (fail_msg == nullptr) {
        h0 = backend.handle_for_completion_for_test(&c);
        if (!h0.has_value()) {
            fail_msg = "the bound Completion must resolve to a slot handle";
        } else if (!drain_bounded(backend,
                                  std::chrono::steady_clock::now() + kWaitTimeout)) {
            fail_msg = "first drain did not complete in time";
        } else if (!c.ready() || !c.result().has_value() || c.result().value() != 1) {
            fail_msg = "first op must complete with the seeded byte";
        }
    }

    if (fail_msg == nullptr) {
        // Late cancel while bound: already_terminal — no tally, no syscall.
        const std::uint64_t syscalls_after_first = backend.syscall_count_for_test();
        backend.cancel(c);
        if (stats.canceled_ops != 0) {
            fail_msg = "late cancel on a terminal op must not tally";
        } else {
            c.reset();  // release handshake: slot freed, generation advances
            // Stale attempts after release:
            if (backend.observe_for_test(*h0).has_value()) {
                fail_msg = "stale handle must fail observation (generation advanced)";
            } else {
                backend.cancel(c);  // pointer no longer resolves -> no-op
                if (stats.canceled_ops != 0 || backend.outstanding() != 0) {
                    fail_msg = "stale cancel attempt must be harmless";
                }
            }
        }
        if (fail_msg == nullptr) {
            // The same physical slot is reused by a NEW request (gen N+1).
            if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
                fail_msg = "second submit must reuse the released slot";
            } else {
                auto h1 = backend.handle_for_completion_for_test(&c);
                if (!h1.has_value()) {
                    fail_msg = "new occupant must resolve to a slot handle";
                } else if (h1->slot.value != h0->slot.value ||
                           h1->generation.value != h0->generation.value + 1) {
                    fail_msg = "reuse must keep the slot and advance generation by one";
                } else if (backend.observe_for_test(*h0).has_value()) {
                    fail_msg = "stale handle must not observe the new occupant";
                } else if (!drain_bounded(backend,
                                           std::chrono::steady_clock::now() + kWaitTimeout)) {
                    fail_msg = "second drain did not complete in time";
                } else if (!c.ready() || !c.result().has_value() ||
                           c.result().value() != 1) {
                    fail_msg = "new occupant must complete with ITS OWN result";
                } else if (backend.syscall_count_for_test() !=
                           syscalls_after_first + 1) {
                    fail_msg = "exactly one new syscall for the new occupant";
                } else if (stats.canceled_ops != 0 || stats.completion_errors != 0) {
                    fail_msg = "stale attempts must leave counters intact";
                }
            }
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- C2b row 8 (ThreadPool): publication boundary — reap gates ready -------
// Runtime evidence that a worker NEVER publishes: once the worker's syscall
// finished and record_terminal stored the backend_ready terminal, the
// Completion is STILL not ready — only poll()/wait_one() reap publishes
// through the slot binding. No gate is needed: the test catches the exact
// backend_ready window (terminal stored, not yet reaped) because only the
// main thread reaps.
SLUICE_TEST_CASE(tp_publication_boundary_reap_gates_ready) {
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});

    TempPath tp("H");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x88}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    // Wait for the worker to record the terminal (backend_ready), BEFORE any
    // reap runs.
    while (backend.backend_ready_count_for_test() == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            fail_msg = "terminal was not recorded before timeout";
            break;
        }
        std::this_thread::yield();
    }

    if (fail_msg == nullptr) {
        if (c.ready()) {
            fail_msg = "Completion must NOT be ready before poll/reap publishes";
        } else if (backend.syscall_count_for_test() != 1) {
            fail_msg = "exactly one syscall must have executed";
        } else if (backend.poll() != 1) {
            fail_msg = "the reap must publish exactly one Completion";
        } else if (!c.ready()) {
            fail_msg = "reap must publish the Completion ready";
        } else if (!c.result().has_value() || c.result().value() != 1) {
            fail_msg = "read must return the 1 seeded byte";
        } else if (backend.poll() != 0) {
            fail_msg = "a second poll must publish nothing (exactly-once)";
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- C2b rows 6/7 (ThreadPool): cancel races the worker terminal winner ----
// Genuine two-thread TSan evidence: a cancel issued concurrently with the
// worker's dispatch/syscall races for the single terminal transition. Each
// iteration asserts the exactly-one winner contract end to end:
//   - exactly one publication (poll total == 1), one ready Completion
//   - the result is EITHER canceled OR the real 1-byte success
//   - canceled_ops tallies exactly the canceled winners (never intent/losers)
//   - syscall_count tallies exactly the syscall winners (cancel-won iterations
//     run no syscall)
// Barrier-released; no sleep_for, no timing assumptions.
SLUICE_TEST_CASE(tp_cancel_races_worker_terminal_exactly_one) {
    constexpr std::size_t kIters = 300;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    TempPath tp("I");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x99}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    std::uint64_t canceled_total = 0;
    std::uint64_t success_total = 0;

    for (std::size_t iter = 0; iter < kIters; ++iter) {
        Completion<std::size_t> c;
        SLUICE_CHECK_MSG(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value(),
                         "submit must succeed on a drained backend");

        // Release the canceler exactly when the op is in flight; the worker
        // races it under the backend's work-domain arbitration.
        std::barrier sync{2};
        std::thread canceler([&] {
            sync.arrive_and_wait();
            backend.cancel(c);
        });
        sync.arrive_and_wait();
        canceler.join();

        // Drain through the real reaper, counting publications.
        std::size_t published = 0;
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (backend.outstanding() > 0) {
            SLUICE_CHECK_MSG(std::chrono::steady_clock::now() < deadline,
                             "drain must complete within the bounded deadline");
            published += backend.poll();
            if (backend.outstanding() > 0) std::this_thread::yield();
        }

        SLUICE_CHECK_MSG(published == 1, "exactly one publication per iteration");
        SLUICE_CHECK(c.ready());
        if (c.result().has_value()) {
            SLUICE_CHECK(c.result().value() == 1);
            ++success_total;
        } else {
            SLUICE_CHECK(c.result().error().code == IoError::Code::canceled);
            ++canceled_total;
        }
        c.reset();

        // Exactly-one accounting: a canceled winner tallied one canceled op and
        // ran no syscall; a syscall winner tallied neither.
        SLUICE_CHECK(stats.canceled_ops == canceled_total);
        SLUICE_CHECK(backend.syscall_count_for_test() == success_total);
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    }

    SLUICE_CHECK(canceled_total + success_total == kIters);
    SLUICE_CHECK(backend.outstanding() == 0);
    ::close(fd);
}
