// sluice::async::detail::QueueItemControl / QueueItemLease — the E12-E Queue
// one-shot, move-only, unforgeable item ownership capability.
//
// This header installs the LINEAR-CAPABILITY FOUNDATION of the Queue
// (E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-2 §4). It defines:
//
//   QueueItemControl  - the per-item control block (location + identity), whose
//                       address is the abstract ItemId. Non-copyable,
//                       non-movable; its `location_` field is private and is
//                       mutated only by the QueuePort / teardown authority.
//   QueueItemLease    - the move-only, non-copyable linear capability that is
//                       the SOLE proof of authority to admit / transfer /
//                       release an item. Exactly one non-empty lease exists per
//                       live control; a move empties its source and requires
//                       an empty destination.
//
// ACCESS INVARIANTS (binding, enforced by type structure — not debug asserts):
//
//   * `QueueItemControl`'s constructor is PRIVATE; only QueuePort,
//     QueueTeardownSession, and QueueItemFactory (all in this namespace) can
//     create one. A downstream caller cannot mint a control from a raw
//     pointer, cannot mutate `location_`, cannot mutate `type_token_`, and
//     cannot reach the private QueuePort embedded in another `AsyncQueue<T>`.
//   * `QueueItemLease`'s ONLY constructors reachable to anyone but the
//     authority friends are the (implicit) private default ctor (empty) and
//     the public move ctor. There is NO public constructor from a raw
//     `QueueItemControl*`; NO public `adopt` / `reset` / `release_control`.
//     A raw control address therefore cannot be upgraded into authority.
//   * `QueueItemLease` move ctor uses `std::exchange` (empties source). Move
//     assignment REQUIRES the destination to be empty (fail-fast otherwise);
//     a live lease is never silently overwritten.
//   * A non-empty lease destructor is a fail-fast invariant violation: every
//     well-formed path transfers or releases the control before destruction.
//     The destructor NEVER deletes a type-erased node and NEVER invokes user
//     code (no `T` destructor here; typed deletion is the factory's job,
//     performed outside all Queue/Scheduler locks).
//
// This header is pure declarations + inline trivial definitions; the
// non-trivial out-of-line bodies (`~QueueItemLease`, `adopt_control`,
// `require_empty_or_terminate`) live in `src/async/queue_port.cpp`. The
// `QueueItemFactory` (typed node + make/release*) is defined alongside
// `QueuePort` in `queue_port.hpp`; it is only forward-declared here.
//
// The authoritative type graph is docs/e12-queue-scheduler-integration.md §4.
#pragma once

#include <sluice/async/detail/fail_fast.hpp>  // for the queue fail-fast prototype below

#include <cstdint>  // std::uint8_t
#include <utility>  // std::exchange

namespace sluice::async {
class Scheduler;  // forward
}  // namespace sluice::async

namespace sluice::async::detail {

class QueuePort;
class QueueOpaquePushResult;
class QueueOpaquePopResult;
class QueueTeardownSession;
class QueueItemFactory;

// Named fail-fast entry for Queue lease / control invariant violations
// (a live lease destroyed without release; a non-empty destination
// overwritten by move-assign). These are unrecoverable ownership bugs; the
// runtime cannot resume user execution while preserving one-shot-lease /
// ring-uniqueness / winner invariants. Same contract shape as
// async_mutex_lock_fail_fast: [[noreturn]] noexcept, no allocation / locking
// / I/O / formatting; ultimately std::terminate().
[[noreturn]] void queue_lease_fail_fast() noexcept;

// Type-token helper: a stable per-type sentinel address used to validate that
// a lease being released through `AsyncQueue<T>` was minted by an
// `AsyncQueue<T>` factory of the SAME `T` (a wrong-type release is fail-fast).
// Header-only (inline) because it is instantiated per-`T` at each
// AsyncQueue<T> use site; the address of a function-local static byte is a
// stable, per-instantiation, linkage-unique address with no heap and no
// thread-safety hazard beyond the trivial zero-init of one byte.
template <class T>
inline const void* queue_type_token() noexcept {
    static_assert(std::is_object_v<T>, "AsyncQueue<T> requires an object type");
    static const std::byte token{0};
    return &token;
}

class QueueItemControl final {
public:
    enum class Location : std::uint8_t {
        detached,
        producer_operation,
        ring,
        consumer_operation,
        teardown,
        released,
    };

private:
    QueuePort* const owner_port_;
    void* const typed_node_;
    const void* const type_token_;
    Location location_{Location::detached};

    explicit QueueItemControl(QueuePort& owner_port, void* typed_node,
                              const void* type_token) noexcept
        : owner_port_(&owner_port),
          typed_node_(typed_node),
          type_token_(type_token) {}

    QueueItemControl(const QueueItemControl&) = delete;
    QueueItemControl& operator=(const QueueItemControl&) = delete;
    QueueItemControl(QueueItemControl&&) = delete;
    QueueItemControl& operator=(QueueItemControl&&) = delete;

    friend class QueuePort;
    friend class QueueItemLease;
    friend class QueueTeardownSession;
    friend class QueueItemFactory;
    friend class ::sluice::async::Scheduler;  // E12-E admit closures commit
                                              // location_ under G+S+role
};

class QueueItemLease final {
public:
    QueueItemLease(QueueItemLease&& other) noexcept
        : control_(std::exchange(other.control_, nullptr)) {}

    QueueItemLease& operator=(QueueItemLease&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        // Binding precondition: the destination is empty. Violation is
        // fail-fast; a live lease is never silently overwritten.
        require_empty_or_terminate();
        control_ = std::exchange(other.control_, nullptr);
        return *this;
    }

    QueueItemLease(const QueueItemLease&) = delete;
    QueueItemLease& operator=(const QueueItemLease&) = delete;

    // A non-empty lease at destruction is an ownership-program bug (a live
    // item was neither transferred to the ring, returned to the caller, nor
    // released). Fail-fast; never deletes a node, never invokes user code.
    ~QueueItemLease() noexcept;

    explicit operator bool() const noexcept { return control_ != nullptr; }

private:
    QueueItemControl* control_{nullptr};

    QueueItemLease() noexcept = default;
    explicit QueueItemLease(QueueItemControl& control) noexcept
        : control_(&control) {}

    // Returns the control pointer and empties this lease. Authority-only.
    QueueItemControl* release_control() noexcept {
        return std::exchange(control_, nullptr);
    }

    // Adopts an already-owned control into this (empty) lease. Authority-only.
    void adopt_control(QueueItemControl& control) noexcept;

    void require_empty_or_terminate() const noexcept;

    friend class QueuePort;
    friend class QueueOpaquePushResult;
    friend class QueueOpaquePopResult;
    friend class QueueTeardownSession;
    friend class QueueItemFactory;
    friend class ::sluice::async::Scheduler;  // E12-E admit closures read/
                                              // mutate control_ custody
};

}  // namespace sluice::async::detail
