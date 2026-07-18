// sluice::async::detail — E12-E Queue non-template authority implementation.
//
// This translation unit owns the non-template Queue authority and lifecycle
// transitions (docs/e12-queue-scheduler-integration.md). P2+P3 scope: the
// QueuePort structural skeleton (ring, lifecycle, close, counters), the
// ordinary CallGuard, the fast paths (try_push / try_pop / close / snapshot),
// and the QueueTeardownSession. The blocking/timed wait-admission paths
// (push / push_until / pop / pop_until) and the Scheduler reconciliation /
// publication are declared but throw std::logic_error until P4-P6 land.
//
// All out-of-line Queue work lives here so the Scheduler TU keeps a single
// Queue reconciliation surface and the Queue authority is reviewable in one
// place. Typed `Node<T>` and the public `AsyncQueue<T>` are header-only.
#include <sluice/async/detail/queue_item.hpp>
#include <sluice/async/detail/queue_port.hpp>
#include <sluice/async/lock_guard.hpp>  // LockGuard

#include <cstdlib>
#include <exception>  // std::terminate
#include <new>        // std::bad_alloc / operator new[]
#include <stdexcept>  // std::logic_error / std::invalid_argument
#include <type_traits>
#include <utility>

namespace sluice::async::detail {

// ---------------------------------------------------------------------------
// Fail-fast entry for Queue lease/control invariant violations. (P1)
// ---------------------------------------------------------------------------
[[noreturn]] void queue_lease_fail_fast() noexcept {
    std::terminate();
}

// ---------------------------------------------------------------------------
// QueueItemLease out-of-line bodies. (P1)
// ---------------------------------------------------------------------------
void QueueItemLease::require_empty_or_terminate() const noexcept {
    if (control_ != nullptr) {
        queue_lease_fail_fast();
    }
}

void QueueItemLease::adopt_control(QueueItemControl& control) noexcept {
    require_empty_or_terminate();
    control_ = &control;
}

QueueItemLease::~QueueItemLease() noexcept {
    if (control_ != nullptr) {
        queue_lease_fail_fast();
    }
}

// (queue_type_token<T> is header-only / inline in queue_item.hpp; nothing to
// define here.)

// ===========================================================================
// P2+P3 — QueuePort structural skeleton + fast paths.
// ===========================================================================

// Ordinary CallGuard: brackets the time inside the non-template QueuePort
// authority. Enters AFTER the lifecycle check (so a tearing_down port never
// increments the counter) and decrements on EVERY return path (normal return,
// failed result, exception unwind). It does NOT cover begin_teardown /
// take_next / typed conversion / typed destruction (§7).
//
// The guard is fail-fast on a non-operational lifecycle: ordinary entry into
// a tearing_down port is an invariant violation (the lifecycle transition
// serializes against this counter — once tearing_down, active_port_calls_ is
// frozen at 0 and any ordinary entry is rejected before construction).
struct QueuePort::CallGuard final {
    explicit CallGuard(QueuePort& port) noexcept : port_(&port) {
        ++port_->active_port_calls_;
    }
    ~CallGuard() noexcept {
        if (port_ != nullptr) {
            --port_->active_port_calls_;
        }
    }
    CallGuard(const CallGuard&) = delete;
    CallGuard& operator=(const CallGuard&) = delete;
    CallGuard(CallGuard&&) = delete;
    CallGuard& operator=(CallGuard&&) = delete;

