// Scheduler RwLock primitive — implementation TU split from scheduler.cpp
// (docs/post-freeze/structural-audit.md §6).
//
// The class declaration, lock domains, atomic orderings, and wake contracts
// remain in include/sluice/async/scheduler.hpp.
//
// FE-3 RwLock vertical slice: the admission closures are extracted into the
// ONE shared rwlock_read_admit_locked / rwlock_write_admit_locked ladders
// (one textual admission law per mode, blocking+timed, parameterized by the
// frontend's WaitResume token), and every winner publication edge routes
// through publish_wait_winner_locked (winner-kind switch). Writer ownership
// identity is ActorId (FE-1b A1): ownership semantics do not depend on the
// ResumeTarget delivery token.
#include <sluice/async/scheduler.hpp>

#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/select.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "scheduler_internal.hpp"  // g_worker + RwWaitCtx (shared, non-installed)

#include <utility>
#include <cstdio>
#include <cstdlib>

// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the internal-testing variant pulls in
// the non-installed test-control header so the phase call sites below resolve to
// the controller. In the production build this include is absent and the call
// sites compile to nothing.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {

// No-publication head-claim primitive. Caller MUST hold G + W. Resolves the
// node Woken (winner CAS), unlinks, retires timer, decrements
// waiting_waitq_count_, and clears the node's temporary next_/prev_ linkage.
// Returns true iff this call won the CAS. Does NOT publish. The caller is
// responsible for committing any resource state (active_readers_++ or
// writer_active_) BEFORE publishing, and for calling route_runnable_locked.
//
// AUTHORITY: shared by rwlock_grant_from_head_locked AND the inline admission
// recheck in the ladders, so there is ONE resolve/unlink/retire/accounting
// sequence — not two potentially-drifted copies.
bool Scheduler::rwlock_claim_node_woken_locked(WaitQueue& waiters,
                                               WaitNode& node) {
    // Caller already holds G + W (verified by SLUICE_REQUIRES at the declaration
    // site). The resolve_ CAS is the single winner authority.
    bool won = waiters.wake_node_locked(node);
    if (!won) {
        // Under continuous G + W, a valid linked eligible node CANNOT lose its
        // CAS (Unlink Law: terminal transition + unlink occur in the same
        // CS; a linked node is therefore Registered and resolvable). A loss
        // here is internal corruption — Category B fail-fast (debug assert +
        // deterministic Release abort). Do NOT silently treat as success.
        assert(false && "E12-F claim_node: wake_node_locked failed for linked "
                        "node (Category B internal invariant violation)");
        std::abort();
    }
    retire_timer_for_node_locked(node);
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // Clear any residual intrusive linkage so the caller (batch loop or
    // publication loop) cannot observe a stale queue position.
    node.next_ = nullptr;
    node.prev_ = nullptr;
    return true;
}

