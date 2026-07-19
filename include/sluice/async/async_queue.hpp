// sluice::async::AsyncQueue<T> — Fiber-suspending async bounded MPMC FIFO
// channel (sluice-CORE-E12-E).
//
// The public typed wrapper over the non-template QueuePort authority
// (docs/e12-queue-scheduler-integration.md, Corrective-2 §3/§5). It is a THIN
// typed layer: it converts the opaque QueuePort results to typed ones, drives
// the QueueItemFactory to mint/recover the exact `Node<T>`, and destroys that
// `Node<T>` OUTSIDE all locks. It performs NO synchronization, holds NO
// Scheduler-internal state, and does NOT name the Scheduler's private seams.
//
// Why a non-template core + thin template:
//   - The non-template QueuePort (one translation unit, src/async/queue_port.cpp)
//     owns ALL synchronization: the fixed ring, lifecycle/close, counters, the
//     G -> S -> exactly-one-role lock order, the Scheduler reconcile/grant
//     seams, the timer model, and the teardown authority. A single reviewable
//     surface for the correctness-critical core.
//   - AsyncQueue<T> is header-only: every typed operation is a thin forward
//     (make lease -> QueuePort seam -> convert opaque result -> release T
//     outside locks). No template instantiation bloats the .cpp; the core
//     compiles once.
//
// Authority (sealed): a downstream caller CANNOT:
//   - name or construct `Node<T>` (private nested in QueueItemFactory),
//   - reach the private QueuePort embedded in another `AsyncQueue<T>`,
//   - mint a lease from a raw control pointer (no public ctor),
//   - reconstruct a typed T from an opaque result without the matching port
//     AND type-token (release_* validates owner_port_ + queue_type_token<T>()
//     + expected Location at every boundary).
//
// Scheduler binding: AsyncQueue<T> borrows Scheduler& for its lifetime. The
// Scheduler must outlive the AsyncQueue. push/pop/push_until/pop_until
// require a currently running Fiber (g_worker->current); try_push/try_pop/
// close/snapshots/begin_teardown do not suspend.
//
// Destruction contract (mirrors QueuePort §6): the ring must be empty and the
// teardown session must have completed before ~AsyncQueue<T> runs. The
// destructor does NOT auto-drain, does NOT cancel waiters, and does NOT
// synthesize closed outcomes. Caller contract violation surfaces as
// queue_lease_fail_fast (std::terminate) in the embedded QueuePort
// destructor. Concurrent AsyncQueue<T> destruction while operations are in
// flight remains a caller contract violation; the QueuePort counters do not
// make arbitrary caller lifetime safe.
//
// Type requirements on T (mirrors QueueItemFactory::make<T>):
//   - object type
//   - nothrow-move-constructible (one T move per push / per pop release)
//   - nothrow-destructible
// T need NOT be default-constructible or move-assignable: the typed results
// use std::optional<T> storage (the runtime is nothrow-move-constructible +
// nothrow-destructible + nothrow default-constructed-empty), and the factory
// moves T exactly once at the typed conversion boundary.
//
// Misuse contracts (debug-grade; surfaces as std::terminate via
// queue_lease_fail_fast):
//   - capacity == 0                       -> ctor throws std::invalid_argument
//   - ordinary op after begin_teardown    -> fail-fast before CallGuard
//   - second begin_teardown               -> fail-fast (lifecycle != operational)
//   - QueueTeardownSession dtor with a
//     non-empty ring                      -> fail-fast
#pragma once

#include <cstddef>
#include <new>        // std::bad_alloc / std::invalid_argument propagation
#include <optional>
#include <stdexcept>  // std::invalid_argument (capacity == 0)
#include <type_traits>
#include <utility>

#include <sluice/async/detail/queue_port.hpp>  // QueuePort + opaque results + factory
#include <sluice/async/scheduler.hpp>          // Scheduler (full def for inline calls)

