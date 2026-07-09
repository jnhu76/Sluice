// sluice::async::WaitQueue — one cancellation-safe wait queue (sluice-CORE-E10).
//
// Implements the minimal queue required for one-node-per-wait registration
// (§5 WaitQueue protocol). See docs/e10-waitnode-wait-queue.md.
//
// Scope (§1): IN — enqueue/register, wake one node, cancel a specific node,
// safe removal/unlink, empty/destruction invariants. OUT — multi-wait, select,
// wake-all, timers, priorities, configurable queue discipline.
//
// Shape, DERIVED from repository constraints (§5):
//   - INTRUSIVE doubly-linked list. Reason: a WaitNode is caller-owned and
//     address-stable while outstanding (mirrors Completion<T> L7); the queue
//     holds pointers to it, never copies. Node identity = its address.
//   - FIFO. Reason: matches the existing queue discipline (local_runnable,
//     pending_spawn_ are FIFO deques). §5 forbids configurable policies and
//     extra fairness beyond the current queue discipline.
//
// THE UNLINK LAW (§7). Queue removal is NOT an independent competing protocol.
// The terminal transition AND the unlink authority are centralized: every
// resolver (wake_one, cancel) takes the queue mtx_, performs the canonical
// resolve_(outcome) CAS on the node, and IF it is the winner (CAS succeeded)
// unlinks the node from the list in the SAME critical section. A losing
// resolver's CAS fails; it performs no unlink and no wake.
//
//   wake_one_locked():
//       head = list head
//       if head && head.resolve_(Woken):   // winner CAS
//           unlink(head)                    // SAME critical section
//           return head                     // exactly one winner
//       return null
//
//   cancel_locked(node):
//       if node.resolve_(Cancelled):        // winner CAS
//           unlink(node)                    // SAME critical section
//           return true
//       return false
//
// Intrusive-link invariants (§5, proven):
//   - linked-at-most-once: register_() only succeeds from the Detached state,
//     and sets home_ once. A node already registered is rejected (C8).
//   - double-unlink impossible: unlink runs only on a winning CAS, exactly
//     once, under the queue mtx_. A loser's CAS fails -> no unlink.
//   - terminal node not indefinitely reachable: the winner unlinks it
//     immediately, in the same critical section as the resolve CAS.
//   - destroyed node never linked: WaitNode's dtor asserts !is_registered().
//
// SYNCHRONIZATION (§9). Two domains:
//   - STRUCTURAL (next_/prev_/home_, list head/tail): WaitQueue::mtx_.
//   - WINNER (terminal outcome): WaitNode::state_ atomic CAS (see wait_node.hpp).
// Every mutating method (_locked variants) requires the caller to hold mtx_.
// The Scheduler resolves a Scheduler-integrated wait through its OWN seams
// (Scheduler::wake_wait_one / cancel_wait), which take mtx_ under global_mtx_
// and perform the canonical route_runnable_locked. Those Scheduler seams call
// the private _locked resolver variants here.
//
// RESOLUTION AUTHORITY (E10-CORRECTIVE C2). The terminal resolution seams of a
// WaitQueue — wake_one_locked / cancel_locked — are PRIVATE. A Scheduler-
// integrated wait (registered via Scheduler::await_wait, which owns
// waiting_waitq_count_ + the runnable-route obligation) may be terminally
// resolved ONLY through Scheduler::wake_wait_one / Scheduler::cancel_wait.
// Calling WaitQueue resolution directly would resolve the node + unlink it
// WITHOUT decrementing waiting_waitq_count_ and WITHOUT routing the resumed
// fiber through the canonical Scheduler wake seam — stranding the fiber and
// leaving MW classification stale. The public surface therefore exposes
// register_wait (registration is not a resolution) but NOT wake_one/cancel.
// The Scheduler is a friend; a pure-protocol test hook (WaitQueueTestHooks,
// defined only in the protocol test TU) reaches the resolvers for C1-C9.
//
// cancel_all REMOVED (E10-CORRECTIVE C3): it had ZERO production callsites, no
// authoritative E10 shutdown semantic required it, and its header text
// ("the Scheduler cancels-all on run termination") was an authority/document
// drift — the Scheduler does NOT auto-resolve waits on run termination (a
// stranded MW-S3 wait is left for the caller, like E9's waiting_ready_).
//
// Layering: BELOW the Scheduler. WaitQueue knows nothing about fibers,
// scheduling, or runnable enqueue. It returns the winning node to the caller,
// which performs the scheduler wake (route_runnable_locked). This keeps the
// "who makes the fiber runnable" decision in scheduler code (§8).
#pragma once

