



















#pragma once

#include <sluice/async/scheduler.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include "queue_detail.hpp"

namespace sluice::async {





class AsyncRwLock;
struct RwWaitCtx;
}

namespace sluice::async {



struct Scheduler::AsyncTestAccess {






    static WorkerState* tls_worker_probe() noexcept { return current_worker(); }





    static void dump_park_forensics(Scheduler& s, const char* tag) {
        s.dump_park_forensics_for_test(tag);
    }
    static std::size_t park_ledger_count(const Scheduler& s) {
        std::lock_guard<std::mutex> lk(s.park_ledger_mtx_);
        return s.park_ledger_count_;
    }



    static void set_park_forensics(Scheduler& s, bool enabled) noexcept {
        s.park_forensics_enabled_.store(enabled, std::memory_order_release);
    }




    static BackendWaitToken backend_wait_token(const Scheduler& s) noexcept {
        return s.ctx_.backend_wait_token_for_test();
    }












    static bool worker_loop_exited(const Scheduler& s,
                                   unsigned worker_id) noexcept {
        LockGuard lk(s.global_mtx_);
        return worker_id < s.workers_.size() &&
               s.workers_[worker_id]->loop_exited.load(
                   std::memory_order_acquire);
    }
    static std::size_t worker_local_runnable(const Scheduler& s,
                                             unsigned worker_id) {
        LockGuard lk(s.global_mtx_);
        if (worker_id >= s.workers_.size()) return 0;
        std::lock_guard<std::mutex> ilk(s.workers_[worker_id]->inbox_mtx);
        return s.workers_[worker_id]->local_runnable.size();
    }
    static WorkerState::ParkDomain worker_park_domain(
        const Scheduler& s, unsigned worker_id) noexcept {
        LockGuard lk(s.global_mtx_);
        if (worker_id >= s.workers_.size())
            return WorkerState::ParkDomain::None;
        return s.workers_[worker_id]->park_domain.load(
            std::memory_order_acquire);
    }









    static WorkerState::ParkDomain worker_park_domain_try(
        const Scheduler& s, unsigned worker_id,
        bool& available) noexcept SLUICE_NO_THREAD_SAFETY_ANALYSIS {
        if (!s.global_mtx_.try_lock()) {
            available = false;
            return WorkerState::ParkDomain::None;
        }
        available = true;
        struct Unlock {
            Mutex& mu;
            ~Unlock() noexcept { mu.unlock(); }
        } unlock{s.global_mtx_};
        if (worker_id >= s.workers_.size())
            return WorkerState::ParkDomain::None;
        return s.workers_[worker_id]->park_domain.load(
            std::memory_order_acquire);
    }
    static int worker_last_classify(const Scheduler& s,
                                    unsigned worker_id) noexcept {
        LockGuard lk(s.global_mtx_);
        return worker_id < s.workers_.size()
                   ? s.workers_[worker_id]->last_classify.load(
                         std::memory_order_acquire)
                   : -1;
    }






    static std::size_t ready_sink_deliveries(const Scheduler& s) noexcept {
        return s.ready_sink_.deliveries();
    }
    static std::size_t ready_sink_routed(const Scheduler& s) noexcept {
        return s.ready_sink_.routed();
    }
    static std::size_t ready_sink_stale_dropped(const Scheduler& s) noexcept {
        return s.ready_sink_.stale_dropped();
    }
    static std::size_t ready_sink_cancel_lost(const Scheduler& s) noexcept {
        return s.ready_sink_.cancel_lost();
    }
    static std::size_t legacy_completion_wait_count(Scheduler& s) {
        LockGuard lk(s.global_mtx_);
        return s.waiting_size_.size() + s.waiting_void_.size();
    }
    static std::size_t wait_registry_live_count(Scheduler& s) {
        LockGuard rlk(s.wait_registry_mtx_);
        return s.wait_record_live_count_;
    }
    static detail::SynchronousReadySink& ready_sink(Scheduler& s) noexcept {
        return s.ready_sink_;
    }
    static std::uint64_t scheduler_identity(const Scheduler& s) noexcept {
        return s.scheduler_identity_;
    }


