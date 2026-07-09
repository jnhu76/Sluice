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
// The public wake_one()/cancel()/register_wait() take the lock internally and
// delegate to the _locked variants. The Scheduler calls the _locked variants
// under global_mtx_ (its existing coordination domain) — see the scheduler
// integration; the queue's own mtx_ is the structural authority.
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

class WaitQueue {
public:
    WaitQueue() noexcept = default;

    // §10 destruction invariant: a queue MUST be empty when destroyed, OR its
    // owner must have explicitly cancelled all registered waiters. Destroying a
    // queue with linked nodes would orphan them (their home_ points to freed
    // memory; §3 Q8). Debug asserts empty; release is a no-op (the nodes are
    // caller-owned and remain valid, but their home_ becomes dangling — the
    // caller contract is to drain first). The Scheduler cancels-all on run
    // termination; see the scheduler integration.
    ~WaitQueue() {
        // head_ == null iff empty (tail_ maintained in lockstep). A non-empty
        // queue at destruction is a caller contract violation (§10).
        assert(head_ == nullptr &&
               "WaitQueue destroyed with registered waiters (cancel-all first)");
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
    bool register_wait(WaitNode& node, Fiber* fiber = nullptr) {
        std::lock_guard<std::mutex> lk(mtx_);
        return register_wait_locked(node, fiber);
    }

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

    // Resolve the FIFO head with Woken, unlink the winner, and return it.
    // Returns nullptr if the queue is empty (no wait to wake) or if the head's
    // resolve CAS failed (the head was concurrently cancelled — a loser here).
    // The caller (the Scheduler) makes the returned node's fiber runnable via
    // the canonical wake seam. Takes mtx_ internally.
    WaitNode* wake_one() {
        std::lock_guard<std::mutex> lk(mtx_);
        return wake_one_locked();
    }

    // _locked variant: caller holds mtx_.
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

    // Resolve `node` with Cancelled and unlink the winner. Returns true iff
    // this call is the winner (node was Registered and is now Cancelled). A
    // losing call (node already terminal — C3/C4/C5) returns false and does
    // nothing. `node` MUST belong to this queue (caller contract). Takes mtx_
    // internally.
    bool cancel(WaitNode& node) {
        std::lock_guard<std::mutex> lk(mtx_);
        return cancel_locked(node);
    }

    // _locked variant: caller holds mtx_.
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

    // ---- Cancel all registered waiters (owner shutdown, §10) ----

    // Resolve every currently-registered node with Cancelled and unlink it.
    // Used by the Scheduler on run termination so no waiter is stranded. Takes
    // mtx_ internally. Returns the number of waiters cancelled-and-unlinked.
    std::size_t cancel_all() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::size_t n = 0;
        while (head_ != nullptr) {
            WaitNode* node = head_;
            // Each head is still Registered (only this loop mutates the list
            // under mtx_). If a concurrent external cancel raced (none should,
            // since cancel_all runs under mtx_ too), resolve_ just loses and
            // we skip the unlink for that one — but cancel_all holds mtx_, so
            // no other resolver can run concurrently. The CAS is defensive.
            if (node->resolve_(WaitOutcome::cancelled)) {
                unlink_locked(*node);
                ++n;
            } else {
                // Terminal-but-still-linked (should not happen under exclusive
                // mtx_): unlink defensively to drain the list.
                unlink_locked(*node);
            }
        }
        return n;
    }

    // ---- Unlink (the single structural-removal seam, §7) ----

    // Unlink a node from the list. Called ONLY by a winning resolver (wake_one
    // / cancel / cancel_all), under mtx_, in the SAME critical section as its
    // winning resolve CAS. This is the ONE unlink path (§7: no competing
    // wake-unlink / cancel-unlink / destructor-unlink). Clears home_.
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

    // Expose the lock so the Scheduler can run register/wake/cancel atomically
    // with its own coordination state (e.g. classify + wake under one lock).
    std::mutex& mtx() noexcept { return mtx_; }

private:
    std::mutex mtx_;
    WaitNode* head_{nullptr};  // FIFO head; null iff empty
    WaitNode* tail_{nullptr};  // FIFO tail; maintained in lockstep with head_
};

}  // namespace sluice::async
