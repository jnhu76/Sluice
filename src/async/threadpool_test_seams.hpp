#pragma once

#include <sluice/async/threadpool_backend.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace sluice::async {

struct ThreadPoolBackend::SubmitEntryPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct ThreadPoolBackend::PreAcceptCommitPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct ThreadPoolBackend::AcceptedPreDispatchPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct ThreadPoolBackend::BeforeWorkerDequeuePauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct ThreadPoolBackend::WorkerClaimedPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct ThreadPoolBackend::WorkerOutcomePreTerminalPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct ThreadPoolBackend::PublicationEpiloguePauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct ThreadPoolBackend::ControlWakeFinalReapPauseGate {
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

inline std::optional<BackendWaitToken> ThreadPoolBackend::try_wait_token_for_test() const noexcept {
    return ready_wait_.try_snapshot();
}

inline std::size_t ThreadPoolBackend::publication_pending_size_for_test() const {
    std::lock_guard<std::mutex> lk(work_mtx_);
    return publication_pending_.size();
}

inline bool ThreadPoolBackend::event_owed_for_test(std::uint32_t slot) const {
    return delivery_[slot].event_owed;
}

inline void ThreadPoolBackend::set_wait_phase_flag_for_test(std::atomic<bool>* flag) noexcept {
    ready_wait_.set_wait_phase_flag(flag);
}

inline void ThreadPoolBackend::set_wait_prepark_counter_for_test(std::atomic<int>* counter) noexcept {
    ready_wait_.set_wait_prepark_counter(counter);
}

inline void ThreadPoolBackend::wait_epoch_changed_for_test(BackendWaitToken observed) noexcept {
    ready_wait_.wait_epoch_changed(observed);
}

inline std::optional<detail::RequestKey>
ThreadPoolBackend::request_key_for_test(const Completion<std::size_t>& c) const {
    return core_binding(c);
}

inline std::optional<detail::RequestKey>
ThreadPoolBackend::request_key_for_test(const Completion<void>& c) const {
    return core_binding(c);
}

inline Result<void> ThreadPoolBackend::register_waiter_key_for_test(detail::RequestKey key,
                                                                    detail::WaiterToken token,
                                                                    detail::RoutingLease lease) {
    if (core_->lookup(key) != detail::PublicLookup::outstanding) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    DeliveryRecord& record = delivery_[key.slot.value];
    if (record.registration == detail::WaiterRegistration::open_registered) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    record.registration = detail::WaiterRegistration::open_registered;
    record.waiter_token = token;
    record.waiter_lease = std::move(lease);
    record.waiter_delivery_present = true;
    return {};
}

inline Result<detail::RoutingLease>
ThreadPoolBackend::cancel_waiter_key_for_test(detail::RequestKey key) {
    DeliveryRecord& record = delivery_[key.slot.value];
    if (record.registration != detail::WaiterRegistration::open_registered) {
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
    }
    detail::RoutingLease lease = std::move(record.waiter_lease);
    record.waiter_token = {};
    record.registration = detail::WaiterRegistration::open_no_waiter;
    record.waiter_delivery_present = false;
    return lease;
}

inline detail::PublicCancel ThreadPoolBackend::cancel_key_for_test(detail::RequestKey key) {
    return cancel_key(key);
}

inline std::optional<ThreadPoolBackend::WaiterObservation>
ThreadPoolBackend::waiter_of_slot_for_test(std::uint32_t slot) const {
    const DeliveryRecord& record = delivery_[slot];
    return WaiterObservation{record.registration, record.waiter_delivery_present,
                             record.waiter_token, record.waiter_lease.id()};
}

inline void ThreadPoolBackend::set_submit_entry_pause_gate(SubmitEntryPauseGate* gate) noexcept {
    submit_entry_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_pre_accept_commit_pause_gate(
    PreAcceptCommitPauseGate* gate) noexcept {
    pre_accept_commit_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_accepted_pre_dispatch_pause_gate(
    AcceptedPreDispatchPauseGate* gate) noexcept {
    accepted_pre_dispatch_gate_.store(gate, std::memory_order_release);
}
inline void
ThreadPoolBackend::set_before_dequeue_pause_gate(BeforeWorkerDequeuePauseGate* gate) noexcept {
    before_dequeue_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_worker_claimed_pause_gate(WorkerClaimedPauseGate* gate) noexcept {
    worker_claimed_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_worker_outcome_pre_terminal_pause_gate(
    WorkerOutcomePreTerminalPauseGate* gate) noexcept {
    worker_outcome_pre_terminal_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_publication_epilogue_pause_gate(
    PublicationEpiloguePauseGate* gate) noexcept {
    publication_epilogue_gate_.store(gate, std::memory_order_release);
}
inline void ThreadPoolBackend::set_control_wake_final_reap_pause_gate(
    ControlWakeFinalReapPauseGate* gate) noexcept {
    control_wake_final_reap_gate_.store(gate, std::memory_order_release);
}
inline void
ThreadPoolBackend::set_dispatch_failure_injection(DispatchFailureInjection* injection) noexcept {
    dispatch_failure_injection_.store(injection, std::memory_order_release);
}
inline void ThreadPoolBackend::set_submit_stage_failure_injection(
    SubmitStageFailureInjection* injection) noexcept {
    submit_stage_failure_injection_.store(injection, std::memory_order_release);
}

inline std::size_t ThreadPoolBackend::sink_deliveries() const noexcept {
    return sink_.deliveries();
}
inline detail::RequestKey ThreadPoolBackend::sink_last_key() const noexcept {
    return sink_.last_key();
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

template <class Gate> void wait_threadpool_gate_paused(Gate& gate) noexcept {
    std::atomic<bool>& paused = gate.paused;
    bool seen = paused.load(std::memory_order_acquire);
    while (!seen) {
        paused.wait(seen, std::memory_order_acquire);
        seen = paused.load(std::memory_order_acquire);
    }
}

template <class Gate> void wait_threadpool_gate_exited(Gate& gate) noexcept {
    std::atomic<bool>& exited = gate.exited;
    bool seen = exited.load(std::memory_order_acquire);
    while (!seen) {
        exited.wait(seen, std::memory_order_acquire);
        seen = exited.load(std::memory_order_acquire);
    }
}

}

#endif
