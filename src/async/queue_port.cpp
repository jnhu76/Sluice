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
#include <sluice/async/scheduler.hpp>   // Scheduler (full def for seam calls)

#include "queue_detail.hpp"  // QueueWaitCtx (shared with scheduler.cpp)

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
    // F.4 corrective: an ordinary entry increments active_port_calls_ under
    // G+S in the caller (so the increment is atomic with the lifecycle gate
    // and observed by begin_teardown), then constructs the guard with the
    // adopt_tag (no second increment). The guard's dtor always decrements.
    struct adopt_tag {};
    // Increment-then-manage form (untimed fast paths that do NOT take G before
    // the lifecycle check — kept for the snapshot projections where the
    // lifecycle check is structurally inside the same scope).
    explicit CallGuard(QueuePort& port) noexcept : port_(&port) {
        ++port_->active_port_calls_;
    }
    // Adopt form: the caller has ALREADY incremented under G+S; the guard
    // owns the decrement at scope exit.
    CallGuard(QueuePort& port, adopt_tag) noexcept : port_(&port) {}
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
    // F.5 corrective: lock-free acquire load. close() does the matching release
    // store under G+S. Callers may invoke this from any OS thread.
    return closed_.load(std::memory_order::acquire);
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
    // F.4 corrective: the lifecycle gate + active_port_calls_ increment MUST
    // be atomic with respect to begin_teardown (which takes G+S and checks
    // active_port_calls_). Hold G+S across the lifecycle check AND the
    // increment; then construct the CallGuard with adopt_tag (no second
    // increment) so its dtor owns the matching decrement. The body re-
    // acquires G+S at the commit step.
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;  // observed under G+S by begin_teardown
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});

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

    // G -> S (P5 lock order). Reconciliation of the OTHER role happens under
    // these same locks so a fast-path commit + wake is atomic with respect to
    // blocking admission.
    LockGuard glk(scheduler_.global_mtx_);
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
    // P5 reconciliation: a new item arrived. If a consumer is parked, grant it
    // the OLDEST ring item (ring_[head] — FIFO) atomically, winner-before-
    // publication (resolve Woken + ring move + retire + publish in one critical
    // section inside queue_grant_consumer_locked).
    (void)scheduler_.queue_grant_consumer_locked(*this);
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
    // F.4 corrective: lifecycle gate + increment atomic w.r.t. begin_teardown.
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});

    // G -> S (P5 lock order).
    LockGuard glk(scheduler_.global_mtx_);
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
        // P5 reconciliation: a slot opened. If a producer is parked, grant it
        // the freed slot atomically (queue_grant_producer_locked moves the
        // winner's lease into the slot, winner-before-publication).
        (void)scheduler_.queue_grant_producer_locked(*this);
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
// P5 closed-reconciliation: wake every blocked producer (P7 closed outcome —
// each retains its lease) and every blocked consumer (consumers pop buffered
// items on resume while the ring still has them; once empty, remaining
// consumers get the closed outcome). The wake is signaling only; each woken
// Fiber's admission loop re-checks under G + S + role and finalizes its own
// outcome (commit-to-ring for a producer if a slot somehow opened; pop for a
// consumer if an item remains; closed otherwise).
void QueuePort::close() noexcept {
    // F.4 corrective: lifecycle gate + increment atomic w.r.t. begin_teardown.
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});

    LockGuard glk(scheduler_.global_mtx_);
    LockGuard lk(state_mtx_);
    // CL1 Open -> Closed; CL2 Closed -> Closed (idempotent). Monotone.
    // F.5 corrective: release store pairs with the acquire load in is_closed().
    closed_.store(true, std::memory_order::release);
    // Closed-reconciliation (P5/P7): drain the consumer FIFO by granting each
    // parked consumer the next buffered item until the ring is empty; further
    // consumers are granted closed+empty (queue_grant_consumer_locked leaves
    // their out empty when the ring is empty). Then drain the producer FIFO:
    // each parked producer is granted "closed" (queue_grant_producer_locked
    // sees closed_ and leaves its lease retained; the producer resume returns
    // it as closed). Both grant seams commit winner-before-publication.
    while (scheduler_.queue_grant_consumer_locked(*this) != nullptr) {
        // keep draining consumers (each gets one buffered item, or closed once
        // the ring is empty)
    }
    while (scheduler_.queue_grant_producer_locked(*this) != nullptr) {
        // keep draining producers (each returns its lease as closed)
    }
}

