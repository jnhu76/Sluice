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
bool drain_bounded(ThreadPoolBackend& backend,
                   std::chrono::steady_clock::time_point deadline) {
    while (backend.outstanding() > 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::size_t n = backend.poll();
        if (n == 0) {
            auto wr = backend.wait_one();
            if (!wr.has_value()) return false;
            n = wr.value();
        }
        if (n == 0) return false;
    }
    return true;
}

// RAII: resume a paused gate and join a thread on scope exit. Guarantees the
// test never leaves a gate armed or a thread joinable if an assertion fails.
template <class Gate>
class ScopedGateAndThread {
public:
    ScopedGateAndThread(Gate& gate, std::thread& t) : gate_(&gate), thread_(&t) {}
    void join() {
        if (joined_) return;
        gate_->resume.store(true, std::memory_order_release);
        thread_->join();
        joined_ = true;
    }
    ~ScopedGateAndThread() { join(); }
private:
    Gate* gate_;
    std::thread* thread_;
    bool joined_ = false;
};

// RAII: resume a paused gate on scope exit (for tests without a submitter thread).
template <class Gate>
class ScopedGateResume {
public:
    explicit ScopedGateResume(Gate& gate) : gate_(&gate) {}
    void resume() {
        resumed_ = true;
        gate_->resume.store(true, std::memory_order_release);
    }
    ~ScopedGateResume() { if (!resumed_) resume(); }
private:
    Gate* gate_;
    bool resumed_ = false;
};

}  // namespace

SLUICE_MAIN()

// Gate A: the pause between enqueue and dispatch push fires INSIDE work_mtx_.
SLUICE_TEST_CASE(tp_enqueue_push_share_one_work_domain) {
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    ThreadPoolBackend::AfterArenaEnqueueBeforeDispatchPushPauseGate gate;
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

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    if (!wait_paused(gate, deadline)) {
        fail_msg = "Gate A did not pause in time";
    } else if (!gate.work_domain_held.load(std::memory_order_acquire)) {
        fail_msg = "Gate A must fire inside the work_mtx_ critical section";
    } else if (gate.dispatch_push_completed.load(std::memory_order_acquire)) {
        fail_msg = "dispatch push must not have completed while paused";
    } else {
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
        const bool real = c.result().has_value() && c.result().value() == 1;
        const bool canceled =
            !c.result().has_value() && c.result().error().code == IoError::Code::canceled;
        SLUICE_CHECK(real || canceled);
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
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
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
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    ThreadPoolBackend::WorkerRunningPauseGate gate;
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
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    ThreadPoolBackend::TerminalPublicationPauseGate gate;
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