#include <sluice/async/wait_node.hpp>

#include <cassert>
#include <mutex>

namespace sluice::async {

class Scheduler;  // forward: friend — the sole resolution authority (C2)

// Pure-protocol test hook (C1-C9). Forward-declared so WaitQueue can friend it;
// defined ONLY in the e10_wait_queue_test TU, exactly like SchedulerTestHooks /
// E9ParkSeamHooks. It exposes no production contract.
struct WaitQueueTestHooks;

class WaitQueue {
public:
    WaitQueue() noexcept = default;

    // §10 destruction invariant: a queue MUST be empty when destroyed, OR its
    // owner must have explicitly resolved (cancelled) all registered waiters.
    // Destroying a queue with linked nodes would orphan them (their home_
    // points to freed memory; §3 Q8). Debug asserts empty; release is a no-op
    // (the nodes are caller-owned and remain valid, but their home_ becomes
    // dangling — the caller contract is to drain first). The Scheduler does NOT
    // auto-resolve waits on run termination (E10-CORRECTIVE C3); an unresolved
    // registered wait is left for the caller, exactly as E9 treats a stranded
    // waiting_ready_ flag (MW-S3 returns STALLED in Drain).
    ~WaitQueue() {
        // head_ == null iff empty (tail_ maintained in lockstep). A non-empty
        // queue at destruction is a caller contract violation (§10).
        assert(head_ == nullptr &&
               "WaitQueue destroyed with registered waiters (resolve them first)");
    }

    WaitQueue(const WaitQueue&) = delete;
    WaitQueue& operator=(const WaitQueue&) = delete;
    WaitQueue(WaitQueue&&) = delete;
    WaitQueue& operator=(WaitQueue&&) = delete;

    // ---- Structural queries (caller holds mtx_) ----

    bool empty_locked() const noexcept { return head_ == nullptr; }

    // ---- Registration (single-wait) ----

    // Register `node` (Detached -> Registered) and link it at the FIFO tail.
    // `fiber` is recorded on the node as the scheduler-facing handle (opaque to
    // WaitNode; the Scheduler uses it to route the resumed fiber). Returns false
    // if `node` is already registered or terminal (C8: a completed node cannot
    // be reused until reset/detached — and E10 nodes are NOT resettable; a new
    // wait needs a new node). Takes mtx_ internally.
    //
    // Registration is PUBLIC because it is NOT a terminal resolution: it does
    // not make any fiber runnable and does not bypass Scheduler authority. A
    // Scheduler-integrated registration goes through Scheduler::await_wait (so
    // waiting_waitq_count_ is accounted), but register_wait remains available
    // for direct use because a non-Scheduler caller may legitimately enqueue a
    // node it intends to resolve through the Scheduler seams.
    bool register_wait(WaitNode& node, Fiber* fiber = nullptr) {
        std::lock_guard<std::mutex> lk(mtx_);
        return register_wait_locked(node, fiber);
    }

    // Expose the lock so the Scheduler can run register/resolve atomically
    // with its own coordination state (e.g. register + make_waiting under one
    // lock, or resolve + route under one lock). The Scheduler is the consumer
    // of this seam; test code reaches the resolvers via WaitQueueTestHooks.
    std::mutex& mtx() noexcept { return mtx_; }

private:
    friend class Scheduler;             // the sole resolution authority (C2)
    friend struct WaitQueueTestHooks;   // pure-protocol test hook (C1-C9)

