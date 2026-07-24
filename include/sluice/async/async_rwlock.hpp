// sluice::async::AsyncRwLock — Fiber-suspending async Read-Write Lock (sluice-CORE-E12-F).
//
// A writer-fair, phase-batched Read-Write Lock built on the closed E10/E11/E12
// wait substrate. Provides shared read acquisition (multiple concurrent readers)
// and exclusive write acquisition (one writer), with FIFO prefix batching for
// reader throughput and writer-fair admission preventing writer starvation.
//
// Semantic model (docs/e12-rwlock.md):
//
//   active_readers_ > 0, writer_active_ == false  <=>  Read-locked (N readers)
//   active_readers_ == 0, writer_active_ == true  <=>  Write-locked (one writer)
//   active_readers_ == 0, writer_active_ == false <=>  Unlocked (free)
//
// Fairness policy: Phase-fair FIFO prefix batching.
//   - No reader may bypass ANY queued waiter (writer OR reader).
//   - Reader inline admission requires: writer_active_ == false AND waiters_ empty.
//   - Writer inline admission requires: active_readers_ == 0 AND writer_active_
//     == false AND waiters_ empty AND current Fiber exists.
//   - On release: grant from queue head. Head writer -> grant ONE writer.
//     Head reader -> grant maximal consecutive reader prefix (batch).
//
// Synchronization domain: ALL authoritative decisions occur under
// Scheduler::global_mtx_ -> AsyncRwLock waiters_.mtx() (the existing coordination
// domain; same lock order as E10/E11/E12-A/B/C/D/E).
//
// Identity model: writer ownership is bound to Fiber* identity (writer_owner_),
// matching AsyncMutex's owner_ model. Reader ownership is a global count (v1
// does NOT track per-fiber reader identity).
//
// SEALED PUBLIC AUTHORITY (mirrors E12-A/B/C/D/E). The AsyncRwLock's private
// WaitQueue is NOT publicly reachable. The ONLY resource-grant authorities are
// unlock_read/unlock_write (head-driven reconcile) and inline admission.
// Cancellation routes through Scheduler::rwlock_cancel on this AsyncRwLock's
// private WaitQueue WITHOUT exposing it.
//
// Scheduler binding: AsyncRwLock borrows Scheduler& for its lifetime.
//
// Destruction contract: destroying an AsyncRwLock while readers/writer are
// active or while wait epochs remain registered is a CALLER CONTRACT VIOLATION.
// The destructor debug-asserts active_readers_ == 0, writer_active_ == false,
// and does NOT cancel/wake/synthesize grants. ~WaitQueue asserts empty.
//
// Explicitly excluded from v1:
//   upgrade/downgrade, recursive/reentrant locking, optimistic reads,
//   priority inheritance, configurable fairness, Select integration, RAII guards.
#pragma once

#include <cassert>
#include <cstddef>

#include <sluice/async/fiber.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

namespace sluice::async {

// A Fiber-suspending async Read-Write Lock.
//
// Non-copyable AND non-movable: the WaitQueue is non-movable (intrusive list
// with pointer identity), and the AsyncRwLock borrows Scheduler& (identity
// matters for wait resolution routing).
class AsyncRwLock {
public:
    // Stable context for TimerRegistration on_resolve hook. Address-stable
    // for the lifetime of this AsyncRwLock. The pump identifies RwLock timers
    // by the hook function pointer and uses this ctx to call rwlock_expire_wait.
    struct ExpireCtx {
        WaitQueue* waiters;
        std::size_t* active_readers;
        bool* writer_active;
        Fiber** writer_owner;
    };

    // Construct an AsyncRwLock bound to `scheduler`, initially unlocked.
    // The Scheduler must outlive the AsyncRwLock.
    explicit AsyncRwLock(Scheduler& scheduler) noexcept
        : scheduler_(scheduler),
          active_readers_(0),
          writer_active_(false),
          writer_owner_(nullptr),
          expire_ctx_{&waiters_, &active_readers_, &writer_active_,
                      &writer_owner_} {}

    // Destruction contract: the AsyncRwLock must be fully released (no active
    // readers, no active writer) and its wait queue must be empty before
    // destruction. Does NOT cancel/wake/detach waiters.
    ~AsyncRwLock() {
        assert(active_readers_ == 0 &&
               "AsyncRwLock destroyed with active readers");
        assert(!writer_active_ &&
               "AsyncRwLock destroyed with active writer");
    }

