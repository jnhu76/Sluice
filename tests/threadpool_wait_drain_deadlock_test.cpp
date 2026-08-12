// Issue #67 deterministic regression: wait_one must not hold access_mtx_
// across the backend ready wait.
//
// The captured production deadlock (TSan + gdb, seed 5905338 round 69):
//   participant A  AsyncIoContext::wait_one -> ThreadPoolBackend::wait_one
//                  -> ready_cv_.wait()  PARKED while holding access_mtx_
//   participant B  loop-top drain -> AsyncIoContext::poll
//                  -> blocked forever on access_mtx_
//   final request  entered backend_ready but no poll/reap path was reachable
//   drain_complete_ never satisfied
//
// This test drives the same state deterministically with the existing
// ThreadPoolBackend test-only seams (the WorkerRunningPauseGate holds the
// final request in `running` so NO ready signal can arrive while A is parked,
// and the wait-phase flag announces A's empty-reap -> park transition):
//
//   A. participant A submits op1 and enters wait_one
//   B. A completes an empty reap and parks in the backend ready wait
//   C. while A is parked, a second participant must be able to reach poll()
//      (this is the property the old code violated: A held access_mtx_)
//   D. the final request is submitted while A is still parked (the running
//      gate holds both workers, so no real readiness races the control wake)
//   E. close_admission wakes A as a control wake — wait_one returns 0, no
//      completion is fabricated, no accounting changes (I8/I9)
//   F. both workers are released, both real syscalls complete and are reaped
//      by poll, and outstanding/backend_ready reach 0
//
// On the pre-fix code this FAILS (bounded) at step C: the probe poll blocks
// on access_mtx_ forever, the bounded join times out, and the test reports
// failure instead of hanging the suite. All waits are bounded; on timeout or
// assertion failure the test resumes every armed gate and joins every created
// thread before reporting failure (the timeout is test isolation only — it
// never carries thread-ordering responsibility).
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

