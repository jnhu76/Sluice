// external_backend_admission_test.cpp
//
// Phase C1 — external/custom backend ADMISSION probe (NOT a conformance test).
//
// This file proves ONLY that a legitimate backend subclass can be built from
// the PUBLIC extension surface and admitted into an AsyncIoContext. It is NOT
// a conformance witness: MinimalExternalBackend implements no real or
// deterministic read/write/cancel/reap semantics, so it is deliberately NOT
// driven through the shared observable-semantics suite.
//
// Header policy (review fix #1): this translation unit includes ONLY installed
// public headers. In particular it does NOT include any
// <sluice/async/detail/*> header: the RequestArena/RequestSlot lifecycle is a
// separate evidence layer and must not be conflated with the external backend
// extension admission contract. What this test may reach is exactly what a
// real out-of-tree backend author gets:
//   - subclass AsyncBackend;
//   - implement every pure virtual;
//   - inherit the protected publication helpers (try_claim / publish /
//     rollback_claim_before_accept / begin_binding / commit_binding / ...);
//   - hand an instance to AsyncIoContext via std::unique_ptr<AsyncBackend>.
//
// Proven properties:
//   (1) A self-contained subclass compiles against the public surface.
//   (2) The subclass can reach the inherited protected helpers to drive a
//       Completion through idle -> outstanding -> ready and observe the
//       terminal result.
//   (3) An instance owned by AsyncIoContext with zero outstanding work
//       destroys cleanly (the caller-destructor L11 happy path).
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdio>
#include <utility>

#include "harness.hpp"

using namespace sluice::async;
using sluice::Result;
using sluice::IoError;

// A minimal external backend built ONLY from the public extension surface. All
// seven pure-virtual AsyncBackend methods are inert stubs. It additionally
// exposes the inherited protected helpers through public wrappers so this test
// can drive a Completion lifecycle directly (exactly the authorized
// backend-author pattern; ordinary NON-backend code cannot reach these — that
// boundary is proven by the companion negative-compile gate).
class MinimalExternalBackend : public AsyncBackend {
  public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return Result<std::size_t>{0}; }
    std::size_t outstanding() const noexcept override { return 0; }

    // Authorized wrappers over the inherited protected helpers. These are the
    // sanctioned backend-author capability; a class that does NOT derive from
    // AsyncBackend cannot name them (see external_backend_authority_negative_
    // probe.cpp).
    template <class T>
    bool claim(Completion<T>& c) noexcept {
        return try_claim(c);
    }
    template <class T>
    void publish_completion(Completion<T>& c, Result<T>&& r) noexcept {
        publish(c, std::move(r));
    }
};

// (1) The subclass compiles against the public surface (this whole TU is the
// proof). (2) It can drive a Completion idle -> outstanding -> ready and the
// terminal result is observable.
SLUICE_TEST_CASE(external_backend_can_claim_and_publish) {
    MinimalExternalBackend backend;
    Completion<std::size_t> c;
    SLUICE_CHECK(c.idle());
    SLUICE_CHECK(!c.outstanding());
    SLUICE_CHECK(!c.ready());

    // Claimed via the inherited protected try_claim: idle -> outstanding.
    SLUICE_CHECK(backend.claim(c));
    SLUICE_CHECK(!c.idle());
    SLUICE_CHECK(c.outstanding());
    SLUICE_CHECK(!c.ready());

    // Published via the inherited protected publish: outstanding -> ready.
    backend.publish_completion(c, Result<std::size_t>{7});
    SLUICE_CHECK(!c.idle());
    SLUICE_CHECK(!c.outstanding());
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().has_value());
    SLUICE_CHECK(c.result().value() == 7);
}

// (3) An instance owned by AsyncIoContext with zero outstanding work destroys
// cleanly (the caller-destructor L11 happy path). If we reach the end of the
// scope without terminating, the admission contract holds.
SLUICE_TEST_CASE(external_backend_owned_by_context_destroys_clean) {
    {
        AsyncIoContext ctx(std::make_unique<MinimalExternalBackend>());
        SLUICE_CHECK(ctx.outstanding() == 0);
        // No submits: just verify ownership transfer + clean destruction.
    }
    // Reached here => destructor did not fail-fast.
    SLUICE_CHECK(true);
}

// A second claim on an already-claimed Completion must lose (exactly-one
// winner): the inherited helper returns false and the Completion stays
// outstanding, not double-claimed. The winner must then be published and reset
// before destruction (L11: destroying an outstanding Completion fails fast).
SLUICE_TEST_CASE(external_backend_double_claim_loses) {
    MinimalExternalBackend a;
    MinimalExternalBackend b;
    Completion<std::size_t> c;
    SLUICE_CHECK(a.claim(c));      // first claim wins
    SLUICE_CHECK(!b.claim(c));     // second claim loses
    SLUICE_CHECK(c.outstanding()); // still exactly one outstanding
    // Drive the winner to ready so the Completion can be reset cleanly before
    // destruction (the L11 happy path).
    a.publish_completion(c, Result<std::size_t>{11});
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 11);
    c.reset();  // ready -> idle; slot-release handshake (no-op without an arena)
}

SLUICE_MAIN()
