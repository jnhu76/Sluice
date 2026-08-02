// ADR-explicit-io-completion-authority — Completion publication authority
// death tests. Verifies that forbidden state transitions fail-fast
// (std::terminate, exit 86) in BOTH Debug and Release:
//
//   1. reset-outstanding: reset() on an outstanding Completion
//   2. destroy-outstanding: destroying an outstanding Completion
//   3. double-publish: publishing to a Completion that is already ready
//
// Also includes a control case (valid lifecycle) that must exit 0.
//
// POSIX only (fork/exec/waitpid). Gated to linux/macOS.
#include "harness.hpp"
#include "death_test_runner_posix.hpp"

#if defined(__unix__)

#include "support/probe_backend.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdlib>

using namespace sluice::async;
using sluice::Result;

// ---- Child case: reset on outstanding Completion MUST fail-fast -------------
void child_reset_outstanding() {
    sluice_death_test::install_deterministic_terminate_handler();

    ProbeBackend pb;
    Completion<std::size_t> c;
    if (!pb.claim(c)) std::_Exit(sluice_death_test::kChildTestFailExit);
    // c is now outstanding. reset() from outstanding is forbidden.
    c.reset();
    // If we reach here, the fail-fast did NOT fire.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ---- Child case: destroying an outstanding Completion MUST fail-fast --------
void child_destroy_outstanding() {
    sluice_death_test::install_deterministic_terminate_handler();

    ProbeBackend pb;
    {
        Completion<std::size_t> c;
        if (!pb.claim(c)) std::_Exit(sluice_death_test::kChildTestFailExit);
        // c goes out of scope while still outstanding → fail-fast.
    }
    // If we reach here, the destructor did NOT fail-fast.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ---- Child case: double-publish MUST fail-fast ------------------------------
void child_double_publish() {
    sluice_death_test::install_deterministic_terminate_handler();

    ProbeBackend pb;
    Completion<std::size_t> c;
    if (!pb.claim(c)) std::_Exit(sluice_death_test::kChildTestFailExit);
    pb.publish_completion(c, Result<std::size_t>{std::size_t{42}});
    if (!c.ready()) std::_Exit(sluice_death_test::kChildTestFailExit);
    // Second publish on a ready Completion is forbidden.
    pb.publish_completion(c, Result<std::size_t>{std::size_t{99}});
    // If we reach here, the fail-fast did NOT fire.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ---- Child case: control — valid lifecycle MUST exit 0 ----------------------
void child_control_valid_lifecycle() {
    ProbeBackend pb;
    Completion<std::size_t> c;

    // idle → outstanding → ready → idle → outstanding → ready
    if (!pb.claim(c)) std::_Exit(sluice_death_test::kChildTestFailExit);
    pb.publish_completion(c, Result<std::size_t>{std::size_t{1}});
    if (!c.ready()) std::_Exit(sluice_death_test::kChildTestFailExit);
    c.reset();
    if (!c.idle()) std::_Exit(sluice_death_test::kChildTestFailExit);
    if (!pb.claim(c)) std::_Exit(sluice_death_test::kChildTestFailExit);
    pb.publish_completion(c, Result<std::size_t>{std::size_t{2}});
    if (!c.ready()) std::_Exit(sluice_death_test::kChildTestFailExit);
    if (c.result().value() != 2) std::_Exit(sluice_death_test::kChildTestFailExit);
    c.reset();
    // Clean destroy (idle).
    std::_Exit(0);
}

// ---- Parent-side test cases -------------------------------------------------

SLUICE_TEST_CASE(completion_death_reset_outstanding) {
    auto r = sluice_death_test::run_death_case("reset-outstanding");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "reset() on an outstanding Completion must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(completion_death_destroy_outstanding) {
    auto r = sluice_death_test::run_death_case("destroy-outstanding");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "destroying an outstanding Completion must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(completion_death_double_publish) {
    auto r = sluice_death_test::run_death_case("double-publish");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "double-publish to a ready Completion must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(completion_death_control_valid_lifecycle) {
    auto r = sluice_death_test::run_death_case("control");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_normal_exit_zero(r),
        "Control: valid Completion lifecycle must exit 0");
}

// ---- Non-death regression: double-claim returns false (no fail-fast) --------

SLUICE_TEST_CASE(completion_double_claim_returns_false) {
    ProbeBackend pb;
    Completion<std::size_t> c;
    SLUICE_CHECK(pb.claim(c));
    // Second claim on an outstanding Completion: CAS fails, returns false.
    SLUICE_CHECK(!pb.claim(c));
    // Still outstanding, not corrupted.
    SLUICE_CHECK(c.outstanding());
    // Clean up: publish so destructor doesn't fail-fast.
    pb.publish_completion(c, Result<std::size_t>{std::size_t{0}});
}

// Child dispatch entry point.
int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        if (child_case == "reset-outstanding") {
            child_reset_outstanding();
        } else if (child_case == "destroy-outstanding") {
            child_destroy_outstanding();
        } else if (child_case == "double-publish") {
            child_double_publish();
        } else if (child_case == "control") {
            child_control_valid_lifecycle();
        } else {
            std::cerr << "[death] unknown child case: " << child_case << "\n";
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    return sluice_test::run_all();
}

#else  // !defined(__unix__)

SLUICE_TEST_CASE(completion_death_skip_non_posix) {
    // Death tests require POSIX fork/exec.
}
SLUICE_MAIN()

#endif  // defined(__unix__)
