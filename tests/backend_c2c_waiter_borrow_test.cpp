// Phase C2c — FakeAsyncBackend borrow / waiter / delivery-lease integration.
//
// Issue #68 rows 11-14 at the REFERENCE-BACKEND integration layer (Layer B):
// these cases prove the REAL Fake submit path produces the same arena borrow
// lifecycle the arena matrix proves, that the waiter seam routes a real
// accepted Completion through the REAL arena register_waiter/cancel_waiter
// authorities (no side-band waiter map, no reimplementation of the waiter
// state machine), and that the production ReferenceReadySink delivers the
// registered waiter's token + lease exactly once at reap.
//
// Every case drives raw FakeAsyncBackend (the public AsyncIoContext access_mtx_
// serialization would hide the poll-gated publication boundary) and links
// sluice_async_internal_testing for the guarded seams.
#include "harness.hpp"

#include "support/waiter_error_vocabulary_cases.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <memory>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;
using sluice::async::detail::WaiterRegistration;
using sluice::async::detail::WaiterToken;

SLUICE_MAIN()

// ---- Row 11/12a/14a: submit borrow exact + waiter delivery exactly-once -----
// The real accepted request carries the submitted fd/address/length with the
// borrow ACTIVE. A registered waiter (token A + lease 99) is delivered by the
// production sink exactly once at poll(): Completion ready, borrow ended
// (acquire observer of ready sees the ended borrow — I18), token A + lease 99
// in the sink's last-delivery observation.
SLUICE_TEST_CASE(fake_borrow_waiter_delivery_integration) {
    FakeAsyncBackend backend{/*request_capacity=*/2};
    std::byte buf[8]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{7, buf, 8, 0}, c).has_value());
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());

    // The real submit path wrote the exact borrow metadata and activated it.
    auto b = backend.borrow_for_test(*h);
    SLUICE_CHECK(b.has_value());
    SLUICE_CHECK(b->fd == 7);
    SLUICE_CHECK(b->address == buf);
    SLUICE_CHECK(b->length == 8);
    SLUICE_CHECK(b->active);
    SLUICE_CHECK(backend.arena_state_is(h->slot.value,
                                        detail::RequestState::enqueued));

    // Register one waiter through the REAL arena authority.
    SLUICE_CHECK(backend.register_waiter_for_test(c, WaiterToken{1, 5, 2},
                                                  detail::RoutingLease{99})
                     .has_value());

    // Terminal helper produces ONLY backend_ready: the Completion is not ready
    // and the borrow is still active before poll().
    backend.complete_oldest_with_bytes(4);
    SLUICE_CHECK(!c.ready());
    {
        auto obs = backend.borrow_for_test(*h);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(obs->active);
    }
    SLUICE_CHECK(backend.sink_deliveries() == 0);

    // poll() reaps: Completion ready; the acquire observer sees the ended
    // borrow; the waiter was delivered exactly once with token A + lease 99.
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().has_value());
    SLUICE_CHECK(c.result().value() == 4);
    {
        auto obs = backend.borrow_for_test(*h);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(!obs->active);  // borrow ended
    }
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    SLUICE_CHECK(backend.sink_last_has_waiter());
    SLUICE_CHECK((backend.sink_last_token() == WaiterToken{1, 5, 2}));
    SLUICE_CHECK(backend.sink_last_lease_id() == 99);

    // Exactly-once: a second poll delivers nothing; a late wait-cancel gets
    // nothing (reap consumed the lease).
    SLUICE_CHECK(backend.poll() == 0);
    auto rl = backend.cancel_waiter_for_test(c);
    SLUICE_CHECK(!rl.has_value());

    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---- Row 12a (Fake): registration in the backend_ready window ---------------