// --- blocking / timed (P4-P6) ---------------------------------------------
//
// The blocking/timed substrate. Each sets up the per-op context (control
// location detached -> producer_operation for push; an empty out-lease for
// pop), allocates a stack WaitNode, delegates to the Scheduler admit closure
// (which suspends until the reconciler commits + publishes), and reads the
// post-resume state to build the opaque result:
//   push: lease empty => committed; lease retained => closed/expired.
//   pop:  out non-empty => item; out empty => closed/expired.
//
// These run inside a Fiber (the admit closures assert g_worker != null). They
// are NOT safe to call from a non-Fiber thread.
QueueOpaquePushResult QueuePort::push(QueueItemLease lease) {
    // F.4 corrective: lifecycle gate + increment atomic w.r.t. begin_teardown.
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});
    QueueItemControl* c = lease.control_;
    if (c == nullptr || c->owner_port_ != this ||
        c->location_ != QueueItemControl::Location::detached) {
        queue_lease_fail_fast();
    }
    c->location_ = QueueItemControl::Location::producer_operation;
    WaitNode node;
    scheduler_.queue_push_admit(*this, node, lease);
    // On return: lease empty => committed (ring owns it); non-empty => closed
    // (untimed push never expires). Distinguish by reading the lease.
    if (lease.control_ == nullptr) {
        return QueueOpaquePushResult::committed();
    }
    // closed: the operation retained the lease; control is still at
    // producer_operation. Move the lease whole into the failed result.
    return QueueOpaquePushResult::failed(
        QueueOpaquePushStatus::closed, std::move(lease));
}

QueueOpaquePushResult QueuePort::push_until(QueueItemLease lease,
                                            queue_deadline_t deadline) {
    // F.4 corrective: lifecycle gate + increment atomic w.r.t. begin_teardown.
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});
    QueueItemControl* c = lease.control_;
    if (c == nullptr || c->owner_port_ != this ||
        c->location_ != QueueItemControl::Location::detached) {
        queue_lease_fail_fast();
    }
    c->location_ = QueueItemControl::Location::producer_operation;
    WaitNode node;
    scheduler_.queue_push_admit_until(*this, node, lease, deadline);
    if (lease.control_ == nullptr) {
        return QueueOpaquePushResult::committed();
    }
    const bool expired = node.was_expired();
    return QueueOpaquePushResult::failed(
        expired ? QueueOpaquePushStatus::expired
                : QueueOpaquePushStatus::closed,
        std::move(lease));
}

QueueOpaquePopResult QueuePort::pop() {
    // F.4 corrective: lifecycle gate + increment atomic w.r.t. begin_teardown.
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});
    QueueItemLease out;  // empty; the reconciler moves a ring item into it
    WaitNode node;
    scheduler_.queue_pop_admit(*this, node, out);
    if (out.control_ != nullptr) {
        return QueueOpaquePopResult::item(std::move(out));
    }
    return QueueOpaquePopResult::closed();
}

QueueOpaquePopResult QueuePort::pop_until(queue_deadline_t deadline) {
    // F.4 corrective: lifecycle gate + increment atomic w.r.t. begin_teardown.
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});
    QueueItemLease out;
    WaitNode node;
    scheduler_.queue_pop_admit_until(*this, node, out, deadline);
    if (out.control_ != nullptr) {
        return QueueOpaquePopResult::item(std::move(out));
    }
    const bool expired = node.was_expired();
    return expired ? QueueOpaquePopResult::expired()
                   : QueueOpaquePopResult::closed();
}

// --- teardown (P7) ---------------------------------------------------------
//
// begin_teardown performs the irreversible operational -> tearing_down
// transition. It does NOT enter the ordinary CallGuard (§7) — teardown is the
// exclusive authority and the four counters below replace the CallGuard's
// "no ordinary op in flight" guarantee. Under G + S it requires ALL of:
//
//   lifecycle_         == operational       (no earlier teardown)
//   active_port_calls_ == 0                 (no ordinary op inside QueuePort)
//   active_wait_associations_ == 0          (no linked Queue wait epoch)
//   active_queue_timers_ == 0               (no ACTIVE Queue timer)
//   granted_not_resumed_ == 0               (no published suspended winner)
//   producer WaitQueue empty                (no parked producer)
//   consumer WaitQueue empty                (no parked consumer)
//
// The lifecycle transition serializes against ordinary call entry: an earlier
// ordinary call makes active_port_calls_ != 0; an earlier teardown makes
// every later ordinary entry fail-fast before CallGuard construction. Once
// tearing_down, the port admits no push/pop/try/timed/close/snapshot/second
// teardown/waiter/timer/ticket.
QueueTeardownSession QueuePort::begin_teardown() noexcept {
    LockGuard glk(scheduler_.global_mtx_);  // G
    LockGuard lk(state_mtx_);               // S (under G)

    if (lifecycle_ != QueueLifecycle::operational ||
        active_port_calls_ != 0 || active_wait_associations_ != 0 ||
        active_queue_timers_ != 0 || granted_not_resumed_ != 0 ||
        !scheduler_.queue_role_waiters_empty_locked(*this)) {
        queue_lease_fail_fast();
    }

    // Irreversible operational -> tearing_down. After this point every ordinary
    // entry fails-fast on the lifecycle check before constructing a CallGuard,
    // and a second begin_teardown fails-fast here (lifecycle_ != operational).
    lifecycle_ = QueueLifecycle::tearing_down;
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
    // Drain ring slots one-by-one (ring -> teardown). The session was minted
    // by begin_teardown under G+S with all four counters zero and both role
    // FIFOs empty, so no concurrent producer/consumer can refill the ring
    // (ordinary entry rejects tearing_down before CallGuard). Move the head
    // slot's lease whole into the result; the source slot becomes empty.
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
