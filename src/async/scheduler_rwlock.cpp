// Scheduler RwLock primitive — implementation TU split from scheduler.cpp in the
// post-freeze R1 structural pass (docs/post-freeze/structural-audit.md §6).
//
// Pure relocation: every definition below is byte-identical to its pre-split
// text at d9184de; the class declaration, lock domains, atomic orderings,
// and wake contracts remain in include/sluice/async/scheduler.hpp.
#include <sluice/async/scheduler.hpp>

#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/select.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "scheduler_internal.hpp"

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
namespace {
// Per-operation context stored on WaitNode::user_ for RwLock waiters.
// Stack-local in the lock function; alive for the entire suspension epoch.
struct RwWaitCtx {
    enum class Mode : std::uint8_t { read, write };
    Mode mode;
};
}  // anonymous namespace

// No-publication head-claim primitive. Caller MUST hold G + W. Resolves the
// node Woken (winner CAS), unlinks, retires timer, decrements
// waiting_waitq_count_, and clears the node's temporary next_/prev_ linkage.
// Returns true iff this call won the CAS. Does NOT publish. The caller is
// responsible for committing any resource state (active_readers_++ or
// writer_active_) BEFORE publishing, and for calling route_runnable_locked.
//
// AUTHORITY: shared by rwlock_grant_from_head_locked AND the inline admission
// recheck in the lock_* registration paths, so there is ONE resolve/unlink/
// retire/accounting sequence — not two potentially-drifted copies.
bool Scheduler::rwlock_claim_node_woken_locked(WaitQueue& waiters,
                                               WaitNode& node) {
    // Caller already holds G + W (verified by SLUICE_REQUIRES at the declaration
    // site). The resolve_ CAS is the single winner authority.
    bool won = waiters.wake_node_locked(node);
    if (!won) {
        // Under continuous G + W, a valid linked eligible node CANNOT lose its
        // CAS (E10 Unlink Law: terminal transition + unlink occur in the same
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
                                             Fiber*& writer_owner) {
    // Unified head-driven grant reconcile. Caller MUST hold G; this function
    // acquires W internally (like mutex_handoff_one_locked). Dispatches based
    // on queue head mode. Publication is under G after W release.
    //
    // Publication data is collected into a local intrusive list (readers) or a
    // single Fiber* (writer) while W is held, then published after W release.
    //
    // AUTHORITY: this is the ONLY head-driven grant path. The unlock_read,
    // unlock_write, cancel, and expiry reconcile flows all call this helper.
    // The inline admission recheck in the registration paths ALSO calls this
    // helper (see registration_admission_drift note in each lock_* function):
    // there is no second grant logic with potentially-drifted mode handling,
    // timer retirement, or accounting.
    Fiber* single_writer_fiber = nullptr;
    WaitNode* pub_head = nullptr;
    WaitNode* pub_tail = nullptr;
    std::size_t granted_readers = 0;
    bool granted_writer = false;

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

            Fiber* f = head->fiber();
            // Shared claim primitive: resolve + unlink + retire + accounting.
            // (Same primitive the inline admission recheck uses — no drift.)
            rwlock_claim_node_woken_locked(waiters, *head);
            // Commit ownership BEFORE publication.
            writer_active = true;
            writer_owner = f;
            single_writer_fiber = f;
            granted_writer = true;
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

    // Publication under G (W already released). Owner lookup is the I47-F1
    // authoritative owner_for_fiber_locked (audit #162 CPP-001: the former
    // fiber_owner_.find + g_worker fallback duplicated the lookup discipline
    // every other primitive fail-fasts through; fiber_owner_ is never erased,
    // so a missing entry is a Scheduler invariant violation, not a state to
    // route around).
    if (granted_writer) {
        if (single_writer_fiber != nullptr && single_writer_fiber->make_runnable()) {
            route_runnable_locked(single_writer_fiber,
                                  owner_for_fiber_locked(single_writer_fiber));
        }
    }
    WaitNode* w = pub_head;
    while (w != nullptr) {
        WaitNode* pub_next = w->next_;
        Fiber* fib = w->fiber();
        w->next_ = nullptr;
        w->prev_ = nullptr;
        if (fib != nullptr && fib->make_runnable()) {
            route_runnable_locked(fib, owner_for_fiber_locked(fib));
        }
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

void Scheduler::rwlock_read_lock(WaitQueue& waiters,
                                 std::size_t& active_readers,
                                 bool& writer_active,
                                 WaitNode& node) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::read_lock requires a running Fiber");
    Fiber* me = ws->current;
    assert(node.user() == nullptr && "AsyncRwLock::read_lock: node.user() must "
                                     "be nullptr on entry (caller contract)");
    // Stack-local context for this wait epoch.
    RwWaitCtx ctx{RwWaitCtx::Mode::read};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (!waiters.register_wait_locked(node, me)) {
            node.set_user(nullptr);
            return;  // C8 contract violation
        }
        ++waiting_waitq_count_;

        // REGISTRATION-ADMISSION-DRIFT NOTE.
        //
        // The design's authoritative admission recheck is
        // rwlock_grant_from_head_locked. We CANNOT call it directly from the
        // read registration path because this seam does not own a
        // `writer_owner&` slot (a read admission MUST NOT mutate writer
        // ownership). If the queue head were a writer admissible at this
        // instant, the helper would have to commit writer_active +
        // writer_owner — a write-side authority this path has no right to
        // perform. (Per docs/e12-rwlock.md §"Queued path (read_lock)", grant
        // is exclusively head-driven and writer ownership is committed only by
        // the writer-grant authority.)
        //
        // Equivalent authority is preserved by the following invariant: when a
        // reader registers and becomes the queue head, an earlier-registered
        // writer head CANNOT be admissible. Either (a) the writer was already
        // admissible and was granted by its own registration admission
        // recheck (writer-grant path), so it is no longer head; or (b) it was
        // not admissible (active_readers_ > 0), and remains so because this
        // read registration does not change active_readers_. Therefore, if
        // this node is the head AND no writer is active, the head IS this
        // reader and granting it is the same head-prefix claim the unified
        // helper would make. We share the claim mechanics via
        // rwlock_claim_node_woken_locked (the no-publication primitive that
        // unlock/cancel/expiry use internally through grant_from_head).
        if (node.prev_ == nullptr && !writer_active) {
            // This node is the FIFO head and no writer active: claim it.
            if (rwlock_claim_node_woken_locked(waiters, node)) {
                ++active_readers;
                node.set_user(nullptr);
                // The current Fiber is Running (it is executing this code on
                // a worker); make_runnable returns false and no publication
                // is needed — the caller continues without suspending.
                return;  // node.outcome() == woken; do NOT suspend
            }
        }

        // Defense-in-depth.
        if (node.is_terminal()) {
            waiters.unlink_locked(node);
            --waiting_waitq_count_;
            node.set_user(nullptr);
            return;
        }
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
                                      Fiber*& writer_owner) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::try_write_lock requires a running Fiber");
    Fiber* me = ws->current;
    LockGuard lk(global_mtx_);
    LockGuard qlk(waiters.mtx());
    // Recursive call by current owner: return false, no mutation.
    if (writer_owner == me) return false;
    if (active_readers == 0 && !writer_active && waiters.empty_locked()) {
        writer_active = true;
        writer_owner = me;
        return true;
    }
    return false;
}

void Scheduler::rwlock_write_lock(WaitQueue& waiters,
                                  std::size_t& active_readers,
                                  bool& writer_active,
                                  Fiber*& writer_owner,
                                  WaitNode& node) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::write_lock requires a running Fiber");
    Fiber* me = ws->current;
    assert(writer_owner != me && "AsyncRwLock::write_lock recursive acquisition "
                                 "is a caller precondition violation");
    assert(node.user() == nullptr && "AsyncRwLock::write_lock: node.user() must "
                                     "be nullptr on entry (caller contract)");
    RwWaitCtx ctx{RwWaitCtx::Mode::write};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (!waiters.register_wait_locked(node, me)) {
            node.set_user(nullptr);
            return;
        }
        ++waiting_waitq_count_;

        // Admission recheck (REGISTRATION-ADMISSION-DRIFT NOTE — see
        // rwlock_read_lock): use the shared no-publication claim primitive so
        // resolve/unlink/retire/accounting match the unlock/cancel/expiry
        // authority exactly. This node is the FIFO head and the resource is
        // free, so a head-prefix claim of exactly this node is the same grant
        // the unified helper would make.
        if (node.prev_ == nullptr && active_readers == 0 && !writer_active) {
            if (rwlock_claim_node_woken_locked(waiters, node)) {
                writer_active = true;
                writer_owner = me;
                node.set_user(nullptr);
                // Current Fiber is Running; make_runnable returns false, no
                // publication needed — the caller continues without suspending.
                return;
            }
        }

        if (node.is_terminal()) {
            waiters.unlink_locked(node);
            --waiting_waitq_count_;
            node.set_user(nullptr);
            return;
        }
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
                                   Fiber*& writer_owner) {
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
                                    Fiber*& writer_owner) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::unlock_write requires a running Fiber");
    Fiber* me = ws->current;
    LockGuard lk(global_mtx_);
    assert(writer_active && "AsyncRwLock::unlock_write while not write-locked "
                            "(caller contract violation)");
    assert(writer_owner == me && "AsyncRwLock::unlock_write by non-owner "
                                 "(caller contract violation)");
    (void)me;
    writer_active = false;
    writer_owner = nullptr;
    // Reconcile queue head (grant acquires W internally).
    rwlock_grant_from_head_locked(waiters, active_readers, writer_active,
                                  writer_owner);
}

