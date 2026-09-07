












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





#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {











bool Scheduler::rwlock_claim_node_woken_locked(WaitQueue& waiters,
                                               WaitNode& node) {


    bool won = waiters.wake_node_locked(node);
    if (!won) {





        assert(false && "E12-F claim_node: wake_node_locked failed for linked "
                        "node (Category B internal invariant violation)");
        std::abort();
    }
    retire_timer_for_node_locked(node);
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;


    node.next_ = nullptr;
    node.prev_ = nullptr;
    return true;
}

void Scheduler::rwlock_grant_from_head_locked(WaitQueue& waiters,
                                             std::size_t& active_readers,
                                             bool& writer_active,
                                             ActorId& writer_owner) {



















    WaitNode* writer_node = nullptr;
    WaitNode* pub_head = nullptr;
    WaitNode* pub_tail = nullptr;
    std::size_t granted_readers = 0;

    {
        LockGuard qlk(waiters.mtx());
        if (waiters.head_ == nullptr) return;

        WaitNode* head = waiters.head_;
        auto* ctx = static_cast<RwWaitCtx*>(head->user());
        if (ctx == nullptr) {
            assert(false && "E12-F grant_from_head: linked head has null user_ "
                            "(internal invariant violation)");
            std::abort();
            return;
        }







        switch (ctx->mode) {
        case RwWaitCtx::Mode::write: {

            if (active_readers > 0 || writer_active) return;



            rwlock_claim_node_woken_locked(waiters, *head);

            writer_active = true;
            writer_owner = ctx->actor;
            writer_node = head;
            break;
        }
        case RwWaitCtx::Mode::read: {

            if (writer_active) return;

            for (WaitNode* n = waiters.head_; n != nullptr; ) {
                WaitNode* next = n->next_;
                auto* nctx = static_cast<RwWaitCtx*>(n->user());
                if (nctx == nullptr) {
                    assert(false && "E12-F reader_batch: linked node has null "
                                    "user_ (internal invariant violation)");
                    std::abort();
                    return;
                }




                switch (nctx->mode) {
                case RwWaitCtx::Mode::read:
                    break;
                case RwWaitCtx::Mode::write:
                    goto batch_done;
                default:
                    assert(false && "E12-F reader_batch: linked node has invalid "
                                    "mode (Category B)");
                    std::abort();
                }


                rwlock_claim_node_woken_locked(waiters, *n);

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


            assert(false && "E12-F grant_from_head: linked head has invalid mode "
                            "(Category B internal invariant violation)");
            std::abort();
            return;
        }
    }






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































Scheduler::WaitAdmitDisposition Scheduler::rwlock_read_admit_locked(
    WaitQueue& waiters, std::size_t& active_readers, bool& writer_active,
    WaitNode& node, const WaitResume& resume, bool timed, deadline_t deadline,
    void* expire_ctx)
    SLUICE_REQUIRES(global_mtx_, waiters.mtx()) {
    TimerRegistration* reg = nullptr;
    if (timed) {


        reg = prepare_ordinary_deadline_locked(&node, &waiters, deadline);
    }
    if (!waiters.register_wait_locked(node, resume)) {
        if (timed) erase_popped_registration_locked(reg);
        return WaitAdmitDisposition::rejected;
    }
    ++waiting_waitq_count_;
    if (timed) {



        publish_ordinary_deadline_locked(reg, &rwlock_timer_expire_reconcile,
                                         expire_ctx);
    }

    if (node.prev_ == nullptr && !writer_active) {

        if (rwlock_claim_node_woken_locked(waiters, node)) {
            ++active_readers;
            node.set_user(nullptr);



            return WaitAdmitDisposition::resolved_inline;
        }
    }
    if (timed) {





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

Scheduler::WaitAdmitDisposition Scheduler::rwlock_write_admit_locked(
    WaitQueue& waiters, std::size_t& active_readers, bool& writer_active,
    ActorId& writer_owner, WaitNode& node, const WaitResume& resume,
    const ActorId& actor, bool timed, deadline_t deadline, void* expire_ctx)
    SLUICE_REQUIRES(global_mtx_, waiters.mtx()) {









    if (writer_owner == actor) {
        detail::async_rwlock_recursive_write_fail_fast();
    }
    TimerRegistration* reg = nullptr;
    if (timed) {
        reg = prepare_ordinary_deadline_locked(&node, &waiters, deadline);
    }
    if (!waiters.register_wait_locked(node, resume)) {
        if (timed) erase_popped_registration_locked(reg);
        return WaitAdmitDisposition::rejected;
    }
    ++waiting_waitq_count_;
    if (timed) {
        publish_ordinary_deadline_locked(reg, &rwlock_timer_expire_reconcile,
                                         expire_ctx);
    }





    if (node.prev_ == nullptr && active_readers == 0 && !writer_active) {
        if (rwlock_claim_node_woken_locked(waiters, node)) {
            writer_active = true;
            writer_owner = actor;
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
false, deadline_t{},
nullptr) !=
            WaitAdmitDisposition::authorized) {
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



bool Scheduler::rwlock_try_write_admission_locked(WaitQueue& waiters,
                                                  std::size_t& active_readers,
                                                  bool& writer_active,
                                                  ActorId& writer_owner,
                                                  const ActorId& caller)
    SLUICE_REQUIRES(global_mtx_, waiters.mtx()) {

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




    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::write_lock requires a running Fiber");
    Fiber* me = ws->current;
    assert(node.user() == nullptr && "AsyncRwLock::write_lock: node.user() must "
                                     "be nullptr on entry (caller contract)");
    RwWaitCtx ctx{RwWaitCtx::Mode::write, ActorId::fiber(me)};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (rwlock_write_admit_locked(waiters, active_readers, writer_active,
                                      writer_owner, node, WaitResume::fiber(me),
                                      ActorId::fiber(me), false,
                                      deadline_t{},
nullptr) !=
            WaitAdmitDisposition::authorized) {
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
                                   ActorId& writer_owner) {
    LockGuard lk(global_mtx_);
    assert(active_readers > 0 && "AsyncRwLock::unlock_read without held share "
                                 "(caller contract violation)");
    --active_readers;
    if (active_readers > 0) return;

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




void Scheduler::rwlock_unlock_write_core_locked(WaitQueue& waiters,
                                                std::size_t& active_readers,
                                                bool& writer_active,
                                                ActorId& writer_owner,
                                                const ActorId& caller)
    SLUICE_REQUIRES(global_mtx_) {
    if (!writer_active) {


        detail::async_rwlock_unlock_write_inactive_fail_fast();
    }
    if (writer_owner != caller) {



        detail::async_rwlock_unlock_write_not_owner_fail_fast();
    }
    writer_active = false;
    writer_owner = ActorId::none();

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

        if (!cancel_primitive_wait_locked(waiters, node)) return false;
        if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    }



    rwlock_grant_from_head_locked(waiters, active_readers, writer_active,
                                  writer_owner);



    publish_wait_winner_locked(node);
    return true;
}

bool Scheduler::rwlock_expire_wait(WaitQueue& waiters,
                                   std::size_t& active_readers,
                                   bool& writer_active,
                                   ActorId& writer_owner,
                                   WaitNode& node) {


    {
        LockGuard qlk(waiters.mtx());
        if (!waiters.expire_locked(node)) return false;

        if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    }


    rwlock_grant_from_head_locked(waiters, active_readers, writer_active,
                                  writer_owner);


    publish_wait_winner_locked(node);
    return true;
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
    RwWaitCtx ctx{RwWaitCtx::Mode::read, ActorId::none()};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (rwlock_read_admit_locked(waiters, active_readers, writer_active,
                                     node, WaitResume::fiber(me),
true, deadline,
                                     expire_ctx) !=
            WaitAdmitDisposition::authorized) {
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
                                        ActorId& writer_owner,
                                        WaitNode& node,
                                        deadline_t deadline,
                                        void* expire_ctx) {




    WorkerState* ws = g_worker;
    assert(ws != nullptr && "AsyncRwLock::write_lock_until requires a running Fiber");
    Fiber* me = ws->current;
    assert(node.user() == nullptr && "AsyncRwLock::write_lock_until: node.user() "
                                     "must be nullptr on entry");
    RwWaitCtx ctx{RwWaitCtx::Mode::write, ActorId::fiber(me)};
    node.set_user(&ctx);
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(waiters.mtx());
        if (rwlock_write_admit_locked(waiters, active_readers, writer_active,
                                      writer_owner, node, WaitResume::fiber(me),
                                      ActorId::fiber(me), true,
                                      deadline, expire_ctx) !=
            WaitAdmitDisposition::authorized) {
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










    (void)owner_ctx;
    (void)timer_won;
}



}
