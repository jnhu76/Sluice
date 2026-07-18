// sluice::async::detail — E12-E Queue non-template authority implementation.
//
// This translation unit owns the non-template Queue authority and lifecycle
// transitions (docs/e12-queue-scheduler-integration.md). P1 scope: only the
// lease/control linear-capability bodies and the fail-fast / type-token
// helpers are defined here yet; the QueuePort state machine, ring, lifecycle,
// and Scheduler seams land in subsequent commits (P2 onward).
//
// All out-of-line Queue work lives here so the Scheduler TU keeps a single
// Queue reconciliation surface and the Queue authority is reviewable in one
// place. Typed `Node<T>` and the public `AsyncQueue<T>` are header-only.
#include <sluice/async/detail/queue_item.hpp>

#include <cstdlib>
#include <exception>  // std::terminate
#include <new>
#include <type_traits>

namespace sluice::async::detail {

// ---------------------------------------------------------------------------
// Fail-fast entry for Queue lease/control invariant violations.
//
// Contract (mirrors async_mutex_lock_fail_fast, ASYNC-MUTEX-NOTHROW-AUTHORITY-1
// §D2; the Queue design binds the same no-throw winner region to this
// substrate): [[noreturn]] noexcept; no allocation / locking / I/O / virtual
// dispatch / dynamic string; ultimately terminates the process. We do not
// attempt to recover Scheduler / Queue state.
// ---------------------------------------------------------------------------
[[noreturn]] void queue_lease_fail_fast() noexcept {
    // std::terminate is the documented process terminator; a direct
    // std::quick_exit/fast_exit would skip destructors the runtime may rely
    // on for diagnostics. Keep this path free of formatting.
    std::terminate();
}

// ---------------------------------------------------------------------------
// QueueItemLease out-of-line bodies.
//
// The destructor and require_empty checks are the runtime backstop for the
// one-shot lease invariant. They NEVER delete a node or invoke user code;
// typed deletion is QueueItemFactory's job (outside all locks).
// ---------------------------------------------------------------------------
void QueueItemLease::require_empty_or_terminate() const noexcept {
    if (control_ != nullptr) {
        queue_lease_fail_fast();
    }
}

void QueueItemLease::adopt_control(QueueItemControl& control) noexcept {
    // adopt_control is authority-only and is only ever called on an empty
    // lease (the QueuePort result/teardown paths construct a fresh lease).
    // The defensive check is cheap and keeps the invariant locally visible.
    require_empty_or_terminate();
    control_ = &control;
}

QueueItemLease::~QueueItemLease() noexcept {
    if (control_ != nullptr) {
        // A live lease reached destruction without being transferred to the
        // ring, returned to the caller, or released. This is an ownership-
        // program bug; fail-fast rather than leak or silently drop a lease
        // (which would break ring uniqueness / no-lost-item invariants).
        queue_lease_fail_fast();
    }
}

// ---------------------------------------------------------------------------
// Type-token helper. A stable per-type sentinel address; `queue_type_token<T>`
// yields the same address for every call with the same T and a distinct
// address for distinct T. Used to validate that a lease released through
// AsyncQueue<T> was minted by an AsyncQueue<T> factory of the same T.
//
// Implementation note: the address of a function-local static `std::byte` is a
// stable, per-instantiation linkage-unique address, with no heap and no
// thread-safety hazard beyond the trivial zero-init of a single byte.
// ---------------------------------------------------------------------------
template <class T>
const void* queue_type_token() noexcept {
    static_assert(std::is_object_v<T>,
                  "AsyncQueue<T> requires an object type");
    static const std::byte token{0};
    return &token;
}

// Explicit instantiations are unnecessary: queue_type_token<T> is instantiated
// on demand by QueueItemFactory::Node<T> (header-only) for each T the user
// instantiates AsyncQueue<T> with.

}  // namespace sluice::async::detail