class TempPath {
public:
    explicit TempPath(const char* tag) {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_wait_drain_" + std::string(tag) + "_" +
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
    if (fd < 0) {
        std::fprintf(stderr, "open_temp failed\n");
        std::exit(1);
    }
    return fd;
}

// Bounded wait on an atomic flag. Returns true when the flag became true.
bool wait_flag(std::atomic<bool>& flag, std::chrono::steady_clock::time_point deadline) {
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// Join a thread with a bounded deadline. The thread must publish `done` when
// its work is finished; the joiner waits for the flag (bounded), then really
// joins (std::thread::joinable() stays true until join(), so spinning on
// joinable() alone would never join; and join() on an already-joined thread
// throws EINVAL, so skip it). Returns false on timeout; the caller must
// unblock the thread before the scope ends.
bool join_bounded(std::thread& t, std::atomic<bool>& done,
                  std::chrono::steady_clock::time_point deadline) {
    if (!t.joinable()) return true;  // already joined
    while (!done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    t.join();
    return true;
}

}  // namespace

SLUICE_MAIN()

// The full issue-#67 state sequence, deterministic under the internal-testing
// seams (see the file header for the step map A-F).
SLUICE_TEST_CASE(wait_one_does_not_starve_poll_or_drain) {
    ThreadPoolBackend::WorkerRunningPauseGate gate;  // must outlive the backend
    std::atomic<bool> wait_phase_entered{false};

    ThreadPoolBackend* raw = nullptr;
    {
        auto backend = std::make_unique<ThreadPoolBackend>(
            ThreadPoolConfig{/*request_capacity=*/2, /*worker_count=*/2});
        raw = backend.get();
        raw->set_running_pause_gate(&gate);
        raw->set_wait_phase_flag_for_test(&wait_phase_entered);

        sluice::AsyncStats stats;
        AsyncIoContext ctx(std::move(backend), &stats);

        TempPath tp("A");
        int fd = open_temp(tp.path());
        const std::byte seed[1] = {std::byte{0x67}};
        SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

        std::byte buf1[1]{};
        std::byte buf2[1]{};
        Completion<std::size_t> c1, c2;

        // Participant A: submit op1 (the worker pauses at the running gate, so
        // no ready signal can arrive while A is parked), then wait_one().
        // Result<T> has no default constructor; the sentinel is only read
        // after a_done proves the assignment happened.
        Result<std::size_t> a_wait_result{std::size_t{0}};
        std::atomic<bool> a_finished{false};
        std::thread participant_a([&] {
            auto sr = ctx.submit_read(ReadOp{fd, buf1, 1, 0}, c1);
            if (sr.has_value()) {
                a_wait_result = ctx.wait_one();
            }
            a_finished.store(true, std::memory_order_release);
        });

        const char* fail_msg = nullptr;
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;

        // B. op1 is `running` (worker paused pre-syscall) and A has parked in
        //    the backend ready wait after an empty reap.
        if (!wait_flag(gate.paused, deadline)) {
            fail_msg = "running gate did not pause in time";
        } else if (!wait_flag(wait_phase_entered, deadline)) {
            fail_msg = "participant A never entered the backend ready wait";
        } else if (a_finished.load(std::memory_order_acquire)) {
            fail_msg = "participant A must still be parked";
        }

        // C. A second participant must be able to reach poll() while A is
        //    parked. The old code held access_mtx_ across the park, so this
        //    poll blocks forever (bounded join -> failure instead of a hang).
        std::size_t probe_poll_result = 0;
        std::atomic<bool> probe_done{false};
        std::thread participant_b([&] {
            probe_poll_result = ctx.poll();
            probe_done.store(true, std::memory_order_release);
        });
        if (fail_msg == nullptr && !join_bounded(participant_b, probe_done, deadline)) {
            fail_msg =
                "poll blocked while a participant was parked in wait_one "
                "(access_mtx_ held across the backend wait)";
        }

        if (fail_msg == nullptr) {
            // The probe poll observed nothing (op1 is still `running`).
            if (probe_poll_result != 0) {
                fail_msg = "probe poll must reap nothing while op1 is running";
            } else if (a_finished.load(std::memory_order_acquire)) {
                fail_msg = "participant A must still be parked after the probe poll";
            }
        }

        // D. The final request is submitted WHILE A is still parked (the gate
        //    still holds BOTH workers, so no real readiness can arrive before
        //    the control wake — the interrupt-vs-progress outcome stays
        //    deterministic). The submit itself must succeed: A holds no
        //    context lock while parked.
        if (fail_msg == nullptr) {
            auto sr = ctx.submit_read(ReadOp{fd, buf2, 1, 0}, c2);
            if (!sr.has_value()) {
                fail_msg = "submit of the final request must succeed while A is parked";
            } else if (ctx.outstanding() != 2) {
                fail_msg = "both requests must be outstanding (workers paused at the gate)";
            }
        }

        // E. Close admission: a control wake, not a fabricated completion.
        if (fail_msg == nullptr) {
            raw->close_admission();
            if (!wait_flag(a_finished, deadline)) {
                fail_msg = "close_admission did not wake the parked participant";
            } else if (a_wait_result.has_value() && a_wait_result.value() != 0) {
                fail_msg = "control wake must return 0 (no completion reaped)";
            } else if (!a_wait_result.has_value()) {
                fail_msg = "control wake must not surface as a backend error";
            } else if (c1.ready() || c2.ready()) {
                fail_msg = "control wake must not fabricate a completion";
            } else if (ctx.outstanding() != 2) {
                fail_msg = "the control wake must not change request accounting";
            } else if (stats.completed_ops != 0 || stats.wait_calls != 1) {
                fail_msg = "control wake must not count completions (I8/I9)";
            }
        }

        // F. Release both workers: real syscalls complete, are reaped by the
        //    remaining participant (poll is the single reap path — this is
        //    what the old code starved while a participant held access_mtx_
        //    across the backend wait), and the context drains to zero.
        if (fail_msg == nullptr) {
            raw->set_running_pause_gate(nullptr);
            resume_threadpool_gate(gate);
            const auto reap_deadline = std::chrono::steady_clock::now() + kWaitTimeout;
            while (!c1.ready() || !c2.ready()) {
                if (std::chrono::steady_clock::now() >= reap_deadline) {
                    fail_msg = "requests were never reaped after the gate release";
                    break;
                }
                ctx.poll();
                std::this_thread::yield();
            }
        }
        if (fail_msg == nullptr) {
            if (!c1.ready() || !c1.result().has_value() || c1.result().value() != 1) {
                fail_msg = "op1 must complete with the 1 seeded byte";
            } else if (!c2.ready() || !c2.result().has_value() || c2.result().value() != 1) {
                fail_msg = "op2 must complete with the 1 seeded byte";
            } else if (ctx.outstanding() != 0) {
                fail_msg = "outstanding must reach zero after the final reap";
            } else if (raw->backend_ready_count_for_test() != 0) {
                fail_msg = "backend_ready must reach zero after the final reap";
            } else if (stats.completed_ops != 2) {
                fail_msg = "exactly two completions must be counted (no double count)";
            } else if (stats.wait_calls != 1) {
                fail_msg = "wait_calls must count the single external wait call";
            }
        }

        // ---- cleanup (runs on both success and failure paths) ----
        raw->set_running_pause_gate(nullptr);
        raw->set_wait_phase_flag_for_test(nullptr);
        resume_threadpool_gate(gate);
        // If A is still parked (failure path), the release above lets op1
        // complete and A's wait_one return, unblocking any stuck probe too.
        (void)join_bounded(participant_a, a_finished,
                           std::chrono::steady_clock::now() + kWaitTimeout);
        (void)join_bounded(participant_b, probe_done,
                           std::chrono::steady_clock::now() + kWaitTimeout);
        if (c1.ready()) c1.reset();
        if (c2.ready()) c2.reset();
        // Drain anything the failure path may have left outstanding.
        {
            const auto drain_deadline = std::chrono::steady_clock::now() + kWaitTimeout;
            while (ctx.outstanding() > 0 && std::chrono::steady_clock::now() < drain_deadline) {
                ctx.poll();
                std::this_thread::yield();
            }
            if (c1.ready()) c1.reset();
            if (c2.ready()) c2.reset();
        }
        ::close(fd);

        if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
    }
}
