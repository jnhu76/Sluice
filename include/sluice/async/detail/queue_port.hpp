// sluice::async::detail::QueuePort — the E12-E Queue fixed non-template
// Scheduler boundary (Corrective-2 §3/§5/§6).
//
// This header installs the NON-TEMPLATE Queue authority surface:
//
//   QueueOpaquePushStatus / QueueOpaquePushResult
//   QueueOpaquePopStatus  / QueueOpaquePopResult
//   QueueItemFactory        (typed Node<T> + make/release*)
//   QueueLifecycle          (operational | tearing_down)
//   QueueTeardownSession    (irreversible, unique, move-only)
//   QueuePort               (the only Queue friend of Scheduler)
//
// `QueuePort` owns the ring + counters + lifecycle + the (P5/P6) Scheduler
// seams. `AsyncQueue<T>` (in async_queue.hpp) is a thin template wrapper that
// converts opaque results to typed ones and destroys the exact `Node<T>`
// OUTSIDE all locks. The typed layer cannot name `Node<T>`, cannot reach the
// private QueuePort embedded in another `AsyncQueue<T>`, and cannot mint a
// lease from a raw control pointer.
//
// P2 scope: the type graph, ring, lifecycle/close state, counters, opaque
// results, teardown session, and factory are defined here. The blocking/timed
// wait-admission paths, Scheduler reconciliation, and publication land in P4-P6
// (they are declared but defer to the Scheduler). The fast paths (try_push /
// try_pop / close / failed-payload return) are P3.
//
// Authority: docs/e12-queue-scheduler-integration.md (Corrective-2 §4-§9).
#pragma once

#include <sluice/async/detail/queue_item.hpp>
#include <sluice/async/mutex.hpp>            // sluice::async::Mutex (state_mtx_)
#include <sluice/async/timer_registration.hpp>  // deadline_tick_t
#include <sluice/async/wait_node.hpp>        // WaitNode (admission node)
#include <sluice/async/wait_queue.hpp>       // WaitQueue (role waiter FIFOs)

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace sluice::async {
class Scheduler;  // forward — QueuePort borrows Scheduler&; deadline_t alias below
}  // namespace sluice::async

namespace sluice::async::detail {

// The push_until / pop_until deadline. Mirrors Scheduler::deadline_t
// (== deadline_tick_t). The Scheduler forward-declaration is sufficient for
// the QueuePort declarations; the .cpp includes the full scheduler.hpp.
using queue_deadline_t = ::sluice::async::deadline_tick_t;

// Waiter role: which of the two role FIFOs a Queue wait epoch belongs to.
// Producer waiters park on push/push_until when the ring is full / closed-
// race-blocked; consumer waiters park on pop/pop_until when the ring is empty.
// The two role mutexes are NEVER held together (lock order G -> S -> exactly
// one role).
enum class QueueRole : std::uint8_t {
    producer = 0,
    consumer = 1,
};

// ---------------------------------------------------------------------------
// Opaque push result (non-template push seam output).
//
// Invariant (checked at every factory and move boundary):
//   status == committed  <=> lease is empty
//   status != committed  <=> lease is non-empty
//
// Move assignment swaps complete valid states; it never overwrites a live
// lease. `take_failed_lease()` leaves a valid moved-from no-payload state.
// ---------------------------------------------------------------------------
enum class QueueOpaquePushStatus : std::uint8_t {
    committed,
    closed,
    expired,
    would_block,
};

class QueueOpaquePushResult final {
public:
    static QueueOpaquePushResult committed() noexcept {
        return QueueOpaquePushResult{QueueOpaquePushStatus::committed,
                                     QueueItemLease{}};
    }
    static QueueOpaquePushResult failed(QueueOpaquePushStatus s,
                                        QueueItemLease&& l) noexcept {
        // A `committed` result carries no lease; any failure status carries
        // exactly one non-empty lease. Enforce at construction.
        if (s == QueueOpaquePushStatus::committed) {
            queue_lease_fail_fast();
        }
        if (!static_cast<bool>(l)) {
            queue_lease_fail_fast();
        }
        return QueueOpaquePushResult{s, std::move(l)};
    }

    QueueOpaquePushResult(QueueOpaquePushResult&& other) noexcept
        : status_(other.status_), lease_(std::move(other.lease_)) {}

    QueueOpaquePushResult& operator=(QueueOpaquePushResult&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        // destroy-and-move-construct: the existing lease (if any) must be
        // empty here (a live result holds its lease until take_failed_lease
        // or destruction). Reuse the lease's own move-assign empty-dest rule.
        lease_ = std::move(other.lease_);
        status_ = other.status_;
        return *this;
    }

