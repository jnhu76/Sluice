// e12_api_contract_probes
//
// Cross-primitive compile-time contract probe (E10-E12-ASYNC-SYNC-API-SEMANTIC-
// CLOSURE-1). A SEPARATE probe TU, mirroring the
// e12_async_mutex_nothrow_authority_probe precedent: verification evidence does
// not belong in the public installed surface, and the assertions below encode
// the cross-primitive parity contract that the semantic-closure authority
// rests on.
//
// Scope: PURE compile-time type-structure and access-control. Verifies that
// every public async synchronization primitive is non-copyable AND non-movable
// (D5: lifecycle is object identity — address stability is a WaitNode/WaitQueue
// invariant), that the typed Queue result types are move-only (D5: result
// model is consistent and minimal — value recovery is by take_value() and the
// result is not copyable), and that the result types remain move-assignable
// even when T is NOT move-assignable (PR #12 corrective — the hand-written
// destroy-and-rebuild operator= preserves the documented T contract).
//
// This probe does NOT replace the per-primitive authority probes
// (e12_event_authority_probe, e12_async_queue_authority_probe, etc.). Those
// gate per-primitive SEALED-AUTHORITY invariants (WaitQueue access, lease
// one-shot, etc.). This probe gates the CROSS-PRIMITIVE contract: every
// primitive advertises the same value-category surface (non-copy/non-move),
// and the Queue result types advertise the documented type constraints.
//
// All runtime semantic behavior (cancel outcomes, deadline precedence, FIFO
// selection, teardown) is exercised by the per-primitive functional test
// suites. This probe verifies the compile-time shape only.
#include <sluice/async/async_queue.hpp>
#include <sluice/async/async_mutex.hpp>
#include <sluice/async/condition.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/semaphore.hpp>
#include <sluice/async/wait_node.hpp>

#include <memory>
#include <type_traits>
#include <utility>

// ---------------------------------------------------------------------------
// D5: every primitive is non-copyable AND non-movable.
//
// Address stability is a WaitNode/WaitQueue invariant: the wait-epoch is
// identified by WaitNode object identity, and a primitive that could be moved
// or copied would invalidate the intrusive list links and the owner_ Fiber*
// identity recorded at admission. Every public primitive therefore deletes
// all four special members.
// ---------------------------------------------------------------------------

// E10 substrate: WaitNode.
static_assert(!std::is_copy_constructible_v<sluice::async::WaitNode>);
static_assert(!std::is_copy_assignable_v<sluice::async::WaitNode>);
static_assert(!std::is_move_constructible_v<sluice::async::WaitNode>);
static_assert(!std::is_move_assignable_v<sluice::async::WaitNode>);

// E12-A: Event.
static_assert(!std::is_copy_constructible_v<sluice::async::Event>);
static_assert(!std::is_copy_assignable_v<sluice::async::Event>);
static_assert(!std::is_move_constructible_v<sluice::async::Event>);
static_assert(!std::is_move_assignable_v<sluice::async::Event>);

// E12-B: Semaphore.
static_assert(!std::is_copy_constructible_v<sluice::async::Semaphore>);
static_assert(!std::is_copy_assignable_v<sluice::async::Semaphore>);
static_assert(!std::is_move_constructible_v<sluice::async::Semaphore>);
static_assert(!std::is_move_assignable_v<sluice::async::Semaphore>);

// E12-C: AsyncMutex.
static_assert(!std::is_copy_constructible_v<sluice::async::AsyncMutex>);
static_assert(!std::is_copy_assignable_v<sluice::async::AsyncMutex>);
static_assert(!std::is_move_constructible_v<sluice::async::AsyncMutex>);
static_assert(!std::is_move_assignable_v<sluice::async::AsyncMutex>);

// E12-D: AsyncCondition.
static_assert(!std::is_copy_constructible_v<sluice::async::AsyncCondition>);
static_assert(!std::is_copy_assignable_v<sluice::async::AsyncCondition>);
static_assert(!std::is_move_constructible_v<sluice::async::AsyncCondition>);
static_assert(!std::is_move_assignable_v<sluice::async::AsyncCondition>);

// E12-E: AsyncQueue<T> (template; assert on a concrete instantiation below to
// avoid polluting the static_assert block with template machinery).

