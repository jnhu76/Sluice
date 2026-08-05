// Issue #67 end-to-end ApplicationRuntime regression: the final
// backend-ready request must drain at shutdown.
//
// The captured production deadlock (TSan + gdb, seed 5905338 round 69,
// buf=64 depth=3 workers=2) parked the MW-S2 participant in
// ThreadPoolBackend::wait_one while AsyncIoContext::wait_one held
// access_mtx_, so the other scheduler worker's loop-top poll/reap — the only
// reap path for a backend_ready request — blocked forever, the coordinated
// run never terminated, and ApplicationRuntime::drain never satisfied
// drain_complete_.
//
// This test drives the exact runtime state deterministically with the
// ThreadPoolBackend test-only seams (armed on the raw backend before it is
// injected into the Runtime):
//
//   A. a task submits one real read and awaits its Completion; the backend
//      worker pauses at the running gate (op `running`, no terminal yet);
//   B. the MW-S2 participant parks in the backend ready wait after an empty
//      reap (wait-phase flag fires);
//   C. request_stop() interrupts the parked participant (control wake) — the
//      run can now reach its termination boundary;
//   D. the gate is released, the final request completes and is reaped by
//      the re-entered run, the awaiting task reaches terminal;
//   E. drain() returns, join() returns, backend_ready == 0, outstanding == 0,
//      the Completion is ready exactly once with the real byte.
//
// All ordering is carried by atomics + the existing pause gates; timeouts
// bound only the failure path (on the pre-fix code the run parks forever and
// drain() would hang — the bounded join converts that into a clean failure).
#include "harness.hpp"

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
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
                 ("sluice_drain_starvation_" + std::string(tag) + "_" +
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

// Join a thread with a bounded deadline (see threadpool_wait_drain_deadlock_test
// for the joinable()/join() reasoning). Returns false on timeout; the caller
// must unblock the thread before the scope ends.
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

}  // namespace

SLUICE_MAIN()

