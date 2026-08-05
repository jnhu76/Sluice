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
//   (2) An instance owned by AsyncIoContext with zero outstanding work
//       destroys cleanly (the caller-destructor L11 happy path).
//
// The claim/publish LIFECYCLE semantics are intentionally NOT re-proven here:
// derived-class ACCESS to the inherited protected helpers is covered at
// compile level by the authority probe's positive control
// (external_backend_authority_negative_probe.cpp, compiled with no NEG_*
// macro), and the idle -> binding -> outstanding -> ready semantics live in
// the arena-backed Completion tests.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdio>

#include "harness.hpp"

using namespace sluice::async;
using sluice::Result;
using sluice::IoError;

// A minimal external backend built ONLY from the public extension surface. All
// seven pure-virtual AsyncBackend methods are inert stubs. This TU proves the
// subclass compiles against the public surface and can be owned by
// AsyncIoContext; it deliberately does not drive the protected publication
// helpers (that access is proven at compile level by the authority probe's
// positive control, and the lifecycle semantics live in the arena-backed
// Completion tests).
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
};

// (1) The subclass compiles against the public surface (this whole TU is the
// proof). (2) An instance owned by AsyncIoContext with zero outstanding work
// destroys cleanly (the caller-destructor L11 happy path). If we reach the end
// of the scope without terminating, the admission contract holds.
SLUICE_TEST_CASE(external_backend_owned_by_context_destroys_clean) {
    {
        AsyncIoContext ctx(std::make_unique<MinimalExternalBackend>());
        SLUICE_CHECK(ctx.outstanding() == 0);
        // No submits: just verify ownership transfer + clean destruction.
    }
    // Reached here => destructor did not fail-fast.
    SLUICE_CHECK(true);
}

SLUICE_MAIN()