// ---------------------------------------------------------------------------
// WaitOutcome vocabulary: the enum is a trivially-copyable value type
// and a plain enum-class. The four outcomes are mutually exclusive: each
// wait-epoch terminates in exactly one.
// ---------------------------------------------------------------------------
static_assert(std::is_enum_v<sluice::async::WaitOutcome>);
static_assert(std::is_trivially_copyable_v<sluice::async::WaitOutcome>);
static_assert(std::is_nothrow_default_constructible_v<sluice::async::WaitOutcome>);

// Four binding outcomes. A static_cast of an out-of-range int would still
// compile, so the values are asserted pairwise-distinct instead.
namespace {
constexpr sluice::async::WaitOutcome kUnresolved = sluice::async::WaitOutcome::unresolved;
constexpr sluice::async::WaitOutcome kWoken = sluice::async::WaitOutcome::woken;
constexpr sluice::async::WaitOutcome kCancelled = sluice::async::WaitOutcome::cancelled;
constexpr sluice::async::WaitOutcome kExpired = sluice::async::WaitOutcome::expired;
static_assert(kUnresolved != kWoken);
static_assert(kWoken != kCancelled);
static_assert(kCancelled != kExpired);
static_assert(kUnresolved != kExpired);
static_assert(kUnresolved != kCancelled);
static_assert(kWoken != kExpired);
}  // namespace

// ---------------------------------------------------------------------------
// D1/D9 / PR #12 corrective: the typed Queue result types are move-only AND
// remain move-assignable even when T is NOT move-assignable. The hand-written
// destroy-and-rebuild operator= (see async_queue.hpp) preserves the documented
// P8 contract (T is object + nothrow-move-constructible + nothrow-destructible
// only — move-assignability is NOT required).
// ---------------------------------------------------------------------------

// A T that satisfies the P8 contract but is NOT move-assignable: copy/move
// ctor and dtor are nothrow, but operator=(T&&) is deleted. This is the type
// the PR #12 corrective was written to support.
struct NonMoveAssignable {
    int value;
    explicit NonMoveAssignable(int v) noexcept : value(v) {}
    NonMoveAssignable(NonMoveAssignable&& other) noexcept : value(other.value) {}
    NonMoveAssignable(const NonMoveAssignable&) = delete;
    NonMoveAssignable& operator=(NonMoveAssignable&&) = delete;
    NonMoveAssignable& operator=(const NonMoveAssignable&) = delete;
    ~NonMoveAssignable() noexcept = default;
};
static_assert(std::is_nothrow_move_constructible_v<NonMoveAssignable>);
static_assert(std::is_nothrow_destructible_v<NonMoveAssignable>);
static_assert(!std::is_move_assignable_v<NonMoveAssignable>);

// A nothrow-move T that IS move-assignable (the common case): same surface.
struct MoveAssignable {
    int value;
    explicit MoveAssignable(int v) noexcept : value(v) {}
    MoveAssignable(MoveAssignable&&) noexcept = default;
    MoveAssignable& operator=(MoveAssignable&&) noexcept = default;
};
static_assert(std::is_nothrow_move_constructible_v<MoveAssignable>);
static_assert(std::is_nothrow_destructible_v<MoveAssignable>);
static_assert(std::is_nothrow_move_assignable_v<MoveAssignable>);

// AsyncQueue<T> is non-copy/non-move regardless of T's value-category.
static_assert(!std::is_copy_constructible_v<sluice::async::AsyncQueue<MoveAssignable>>);
static_assert(!std::is_copy_assignable_v<sluice::async::AsyncQueue<MoveAssignable>>);
static_assert(!std::is_move_constructible_v<sluice::async::AsyncQueue<MoveAssignable>>);
static_assert(!std::is_move_assignable_v<sluice::async::AsyncQueue<MoveAssignable>>);

// PR #12 corrective — both result types are move-only for either T flavor.
using PR_Move = sluice::async::QueuePushResult<MoveAssignable>;
using PR_NonMove = sluice::async::QueuePushResult<NonMoveAssignable>;
using RR_Move = sluice::async::QueuePopResult<MoveAssignable>;
using RR_NonMove = sluice::async::QueuePopResult<NonMoveAssignable>;

static_assert(std::is_move_constructible_v<PR_Move>);
static_assert(std::is_move_constructible_v<PR_NonMove>);
static_assert(std::is_move_constructible_v<RR_Move>);
static_assert(std::is_move_constructible_v<RR_NonMove>);