void Scheduler::rwlock_grant_from_head_locked(WaitQueue& waiters,
                                             std::size_t& active_readers,
                                             bool& writer_active,
                                             ActorId& writer_owner) {
    // Unified head-driven grant reconcile. Caller MUST hold G; this function
    // acquires W internally (like mutex_handoff_one_locked). Dispatches based
    // on queue head mode. Publication is under G after W release.
    //
    // Publication data is collected into a local intrusive list (readers) or
    // a single node pointer (writer) while W is held, then published after W
    // release through the ONE winner-kind tail (fiber stackful route /
    // deferred delivery obligation / none).
    //
    // AUTHORITY: this is the ONLY head-driven grant path. The unlock_read,
    // unlock_write, cancel, and expiry reconcile flows all call this helper.
    // The inline admission recheck in the ladders ALSO shares the claim
    // primitive (see the REGISTRATION-ADMISSION-DRIFT note at the read
    // ladder): there is no second grant logic with potentially-drifted mode
    // handling, timer retirement, or accounting.
    //
    // FE-3 ownership commit: the writer grant commits the WINNER's
    // ActorIdentity (RwWaitCtx::actor — bound by the admitting frontend),
    // never the ResumeTarget delivery token.
    WaitNode* writer_node = nullptr;
    WaitNode* pub_head = nullptr;
    WaitNode* pub_tail = nullptr;
    std::size_t granted_readers = 0;

    {
        LockGuard qlk(waiters.mtx());
        if (waiters.head_ == nullptr) return;  // empty queue

        WaitNode* head = waiters.head_;
        auto* ctx = static_cast<RwWaitCtx*>(head->user());
        if (ctx == nullptr) {
            assert(false && "E12-F grant_from_head: linked head has null user_ "
                            "(internal invariant violation)");
            std::abort();
            return;
        }

        // Explicit mode dispatch with a Category B fail-fast default. A linked
        // node's mode MUST be read or write; any other value is internal state
        // corruption (the Scheduler set user_ before registration and is the
        // only authority that clears it while the node is linked). Treating an
        // unknown mode as either reader or writer would silently mask the
        // corruption; instead we deterministically abort.
        switch (ctx->mode) {
        case RwWaitCtx::Mode::write: {
            // --- Writer grant: grant exactly ONE writer ---
            if (active_readers > 0 || writer_active) return;  // not admissible

            // Shared claim primitive: resolve + unlink + retire + accounting.
            // (Same primitive the inline admission recheck uses — no drift.)
            rwlock_claim_node_woken_locked(waiters, *head);
            // Commit ownership BEFORE publication: the winner's ACTOR identity.
            writer_active = true;
            writer_owner = ctx->actor;
            writer_node = head;
            break;
        }
        case RwWaitCtx::Mode::read: {
            // --- Reader batch grant: maximal consecutive reader prefix ---
            if (writer_active) return;  // not admissible

            for (WaitNode* n = waiters.head_; n != nullptr; ) {
                WaitNode* next = n->next_;  // cache BEFORE claim clears links
                auto* nctx = static_cast<RwWaitCtx*>(n->user());
                if (nctx == nullptr) {
                    assert(false && "E12-F reader_batch: linked node has null "
                                    "user_ (internal invariant violation)");
                    std::abort();
                    return;
                }
                // Per-node mode check: read continues the batch; write is a
                // batch boundary (correct FIFO stop); any other value is an
                // internal corruption that MUST NOT be silently treated as
                // either (Category B fail-fast).
                switch (nctx->mode) {
                case RwWaitCtx::Mode::read:
                    break;  // batch member
                case RwWaitCtx::Mode::write:
                    goto batch_done;  // writer stops the batch (FIFO boundary)
                default:
                    assert(false && "E12-F reader_batch: linked node has invalid "
                                    "mode (Category B)");
                    std::abort();
                }
                // Shared claim primitive (resolves + unlinks + retires + clears
                // next_/prev_ — so we thread onto the publication list AFTER).
                rwlock_claim_node_woken_locked(waiters, *n);
                // Thread onto local publication list (claim cleared links).
                n->next_ = nullptr;
                n->prev_ = pub_tail;
                if (pub_tail != nullptr) pub_tail->next_ = n;
                else pub_head = n;
                pub_tail = n;
                ++granted_readers;
                n = next;
            }
        batch_done:
            if (granted_readers > 0) active_readers += granted_readers;
            break;
        }
        default:
            // Category B: a linked node carries a mode that is neither read nor
            // write. Debug: precise assert. Release: deterministic fail-fast.
            assert(false && "E12-F grant_from_head: linked head has invalid mode "
                            "(Category B internal invariant violation)");
            std::abort();
            return;
        }
    }  // W released here

    // Publication under G (W already released) through the ONE winner-kind
    // tail. The fiber branch is the unchanged stackful route (authoritative
    // owner_for_fiber_locked + exactly-once make_runnable guard + worker
    // routing); the deferred branch commits the delivery obligation; none
    // publishes nothing (the historical null-Fiber skip).
    if (writer_node != nullptr) {
        publish_wait_winner_locked(*writer_node);
    }
    WaitNode* w = pub_head;
    while (w != nullptr) {
        WaitNode* pub_next = w->next_;
        w->next_ = nullptr;
        w->prev_ = nullptr;
        publish_wait_winner_locked(*w);
        w = pub_next;
    }
}

bool Scheduler::rwlock_try_read_lock(WaitQueue& waiters,
                                     std::size_t& active_readers,
                                     bool& writer_active) {
    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());
    if (!writer_active && waiters.empty_locked()) {
        ++active_readers;
        return true;
    }
    return false;
}

// ---- FE shared RwLock admission ladders (FE-3) -----------------------------
//
// ONE textual admission sequence per mode, shared by EVERY frontend (stackful
// Fiber entries / deferred awaiters). Pure admission law: no g_worker access,
// no coroutine knowledge — only the ResumeTarget token (+ the write mode's
// actor for the ownership commit). The ENTRY commits its frontend-specific
// PublicationEligibility in the SAME critical section when `authorized`
// (fiber: commit_suspend_locked; deferred: frontend record arm) and suspends
// physically outside the lock (FE-1b L7/L10). Inline dispositions publish
// nothing — the caller never suspended (L6).