    static std::size_t configured_wait_capacity(const Scheduler& s) {
        LockGuard rlk(s.wait_registry_mtx_);
        return s.wait_capacity_;
    }
    static std::size_t wait_record_storage_size(const Scheduler& s) {
        LockGuard rlk(s.wait_registry_mtx_);
        return s.wait_records_.size();
    }


    static void enable_test_clock(Scheduler& s) noexcept {
        s.test_clock_mode_.store(true, std::memory_order::release);
        s.clock_.store(0, std::memory_order::release);
    }
    static void set_clock(Scheduler& s, deadline_t t) noexcept {
        s.clock_.store(t, std::memory_order::release);
    }
    static deadline_t clock_now(const Scheduler& s) noexcept {
        return s.clock_.load(std::memory_order::acquire);
    }




    static bool worker_topology_lock_available(Scheduler& s) noexcept {
        if (!s.global_mtx_.try_lock()) {
            return false;
        }
        s.global_mtx_.unlock();
        return true;
    }







    static unsigned elected_participant_id(Scheduler& s) {
        s.global_mtx_.lock();
        unsigned id = s.admission_owner_;
        s.global_mtx_.unlock();
        return id;
    }




    static TimerRegistration* register_test_deadline(Scheduler& s,
                                                     WaitNode* node,
                                                     WaitQueue* q,
                                                     deadline_t deadline);




    static std::size_t timer_pool_size(const Scheduler& s) noexcept;
    static std::size_t deadline_heap_size(const Scheduler& s) noexcept;
    static std::size_t deadline_heap_capacity(const Scheduler& s) noexcept;




    static std::size_t active_deadline_count(const Scheduler& s) noexcept;
    static std::size_t timer_pool_count_in_state(const Scheduler& s,
                                                 TimerRegistration::State st) noexcept;
    static bool earliest_active_deadline(Scheduler& s, deadline_t& out);





    static void select_event_link(Scheduler& s, Event& event,
                                  detail::SelectArmSlot& arm) {
        LockGuard lk(s.global_mtx_);
        s.select_event_link_locked(event, arm);
    }



    static void select_event_unlink(Scheduler& s, Event& event,
                                    detail::SelectArmSlot& arm) {
        LockGuard lk(s.global_mtx_);
        s.select_event_unlink_locked(event, arm);
    }



    static std::size_t select_event_scan(Scheduler& s, Event& event) {
        LockGuard lk(s.global_mtx_);
        return s.select_event_scan_locked(event);
    }



    static void set_arm_state(Scheduler& s, detail::SelectArmSlot& arm,
                              detail::ArmState st);











    static void select_event_forge_stale_home(Scheduler& s, Event& event,
                                              detail::SelectArmSlot& arm);






    static void select_event_forge_wrong_home(Scheduler& s, Event& event_a,
                                              Event& event_b,
                                              detail::SelectArmSlot& arm);












    static detail::SelectTimerRegistration* register_synthetic_select_timer(
        Scheduler& s, detail::SelectArmSlot* arm, deadline_t deadline) {
        std::list<detail::SelectTimerRegistration> tmp;
        tmp.emplace_back(arm, &s, deadline);


        LockGuard lk(s.global_mtx_);
        s.deadline_heap_.reserve(s.deadline_heap_.size() + 1);
        return s.select_timer_splice_one_locked(tmp, tmp.begin());
    }




    static bool retire_synthetic_select_timer(
        Scheduler& s, detail::SelectTimerRegistration& reg) {
        LockGuard lk(s.global_mtx_);
        return s.select_timer_retire_locked(reg);
    }
    static bool consume_synthetic_select_timer(
        Scheduler& s, detail::SelectTimerRegistration& reg) {
        LockGuard lk(s.global_mtx_);
        return s.select_timer_consume_locked(reg);
    }










