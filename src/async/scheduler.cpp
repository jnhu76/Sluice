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
#include <new>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {

namespace {

void fiber_entry_bridge(fiber_ctx::Switch* resumed_by, void* user_data) {
    (void)resumed_by;
    auto* fiber = static_cast<Fiber*>(user_data);
    if (fiber->entry()) {
        fiber->entry()(*fiber);
    }
    fiber->make_done();

    fiber_ctx::context_switch_final(fiber->ctx, g_worker->sched_ctx);
}

} // namespace

namespace {
std::uint64_t next_scheduler_identity() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return ++counter;
}
} // namespace

Scheduler::Scheduler(AsyncIoContext& ctx, std::size_t wait_capacity)
    : ctx_(ctx), wait_capacity_(wait_capacity == 0 ? 1 : wait_capacity),
      scheduler_identity_(next_scheduler_identity()) {
    detail::require_evented_supported(detail::evented_admission_check());

    wake_control_ = std::make_shared<SchedulerWakeHandle::Control>();

    wait_records_.reserve(wait_capacity_);
    for (std::size_t i = 0; i < wait_capacity_; ++i) {
        auto rec = std::make_unique<WaitRecord>();
        rec->index = static_cast<std::uint32_t>(i);

        wait_records_.push_back(std::move(rec));
    }

    WaitRecord** tail = &wait_record_free_head_;
    for (std::size_t i = 0; i < wait_capacity_; ++i) {
        WaitRecord* r = wait_records_[i].get();
        *tail = r;
        tail = &r->next_free;
    }
    *tail = nullptr;

    ctx_.set_ready_sink(&ready_sink_);
}

Scheduler::~Scheduler() {
    ctx_.set_ready_sink(nullptr);

    if (wake_control_) {
        {
            LockGuard lk(wake_control_->mtx);
            wake_control_->alive = false;
            wake_control_->scheduler = nullptr;
        }
        wake_control_.reset();
    }

    [[maybe_unused]] bool any_active_select = false;
    for (auto& r : select_timer_pool_) {
        if (r.is_active()) {
            any_active_select = true;
            break;
        }
    }
    assert(!any_active_select &&
           "~Scheduler: an ACTIVE SelectTimerRegistration remains (live Select "
           "timer authority not closed — caller contract violation)");
    assert(active_deadline_count_ == 0 &&
           "~Scheduler: active_deadline_count_ != 0 (a timer registration was "
           "not retired/consumed before teardown)");

    assert(waiting_select_count_ == 0 &&
           "~Scheduler: waiting_select_count_ != 0 (a suspended SelectGroup "
           "remains Armed — an Event-only Select with no active Timer escapes "
           "the timer teardown checks; caller contract violation)");

    {
        LockGuard rlk(wait_registry_mtx_);
        assert(wait_record_live_count_ == 0 &&
               "~Scheduler: wait_record_live_count_ != 0 (a registered "
               "Completion waiter was neither delivered nor cancelled — "
               "abandoned wake obligation)");
        if (wait_record_live_count_ != 0) {
            detail::scheduler_wait_registry_nonempty_fail_fast();
        }
    }

    if (!deferred_publications_.empty()) {
        detail::scheduler_deferred_publication_stranded_fail_fast();
    }
    if (any_active_select || active_deadline_count_ != 0 || waiting_select_count_ != 0) {
        detail::select_invariant_fail_fast();
    }
}

bool Scheduler::init_fiber(Fiber& fiber, std::byte* stack_base, std::size_t stack_size) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    if (force_init_fiber_fail_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
#endif
    return fiber_ctx::init_context(fiber.ctx, &fiber_entry_bridge, &fiber, stack_base, stack_size);
}

void Scheduler::spawn(Fiber& fiber) noexcept {
    if (!fiber.make_runnable())
        return;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
                                  sluice_async_test::PhaseTag::worker_topology_reader_attempt);
#endif
    LockGuard lk(global_mtx_);
    const unsigned participant_count = active_worker_count_.load(std::memory_order_acquire);
    if (participant_count != 0 && !global_terminate_.load(std::memory_order_acquire)) {
        unsigned target = next_spawn_worker_++ % participant_count;
        {
            std::lock_guard<std::mutex> wlk(workers_[target]->inbox_mtx);
            workers_[target]->local_runnable.push_back(&fiber);

            fiber_owner_[&fiber] = workers_[target].get();
        }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        sluice_async_test::set_trace_wake_cause(*this, sluice_async_test::WakeCause::runnable_route,
                                                static_cast<unsigned>(-1));
#endif
        signal_wake_locked();
    } else {
        pending_spawn_.push_back(&fiber);
    }
}

void Scheduler::spawn_on(Fiber& fiber, unsigned worker_id) noexcept {
    if (!fiber.make_runnable())
        return;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
                                  sluice_async_test::PhaseTag::worker_topology_reader_attempt);
#endif
    LockGuard lk(global_mtx_);
    const unsigned participant_count = active_worker_count_.load(std::memory_order_acquire);
    if (participant_count == 0 || global_terminate_.load(std::memory_order_acquire) ||
        worker_id >= participant_count) {
        pending_spawn_.push_back(&fiber);
        return;
    }
    WorkerState* tgt = workers_[worker_id].get();
    {
        std::lock_guard<std::mutex> wlk(tgt->inbox_mtx);
        tgt->local_runnable.push_back(&fiber);
        fiber_owner_[&fiber] = tgt;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    sluice_async_test::set_trace_wake_cause(*this, sluice_async_test::WakeCause::runnable_route,
                                            static_cast<unsigned>(-1));
#endif
    signal_wake_locked();
}

