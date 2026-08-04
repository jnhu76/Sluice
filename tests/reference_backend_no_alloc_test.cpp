// Phase B — reference-backend allocation-freedom + transactional-rejection
// proof (review test-gap 3 / review C1 fault-injection matrix).
//
// The review rejected "ASan/UBSan + a 1000-iteration loop" as proof of "zero
// allocation": neither detects allocations. This test replaces that with a
// REAL allocation probe:
//   - a counting + always-throw operator new (malloc-based, so it composes
//     with ASan/TSan interposition);
//   - the accepted submit -> poll (reap) -> reset path is then driven with
//     EVERY allocation throwing std::bad_alloc;
//   - the path must still succeed (it performs zero allocations), and the
//     allocation counter must still read 0.
//
// This is the structural proof for review C1/C3/I9: after construction, the
// reference backends' admission (reserve/prepare/binding-install/CAS/commit/
// enqueue), the manual completion path (complete_oldest_* -> record_terminal),
// the reap publication (in-domain publish through the slot-bound thunk), and
// the caller reset handshake allocate NOTHING — the pre-commit bookkeeping is
// construction-time-bounded (binding in the slot record; the side-band FIFO and
// staging deques were removed in review findings #1/#2, so complete_oldest_* is
// now a bounded O(capacity) scan + record_terminal with no allocation), and a
// rejected submit (lost binding CAS) is zero-side-effect AND zero-allocation.
//
// The always-throw window covers the measured operations; with the staging
// deques gone, complete_oldest_with_bytes() runs INSIDE the window (the
// fake_complete_reap_reset_is_allocation_free case proves Decision 14 for the
// manual-completion path that the prior implementation could not).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/sync_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// --- Counting + fault-injection allocation probe -----------------------------
// malloc-based replacement so sanitizer interposition still tracks the bytes;
// the counter counts allocations, the throw-flag turns every allocation into
// std::bad_alloc (an allocation-fault injection at the strongest point).
namespace {
std::atomic<std::size_t> g_allocations{0};
// [[maybe_unused]]: under TSan the probe is compiled out (kAllocProbeActive ==
// false) and only g_allocations is touched.
[[maybe_unused]] std::atomic<bool> g_throw_all{false};
}  // namespace

// The TSan C++ runtime defines ALL operator new/delete variants as strong
// symbols, so the counting/fault-injection replacements below cannot be linked
// into a TSan binary. Under TSan the allocation probe is compiled out
// (kAllocProbeActive == false) and the counter/always-throw assertions are
// skipped; the lifecycle and zero-side-effect assertions remain active (the
// TSan run's job is the concurrency/race evidence — the allocation-freedom
// proof comes from the Debug/Release/ASan runs where the probe is active).
#if !defined(__has_feature) || !__has_feature(thread_sanitizer)
constexpr bool kAllocProbeActive = true;

