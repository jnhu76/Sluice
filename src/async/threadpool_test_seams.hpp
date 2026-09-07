#pragma once

#include <sluice/async/threadpool_backend.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace sluice::async {

struct ThreadPoolBackend::AfterArenaEnqueueBeforeDispatchPushPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};

    std::atomic<bool> work_domain_held{false};

    std::atomic<bool> dispatch_push_completed{false};

    std::atomic<bool> exited{true};
};

struct ThreadPoolBackend::BeforeWorkerDequeuePauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};

    std::atomic<std::uint64_t> armed{0};
    std::atomic<std::uint64_t> paused_at{0};
    std::atomic<std::uint64_t> resumed_at{0};
    std::atomic<std::uint64_t> acked_at{0};
};

struct ThreadPoolBackend::PostResumePrePopHoldGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};
struct ThreadPoolBackend::WorkerRunningPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};
struct ThreadPoolBackend::TerminalPublicationPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct ThreadPoolBackend::ControlWakeFinalReapPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct ThreadPoolBackend::BeforeEnqueueLockPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct ThreadPoolBackend::BeforeAdmissionLockPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};
struct ThreadPoolBackend::BeforeCommitBindingPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct ThreadPoolBackend::DispatchFailureInjection {
    std::atomic<bool> armed{false};
    std::atomic<std::size_t> fired{0};
};

struct ThreadPoolBackend::SubmitStageFailureInjection {
    std::atomic<bool> fail_reserve{false};
    std::atomic<bool> fail_prepare{false};
    std::atomic<bool> fail_commit{false};
    std::atomic<std::size_t> reserve_fired{0};
    std::atomic<std::size_t> prepare_fired{0};
    std::atomic<std::size_t> commit_fired{0};
};

inline std::size_t ThreadPoolBackend::workers_spawned_for_test() const noexcept {
    return workers_.size();
}

inline std::size_t ThreadPoolBackend::active_workers_for_test() const {
    std::lock_guard<std::mutex> lk(work_mtx_);
    return active_workers_;
}

inline std::size_t ThreadPoolBackend::dispatch_size_for_test() const {
    std::lock_guard<std::mutex> lk(work_mtx_);
    return dispatch_.size();
}
inline std::size_t ThreadPoolBackend::dispatch_high_water_for_test() const {
    std::lock_guard<std::mutex> lk(work_mtx_);
    return dispatch_.high_water();
}

inline std::uint64_t ThreadPoolBackend::syscall_count_for_test() const noexcept {
    return syscall_count_.load();
}

inline std::size_t ThreadPoolBackend::backend_ready_count_for_test() const noexcept {
    return arena_.backend_ready_count();
}

inline std::optional<BackendWaitToken> ThreadPoolBackend::try_wait_token_for_test() const noexcept {
    return ready_wait_.try_snapshot();
}
inline std::optional<std::size_t> ThreadPoolBackend::try_outstanding_for_test() const noexcept {
    return arena_.try_accepted_outstanding();
}
inline std::optional<std::size_t>
ThreadPoolBackend::try_backend_ready_count_for_test() const noexcept {
    return arena_.try_backend_ready_count();
}

inline void ThreadPoolBackend::set_wait_phase_flag_for_test(std::atomic<bool>* flag) noexcept {
    ready_wait_.set_wait_phase_flag(flag);
}

inline void
ThreadPoolBackend::set_wait_prepark_counter_for_test(std::atomic<int>* counter) noexcept {
    ready_wait_.set_wait_prepark_counter(counter);
}

inline void ThreadPoolBackend::wait_epoch_changed_for_test(BackendWaitToken observed) noexcept {
    ready_wait_.wait_epoch_changed(observed);
}

inline std::optional<detail::SlotHandle>
ThreadPoolBackend::handle_for_completion_for_test(const void* completion) const noexcept {
    return arena_.resolve_completion(completion);
}

inline std::optional<detail::RequestArena::RequestObservation>
ThreadPoolBackend::observe_for_test(detail::SlotHandle h) const noexcept {
    return arena_.observe_for_test(h);
}

inline detail::CancelDisposition
ThreadPoolBackend::cancel_handle_for_test(detail::SlotHandle h) noexcept {
    detail::CancelDisposition disp;
    {
        std::lock_guard<std::mutex> lk(work_mtx_);
        (void)dispatch_.remove_exact(h);
        disp = arena_.cancel(h);
    }
    if (disp == detail::CancelDisposition::terminal_won) {
        tally_canceled();
        signal_ready_progress();
    }
    return disp;
}

inline void ThreadPoolBackend::set_after_enqueue_before_push_pause_gate(
    AfterArenaEnqueueBeforeDispatchPushPauseGate* gate) noexcept {
    after_enqueue_before_push_gate_.store(gate, std::memory_order_release);
}
inline void
ThreadPoolBackend::set_before_dequeue_pause_gate(BeforeWorkerDequeuePauseGate* gate) noexcept {
    before_dequeue_gate_.store(gate, std::memory_order_release);
}