// complete_* produces ONLY backend_ready — the Completion is not ready, no
// sink delivery has run. Per ADR Decision 10, the terminal winner does NOT
// close registration: a waiter registered in this window succeeds and reap
// delivers it exactly once together with the terminal result.
SLUICE_TEST_CASE(fake_backend_ready_window_waiter_registration) {
    FakeAsyncBackend backend{/*request_capacity=*/2};
    std::byte buf[8]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());

    // Terminal recorded, reap not run: not ready, borrow active, sink silent.
    backend.complete_oldest_with_bytes(4);
    SLUICE_CHECK(!c.ready());
    {
        auto obs = backend.borrow_for_test(*h);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(obs->active);
    }
    SLUICE_CHECK(backend.sink_deliveries() == 0);

    // The terminal winner does NOT close registration (ADR Decision 10):
    // registering in this window succeeds and reap delivers the waiter.
    SLUICE_CHECK(backend.register_waiter_for_test(c, WaiterToken{6, 3, 3},
                                                  detail::RoutingLease{88})
                     .has_value());

    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().has_value());
    SLUICE_CHECK(c.result().value() == 4);
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    SLUICE_CHECK(backend.sink_last_has_waiter());
    SLUICE_CHECK((backend.sink_last_token() == WaiterToken{6, 3, 3}));
    SLUICE_CHECK(backend.sink_last_lease_id() == 88);
    SLUICE_CHECK(backend.poll() == 0);  // exactly-once publication
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---- Row 13: wait-cancel removes ONLY the waiter; the I/O still completes ----
// cancel_waiter returns lease A and reopens registration; the accepted I/O
// stays outstanding with its borrow active, no terminal, no canceled tally.
// The I/O then completes normally; the sink delivers NO waiter; lease A never
// reappears.
SLUICE_TEST_CASE(fake_wait_cancel_keeps_io) {
    FakeAsyncBackend backend{/*request_capacity=*/2};
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);
    std::byte buf[8]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());

    SLUICE_CHECK(backend.register_waiter_for_test(c, WaiterToken{2, 0, 0},
                                                  detail::RoutingLease{55})
                     .has_value());
    auto rl = backend.cancel_waiter_for_test(c);
    SLUICE_CHECK(rl.has_value());
    SLUICE_CHECK_MSG(rl.value().id() == 55,
                     "cancel_waiter must return the registered lease");
    auto w = backend.waiter_for_test(*h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_no_waiter);
    SLUICE_CHECK(!w->delivery_present);

    // The I/O is untouched: still accepted/outstanding, borrow active, no
    // terminal, no canceled tally.
    SLUICE_CHECK(backend.outstanding() == 1);
    {
        auto obs = backend.borrow_for_test(*h);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(obs->active);
    }
    SLUICE_CHECK(stats.canceled_ops == 0);
    SLUICE_CHECK(stats.completion_errors == 0);

    // The I/O completes normally and the sink delivers no waiter.
    backend.complete_oldest_with_bytes(8);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 8);
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    SLUICE_CHECK(!backend.sink_last_has_waiter());
    SLUICE_CHECK(stats.canceled_ops == 0);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---- Row 13: I/O cancel keeps the waiter registration -----------------------
// I/O terminal cancellation is NOT waiter cancellation: a cancel that WINS the
// canceled terminal leaves the registered waiter untouched, and reap delivers
// BOTH the canceled result and the waiter token/lease exactly once.
SLUICE_TEST_CASE(fake_io_cancel_keeps_waiter) {
    FakeAsyncBackend backend{/*request_capacity=*/2};
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);
    std::byte buf[8]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());

    SLUICE_CHECK(backend.register_waiter_for_test(c, WaiterToken{3, 1, 1},
                                                  detail::RoutingLease{66})
                     .has_value());
    backend.cancel(c);  // I/O cancel wins the terminal (enqueued, Scheme B)
    SLUICE_CHECK(stats.canceled_ops == 1);

    // The I/O cancel did NOT delete the waiter registration, end the borrow,
    // or publish ready.
    auto w = backend.waiter_for_test(*h);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK(((w->token == WaiterToken{3, 1, 1})));
    SLUICE_CHECK(w->lease_id == 66);
    {
        auto obs = backend.borrow_for_test(*h);
        SLUICE_CHECK(obs.has_value());
        SLUICE_CHECK(obs->active);
    }
    SLUICE_CHECK(!c.ready());

    // Reap: canceled result + waiter delivered together, exactly once.
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::canceled);
    SLUICE_CHECK(backend.sink_deliveries() == 1);
    SLUICE_CHECK(backend.sink_last_has_waiter());
    SLUICE_CHECK((backend.sink_last_token() == WaiterToken{3, 1, 1}));
    SLUICE_CHECK(backend.sink_last_lease_id() == 66);
    SLUICE_CHECK(backend.poll() == 0);  // exactly-once
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---- Row 12a/14a: stale waiter authority cannot touch the N+1 occupant ------
// After release + reuse of the same physical slot, a stale-generation
// handle cannot touch the live N+1 occupant's waiter: register rejects
// invalid_state (provenance misuse, Decision 6 — W1, S0B-CORRECTIVE-1) and
// cancel rejects not_found (benign cancel-lookup miss), both with ZERO side
// effect on the live occupant's registration (token B + lease 200 intact);
// the new occupant still delivers B exactly once at reap.
SLUICE_TEST_CASE(fake_stale_waiter_authority_harmless) {
    FakeAsyncBackend backend{/*request_capacity=*/1};
    std::byte buf[8]{};
    Completion<std::size_t> c;

    // Generation N: full lifecycle with waiter A; capture the handle BEFORE
    // the release (it becomes stale the moment the slot is freed).
    SLUICE_CHECK(backend.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());
    auto h0 = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h0.has_value());
    SLUICE_CHECK(backend.register_waiter_for_test(c, WaiterToken{9, 0, 0},
                                                  detail::RoutingLease{100})
                     .has_value());
    backend.complete_oldest_with_bytes(1);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    c.reset();  // slot freed; generation advances to N+1

    // Generation N+1: the SAME physical slot is reused and registers waiter B.
    SLUICE_CHECK(backend.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());
    auto h1 = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h1.has_value());
    SLUICE_CHECK(h1->slot.value == h0->slot.value);
    SLUICE_CHECK(h1->generation.value == h0->generation.value + 1);
    SLUICE_CHECK(backend.register_waiter_for_test(c, WaiterToken{4, 2, 2},
                                                  detail::RoutingLease{200})
                     .has_value());

    // Inject the STALE N-handle through the REAL waiter authorities: register
    // rejects invalid_state, cancel rejects not_found, both leave B's
    // registration untouched.
    auto stale_cancel = backend.cancel_waiter_handle_for_test(*h0);
    SLUICE_CHECK(!stale_cancel.has_value());
    SLUICE_CHECK(stale_cancel.error().code == IoError::Code::not_found);
    auto stale_register = backend.register_waiter_handle_for_test(
        *h0, WaiterToken{9, 9, 9}, detail::RoutingLease{300});
    SLUICE_CHECK(!stale_register.has_value());
    SLUICE_CHECK(stale_register.error().code == IoError::Code::invalid_state);

    // The live N+1 occupant's registration is untouched.
    auto w = backend.waiter_for_test(*h1);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w->registration == WaiterRegistration::open_registered);
    SLUICE_CHECK(((w->token == WaiterToken{4, 2, 2})));
    SLUICE_CHECK(w->lease_id == 200);

    // The N+1 occupant delivers ITS OWN waiter B exactly once: exactly one
    // delivery per generation (2 total: A in gen-N, B in gen-N+1) and the
    // last-delivery payload is B's token + lease 200.
    backend.complete_oldest_with_bytes(1);
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(backend.poll() == 0);  // exactly-once publication
    SLUICE_CHECK(backend.sink_deliveries() == 2);
    SLUICE_CHECK(backend.sink_last_has_waiter());
    SLUICE_CHECK((backend.sink_last_token() == WaiterToken{4, 2, 2}));
    SLUICE_CHECK(backend.sink_last_lease_id() == 200);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// ---- Row 12a: waiter seam on an unbound / released Completion ----------------