    QueueOpaquePushResult(const QueueOpaquePushResult&) = delete;
    QueueOpaquePushResult& operator=(const QueueOpaquePushResult&) = delete;

    ~QueueOpaquePushResult() noexcept = default;  // lease_ enforces its own rule

    QueueOpaquePushStatus status() const noexcept { return status_; }

    QueueItemLease take_failed_lease() && noexcept {
        // After take, status remains its original failure status but the
        // lease is empty — a valid moved-from no-payload state. Callers MUST
        // have read status() before calling take; the typed layer does.
        return std::move(lease_);
    }

private:
    QueueOpaquePushStatus status_;
    QueueItemLease lease_;

    explicit QueueOpaquePushResult(QueueOpaquePushStatus s,
                                   QueueItemLease&& l) noexcept
        : status_(s), lease_(std::move(l)) {}

    friend class QueuePort;
    friend class QueueItemFactory;
};

// ---------------------------------------------------------------------------
// Opaque pop result (symmetric move-only pair). Owns a non-empty lease iff
// status == item; closed/expired/would_block carry no lease.
// ---------------------------------------------------------------------------
enum class QueueOpaquePopStatus : std::uint8_t {
    item,
    closed,
    expired,
    would_block,
};

class QueueOpaquePopResult final {
public:
    static QueueOpaquePopResult item(QueueItemLease&& l) noexcept {
        if (!static_cast<bool>(l)) {
            queue_lease_fail_fast();
        }
        return QueueOpaquePopResult{QueueOpaquePopStatus::item, std::move(l)};
    }
    static QueueOpaquePopResult closed() noexcept {
        return QueueOpaquePopResult{QueueOpaquePopStatus::closed,
                                    QueueItemLease{}};
    }
    static QueueOpaquePopResult expired() noexcept {
        return QueueOpaquePopResult{QueueOpaquePopStatus::expired,
                                    QueueItemLease{}};
    }
    static QueueOpaquePopResult would_block() noexcept {
        return QueueOpaquePopResult{QueueOpaquePopStatus::would_block,
                                    QueueItemLease{}};
    }

    QueueOpaquePopResult(QueueOpaquePopResult&& other) noexcept
        : status_(other.status_), lease_(std::move(other.lease_)) {}

    QueueOpaquePopResult& operator=(QueueOpaquePopResult&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        lease_ = std::move(other.lease_);
        status_ = other.status_;
        return *this;
    }

    QueueOpaquePopResult(const QueueOpaquePopResult&) = delete;
    QueueOpaquePopResult& operator=(const QueueOpaquePopResult&) = delete;
    ~QueueOpaquePopResult() noexcept = default;

    QueueOpaquePopStatus status() const noexcept { return status_; }

    QueueItemLease take_item_lease() && noexcept {
        return std::move(lease_);
    }

private:
    QueueOpaquePopStatus status_;
    QueueItemLease lease_;

    explicit QueueOpaquePopResult(QueueOpaquePopStatus s,
                                  QueueItemLease&& l) noexcept
        : status_(s), lease_(std::move(l)) {}

    friend class QueuePort;
    friend class QueueItemFactory;
};

// ---------------------------------------------------------------------------
// QueueItemFactory — the fixed, non-template typed-node factory.
//
// Hides the exact `Node<T>` (control + value co-located, one allocation) and
// embeds the control so their addresses/lifetimes are related. `make<T>`
// allocates OUTSIDE internal locks and returns the only lease. The three
// release functions validate owner_port + type_token + expected location,
// change location to released, empty the lease, and use the exact `Node<T>`
// static type for move/destruction OUTSIDE all locks.
//
// A downstream caller cannot name or construct `Node<T>`, cannot reach the
// private QueuePort inside another `AsyncQueue<T>`, and cannot mint authority
// from a raw control pointer.
// ---------------------------------------------------------------------------
class QueueItemFactory final {
public:
    template <class T, class U>
    static QueueItemLease make(QueuePort& port, U&& value) {
        static_assert(std::is_object_v<T>, "AsyncQueue<T> requires object T");
        static_assert(std::is_nothrow_move_constructible_v<T>,
                      "AsyncQueue<T> requires nothrow-move-constructible T");
        static_assert(std::is_nothrow_destructible_v<T>,
                      "AsyncQueue<T> requires nothrow-destructible T");
        // Allocation + the user's T constructor run OUTSIDE internal locks.
        // The node owns its embedded control; its address is the ItemId.
        Node<T>* n = new Node<T>(port, std::forward<U>(value));
        return QueueItemLease{n->control_};
    }