    AsyncRwLock(const AsyncRwLock&) = delete;
    AsyncRwLock& operator=(const AsyncRwLock&) = delete;
    AsyncRwLock(AsyncRwLock&&) = delete;
    AsyncRwLock& operator=(AsyncRwLock&&) = delete;

    // --- Read acquisition ---

    // Attempt to acquire a read share WITHOUT suspending. Under G + W:
    //   if writer_active_ == false AND waiters_ is empty:
    //       active_readers_++ ; return true
    //   otherwise: return false (no mutation)
    // No reader barging: if ANY waiter is queued (reader or writer), a new
    // reader cannot acquire inline.
    [[nodiscard]] bool try_read_lock() {
        return scheduler_.rwlock_try_read_lock(waiters_, active_readers_,
                                               writer_active_);
    }

    // Acquire a read share, suspending the calling Fiber if not immediately
    // admissible. `node` must be Detached (fresh) and outlive this call.
    // Internally creates a stack-local RwWaitCtx{mode=read} and manages the
    // user_ pointer (caller does NOT construct this).
    void read_lock(WaitNode& node) {
        scheduler_.rwlock_read_lock(waiters_, active_readers_, writer_active_,
                                    node);
    }

    // Deadline-aware read acquisition. Composes read_lock with E11
    // TimerRegistration. Resolves Woken/Cancelled/Expired.
    void read_lock_until(WaitNode& node, Scheduler::deadline_t deadline) {
        scheduler_.rwlock_read_lock_until(waiters_, active_readers_,
                                          writer_active_, node, deadline,
                                          &expire_ctx_);
    }

    // --- Write acquisition ---

    // Attempt to acquire exclusive write ownership WITHOUT suspending.
    // Under G + W:
    //   if active_readers_ == 0 AND writer_active_ == false AND waiters_ empty
    //      AND current Fiber exists AND current Fiber != writer_owner_:
    //       writer_active_ = true; writer_owner_ = current Fiber; return true
    //   otherwise: return false (no mutation)
    // Recursive call by current owner returns false (no assertion).
    // External OS thread: FORBIDDEN (no current Fiber to record as owner).
    [[nodiscard]] bool try_write_lock() {
        return scheduler_.rwlock_try_write_lock(waiters_, active_readers_,
                                                writer_active_, writer_owner_);
    }

    // Acquire exclusive write ownership, suspending if not immediately
    // admissible. `node` must be Detached (fresh) and outlive this call.
    void write_lock(WaitNode& node) {
        scheduler_.rwlock_write_lock(waiters_, active_readers_, writer_active_,
                                     writer_owner_, node);
    }

    // Deadline-aware write acquisition.
    void write_lock_until(WaitNode& node, Scheduler::deadline_t deadline) {
        scheduler_.rwlock_write_lock_until(waiters_, active_readers_,
                                           writer_active_, writer_owner_,
                                           node, deadline, &expire_ctx_);
    }

    // --- Release ---

    // Release one read share. May be called from any context (Fiber or external
    // OS thread). Caller contract: holds a read share.
    // Under G + W: active_readers_--; if reaches 0, reconcile queue head.
    void unlock_read() noexcept {
        scheduler_.rwlock_unlock_read(waiters_, active_readers_, writer_active_,
                                      writer_owner_);
    }

    // Release exclusive write ownership. Requires: writer_active_ == true AND
    // writer_owner_ == current Fiber. Non-owner unlock is a caller contract
    // violation (debug assert).
    void unlock_write() noexcept {
        scheduler_.rwlock_unlock_write(waiters_, active_readers_,
                                       writer_active_, writer_owner_);
    }

    // --- Cancellation ---

    // Queue-identity-safe cancellation. Returns true ONLY if node is currently
    // Registered AND linked in THIS AsyncRwLock's WaitQueue AND CANCEL wins.
    // After unlink, performs head reconcile (may grant newly exposed waiters).
    // Safe from any OS thread.
    [[nodiscard]] bool cancel(WaitNode& node) {
        return scheduler_.rwlock_cancel(waiters_, active_readers_,
                                        writer_active_, writer_owner_, node);
    }

private:
    Scheduler& scheduler_;
    std::size_t active_readers_;
    bool writer_active_;
    Fiber* writer_owner_;
    WaitQueue waiters_;
    ExpireCtx expire_ctx_;  // stable context for timer hook
};

}  // namespace sluice::async
