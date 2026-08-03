// Phase B — RequestArena fail-fast death tests.
//
// ADR-explicit-io-request-contract (Accepted) Decision 15 / AC-13 :566-572:
// release() is a contract violation (fail-fast in BOTH Debug and Release) when:
//   - the enqueue-in-flight pin is still live (the submit path has not
//     acknowledged its final slot access; reap must not have published)
//   - registration is still open_registered (a stored token/lease has not been
//     consumed by reap or wait-cancel)
//
// These are the runtime guards that make the Phase B reference lifecycle
// safe-by-construction: a backend that mis-sequences its admission cannot
// silently leak a half-installed request or double-deliver a waiter lease. The
// truthful deterministic contract is std::terminate (exit 86 via the death-test
// harness), not a recoverable result.
//
// Also includes a control case (valid release after reap) that must exit 0.
//
// POSIX only (fork/exec/waitpid). Gated to linux/macOS.
#include "death_test_runner_posix.hpp"
#include "harness.hpp"

#if defined(__unix__)

#include <sluice/async/detail/request_arena.hpp>
#include <sluice/result.hpp>

#include <cstdlib>

using sluice::async::detail::ContextIdentity;
using sluice::async::detail::OperationKind;
using sluice::async::detail::RequestArena;
using sluice::async::detail::RoutingLease;
using sluice::async::detail::SlotHandle;
using sluice::async::detail::TerminalResult;
using sluice::async::detail::WaiterToken;

// A no-op publish callback (the death cases exercise the arena release guards,
// not the Completion publish wiring).
static void noop_publish(const RequestArena::ReapPublication&) {}