    // Recover the exact typed value from a failed push (closed/expired/
    // would_block). Validates port + type-token + location, moves T exactly
    // once into the return value, changes location -> released, empties the
    // lease, and deletes the exact Node<T> OUTSIDE all locks.
    template <class T>
    static T release_failed(QueuePort& port, QueueItemLease&& lease) noexcept {
        return release_typed_<T>(port, std::move(lease),
                                 QueueItemControl::Location::producer_operation,
                                 /*from_detached_via_producer_op=*/true);
    }

    // Recover the exact typed value from a successful pop. The control was at
    // consumer_operation; move T once, released, delete Node<T> outside locks.
    template <class T>
    static T release_popped(QueuePort& port,
                            QueueItemLease&& lease) noexcept {
        return release_typed_<T>(port, std::move(lease),
                                 QueueItemControl::Location::consumer_operation,
                                 /*from_detached_via_producer_op=*/false);
    }

    // Release a teardown-drained item (control was at teardown). Move T once
    // for the caller, released, delete Node<T> outside locks.
    template <class T>
    static T release_teardown(QueuePort& port,
                              QueueItemLease&& lease) noexcept {
        return release_typed_<T>(port, std::move(lease),
                                 QueueItemControl::Location::teardown,
                                 /*from_detached_via_producer_op=*/false);
    }

private:
    static QueueItemControl make_control(QueuePort& port, void* typed_node,
                                         const void* type_token) noexcept {
        return QueueItemControl{port, typed_node, type_token};
    }

    template <class T>
    class Node final {
    private:
        template <class U>
        explicit Node(QueuePort& port, U&& value)
            : control_(QueueItemFactory::make_control(
                  port, this, queue_type_token<T>())),
              value_(std::forward<U>(value)) {}

        QueueItemControl control_;
        T value_;

        friend class QueueItemFactory;
    };

    // Shared typed-extraction core: validate identity + location, move T once,
    // mark released, delete the exact Node<T>. The lease is emptied by the
    // control lookup; the Node is destroyed OUTSIDE any Queue/Scheduler lock.
    template <class T>
    static T release_typed_(QueuePort& port, QueueItemLease&& lease,
                            QueueItemControl::Location expected,
                            bool allow_detached) noexcept {
        QueueItemControl* c = lease.release_control();
        if (c == nullptr) {
            queue_lease_fail_fast();
        }
        // Identity: the control must belong to this port and the right type.
        const bool loc_ok = (c->location_ == expected) ||
                            (allow_detached &&
                             c->location_ == QueueItemControl::Location::detached);
        if (c->owner_port_ != &port || c->type_token_ != queue_type_token<T>() ||
            !loc_ok) {
            queue_lease_fail_fast();
        }
        // Recover the exact Node<T> via the control's typed_node_ back-pointer
        // (stored at make_control time = `this` of the Node). This is the
        // design's authority path — no reinterpret_cast / layout assumption.
        Node<T>* node = static_cast<Node<T>*>(c->typed_node_);
        // Mark released BEFORE moving T (T is nothrow-move-constructible by
        // static_assert; the ordering is defensive).
        c->location_ = QueueItemControl::Location::released;
        T value = std::move(node->value_);
        delete node;
        return value;
    }

    friend class QueueItemControl;
    friend class QueueItemLease;
    friend class QueuePort;
};

// ---------------------------------------------------------------------------
// QueueLifecycle — distinct from Queue close state. Both lifecycle and close
// are stored on the QueuePort. `tearing_down` is absorbing and rejects every
// ordinary QueuePort operation (including close).
// ---------------------------------------------------------------------------
enum class QueueLifecycle : std::uint8_t {
    operational,
    tearing_down,
};

class QueueTeardownSession final {
public:
    QueueTeardownSession(QueueTeardownSession&& other) noexcept
        : port_(std::exchange(other.port_, nullptr)) {}

    QueueTeardownSession& operator=(QueueTeardownSession&&) = delete;
    QueueTeardownSession(const QueueTeardownSession&) = delete;
    QueueTeardownSession& operator=(const QueueTeardownSession&) = delete;

    ~QueueTeardownSession() noexcept;

    // Move one non-empty ring slot into the result (ring -> teardown). Source
    // slot becomes empty. Returns an empty lease when the ring is empty.
    QueueItemLease take_next() noexcept;

