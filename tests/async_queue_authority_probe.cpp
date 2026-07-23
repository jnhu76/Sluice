// async_queue_authority_probe
//
// Compile-time + run-time probe for the E12-E Queue LINEAR-CAPABILITY
// foundation (E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-2 §4):
// QueueItemControl / QueueItemLease one-shot, move-only, unforgeable
// ownership.
//
// Deliberately a SEPARATE probe TU, not a block in the installed header
// (mirrors the e12_async_mutex_nothrow_authority_probe precedent): verification
// evidence does not belong in the public installed surface, and the
// negative-compile assertions below are driven by a compile-probe gate, not by
// the production build.
//
// Scope (P1): type-structure and access-control only. The full lifecycle
// (mint, admission, ring transfer, teardown) — including the runtime move /
// empty-source / fail-fast behaviors — is exercised by the Queue functional
// test suite through the public AsyncQueue<T> API. This probe verifies the
// compile-time access-control and linear-capability claims the design rests on.
#include <sluice/async/detail/queue_item.hpp>

#include <type_traits>
#include <utility>

using namespace sluice::async::detail;

// ---------------------------------------------------------------------------
// Positive type-structure assertions (require no instance).
// ---------------------------------------------------------------------------

// QueueItemControl is non-copyable, non-movable (its address IS the ItemId).
static_assert(!std::is_copy_constructible_v<QueueItemControl>);
static_assert(!std::is_copy_assignable_v<QueueItemControl>);
static_assert(!std::is_move_constructible_v<QueueItemControl>);
static_assert(!std::is_move_assignable_v<QueueItemControl>);

// QueueItemLease is move-only.
static_assert(std::is_move_constructible_v<QueueItemLease>);
static_assert(std::is_move_assignable_v<QueueItemLease>);
static_assert(!std::is_copy_constructible_v<QueueItemLease>);
static_assert(!std::is_copy_assignable_v<QueueItemLease>);

// Move ctor / move assign are noexcept (they only exchange a pointer; never
// allocate, never invoke user code).
static_assert(std::is_nothrow_move_constructible_v<QueueItemLease>);
static_assert(std::is_nothrow_move_assignable_v<QueueItemLease>);

// Destructor is noexcept (it never invokes user code; a non-empty lease
// fail-fasts via std::terminate, which is noexcept).
static_assert(std::is_nothrow_destructible_v<QueueItemLease>);

// QueueItemControl::Location is a scoped enum with exactly the six binding
// locations (detached / producer_operation / ring / consumer_operation /
// teardown / released). A static cast of an out-of-range int would still
// compile, so we assert the enumerators explicitly instead.
static_assert(QueueItemControl::Location::detached !=
              QueueItemControl::Location::producer_operation);
static_assert(QueueItemControl::Location::producer_operation !=
              QueueItemControl::Location::ring);
static_assert(QueueItemControl::Location::ring !=
              QueueItemControl::Location::consumer_operation);
static_assert(QueueItemControl::Location::consumer_operation !=
              QueueItemControl::Location::teardown);
static_assert(QueueItemControl::Location::teardown !=
              QueueItemControl::Location::released);

// ---------------------------------------------------------------------------
// Negative-compile assertions.
//
// Each `#ifdef NEG_...` block is compiled SEPARATELY by the compile-probe gate
// (scripts/verify-e12-queue-formal.sh, or the dedicated authority-probe gate)
// with `-DNEG_<name>` and is REQUIRED to fail to compile. A successful compile
// is a regression (the access control has been opened). These are NOT built by
// the production target.
// ---------------------------------------------------------------------------

#ifdef NEG_LEASE_COPY
// Copy construction of a lease must not compile (one-shot ownership).
QueueItemLease copy_neg(const QueueItemLease& src) {
    return src;  // ERROR: deleted copy ctor
}
#endif

#ifdef NEG_LEASE_COPY_ASSIGN
void copy_assign_neg(QueueItemLease& dst, const QueueItemLease& src) {
    dst = src;  // ERROR: deleted copy assign
}
#endif

#ifdef NEG_LEASE_PUBLIC_DEFAULT_CTOR
// The empty default ctor is PRIVATE (authority-only). Downstream code must not
// be able to construct a lease without the QueuePort minting it.
QueueItemLease public_default_ctor_neg() {
    return QueueItemLease{};  // ERROR: private default ctor
}
#endif

#ifdef NEG_LEASE_PUBLIC_CONTROL_CTOR
// There is NO public constructor from a raw QueueItemControl*; a raw control
// address cannot be upgraded into authority.
QueueItemLease raw_control_ctor_neg(QueueItemControl& c) {
    return QueueItemLease{c};  // ERROR: private control ctor
}
#endif

#ifdef NEG_CONTROL_PUBLIC_CTOR
// QueueItemControl's constructor is PRIVATE; downstream cannot mint a control.
QueueItemControl control_public_ctor_neg(QueuePort& p) {
    return QueueItemControl{p, nullptr, nullptr};  // ERROR: private ctor
}
#endif

#ifdef NEG_CONTROL_COPY
QueueItemControl control_copy_neg(const QueueItemControl& src) {
    return src;  // ERROR: deleted copy ctor
}
#endif

#ifdef NEG_CONTROL_MOVE
QueueItemControl control_move_neg(QueueItemControl& src) {
    return std::move(src);  // ERROR: deleted move ctor
}
#endif

int main() {
    // The probe's main is intentionally trivial: all P1 verification is
    // compile-time. The Queue functional test suite exercises runtime
    // lifecycle behavior through the public AsyncQueue<T> API.
    return 0;
}