static_assert(!std::is_copy_constructible_v<PR_Move>);
static_assert(!std::is_copy_constructible_v<PR_NonMove>);
static_assert(!std::is_copy_constructible_v<RR_Move>);
static_assert(!std::is_copy_constructible_v<RR_NonMove>);
static_assert(!std::is_copy_assignable_v<PR_Move>);
static_assert(!std::is_copy_assignable_v<PR_NonMove>);
static_assert(!std::is_copy_assignable_v<RR_Move>);
static_assert(!std::is_copy_assignable_v<RR_NonMove>);

// The decisive PR #12 assertion: move-assign remains well-formed even when T
// is NOT move-assignable. The hand-written operator= preserves the contract.
static_assert(std::is_move_assignable_v<PR_Move>);
static_assert(std::is_move_assignable_v<PR_NonMove>);
static_assert(std::is_move_assignable_v<RR_Move>);
static_assert(std::is_move_assignable_v<RR_NonMove>);

// Move-ctor and move-assign are noexcept: storage is std::optional<T>, and T
// is nothrow-move-constructible (P8 contract). The hand-written operator=
// performs at most one T move-construct and one T destruct, both nothrow.
static_assert(std::is_nothrow_move_constructible_v<PR_Move>);
static_assert(std::is_nothrow_move_constructible_v<PR_NonMove>);
static_assert(std::is_nothrow_move_constructible_v<RR_Move>);
static_assert(std::is_nothrow_move_constructible_v<RR_NonMove>);
static_assert(std::is_nothrow_move_assignable_v<PR_Move>);
static_assert(std::is_nothrow_move_assignable_v<PR_NonMove>);
static_assert(std::is_nothrow_move_assignable_v<RR_Move>);
static_assert(std::is_nothrow_move_assignable_v<RR_NonMove>);

// ---------------------------------------------------------------------------
// Negative-compile assertions.
//
// Each `#ifdef NEG_...` block is compiled SEPARATELY by the compile-probe gate
// with `-DNEG_<name>` and is REQUIRED to fail to compile. A successful compile
// is a regression (the access control has been opened). These are NOT built by
// the production target.
// ---------------------------------------------------------------------------

#ifdef NEG_WAITNODE_COPY
sluice::async::WaitNode waitnode_copy_neg(const sluice::async::WaitNode& src) {
    return src;  // ERROR: deleted copy ctor
}
#endif

#ifdef NEG_WAITNODE_MOVE
sluice::async::WaitNode waitnode_move_neg(sluice::async::WaitNode& src) {
    return std::move(src);  // ERROR: deleted move ctor
}
#endif

#ifdef NEG_EVENT_COPY
sluice::async::Event event_copy_neg(const sluice::async::Event& src) {
    return src;  // ERROR: deleted copy ctor
}
#endif

#ifdef NEG_SEMAPHORE_MOVE
sluice::async::Semaphore semaphore_move_neg(sluice::async::Semaphore& src) {
    return std::move(src);  // ERROR: deleted move ctor
}
#endif

#ifdef NEG_ASYNCMUTEX_COPY
sluice::async::AsyncMutex asyncmutex_copy_neg(const sluice::async::AsyncMutex& src) {
    return src;  // ERROR: deleted copy ctor
}
#endif

#ifdef NEG_ASYNCCONDITION_MOVE
sluice::async::AsyncCondition
asynccondition_move_neg(sluice::async::AsyncCondition& src) {
    return std::move(src);  // ERROR: deleted move ctor
}
#endif

#ifdef NEG_QUEUE_COPY
sluice::async::AsyncQueue<int>
queue_copy_neg(const sluice::async::AsyncQueue<int>& src) {
    return src;  // ERROR: deleted copy ctor
}
#endif

#ifdef NEG_QUEUE_PUSH_RESULT_COPY
sluice::async::QueuePushResult<int>
push_result_copy_neg(const sluice::async::QueuePushResult<int>& src) {
    return src;  // ERROR: deleted copy ctor
}
#endif

#ifdef NEG_QUEUE_POP_RESULT_COPY
sluice::async::QueuePopResult<int>
pop_result_copy_neg(const sluice::async::QueuePopResult<int>& src) {
    return src;  // ERROR: deleted copy ctor
}
#endif

int main() {
    // The probe's main is intentionally trivial: all verification is
    // compile-time. Runtime semantic behavior is exercised by the per-
    // primitive functional test suites.
    return 0;
}
