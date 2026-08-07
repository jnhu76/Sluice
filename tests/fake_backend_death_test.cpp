// FakeAsyncBackend non-quiescent destruction fail-fast death tests (Phase C2e,
// Issue #68 row 16; ADR Decision 15).
//
// The reference backend's destructor is `= default`: non-quiescent destruction
// fail-fasts through the arena destructor (~RequestArena -> slot_in_use != 0 ->
// request_arena_destruction_fail_fast) in BOTH Debug and Release. These cases
// prove the reference path through the CONCRETE FakeAsyncBackend type (not
// only the arena in isolation):
//
//   1) destroy with a bound-but-unreaped request   -> fail-fast (exit 86);
//   2) destroy with a completion-ready-but-unreset  -> fail-fast (exit 86);
//      Completion (reaped; the slot is still bound by the caller-owned
//      publication binding — drained != releasable);
//   3) control: close_admission -> complete -> reap -> reset -> destroy
//      exits 0 (quiescent path).
//
// POSIX-only (fork/exec/waitpid via death_test_runner_posix.hpp). Each child
// case declares the Completion BEFORE the backend (outside the backend's
// scope) so the backend/arena destructor is the FIRST fail-fast authority;
// a Completion destroyed first would release its own slot and mask the
// intended non-quiescent-destruction violation (same discipline as
// threadpool_backend_death_test).
#include "harness.hpp"
#include "death_test_runner_posix.hpp"

#if defined(__unix__)

#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// 1) Destroy with a bound request that was never reaped.
void child_destroy_with_unreaped_request() {
    sluice_death_test::install_deterministic_terminate_handler();

    std::byte buf[1]{};
    Completion<std::size_t> c;  // declared BEFORE the backend (see header)

    {
        FakeAsyncBackend backend(/*request_capacity=*/1);
        if (!backend.submit_read(ReadOp{-1, buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (backend.arena_slot_in_use() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (!c.outstanding()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // backend destroyed here while the slot is bound -> arena destructor
        // fail-fast (request_arena_destruction_fail_fast).
    }

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 2) Destroy with a completion-ready-but-unreset Completion. Reap published
// completion-ready but did NOT release the slot: the caller-owned binding
// still holds it, so the backend is not quiescent and destruction fail-fasts.
void child_destroy_with_ready_unreset() {
    sluice_death_test::install_deterministic_terminate_handler();

    std::byte buf[1]{};
    Completion<std::size_t> c;  // declared BEFORE the backend (see header)

    {
        FakeAsyncBackend backend(/*request_capacity=*/1);
        if (!backend.submit_read(ReadOp{-1, buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // Terminalize + reap: the Completion becomes ready, outstanding -> 0.
        backend.complete_oldest_with_error(IoError{IoError::Code::canceled});
        if (backend.poll() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (!c.ready()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // drained != releasable: outstanding is 0 but the slot is still bound.
        if (backend.outstanding() != 0) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (backend.arena_slot_in_use() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // backend destroyed here while a ready-but-unreset Completion still
        // holds the slot -> arena destructor fail-fast.
    }

    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// 3) Control: quiescent destroy after close_admission + drain + reset.
void child_control_quiescent_destroy() {
    std::byte buf[1]{};
    {
        FakeAsyncBackend backend(/*request_capacity=*/1);
        Completion<std::size_t> c;
        if (!backend.submit_read(ReadOp{-1, buf, 1, 0}, c).has_value()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        backend.close_admission();
        if (backend.outstanding() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // close must not retroactively cancel: the accepted request still
        // terminalizes and reaps normally.
        backend.complete_oldest_with_bytes(1);
        if (backend.poll() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (!c.ready()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        if (backend.outstanding() != 0 || backend.arena_slot_in_use() != 1) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        c.reset();  // release the slot
        if (backend.arena_slot_in_use() != 0) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // backend destroyed here cleanly (quiescent).
    }
    std::_Exit(0);
}

SLUICE_TEST_CASE(fake_death_destroy_with_unreaped_request) {
    auto r = sluice_death_test::run_death_case("unreaped");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying a Fake with a bound unreaped request must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(fake_death_destroy_with_ready_unreset) {
    auto r = sluice_death_test::run_death_case("ready-unreset");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "destroying a Fake with a ready-but-unreset Completion must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(fake_death_control_quiescent_destroy) {
    auto r = sluice_death_test::run_death_case("control");
    SLUICE_CHECK_MSG(sluice_death_test::expect_normal_exit_zero(r),
                     "quiescent Fake destroy after close + drain + reset must exit 0");
}

int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        if (child_case == "unreaped") {
            child_destroy_with_unreaped_request();
        } else if (child_case == "ready-unreset") {
            child_destroy_with_ready_unreset();
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

SLUICE_TEST_CASE(fake_death_skip_non_posix) {
    // Death tests require POSIX fork/exec.
}
SLUICE_MAIN()

#endif  // defined(__unix__)