bool Scheduler::rwlock_cancel(WaitQueue& waiters,
                              std::size_t& active_readers,
                              bool& writer_active,
                              Fiber*& writer_owner,
                              WaitNode& node) {
    LockGuard lk(global_mtx_);
    Fiber* cancel_fiber = nullptr;
    WorkerState* cancel_owner = nullptr;
    {
        LockGuard qlk(waiters.mtx());
        // Membership gate.
        if (!waiters.contains_locked(node)) return false;
        if (!waiters.cancel_locked(node)) return false;  // concurrent resolver won
        retire_timer_for_node_locked(node);
        if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
        // Capture publication data for the cancel winner (I47-F1
        // authoritative owner lookup; see the grant-path note).
        cancel_fiber = node.fiber();
        if (cancel_fiber != nullptr) {
            cancel_owner = owner_for_fiber_locked(cancel_fiber);
        }
    }  // W released

    // Head reconcile: the newly exposed head may be admissible.
    // grant acquires W internally.
    rwlock_grant_from_head_locked(waiters, active_readers, writer_active,
                                  writer_owner);

    // Publish cancel winner (after grant publications).
    if (cancel_fiber != nullptr && cancel_fiber->make_runnable()) {
        route_runnable_locked(cancel_fiber, cancel_owner);
    }
    return true;
}