    bool empty() const noexcept;

private:
    QueuePort* port_{nullptr};

    explicit QueueTeardownSession(QueuePort& port) noexcept : port_(&port) {}

    friend class QueuePort;
};

// ---------------------------------------------------------------------------
// QueuePort — the fixed non-template Scheduler friend. Owns the ring,
// counters, lifecycle, close state, and the (P5/P6) Scheduler seams.
//
// P2 scope: structural shape, ring, lifecycle, close, counters, teardown
// session. Fast paths (try_push / try_pop / close) are P3. Blocking/timed
// wait admission and Scheduler reconciliation are P4-P6.
// ---------------------------------------------------------------------------
class QueuePort final {
public:
    explicit QueuePort(Scheduler& sched, std::size_t capacity);
    ~QueuePort();

    QueuePort(const QueuePort&) = delete;
    QueuePort& operator=(const QueuePort&) = delete;
    QueuePort(QueuePort&&) = delete;
    QueuePort& operator=(QueuePort&&) = delete;

    // --- fast paths (P3) ---
    QueueOpaquePushResult try_push(QueueItemLease lease);
    QueueOpaquePopResult try_pop();
    void close() noexcept;

    // --- snapshot projections (CallGuard-covered; P3) ---
    bool is_closed() const noexcept;
    std::size_t capacity() const noexcept;
    std::size_t size() const noexcept;

    // --- blocking / timed (P4-P6: declared, deferred) ---
    QueueOpaquePushResult push(QueueItemLease lease);
    QueueOpaquePushResult push_until(QueueItemLease lease,
                                     queue_deadline_t deadline);
    QueueOpaquePopResult pop();
    QueueOpaquePopResult pop_until(queue_deadline_t deadline);

    // --- teardown (P7) ---
    QueueTeardownSession begin_teardown() noexcept;

private:
    Scheduler& scheduler_;
    const std::size_t capacity_;
    // Fixed ring: capacity_ slots, each an empty-or-non-empty move-only lease.
    // Allocated once at construction. A non-empty slot's control location is
    // `ring`. ItemIds are unique by the one-shot-lease invariant.
    std::unique_ptr<QueueItemLease[]> ring_;
    std::size_t ring_head_{0};   // pop index (oldest item)
    std::size_t ring_count_{0};  // number of non-empty slots

    // State (all mutated under state_mtx_ + global_mtx_ per lock order).
    // state_mtx_ is the Queue's own structural lock; global_mtx_ is the
    // Scheduler coordination domain (acquired BEFORE state_mtx_ when both
    // are held: G -> S -> exactly one role WaitQueue).
    Mutex state_mtx_;
    QueueLifecycle lifecycle_{QueueLifecycle::operational};
    bool closed_{false};

    // Two role waiter FIFOs. Producer waiters park on push/push_until when
    // the ring is full or the close race blocks them; consumer waiters park
    // on pop/pop_until when the ring is empty. The two role mutexes are
    // NEVER held together. The Scheduler resolves the FIFO head of a role
    // under G + role.mtx() (the canonical seam). No public wait_queue()
    // accessor (sealed authority — mirrors Semaphore/AsyncMutex).
    WaitQueue waiters_[2];  // [producer, consumer]

    // Counter ledger (§7). active_port_calls_ counts ordinary QueuePort
    // call intervals ONLY (not typed conversion / arbitrary callers).
    std::size_t active_port_calls_{0};
    std::size_t active_wait_associations_{0};
    std::size_t active_queue_timers_{0};
    std::size_t granted_not_resumed_{0};

    // --- internal helpers (ring + lifecycle primitives) ---
    bool ring_empty_locked() const noexcept { return ring_count_ == 0; }
    bool ring_full_locked() const noexcept { return ring_count_ == capacity_; }
    std::size_t ring_slot(std::size_t logical_index) const noexcept {
        return (ring_head_ + logical_index) % capacity_;
    }
    WaitQueue& role_queue(QueueRole r) noexcept { return waiters_[static_cast<std::size_t>(r)]; }

    // CallGuard: ordinary QueuePort entry/return bracket. The guard enters
    // after the lifecycle check and decrements on every return path.
    struct CallGuard;
    friend struct CallGuard;

    friend class QueueItemFactory;
    friend class QueueItemControl;
    friend class QueueItemLease;
    friend class QueueTeardownSession;
    friend class ::sluice::async::Scheduler;
};

}  // namespace sluice::async::detail