WorkerState* Scheduler::current_worker() {
    return g_worker;
}

void Scheduler::run(unsigned worker_count) {
    run_impl(worker_count, RunMode::drain);
}

void Scheduler::run_live(unsigned worker_count) {
    run_impl(worker_count, RunMode::live);
}

void Scheduler::run_live(unsigned worker_count, bool (*stop_fn)(void*), void* stop_ctx) {
    invocation_stop_fn_ = stop_fn;
    invocation_stop_ctx_ = stop_ctx;
    run_impl(worker_count, RunMode::live);
    invocation_stop_fn_ = nullptr;
    invocation_stop_ctx_ = nullptr;
}

void Scheduler::run_impl(unsigned worker_count, RunMode mode) {
    if (worker_count == 0)
        worker_count = 1;
    run_mode_ = mode;

    WorkerSnapshot run_workers;
    run_workers.reserve(worker_count);
    {
        LockGuard lk(global_mtx_);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase(*this, sluice_async_test::PhaseTag::worker_topology_mutation);
#endif
        ensure_workers_locked(worker_count, run_workers);

        for (WorkerState* worker : run_workers) {
            fiber_ctx::reset_context(worker->sched_ctx);
        }

        unsigned target = 0;
        while (!pending_spawn_.empty()) {
            Fiber* fiber = pending_spawn_.front();
            pending_spawn_.pop_front();
            WorkerState* worker = run_workers[target % worker_count];
            std::lock_guard<std::mutex> wlk(worker->inbox_mtx);
            worker->local_runnable.push_back(fiber);
            fiber_owner_[fiber] = worker;
            ++target;
        }
        next_spawn_worker_ = 0;
        admission_ = AdmissionState::none;
        admission_owner_ = static_cast<unsigned>(-1);
        running_fiber_count_.store(0, std::memory_order_release);
        idle_workers_.store(0, std::memory_order_release);
        global_terminate_.store(false, std::memory_order_release);
        in_coordinated_run_ = true;
        active_worker_count_.store(worker_count, std::memory_order_release);
        live_loop_workers_ = worker_count;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
                                  sluice_async_test::PhaseTag::worker_topology_ready_before_start);
#endif

    if (worker_count == 1) {
        WorkerState* worker = run_workers[0];
        g_worker = worker;
        worker->owner_scheduler = this;
        worker->active.store(true, std::memory_order_release);
        worker->idle_dance_contributed_.store(0, std::memory_order_release);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        worker->loop_exit_reason.store(WorkerState::LoopExitReason::live,
                                       std::memory_order_relaxed);
        worker->loop_exited.store(false, std::memory_order_relaxed);
#endif
        worker_loop(worker, run_workers);
        worker->active.store(false, std::memory_order_release);
        g_worker = nullptr;
    } else {
        std::vector<std::thread> threads;
        threads.reserve(worker_count);
        for (WorkerState* worker : run_workers) {
            threads.emplace_back([this, worker, &run_workers] {
                g_worker = worker;
                worker->owner_scheduler = this;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                sluice_async_test::test_phase_worker(
                    *this, sluice_async_test::PhaseTag::worker_startup_before_publication,
                    worker->id);
#endif
                worker->active.store(true, std::memory_order_release);
                worker->idle_dance_contributed_.store(0, std::memory_order_release);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                worker->loop_exit_reason.store(WorkerState::LoopExitReason::live,
                                               std::memory_order_relaxed);
                worker->loop_exited.store(false, std::memory_order_relaxed);
#endif
                worker_loop(worker, run_workers);
                worker->active.store(false, std::memory_order_release);
                g_worker = nullptr;
            });
        }
        for (auto& thread : threads) {
            if (thread.joinable())
                thread.join();
        }
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::worker_topology_joined_before_unpublish);
#endif
    {
        LockGuard lk(global_mtx_);
        in_coordinated_run_ = false;
        active_worker_count_.store(0, std::memory_order_release);
    }
}

void Scheduler::ensure_workers_locked(unsigned worker_count, WorkerSnapshot& run_workers) {
    while (workers_.size() < worker_count) {
        workers_.push_back(std::make_unique<WorkerState>());
        workers_.back()->id = static_cast<unsigned>(workers_.size() - 1);
    }
    for (unsigned i = 0; i < worker_count; ++i) {
        run_workers.push_back(workers_[i].get());
    }
}