namespace sluice::async {

// Public typed push status. Aliased to the opaque status: the status values
// are type-erased (only the payload T is typed), so the underlying enum is
// shared.
using QueuePushStatus = detail::QueueOpaquePushStatus;
using QueuePopStatus = detail::QueueOpaquePopStatus;

// Public typed push result. Owns a T payload iff status != committed
// (committed / closed / expired / would_block; committed carries no payload,
// the three failure statuses each carry the exact original T). Move-only.
//
// Storage is std::optional<T> so T need not be default-constructible or
// move-assignable: the move-ctor moves the optional whole (one T
// move-construct), and the move-ASSIGN operator is HAND-WRITTEN with a
// destroy-and-rebuild sequence (reset + emplace) rather than `= default`.
// `= default` would delegate to std::optional<T>::operator=(optional&&),
// whose SFINAE requires T to be move-assignable — that would exclude every
// T that satisfies the documented P8 contract (object +
// nothrow-move-constructible + nothrow-destructible) but is not
// move-assignable. The hand-written form preserves the contract.
template <class T>
class QueuePushResult final {
public:
    static QueuePushResult committed() noexcept {
        return QueuePushResult{QueuePushStatus::committed, std::nullopt};
    }
    // failed(): the caller provides the EXACT recovered T (the factory moved it
    // once from the Node<T> outside locks). A `committed` status is rejected
    // here — committed carries no payload.
    static QueuePushResult failed(QueuePushStatus s, T&& value) noexcept {
        if (s == QueuePushStatus::committed) {
            detail::queue_lease_fail_fast();
        }
        return QueuePushResult{s, std::move(value)};
    }

    QueuePushResult(QueuePushResult&&) noexcept = default;
    // PR #12 review corrective: hand-written destroy-and-rebuild so T need NOT
    // be move-assignable (only move-constructible + destructible). Steps:
    //   1. self-move guard;
    //   2. destroy destination payload (value_.reset());
    //   3. copy enum status;
    //   4. if source has payload, emplace(std::move(*src)) into destination,
    //      then reset source (single T move-construct, single T destruct).
    QueuePushResult& operator=(QueuePushResult&& other) noexcept {
        if (this == &other) return *this;
        value_.reset();
        status_ = other.status_;
        if (other.value_.has_value()) {
            value_.emplace(std::move(*other.value_));
            other.value_.reset();
        }
        return *this;
    }
    QueuePushResult(const QueuePushResult&) = delete;
    QueuePushResult& operator=(const QueuePushResult&) = delete;
    ~QueuePushResult() = default;

    QueuePushStatus status() const noexcept { return status_; }

    // Recover the EXACT typed value from a failed push (closed / expired /
    // would_block). Only valid when status() != committed. After take, status()
    // is unchanged; the result is a valid moved-from no-payload shell.
    T take_value() && noexcept {
        if (!value_.has_value()) {
            detail::queue_lease_fail_fast();
        }
        return std::move(*value_);
    }

private:
    QueuePushResult(QueuePushStatus s, std::optional<T>&& v) noexcept
        : status_(s), value_(std::move(v)) {}
    QueuePushResult(QueuePushStatus s, T&& v) noexcept
        : status_(s), value_(std::move(v)) {}

    QueuePushStatus status_;
    std::optional<T> value_;
};

// Public typed pop result. Owns a T payload iff status == item; closed /
// expired / would_block carry no payload. Move-only. Same storage rationale
// and hand-written move-assign as QueuePushResult (PR #12 review corrective).
template <class T>
class QueuePopResult final {
public:
    static QueuePopResult item(T&& value) noexcept {
        return QueuePopResult{QueuePopStatus::item, std::move(value)};
    }
    static QueuePopResult closed() noexcept {
        return QueuePopResult{QueuePopStatus::closed, std::nullopt};
    }
    static QueuePopResult expired() noexcept {
        return QueuePopResult{QueuePopStatus::expired, std::nullopt};
    }
    static QueuePopResult would_block() noexcept {
        return QueuePopResult{QueuePopStatus::would_block, std::nullopt};
    }

    QueuePopResult(QueuePopResult&&) noexcept = default;
    // PR #12 review corrective: hand-written destroy-and-rebuild (see
    // QueuePushResult for the rationale — T need NOT be move-assignable).
    QueuePopResult& operator=(QueuePopResult&& other) noexcept {
        if (this == &other) return *this;
        value_.reset();
        status_ = other.status_;
        if (other.value_.has_value()) {
            value_.emplace(std::move(*other.value_));
            other.value_.reset();
        }
        return *this;
    }
    QueuePopResult(const QueuePopResult&) = delete;
    QueuePopResult& operator=(const QueuePopResult&) = delete;
    ~QueuePopResult() = default;

    QueuePopStatus status() const noexcept { return status_; }