    static detail::SelectTimerRegistration* splice_one_for_test(
        Scheduler& s,
        std::list<detail::SelectTimerRegistration>& tmp_pool,
        std::list<detail::SelectTimerRegistration>::iterator it) {
        LockGuard lk(s.global_mtx_);
        s.deadline_heap_.reserve(s.deadline_heap_.size() + 1);
        return s.select_timer_splice_one_locked(tmp_pool, it);
    }











    static bool detached_try_claim_expiry(
        detail::SelectTimerRegistration& reg) noexcept {
        assert(reg.scheduler() == nullptr &&
               "detached CAS accessor requires a never-registered registration");
        if (reg.scheduler() != nullptr) {
            detail::select_invariant_fail_fast();
        }
        return reg.try_claim_expiry();
    }
    static bool detached_retire(
        detail::SelectTimerRegistration& reg) noexcept {
        assert(reg.scheduler() == nullptr &&
               "detached CAS accessor requires a never-registered registration");
        if (reg.scheduler() != nullptr) {
            detail::select_invariant_fail_fast();
        }
        return reg.retire();
    }















    static bool detached_claim_winner(detail::SelectGroup& group,
                                     std::uint32_t arm_index) noexcept;












    static bool select_process_group(Scheduler& s,
                                     detail::SelectGroup& group,
                                     std::uint32_t candidate_index);





    static bool select_all_authority_closed(const Scheduler& s,
                                            const detail::SelectGroup& group);









    static void assert_select_all_authority_closed(
        const Scheduler& s, const detail::SelectGroup& group);


    static void advance_clock(Scheduler& s, deadline_t t);


    static std::size_t select_timer_pool_size(
        const Scheduler& s) noexcept;
    static std::size_t select_timer_count_in_state(
        const Scheduler& s,
        detail::SelectTimerRegistration::State st) noexcept;

    static std::array<std::size_t, 2> tagged_heap_counts_by_kind(
        const Scheduler& s) noexcept;






    static bool deadline_heap_has_select_target(
        const Scheduler& s,
        const detail::SelectTimerRegistration* target) noexcept;




    static std::size_t select_timer_arm_load_count(
        const Scheduler& s) noexcept {
        return s.select_timer_arm_load_count_;
    }
    static void reset_select_timer_arm_load_count(Scheduler& s) noexcept {
        s.select_timer_arm_load_count_ = 0;
    }




    static std::size_t waiting_select_count(const Scheduler& s) noexcept {
        LockGuard lk(s.global_mtx_);
        return s.waiting_select_count_;
    }







    static void inc_waiting_select_for_test(Scheduler& s) noexcept {
        LockGuard lk(s.global_mtx_);
        ++s.waiting_select_count_;
    }




    static SelectResult group_result(const Scheduler& s,
                                     const detail::SelectGroup& group);








    static void select_publish(Scheduler& s, detail::SelectGroup& group) {
        LockGuard lk(s.global_mtx_);
        s.select_publish_locked(group);
    }






    static void select_begin_rollback(Scheduler& s,
                                      detail::SelectGroup& group) {
        LockGuard lk(s.global_mtx_);
        s.select_begin_rollback_locked(group);
    }
    static void select_rollback_arm(Scheduler& s, detail::SelectGroup& group,
                                    detail::SelectArmSlot& arm) {
        LockGuard lk(s.global_mtx_);
        s.select_rollback_arm_locked(group, arm);
    }
    static void select_finish_rollback(Scheduler& s,
                                       detail::SelectGroup& group,
                                       detail::SelectArmSlot* arms,
                                       std::size_t arm_count,
                                       std::size_t registered_count) {
        LockGuard lk(s.global_mtx_);
        s.select_finish_rollback_locked(group, arms, arm_count,
                                        registered_count);
    }
    static void select_rollback_registration(
        Scheduler& s, detail::SelectGroup& group,
        detail::SelectArmSlot* arms, std::size_t arm_count,
        std::size_t registered_count) {
        LockGuard lk(s.global_mtx_);
        s.select_rollback_registration_locked(group, arms, arm_count,
                                              registered_count);
    }