void Scheduler::worker_loop(WorkerState* ws, const WorkerSnapshot& run_workers) {
    while (true) {
        ws->idle_dance_contributed_.store(0, std::memory_order_release);

        Fiber* f = nullptr;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        if (sluice_async_test::schedule_script_active(*this)) {
            f = sluice_async_test::schedule_script_pick(*this, ws);
        }
#endif
        if (!f) {
            std::lock_guard<std::mutex> lk(ws->inbox_mtx);
            if (!ws->local_runnable.empty()) {
                f = ws->local_runnable.front();
                ws->local_runnable.pop_front();
            }
        }
        if (!f) {
            LockGuard lk(global_mtx_);
            if (!pending_spawn_.empty()) {
                f = pending_spawn_.front();
                pending_spawn_.pop_front();

                fiber_owner_[f] = ws;
            }
        }

        if (!f && run_workers.size() > 1) {
            if (try_steal(ws, run_workers)) {
                continue;
            }
        }

        if (f) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

            sluice_async_test::test_phase_worker(
                *this, sluice_async_test::PhaseTag::worker_ticket_popped, ws->id);
#endif

            {
                const unsigned erased = idle_workers_.exchange(0, std::memory_order_acq_rel);
                if (erased != 0) {
                    dance_epoch_.fetch_add(1, std::memory_order_acq_rel);
                }
            }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

            sluice_async_test::test_phase_worker(
                *this, sluice_async_test::PhaseTag::worker_ticket_erase_done, ws->id);
#endif
            run_next_on(ws, f);
            continue;
        }

        MwState state;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        bool tv1_routed = false;
#endif
        {
            LockGuard lk(global_mtx_);
            (void)drain_routed_completion_waits_locked();
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            tv1_routed = wake_ready_flags_locked();
#else
            (void)wake_ready_flags_locked();
#endif
            (void)pump_deadlines_locked();
            state = classify_locked(run_workers, ws);
        }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        if (tv1_routed) {
            sluice_async_test::test_phase(*this, sluice_async_test::PhaseTag::tv1_wake_scan_routed);
        }
#endif

        if (state == MwState::mw_s1) {
            {
                const unsigned erased = idle_workers_.exchange(0, std::memory_order_acq_rel);
                if (erased != 0) {
                    dance_epoch_.fetch_add(1, std::memory_order_acq_rel);
                }
            }
            if (global_terminate_.load(std::memory_order_acquire)) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                ws->loop_exit_reason = WorkerState::LoopExitReason::mw_s1_terminate_observed;
#endif
                break;
            }
        }

        if (state == MwState::mw_s2) {
            bool elected = false;
            {
                LockGuard lk(global_mtx_);

                if (classify_locked(run_workers, ws) == MwState::mw_s2 &&
                    admission_ == AdmissionState::none) {
                    unsigned lowest_alive = static_cast<unsigned>(-1);
                    for (WorkerState* w : run_workers) {
                        if (w->active.load(std::memory_order_acquire) && w->id < lowest_alive) {
                            lowest_alive = w->id;
                        }
                    }
                    if (ws->id == lowest_alive) {
                        admission_ = AdmissionState::candidate;
                        admission_owner_ = ws->id;
                        elected = true;
                    }
                }
            }

            if (elected) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                sluice_async_test::test_phase(*this,
                                              sluice_async_test::PhaseTag::mw_admission_phase_b);
#endif

                bool phase_b_committed = false;

                bool ready_flag_observation = false;
                {
                    LockGuard lk(global_mtx_);
                    ready_flag_observation = !waiting_ready_.empty();

                    if (admission_ != AdmissionState::candidate || admission_owner_ != ws->id) {
                        continue;
                    }

                    (void)drain_routed_completion_waits_locked();
                    (void)wake_ready_flags_locked();
                    (void)pump_deadlines_locked();
                    MwState s2 = classify_locked(run_workers, ws);
                    if (s2 != MwState::mw_s2) {
                        admission_ = AdmissionState::none;
                        admission_owner_ = static_cast<unsigned>(-1);
                        continue;
                    }

                    admission_ = AdmissionState::committed;
                    phase_b_committed = true;

                    const bool split_wait = ctx_.has_split_wait_capability();
                    const bool bounded_park_needed =
                        ready_flag_observation ||
                        earliest_active_deadline_.load(std::memory_order::acquire) != kNoDeadline;
                    const bool backend_park_ok =
                        split_wait &&
                        (!bounded_park_needed || ctx_.has_bounded_split_wait_capability());
                    ws->park_domain = backend_park_ok
                                          ? WorkerState::ParkDomain::Backend
                                          : (!split_wait && !external_wake_possible_locked()
                                                 ? WorkerState::ParkDomain::Backend
                                                 : WorkerState::ParkDomain::Scheduler);

                    if (ws->park_domain == WorkerState::ParkDomain::Backend) {
                        ctx_.arm_backend_wait_commit();
                        backend_wait_active_.store(true, std::memory_order_release);
                    }
                }

                if (phase_b_committed) {
                    if (ws->park_domain == WorkerState::ParkDomain::Scheduler) {
                        ws->park_domain = WorkerState::ParkDomain::None;

                        park_on_wake_source(ws, true);

                        {
                            LockGuard lk(global_mtx_);
                            admission_ = AdmissionState::none;
                            admission_owner_ = static_cast<unsigned>(-1);
                            (void)drain_routed_completion_waits_locked();
                            (void)wake_ready_flags_locked();
                            (void)pump_deadlines_locked();
                        }

                        idle_workers_.store(0, std::memory_order_release);
                        continue;
                    }

                    ws->park_domain = WorkerState::ParkDomain::Backend;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                    sluice_async_test::test_phase(
                        *this, sluice_async_test::PhaseTag::mw_s2_committed_before_wait_one);
#endif

                    auto max_park = std::chrono::nanoseconds::max();
                    {
                        deadline_t earliest =
                            earliest_active_deadline_.load(std::memory_order::acquire);
                        if (earliest != kNoDeadline) {
                            if (test_clock_mode_.load(std::memory_order::acquire)) {
                                max_park = std::chrono::milliseconds(1);
                            } else {
                                deadline_t now_ticks = clock_now_unlocked();
                                if (earliest <= now_ticks) {
                                    max_park = std::chrono::nanoseconds::zero();
                                } else {
                                    max_park = std::chrono::milliseconds(earliest - now_ticks);
                                }
                            }
                        }
                        if (ready_flag_observation) {
                            const auto observation = std::chrono::milliseconds(2);
                            if (max_park == std::chrono::nanoseconds::max() ||
                                max_park > observation) {
                                max_park = observation;
                            }
                        }
                    }

                    if (max_park != std::chrono::nanoseconds::max() &&
                        !ctx_.has_bounded_split_wait_capability()) {
                        max_park = std::chrono::nanoseconds::max();
                    }
                    auto wr = ctx_.wait_one(max_park);
                    backend_wait_active_.store(false, std::memory_order_release);
                    ws->park_domain = WorkerState::ParkDomain::None;

                    bool made_progress = wr.has_value() && wr.value() > 0;

                    {
                        LockGuard lk(global_mtx_);
                        admission_ = AdmissionState::none;
                        admission_owner_ = static_cast<unsigned>(-1);
                        (void)drain_routed_completion_waits_locked();
                        (void)wake_ready_flags_locked();
                        (void)pump_deadlines_locked();
                    }

                    if (!made_progress) {
                        LockGuard lk(global_mtx_);

                        if (classify_locked(run_workers, ws) == MwState::mw_s1) {
                            continue;
                        }

                        if (external_wake_possible_locked()) {
                            continue;
                        }

                        global_terminate_.store(true, std::memory_order_release);

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                        sluice_async_test::set_trace_wake_cause(
                            *this, sluice_async_test::WakeCause::terminate,
                            static_cast<unsigned>(-1));
#endif
                        signal_wake_locked();

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                        sluice_async_test::release_all_phases(*this);
                        ws->loop_exit_reason =
                            WorkerState::LoopExitReason::mw_s2_no_progress_terminate;
#endif
                        break;
                    }
                    idle_workers_.store(0, std::memory_order_release);
                    continue;
                }
            }
        }

        bool ready_flag_observation = false;
        {
            LockGuard lk(global_mtx_);

            ready_flag_observation = !waiting_ready_.empty();

            MwState final_state = classify_locked(run_workers, ws);

            if (final_state == MwState::mw_s1 || final_state == MwState::mw_s2) {
                idle_workers_.store(0, std::memory_order_release);
            } else {
                if (final_state == MwState::mw_s3_unresolved && run_mode_ == RunMode::live &&
                    external_wake_possible_locked()) {
                    if (invocation_stop_fn_ != nullptr &&
                        invocation_stop_fn_(invocation_stop_ctx_)) {
                        ws->dance_epoch_at_contribution_.store(
                            dance_epoch_.load(std::memory_order_acquire),
                            std::memory_order_release);
                        unsigned prev = idle_workers_.fetch_add(1, std::memory_order_acq_rel);
                        ws->idle_dance_contributed_.store(1, std::memory_order_release);
                        if (prev + 1 >= live_loop_workers_) {
                            MwState still = classify_locked(run_workers, ws);
                            if (still == MwState::mw_s3_unresolved || still == MwState::quiescent) {
                                global_terminate_.store(true, std::memory_order_release);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                                sluice_async_test::set_trace_wake_cause(
                                    *this, sluice_async_test::WakeCause::terminate,
                                    static_cast<unsigned>(-1));
#endif
                                signal_wake_locked();
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                                sluice_async_test::release_all_phases(*this);
                                ws->loop_exit_reason =
                                    WorkerState::LoopExitReason::e14f1_last_idle_terminate;
#endif
                                break;
                            }
                            idle_workers_.store(0, std::memory_order_release);

                            continue;
                        } else {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                            sluice_async_test::set_trace_wake_cause(
                                *this, sluice_async_test::WakeCause::idle_dance,
                                static_cast<unsigned>(-1));
#endif
                            signal_wake_locked();
                        }
                    } else {
                        idle_workers_.store(0, std::memory_order_release);
                    }
                } else {
                    ws->dance_epoch_at_contribution_.store(
                        dance_epoch_.load(std::memory_order_acquire), std::memory_order_release);
                    unsigned prev = idle_workers_.fetch_add(1, std::memory_order_acq_rel);
                    ws->idle_dance_contributed_.store(1, std::memory_order_release);
                    if (prev + 1 >= live_loop_workers_) {
                        MwState still = classify_locked(run_workers, ws);
                        if (still == MwState::mw_s3_unresolved || still == MwState::quiescent) {
                            global_terminate_.store(true, std::memory_order_release);

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                            sluice_async_test::set_trace_wake_cause(
                                *this, sluice_async_test::WakeCause::terminate,
                                static_cast<unsigned>(-1));
#endif
                            signal_wake_locked();

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                            sluice_async_test::release_all_phases(*this);
                            ws->loop_exit_reason = WorkerState::LoopExitReason::last_idle_terminate;
#endif
                            break;
                        }
                        idle_workers_.store(0, std::memory_order_release);

                        continue;
                    } else {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                        sluice_async_test::set_trace_wake_cause(
                            *this, sluice_async_test::WakeCause::idle_dance,
                            static_cast<unsigned>(-1));
#endif
                        signal_wake_locked();
                    }
                }
            }
        }

        if (global_terminate_.load(std::memory_order_acquire)) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            ws->loop_exit_reason = WorkerState::LoopExitReason::final_park_terminate;