bool Scheduler::rwlock_expire_wait(WaitQueue& waiters,
                                   std::size_t& active_readers,
                                   bool& writer_active,
                                   Fiber*& writer_owner,
                                   WaitNode& node) {
    // Called under G (from pump_deadlines_locked). Acquire W for expire, then
    // release; grant acquires W internally. Returns true iff expire_locked won.
    Fiber* exp_fiber = nullptr;
    WorkerState* exp_owner = nullptr;
    bool won = false;
    {
        LockGuard qlk(waiters.mtx());
        if (!waiters.expire_locked(node)) return false;  // lost to grant/cancel
        // Timer already CONSUMED by pump. Update accounting.
        if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
        // Capture publication data (I47-F1 authoritative owner lookup; see
        // the grant-path note).
        exp_fiber = node.fiber();
        if (exp_fiber != nullptr) {
            exp_owner = owner_for_fiber_locked(exp_fiber);
        }
        won = true;
    }  // W released

    // Head reconcile (grant acquires W internally).
    rwlock_grant_from_head_locked(waiters, active_readers, writer_active,
                                  writer_owner);

    // Publish expired waiter.
    if (exp_fiber != nullptr && exp_fiber->make_runnable()) {
        route_runnable_locked(exp_fiber, exp_owner);
    }
    return won;
}

