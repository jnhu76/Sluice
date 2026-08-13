// F3 (issue #98) — public RequestHandle: opaque accepted-request identity.
// ADR-public-request-handle. Proves through PUBLIC headers only (no src/, no
// detail/ mutation, no SLUICE_ASYNC_INTERNAL_TESTING):
//   F3-1 accepted submit -> valid handle
//   F3-2 synchronous rejection (capacity) -> error, NO handle, no acceptance
//   F3-3 external/legacy backend -> not_supported, no side effect
//   F3-4 request_state lifecycle: outstanding -> completion_ready
//   F3-5 cross-context handle -> not_found (C2b row 4b)
//   F3-6 stale generation after slot reuse -> not_found (C2c row 14b)
//
// Each case quiesces before scope exit: an accepted Completion is completed,
// reaped, and reset so the context is destroyed with zero outstanding work
// (AGENTS.md §14 — non-quiescent destruction fails fast).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/request_handle.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <memory>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// ---- F3-1: accepted submit produces a valid handle --------------------------
SLUICE_TEST_CASE(f3_accepted_submit_yields_valid_handle) {
    auto owned = std::make_unique<FakeAsyncBackend>(2);
    FakeAsyncBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Completion<std::size_t> c;
    std::byte buf[4]{};
    auto hr = ctx.submit_read_request(ReadOp{0, buf, 4, 0}, c);
    SLUICE_CHECK(hr.has_value());
    SLUICE_CHECK(hr.value().valid());
    // The handle names a currently-accepted request of THIS context.
    auto st = ctx.request_state(hr.value());
    SLUICE_CHECK(st.has_value());
    SLUICE_CHECK(st.value() == RequestHandleState::outstanding);

    raw->complete_oldest_with_bytes(4);
    ctx.poll();
    c.reset();
}

// ---- F3-2: synchronous rejection (capacity) -> error, no handle -------------
SLUICE_TEST_CASE(f3_capacity_rejection_yields_no_handle) {
    auto owned = std::make_unique<FakeAsyncBackend>(1);  // capacity 1
    FakeAsyncBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Completion<std::size_t> c1, c2;
    std::byte b1[4]{}, b2[4]{};
    SLUICE_CHECK(ctx.submit_read_request(ReadOp{0, b1, 4, 0}, c1).has_value());
    // Second submit exceeds arena capacity -> synchronous would_block, no handle,
    // and c2 is never accepted.
    auto hr2 = ctx.submit_read_request(ReadOp{0, b2, 4, 0}, c2);
    SLUICE_CHECK(!hr2.has_value());
    SLUICE_CHECK(hr2.error().code == IoError::Code::would_block);
    SLUICE_CHECK(!c2.ready());
    SLUICE_CHECK(ctx.outstanding() == 1);

    raw->complete_oldest_with_bytes(4);  // quiesce c1
    ctx.poll();
    c1.reset();
}

// ---- F3-3: external/legacy backend -> not_supported, no side effect ----------
// A backend that does NOT override supports_request_identity() (defaults false)
// must cause submit_*_request to return not_supported WITHOUT submitting.
class LegacyBackend : public AsyncBackend {
public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override { return {}; }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override { return {}; }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return Result<std::size_t>{0}; }
    std::size_t outstanding() const noexcept override { return 0; }
};

SLUICE_TEST_CASE(f3_legacy_backend_returns_not_supported) {
    AsyncIoContext ctx(std::make_unique<LegacyBackend>());
    Completion<std::size_t> c;
    std::byte buf[4]{};
    auto hr = ctx.submit_read_request(ReadOp{0, buf, 4, 0}, c);
    SLUICE_CHECK(!hr.has_value());
    SLUICE_CHECK(hr.error().code == IoError::Code::not_supported);
    // No side effect: Completion stays idle, nothing outstanding.
    SLUICE_CHECK(!c.ready());
    SLUICE_CHECK(ctx.outstanding() == 0);
}

// ---- F3-4: request_state lifecycle outstanding -> completion_ready ----------
SLUICE_TEST_CASE(f3_request_state_lifecycle) {
    auto owned = std::make_unique<FakeAsyncBackend>(2);
    FakeAsyncBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Completion<std::size_t> c;
    std::byte buf[4]{};
    auto hr = ctx.submit_read_request(ReadOp{0, buf, 4, 0}, c);
    SLUICE_CHECK(ctx.request_state(hr.value()).value() == RequestHandleState::outstanding);

    raw->complete_oldest_with_bytes(4);
    ctx.poll();  // publish terminal -> completion-ready
    SLUICE_CHECK(ctx.request_state(hr.value()).value() == RequestHandleState::completion_ready);
    c.reset();
}

// ---- F3-5: cross-context handle is not_found (C2b row 4b) -------------------
SLUICE_TEST_CASE(f3_cross_context_handle_is_not_found) {
    auto a = std::make_unique<FakeAsyncBackend>(2);
    FakeAsyncBackend* ra = a.get();
    AsyncIoContext ctxA(std::move(a));
    auto b = std::make_unique<FakeAsyncBackend>(2);
    AsyncIoContext ctxB(std::move(b));

    Completion<std::size_t> c;
    std::byte buf[4]{};
    auto hr = ctxA.submit_read_request(ReadOp{0, buf, 4, 0}, c);
    SLUICE_CHECK(ctxA.request_state(hr.value()).value() == RequestHandleState::outstanding);
    // A handle minted by ctxA is foreign to ctxB -> not_found (provenance).
    SLUICE_CHECK(ctxB.request_state(hr.value()).value() == RequestHandleState::not_found);
    // ctxA still observes its own request; ctxB cannot perturb it.
    SLUICE_CHECK(ctxA.request_state(hr.value()).value() == RequestHandleState::outstanding);

    ra->complete_oldest_with_bytes(4);  // quiesce ctxA's request
    ctxA.poll();
    c.reset();
}

// ---- F3-6: stale generation after slot reuse is not_found (C2c row 14b) -----
SLUICE_TEST_CASE(f3_stale_generation_after_reuse_is_not_found) {
    auto owned = std::make_unique<FakeAsyncBackend>(1);  // capacity 1
    FakeAsyncBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Completion<std::size_t> c1;
    std::byte b1[4]{};
    auto h1 = ctx.submit_read_request(ReadOp{0, b1, 4, 0}, c1);
    raw->complete_oldest_with_bytes(4);
    ctx.poll();
    SLUICE_CHECK(ctx.request_state(h1.value()).value() == RequestHandleState::completion_ready);

    c1.reset();  // release slot 0; generation advances before reuse

    // Reuse slot 0 for a new request -> a new handle with generation+1.
    Completion<std::size_t> c2;
    std::byte b2[4]{};
    auto h2 = ctx.submit_read_request(ReadOp{0, b2, 4, 0}, c2);
    SLUICE_CHECK(h2.value().valid());
    // The old handle is stale: generation no longer matches the occupant.
    SLUICE_CHECK(ctx.request_state(h1.value()).value() == RequestHandleState::not_found);
    // The new handle names the current occupant.
    SLUICE_CHECK(ctx.request_state(h2.value()).value() == RequestHandleState::outstanding);

    raw->complete_oldest_with_bytes(4);  // quiesce c2
    ctx.poll();
    c2.reset();
}

SLUICE_MAIN()