#endif
            break;
        }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        sluice_async_test::test_phase_worker(
            *this, sluice_async_test::PhaseTag::scheduler_park_candidate, ws->id);
#endif

        park_on_wake_source(ws, ready_flag_observation);

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        sluice_async_test::test_phase(*this, sluice_async_test::PhaseTag::worker_park_returned);
#endif
    }

    {
        LockGuard glk(global_mtx_);

        --live_loop_workers_;

        ws->active.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> ilk(ws->inbox_mtx);
            if (!ws->local_runnable.empty()) {
                for (Fiber* f : ws->local_runnable) {
                    pending_spawn_.push_back(f);
                }
                ws->local_runnable.clear();
            }
        }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        sluice_async_test::set_trace_wake_cause(
            *this, sluice_async_test::WakeCause::retire_epilogue, ws->id);
#endif
        signal_wake_locked();
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    ws->loop_exited.store(true, std::memory_order_release);
#endif
}

void Scheduler::run_next_on(WorkerState* ws, Fiber* fiber) {
    if (!fiber->make_running()) {
        detail::scheduler_invalid_runnable_ticket_fail_fast();
    }
    ws->current = fiber;
    running_fiber_count_.fetch_add(1, std::memory_order_acq_rel);
    fiber_ctx::Switch s;
    s.old = &ws->sched_ctx;
    s.new_ = &fiber->ctx;
    (void)fiber_ctx::context_switch(&s);

    ws->suspend_switch_pending.store(false, std::memory_order_release);
    ws->current = nullptr;
    running_fiber_count_.fetch_sub(1, std::memory_order_acq_rel);
}

