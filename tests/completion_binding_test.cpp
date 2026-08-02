// Phase B reference lifecycle — Completion binding transient tests.
//
// ADR-explicit-io-request-contract (Accepted) Decision 5 / I2 / I15:
// the accepted request lifecycle splits the single idle -> outstanding claim
// into a private two-stage
//
//   idle --CAS--> binding --release-store--> outstanding
//
// so the winning backend can install RequestKey / ContextIdentity / slot-release
// capability before the Completion becomes observable as outstanding. While in
// `binding`, the Completion is NOT outstanding: cancel / await / waiter-
// registration reject synchronously, and reset / destruction fail-fast.
//
// These tests drive the binding protocol directly via ProbeBackend (the same
// authorized driver used by completion_authority_death_test). The full
// five-stage submit path against the migrated reference backends is exercised
// in tests/request_lifecycle_scheme_b_test.cpp (commit 3).
#include "harness.hpp"
#include "support/probe_backend.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <barrier>
#include <cstddef>
#include <thread>

using namespace sluice::async;
using sluice::Result;

SLUICE_MAIN()

// ---- Slice 1: binding CAS elects exactly one context; loser cannot win ------
// I2: the idle -> binding CAS elects exactly one submitting context. A second
// concurrent begin_binding on the same Completion returns false; the loser does
// NOT observe the winner's binding window and must roll back only its own
// candidate slot (out of scope for this direct-Completion test).
SLUICE_TEST_CASE(binding_cas_elects_one_context) {
    ProbeBackend pb;
    Completion<std::size_t> c;

    SLUICE_CHECK(c.idle());
    SLUICE_CHECK(pb.begin_binding(c));   // winner: idle -> binding
    // While binding, the Completion is NOT outstanding (I15): cancel/await paths
    // gate on outstanding() and must observe not-yet-accepted.
    SLUICE_CHECK(!c.outstanding());
    SLUICE_CHECK(!c.idle());
    SLUICE_CHECK(!c.ready());

    // A second begin_binding loses the CAS and returns false.
    SLUICE_CHECK(!pb.begin_binding(c));

    // Winner commits: binding -> outstanding. This is the submit-success
    // linearization point; an acquire observer now sees outstanding.
    pb.commit_binding(c);
    SLUICE_CHECK(c.outstanding());
    SLUICE_CHECK(!c.idle());

    // Clean teardown so the destructor does not fail-fast.
    pb.publish_completion(c, Result<std::size_t>{std::size_t{1}});
    c.reset();
}

// ---- Slice 1b: binding rollback restores idle (winner that fails pre-commit) -
// A winner that fails between begin_binding and commit (e.g. prepare-stage
// validation) rolls the binding back to idle. The Completion is then fully
// reusable — a later claim/publish cycle works normally.
SLUICE_TEST_CASE(binding_rollback_restores_idle) {
    ProbeBackend pb;
    Completion<std::size_t> c;

    SLUICE_CHECK(pb.begin_binding(c));
    SLUICE_CHECK(!c.idle());
    SLUICE_CHECK(!c.outstanding());

    pb.rollback_binding(c);
    SLUICE_CHECK(c.idle());   // fully reusable

    // A normal lifecycle still works after rollback.
    SLUICE_CHECK(pb.begin_binding(c));
    pb.commit_binding(c);
    SLUICE_CHECK(c.outstanding());
    pb.publish_completion(c, Result<std::size_t>{42});
    SLUICE_CHECK(c.result().value() == 42);
    c.reset();
}

// ---- Slice 2: concurrent binding elects exactly one winner ------------------
// Two threads race begin_binding on the same idle Completion. Exactly one wins
// (CAS single-winner); the loser returns false and never enters the binding
// window. Barrier-released, no sleeps. Mirrors the concurrent-claim regression
// in completion_authority_death_test but for the new binding path.
SLUICE_TEST_CASE(concurrent_binding_exactly_one_wins) {
    ProbeBackend pb;
    Completion<std::size_t> c;
    std::atomic<int> winners{0};
    std::barrier sync{2};
    std::thread a([&] {
        sync.arrive_and_wait();
        if (pb.begin_binding(c)) winners.fetch_add(1, std::memory_order_relaxed);
    });
    std::thread b([&] {
        sync.arrive_and_wait();
        if (pb.begin_binding(c)) winners.fetch_add(1, std::memory_order_relaxed);
    });
    a.join();
    b.join();
    SLUICE_CHECK_MSG(winners.load(std::memory_order_relaxed) == 1,
                     "exactly one concurrent begin_binding may win (CAS single-winner)");
    // The one winner holds the binding; the Completion is not outstanding yet.
    SLUICE_CHECK(!c.outstanding());
    // Clean teardown if and only if a winner exists (guard avoids masking the
    // CHECK above by fail-fast on an idle Completion).
    if (winners.load(std::memory_order_relaxed) == 1) {
        pb.commit_binding(c);
        pb.publish_completion(c, Result<std::size_t>{std::size_t{0}});
        c.reset();
    }
}