    static bool select_resolve_event(Scheduler& s, Event& event) {
        LockGuard lk(s.global_mtx_);
        return s.select_resolve_event_locked(event);
    }




    static bool select_resolve_timer(Scheduler& s,
                                     detail::SelectTimerRegistration& reg) {
        LockGuard lk(s.global_mtx_);
        return s.select_resolve_timer_locked(reg);
    }






    static void drain_select_pool(Scheduler& s) {


        {
            LockGuard lk(s.global_mtx_);
            for (auto& reg : s.select_timer_pool_) {
                if (reg.is_active()) s.select_timer_retire_locked(reg);
            }
        }


        s.advance_clock(static_cast<deadline_t>(1) << 62);
    }


















    static void rwlock_death_forge_invalid_head_mode(Scheduler& s,
                                                     AsyncRwLock& rw);


    static void rwlock_death_forge_null_head_user(Scheduler& s,
                                                  AsyncRwLock& rw);



    static void rwlock_death_forge_invalid_batch_member(Scheduler& s,
                                                        AsyncRwLock& rw);







    struct RunnablePublicationSnapshot {
        unsigned fiber_owner_worker_id{static_cast<unsigned>(-1)};
        bool owner_queue_contains_fiber{false};
        bool resolver_queue_contains_fiber{false};
        bool pending_spawn_contains_fiber{false};
        bool owner_suspend_switch_pending{false};
        FiberState fiber_state{FiberState::created};
    };

    static RunnablePublicationSnapshot capture_runnable_publication(
        Scheduler& s, Fiber& fiber,
        unsigned owner_worker_id, unsigned resolver_worker_id) {
        RunnablePublicationSnapshot snap;
        LockGuard lk(s.global_mtx_);

        auto it = s.fiber_owner_.find(&fiber);
        if (it != s.fiber_owner_.end() && it->second != nullptr) {
            snap.fiber_owner_worker_id = it->second->id;
        }

        snap.fiber_state = fiber.state();

        if (owner_worker_id < s.workers_.size()) {
            snap.owner_suspend_switch_pending =
                s.workers_[owner_worker_id]->suspend_switch_pending.load(
                    std::memory_order_acquire);
        }

        if (owner_worker_id < s.workers_.size()) {
            auto& w = *s.workers_[owner_worker_id];
            std::lock_guard<std::mutex> wlk(w.inbox_mtx);
            for (auto* f : w.local_runnable) {
                if (f == &fiber) { snap.owner_queue_contains_fiber = true; break; }
            }
        }

        if (resolver_worker_id < s.workers_.size() &&
            resolver_worker_id != owner_worker_id) {
            auto& w = *s.workers_[resolver_worker_id];
            std::lock_guard<std::mutex> wlk(w.inbox_mtx);
            for (auto* f : w.local_runnable) {
                if (f == &fiber) { snap.resolver_queue_contains_fiber = true; break; }
            }
        }

        for (auto* f : s.pending_spawn_) {
            if (f == &fiber) { snap.pending_spawn_contains_fiber = true; break; }
        }
        return snap;
    }






    static void force_next_init_fiber_fail(Scheduler& s) noexcept {
        s.force_init_fiber_fail_.store(true, std::memory_order::release);
    }
    static bool init_fiber_fail_armed(const Scheduler& s) noexcept {
        return s.force_init_fiber_fail_.load(std::memory_order::acquire);
    }







    static void set_evented_admission_override(bool supported) noexcept;
    static bool evented_admission_override() noexcept;
















    struct FeDeferredRecord {
        enum class State : std::uint8_t { unarmed = 0, armed = 1, consumed = 2 };
        std::atomic<State> state{State::unarmed};
        void* handle_address = nullptr;





        void arm(std::memory_order order = std::memory_order_release) noexcept {
            state.store(State::armed, order);
        }




