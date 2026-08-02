// ADR-explicit-io-completion-authority — Completion publication authority
// death tests. Verifies that forbidden state transitions fail-fast
// (std::terminate, exit 86) in BOTH Debug and Release:
//
//   1. reset-outstanding: reset() on an outstanding Completion
//   2. destroy-outstanding: destroying an outstanding Completion
//   3. double-publish: publishing to a Completion that is already ready
//   4. concurrent-publish: two threads publishing one outstanding Completion —
//      exactly one wins the publish CAS, the loser MUST fail-fast (no
//      concurrent storage write)
//
// Also includes a control case (valid lifecycle + idle-reset no-op) that must
// exit 0, and a two-thread concurrent-claim test (exactly one wins).
//
// POSIX only (fork/exec/waitpid). Gated to linux/macOS.
#include "harness.hpp"
#include "death_test_runner_posix.hpp"

#if defined(__unix__)

#include "support/probe_backend.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdlib>
#include <thread>

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

    // AC-13 (as amended): reset() from idle is an IDEMPOTENT NO-OP, not a
    // fail-fast. op_helpers relies on the defensive first-iteration reset.
    c.reset();
    if (!c.idle()) std::_Exit(sluice_death_test::kChildTestFailExit);

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

// ---- Child case: two threads publish the same outstanding Completion
// concurrently. Exactly one wins the publish CAS (single-winner); the loser
// MUST fail-fast (std::terminate → exit 86). A load-then-store publish would
// let both threads race storage_, so this death test is the runtime proof that
// the CAS publishing state exists. Barrier-released, no sleeps.
void child_concurrent_publish() {
    sluice_death_test::install_deterministic_terminate_handler();

    ProbeBackend pb;
    Completion<std::size_t> c;
    if (!pb.claim(c)) std::_Exit(sluice_death_test::kChildTestFailExit);
    // c is outstanding. Release both threads at the same instant.
    std::barrier sync{2};
    std::thread a([&] {
        sync.arrive_and_wait();
        pb.publish_completion(c, Result<std::size_t>{std::size_t{1}});
    });
    std::thread b([&] {
        sync.arrive_and_wait();
        pb.publish_completion(c, Result<std::size_t>{std::size_t{2}});
    });
    a.join();
    b.join();
    // Only reachable if BOTH publishes "succeeded" — impossible: the publish
    // CAS is single-winner, so exactly one thread always loses and fail-fasts.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
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
        "Control: valid Completion lifecycle (incl. idle-reset no-op) must exit 0");
}

SLUICE_TEST_CASE(completion_death_concurrent_publish_fail_fast) {
    auto r = sluice_death_test::run_death_case("concurrent-publish");
    SLUICE_CHECK_MSG(
        sluice_death_test::expect_terminated_via_fail_fast(r),
        "two threads publishing one outstanding Completion: the loser must fail-fast (exit 86)");
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

// ---- Non-death regression: claim rollback (ADR §10) restores idle -----------
// The io_uring backend claims BEFORE acquiring an SQE and rolls the claim back
// if SQE acquisition fails; the Completion must be fully reusable afterwards.
SLUICE_TEST_CASE(completion_claim_rollback_returns_to_idle) {
    ProbeBackend pb;
    Completion<std::size_t> c;
    SLUICE_CHECK(pb.claim(c));
    SLUICE_CHECK(c.outstanding());
    pb.rollback_claim(c);
    SLUICE_CHECK(c.idle());
    // Fully reusable: can claim again and complete normally.
    SLUICE_CHECK(pb.claim(c));
    pb.publish_completion(c, Result<std::size_t>{std::size_t{7}});
    SLUICE_CHECK(c.result().value() == 7);
    c.reset();
}

// ---- Non-death concurrency regression: two threads claim the same Completion
// concurrently — exactly one wins (ADR §6), never a double claim. Barrier-
// released; no sleeps.
SLUICE_TEST_CASE(completion_concurrent_claim_exactly_one_wins) {
    ProbeBackend pb;
    Completion<std::size_t> c;
    std::atomic<int> winners{0};
    std::barrier sync{2};
    std::thread a([&] {
        sync.arrive_and_wait();
        if (pb.claim(c)) winners.fetch_add(1, std::memory_order_relaxed);
    });
    std::thread b([&] {
        sync.arrive_and_wait();
        if (pb.claim(c)) winners.fetch_add(1, std::memory_order_relaxed);
    });
    a.join();
    b.join();
    SLUICE_CHECK_MSG(winners.load(std::memory_order_relaxed) == 1,
                     "exactly one concurrent claim may succeed (CAS single-winner)");
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
        } else if (child_case == "concurrent-publish") {
            child_concurrent_publish();
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