void Scheduler::commit_suspend_locked(WorkerState* ws, Fiber* fiber) {
    ws->suspend_switch_pending.store(true, std::memory_order_release);
    if (!fiber->make_waiting()) {
        detail::scheduler_invalid_suspend_transition_fail_fast();
    }
}

void Scheduler::route_runnable(Fiber* f, WorkerState* owner) {
    if (owner) {
        std::lock_guard<std::mutex> lk(owner->inbox_mtx);
        owner->local_runnable.push_back(f);
    } else {
        pending_spawn_.push_back(f);
    }
}

bool Scheduler::drain_routed_completion_waits_locked() {
    (void)ctx_.poll();
    bool woken = false;
    WaitRecord* head = nullptr;
    {
        LockGuard rlk(wait_registry_mtx_);
        head = wait_delivered_head_;
        wait_delivered_head_ = nullptr;
    }
    while (head != nullptr) {
        WaitRecord* r = head;
        head = r->next_delivered;
        r->next_delivered = nullptr;

        Fiber* f = r->fiber;
        WorkerState* owner = r->owner;
        {
            LockGuard rlk(wait_registry_mtx_);
            r->state = WaitRecordState::free;
            r->fiber = nullptr;
            r->owner = nullptr;
            r->completion = nullptr;
            r->next_free = wait_record_free_head_;
            wait_record_free_head_ = r;
            --wait_record_live_count_;
        }

        f->set_completion_wait_outcome(CompletionWaitOutcome::completed);
        if (f->make_runnable()) {
            route_runnable_locked(f, owner);
            woken = true;
        }
    }

    for (auto it = waiting_size_.begin(); it != waiting_size_.end();) {
        auto* c = static_cast<Completion<std::size_t>*>(it->first);
        if (c->ready()) {
            Fiber* f = it->second.fiber;
            WorkerState* owner = it->second.owner;
            it = waiting_size_.erase(it);

            f->set_completion_wait_outcome(CompletionWaitOutcome::completed);
            if (f->make_runnable()) {
                route_runnable_locked(f, owner);
                woken = true;
            }
        } else {
            ++it;
        }
    }
    for (auto it = waiting_void_.begin(); it != waiting_void_.end();) {
        auto* c = static_cast<Completion<void>*>(it->first);
        if (c->ready()) {
            Fiber* f = it->second.fiber;
            WorkerState* owner = it->second.owner;
            it = waiting_void_.erase(it);

            f->set_completion_wait_outcome(CompletionWaitOutcome::completed);
            if (f->make_runnable()) {
                route_runnable_locked(f, owner);
                woken = true;
            }
        } else {
            ++it;
        }
    }
    return woken;
}

void Scheduler::ReadyRoutingSink::on_ready(detail::ReadyEvent event) noexcept {
    Scheduler* s = scheduler_;
    if (s == nullptr)
        return;
    if (!event.waiter.has_waiter) {
        return;
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    ++deliveries_;
#endif
    const detail::WaiterToken& t = event.waiter.token;
    LockGuard rlk(s->wait_registry_mtx_);

    if (t.scheduler_identity != s->scheduler_identity_) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        ++stale_dropped_;
#endif
        return;
    }
    if (t.registration_slot >= s->wait_records_.size()) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        ++stale_dropped_;
#endif
        return;
    }
    WaitRecord* r = s->wait_records_[t.registration_slot].get();
    if (r->generation != t.registration_generation) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        ++stale_dropped_;
#endif
        return;
    }
    if (r->state != WaitRecordState::registered) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        ++cancel_lost_;
#endif
        return;
    }
    r->state = WaitRecordState::delivered;
    r->next_delivered = s->wait_delivered_head_;
    s->wait_delivered_head_ = r;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    ++routed_;
#endif
}

bool Scheduler::wake_ready_flags_locked() {
    bool woken = false;
    for (auto it = waiting_ready_.begin(); it != waiting_ready_.end();) {
        if (it->first->load(std::memory_order::acquire)) {
            Fiber* f = it->second.fiber;
            WorkerState* owner = it->second.owner;
            it = waiting_ready_.erase(it);
            if (f->make_runnable()) {
                route_runnable_locked(f, owner);
                woken = true;
            }
        } else {
            ++it;
        }
    }
    return woken;
}

