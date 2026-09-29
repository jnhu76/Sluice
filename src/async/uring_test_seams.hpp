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
inline detail::RequestKey UringAsyncBackend::sink_last_key() const noexcept {
    return sink_.last_key();
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

}

#endif