// The captured drain-starvation state, end to end (see the file header for the
// step map A-E).
SLUICE_TEST_CASE(final_backend_ready_request_drains_at_shutdown) {
    // Seams must outlive the backend (declared before it, destroyed after).
    ThreadPoolBackend::WorkerRunningPauseGate gate;
    std::atomic<bool> wait_phase_entered{false};

    ThreadPoolBackend* raw = nullptr;
    {
        auto backend = std::make_unique<ThreadPoolBackend>(
            ThreadPoolConfig{/*request_capacity=*/2, /*worker_count=*/2});
        raw = backend.get();
        raw->set_running_pause_gate(&gate);
        raw->set_wait_phase_flag_for_test(&wait_phase_entered);

        // Multi-participant topology matching the captured run (workers=2).
        RuntimeBuilder builder;
        builder.backend(std::move(backend)).workers(2);
        auto rt_result = builder.build();
        SLUICE_CHECK(rt_result.has_value());
        auto& rt = *rt_result.value();
        SLUICE_CHECK(rt.start().has_value());

        TempPath tp("A");
        int fd = open_temp(tp.path());
        const std::byte seed[1] = {std::byte{0x67}};
        SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

        std::byte buf[1]{};
        Completion<std::size_t> c;
        std::atomic<bool> task_done{false};
        SLUICE_CHECK(rt.submit([&](RuntimeTaskContext& rctx) {
            auto sr = rctx.submit_read(ReadOp{fd, buf, 1, 0}, c);
            if (sr.has_value()) {
                rctx.await_completion(c);
            }
            task_done.store(true, std::memory_order_release);
        }).has_value());

        const char* fail_msg = nullptr;
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;

        // B. The request is `running` (worker paused pre-syscall) and the
        //    MW-S2 participant has parked in the backend ready wait after an
        //    empty reap.
        if (!wait_flag(gate.paused, deadline)) {
            fail_msg = "running gate did not pause in time";
        } else if (!wait_flag(wait_phase_entered, deadline)) {
            fail_msg = "the MW-S2 participant never entered the backend ready wait";
        }

        // C. request_stop() must interrupt the parked participant (control
        //    wake), so the coordinated run can terminate and re-enter.
        if (fail_msg == nullptr) {
            rt.request_stop();
            // Give the driver time to terminate the run and re-enter; the
            // re-entered run parks again (the flag is a latch). A bounded
            // wait for "the run actually re-entered the ready wait" is not
            // observable directly; the drain below is the proof.
            std::this_thread::yield();
        }

        // D. Release the final request: it completes, is reaped by the
        //    re-entered run, and the awaiting task reaches terminal. The
        //    seams are disarmed HERE (before drain/join): join() destroys the
        //    backend, so `raw` must not be touched afterwards.
        if (fail_msg == nullptr) {
            raw->set_running_pause_gate(nullptr);
            raw->set_wait_phase_flag_for_test(nullptr);
            gate.resume.store(true, std::memory_order_release);
        }

        // E. drain() must return (drain_complete_ satisfied), then join().
        Result<void> drain_result = make_unexpected_void(IoError{IoError::Code::backend_error});
        std::atomic<bool> drain_done{false};
        std::thread drainer([&] {
            drain_result = rt.drain();
            drain_done.store(true, std::memory_order_release);
        });
        if (fail_msg == nullptr && !join_bounded(drainer, drain_done,
                                                 std::chrono::steady_clock::now() + kWaitTimeout)) {
            fail_msg = "drain did not complete (drain_complete_ never satisfied)";
        }
        if (fail_msg == nullptr) {
            if (!drain_result.has_value()) {
                fail_msg = "drain must return success";
            } else if (!task_done.load(std::memory_order_acquire)) {
                fail_msg = "the awaiting task must reach terminal";
            } else if (!c.ready() || !c.result().has_value() || c.result().value() != 1) {
                fail_msg = "the final request must complete with the 1 seeded byte";
            } else if (raw->backend_ready_count_for_test() != 0) {
                fail_msg = "backend_ready must reach zero before join";
            } else if (raw->outstanding() != 0) {
                fail_msg = "outstanding must reach zero before join";
            }
        }
        // The caller releases the ready Completion's slot (ADR Decision 15:
        // reap publishes Completion-ready, but slot_in_use is released only by
        // caller reset) BEFORE join() destroys the backend — quiescent
        // teardown requires slot_in_use == 0.
        if (fail_msg == nullptr) {
            c.reset();
            if (raw->arena_slot_in_use() != 0) {
                fail_msg = "caller reset must release the slot before join";
            }
        }

        Result<void> join_result = make_unexpected_void(IoError{IoError::Code::backend_error});
        std::atomic<bool> join_done{false};
        std::thread joiner([&] {
            join_result = rt.join();
            join_done.store(true, std::memory_order_release);
        });
        if (fail_msg == nullptr && !join_bounded(joiner, join_done,
                                                 std::chrono::steady_clock::now() + kWaitTimeout)) {
            fail_msg = "join did not complete (driver never exited)";
        }
        if (fail_msg == nullptr) {
            if (!join_result.has_value()) {
                fail_msg = "join must return success";
            }
            // NOTE: `raw` is destroyed by join()'s close (quiescent teardown);
            // the backend's own destructor fail-fast IS the quiescence proof —
            // a non-quiescent join would have terminated before returning.
        }

        // ---- cleanup (runs on both success and failure paths) ----
        // NOTE: after a successful join() the backend is DESTROYED — `raw`
        // must not be touched here; the gate object is separate and safe.
        gate.resume.store(true, std::memory_order_release);
        // Unblock a stuck drain/join so the threads can finish.
        rt.request_stop();
        (void)join_bounded(drainer, drain_done, std::chrono::steady_clock::now() + kWaitTimeout);
        // Release the slot BEFORE the joiner's join can tear the backend down
        // (only a ready Completion may be reset; an outstanding one is a
        // caller contract violation).
        if (c.ready()) c.reset();
        (void)join_bounded(joiner, join_done, std::chrono::steady_clock::now() + kWaitTimeout);
        ::close(fd);

        if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
    }
}