// REGISTRATION-ADMISSION-DRIFT NOTE (lives here ONCE now, for both modes).
//
// The design's authoritative admission recheck is
// rwlock_grant_from_head_locked. The READ ladder cannot call it directly
// because this path does not own a writer_owner slot (a read admission MUST
// NOT mutate writer ownership). If the queue head were an admissible writer,
// the helper would have to commit writer_active + writer_owner — a write-side
// authority the read path has no right to perform. Equivalent authority is
// preserved by the following invariant: when a reader registers and becomes
// the queue head, an earlier-registered writer head CANNOT be admissible.
// Either (a) the writer was already admissible and was granted by its own
// registration admission recheck (writer-grant path), so it is no longer
// head; or (b) it was not admissible (active_readers_ > 0), and remains so
// because a read registration does not change active_readers_. Therefore, if
// this node is the head AND no writer is active, the head IS this reader and
// granting it is the same head-prefix claim the unified helper would make.
// Both ladders share the claim mechanics via rwlock_claim_node_woken_locked
// (the no-publication primitive that unlock/cancel/expiry use internally
// through grant_from_head).
Scheduler::WaitAdmitDisposition Scheduler::rwlock_read_admit_locked(
    WaitQueue& waiters, std::size_t& active_readers, bool& writer_active,
    WaitNode& node, const WaitResume& resume, bool timed, deadline_t deadline,
    void* expire_ctx)
    SLUICE_REQUIRES(global_mtx_, waiters.mtx()) {
    TimerRegistration* reg = nullptr;
    if (timed) {
        // R2-ALLOC: allocations before any admission state mutation (a
        // bad_alloc here leaves the node Detached and all counters intact).
        reg = prepare_ordinary_deadline_locked(&node, &waiters, deadline);
    }
    if (!waiters.register_wait_locked(node, resume)) {
        if (timed) erase_popped_registration_locked(reg);  // never published
        return WaitAdmitDisposition::rejected;
    }
    ++waiting_waitq_count_;
    if (timed) {
        // Publish via the ordinary deadline authority, attaching the
        // RwLock-only expire/reconcile binding INSIDE the same G critical
        // section.
        publish_ordinary_deadline_locked(reg, &rwlock_timer_expire_reconcile,
                                         expire_ctx);
    }
    // Admission precedence 1: resource admission wins over a due deadline.
    if (node.prev_ == nullptr && !writer_active) {
        // This node is the FIFO head and no writer active: claim it.
        if (rwlock_claim_node_woken_locked(waiters, node)) {
            ++active_readers;
            node.set_user(nullptr);
            // The caller is Running (executing this code, or about to observe
            // the inline outcome on its own thread): no publication is needed
            // — the caller continues without suspending (L6).
            return WaitAdmitDisposition::resolved_inline;
        }
    }
    if (timed) {
        // Admission precedence 2: already-due deadline. Gate the
        // timer-accounting cleanup on a successful consume (expire_locked won
        // the resolve CAS, but the bound timer is still ACTIVE until claimed;
        // a failed claim is the same Category B unreachable state as before —
        // under continuous G+W the node is terminal and the timer unclaimed).
        if (clock_now_unlocked() >= deadline) {
            if (waiters.expire_locked(node)) {
                if (!consume_ordinary_deadline_locked(*reg))
                    assert(false && "E12-F read_lock_until: try_claim_expiry "
                                    "failed after expire_locked win (Category B)");
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                node.set_user(nullptr);
                return WaitAdmitDisposition::resolved_inline;
            }
        }
    }
    // Defense-in-depth.
    if (node.is_terminal()) {
        waiters.unlink_locked(node);
        --waiting_waitq_count_;
        if (timed) {
            // ACTIVE->RETIRED via the ordinary deadline authority.
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
        }
        node.set_user(nullptr);
        return WaitAdmitDisposition::resolved_inline;
    }
    // Suspension authorized. The epoch is Registered, un-resolved, and every
    // resolver is excluded until THIS critical section releases; the entry
    // commits its PublicationEligibility now (contract L7) and suspends
    // physically afterwards (L10).
    return WaitAdmitDisposition::authorized;
}

