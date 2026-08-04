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

// ---- Slice 3: the release capability makes reset() the slot-release
// handshake (ADR Decision 15 / design §9) --------------------------------
// The binding CAS winner installs the opaque slot-release capability (arena +
// slot handle). reset() on the ready Completion returns the slot to the arena
// (slot_in_use--, generation++) under the leaf slot-lifecycle domain — this is
// the completion_ready -> free transition the design assigns to the caller.
SLUICE_TEST_CASE(binding_release_capability_reset_releases_slot) {
    ProbeBackend pb;
    detail::RequestArena arena{detail::ContextIdentity::for_testing(1), 1};
    Completion<std::size_t> c;

    // Full lifecycle: reserve -> prepare -> install publication binding ->
    // binding CAS -> commit -> install release capability -> commit_binding
    // (LP) -> terminal -> enqueue (acks pin) -> reap.
    auto rh = arena.reserve();
    SLUICE_CHECK(rh.has_value());
    detail::SlotHandle h = rh.value();
    SLUICE_CHECK(arena.prepare(h, detail::OperationKind::read,
                               detail::BorrowMetadata{0, nullptr, 4})
                     .has_value());
    // The slot carries a REAL publication binding (review C2/C3): reap
    // publishes Completion-ready through this thunk inside the leaf domain.
    SLUICE_CHECK(arena.install_publication_binding(h, &c, 4, &ProbeBackend::publish_size_ready)
                     .has_value());
    SLUICE_CHECK(pb.begin_binding(c));
    SLUICE_CHECK(arena.commit(h).has_value());
    pb.install_binding(c, &arena, h);   // only the binding CAS winner installs
    pb.commit_binding(c);               // submit-success linearization point
    SLUICE_CHECK(c.outstanding());
    SLUICE_CHECK(arena.slot_in_use() == 1);

    SLUICE_CHECK(arena.record_terminal(h, detail::TerminalResult::ok_bytes(4)));
    SLUICE_CHECK(arena.enqueue(h) == detail::EnqueueOutcome::terminal_noop);
    struct NoopSink : detail::SynchronousReadySink {
        void on_ready(detail::ReadyEvent) noexcept override {}
    } sink;
    // Reap publishes through the slot binding (inside the leaf domain): the
    // Completion becomes ready; the slot is NOT freed by reap.
    SLUICE_CHECK(arena.reap(sink) == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 4);
    // Reap published completion-ready but did NOT free the slot: it stays
    // bound until the caller's reset handshake (ADR Decision 4).
    SLUICE_CHECK(arena.slot_in_use() == 1);

    c.reset();  // the release capability returns the slot (generation++)
    SLUICE_CHECK(arena.slot_in_use() == 0);
    SLUICE_CHECK(arena.generation_of(h.slot).value == h.generation.value + 1);
    SLUICE_CHECK(c.idle());
}

// ---- Slice 4: ready-Completion destruction also releases the slot -----------
// ADR Decision 15: destroying a ready Completion performs the same allocation-
// free slot release as reset() before the address becomes invalid.
SLUICE_TEST_CASE(binding_release_capability_ready_destruction_releases_slot) {
    ProbeBackend pb;
    detail::RequestArena arena{detail::ContextIdentity::for_testing(1), 1};

    {
        Completion<std::size_t> c;
        auto rh = arena.reserve();
        SLUICE_CHECK(rh.has_value());
        detail::SlotHandle h = rh.value();
        SLUICE_CHECK(arena.prepare(h, detail::OperationKind::write,
                                   detail::BorrowMetadata{0, nullptr, 4})
                         .has_value());
        SLUICE_CHECK(arena.install_publication_binding(h, &c, 4, &ProbeBackend::publish_size_ready)
                         .has_value());
        SLUICE_CHECK(pb.begin_binding(c));
        SLUICE_CHECK(arena.commit(h).has_value());
        pb.install_binding(c, &arena, h);
        pb.commit_binding(c);
        SLUICE_CHECK(arena.record_terminal(h, detail::TerminalResult::ok_bytes(4)));
        SLUICE_CHECK(arena.enqueue(h) == detail::EnqueueOutcome::terminal_noop);
        struct NoopSink : detail::SynchronousReadySink {
            void on_ready(detail::ReadyEvent) noexcept override {}
        } sink;
        SLUICE_CHECK(arena.reap(sink) == 1);
        SLUICE_CHECK(c.ready());
        SLUICE_CHECK(arena.slot_in_use() == 1);
        // c goes out of scope at ready: the destructor releases the slot.
    }
    SLUICE_CHECK(arena.slot_in_use() == 0);
}

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
