// Tests for AsyncIoContext lifecycle / move semantics (sluice-CORE-017).
//
// Closes E15-P1-03: AsyncIoContext move assignment must not silently abandon
// the destination's outstanding Completions. The destination-outstanding and
// destroy-outstanding violations are exercised in the companion death test
// (e15_context_death_test.cpp); this file covers the SAFE move paths that must
// continue to work after the fix:
//
//   - move construction from an idle source
//   - move construction from a source WITH outstanding work (safe transfer)
//   - move assignment: idle destination, idle source
//   - move assignment: idle destination, source WITH outstanding work
//   - self move assignment (no-op)
//   - destruction of a moved-from context
//   - publication (poll/wait_one) of source-side outstanding ops after a valid
//     move — the caller's Completion resolves via the NEW owner
//
// The controllable backend used here is FakeAsyncBackend: a held-outstanding
// backend whose publication authority is observable (an outstanding Completion
// the fake "knows about" only resolves when the owning context polls it).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <utility>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// ---- Idle-to-idle move ctor -------------------------------------------------

SLUICE_TEST_CASE(async_io_context_move_ctor_idle_source) {
    AsyncIoContext src(std::make_unique<FakeAsyncBackend>());
    AsyncIoContext dst(std::move(src));
    // Source is left without a backend; outstanding() must report 0 cleanly.
    SLUICE_CHECK(src.outstanding() == 0);
    // Destination took ownership and accepts new work.
    std::byte b[8]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(dst.submit_read(ReadOp{0, b, 8, 0}, c).has_value());
    SLUICE_CHECK(c.outstanding());
    SLUICE_CHECK(dst.outstanding() == 1);
    // Clean teardown: cancel + reap (the fake completes canceled ops at poll).
    dst.cancel(c);
    SLUICE_CHECK(dst.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(dst.outstanding() == 0);
    c.reset();  // slot release handshake (ADR Decision 15)
}

// ---- Move ctor with outstanding source work TRANSFERS publication authority -
// The caller's Completion, submitted through the SOURCE, must resolve via the
// DESTINATION after the move. This is the safe-transfer half of L6/L11.

SLUICE_TEST_CASE(async_io_context_move_ctor_transfers_outstanding_authority) {
    auto src_backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* raw = src_backend.get();
    AsyncIoContext src(std::move(src_backend));

    std::byte b[8]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(src.submit_read(ReadOp{0, b, 8, 0}, c).has_value());
    SLUICE_CHECK(c.outstanding());
    SLUICE_CHECK(src.outstanding() == 1);

    // Move: the backend (and thus its record of `c`) transfers to dst.
    AsyncIoContext dst(std::move(src));
    SLUICE_CHECK(src.outstanding() == 0); // source has no backend now
    SLUICE_CHECK(dst.outstanding() == 1); // dst owns the outstanding op

    // The caller's Completion is STILL outstanding — NOT abandoned — and now
    // resolves through the NEW owner (dst) when dst reaps.
    SLUICE_CHECK(c.outstanding());
    raw->complete_oldest_with_bytes(8); // stage a result on the moved backend
    SLUICE_CHECK(dst.poll() == 1);
    SLUICE_CHECK(c.ready());
    auto r = c.result();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 8);
    SLUICE_CHECK(dst.outstanding() == 0);
    c.reset();  // slot release handshake (ADR Decision 15)
}

// ---- Move assignment: idle destination, idle source -------------------------

SLUICE_TEST_CASE(async_io_context_move_assign_idle_dst_idle_src) {
    AsyncIoContext dst(std::make_unique<FakeAsyncBackend>());
    AsyncIoContext src(std::make_unique<FakeAsyncBackend>());
    dst = std::move(src);
    SLUICE_CHECK(src.outstanding() == 0);
    // dst now owns src's backend and accepts new work.
    std::byte b[4]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(dst.submit_read(ReadOp{0, b, 4, 0}, c).has_value());
    dst.cancel(c);
    SLUICE_CHECK(dst.poll() == 1);
    c.reset();  // slot release handshake (ADR Decision 15)
}

// ---- Move assignment: idle destination, source WITH outstanding work --------
// Safe transfer (same authority as move-ctor). The destination's poll drives
// the source's outstanding op to ready.

SLUICE_TEST_CASE(async_io_context_move_assign_idle_dst_outstanding_src) {
    auto src_backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* raw = src_backend.get();
    AsyncIoContext src(std::move(src_backend));
    AsyncIoContext dst(std::make_unique<FakeAsyncBackend>());

    std::byte b[16]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(src.submit_read(ReadOp{0, b, 16, 0}, c).has_value());

    // dst is idle; the move is permitted and the source's op transfers.
    dst = std::move(src);
    SLUICE_CHECK(src.outstanding() == 0);
    SLUICE_CHECK(dst.outstanding() == 1);
    SLUICE_CHECK(c.outstanding()); // not abandoned

    raw->complete_oldest_with_bytes(16);
    SLUICE_CHECK(dst.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 16);
    SLUICE_CHECK(dst.outstanding() == 0);
    c.reset();  // slot release handshake (ADR Decision 15)
}

// ---- Self move assignment is a no-op ----------------------------------------

SLUICE_TEST_CASE(async_io_context_self_move_assignment_safe) {
    AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
    std::byte b[4]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b, 4, 0}, c).has_value());
    SLUICE_CHECK(ctx.outstanding() == 1);

    // Self move via pointer alias (suppresses -Wself-move while still
    // producing the self-assignment case at runtime).
    AsyncIoContext* p = &ctx;
    ctx = std::move(*p);
    // State preserved: the op is still outstanding against the same backend.
    SLUICE_CHECK(ctx.outstanding() == 1);
    SLUICE_CHECK(c.outstanding());

    // Clean teardown.
    ctx.cancel(c);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    c.reset();  // slot release handshake (ADR Decision 15)
}

// ---- Destruction of a moved-from context is safe (no backend) ---------------

SLUICE_TEST_CASE(async_io_context_destruction_of_moved_from_is_safe) {
    AsyncIoContext src(std::make_unique<FakeAsyncBackend>());
    {
        AsyncIoContext dst(std::move(src));
        // src is moved-from; letting it go out of scope here would destroy it.
        // Instead we destroy dst (cleanly) and then src.
    }
    // src destructor runs at end of scope — backend_ is nullptr, no fail-fast.
}

// ---- Repeated move chains preserve authority --------------------------------

SLUICE_TEST_CASE(async_io_context_chained_moves_preserve_authority) {
    auto backend_up = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* raw = backend_up.get();
    AsyncIoContext a(std::move(backend_up));

    std::byte b[8]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(a.submit_read(ReadOp{0, b, 8, 0}, c).has_value());

    AsyncIoContext b_ctx(std::move(a));
    AsyncIoContext c_ctx(std::move(b_ctx));
    // c_ctx now owns the backend; the caller's Completion still resolves here.
    SLUICE_CHECK(c_ctx.outstanding() == 1);
    SLUICE_CHECK(c.outstanding());

    raw->complete_oldest_with_bytes(8);
    SLUICE_CHECK(c_ctx.poll() == 1);
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 8);
    SLUICE_CHECK(c_ctx.outstanding() == 0);
    c.reset();  // slot release handshake (ADR Decision 15)
}

SLUICE_MAIN()
