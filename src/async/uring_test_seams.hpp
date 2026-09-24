#pragma once

#include <sluice/async/uring_backend.hpp>

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <optional>

namespace sluice::async {

struct UringBackendSubmitTestHooks {
    using SubmitFn = int (*)(void*, ::io_uring*) noexcept;
    using SubmitAndWaitFn = int (*)(void*, ::io_uring*, unsigned) noexcept;
    using BeforePoisonWaitFn = void (*)(void*) noexcept;

    void* context = nullptr;
    SubmitFn submit = nullptr;
    SubmitAndWaitFn submit_and_wait = nullptr;
    BeforePoisonWaitFn before_poison_wait = nullptr;
};

struct UringAsyncBackend::SubmitEntryPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct UringAsyncBackend::PreAcceptCommitPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct UringAsyncBackend::AcceptedPreDispatchPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct UringAsyncBackend::PublicationEpiloguePauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{true};
};

struct UringAsyncBackend::DispatchFailureInjection {
    std::atomic<bool> armed{false};
    std::atomic<std::size_t> fired{0};
};

struct UringAsyncBackend::SubmitStageFailureInjection {
    std::atomic<bool> fail_reserve{false};
    std::atomic<bool> fail_prepare{false};
    std::atomic<bool> fail_commit{false};
    std::atomic<std::size_t> reserve_fired{0};
    std::atomic<std::size_t> prepare_fired{0};
    std::atomic<std::size_t> commit_fired{0};
};

inline std::uint64_t UringAsyncBackend::submit_flushes_for_test() const noexcept {
    return submit_flushes_.load(std::memory_order_relaxed);
}

inline std::size_t UringAsyncBackend::live_cookies_for_test() const noexcept {
    return live_cookies_.load(std::memory_order_relaxed);
}

inline void UringAsyncBackend::inject_cqe_for_test(std::uint64_t cookie, int res) noexcept {
    handle_one_cqe(cookie, res);
}

inline std::uint64_t UringAsyncBackend::peek_next_cookie_for_test() const noexcept {
    return next_cookie_;
}

inline std::optional<std::uint64_t>
UringAsyncBackend::live_cookie_for_offset_for_test(std::uint64_t offset) const noexcept {
    for (const auto& entry : router_) {
        if (entry.in_use && prepared_ops_[entry.handle.slot.value].offset == offset)
            return entry.cookie;
    }
    return std::nullopt;
}

inline std::optional<detail::RequestKey>
UringAsyncBackend::request_key_for_test(const Completion<std::size_t>& c) const noexcept {
    return core_binding(c);
}

inline std::optional<detail::RequestKey>
UringAsyncBackend::request_key_for_test(const Completion<void>& c) const noexcept {
    return core_binding(c);
}

inline std::size_t UringAsyncBackend::sink_deliveries() const noexcept {
    return sink_.deliveries();
}
inline bool UringAsyncBackend::sink_last_has_waiter() const noexcept {
    return sink_.last_has_waiter();
}
inline detail::WaiterToken UringAsyncBackend::sink_last_token() const noexcept {
    return sink_.last_token();
}
inline std::uint64_t UringAsyncBackend::sink_last_lease_id() const noexcept {
    return sink_.last_lease_id();
}

inline std::optional<UringAsyncBackend::WaiterObservation>
UringAsyncBackend::waiter_of_slot_for_test(std::uint32_t slot) const noexcept {
    const DeliveryRecord& record = delivery_[slot];
    return WaiterObservation{record.registration, record.waiter_delivery_present,
                             record.waiter_token, record.waiter_lease.id()};
}

inline void UringAsyncBackend::set_submit_entry_pause_gate(SubmitEntryPauseGate* gate) noexcept {
    submit_entry_gate_.store(gate, std::memory_order_release);
}
inline void
UringAsyncBackend::set_pre_accept_commit_pause_gate(PreAcceptCommitPauseGate* gate) noexcept {
    pre_accept_commit_gate_.store(gate, std::memory_order_release);
}
inline void
UringAsyncBackend::set_accepted_pre_dispatch_pause_gate(AcceptedPreDispatchPauseGate* gate) noexcept {
    accepted_pre_dispatch_gate_.store(gate, std::memory_order_release);
}
inline void
UringAsyncBackend::set_publication_epilogue_pause_gate(PublicationEpiloguePauseGate* gate) noexcept {
    publication_epilogue_gate_.store(gate, std::memory_order_release);
}

inline void
UringAsyncBackend::set_dispatch_failure_injection(DispatchFailureInjection* injection) noexcept {
    dispatch_failure_injection_.store(injection, std::memory_order_release);
}
inline void UringAsyncBackend::set_submit_stage_failure_injection(
    SubmitStageFailureInjection* injection) noexcept {
    submit_stage_failure_injection_.store(injection, std::memory_order_release);
}

inline void UringAsyncBackend::set_wait_phase_flag_for_test(std::atomic<bool>* flag) noexcept {
    if (wait_source_) {
        wait_source_->set_wait_phase_flag(flag);
    }
}

inline void
UringAsyncBackend::set_wait_prepark_counter_for_test(std::atomic<int>* counter) noexcept {
    if (wait_source_) {
        wait_source_->set_wait_prepark_counter(counter);
    }
}

inline void UringAsyncBackend::set_wait_control_wake_final_reap_pause_gate(
    detail::UringWaitSource::ControlWakeFinalReapPauseGate* gate) noexcept {
    if (wait_source_) {
        wait_source_->set_control_wake_final_reap_pause_gate(gate);
    }
}

inline void UringAsyncBackend::set_wait_before_physical_poll_pause_gate(
    detail::UringWaitSource::BeforePhysicalPollPauseGate* gate) noexcept {
    if (wait_source_) {
        wait_source_->set_before_physical_poll_pause_gate(gate);
    }
}

inline void UringAsyncBackend::set_wait_poll_ring_fd_override_for_test(int fd) noexcept {
    if (wait_source_) {
        wait_source_->set_poll_ring_fd_override_for_test(fd);
    }
}

inline void UringAsyncBackend::set_wait_poll_fn_for_test(detail::UringWaitSource::PollFn fn,
                                                         void* ctx) noexcept {
    if (wait_source_) {
        wait_source_->set_poll_fn_for_test(fn, ctx);
    }
}

inline bool UringAsyncBackend::wait_epoch_changed_for_test(BackendWaitToken observed) noexcept {
    if (!wait_source_)
        return false;
    wait_source_->wait_epoch_changed(observed);
    return true;
}

inline std::optional<BackendWaitToken> UringAsyncBackend::try_wait_token_for_test() const noexcept {
    if (!wait_source_)
        return std::nullopt;
    return wait_source_->try_snapshot();
}

}

#endif