// ---- Child: release while the enqueue pin is still live MUST fail-fast -------
// Models a mis-sequenced backend that releases a slot before its submit path
// acknowledged the enqueue pin. The pin is set at commit; release before
// acknowledge is a contract violation (reap-ineligible; the op may still be
// racing the submit thread).
void child_release_with_live_pin() {
    sluice_death_test::install_deterministic_terminate_handler();
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    auto rh = arena.reserve();
    if (!rh.has_value())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    SlotHandle h = rh.value();
    if (!arena.prepare(h, OperationKind::read, {}).has_value())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    if (!arena.commit(h).has_value()) // sets the enqueue pin
        std::_Exit(sluice_death_test::kChildTestFailExit);
    // Pin is live. release() MUST fail-fast.
    (void)arena.release(h);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ---- Child: release while a waiter is still registered MUST fail-fast --------
// Models a backend that releases a slot whose waiter registration is still open
// (the token/lease has not been consumed by reap or wait-cancel). A dangling
// routing record would result; fail-fast is the guard.
void child_release_with_registered_waiter() {
    sluice_death_test::install_deterministic_terminate_handler();
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    auto rh = arena.reserve();
    if (!rh.has_value())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    SlotHandle h = rh.value();
    if (!arena.prepare(h, OperationKind::read, {}).has_value())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    if (!arena.commit(h).has_value())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    if (!arena.register_waiter(h, WaiterToken{1, 0, 0}, RoutingLease{7}).has_value())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    arena.acknowledge_enqueue_pin(h); // pin acked, but registration still open
    (void)arena.release(h);           // MUST fail-fast (open registration)
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ---- Child: control — valid release after reap MUST exit 0 -------------------
// Proves the fail-fast above is not a false positive: a slot whose pin was
// acknowledged, whose registration was closed by reap, and that reached
// completion_ready releases cleanly.
void child_control_valid_release() {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    auto rh = arena.reserve();
    if (!rh.has_value())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    SlotHandle h = rh.value();
    if (!arena.prepare(h, OperationKind::read, {}).has_value())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    if (!arena.commit(h).has_value())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    if (!arena.record_terminal(h, TerminalResult::ok_bytes(1)))
        std::_Exit(sluice_death_test::kChildTestFailExit);
    arena.acknowledge_enqueue_pin(h);
    // Reap closes registration and transitions to completion_ready.
    struct NoopSink : sluice::async::detail::SynchronousReadySink {
        void on_ready(sluice::async::detail::ReadyEvent) noexcept override {}
    } sink;
    if (arena.reap(sink, noop_publish) != 1)
        std::_Exit(sluice_death_test::kChildTestFailExit);
    if (!arena.release(h).has_value())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    std::_Exit(0);
}

// ---- Child: enqueue before commit MUST fail-fast -----------------------------
// The Scheme-B arbitration has exactly two legal enqueue outcomes (pending ->
// enqueued, or observing backend_ready -> no-op). Enqueueing a slot that never
// reached commit (reserved/prepared) would silently strand the op; the design
// (§9) classifies it as an invariant violation (fail-fast in BOTH Debug and
// Release).
void child_enqueue_before_commit() {
    sluice_death_test::install_deterministic_terminate_handler();
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    auto rh = arena.reserve();
    if (!rh.has_value())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    SlotHandle h = rh.value();
    if (!arena.prepare(h, OperationKind::read, {}).has_value())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    // No commit: enqueue on a `prepared` slot is an invariant violation.
    (void)arena.enqueue(h);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ---- Child: arena destruction with slot_in_use != 0 MUST fail-fast -----------
// ADR Decision 15: quiescent destruction requires every slot free. Destroying
// the arena (via backend/context destruction) while a slot is still bound —
// e.g. the caller holds a ready Completion it never reset — must terminate in
// BOTH Debug and Release; the Completion-bound release capability must never
// dangle.
void child_destroy_arena_with_slot_in_use() {
    sluice_death_test::install_deterministic_terminate_handler();
    {
        RequestArena arena{ContextIdentity::for_testing(1), 1};
        auto rh = arena.reserve();
        if (!rh.has_value())
            std::_Exit(sluice_death_test::kChildTestFailExit);
        SlotHandle h = rh.value();
        if (!arena.prepare(h, OperationKind::read, {}).has_value())
            std::_Exit(sluice_death_test::kChildTestFailExit);
        // arena goes out of scope with slot_in_use == 1 -> fail-fast.
        (void)h;
    }
    // If we reach here, the arena destructor did NOT fail-fast.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ---- Parent-side test cases -------------------------------------------------

SLUICE_TEST_CASE(arena_death_release_with_live_pin) {
    auto r = sluice_death_test::run_death_case("release-with-live-pin");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "release() while the enqueue-in-flight pin is live must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(arena_death_release_with_registered_waiter) {
    auto r = sluice_death_test::run_death_case("release-with-registered-waiter");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "release() while a waiter is still registered must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(arena_death_control_valid_release) {
    auto r = sluice_death_test::run_death_case("control-valid-release");
    SLUICE_CHECK_MSG(sluice_death_test::expect_normal_exit_zero(r),
                     "Control: valid release after reap must exit 0");
}

SLUICE_TEST_CASE(arena_death_enqueue_before_commit) {
    auto r = sluice_death_test::run_death_case("enqueue-before-commit");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "enqueue() on a slot that never reached commit must fail-fast (exit 86)");
}

SLUICE_TEST_CASE(arena_death_destroy_with_slot_in_use) {
    auto r = sluice_death_test::run_death_case("destroy-arena-with-slot-in-use");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "arena destruction with slot_in_use != 0 must fail-fast (exit 86)");
}

// Child dispatch entry point.
int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        if (child_case == "release-with-live-pin") {
            child_release_with_live_pin();
        } else if (child_case == "release-with-registered-waiter") {
            child_release_with_registered_waiter();
        } else if (child_case == "control-valid-release") {
            child_control_valid_release();
        } else if (child_case == "enqueue-before-commit") {
            child_enqueue_before_commit();
        } else if (child_case == "destroy-arena-with-slot-in-use") {
            child_destroy_arena_with_slot_in_use();
        } else {
            std::cerr << "[death] unknown child case: " << child_case << "\n";
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    return sluice_test::run_all();
}

#else // !defined(__unix__)

SLUICE_TEST_CASE(arena_death_skip_non_posix) {
    // Death tests require POSIX fork/exec.
}
SLUICE_MAIN()

#endif // defined(__unix__)