void Scheduler::route_runnable_locked(Fiber* f, WorkerState* owner) {
    const unsigned participant_count = active_worker_count_.load(std::memory_order_acquire);
    if (participant_count == 0) {
        pending_spawn_.push_back(f);
        signal_wake_locked();
        return;
    }

    global_terminate_.store(false, std::memory_order_release);

    {
        const unsigned erased = idle_workers_.exchange(0, std::memory_order_acq_rel);
        if (erased != 0) {
            dance_epoch_.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    if (admission_ == AdmissionState::candidate) {
        admission_ = AdmissionState::none;
        admission_owner_ = static_cast<unsigned>(-1);
    }

    WorkerState* target = owner;
    if (target == nullptr || target->id >= participant_count ||
        workers_[target->id].get() != target) {
        const unsigned target_id = next_spawn_worker_++ % participant_count;
        target = workers_[target_id].get();
        fiber_owner_[f] = target;
    }
    {
        std::lock_guard<std::mutex> lk(target->inbox_mtx);
        target->local_runnable.push_back(f);
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    sluice_async_test::set_trace_wake_cause(*this, sluice_async_test::WakeCause::runnable_route,
                                            static_cast<unsigned>(-1));
#endif
    signal_wake_locked();
}

WorkerState* Scheduler::owner_for_fiber_locked(Fiber* fiber) {
    auto it = fiber_owner_.find(fiber);
    if (it == fiber_owner_.end() || it->second == nullptr) {
        detail::scheduler_missing_fiber_owner_fail_fast();
    }
    return it->second;
}

bool Scheduler::publish_waiting_fiber_runnable_locked(Fiber* fiber) {
    WorkerState* owner = owner_for_fiber_locked(fiber);
    if (!fiber->make_runnable()) {
        return false;
    }
    route_runnable_locked(fiber, owner);
    return true;
}

void Scheduler::publish_wait_winner_locked(WaitNode& won) {
    const WaitResume& r = won.resume();
    switch (r.kind()) {
    case WaitResume::Kind::fiber:
        (void)publish_waiting_fiber_runnable_locked(r.as_fiber());
        break;
    case WaitResume::Kind::deferred:
        defer_publication_locked(r.as_deferred());
        break;
    case WaitResume::Kind::none:
        break;
    }
}

void Scheduler::defer_publication_locked(void* delivery_record) noexcept {
    try {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        if (sluice_async_test::deferred_publication_alloc_should_fail(*this)) {
            throw std::bad_alloc();
        }
#endif
        deferred_publications_.push_back(delivery_record);
    } catch (...) {
        detail::scheduler_deferred_publication_stranded_fail_fast();
    }
}

std::size_t Scheduler::take_deferred_publications(void** out, std::size_t cap) {
    if (cap == 0)
        return 0;
    LockGuard lk(global_mtx_);
    const std::size_t n = std::min(cap, deferred_publications_.size());
    if (n == 0)
        return 0;
    std::move(deferred_publications_.begin(),
              deferred_publications_.begin() + static_cast<std::ptrdiff_t>(n), out);
    deferred_publications_.erase(deferred_publications_.begin(),
                                 deferred_publications_.begin() + static_cast<std::ptrdiff_t>(n));
    return n;
}

Scheduler::MwState Scheduler::classify_locked(const WorkerSnapshot& run_workers,
                                              WorkerState* classify_ws) const {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    const MwState traced = classify_locked_impl(run_workers);
    if (classify_ws != nullptr) {
        classify_ws->last_classify.store(static_cast<int>(traced), std::memory_order_relaxed);
        classify_ws->classify_seq.fetch_add(1, std::memory_order_relaxed);
    }
    return traced;
#else
    (void)classify_ws;
    return classify_locked_impl(run_workers);
#endif
}

Scheduler::MwState Scheduler::classify_locked_impl(const WorkerSnapshot& run_workers) const {
    bool any_runnable = !pending_spawn_.empty();
    if (!any_runnable) {
        for (WorkerState* worker : run_workers) {
            std::lock_guard<std::mutex> wlk(worker->inbox_mtx);
            if (!worker->local_runnable.empty()) {
                any_runnable = true;
                break;
            }
        }
    }
    const bool any_running = running_fiber_count_.load(std::memory_order_acquire) > 0;
    if (any_runnable || any_running)
        return MwState::mw_s1;

    const bool any_outstanding = ctx_.outstanding() > 0;
    if (any_outstanding)
        return MwState::mw_s2;

    const bool any_wait = !waiting_size_.empty() || !waiting_void_.empty() ||
                          !waiting_ready_.empty() || waiting_waitq_count_ > 0 ||
                          waiting_select_count_ > 0;
    if (any_wait)
        return MwState::mw_s3_unresolved;

    return MwState::quiescent;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
void Scheduler::AsyncTestAccess::set_arm_state(Scheduler& s, detail::SelectArmSlot& arm,
                                               detail::ArmState st) {
    LockGuard lk(s.global_mtx_);
    arm.state = st;
}

bool Scheduler::AsyncTestAccess::detached_claim_winner(detail::SelectGroup& group,
                                                       std::uint32_t arm_index) noexcept {
    assert(group.scheduler_ == nullptr &&
           "detached winner-CAS accessor requires scheduler_ == nullptr");
    assert(group.arms_ == nullptr && "detached winner-CAS accessor requires arms_ == nullptr");
    assert(group.arm_count_ == 0 && "detached winner-CAS accessor requires arm_count_ == 0");
    return group.claim_winner_locked(arm_index);
}

bool Scheduler::AsyncTestAccess::select_process_group(Scheduler& s, detail::SelectGroup& group,
                                                      std::uint32_t candidate_index) {
    LockGuard lk(s.global_mtx_);
    return s.select_process_group_locked(group, candidate_index);
}

bool Scheduler::AsyncTestAccess::select_all_authority_closed(const Scheduler& s,
                                                             const detail::SelectGroup& group) {
    LockGuard lk(s.global_mtx_);
    return s.select_all_authority_closed_locked(group);
}

void Scheduler::AsyncTestAccess::assert_select_all_authority_closed(
    const Scheduler& s, const detail::SelectGroup& group) {
    LockGuard lk(s.global_mtx_);
    const bool closed = s.select_all_authority_closed_locked(group);
    assert(closed && "Select publication requires all arm authority closed");
    if (!closed)
        detail::select_invariant_fail_fast();
}

void Scheduler::AsyncTestAccess::select_event_forge_stale_home(Scheduler& s, Event& event,
                                                               detail::SelectArmSlot& arm) {
    LockGuard lk(s.global_mtx_);
    assert(&event.scheduler_ == &s &&
           "select_event_forge_stale_home: Event does not belong to this Scheduler");
    assert(arm.home_ == nullptr &&
           "select_event_forge_stale_home: arm must be unlinked (home_ != nullptr)");
    assert(arm.next_ == nullptr && arm.prev_ == nullptr &&
           "select_event_forge_stale_home: arm must be fully unlinked");

    for (detail::SelectArmSlot* p = event.select_port_.head_; p != nullptr; p = p->next_) {
        assert(p != &arm && "select_event_forge_stale_home: arm is actually "
                            "linked into the port intrusive list; cannot forge stale home_");
    }
    arm.home_ = &event.select_port_;
}

void Scheduler::AsyncTestAccess::select_event_forge_wrong_home(Scheduler& s, Event& event_a,
                                                               Event& event_b,
                                                               detail::SelectArmSlot& arm) {
    LockGuard lk(s.global_mtx_);
    assert(&event_a.scheduler_ == &s && &event_b.scheduler_ == &s &&
           "select_event_forge_wrong_home: Events must belong to this Scheduler");
    assert(arm.kind == detail::ArmKind::event && arm.event.event_ == &event_a &&
           "select_event_forge_wrong_home: arm must be an Event arm bound to event_a");

    arm.home_ = &event_b.select_port_;
}

namespace {

struct ForgedRwWaitCtx {
    enum class Mode : std::uint8_t { read, write };
    Mode mode;
};
} // namespace
void Scheduler::AsyncTestAccess::rwlock_death_forge_invalid_head_mode(Scheduler& s,
                                                                      AsyncRwLock& rw) {
    struct BadCtx {
        std::uint8_t mode{99};
    } bad;
    WaitNode forged_head;
    forged_head.set_user(&bad);
    {
        LockGuard lk(s.global_mtx_);
        LockGuard qlk(rw.waiters_.mtx());
        (void)rw.waiters_.register_wait_locked(forged_head, WaitResume::none());
        ++s.waiting_waitq_count_;
    }

    LockGuard glk(s.global_mtx_);
    s.rwlock_grant_from_head_locked(rw.waiters_, rw.active_readers_, rw.writer_active_,
                                    rw.writer_owner_);
}

void Scheduler::AsyncTestAccess::rwlock_death_forge_null_head_user(Scheduler& s, AsyncRwLock& rw) {
    WaitNode forged_head;

    {
        LockGuard lk(s.global_mtx_);
        LockGuard qlk(rw.waiters_.mtx());
        (void)rw.waiters_.register_wait_locked(forged_head, WaitResume::none());
        ++s.waiting_waitq_count_;
    }
    LockGuard lk(s.global_mtx_);
    s.rwlock_grant_from_head_locked(rw.waiters_, rw.active_readers_, rw.writer_active_,
                                    rw.writer_owner_);
}

void Scheduler::AsyncTestAccess::rwlock_death_forge_invalid_batch_member(Scheduler& s,
                                                                         AsyncRwLock& rw) {
    ForgedRwWaitCtx good_read{ForgedRwWaitCtx::Mode::read};
    struct BadCtx {
        std::uint8_t mode{99};
    } bad;
    WaitNode forged_head;
    WaitNode forged_second;
    forged_head.set_user(&good_read);
    forged_second.set_user(&bad);
    {
        LockGuard lk(s.global_mtx_);
        LockGuard qlk(rw.waiters_.mtx());
        (void)rw.waiters_.register_wait_locked(forged_head, WaitResume::none());
        (void)rw.waiters_.register_wait_locked(forged_second, WaitResume::none());
        s.waiting_waitq_count_ += 2;
    }
    LockGuard lk(s.global_mtx_);
    s.rwlock_grant_from_head_locked(rw.waiters_, rw.active_readers_, rw.writer_active_,
                                    rw.writer_owner_);
}
#endif

std::size_t Scheduler::runnable_count() const {
    std::size_t total = 0;
    LockGuard lk(global_mtx_);
    total += pending_spawn_.size();
    for (auto& w : workers_) {
        std::lock_guard<std::mutex> wlk(w->inbox_mtx);
        total += w->local_runnable.size();
    }
    return total;
}

bool Scheduler::unguarded_progress_pending_locked() const {
    if (!pending_spawn_.empty()) {
        return true;
    }
    const unsigned active_count = active_worker_count_.load(std::memory_order_acquire);

    for (unsigned i = 0; i < active_count && i < workers_.size(); ++i) {
        WorkerState* w = workers_[i].get();
        std::lock_guard<std::mutex> ilk(w->inbox_mtx);
        if (!w->local_runnable.empty()) {
            return true;
        }
    }

    if (running_fiber_count_.load(std::memory_order_acquire) > 0 ||
        backend_wait_active_.load(std::memory_order_acquire) ||
        admission_ != AdmissionState::none) {
        return false;
    }

    return ctx_.outstanding() > 0;
}

bool Scheduler::try_steal(WorkerState* thief, const WorkerSnapshot& run_workers) {
    if (run_workers.size() <= 1)
        return false;

    LockGuard lk(global_mtx_);

    unsigned n = static_cast<unsigned>(run_workers.size());
    for (unsigned k = 1; k < n; ++k) {
        unsigned vidx = (thief->id + k) % n;
        WorkerState* victim = run_workers[vidx];

        if (victim->suspend_switch_pending.load(std::memory_order_acquire)) {
            continue;
        }
        Fiber* stolen = nullptr;
        {
            std::lock_guard<std::mutex> vlk(victim->inbox_mtx);

            for (auto it = victim->local_runnable.begin(); it != victim->local_runnable.end();
                 ++it) {
                Fiber* f = *it;
                if (f->state() != FiberState::runnable)
                    continue;
                auto oit = fiber_owner_.find(f);
                if (oit == fiber_owner_.end() || oit->second != victim)
                    continue;
                stolen = f;
                victim->local_runnable.erase(it);
                break;
            }
        }
        if (stolen) {
            fiber_owner_[stolen] = thief;
            {
                std::lock_guard<std::mutex> tlk(thief->inbox_mtx);
                thief->local_runnable.push_back(stolen);
            }

            return true;
        }
    }
    return false;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
TimerRegistration* Scheduler::AsyncTestAccess::register_test_deadline(Scheduler& s, WaitNode* node,
                                                                      WaitQueue* q,
                                                                      deadline_t deadline) {
    LockGuard lk(s.global_mtx_);
    return s.register_test_deadline_locked(node, q, deadline);
}

std::size_t Scheduler::AsyncTestAccess::timer_pool_size(const Scheduler& s) noexcept
    SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    return s.timer_pool_.size();
}

std::size_t Scheduler::AsyncTestAccess::deadline_heap_size(const Scheduler& s) noexcept
    SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    return s.deadline_heap_.size();
}

std::size_t Scheduler::AsyncTestAccess::deadline_heap_capacity(const Scheduler& s) noexcept
    SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    return s.deadline_heap_.capacity();
}

std::size_t Scheduler::AsyncTestAccess::active_deadline_count(const Scheduler& s) noexcept {
    LockGuard lk(s.global_mtx_);
    return s.active_deadline_count_;
}

std::size_t Scheduler::AsyncTestAccess::timer_pool_count_in_state(
    const Scheduler& s, TimerRegistration::State st) noexcept SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    std::size_t n = 0;
    for (const auto& r : s.timer_pool_) {
        if (r.state() == st)
            ++n;
    }
    return n;
}

bool Scheduler::AsyncTestAccess::earliest_active_deadline(Scheduler& s, deadline_t& out) {
    LockGuard lk(s.global_mtx_);
    return s.earliest_active_deadline_locked(out);
}

void Scheduler::AsyncTestAccess::advance_clock(Scheduler& s, deadline_t t) {
    s.advance_clock(t);
}

std::size_t Scheduler::AsyncTestAccess::select_timer_pool_size(const Scheduler& s) noexcept
    SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    return s.select_timer_pool_.size();
}