    // _locked variant: caller holds mtx_.
    bool register_wait_locked(WaitNode& node, Fiber* fiber = nullptr) {
        if (!node.register_(this, fiber)) return false;  // already registered/terminal (C8)
        // FIFO tail-link.
        node.next_ = nullptr;
        node.prev_ = tail_;
        if (tail_ != nullptr) {
            tail_->next_ = &node;
        } else {
            head_ = &node;
        }
        tail_ = &node;
        return true;
    }

    // ---- Wake one (canonical terminal resolver, Woken outcome) ----
    //
    // PRIVATE (C2): the Scheduler resolves a Scheduler-integrated wait via
    // Scheduler::wake_wait_one, which calls this under global_mtx_ + mtx_ and
    // routes the winner's fiber through the canonical wake seam. Resolving a
    // Scheduler-integrated wait directly here would bypass the count/runnable
    // integration and strand the fiber.

    // Resolve the FIFO head with Woken, unlink the winner, and return it.
    // Returns nullptr if the queue is empty (no wait to wake) or if the head's
    // resolve CAS failed (the head was concurrently cancelled — a loser here).
    WaitNode* wake_one_locked() {
        if (head_ == nullptr) return nullptr;
        WaitNode* n = head_;
        if (n->resolve_(WaitOutcome::woken)) {  // winner CAS (§2/§7)
            unlink_locked(*n);                  // SAME critical section (§7)
            return n;                           // exactly one winner
        }
        // The head was resolved concurrently (e.g. cancelled between the
        // caller's empty check and now). It is now terminal; the winner
        // (canceller) is responsible for unlinking it. Return null: no wake
        // was delivered by THIS call.
        return nullptr;
    }

    // ---- Cancel a specific node (canonical terminal resolver, Cancelled) ----
    //
    // PRIVATE (C2): the Scheduler resolves via Scheduler::cancel_wait, which
    // calls this and routes the winner. See wake_one_locked.

    // Resolve `node` with Cancelled and unlink the winner. Returns true iff
    // this call is the winner (node was Registered and is now Cancelled). A
    // losing call (node already terminal — C3/C4/C5) returns false and does
    // nothing. `node` MUST belong to this queue (caller contract).
    bool cancel_locked(WaitNode& node) {
        // `node` may not belong to this queue (caller contract violation). The
        // resolve CAS still cannot wrongly succeed: a Registered node has a
        // single home_ (set under its own state CAS at register time). We do
        // NOT assert home_==this here to avoid racing a concurrent unregister
        // in release builds; the CAS is the authority.
        if (node.resolve_(WaitOutcome::cancelled)) {  // winner CAS (§2/§7)
            unlink_locked(node);                      // SAME critical section (§7)
            return true;
        }
        return false;  // already terminal (loser): C2/C3/C4/C5 no-op
    }

    // ---- Unlink (the single structural-removal seam, §7) ----

    // Unlink a node from the list. Called ONLY by a winning resolver
    // (wake_one_locked / cancel_locked), under mtx_, in the SAME critical
    // section as its winning resolve CAS. This is the ONE unlink path (§7: no
    // competing wake-unlink / cancel-unlink / destructor-unlink). Clears home_.
    void unlink_locked(WaitNode& node) {
        // Splice node out of the doubly-linked list.
        if (node.prev_ != nullptr) {
            node.prev_->next_ = node.next_;
        } else {
            head_ = node.next_;
        }
        if (node.next_ != nullptr) {
            node.next_->prev_ = node.prev_;
        } else {
            tail_ = node.prev_;
        }
        node.next_ = nullptr;
        node.prev_ = nullptr;
        node.home_ = nullptr;
    }

    std::mutex mtx_;
    WaitNode* head_{nullptr};  // FIFO head; null iff empty
    WaitNode* tail_{nullptr};  // FIFO tail; maintained in lockstep with head_
};

}  // namespace sluice::async
