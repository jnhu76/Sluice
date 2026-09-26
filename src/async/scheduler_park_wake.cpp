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


namespace sluice::async {

bool SchedulerWakeHandle::notify() noexcept {
    if (!control_)
        return false;

    LockGuard lk(control_->mtx);
    if (!control_->alive || control_->scheduler == nullptr) {
        return false;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (control_->lifetime_seam_armed) {
        std::unique_lock<std::mutex> slk(control_->lifetime_seam_mtx);
        control_->lifetime_seam_paused = true;
        control_->lifetime_seam_cv.notify_all();
        control_->lifetime_seam_cv.wait(slk, [this] { return !control_->lifetime_seam_armed; });
        control_->lifetime_seam_paused = false;

        if (!control_->alive || control_->scheduler == nullptr) {
            return false;
        }
    }
#endif
    control_->scheduler->notify_external_wake();
    return true;
}

bool SchedulerWakeHandle::bound() const noexcept {
    if (!control_)
        return false;
    LockGuard lk(control_->mtx);
    return control_->alive && control_->scheduler != nullptr;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
void SchedulerWakeHandle::lifetime_seam_arm() noexcept {
    if (!control_)
        return;
    std::lock_guard<std::mutex> lk(control_->lifetime_seam_mtx);
    control_->lifetime_seam_armed = true;
}

void SchedulerWakeHandle::lifetime_seam_wait_paused() noexcept {
    if (!control_)
        return;
    std::unique_lock<std::mutex> lk(control_->lifetime_seam_mtx);
    control_->lifetime_seam_cv.wait(lk, [this] { return control_->lifetime_seam_paused; });
}

bool SchedulerWakeHandle::lifetime_seam_is_paused() const noexcept {
    if (!control_)
        return false;
    std::lock_guard<std::mutex> lk(control_->lifetime_seam_mtx);
    return control_->lifetime_seam_paused;
}

void SchedulerWakeHandle::lifetime_seam_release() noexcept {
    if (!control_)
        return;
    std::lock_guard<std::mutex> lk(control_->lifetime_seam_mtx);
    control_->lifetime_seam_armed = false;
    control_->lifetime_seam_cv.notify_all();
}
#endif

SchedulerWakeHandle Scheduler::make_wake_handle() noexcept {
    {
        LockGuard lk(wake_control_->mtx);
        wake_control_->scheduler = this;
        wake_control_->alive = true;
    }
    return SchedulerWakeHandle{wake_control_};
}

void Scheduler::notify_external_wake() noexcept {
    signal_wake_locked();
}

void Scheduler::signal_wake_locked() {
    {
        LockGuard lk(wake_mtx_);
        ++wake_epoch_;
    }
    wake_cv_.notify_all();

    if (backend_wait_active_.load(std::memory_order_acquire)) {
        ctx_.interrupt_backend_waiters();
    }
}

void Scheduler::park_on_wake_source(WorkerState* ws, bool bounded_backend_observation)
    SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    ws->park_domain = WorkerState::ParkDomain::Scheduler;


#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    ParkLedgerRecord forensics_rec{};
    if (park_forensics_enabled_.load(std::memory_order_acquire)) {
        {
            LockGuard glk(global_mtx_);
            forensics_rec.waiting_registered = waiting_size_.size() + waiting_void_.size() +
                                               waiting_ready_.size() +
                                               static_cast<std::size_t>(waiting_waitq_count_) +
                                               static_cast<std::size_t>(waiting_select_count_);
            forensics_rec.external_wake_possible = external_wake_possible_locked();
        }
        const BackendWaitToken forensics_tok = ctx_.backend_wait_token_for_test();
        forensics_rec.ready_generation = forensics_tok.progress_generation;
        forensics_rec.control_generation = forensics_tok.control_generation;
        forensics_rec.backend_outstanding = ctx_.outstanding();
    }
    forensics_rec.worker_id = ws->id;
    forensics_rec.idle_workers = idle_workers_.load(std::memory_order_acquire);
    forensics_rec.backend_wait_active = backend_wait_active_.load(std::memory_order_acquire);
    forensics_rec.ready_flag_bounded = bounded_backend_observation;
    forensics_rec.global_terminate = global_terminate_.load(std::memory_order_acquire);

    forensics_rec.last_classify = ws->last_classify.load(std::memory_order_relaxed);
    forensics_rec.classify_seq = ws->classify_seq.load(std::memory_order_relaxed);
#endif

    {
        LockGuard glk(global_mtx_);
        const unsigned own_dance = ws->idle_dance_contributed_.load(std::memory_order_acquire);

        if (unguarded_progress_pending_locked() ||
            idle_workers_.load(std::memory_order_acquire) > own_dance ||
            (own_dance != 0 &&
             dance_epoch_.load(std::memory_order_acquire) !=
                 ws->dance_epoch_at_contribution_.load(std::memory_order_acquire))) {
            signal_wake_locked();
            ws->park_domain = WorkerState::ParkDomain::None;
            return;
        }
        LockGuard wlk(wake_mtx_);
        ws->observed_epoch = wake_epoch_;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        forensics_rec.epoch_at_commit = wake_epoch_;
        if (park_forensics_enabled_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> plk(park_ledger_mtx_);

            forensics_rec.park_seq = park_ledger_total_ + 1;
            ++park_ledger_total_;
            park_ledger_[park_ledger_next_] = forensics_rec;
            park_ledger_next_ = (park_ledger_next_ + 1) % kParkLedgerCapacity;
            if (park_ledger_count_ < kParkLedgerCapacity)
                ++park_ledger_count_;
        }
#endif
    }
    std::unique_lock<Mutex> lk(wake_mtx_);

    auto park_pred = [&]() SLUICE_NO_THREAD_SAFETY_ANALYSIS {
        if (wake_epoch_ != ws->observed_epoch ||
            global_terminate_.load(std::memory_order_acquire)) {
            return true;
        }
        std::lock_guard<std::mutex> ilk(ws->inbox_mtx);
        return !ws->local_runnable.empty();
    };

    static constexpr auto kParkBackstop = std::chrono::milliseconds(2);
    static constexpr auto kTestParkPoll = std::chrono::milliseconds(1);
    deadline_t earliest = earliest_active_deadline_.load(std::memory_order::acquire);

    if (earliest == kNoDeadline && !bounded_backend_observation) {
        wake_cv_.wait(lk, park_pred);
        ws->park_domain = WorkerState::ParkDomain::None;
        return;
    }
    deadline_t now_ticks = clock_now_unlocked();
    auto wake_deadline = std::chrono::steady_clock::time_point::max();
    if (earliest != kNoDeadline) {
        if (earliest <= now_ticks) {
            wake_deadline = std::chrono::steady_clock::now();
        } else {
            deadline_t remaining = earliest - now_ticks;
            if (test_clock_mode_.load(std::memory_order_acquire)) {
                wake_deadline = std::chrono::steady_clock::now() + kTestParkPoll;
            } else if (bounded_backend_observation) {
                auto delay = std::min(std::chrono::milliseconds(remaining), kParkBackstop);
                wake_deadline = std::chrono::steady_clock::now() + delay;
            } else {
                wake_deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(remaining);
            }
        }
    } else {
        wake_deadline = std::chrono::steady_clock::now() + kParkBackstop;
    }
    wake_cv_.wait_until(lk, wake_deadline, park_pred);
    ws->park_domain = WorkerState::ParkDomain::None;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
void Scheduler::dump_park_forensics_for_test(const char* tag) {
    std::fprintf(stderr, "=== park-forensics[%s] begin ===\n", tag);

    std::vector<WorkerState*> worker_ptrs;
    const char* admission = "none";
    std::size_t w_size = 0, w_void = 0, w_ready = 0, w_waitq = 0, w_select = 0;
    std::size_t pending_spawn = 0;

    unsigned active_workers = 0, live_loop = 0, idle_now = 0;
    std::size_t running_fibers = 0;
    bool global_term = false, in_run = false;
    {
        LockGuard glk(global_mtx_);
        worker_ptrs.reserve(workers_.size());
        for (const std::unique_ptr<WorkerState>& w : workers_) {
            worker_ptrs.push_back(w.get());
        }
        if (admission_ == AdmissionState::candidate) {
            admission = "candidate";
        } else if (admission_ == AdmissionState::committed) {
            admission = "committed";
        }
        w_size = waiting_size_.size();
        w_void = waiting_void_.size();
        w_ready = waiting_ready_.size();
        w_waitq = waiting_waitq_count_;
        w_select = waiting_select_count_;
        pending_spawn = pending_spawn_.size();
        active_workers = active_worker_count_.load(std::memory_order_acquire);
        live_loop = static_cast<unsigned>(live_loop_workers_);
        idle_now = idle_workers_.load(std::memory_order_acquire);
        running_fibers = running_fiber_count_.load(std::memory_order_acquire);
        global_term = global_terminate_.load(std::memory_order_acquire);
        in_run = in_coordinated_run_;
    }

    std::size_t wait_live = 0, wait_delivered = 0;
    {
        LockGuard rlk(wait_registry_mtx_);
        wait_live = wait_record_live_count_;
        for (WaitRecord* r = wait_delivered_head_; r != nullptr; r = r->next_delivered) {
            ++wait_delivered;
        }
    }
    std::fprintf(stderr,
                 "[park-forensics] run: active_workers=%u live_loop=%u "
                 "idle=%u running_fibers=%zu terminate=%d in_coordinated=%d "
                 "pending_spawn=%zu wait_live=%zu wait_delivered_pending=%zu\n",
                 active_workers, live_loop, idle_now, running_fibers, global_term ? 1 : 0,
                 in_run ? 1 : 0, pending_spawn, wait_live, wait_delivered);
    std::fflush(stderr);

    std::uint64_t wake_epoch_now = 0;
    {
        LockGuard wlk(wake_mtx_);
        wake_epoch_now = wake_epoch_;
    }

    const BackendWaitToken tok = ctx_.backend_wait_token_for_test();
    const std::size_t outstanding = ctx_.outstanding();

    std::fprintf(stderr,
                 "[park-forensics] wake_epoch=%llu token=(ready=%llu,ctrl=%llu) "
                 "outstanding=%zu admission=%s idle_workers=%u terminate=%d "
                 "backend_wait_active=%d\n",
                 static_cast<unsigned long long>(wake_epoch_now),
                 static_cast<unsigned long long>(tok.progress_generation),
                 static_cast<unsigned long long>(tok.control_generation), outstanding, admission,
                 idle_workers_.load(std::memory_order_acquire),
                 global_terminate_.load(std::memory_order_acquire) ? 1 : 0,
                 backend_wait_active_.load(std::memory_order_acquire) ? 1 : 0);
    std::fprintf(stderr,
                 "[park-forensics] waiting: size=%zu void=%zu ready=%zu waitq=%zu "
                 "select=%zu running_fibers=%ld pending_spawn=%zu\n",
                 w_size, w_void, w_ready, w_waitq, w_select,
                 static_cast<long>(running_fiber_count_.load(std::memory_order_acquire)),
                 pending_spawn);

    {
        LockGuard wlk(wake_mtx_);
        static const char* kExitName[] = {
            "(live)",
            "mw_s1_terminate_observed",
            "mw_s2_no_progress_terminate",
            "e14f1_last_idle_terminate",
            "last_idle_terminate",
            "final_park_terminate",
        };
        for (WorkerState* w : worker_ptrs) {
            const auto domain_raw = w->park_domain.load(std::memory_order_acquire);
            const char* domain = "None";
            if (domain_raw == WorkerState::ParkDomain::Scheduler) {
                domain = "SCHEDULER";
            } else if (domain_raw == WorkerState::ParkDomain::Backend) {
                domain = "Backend";
            }
            std::size_t local_runnable = 0;
            {
                std::lock_guard<std::mutex> ilk(w->inbox_mtx);
                local_runnable = w->local_runnable.size();
            }

            const Fiber* cur = w->current.load(std::memory_order_acquire);
            const char* fiber_state = "-";
            if (cur != nullptr) {
                switch (cur->state()) {
                case FiberState::created:
                    fiber_state = "created";
                    break;
                case FiberState::runnable:
                    fiber_state = "runnable";
                    break;
                case FiberState::running:
                    fiber_state = "running";
                    break;
                case FiberState::waiting:
                    fiber_state = "waiting";
                    break;
                case FiberState::done:
                    fiber_state = "done";
                    break;
                }
            }
            const auto exit_raw = w->loop_exit_reason.load(std::memory_order_acquire);
            const char* exit_name = (exit_raw >= WorkerState::LoopExitReason::live &&
                                     exit_raw <= WorkerState::LoopExitReason::final_park_terminate)
                                        ? kExitName[static_cast<std::size_t>(exit_raw)]
                                        : "(unknown)";
            const int cls = w->last_classify.load(std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[park-forensics] worker id=%u domain=%s "
                         "observed_epoch=%llu local_runnable=%zu "
                         "current_fiber=%p fiber_state=%s exit_reason=%s "
                         "loop_exited=%d last_classify=%d\n",
                         w->id, domain, static_cast<unsigned long long>(w->observed_epoch),
                         local_runnable, static_cast<const void*>(cur), fiber_state, exit_name,
                         w->loop_exited.load(std::memory_order_acquire) ? 1 : 0, cls);
        }
    }

    {
        std::lock_guard<std::mutex> plk(park_ledger_mtx_);
        static const char* kMwName[] = {"mw_s1", "mw_s2", "mw_s3", "quiescent"};
        for (std::size_t i = 0; i < park_ledger_count_; ++i) {
            const std::size_t idx = (park_ledger_count_ < kParkLedgerCapacity)
                                        ? i
                                        : (park_ledger_next_ + i) % kParkLedgerCapacity;
            const ParkLedgerRecord& r = park_ledger_[idx];
            std::fprintf(stderr,
                         "[park-forensics] ledger seq=%llu worker=%u "
                         "epoch_at_commit=%llu ready=%llu ctrl=%llu "
                         "outstanding=%zu waiting=%zu classify=%s "
                         "classify_seq=%llu bounded=%d "
                         "idle=%u term=%d extwake=%d bwait=%d\n",
                         static_cast<unsigned long long>(r.park_seq), r.worker_id,
                         static_cast<unsigned long long>(r.epoch_at_commit),
                         static_cast<unsigned long long>(r.ready_generation),
                         static_cast<unsigned long long>(r.control_generation),
                         r.backend_outstanding, r.waiting_registered,
                         (r.last_classify >= 0 && r.last_classify <= 3) ? kMwName[r.last_classify]
                                                                        : "?",
                         static_cast<unsigned long long>(r.classify_seq),
                         r.ready_flag_bounded ? 1 : 0, r.idle_workers, r.global_terminate ? 1 : 0,
                         r.external_wake_possible ? 1 : 0, r.backend_wait_active ? 1 : 0);
        }
    }
    std::fprintf(stderr, "=== park-forensics[%s] end ===\n", tag);
}
#endif

Scheduler::WaitRecord* Scheduler::acquire_wait_record_locked(Fiber* fiber, WorkerState* owner,
                                                             const void* completion,
                                                             const detail::RequestKey& request_key) {
    LockGuard rlk(wait_registry_mtx_);
    WaitRecord* r = wait_record_free_head_;
    if (r != nullptr) {
        wait_record_free_head_ = r->next_free;
        r->next_free = nullptr;
    } else {
        return nullptr;
    }
    r->state = WaitRecordState::registered;
    r->fiber = fiber;
    r->owner = owner;
    r->completion = completion;
    r->request_key = request_key;
    wait_by_request_slot_[request_key.slot.value] = r;
    ++wait_record_live_count_;
    return r;
}

void Scheduler::retire_wait_record_locked(WaitRecord* record) {
    LockGuard rlk(wait_registry_mtx_);
    if (record->state != WaitRecordState::registered) {
        detail::scheduler_wait_registry_invariant_fail_fast();
    }
    record->state = WaitRecordState::free;
    record->fiber = nullptr;
    record->owner = nullptr;
    record->completion = nullptr;
    wait_by_request_slot_.erase(record->request_key.slot.value);
    record->request_key = {};
    record->next_free = wait_record_free_head_;
    wait_record_free_head_ = record;
    --wait_record_live_count_;
}

std::size_t Scheduler::wait_record_live_count_locked() const {
    LockGuard rlk(wait_registry_mtx_);
    return wait_record_live_count_;
}

Result<void> Scheduler::await_completion_size(Completion<std::size_t>& c) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;

    {
        LockGuard lk(global_mtx_);
        const auto attach = ctx_.attach_observer(c);
        if (!attach.armed()) {
            if (c.ready()) {
                return Result<void>{};
            }
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        WaitRecord* rec = acquire_wait_record_locked(me, ws, &c, attach.key);
        if (rec == nullptr) {
            (void)ctx_.cancel_observer(attach.key);
            return make_unexpected<void>(IoError{IoError::Code::no_space});
        }

        commit_suspend_locked(ws, me);
    }

    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);

    if (me->completion_wait_outcome() == CompletionWaitOutcome::completed)
        return Result<void>{};
    return make_unexpected<void>(IoError{IoError::Code::canceled});
}

Result<void> Scheduler::await_completion_void(Completion<void>& c) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;

    {
        LockGuard lk(global_mtx_);
        const auto attach = ctx_.attach_observer(c);
        if (!attach.armed()) {
            if (c.ready()) {
                return Result<void>{};
            }
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        WaitRecord* rec = acquire_wait_record_locked(me, ws, &c, attach.key);
        if (rec == nullptr) {
            (void)ctx_.cancel_observer(attach.key);
            return make_unexpected<void>(IoError{IoError::Code::no_space});
        }

        commit_suspend_locked(ws, me);
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);

    if (me->completion_wait_outcome() == CompletionWaitOutcome::completed)
        return Result<void>{};
    return make_unexpected<void>(IoError{IoError::Code::canceled});
}

template <class T>
Result<bool> Scheduler::cancel_waiter_impl(Completion<T>& c) {
    LockGuard lk(global_mtx_);
    const auto cancel = ctx_.cancel_observer(c);
    if (!cancel.retired()) {
        return Result<bool>{false};
    }

    Fiber* f = nullptr;
    WorkerState* owner = nullptr;
    {
        LockGuard rlk(wait_registry_mtx_);
        auto it = wait_by_request_slot_.find(cancel.key.slot.value);
        if (it != wait_by_request_slot_.end()) {
            WaitRecord* r = it->second;
            if (r->state == WaitRecordState::registered &&
                r->request_key.generation == cancel.key.generation &&
                r->request_key.context == cancel.key.context) {
                r->state = WaitRecordState::cancelled;
                f = r->fiber;
                owner = r->owner;
                r->state = WaitRecordState::free;
                r->fiber = nullptr;
                r->owner = nullptr;
                r->completion = nullptr;
                r->request_key = {};
                r->next_free = wait_record_free_head_;
                wait_record_free_head_ = r;
                --wait_record_live_count_;
                wait_by_request_slot_.erase(it);
            }
        }
    }
    if (f != nullptr) {
        f->set_completion_wait_outcome(CompletionWaitOutcome::canceled);
        if (f->make_runnable()) {
            route_runnable_locked(f, owner);
        }
        return Result<bool>{true};
    }
    return Result<bool>{false};
}

Result<bool> Scheduler::cancel_waiter(Completion<std::size_t>& c) {
    return cancel_waiter_impl(c);
}

Result<bool> Scheduler::cancel_waiter(Completion<void>& c) {
    return cancel_waiter_impl(c);
}

void Scheduler::await_ready_flag(const std::atomic<bool>& ready) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    if (ready.load(std::memory_order::acquire))
        return;
#if defined(SLUICE_TV1_C001_MUTANT)

    {
        LockGuard lk(global_mtx_);
        waiting_ready_[&ready] = {me, ws};
    }
    if (ready.load(std::memory_order::acquire)) {
        LockGuard lk(global_mtx_);
        waiting_ready_.erase(&ready);
        return;
    }
    me->make_waiting();
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
#else

    {
        LockGuard lk(global_mtx_);
        waiting_ready_[&ready] = {me, ws};
        if (ready.load(std::memory_order::acquire)) {
            waiting_ready_.erase(&ready);
            return;
        }
        commit_suspend_locked(ws, me);
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
#endif
}

void Scheduler::await_wait(WaitQueue& q, WaitNode& node) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;

    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(q.mtx());
        if (!q.register_wait_locked(node, WaitResume::fiber(me))) {
            return;
        }
        ++waiting_waitq_count_;

        if (node.is_terminal()) {
            q.unlink_locked(node);
            --waiting_waitq_count_;
            return;
        }
        commit_suspend_locked(ws, me);
    }
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

WaitNode* Scheduler::wake_wait_one_locked(WaitQueue& q) {
    LockGuard qlk(q.mtx());
    WaitNode* won = q.wake_one_locked();
    if (won == nullptr)
        return nullptr;
    retire_timer_for_node_locked(*won);
    if (waiting_waitq_count_ > 0)
        --waiting_waitq_count_;

    publish_wait_winner_locked(*won);
    return won;
}

bool Scheduler::wake_wait_one(WaitQueue& q) {
    LockGuard lk(global_mtx_);
    return wake_wait_one_locked(q) != nullptr;
}

bool Scheduler::cancel_primitive_wait_locked(WaitQueue& waiters, WaitNode& node) {
    if (!waiters.contains_locked(node))
        return false;
    if (!waiters.cancel_locked(node))
        return false;
    retire_timer_for_node_locked(node);
    return true;
}

bool Scheduler::cancel_wait(WaitQueue& q, WaitNode& node) {
    LockGuard lk(global_mtx_);
    LockGuard qlk(q.mtx());
    if (!q.cancel_locked(node))
        return false;
    retire_timer_for_node_locked(node);
    if (waiting_waitq_count_ > 0)
        --waiting_waitq_count_;

    const WaitResume& r = node.resume();
    if (r.kind() == WaitResume::Kind::fiber) {
        Fiber* f = r.as_fiber();
        if (f != nullptr) {
            if (publish_waiting_fiber_runnable_locked(f)) {
                return true;
            }
        }
        return false;
    }
    if (r.kind() == WaitResume::Kind::deferred) {
        defer_publication_locked(r.as_deferred());
        return true;
    }
    return false;
}

void Scheduler::attach_ready_wake(const std::atomic<bool>& ready, SchedulerWakeHandle& wh) {
    bool need_signal = false;
    {
        LockGuard lk(global_mtx_);
        auto it = waiting_ready_.find(&ready);
        if (it == waiting_ready_.end()) {
            return;
        }

        if (ready.load(std::memory_order::acquire)) {
            need_signal = true;
        }
    }

    (void)wh;
    if (need_signal) {
        signal_wake_locked();
    }
}

}