inline void
ThreadPoolBackend::set_post_resume_pre_pop_hold_gate(PostResumePrePopHoldGate* gate) noexcept {
    post_resume_pre_pop_hold_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_running_pause_gate(WorkerRunningPauseGate* gate) noexcept {
    running_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_terminal_publication_pause_gate(
    TerminalPublicationPauseGate* gate) noexcept {
    terminal_publication_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_control_wake_final_reap_pause_gate(
    ControlWakeFinalReapPauseGate* gate) noexcept {
    control_wake_final_reap_gate_.store(gate, std::memory_order_release);
}
inline void
ThreadPoolBackend::set_before_enqueue_lock_pause_gate(BeforeEnqueueLockPauseGate* gate) noexcept {
    before_enqueue_lock_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_before_admission_lock_pause_gate(
    BeforeAdmissionLockPauseGate* gate) noexcept {
    before_admission_lock_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_before_commit_binding_pause_gate(
    BeforeCommitBindingPauseGate* gate) noexcept {
    before_commit_binding_gate_.store(gate, std::memory_order_release);
}
inline void
ThreadPoolBackend::set_dispatch_failure_injection(DispatchFailureInjection* injection) noexcept {
    dispatch_failure_injection_.store(injection, std::memory_order_release);
}
inline void ThreadPoolBackend::set_submit_stage_failure_injection(
    SubmitStageFailureInjection* injection) noexcept {
    submit_stage_failure_injection_.store(injection, std::memory_order_release);
}

inline Result<void> ThreadPoolBackend::register_waiter_for_test(Completion<std::size_t>& c,
                                                                detail::WaiterToken token,
                                                                detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}
inline Result<void> ThreadPoolBackend::register_waiter_for_test(Completion<void>& c,
                                                                detail::WaiterToken token,
                                                                detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}

inline Result<detail::RoutingLease>
ThreadPoolBackend::cancel_waiter_for_test(Completion<std::size_t>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}
inline Result<detail::RoutingLease> ThreadPoolBackend::cancel_waiter_for_test(Completion<void>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}

inline Result<void> ThreadPoolBackend::register_waiter_handle_for_test(detail::SlotHandle h,
                                                                       detail::WaiterToken token,
                                                                       detail::RoutingLease lease) {
    return arena_.register_waiter(h, token, std::move(lease));
}
inline Result<detail::RoutingLease>
ThreadPoolBackend::cancel_waiter_handle_for_test(detail::SlotHandle h) {
    return arena_.cancel_waiter(h);
}

inline std::optional<detail::RequestArena::BorrowSnapshot>
ThreadPoolBackend::borrow_for_test(detail::SlotHandle h) const noexcept {
    return arena_.borrow_for_test(h);
}

inline std::optional<detail::RequestArena::WaiterObservation>
ThreadPoolBackend::waiter_for_test(detail::SlotHandle h) const noexcept {
    return arena_.waiter_for_test(h);
}

inline std::size_t ThreadPoolBackend::sink_deliveries() const noexcept {
    return sink_.deliveries();
}
inline bool ThreadPoolBackend::sink_last_has_waiter() const noexcept {
    return sink_.last_has_waiter();
}
inline detail::WaiterToken ThreadPoolBackend::sink_last_token() const noexcept {
    return sink_.last_token();
}
inline std::uint64_t ThreadPoolBackend::sink_last_lease_id() const noexcept {
    return sink_.last_lease_id();
}

template <class Gate> void resume_threadpool_gate(Gate& gate) noexcept {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

template <class Gate> void rearm_threadpool_gate(Gate& gate) noexcept {
    gate.paused.store(false, std::memory_order_release);
    gate.resume.store(false, std::memory_order_release);
    gate.exited.store(true, std::memory_order_release);
}

namespace dequeue_gate_detail {
inline void publish_max_(std::atomic<std::uint64_t>& a, std::uint64_t v) noexcept {
    std::uint64_t cur = a.load(std::memory_order_relaxed);
    while (cur < v &&
           !a.compare_exchange_weak(cur, v, std::memory_order_release, std::memory_order_relaxed)) {
    }
}
} // namespace dequeue_gate_detail

inline void arm_dequeue_gate_generation(ThreadPoolBackend::BeforeWorkerDequeuePauseGate& gate,
                                        std::uint64_t generation) noexcept {
    dequeue_gate_detail::publish_max_(gate.armed, generation);
}

inline void wait_dequeue_gate_paused(ThreadPoolBackend::BeforeWorkerDequeuePauseGate& gate,
                                     std::uint64_t generation) noexcept {
    std::uint64_t seen = gate.paused_at.load(std::memory_order_acquire);
    while (seen < generation) {
        gate.paused_at.wait(seen, std::memory_order_acquire);
        seen = gate.paused_at.load(std::memory_order_acquire);
    }
}

inline void resume_dequeue_gate_generation(ThreadPoolBackend::BeforeWorkerDequeuePauseGate& gate,
                                           std::uint64_t generation) noexcept {
    dequeue_gate_detail::publish_max_(gate.resumed_at, generation);
    gate.resumed_at.notify_all();
}

inline void wait_dequeue_gate_ack(ThreadPoolBackend::BeforeWorkerDequeuePauseGate& gate,
                                  std::uint64_t generation) noexcept {
    std::uint64_t seen = gate.acked_at.load(std::memory_order_acquire);
    while (seen < generation) {
        gate.acked_at.wait(seen, std::memory_order_acquire);
        seen = gate.acked_at.load(std::memory_order_acquire);
    }
}

} // namespace sluice::async

#endif