Scheduler::WaitAdmitDisposition Scheduler::rwlock_write_admit_locked(
    WaitQueue& waiters, std::size_t& active_readers, bool& writer_active,
    ActorId& writer_owner, WaitNode& node, const WaitResume& resume,
    const ActorId& actor, bool timed, deadline_t deadline, void* expire_ctx)
    SLUICE_REQUIRES(global_mtx_, waiters.mtx()) {
    TimerRegistration* reg = nullptr;
    if (timed) {
        reg = prepare_ordinary_deadline_locked(&node, &waiters, deadline);
    }
    if (!waiters.register_wait_locked(node, resume)) {
        if (timed) erase_popped_registration_locked(reg);  // never published
        return WaitAdmitDisposition::rejected;
    }
    ++waiting_waitq_count_;
    if (timed) {
        publish_ordinary_deadline_locked(reg, &rwlock_timer_expire_reconcile,
                                         expire_ctx);
    }
    // Admission precedence 1: resource admission. This node is the FIFO head
    // and the resource is free, so a head-prefix claim of exactly this node is
    // the same grant the unified helper would make (see the read ladder's
    // REGISTRATION-ADMISSION-DRIFT note). claim_node retires the bound
    // TimerRegistration internally, so only the ownership commit remains.
    if (node.prev_ == nullptr && active_readers == 0 && !writer_active) {
        if (rwlock_claim_node_woken_locked(waiters, node)) {
            writer_active = true;
            writer_owner = actor;  // the caller's ACTOR identity (FE-1b A1)
            node.set_user(nullptr);
            return WaitAdmitDisposition::resolved_inline;
        }
    }
    if (timed) {
        if (clock_now_unlocked() >= deadline) {
            if (waiters.expire_locked(node)) {
                if (!consume_ordinary_deadline_locked(*reg))
                    assert(false && "E12-F write_lock_until: try_claim_expiry "
                                    "failed after expire_locked win (Category B)");
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                node.set_user(nullptr);
                return WaitAdmitDisposition::resolved_inline;
            }
        }
    }
    if (node.is_terminal()) {
        waiters.unlink_locked(node);
        --waiting_waitq_count_;
        if (timed) {
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
        }
        node.set_user(nullptr);
        return WaitAdmitDisposition::resolved_inline;
    }
    return WaitAdmitDisposition::authorized;
}

void Scheduler::rwlock_read_lock(WaitQueue& waiters,
                                 std::size_t& active_readers,
                                 bool& writer_active,
                                 WaitNode& node) {
    // Blocking read — stackful (Fiber) frontend entry over the SHARED read
    // ladder. The RwWaitCtx is stack-local; on a Fiber the stack persists
    // across the context switch.
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::read_lock requires a running Fiber");
    Fiber* me = ws->current;
    assert(node.user() == nullptr && "AsyncRwLock::read_lock: node.user() must "
                                     "be nullptr on entry (caller contract)");
    RwWaitCtx ctx{RwWaitCtx::Mode::read, ActorId::none()};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (rwlock_read_admit_locked(waiters, active_readers, writer_active,
                                     node, WaitResume::fiber(me),
                                     /*timed=*/false, deadline_t{},
                                     /*expire_ctx=*/nullptr) !=
            WaitAdmitDisposition::authorized) {
            return;  // rejected or resolved inline: this fiber never suspends
        }
        // Fiber-kind PublicationEligibility commit (FE-1b L7).
        commit_suspend_locked(ws, me);
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    // Resumed: clear user_ before return.
    node.set_user(nullptr);
}

bool Scheduler::rwlock_try_write_lock(WaitQueue& waiters,
                                      std::size_t& active_readers,
                                      bool& writer_active,
                                      ActorId& writer_owner) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::try_write_lock requires a running Fiber");
    Fiber* me = ws->current;
    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());
    return rwlock_try_write_admission_locked(waiters, active_readers,
                                             writer_active, writer_owner,
                                             ActorId::fiber(me));
}

// Shared inline try-write admission core (the ONE textual ownership
// decision; see scheduler.hpp). `caller` is the calling ACTOR's identity.
bool Scheduler::rwlock_try_write_admission_locked(WaitQueue& waiters,
                                                  std::size_t& active_readers,
                                                  bool& writer_active,
                                                  ActorId& writer_owner,
                                                  const ActorId& caller)
    SLUICE_REQUIRES(global_mtx_, waiters.mtx()) {
    // Recursive call by the current owner ACTOR: return false, no mutation.
    if (writer_owner == caller) return false;
    if (active_readers == 0 && !writer_active && waiters.empty_locked()) {
        writer_active = true;
        writer_owner = caller;
        return true;
    }
    return false;
}