        bool try_consume() noexcept {
            State expected = State::armed;
            return state.compare_exchange_strong(expected, State::consumed,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
        }
    };















    static bool event_wait_deferred_for_test(Scheduler& s, Event& event,
                                             WaitNode& node,
                                             FeDeferredRecord& record);



    static bool event_wait_deferred_deadline_for_test(Scheduler& s,
                                                      Event& event,
                                                      WaitNode& node,
                                                      FeDeferredRecord& record,
                                                      deadline_t deadline);




    static bool event_cancel_deferred_for_test(Scheduler& s, Event& event,
                                               WaitNode& node);


    static std::size_t take_deferred_for_test(Scheduler& s, void** out,
                                              std::size_t cap) {
        return s.take_deferred_publications(out, cap);
    }

    static std::size_t deferred_depth_for_test(Scheduler& s) {
        LockGuard lk(s.global_mtx_);
        return s.deferred_publications_.size();
    }








    static bool try_lock_global_for_test(Scheduler& s) noexcept
        SLUICE_NO_THREAD_SAFETY_ANALYSIS {
        return s.global_mtx_.try_lock();
    }
    static void unlock_global_for_test(Scheduler& s) noexcept
        SLUICE_NO_THREAD_SAFETY_ANALYSIS {
        s.global_mtx_.unlock();
    }





















    static bool queue_push_deferred_for_test(Scheduler& s,
                                             detail::QueuePort& port,
                                             detail::QueueItemLease& lease,
                                             WaitNode& node, QueueWaitCtx& ctx,
                                             FeDeferredRecord& record);
    static bool queue_push_deferred_until_for_test(
        Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& lease,
        WaitNode& node, QueueWaitCtx& ctx, FeDeferredRecord& record,
        deadline_t deadline);
    static bool queue_pop_deferred_for_test(Scheduler& s,
                                            detail::QueuePort& port,
                                            detail::QueueItemLease& out,
                                            WaitNode& node, QueueWaitCtx& ctx,
                                            FeDeferredRecord& record);
    static bool queue_pop_deferred_until_for_test(
        Scheduler& s, detail::QueuePort& port, detail::QueueItemLease& out,
        WaitNode& node, QueueWaitCtx& ctx, FeDeferredRecord& record,
        deadline_t deadline);



    static bool queue_cancel_deferred_for_test(Scheduler& s,
                                               detail::QueuePort& port,
                                               detail::QueueRole role,
                                               WaitNode& node) {
        return s.queue_cancel(port, role, node);
    }



    static bool queue_lease_empty_for_test(const detail::QueueItemLease& l) {
        return l.control_ == nullptr;
    }





    static detail::QueueItemLease queue_make_empty_lease_for_test() {
        return detail::QueueItemLease{};
    }















    static bool queue_push_core_(Scheduler& s, detail::QueuePort& port,
                                 detail::QueueItemLease& lease, WaitNode& node,
                                 FeDeferredRecord& record, bool timed,
                                 deadline_t deadline);
    static bool queue_pop_core_(Scheduler& s, detail::QueuePort& port,
                                detail::QueueItemLease& out, WaitNode& node,
                                FeDeferredRecord& record, bool timed,
                                deadline_t deadline);



    static void queue_release_deferred_pin_for_test(detail::QueuePort& port);



    static std::size_t queue_active_port_calls_for_test(
        const detail::QueuePort& port) {
        LockGuard glk(port.scheduler_.global_mtx_);
        LockGuard slk(port.state_mtx_);
        return port.active_port_calls_;
    }


    static std::size_t queue_active_wait_associations_for_test(
        const detail::QueuePort& port) {
        LockGuard glk(port.scheduler_.global_mtx_);
        LockGuard slk(port.state_mtx_);
        return port.active_wait_associations_;
    }
    static std::size_t queue_active_queue_timers_for_test(
        const detail::QueuePort& port) {
        LockGuard glk(port.scheduler_.global_mtx_);
        LockGuard slk(port.state_mtx_);
        return port.active_queue_timers_;
    }
    static std::size_t queue_granted_not_resumed_for_test(
        const detail::QueuePort& port) {
        LockGuard glk(port.scheduler_.global_mtx_);
        LockGuard slk(port.state_mtx_);
        return port.granted_not_resumed_;
    }