    // Recover the EXACT typed value from a successful pop. Only valid when
    // status() == item. After take, the result is a valid moved-from shell.
    T take_value() && noexcept {
        if (!value_.has_value()) {
            detail::queue_lease_fail_fast();
        }
        return std::move(*value_);
    }

private:
    QueuePopResult(QueuePopStatus s, std::optional<T>&& v) noexcept
        : status_(s), value_(std::move(v)) {}
    QueuePopResult(QueuePopStatus s, T&& v) noexcept
        : status_(s), value_(std::move(v)) {}

    QueuePopStatus status_;
    std::optional<T> value_;
};

// A Fiber-suspending async bounded MPMC FIFO channel of move-only T.
//
// Non-copyable AND non-movable: the embedded QueuePort is non-movable
// (intrusive WaitQueues + borrowed Scheduler&; identity matters for wait
// resolution routing and for the embedded ring lease custody). An AsyncQueue
// is constructed once and lives at one address for its lifetime.
template <class T>
class AsyncQueue final {
    static_assert(std::is_object_v<T>, "AsyncQueue<T> requires object T");
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "AsyncQueue<T> requires nothrow-move-constructible T");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "AsyncQueue<T> requires nothrow-destructible T");

public:
    // Construct an AsyncQueue<T> bound to `scheduler` with a fixed ring of
    // `capacity` slots. capacity >= 1 (capacity 0 is rejected with
    // std::invalid_argument by the embedded QueuePort). The Scheduler must
    // outlive the AsyncQueue.
    explicit AsyncQueue(Scheduler& scheduler, std::size_t capacity)
        : port_(scheduler, capacity) {}

    ~AsyncQueue() = default;

    AsyncQueue(const AsyncQueue&) = delete;
    AsyncQueue& operator=(const AsyncQueue&) = delete;
    AsyncQueue(AsyncQueue&&) = delete;
    AsyncQueue& operator=(AsyncQueue&&) = delete;

    // --- fast paths (no Scheduler suspend) ---

    // Try to push `value` without suspending. Outcomes:
    //   committed    -> value entered the ring (or was granted to a parked
    //                   consumer directly)
    //   would_block  -> ring full AND no older eligible producer; value is
    //                   returned to the caller unchanged
    //   closed       -> the Queue is closed; value is returned to the caller
    //                   unchanged (no copy / alias / default)
    // The factory mints the typed Node<T> OUTSIDE locks; the QueuePort seam
    // moves the lease whole into the ring slot or back into the failed result.
    // On a failure status, take_value() recovers the exact original T (one T
    // move from the recovered Node<T>, destroyed outside locks).
    [[nodiscard]] QueuePushResult<T> try_push(T value) {
        detail::QueueItemLease lease =
            detail::QueueItemFactory::make<T>(port_, std::move(value));
        detail::QueueOpaquePushResult r = port_.try_push(std::move(lease));
        return from_opaque_push_(std::move(r));
    }

    // Try to pop one item without suspending. Outcomes:
    //   item        -> one buffered (or producer-direct) item moved out
    //   would_block -> ring empty AND open
    //   closed      -> ring empty AND closed (terminal)
    [[nodiscard]] QueuePopResult<T> try_pop() {
        detail::QueueOpaquePopResult r = port_.try_pop();
        return from_opaque_pop_(std::move(r));
    }

    // Monotone Open -> Closed. Idempotent on Closed. Drains both role FIFOs:
    //   - each parked consumer is granted one buffered item until the ring is
    //     empty, then closed+empty
    //   - each parked producer is granted `closed` with its lease retained
    void close() noexcept { port_.close(); }

    // --- snapshot projections (lock-free or briefly-locked observation) ---

    [[nodiscard]] bool is_closed() const noexcept { return port_.is_closed(); }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return port_.capacity();
    }
    [[nodiscard]] std::size_t size() const noexcept { return port_.size(); }

    // --- blocking / timed (require a currently running Fiber) ---

    // Blocking push. Suspends the calling Fiber if the ring is full (or the
    // close-race blocks it); resumes when a consumer frees a slot (committed)
    // or close completes the wait with `closed` (the exact original T is
    // retained).
    [[nodiscard]] QueuePushResult<T> push(T value) {
        detail::QueueItemLease lease =
            detail::QueueItemFactory::make<T>(port_, std::move(value));
        detail::QueueOpaquePushResult r = port_.push(std::move(lease));
        return from_opaque_push_(std::move(r));
    }

    // Deadline-aware push. Resolves when EXACTLY ONE cause wins:
    //   committed (a slot was granted)
    //   closed    (close completed the wait)
    //   expired   (the deadline elapsed with no slot and no close)
    [[nodiscard]] QueuePushResult<T> push_until(T value,
                                                Scheduler::deadline_t deadline) {
        detail::QueueItemLease lease =
            detail::QueueItemFactory::make<T>(port_, std::move(value));
        detail::QueueOpaquePushResult r =
            port_.push_until(std::move(lease), deadline);
        return from_opaque_push_(std::move(r));
    }

    // Blocking pop. Suspends the calling Fiber if the ring is empty; resumes
    // when a producer commits an item (granted directly) or close completes
    // the wait with `closed` (empty ring).
    [[nodiscard]] QueuePopResult<T> pop() {
        detail::QueueOpaquePopResult r = port_.pop();
        return from_opaque_pop_(std::move(r));
    }

    // Deadline-aware pop. Resolves when EXACTLY ONE cause wins:
    //   item    (a producer committed an item, granted directly or buffered)
    //   closed  (close completed the wait with an empty ring)
    //   expired (the deadline elapsed with no item and no close)
    [[nodiscard]] QueuePopResult<T> pop_until(Scheduler::deadline_t deadline) {
        detail::QueueOpaquePopResult r = port_.pop_until(deadline);
        return from_opaque_pop_(std::move(r));
    }

    // --- teardown (irreversible) ---

    // Begin the irreversible operational -> tearing_down transition. The
    // returned session is the unique authority to drain the remaining ring
    // items. See QueuePort::begin_teardown for the full precondition set
    // (all four counters zero, both role FIFOs empty, lifecycle operational).
    // Once torn down, the AsyncQueue admits no further ordinary op; a second
    // begin_teardown fail-fasts.
    detail::QueueTeardownSession begin_teardown() noexcept {
        return port_.begin_teardown();
    }

    // Recover the exact typed T from a teardown-drained lease. The session
    // (which this AsyncQueue minted via begin_teardown) yields ring slots one-
    // by-one via take_next (ring -> teardown); this helper moves T once, marks
    // the control released, and destroys the exact Node<T> OUTSIDE all locks.
    // The factory re-validates owner_port_ + type-token + location at release.
    T release_teardown(detail::QueueTeardownSession& session) noexcept {
        detail::QueueItemLease lease = session.take_next();
        return detail::QueueItemFactory::release_teardown<T>(port_,
                                                             std::move(lease));
    }