   private:
    QueuePort* port_;
};

QueuePort::QueuePort(Scheduler& sched, std::size_t capacity)
    : scheduler_(sched), capacity_(capacity) {
    // Caller contract: capacity >= 1 (runtime fixed; rendezvous/capacity-0 is
    // a deferred Queue-v2 protocol). A zero-capacity QueuePort is rejected
    // here rather than producing a degenerate ring.
    if (capacity_ == 0) {
        // Construction-time precondition violation: throw (caller can recover
        // by fixing the argument). This is NOT a winner-path fail-fast.
        throw std::invalid_argument(
            "sluice::async::AsyncQueue capacity must be >= 1");
    }
    // Allocate the fixed ring of empty leases. `std::make_unique<...[]>(n)`
    // value-initializes each QueueItemLease to its empty default state. The
    // default ctor is private but QueuePort is a friend of QueueItemLease.
    ring_ = std::unique_ptr<QueueItemLease[]>(
        new QueueItemLease[capacity_]());  // () = value-init each slot empty
}

QueuePort::~QueuePort() {
    // Destruction contract (§6): the ring must be empty and the teardown
    // session complete. This is a caller contract; we do not auto-drain.
    // Debug-grade check: if the ring is non-empty at destruction, the caller
    // violated the contract. Fail-fast to surface the bug rather than leak.
    //
    // NOTE: in a well-formed program begin_teardown() drains the ring to empty
    // before ~QueuePort runs. A non-empty ring here means the caller destroyed
    // the QueuePort without completing teardown — an invariant violation.
    if (ring_count_ != 0) {
        queue_lease_fail_fast();
    }
}

// --- snapshot projections (CallGuard-covered; lock-free observation) -------
//
// These observe close state and counts. is_closed / capacity are stable or
// monotonic and need no lock for the projection itself; size reads
// ring_count_ which is mutated under state_mtx_, so a cheap lock makes the
// observation consistent. (Snapshot is in the CallGuard list per §7.)
bool QueuePort::is_closed() const noexcept {
    return closed_;
}

std::size_t QueuePort::capacity() const noexcept {
    return capacity_;
}

std::size_t QueuePort::size() const noexcept {
    // state_mtx_ is mutable; const-method lock.
    const_cast<Mutex&>(state_mtx_).lock();
    const std::size_t n = ring_count_;
    const_cast<Mutex&>(state_mtx_).unlock();
    return n;
}

// --- fast paths (P3) -------------------------------------------------------

// try_push (P2 FastPushCommit / P4 TryPushWouldBlock, no Scheduler):
//   - lifecycle must be operational (CallGuard rejects tearing_down)
//   - lease must be non-empty, owner_port == this, location == detached
//   - if closed: P3 PushClosed — return the exact lease as `closed`
//   - if ring has space AND no older eligible producer: P2 FastPushCommit —
//       detached -> producer_operation -> ring; return committed
//   - else (full OR an older producer is linked): P4 TryPushWouldBlock —
//       return the exact lease as `would_block`
//
// In P2+P3 the no-older-eligible-producer check is trivially true: there are
// no linked producers until P5 lands. So the only fast-path branches are
// closed / space / would_block(full). The producer-WaitQueue FIFO check is
// added in P5 (it serializes under global_mtx_ which the fast path does not
// take; for P3 the fast path correctly returns would_block when full).
QueueOpaquePushResult QueuePort::try_push(QueueItemLease lease) {
    // Lifecycle gate BEFORE CallGuard: a tearing_down port rejects ordinary
    // entry without incrementing the counter.
    {
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
    }
    CallGuard guard(*this);

    // Entry contract: non-empty lease of this port at detached. Validate via
    // the lease's control pointer WITHOUT releasing it yet — the lease keeps
    // custody so the failure paths can return the exact original lease and
    // the commit path can move the lease whole into the ring slot.
    QueueItemControl* c = lease.control_;  // friend access
    if (c == nullptr || c->owner_port_ != this ||
        c->location_ != QueueItemControl::Location::detached) {
        queue_lease_fail_fast();
    }
    // detached -> producer_operation (admission; the lease is now "in" the
    // producer operation).
    c->location_ = QueueItemControl::Location::producer_operation;

    LockGuard lk(state_mtx_);
    // P3 PushClosed: closed rejects the producer; return the EXACT original
    // lease (producer_operation -> detached; no copy / alias / default).
    if (closed_) {
        c->location_ = QueueItemControl::Location::detached;
        return QueueOpaquePushResult::failed(
            QueueOpaquePushStatus::closed, std::move(lease));
    }
    // P4 TryPushWouldBlock (full): return the exact lease as would_block.
    if (ring_full_locked()) {
        c->location_ = QueueItemControl::Location::detached;
        return QueueOpaquePushResult::failed(
            QueueOpaquePushStatus::would_block, std::move(lease));
    }
    // P2 FastPushCommit: producer_operation -> ring. Move the WHOLE lease
    // (control custody included) into the empty tail slot. The destination
    // slot is empty (ring invariant); move ctor empties the source `lease`.
    const std::size_t tail =
        ring_slot(ring_count_);  // logical tail = (head + count) % cap
    c->location_ = QueueItemControl::Location::ring;
    ring_[tail] = std::move(lease);  // source `lease` now empty
    ++ring_count_;
    return QueueOpaquePushResult::committed();
}

// try_pop (P3 FastPopCommit / C2 PopClosedEmpty / C3 TryPopWouldBlock):
//   - lifecycle operational
//   - if ring non-empty (and no older eligible consumer): C1 FastPopCommit —
//       ring -> consumer_operation; return item with the lease
//   - if closed AND ring empty: C2 PopClosedEmpty — return closed
//   - else (empty, open; or older consumer linked): C3 TryPopWouldBlock —
//       return would_block
//
// As with try_push, the older-consumer check is trivially true until P5.
QueueOpaquePopResult QueuePort::try_pop() {
    {
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
    }
    CallGuard guard(*this);

    LockGuard lk(state_mtx_);
    // C1 FastPopCommit: move the head slot's lease out; ring slot becomes
    // empty; control location ring -> consumer_operation.
    if (!ring_empty_locked()) {
        const std::size_t head = ring_head_;
        // The slot owns the control; move the whole lease out. Source slot
        // becomes empty (move ctor empties source).
        QueueItemLease out = std::move(ring_[head]);
        ring_head_ = (ring_head_ + 1) % capacity_;
        --ring_count_;
        // Mark the moved-out control at consumer_operation (friend access).
        out.control_->location_ =
            QueueItemControl::Location::consumer_operation;
        return QueueOpaquePopResult::item(std::move(out));
    }
    // C2 PopClosedEmpty: closed + empty consumer terminal.
    if (closed_) {
        return QueueOpaquePopResult::closed();
    }
    // C3 TryPopWouldBlock: empty + open.
    return QueueOpaquePopResult::would_block();
}

// close (CL1 / CL2): monotonic Open -> Closed. Idempotent on Closed.
// P5 adds closed-reconciliation (drain to eligible consumers, complete blocked
// producers/consumers); P3 closes the linearization point only.
void QueuePort::close() noexcept {
    // Lifecycle gate + CallGuard.
    {
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
    }
    CallGuard guard(*this);

    LockGuard lk(state_mtx_);
    // CL1 Open -> Closed; CL2 Closed -> Closed (idempotent). Monotone.
    closed_ = true;
    // P5 reconciliation runs here under G+S once linked waiters exist.
}

// --- blocking / timed (P4-P6 deferred) ------------------------------------
//
// These require Scheduler wait admission, PREPARED timers, reconciliation,
// and publication. Until P4-P6 land, they throw std::logic_error so the
// compile type graph is complete and the fast paths are independently
// testable. (This is NOT a winner-path exception; blocking is a caller-facing
// API that may legitimately surface "not implemented" while the wait
// substrate is wired.)
QueueOpaquePushResult QueuePort::push(QueueItemLease /*lease*/) {
    throw std::logic_error(
        "AsyncQueue blocking push: wait admission not yet implemented (P4-P6)");
}
QueueOpaquePushResult QueuePort::push_until(QueueItemLease /*lease*/,
                                            queue_deadline_t /*deadline*/) {
    throw std::logic_error(
        "AsyncQueue push_until: timed wait admission not yet implemented (P4-P6)");
}
QueueOpaquePopResult QueuePort::pop() {
    throw std::logic_error(
        "AsyncQueue blocking pop: wait admission not yet implemented (P4-P6)");
}
QueueOpaquePopResult QueuePort::pop_until(queue_deadline_t /*deadline*/) {
    throw std::logic_error(
        "AsyncQueue pop_until: timed wait admission not yet implemented (P4-P6)");
}

// --- teardown (P7: structural skeleton; full drain wired in P7) -----------
//
// P7 implements the irreversible operational -> tearing_down transition under
// G+S with the full precondition check (active_port_calls_==0, etc.). P2
// stubs begin_teardown so the type graph is complete; P7 replaces the body.
QueueTeardownSession QueuePort::begin_teardown() noexcept {
    // P7 will perform the precondition check + lifecycle transition under
    // G+S. For P2, return a session bound to this port (the session's
    // take_next / empty / dtor are wired in P7).
    return QueueTeardownSession{*this};
}

// --- QueueTeardownSession --------------------------------------------------

QueueItemLease QueueTeardownSession::take_next() noexcept {
    if (port_ == nullptr) {
        queue_lease_fail_fast();
    }
    LockGuard lk(port_->state_mtx_);
    if (port_->lifecycle_ != QueueLifecycle::tearing_down) {
        queue_lease_fail_fast();
    }
    // P7: drain ring slots one-by-one (ring -> teardown). P2 skeleton: once
    // tearing_down is set (P7), move the head slot out. Until P7 wires the
    // lifecycle transition, the ring is empty at teardown (no producer path),
    // so take_next returns an empty lease here.
    if (port_->ring_empty_locked()) {
        return QueueItemLease{};
    }
    const std::size_t head = port_->ring_head_;
    QueueItemLease out = std::move(port_->ring_[head]);
    port_->ring_head_ = (port_->ring_head_ + 1) % port_->capacity_;
    --port_->ring_count_;
    out.control_->location_ = QueueItemControl::Location::teardown;
    return out;
}

bool QueueTeardownSession::empty() const noexcept {
    if (port_ == nullptr) {
        return true;
    }
    LockGuard lk(port_->state_mtx_);
    return port_->ring_empty_locked();
}

QueueTeardownSession::~QueueTeardownSession() noexcept {
    // A moved-from session (port_ == nullptr) is a valid empty shell.
    // A live session at destruction must have drained the ring to empty
    // (§6). Fail-fast otherwise.
    if (port_ == nullptr) {
        return;
    }
    LockGuard lk(port_->state_mtx_);
    if (!port_->ring_empty_locked()) {
        queue_lease_fail_fast();
    }
    // P7 marks the unique teardown authority complete here.
}

}  // namespace sluice::async::detail