void Scheduler::rwlock_write_lock(WaitQueue& waiters,
                                  std::size_t& active_readers,
                                  bool& writer_active,
                                  ActorId& writer_owner,
                                  WaitNode& node) {
    // Blocking write — stackful frontend entry over the SHARED write ladder.
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::write_lock requires a running Fiber");
    Fiber* me = ws->current;
    // Recursive acquisition is a caller precondition violation: named
    // fail-fast active in Debug AND Release (the historical debug-only
    // assert, re-based onto ActorId; the try form returns false instead).
    if (writer_owner == ActorId::fiber(me)) {
        detail::async_rwlock_recursive_write_fail_fast();
    }
    assert(node.user() == nullptr && "AsyncRwLock::write_lock: node.user() must "
                                     "be nullptr on entry (caller contract)");
    RwWaitCtx ctx{RwWaitCtx::Mode::write, ActorId::fiber(me)};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (rwlock_write_admit_locked(waiters, active_readers, writer_active,
                                      writer_owner, node, WaitResume::fiber(me),
                                      ActorId::fiber(me), /*timed=*/false,
                                      deadline_t{},
                                      /*expire_ctx=*/nullptr) !=
            WaitAdmitDisposition::authorized) {
            return;  // rejected or resolved inline: this fiber never suspends
        }
        // Fiber-kind PublicationEligibility commit (FE-1b L7).
        commit_suspend_locked(ws, me);
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    node.set_user(nullptr);
}

void Scheduler::rwlock_unlock_read(WaitQueue& waiters,
                                   std::size_t& active_readers,
                                   bool& writer_active,
                                   ActorId& writer_owner) {
    LockGuard lk(global_mtx_);
    assert(active_readers > 0 && "AsyncRwLock::unlock_read without held share "
                                 "(caller contract violation)");
    --active_readers;
    if (active_readers > 0) return;  // other readers still hold
    // Last reader released: reconcile queue head (grant acquires W internally).
    rwlock_grant_from_head_locked(waiters, active_readers, writer_active,
                                  writer_owner);
}

void Scheduler::rwlock_unlock_write(WaitQueue& waiters,
                                    std::size_t& active_readers,
                                    bool& writer_active,
                                    ActorId& writer_owner) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::unlock_write requires a running Fiber");
    Fiber* me = ws->current;
    LockGuard lk(global_mtx_);
    rwlock_unlock_write_core_locked(waiters, active_readers, writer_active,
                                    writer_owner, ActorId::fiber(me));
}

// Shared checked write-release core (the ONE textual ownership check +
// release + head reconcile; see scheduler.hpp). `caller` is the calling
// ACTOR's identity.
void Scheduler::rwlock_unlock_write_core_locked(WaitQueue& waiters,
                                                std::size_t& active_readers,
                                                bool& writer_active,
                                                ActorId& writer_owner,
                                                const ActorId& caller)
    SLUICE_REQUIRES(global_mtx_) {
    if (!writer_active) {
        // Not write-locked: caller contract violation. Named fail-fast in
        // Debug AND Release (the historical debug-only assert).
        detail::async_rwlock_unlock_write_inactive_fail_fast();
    }
    if (writer_owner != caller) {
        // Non-owner release: caller contract violation. The ownership check
        // compares ACTOR identity — a frontend actor releases with its own
        // token, never with a ResumeTarget (FE-1b A1).
        detail::async_rwlock_unlock_write_not_owner_fail_fast();
    }
    writer_active = false;
    writer_owner = ActorId::none();
    // Reconcile queue head (grant acquires W internally).
    rwlock_grant_from_head_locked(waiters, active_readers, writer_active,
                                  writer_owner);
}

bool Scheduler::rwlock_cancel(WaitQueue& waiters,
                              std::size_t& active_readers,
                              bool& writer_active,
                              ActorId& writer_owner,
                              WaitNode& node) {
    LockGuard lk(global_mtx_);
    {
        LockGuard qlk(waiters.mtx());
        // Membership gate.
        if (!cancel_primitive_wait_locked(waiters, node)) return false;
        if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    }  // W released

    // Head reconcile: the newly exposed head may be admissible.
    // grant acquires W internally.
    rwlock_grant_from_head_locked(waiters, active_readers, writer_active,
                                  writer_owner);

    // Publish cancel winner (after grant publications) through the ONE
    // winner-kind tail; its result does NOT undo the cancel.
    publish_wait_winner_locked(node);
    return true;
}