void* operator new(std::size_t n) {
    if (g_throw_all.load(std::memory_order_acquire)) {
        throw std::bad_alloc{};
    }
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
// Sized-delete variants MUST also route through plain free so sanitizer
// interposition sees a consistent malloc/new class (an alloc-dealloc mismatch
// otherwise fires under ASan).
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
// Aligned overloads MUST preserve the requested alignment (CodeRabbit finding:
// forwarding to ::operator new(n) returns std::malloc memory, which is only
// max_align_t-aligned — an over-aligned allocation, e.g. a cache-line-padded
// slot record, would receive misaligned storage and the matching aligned
// deletes would free it, which is UB and an ASan alloc-alignment mismatch).
// std::aligned_alloc requires a size that is an integer multiple of the
// alignment; round up. <cstdlib> (already included) provides it.
void* operator new(std::size_t n, std::align_val_t a) {
    if (g_throw_all.load(std::memory_order_acquire)) {
        throw std::bad_alloc{};
    }
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    const std::size_t align = static_cast<std::size_t>(a);
    const std::size_t size = ((n + align - 1) / align) * align;
    if (void* p = std::aligned_alloc(align, size)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, std::size_t) noexcept { std::free(p); }
#else
constexpr bool kAllocProbeActive = false;
#endif

SLUICE_MAIN()

// ---- SyncBackend: submit -> poll (reap) -> reset allocates NOTHING ----------
// With every allocation throwing, the full accepted lifecycle of one op must
// still succeed — the arena, the slot binding, the reap publication, and the
// reset handshake are all construction-time-bounded (review C1/C3/I9).
SLUICE_TEST_CASE(sync_submit_reap_reset_is_allocation_free) {
    auto backend = std::make_unique<SyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[8]{};
    Completion<std::size_t> c;

    if constexpr (kAllocProbeActive) {
        g_allocations.store(0, std::memory_order_relaxed);
        g_throw_all.store(true, std::memory_order_relaxed);
    }
    bool submit_ok = ctx.submit_read(ReadOp{0, buf, 8, 0}, c).has_value();
    std::size_t polled = ctx.poll();
    bool ready = c.ready();
    c.reset();
    if constexpr (kAllocProbeActive) {
        g_throw_all.store(false, std::memory_order_relaxed);
    }

    SLUICE_CHECK_MSG(submit_ok, "submit must succeed under always-throw operator new");
    SLUICE_CHECK_MSG(polled == 1, "poll must reap exactly one under always-throw operator new");
    SLUICE_CHECK_MSG(ready, "the Completion must become ready under always-throw operator new");
    SLUICE_CHECK_MSG(c.idle(), "reset must return the Completion to idle");
    if constexpr (kAllocProbeActive) {
        std::size_t allocs = g_allocations.load(std::memory_order_relaxed);
        SLUICE_CHECK_MSG(allocs == 0, "the accepted submit/reap/reset path must allocate nothing");
    }
    SLUICE_CHECK_MSG(ctx.outstanding() == 0, "arena drained");
}

// ---- SyncBackend: would_block rejection is allocation-free ------------------
// Capacity pressure (ADR Decision 6/13) on a full arena: the rejection path
// (reserve -> would_block) also allocates nothing — even the rejection is
// transactional under allocation failure.
SLUICE_TEST_CASE(sync_full_arena_rejection_is_allocation_free) {
    auto backend = std::make_unique<SyncBackend>(/*request_capacity=*/2);
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[8]{};
    Completion<std::size_t> c1, c2, c3;

    g_allocations.store(0, std::memory_order_relaxed);
    if constexpr (kAllocProbeActive) {
        g_throw_all.store(true, std::memory_order_relaxed);
    }
    bool ok1 = ctx.submit_read(ReadOp{0, buf, 8, 0}, c1).has_value();
    bool ok2 = ctx.submit_read(ReadOp{0, buf, 8, 0}, c2).has_value();
    auto r3 = ctx.submit_read(ReadOp{0, buf, 8, 0}, c3);
    bool rejected_would_block =
        !r3.has_value() && r3.error().code == IoError::Code::would_block;
    std::size_t polled = ctx.poll();
    c1.reset();
    c2.reset();
    if constexpr (kAllocProbeActive) {
        g_throw_all.store(false, std::memory_order_relaxed);
    }

    SLUICE_CHECK_MSG(ok1 && ok2, "both capacity slots must accept");
    SLUICE_CHECK_MSG(rejected_would_block,
                     "third submit must would_block (capacity, never invalid_state)");
    SLUICE_CHECK_MSG(c3.idle(), "rejected Completion stays idle");
    SLUICE_CHECK_MSG(polled == 2, "both accepted ops must reap");
    if constexpr (kAllocProbeActive) {
        std::size_t allocs = g_allocations.load(std::memory_order_relaxed);
        SLUICE_CHECK_MSG(allocs == 0, "the reject path must allocate nothing");
    }
}

// ---- FakeAsyncBackend: CAS-loss rejection is zero-side-effect + zero-alloc --
// Review C1 fault-injection matrix, "binding CAS loss": submitting into a
// non-idle Completion loses the binding CAS and returns invalid_state. Assert
// the full zero-side-effect contract: Completion untouched (still outstanding,
// its original op unaffected), slot_in_use unchanged, accepted_outstanding
// unchanged, submission-order FIFO unchanged, and no future result
// contamination (the staged result still lands on the right op). Everything
// runs under always-throw operator new — the rejection path allocates nothing.
SLUICE_TEST_CASE(fake_cas_loss_rejection_zero_side_effects) {
    // Capacity 3: two accepted ops fill slots 0/1, leaving a FREE slot for the
    // third submit — so the rejection happens at the Completion binding CAS
    // (non-idle Completion), NOT at reserve (would_block).
    auto backend = std::make_unique<FakeAsyncBackend>(/*request_capacity=*/3);
    FakeAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[8]{};
    Completion<std::size_t> c1, c2;

    // Two accepted ops; the third submit on c1 loses the binding CAS.
    g_allocations.store(0, std::memory_order_relaxed);
    if constexpr (kAllocProbeActive) {
        g_throw_all.store(true, std::memory_order_relaxed);
    }
    bool ok1 = ctx.submit_read(ReadOp{0, buf, 8, 0}, c1).has_value();
    bool ok2 = ctx.submit_read(ReadOp{0, buf, 8, 0}, c2).has_value();
    // Submit into c1 AGAIN while it is outstanding: reserve succeeds (free
    // slot), prepare + binding install succeed, the binding CAS loses.
    auto rej = ctx.submit_read(ReadOp{0, buf, 8, 0}, c1);
    bool rejected_invalid =
        !rej.has_value() && rej.error().code == IoError::Code::invalid_state;
    if constexpr (kAllocProbeActive) {
        g_throw_all.store(false, std::memory_order_relaxed);
    }

    SLUICE_CHECK_MSG(ok1 && ok2, "both capacity slots must accept");
    SLUICE_CHECK_MSG(rejected_invalid, "submit into a non-idle Completion must invalid_state");
    // Zero side effects (review C1): the rejected submit touched nothing —
    // its candidate slot was rolled back (slot_in_use unchanged at 2). There is
    // no side-band FIFO to leave residue in (review finding #1 removed it); the
    // candidate was freed before commit, so neither submit_seq nor ready-ring
    // linkage was ever installed for it.
    SLUICE_CHECK_MSG(c1.outstanding(), "the original Completion is untouched");
    SLUICE_CHECK_MSG(c2.outstanding(), "the other Completion is untouched");
    SLUICE_CHECK_MSG(raw->arena_slot_in_use() == 2, "slot_in_use unchanged (candidate rolled back)");
    SLUICE_CHECK_MSG(ctx.outstanding() == 2, "accepted_outstanding unchanged");
    if constexpr (kAllocProbeActive) {
        std::size_t allocs = g_allocations.load(std::memory_order_relaxed);
        SLUICE_CHECK_MSG(allocs == 0, "the rejected submit must allocate nothing");
    }

    // No future result contamination: complete + reap deliver exactly the
    // resolved bytes to the right ops; the rejected submit left no ghost.
    raw->complete_oldest_with_bytes(8);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c1.ready());
    SLUICE_CHECK(c1.result().value() == 8);
    SLUICE_CHECK(!c2.ready());
    raw->complete_oldest_with_bytes(16);
    SLUICE_CHECK(ctx.poll() == 1);
    SLUICE_CHECK(c2.ready());
    SLUICE_CHECK(c2.result().value() == 16);
    c1.reset();
    c2.reset();
    SLUICE_CHECK(raw->arena_slot_in_use() == 0);
}

// ---- FakeAsyncBackend: complete_* -> poll (reap) -> reset is allocation-free --
// Review finding #2 (ADR Decision 14): the manual-completion path (complete_oldest_*
// binding a terminal result to a RequestKey, then reap publishing it) MUST NOT
// allocate. The previous implementation staged results in a std::deque, so this
// test deliberately ran complete_oldest_with_bytes() OUTSIDE the always-throw
// window — meaning the manual path was never actually proven allocation-free.
// With the staging deque removed (terminal evidence binds to the slot
// immediately via record_terminal), complete_oldest_* + poll + reset runs
// entirely under always-throw operator new and must still succeed with zero
// allocations. This closes the Decision 14 gap.
SLUICE_TEST_CASE(fake_complete_reap_reset_is_allocation_free) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    std::byte buf[8]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(ctx.submit_read(ReadOp{0, buf, 8, 0}, c).has_value());

    if constexpr (kAllocProbeActive) {
        g_allocations.store(0, std::memory_order_relaxed);
        g_throw_all.store(true, std::memory_order_relaxed);
    }
    // The previously-uncovered path: complete (record_terminal) + reap + reset,
    // all under always-throw operator new.
    raw->complete_oldest_with_bytes(8);
    std::size_t polled = ctx.poll();
    bool ready = c.ready();
    c.reset();
    if constexpr (kAllocProbeActive) {
        g_throw_all.store(false, std::memory_order_relaxed);
    }

    SLUICE_CHECK_MSG(polled == 1, "poll must reap exactly one under always-throw operator new");
    SLUICE_CHECK_MSG(ready, "the Completion must become ready under always-throw operator new");
    SLUICE_CHECK_MSG(c.idle(), "reset must return the Completion to idle");
    if constexpr (kAllocProbeActive) {
        std::size_t allocs = g_allocations.load(std::memory_order_relaxed);
        SLUICE_CHECK_MSG(allocs == 0,
                         "the complete/reap/reset path must allocate nothing (Decision 14)");
    }
    SLUICE_CHECK_MSG(ctx.outstanding() == 0, "arena drained");
}
