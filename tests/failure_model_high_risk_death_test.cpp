// failure_model_high_risk_death_test — PR-D (#135 Case B) death-test
// obligations for the ScriptedAsyncBackend fail-closed guards.
//
// The #147 policy (AGENTS.md §9.2, docs/architecture/failure-model.md)
// requires a T3/T6 no-channel fail-fast to be a NAMED fail-fast, active in
// Debug AND Release, and DEATH-TESTED. The two guards live in
// tests/support/scripted_async_backend.cpp:
//
//   SB-A  constructing the backend with a null shared state (T3) must
//         terminate via scripted_backend_state_fail_fast.
//   SB-B  destroying the backend while scripted operations are still
//         outstanding (T6 — lifetime violation, per failure-model.md §6)
//         must terminate via
//         scripted_backend_non_quiescent_destruction_fail_fast.
//   SB-C  control: the same entries in a clean construct → submit →
//         complete → drain → destroy lifecycle must exit 0 (the guards are
//         inert on a legitimate lifecycle).
//
// This binary has its OWN int main(int, char**) (NOT SLUICE_MAIN): the
// cooperative harness cannot survive a std::terminate in-process, so each
// case runs in a forked child that re-execs this binary
// (death_test_runner_posix.hpp). The child installs a deterministic
// terminate handler and the parent asserts the exact exit code (86).
//
// Real object path, not a fail-fast call: SB-A reaches the authority from
// the real ScriptedAsyncBackend constructor; SB-B submits a REAL read
// through the backend's submit entry (so the shared state genuinely holds an
// accepted operation) and then runs the REAL destructor; neither child ever
// names the fail-fast function directly. Attribution is by construction:
// the only terminate boundary each child body can reach is the authority
// under test, and kUnexpectedReturnExit (87) marks a guard that failed to
// fire. Both cases are deterministic (single-threaded children, no timers,
// no cross-thread scheduling); they must pass in Debug AND Release because
// the guards are explicit control flow, not asserts.
//
// POSIX-only (fork/exec/waitpid): the xmake target is gated to
// linux/macosx; see death_test_runner_posix.hpp.
#include "death_test_runner_posix.hpp"

#if defined(__unix__)
#include "support/scripted_async_backend.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>

#include <cstdio>
#include <memory>
#include <string>

namespace {

using sluice::async::AsyncIoContext;
using sluice::async::Completion;
using sluice::async::make_scripted_backend;

// --------------------------------------------------------------------------
// Child-mode bodies.
// --------------------------------------------------------------------------

// SB-A — null shared state at construction (T3). The real constructor is
// the only entry; the named authority is reached from it, never directly.
void child_sb_a_null_state() {
    std::shared_ptr<sluice::async::ScriptedBackendSharedState> null_state;
    sluice::async::ScriptedAsyncBackend backend{null_state};
    // MUST terminate inside the constructor; reaching here is the failure
    // mode this regression exists to catch.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// SB-B — non-quiescent destruction (T6). Submit a REAL read through the
// backend (accepted, pending — nothing completes or drains it), then run
// the REAL destructor. Destroying the backend explicitly (unique_ptr reset)
// keeps attribution single: if the guard were removed, control returns and
// the child exits 87 before the still-outstanding Completion could hit a
// different authority.
void child_sb_b_non_quiescent_destruction() {
    auto pair = make_scripted_backend();
    std::byte buf[8]{};
    Completion<std::size_t> c;
    const auto sub = pair.backend->submit_read(
        sluice::async::ReadOp{0, buf, sizeof(buf), 0}, c);
    if (!sub.has_value()) {
        std::fprintf(stderr, "[death] SB-B child: unexpected submit "
                             "rejection in setup\n");
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    pair.backend.reset();  // MUST terminate via the T6 authority; dtor path.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// SB-C — control. Clean lifecycle: construct, submit a real read, complete
// it through the controller (the supported test-thread control plane), poll
// the staged result into the Completion, take the result, reset the
// Completion (releases the slot), then let scope exit destroy the context
// (and with it the backend) quiescently. Declaration order matters exactly
// as in scripted_backend_test: the Completion is ready and reset before the
// context destructs, so the T6 guard must observe an empty state and stay
// silent. Only AFTER the full clean teardown does the caller _Exit(0); a
// guard that false-fires here terminates with 86 and the parent FAILs.
void child_sb_c_control() {
    auto pair = make_scripted_backend();
    AsyncIoContext ctx(std::move(pair.backend));
    auto& ctrl = pair.controller;
    std::byte buf[8]{};
    Completion<std::size_t> c;
    const auto sub = ctx.submit_read(
        sluice::async::ReadOp{0, buf, sizeof(buf), 0}, c);
    if (!sub.has_value()) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    const std::byte data[4]{std::byte{1}, std::byte{2}, std::byte{3},
                            std::byte{4}};
    ctrl.complete_read_with_data(1, data, sizeof(data));  // id 1 = 1st submit
    (void)ctx.poll();
    if (!c.ready() || !c.result().has_value() || c.result().value() != 4) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    c.reset();
    ctrl.expect_no_outstanding();
    // Scope exit: c (already reset), buf, ctx — whose backend destructor
    // must NOT terminate — then pair.
}

// dispatch_child never returns: every known case calls _Exit (either via the
// terminate handler's 86, or an explicit failure/success code). SB-C's body
// returns only after its real quiescent teardown completed silently.
void dispatch_child(const std::string& case_name) {
    if (case_name == "SB-A") child_sb_a_null_state();
    if (case_name == "SB-B") child_sb_b_non_quiescent_destruction();
    if (case_name == "SB-C") {
        child_sb_c_control();
        std::_Exit(0);
    }
    std::fprintf(stderr, "[death] unknown child case: %s\n", case_name.c_str());
    std::_Exit(sluice_death_test::kChildTestFailExit);
}

// --------------------------------------------------------------------------
// Parent mode.
// --------------------------------------------------------------------------

int run_parent() {
    using sluice_death_test::DeathResult;
    using sluice_death_test::expect_normal_exit_zero;
    using sluice_death_test::expect_terminated_via_fail_fast;
    using sluice_death_test::run_death_case;

    bool ok = true;

    // T3 / T6 fail-fast cases: MUST terminate (exit 86) in BOTH modes.
    for (const char* name : {"SB-A", "SB-B"}) {
        const DeathResult r = run_death_case(name);
        ok = expect_terminated_via_fail_fast(r) && ok;
    }

    // Control: clean lifecycle MUST exit 0.
    {
        const DeathResult r = run_death_case("SB-C");
        ok = expect_normal_exit_zero(r) && ok;
    }

    if (!ok) {
        std::fprintf(stderr, "failure_model_high_risk_death_test: FAILED\n");
        return 1;
    }
    std::printf("failure_model_high_risk_death_test: all death cases passed "
                "(SB-A/SB-B terminated via fail-fast, SB-C control exit 0)\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        sluice_death_test::install_deterministic_terminate_handler();
        dispatch_child(child_case);
    }
    return run_parent();
}

#else   // !defined(__unix__)

int main() {
    std::printf("failure_model_high_risk_death_test: NOT RUN on this platform "
                "(POSIX fork/exec harness; target is platform-gated)\n");
    return 0;
}

#endif  // __unix__