bool Scheduler::rwlock_expire_wait(WaitQueue& waiters,
                                   std::size_t& active_readers,
                                   bool& writer_active,
                                   ActorId& writer_owner,
                                   WaitNode& node) {
    // Called under G (from pump_deadlines_locked). Acquire W for expire, then
    // release; grant acquires W internally. Returns true iff expire_locked won.
    {
        LockGuard qlk(waiters.mtx());
        if (!waiters.expire_locked(node)) return false;  // lost to grant/cancel
        // Timer already CONSUMED by pump. Update accounting.
        if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    }  // W released

    // Head reconcile (grant acquires W internally).
    rwlock_grant_from_head_locked(waiters, active_readers, writer_active,
                                  writer_owner);

    // Publish expired waiter through the ONE winner-kind tail.
    publish_wait_winner_locked(node);
    return true;
}

void Scheduler::rwlock_read_lock_until(WaitQueue& waiters,
                                       std::size_t& active_readers,
                                       bool& writer_active,
                                       WaitNode& node,
                                       deadline_t deadline,
                                       void* expire_ctx) {
    // Timed read — stackful frontend entry over the SHARED read ladder with
    // the ordinary deadline authority (timed=true).
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::read_lock_until requires a running Fiber");
    Fiber* me = ws->current;
    assert(node.user() == nullptr && "AsyncRwLock::read_lock_until: node.user() "
                                     "must be nullptr on entry");
    RwWaitCtx ctx{RwWaitCtx::Mode::read, ActorId::none()};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (rwlock_read_admit_locked(waiters, active_readers, writer_active,
                                     node, WaitResume::fiber(me),
                                     /*timed=*/true, deadline,
                                     expire_ctx) !=
            WaitAdmitDisposition::authorized) {
            return;  // rejected or resolved inline: this fiber never suspends
        }
        // Fiber-kind PublicationEligibility commit (FE-1b L7).
        commit_suspend_locked(ws, me);
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    node.set_user(nullptr);
}

void Scheduler::rwlock_write_lock_until(WaitQueue& waiters,
                                        std::size_t& active_readers,
                                        bool& writer_active,
                                        ActorId& writer_owner,
                                        WaitNode& node,
                                        deadline_t deadline,
                                        void* expire_ctx) {
    // Timed write — stackful frontend entry over the SHARED write ladder.
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::write_lock_until requires a running Fiber");
    Fiber* me = ws->current;
    // Recursive acquisition: named fail-fast (Debug AND Release) — see
    // rwlock_write_lock.
    if (writer_owner == ActorId::fiber(me)) {
        detail::async_rwlock_recursive_write_fail_fast();
    }
    assert(node.user() == nullptr && "AsyncRwLock::write_lock_until: node.user() "
                                     "must be nullptr on entry");
    RwWaitCtx ctx{RwWaitCtx::Mode::write, ActorId::fiber(me)};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (rwlock_write_admit_locked(waiters, active_readers, writer_active,
                                      writer_owner, node, WaitResume::fiber(me),
                                      ActorId::fiber(me), /*timed=*/true,
                                      deadline, expire_ctx) !=
            WaitAdmitDisposition::authorized) {
            return;  // rejected or resolved inline: this fiber never suspends
        }
        // Fiber-kind PublicationEligibility commit (FE-1b L7).
        commit_suspend_locked(ws, me);
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
    node.set_user(nullptr);
}

void Scheduler::rwlock_timer_expire_reconcile(void* owner_ctx,
                                              bool timer_won) noexcept {
    // This is the on_resolve_ hook installed on RwLock TimerRegistrations.
    // For RwLock, we only need to handle the timer-won case (the pump calls
    // this AFTER try_claim_expiry succeeds). The timer-lost case (retired by
    // grant/cancel) is handled by retire_timer_for_node_locked which already
    // does the accounting. This hook is a no-op for timer_won=false because
    // the standard retire path handles it.
    //
    // NOTE: The actual expiry reconcile (resolve + unlink + head advance) is
    // performed by rwlock_expire_wait which is called from pump_deadlines_locked
    // directly. This hook exists only for the on_resolve_ accounting pattern.
    (void)owner_ctx;
    (void)timer_won;
}

// ===========================================================================

}  // namespace sluice::async
