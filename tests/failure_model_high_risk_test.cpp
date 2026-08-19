// failure_model_high_risk_test — PR-D (#135 Case B) regression evidence.
//
// Covers the three high-risk assert sites reclassified under
// docs/architecture/failure-model.md (PR agent/failure-model-policy):
//
//   1. Uring test seams observing a backend whose ring construction failed
//      (kernel without io_uring, sandbox seccomp): T5 — environment
//      availability, not a programmer error. The seams must return typed
//      results (nullopt / false), never assert (the previous bare asserts
//      were Debug-only and left Release test binaries a null dereference).
//      The cases below are environment-honest: they assert the INVARIANT
//      (availability <=> token present) so the same case proves the nullopt
//      semantics in stub / no-ring environments and the token semantics on
//      a real ring, without environment-conditional branching on anything
//      weaker than available().
//
//   2. SelectResult::index() no-winner Release fallback (Completion L9
//      pattern): Debug keeps the tripwire; Release returns the documented
//      deterministic 0. Observable only under NDEBUG — the same case runs
//      in both modes and checks the fallback exactly where it exists.
//
//   3. ScriptedAsyncBackend fail-closed guards: the positive control is the
//      existing scripted_backend_test binary (clean construct/submit/drain/
//      destroy in both modes). The abort paths cannot run in this
//      cooperative harness; they are verified by the manual probe recorded
//      in the PR description (Debug AND Release abort with the named
//      reason).
#include "async_test_control.hpp"
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/select.hpp>
#include <sluice/async/uring_backend.hpp>

#include <cstdio>

namespace sa = sluice::async;

namespace {

// The uring seam members exist only in liburing builds
// (`#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)`
// in uring_backend.hpp), so both uring cases compile only there; stub builds
// still run the SelectResult case below.
#if defined(SLUICE_HAS_LIBURING)

// BackendWaitToken/epoch seam availability invariant (T5). In EVERY environment:
// a backend that reports available() has a wait source whose token can be
// try-snapshotted; a constructible-but-unavailable backend (stub build, or
// real build where io_uring_queue_init failed) returns typed nullopt.
SLUICE_TEST_CASE(uring_wait_token_seam_typed_without_wait_source) {
    sa::UringAsyncBackend backend{4};
    const auto token = backend.try_wait_token_for_test();
    SLUICE_CHECK(backend.available() == token.has_value());
    if (!backend.available()) {
        std::printf("[evidence-meta] mode=no-wait-source token=nullopt\n");
    } else {
        std::printf("[evidence-meta] mode=real-ring token=present\n");
    }
}

// The blocking epoch observer must return typed `false` (having waited for
// nothing) when there is no wait source, instead of asserting/dererefencing.
// The no-wait-source state is forced deterministically even in liburing
// builds: a queue depth no kernel will satisfy makes io_uring_queue_init
// fail (ENOMEM or EINVAL — both leave the backend
// constructible-but-unavailable with a null wait source), so the T5 paths
// run on every real-liburing machine, not only sandboxed ones. The normal
// blocking-wait semantics on a working ring are exercised by
// phase_g_closeout_uring_test.
SLUICE_TEST_CASE(uring_wait_epoch_seam_false_without_wait_source) {
    sa::UringAsyncBackend forced{sa::UringConfig{4, 0x40000000u}};
    SLUICE_CHECK(!forced.available());
    const auto tok = forced.try_wait_token_for_test();
    SLUICE_CHECK(!tok.has_value());
    const sa::BackendWaitToken observed{};
    SLUICE_CHECK(!forced.wait_epoch_changed_for_test(observed));
}

#endif  // SLUICE_HAS_LIBURING

// SelectResult::index() Release fallback (L9 pattern): under NDEBUG the
// no-winner call returns the documented deterministic 0 (and kind() /
// timer_outcome() keep their existing fallbacks). In Debug the tripwire is
// active, so the same case defers to the Release gate run of this binary;
// the winner path (TestInit) is checked in BOTH modes as the positive
// control.
SLUICE_TEST_CASE(select_result_index_no_winner_fallback) {
#if defined(NDEBUG)
    constexpr sa::SelectResult no_winner{};
    static_assert(!no_winner.has_winner());
    SLUICE_CHECK(no_winner.index() == 0);
    SLUICE_CHECK(no_winner.kind() == sa::SelectKind::event);
    SLUICE_CHECK(no_winner.timer_outcome() == sa::SelectTimerOutcome::fired);
#else
    std::printf("[evidence-meta] debug tripwire active; "
                "fallback checked by the Release run\n");
#endif
    // Positive control (both modes): a real winner reports its index/kind.
    const sa::SelectResult winner{2, sa::SelectKind::timer,
                                  sa::SelectTimerOutcome::fired,
                                  sa::SelectResult::TestInit{}};
    SLUICE_CHECK(winner.has_winner());
    SLUICE_CHECK(winner.index() == 2);
    SLUICE_CHECK(winner.kind() == sa::SelectKind::timer);
    SLUICE_CHECK(winner.timer_outcome() == sa::SelectTimerOutcome::fired);
}

}  // namespace

SLUICE_MAIN()