void Scheduler::rwlock_read_lock_until(WaitQueue& waiters,
                                       std::size_t& active_readers,
                                       bool& writer_active,
                                       WaitNode& node,
                                       deadline_t deadline,
                                       void* expire_ctx) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::read_lock_until requires a running Fiber");
    Fiber* me = ws->current;
    assert(node.user() == nullptr && "AsyncRwLock::read_lock_until: node.user() "
                                     "must be nullptr on entry");
    RwWaitCtx ctx{RwWaitCtx::Mode::read};
    node.set_user(&ctx);
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (!waiters.register_wait_locked(node, me)) {
            node.set_user(nullptr);
            return;
        }
        ++waiting_waitq_count_;
        // Create timer registration.
        timer_pool_.emplace_back(&node, &waiters, deadline);
        reg = &timer_pool_.back();
        reg->on_resolve_ = &rwlock_timer_expire_reconcile;
        reg->owner_ctx_ = expire_ctx;
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();

        // Admission precedence 1: resource admission wins over due deadline.
        // REGISTRATION-ADMISSION-DRIFT NOTE (see rwlock_read_lock): use the
        // shared no-publication claim primitive. claim_node retires the bound
        // TimerRegistration internally (via retire_timer_for_node_locked) and
        // recomputes the earliest deadline, so we only need to commit the
        // reader-side resource state afterward.
        if (node.prev_ == nullptr && !writer_active) {
            if (rwlock_claim_node_woken_locked(waiters, node)) {
                ++active_readers;
                node.set_user(nullptr);
                // Current Fiber is Running; make_runnable returns false. The
                // caller continues without suspending.
                return;
            }
        }

        // Admission precedence 2: already-due deadline (E11 I5).
        // Gate the timer-accounting cleanup on a successful try_claim_expiry():
        // expire_locked won the resolve CAS, but the bound timer is still
        // ACTIVE until claimed. If the claim somehow fails (should be
        // unreachable under continuous G+W — the node is terminal and the
        // timer is unclaimed), we must NOT decrement active_deadline_count_
        // for a timer we did not consume.
        if (clock_now_unlocked() >= deadline) {
            if (waiters.expire_locked(node)) {
                if (reg->try_claim_expiry()) --active_deadline_count_;
                else assert(false && "E12-F read_lock_until: try_claim_expiry "
                                     "failed after expire_locked win (Category B)");
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                node.set_user(nullptr);
                // The current Fiber is RUNNING (never called make_waiting());
                // it continues inline. No runnable publication is needed and
                // make_runnable would be a no-op from running (audit #162
                // CPP-002 removed the dead call).
                return;
            }
        }

        if (node.is_terminal()) {
            waiters.unlink_locked(node);
            --waiting_waitq_count_;
            if (reg->retire()) --active_deadline_count_;
            recompute_earliest_deadline_locked();
            node.set_user(nullptr);
            return;
        }
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
                                        Fiber*& writer_owner,
                                        WaitNode& node,
                                        deadline_t deadline,
                                        void* expire_ctx) {
    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::write_lock_until requires a running Fiber");
    Fiber* me = ws->current;
    assert(writer_owner != me && "AsyncRwLock::write_lock_until recursive "
                                 "acquisition is a caller precondition violation");
    assert(node.user() == nullptr && "AsyncRwLock::write_lock_until: node.user() "
                                     "must be nullptr on entry");
    RwWaitCtx ctx{RwWaitCtx::Mode::write};
    node.set_user(&ctx);
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (!waiters.register_wait_locked(node, me)) {
            node.set_user(nullptr);
            return;
        }
        ++waiting_waitq_count_;
        timer_pool_.emplace_back(&node, &waiters, deadline);
        reg = &timer_pool_.back();
        reg->on_resolve_ = &rwlock_timer_expire_reconcile;
        reg->owner_ctx_ = expire_ctx;
        ++active_deadline_count_;
        heap_push_ordinary_locked(reg);
        recompute_earliest_deadline_locked();

        // Admission precedence 1: resource admission.
        // REGISTRATION-ADMISSION-DRIFT NOTE (see rwlock_read_lock): use the
        // shared no-publication claim primitive. claim_node retires the bound
        // TimerRegistration internally and recomputes the earliest deadline.
        if (node.prev_ == nullptr && active_readers == 0 && !writer_active) {
            if (rwlock_claim_node_woken_locked(waiters, node)) {
                writer_active = true;
                writer_owner = me;
                node.set_user(nullptr);
                // Current Fiber is Running; no publication needed.
                return;
            }
        }

        // Admission precedence 2: already-due deadline.
        // Gate the timer-accounting cleanup on a successful try_claim_expiry()
        // (same reasoning as rwlock_read_lock_until).
        if (clock_now_unlocked() >= deadline) {
            if (waiters.expire_locked(node)) {
                if (reg->try_claim_expiry()) --active_deadline_count_;
                else assert(false && "E12-F write_lock_until: try_claim_expiry "
                                     "failed after expire_locked win (Category B)");
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                node.set_user(nullptr);
                // Same as read_lock_until: the Fiber is RUNNING and continues
                // inline; no publication (audit #162 CPP-002).
                return;
            }
        }

        if (node.is_terminal()) {
            waiters.unlink_locked(node);
            --waiting_waitq_count_;
            if (reg->retire()) --active_deadline_count_;
            recompute_earliest_deadline_locked();
            node.set_user(nullptr);
            return;
        }
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