private:
    // Convert an opaque push result to a typed one. The committed path carries
    // no lease; the three failure paths each carry exactly one lease, which is
    // released via the factory (validates port + type-token + location, moves
    // T once, marks released, deletes Node<T> outside locks). All factory work
    // happens HERE, after QueuePort has returned and its CallGuard has
    // decremented active_port_calls_ (typed conversion is explicitly NOT
    // counted — see docs/e12-queue-scheduler-integration.md §7).
    QueuePushResult<T> from_opaque_push_(detail::QueueOpaquePushResult&& r) {
        using S = detail::QueueOpaquePushStatus;
        const S s = r.status();
        if (s == S::committed) {
            return QueuePushResult<T>::committed();
        }
        detail::QueueItemLease lease = std::move(r).take_failed_lease();
        T value =
            detail::QueueItemFactory::release_failed<T>(port_, std::move(lease));
        return QueuePushResult<T>::failed(s, std::move(value));
    }

    // Convert an opaque pop result to a typed one. Only the item path carries a
    // lease; closed/expired/would_block carry none.
    QueuePopResult<T> from_opaque_pop_(detail::QueueOpaquePopResult&& r) {
        using S = detail::QueueOpaquePopStatus;
        const S s = r.status();
        if (s != S::item) {
            switch (s) {
                case S::closed:
                    return QueuePopResult<T>::closed();
                case S::expired:
                    return QueuePopResult<T>::expired();
                default:
                    return QueuePopResult<T>::would_block();
            }
        }
        detail::QueueItemLease lease = std::move(r).take_item_lease();
        T value =
            detail::QueueItemFactory::release_popped<T>(port_, std::move(lease));
        return QueuePopResult<T>::item(std::move(value));
    }

    detail::QueuePort port_;
};

}  // namespace sluice::async
