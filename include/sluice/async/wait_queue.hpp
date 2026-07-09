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
// SEALED AUTHORITY (E10-CORRECTIVE-2 R1+R2). ALL structural operations of a
// WaitQueue are PRIVATE, with Scheduler the ONLY friend:
//
//   - register_wait_locked : the registration integration authority's
//                            structural half (Scheduler::await_wait wraps it
//                            with waiting_waitq_count_ accounting + the
//                            waiting/runnable transition).
//   - wake_one_locked      : terminal-winner resolution (Woken).
//   - cancel_locked        : terminal-winner resolution (Cancelled).
//   - unlink_locked        : the one structural-removal seam.
//   - mtx()                : the structural lock the Scheduler takes UNDER
//                            global_mtx_ to make register/resolve atomic with
//                            its own coordination state.
//
// A Scheduler-integrated wait (registered via Scheduler::await_wait, which owns
// waiting_waitq_count_ + the runnable-route obligation) may be terminally
// resolved ONLY through Scheduler::wake_wait_one / Scheduler::cancel_wait, and
// registered ONLY through Scheduler::await_wait. Calling WaitQueue registration
// or resolution directly would bypass Scheduler accounting: an arbitrary
// Fiber* could be injected into a queue that Scheduler resolution later trusts
// (R2 P1/P2/P5), or a node could be resolved + unlinked WITHOUT decrementing
// waiting_waitq_count_ and WITHOUT routing the resumed fiber (stranding the
// fiber and leaving MW classification stale).
//
// There is NO test friend and NO publicly nameable access type. A downstream TU
// cannot reach mtx_, register_wait_locked, wake_one_locked, cancel_locked, or
// unlink_locked. This closes the unconditional WaitQueueTestHooks friend grant
// that previously let an arbitrary TU define the granted friend type and call
// the private seams (R1). Tests observe the protocol through the public
// Scheduler integration seams (Scheduler::await_wait / wake_wait_one /
// cancel_wait) and the lock-free WaitNode state queries; production authority
// is never weakened to make tests convenient.
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

class Scheduler;  // forward: friend — the sole registration+resolution authority

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

private:
    friend class Scheduler;  // the sole registration + resolution authority

    // Expose the structural lock so the Scheduler can run register/resolve
    // atomically with its own coordination state (register + make_waiting under
    // one critical section; resolve + route under one). Scheduler is the ONLY
    // consumer of this seam. Private + friended: an external TU cannot acquire
    // the queue lock and therefore cannot reach the _locked resolvers.
    std::mutex& mtx() noexcept { return mtx_; }

    // Structural query (caller holds mtx_). Used by the Scheduler; private so
    // external code cannot inspect queue membership.
    bool empty_locked() const noexcept { return head_ == nullptr; }

    // ---- Registration (single-wait) ----

    // Register `node` (Detached -> Registered) and link it at the FIFO tail.
    // `fiber` is recorded on the node as the scheduler-facing handle (opaque to
    // WaitNode; the Scheduler uses it to route the resumed fiber). Returns false
    // if `node` is already registered or terminal (C8). _locked variant: caller
    // holds mtx_.
    //
    // PRIVATE (E10-CORRECTIVE-2 R2): a Scheduler-integrated registration goes
    // through Scheduler::await_wait, which calls this under global_mtx_ + mtx_,
    // records waiting_waitq_count_, captures ws->current as the Fiber handle,
    // and performs the waiting transition. An external TU cannot express
    // register_wait(node, arbitrary_fiber): that would inject an arbitrary
    // Fiber* into a queue the Scheduler later trusts at resolution time, outside
    // Scheduler accounting. Scheduler is the only friend; there is no public
    // wrapper and no test hook.
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