std::size_t Scheduler::AsyncTestAccess::select_timer_count_in_state(
    const Scheduler& s,
    detail::SelectTimerRegistration::State st) noexcept SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    std::size_t n = 0;
    for (const auto& r : s.select_timer_pool_) {
        if (r.state() == st)
            ++n;
    }
    return n;
}

std::array<std::size_t, 2> Scheduler::AsyncTestAccess::tagged_heap_counts_by_kind(
    const Scheduler& s) noexcept SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    std::array<std::size_t, 2> counts{0, 0};
    for (const auto& e : s.deadline_heap_) {
        if (e.kind == detail::DeadlineHeapEntry::Kind::ordinary) {
            ++counts[0];
        } else {
            ++counts[1];
        }
    }
    return counts;
}

bool Scheduler::AsyncTestAccess::deadline_heap_has_select_target(
    const Scheduler& s,
    const detail::SelectTimerRegistration* target) noexcept SLUICE_NO_THREAD_SAFETY_ANALYSIS {
    for (const auto& e : s.deadline_heap_) {
        if (e.kind == detail::DeadlineHeapEntry::Kind::select && e.target.select == target) {
            return true;
        }
    }
    return false;
}

namespace detail {
void set_evented_admission_override_impl(bool supported) noexcept;
void clear_evented_admission_override_impl() noexcept;
bool get_evented_admission_override_impl() noexcept;
} // namespace detail

void Scheduler::AsyncTestAccess::set_evented_admission_override(bool supported) noexcept {
    detail::set_evented_admission_override_impl(supported);
}

bool Scheduler::AsyncTestAccess::evented_admission_override() noexcept {
    return detail::get_evented_admission_override_impl();
}
#endif

} // namespace sluice::async
