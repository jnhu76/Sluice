// TAX-0 router-fix candidate shootout (#255) — R3 fail-fast death gates.
//
// POSIX-only (fork/exec/waitpid via death_test_runner_posix.hpp). Each child
// re-execs this binary, installs the deterministic terminate handler, and
// provokes one R3-bounded-table / mode-seam IMPOSSIBLE internal state:
//
//   table-duplicate-insert    insert of an already-present cookie
//   table-erase-absent        erase of a cookie the table does not hold
//   table-insert-zero         insert of cookie 0 (outside key domain [1, 2^63-1])
//   table-erase-zero          erase of cookie 0 (outside key domain)
//   mode-switch-nonquiescent  router-fix mode switch with live router entries
//
// Expected: exit 86 (fail-fast fired) for every case above; the control
// case (quiescent mode switch) exits 0 from the normal parent run.
//
// These gates prove the R3 candidate's fail-fast discipline: deterministic
// named termination for impossible internal states, NEVER silent repair,
// NEVER a wrong-resolution fallback (a desynced table must crash loudly,
// not route a stale CQE to a wrong request). Unreachable through the
// production install/retire pairing by construction — which is exactly why
// the gates drive the table seam directly.
#include "harness.hpp"
#include "death_test_runner_posix.hpp"

#if defined(__unix)

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include <sluice/async/uring_backend.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include <vector>

using namespace sluice::async;

namespace {

// Bare-table cases: no backend state involved.
void child_table_duplicate_insert() {
    sluice_death_test::install_deterministic_terminate_handler();
    RouterCookieTableForTest table{8};
    table.insert(11, 0);
    table.insert(11, 1); // duplicate: impossible via the install pairing
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

void child_table_erase_absent() {
    sluice_death_test::install_deterministic_terminate_handler();
    RouterCookieTableForTest table{8};
    table.insert(11, 0);
    table.erase(12); // absent: impossible via the retire pairing
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

void child_table_insert_zero() {
    sluice_death_test::install_deterministic_terminate_handler();
    RouterCookieTableForTest table{8};
    table.insert(0, 0); // 0 is the empty-slot sentinel / outside the domain
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

void child_table_erase_zero() {
    sluice_death_test::install_deterministic_terminate_handler();
    RouterCookieTableForTest table{8};
    table.erase(0);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// Mode-seam case: the mode switch is a fresh-backend operation; a live
// router entry at switch time is an invariant violation (the placement
// change would silently orphan the live set's physical assumption).
void child_mode_switch_nonquiescent() {
    sluice_death_test::install_deterministic_terminate_handler();
    UringAsyncBackend backend{UringConfig{8, 8}};
    if (!backend.available())
        std::_Exit(0); // no ring: the seam case is unreachable, not failed
    char path[] = "/tmp/sluice_rfix_death_XXXXXX";
    int fd = ::mkstemp(path);
    if (fd >= 0)
        (void)::unlink(path);
    std::byte buf[8]{std::byte{0x5A}};
    Completion<std::size_t> c;
    if (!backend.submit_write(WriteOp{fd, buf, 8, 0}, c).has_value())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    // outstanding > 0 here: the switch MUST terminate.
    backend.set_router_fix_mode_for_test(
        UringAsyncBackend::RouterFixModeForTest::bounded_cookie_table);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

} // namespace

// ---------------------------------------------------------------------------
// Parent cases
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_router_fix_death_table_duplicate_insert) {
    auto r = sluice_death_test::run_death_case("table-duplicate-insert");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "duplicate insert must fail-fast (86)");
}

SLUICE_TEST_CASE(uring_router_fix_death_table_erase_absent) {
    auto r = sluice_death_test::run_death_case("table-erase-absent");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "erase of absent cookie must fail-fast (86)");
}

SLUICE_TEST_CASE(uring_router_fix_death_table_insert_zero) {
    auto r = sluice_death_test::run_death_case("table-insert-zero");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "insert of cookie 0 must fail-fast (86)");
}

SLUICE_TEST_CASE(uring_router_fix_death_table_erase_zero) {
    auto r = sluice_death_test::run_death_case("table-erase-zero");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "erase of cookie 0 must fail-fast (86)");
}

SLUICE_TEST_CASE(uring_router_fix_death_mode_switch_nonquiescent) {
    auto r = sluice_death_test::run_death_case("mode-switch-nonquiescent");
    SLUICE_CHECK_MSG(sluice_death_test::expect_terminated_via_fail_fast(r),
                     "mode switch with live entries must fail-fast (86)");
}

// Control: a quiescent switch is legal (exercised extensively by the
// equivalence suite); this case proves the death harness itself passes a
// healthy child through. The quiescent switch exits 0 via the normal path.
SLUICE_TEST_CASE(uring_router_fix_death_control) {
    UringAsyncBackend backend{UringConfig{8, 8}};
    if (!backend.available())
        return;
    backend.set_router_fix_mode_for_test(
        UringAsyncBackend::RouterFixModeForTest::bounded_cookie_table);
    SLUICE_CHECK(backend.router_fix_mode_for_test() ==
                 UringAsyncBackend::RouterFixModeForTest::bounded_cookie_table);
}

int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        if (child_case == "table-duplicate-insert")
            child_table_duplicate_insert();
        else if (child_case == "table-erase-absent")
            child_table_erase_absent();
        else if (child_case == "table-insert-zero")
            child_table_insert_zero();
        else if (child_case == "table-erase-zero")
            child_table_erase_zero();
        else if (child_case == "mode-switch-nonquiescent")
            child_mode_switch_nonquiescent();
        else
            std::_Exit(sluice_death_test::kChildTestFailExit);
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    return sluice_test::run_all();
}

#else // real-liburing guard

// Stub builds: no ring, no seam. Build/API honesty only.
SLUICE_TEST_CASE(uring_router_fix_death_stub_build_compile) {}

int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty())
        std::_Exit(sluice_death_test::kChildTestFailExit);
    return sluice_test::run_all();
}

#endif // defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)

#else // !defined(__unix)

SLUICE_TEST_CASE(uring_router_fix_death_skip_non_posix) {
    // Death tests require POSIX fork/exec.
}

SLUICE_MAIN()

#endif // defined(__unix)