// The seam resolves the Completion through the arena's real identity bridge.
// W1 (S0B-CORRECTIVE-1): an unbound (or already-released) Completion resolves
// nothing -> register_waiter rejects invalid_state (provenance misuse,
// Decision 6) while cancel_waiter keeps not_found (benign cancel-lookup
// miss); no waiter state machine is manufactured by the test.
SLUICE_TEST_CASE(fake_waiter_seam_unbound_completion_error_split) {
    FakeAsyncBackend backend{/*request_capacity=*/1};
    std::byte buf[8]{};
    Completion<std::size_t> c;
    Completion<std::size_t> unbound;

    auto r = backend.register_waiter_for_test(unbound, WaiterToken{1, 0, 0},
                                              detail::RoutingLease{1});
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    auto rl = backend.cancel_waiter_for_test(unbound);
    SLUICE_CHECK(!rl.has_value());
    SLUICE_CHECK(rl.error().code == IoError::Code::not_found);

    // After a full lifecycle + reset, the released Completion resolves nothing.
    SLUICE_CHECK(backend.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());
    backend.complete_oldest_with_bytes(8);
    SLUICE_CHECK(backend.poll() == 1);
    c.reset();
    auto r2 = backend.register_waiter_for_test(c, WaiterToken{1, 0, 0},
                                               detail::RoutingLease{1});
    SLUICE_CHECK(!r2.has_value());
    SLUICE_CHECK(r2.error().code == IoError::Code::invalid_state);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// S0B-CORRECTIVE-1 W1 — the adjudicated register/cancel error-vocabulary
// split (unbound / cross-context / duplicate / post-reap / stale / no-
// registration), driven through the PUBLIC FakeAsyncBackend interface.
SLUICE_TEST_CASE(fake_waiter_error_vocabulary_split) {
    auto rc = waiter_error_vocabulary::run_waiter_error_vocabulary_cases<
        FakeAsyncBackend>(
        [] { return std::make_unique<FakeAsyncBackend>(/*request_capacity=*/4); },
        /*fd=*/0,
        [](FakeAsyncBackend& b, Completion<std::size_t>& c) {
            b.complete_oldest_with_bytes(8);
            return b.poll() == 1;
        });
    if (rc != nullptr) SLUICE_FAIL(rc);
}
