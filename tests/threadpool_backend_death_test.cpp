// Phase E ThreadPoolBackend quiescent-destruction fail-fast death tests.
//
// POSIX-only (fork/exec/waitpid). Each child case re-execs this binary via
// death_test_runner_posix.hpp, installs a deterministic terminate handler, and
// either terminates with exit 86 (fail-fast fired) or exits 87 (the call
// returned without terminating — the regression). The control case exits 0.
//
// Critical: cases 1 and 2 arm pause gates that park the worker in a state that
// the pre-fix destructor would try to join (and hang). The new destructor
// checks quiescence BEFORE setting stopping_ or joining, so it fail-fasts
// immediately while the worker is still paused. The death test therefore ships
// WITH the destructor fix (commit 5), not before it.
#include "harness.hpp"
#include "death_test_runner_posix.hpp"

#if defined(__unix__)

#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string>
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
                 ("sluice_tp_death_" + std::string(tag) + "_" +
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
    if (fd < 0) std::_Exit(sluice_death_test::kChildTestFailExit);
    return fd;
}

template <class Gate>
bool wait_paused(Gate& gate, std::chrono::steady_clock::time_point deadline) {
    while (!gate.paused.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

}  // namespace

// 1) Destroy with an enqueued op: the worker is paused before dequeue.
void child_destroy_with_enqueued() {
    sluice_death_test::install_deterministic_terminate_handler();

    {
        ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
        ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
        backend.set_before_dequeue_pause_gate(&gate);

        TempPath tp("enqueued");
        int fd = open_temp(tp.path());
        const std::byte seed[1] = {std::byte{0x11}};
        if (::pwrite(fd, seed, 1, 0) != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        std::byte buf[1]{};
        Completion<std::size_t> c;
        if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        if (!wait_paused(gate, deadline)) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        auto h = backend.handle_for_completion_for_test(&c);
        if (!h.has_value()) std::_Exit(sluice_death_test::kChildTestFailExit);
        auto obs = backend.observe_for_test(*h);
        if (!obs.has_value() || obs->state != detail::RequestState::enqueued) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (backend.dispatch_size_for_test() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        // backend destroyed here while dispatch_ is non-empty -> fail-fast.
        ::close(fd);
    }

    // Destructor did NOT fail-fast; the regression.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 2) Destroy with a running worker: the worker is paused after mark_running.
void child_destroy_with_running() {
    sluice_death_test::install_deterministic_terminate_handler();

    {
        ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
        ThreadPoolBackend::WorkerRunningPauseGate gate;
        backend.set_running_pause_gate(&gate);

        TempPath tp("running");
        int fd = open_temp(tp.path());
        const std::byte seed[1] = {std::byte{0x22}};
        if (::pwrite(fd, seed, 1, 0) != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        std::byte buf[1]{};
        Completion<std::size_t> c;
        if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        if (!wait_paused(gate, deadline)) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        if (backend.active_workers_for_test() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        auto h = backend.handle_for_completion_for_test(&c);
        if (!h.has_value()) std::_Exit(sluice_death_test::kChildTestFailExit);
        auto obs = backend.observe_for_test(*h);
        if (!obs.has_value() || obs->state != detail::RequestState::running) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        // backend destroyed here while active_workers_ != 0 -> fail-fast.
        ::close(fd);
    }

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 3) Destroy with a backend-ready terminal that has not been reaped.
void child_destroy_with_backend_ready() {
    sluice_death_test::install_deterministic_terminate_handler();

    {
        ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});

        TempPath tp("backend_ready");
        int fd = open_temp(tp.path());
        const std::byte seed[1] = {std::byte{0x33}};
        if (::pwrite(fd, seed, 1, 0) != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        std::byte buf[1]{};
        Completion<std::size_t> c;
        if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        for (;;) {
            if (backend.active_workers_for_test() == 0 &&
                backend.backend_ready_count_for_test() == 1 &&
                !c.ready()) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                std::_Exit(sluice_death_test::kChildTestFailExit);
            }
            std::this_thread::yield();
        }

        // backend destroyed here while backend_ready != 0 -> fail-fast.
        ::close(fd);
    }

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 4) Destroy with a completion-ready Completion that the caller never reset.
//
// Completion must be declared BEFORE the backend so that the backend destructor
// runs while the Completion is still alive and unreset. If Completion were
// destroyed first, its destructor would reset/release the slot, leaving the
// backend destructor to see a quiescent arena and not fail-fast (the intended
// scenario is the caller error of destroying the backend before the bound
// Completion).
void child_destroy_with_completion_ready() {
    sluice_death_test::install_deterministic_terminate_handler();

    std::byte buf[1]{};
    Completion<std::size_t> c;

    {
        ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});

        TempPath tp("completion_ready");
        int fd = open_temp(tp.path());
        const std::byte seed[1] = {std::byte{0x44}};
        if (::pwrite(fd, seed, 1, 0) != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (!c.ready()) {
            auto wr = backend.wait_one();
            if (!wr.has_value()) std::_Exit(sluice_death_test::kChildTestFailExit);
            if (std::chrono::steady_clock::now() >= deadline) {
                std::_Exit(sluice_death_test::kChildTestFailExit);
            }
        }
        if (!c.ready()) std::_Exit(sluice_death_test::kChildTestFailExit);
        std::fprintf(stderr, "[debug death4] before destroy: slot_in_use=%zu outstanding=%zu backend_ready=%zu\n",
                     backend.arena_slot_in_use(), backend.outstanding(), backend.backend_ready_count_for_test());
        if (backend.arena_slot_in_use() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        // backend destroyed here while slot_in_use != 0 -> fail-fast.
        ::close(fd);
    }

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 5) Control: quiescent destroy after close_admission + drain + reset.
void child_control_quiescent_destroy() {
    {
        ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});

        TempPath tp("control");
        int fd = open_temp(tp.path());
        const std::byte seed[1] = {std::byte{0x55}};
        if (::pwrite(fd, seed, 1, 0) != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        std::byte buf[1]{};
        Completion<std::size_t> c;
        if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        backend.close_admission();
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (backend.outstanding() > 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::_Exit(sluice_death_test::kChildTestFailExit);
            }
            if (backend.poll() == 0) {
                auto wr = backend.wait_one();
                if (!wr.has_value()) std::_Exit(sluice_death_test::kChildTestFailExit);
            }
        }
        if (!c.ready()) std::_Exit(sluice_death_test::kChildTestFailExit);
        c.reset();
        if (backend.arena_slot_in_use() != 0) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }

        // backend destroyed here cleanly.
        ::close(fd);
    }

    std::_Exit(0);
}

SLUICE_TEST_CASE(tp_death_destroy_with_enqueued) {
    auto r = sluice_death_test::run_death_case("enqueued");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying with an enqueued op must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(tp_death_destroy_with_running) {
    auto r = sluice_death_test::run_death_case("running");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying with a running worker must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(tp_death_destroy_with_backend_ready) {
    auto r = sluice_death_test::run_death_case("backend-ready");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying with backend-ready unreaped must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(tp_death_destroy_with_completion_ready) {
    auto r = sluice_death_test::run_death_case("completion-ready");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying with completion-ready unreset must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(tp_death_control_quiescent_destroy) {
    auto r = sluice_death_test::run_death_case("control");
    SLUICE_CHECK_MSG(sluice_death_test::expect_normal_exit_zero(r),
                     "quiescent destroy after close_admission + drain + reset must exit 0");
}

int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        if (child_case == "enqueued") {
            child_destroy_with_enqueued();
        } else if (child_case == "running") {
            child_destroy_with_running();
        } else if (child_case == "backend-ready") {
            child_destroy_with_backend_ready();
        } else if (child_case == "completion-ready") {
            child_destroy_with_completion_ready();
        } else if (child_case == "control") {
            child_control_quiescent_destroy();
        } else {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    return sluice_test::run_all();
}

#else  // !defined(__unix__)

SLUICE_TEST_CASE(tp_death_skip_non_posix) {
    // Death tests require POSIX fork/exec.
}
SLUICE_MAIN()

#endif  // defined(__unix__)