    static bool rwlock_read_deferred_for_test(Scheduler& s, AsyncRwLock& lock,
                                              WaitNode& node, void* actor_token,
                                              RwWaitCtx& ctx,
                                              FeDeferredRecord& record);
    static bool rwlock_read_deferred_until_for_test(
        Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
        RwWaitCtx& ctx, FeDeferredRecord& record, deadline_t deadline);
    static bool rwlock_write_deferred_for_test(Scheduler& s, AsyncRwLock& lock,
                                               WaitNode& node,
                                               void* actor_token,
                                               RwWaitCtx& ctx,
                                               FeDeferredRecord& record);
    static bool rwlock_write_deferred_until_for_test(
        Scheduler& s, AsyncRwLock& lock, WaitNode& node, void* actor_token,
        RwWaitCtx& ctx, FeDeferredRecord& record, deadline_t deadline);




    static bool rwlock_try_write_deferred_for_test(Scheduler& s,
                                                   AsyncRwLock& lock,
                                                   void* actor_token);


    static void rwlock_unlock_write_deferred_for_test(Scheduler& s,
                                                      AsyncRwLock& lock,
                                                      void* actor_token);


    static bool rwlock_read_core_(Scheduler& s, AsyncRwLock& lock,
                                  WaitNode& node, void* actor_token,
                                  RwWaitCtx& ctx, FeDeferredRecord& record,
                                  bool timed, deadline_t deadline);
    static bool rwlock_write_core_(Scheduler& s, AsyncRwLock& lock,
                                   WaitNode& node, void* actor_token,
                                   RwWaitCtx& ctx, FeDeferredRecord& record,
                                   bool timed, deadline_t deadline);


    static void rwlock_unlock_read_for_test(Scheduler& s, AsyncRwLock& lock);


    static bool rwlock_try_read_for_test(Scheduler& s, AsyncRwLock& lock);


    static bool rwlock_cancel_deferred_for_test(Scheduler& s,
                                                AsyncRwLock& lock,
                                                WaitNode& node);




    static bool rwlock_writer_active_for_test(AsyncRwLock& lock);
    static bool rwlock_owned_by_for_test(AsyncRwLock& lock,
                                         const void* actor_token);





























    static bool condition_wait_deferred_for_test(Scheduler& s,
                                                 WaitQueue& cond_waiters,
                                                 WaitNode& cond_node,
                                                 WaitQueue& mutex_waiters,
                                                 Fiber*& owner,
                                                 FeDeferredRecord& record,
                                                 bool& released);
    static bool condition_wait_deferred_until_for_test(
        Scheduler& s, WaitQueue& cond_waiters, WaitNode& cond_node,
        WaitQueue& mutex_waiters, Fiber*& owner, deadline_t deadline,
        FeDeferredRecord& record, bool& released);





    static void condition_notify_one_for_test(Scheduler& s,
                                              WaitQueue& cond_waiters) {
        s.condition_notify_one(cond_waiters);
    }
    static std::size_t condition_notify_all_for_test(Scheduler& s,
                                                     WaitQueue& cond_waiters) {
        return s.condition_notify_all(cond_waiters);
    }
    static bool condition_cancel_for_test(Scheduler& s,
                                          WaitQueue& cond_waiters,
                                          WaitNode& cond_node) {
        return s.condition_cancel_wait(cond_waiters, cond_node);
    }




    static bool condition_wait_deferred_core_(
        Scheduler& s, WaitQueue& cond_waiters, WaitNode& cond_node,
        WaitQueue& mutex_waiters, Fiber*& owner, FeDeferredRecord& record,
        bool timed, deadline_t deadline, bool& released);
};

}

#endif
