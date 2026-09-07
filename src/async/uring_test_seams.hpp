#pragma once

#include <sluice/async/uring_backend.hpp>

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <cassert>
#include <cstdio>
#include <exception>

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

struct UringAsyncBackend::AfterCommitBeforeEnqueuePauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{false};
};
struct UringAsyncBackend::BeforeDispatchTransferPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{false};

    std::atomic<bool> dispatch_domain_released{false};
};
struct UringAsyncBackend::BeforeCommitBindingPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{false};

    std::atomic<bool> admission_domain_held{false};
};
struct UringAsyncBackend::BeforeAdmissionLockPauseGate {
    std::atomic<bool> paused{false};
    std::atomic<bool> resume{false};
    std::atomic<bool> exited{false};
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

inline Result<void> UringAsyncBackend::validate_write_for_test(WriteOp op) noexcept {
    return validate_write(op);
}

inline std::size_t UringAsyncBackend::backend_ready_count_for_test() const noexcept {
    return arena_.backend_ready_count();
}

inline std::size_t UringAsyncBackend::live_control_sqes_for_test() const noexcept {
    return live_control_sqes_.load(std::memory_order_relaxed);
}

inline std::optional<detail::SlotHandle>
UringAsyncBackend::handle_for_completion_for_test(const void* completion) const noexcept {
    return arena_.resolve_completion(completion);
}

inline std::optional<detail::RequestArena::RequestObservation>
UringAsyncBackend::observe_for_test(detail::SlotHandle h) const noexcept {
    return arena_.observe_for_test(h);
}

inline detail::CancelDisposition
UringAsyncBackend::cancel_handle_for_test(detail::SlotHandle h) noexcept {
    return cancel_handle_(h);
}

inline Result<void> UringAsyncBackend::register_waiter_for_test(Completion<std::size_t>& c,
                                                                detail::WaiterToken token,
                                                                detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}
inline Result<void> UringAsyncBackend::register_waiter_for_test(Completion<void>& c,
                                                                detail::WaiterToken token,
                                                                detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}

inline Result<detail::RoutingLease>
UringAsyncBackend::cancel_waiter_for_test(Completion<std::size_t>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}
inline Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter_for_test(Completion<void>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}

inline Result<void> UringAsyncBackend::register_waiter_handle_for_test(detail::SlotHandle h,
                                                                       detail::WaiterToken token,
                                                                       detail::RoutingLease lease) {
    return arena_.register_waiter(h, token, std::move(lease));
}
inline Result<detail::RoutingLease>
UringAsyncBackend::cancel_waiter_handle_for_test(detail::SlotHandle h) {
    return arena_.cancel_waiter(h);
}

inline std::optional<detail::RequestArena::BorrowSnapshot>
UringAsyncBackend::borrow_for_test(detail::SlotHandle h) const noexcept {
    return arena_.borrow_for_test(h);
}

inline std::optional<detail::RequestArena::WaiterObservation>
UringAsyncBackend::waiter_for_test(detail::SlotHandle h) const noexcept {
    return arena_.waiter_for_test(h);
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

inline void UringAsyncBackend::set_after_commit_before_enqueue_pause_gate(
    AfterCommitBeforeEnqueuePauseGate* gate) noexcept {
    after_commit_before_enqueue_gate_.store(gate, std::memory_order_release);
}
inline void UringAsyncBackend::set_before_dispatch_transfer_pause_gate(
    BeforeDispatchTransferPauseGate* gate) noexcept {
    before_dispatch_transfer_gate_.store(gate, std::memory_order_release);
}
inline void UringAsyncBackend::set_before_commit_binding_pause_gate(
    BeforeCommitBindingPauseGate* gate) noexcept {
    before_commit_binding_gate_.store(gate, std::memory_order_release);
}
inline void UringAsyncBackend::set_before_admission_lock_pause_gate(
    BeforeAdmissionLockPauseGate* gate) noexcept {
    before_admission_lock_gate_.store(gate, std::memory_order_release);
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
inline std::optional<std::size_t> UringAsyncBackend::try_outstanding_for_test() const noexcept {
    return arena_.try_accepted_outstanding();
}
inline std::optional<std::size_t>
UringAsyncBackend::try_backend_ready_count_for_test() const noexcept {
    return arena_.try_backend_ready_count();
}

inline void UringAsyncBackend::set_before_queue_exit_hook_for_test(BeforeQueueExitFn fn,
                                                                   void* ctx) noexcept {
    before_queue_exit_fn_.store(fn, std::memory_order_release);
    before_queue_exit_ctx_.store(ctx, std::memory_order_release);
}

inline void UringAsyncBackend::set_router_scan_mode_for_test(RouterScanModeForTest mode) noexcept {
    router_scan_mode_for_test_ = mode;
}
inline UringAsyncBackend::RouterScanModeForTest
UringAsyncBackend::router_scan_mode_for_test() const noexcept {
    return router_scan_mode_for_test_;
}

inline std::size_t
UringAsyncBackend::find_live_router_cookie_for_test(std::uint64_t cookie) const noexcept {
    return find_live_router_cookie_(cookie);
}
inline const UringAsyncBackend::RouterScanDiagnosticsForTest&
UringAsyncBackend::router_scan_diagnostics_for_test() const noexcept {
    return router_diag_for_test_;
}
inline void UringAsyncBackend::reset_router_scan_diagnostics_for_test() noexcept {
    router_diag_for_test_ = RouterScanDiagnosticsForTest{};
}

[[noreturn]] inline void router_table_fatal_(const char* what) {
    std::fprintf(stderr,
                 "sluice::async::RouterCookieTableForTest: %s "
                 "(impossible internal state - invariant violation)\n",
                 what);
    std::fflush(stderr);
    std::terminate();
}

struct RouterCookieTableForTest {
    struct Slot {
        std::uint64_t cookie = 0;
        std::uint32_t router_index = 0;
        std::uint32_t pad = 0;
    };

    static constexpr std::size_t kMiss = static_cast<std::size_t>(-1);

    std::uint32_t log2_size = 0;
    std::size_t size = 0;
    std::size_t mask = 0;
    std::vector<Slot> slots;

    mutable std::uint64_t last_probes = 0;

    explicit RouterCookieTableForTest(std::size_t request_capacity) {
        std::size_t want = request_capacity * 2;
        if (want < 16)
            want = 16;
        while ((std::size_t{1} << log2_size) < want)
            ++log2_size;
        size = std::size_t{1} << log2_size;
        mask = size - 1;
        slots.assign(size, Slot{});
    }

    static std::size_t hash(std::uint64_t cookie, std::uint32_t log2) noexcept {
        return static_cast<std::size_t>((cookie * std::uint64_t{0x9E3779B97F4A7C15ull}) >>
                                        (64 - log2));
    }

    void insert(std::uint64_t cookie, std::size_t router_index) noexcept {
        if (cookie == 0)
            router_table_fatal_("insert of cookie 0 (outside key domain)");
        std::uint64_t probes = 0;
        std::size_t i = hash(cookie, log2_size);
        for (;;) {
            Slot& s = slots[i];
            if (s.cookie == 0) {
                s.cookie = cookie;
                s.router_index = static_cast<std::uint32_t>(router_index);
                last_probes = probes + 1;
                return;
            }
            if (s.cookie == cookie)
                router_table_fatal_("duplicate insert");
            ++probes;
            if (probes > size)
                router_table_fatal_("insert probe overrun (table full)");
            i = (i + 1) & mask;
        }
    }

    std::size_t lookup(std::uint64_t cookie) const noexcept {
        if (cookie == 0) {
            last_probes = 1;
            return kMiss;
        }
        std::uint64_t probes = 0;
        std::size_t i = hash(cookie, log2_size);
        for (;;) {
            const Slot& s = slots[i];
            if (s.cookie == cookie) {
                last_probes = probes + 1;
                return s.router_index;
            }
            if (s.cookie == 0) {
                last_probes = probes + 1;
                return kMiss;
            }
            ++probes;
            if (probes > size)
                router_table_fatal_("lookup probe overrun (no empty slot)");
            i = (i + 1) & mask;
        }
    }

    void erase(std::uint64_t cookie) noexcept {
        if (cookie == 0)
            router_table_fatal_("erase of cookie 0 (outside key domain)");
        std::uint64_t probes = 0;
        std::size_t i = hash(cookie, log2_size);
        for (;;) {
            if (slots[i].cookie == cookie)
                break;
            if (slots[i].cookie == 0)
                router_table_fatal_("erase of absent cookie");
            ++probes;
            if (probes > size)
                router_table_fatal_("erase probe overrun (no empty slot)");
            i = (i + 1) & mask;
        }
        const std::uint64_t erase_probes = probes + 1;

        std::size_t hole = i;
        std::size_t j = (i + 1) & mask;
        while (slots[j].cookie != 0) {
            const std::size_t home = hash(slots[j].cookie, log2_size);
            const std::size_t d_hole = (hole + size - home) & mask;
            const std::size_t d_j = (j + size - home) & mask;
            if (d_hole <= d_j) {
                slots[hole] = slots[j];
                hole = j;
            }
            j = (j + 1) & mask;
        }
        slots[hole] = Slot{};
        last_probes = erase_probes;
    }

    std::size_t fixed_bytes() const noexcept { return size * sizeof(Slot); }
};

inline void UringAsyncBackend::set_router_fix_mode_for_test(RouterFixModeForTest mode) noexcept {
    if (outstanding() != 0 || live_cookies_.load(std::memory_order_relaxed) != 0 ||
        cookie_free_list_.size() != router_.size()) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: router-fix mode "
                             "switch on a non-quiescent backend (invariant "
                             "violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    router_fix_mode_for_test_ = mode;

    cookie_free_list_.assign(router_.size(), detail::SlotIndex{0});
    if (mode == RouterFixModeForTest::low_placement_forward) {
        for (std::size_t i = 0; i < router_.size(); ++i)
            cookie_free_list_[i] =
                detail::SlotIndex{static_cast<std::uint32_t>(router_.size() - 1 - i)};
    } else {
        for (std::size_t i = 0; i < router_.size(); ++i)
            cookie_free_list_[i] = detail::SlotIndex{static_cast<std::uint32_t>(i)};
    }
}
inline UringAsyncBackend::RouterFixModeForTest
UringAsyncBackend::router_fix_mode_for_test() const noexcept {
    return router_fix_mode_for_test_;
}

inline std::size_t UringAsyncBackend::router_install_cookie_for_test() noexcept {
    if (cookie_free_list_.empty()) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: router exhaustion "
                             "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    detail::SlotIndex router_slot = cookie_free_list_.back();
    cookie_free_list_.pop_back();
    const std::uint64_t op_cookie = allocate_cookie_();
    RouterEntry& route = router_[router_slot.value];
    route = RouterEntry{};
    route.cookie = op_cookie;
    route.in_use = true;
    live_cookies_.fetch_add(1, std::memory_order_relaxed);
    router_table_insert_(op_cookie, router_slot.value);
    return router_slot.value;
}
inline void UringAsyncBackend::router_retire_cookie_for_test(std::size_t router_index) noexcept {
    retire_router_entry_(router_index);
}

inline std::size_t UringAsyncBackend::router_entry_bytes_for_test() noexcept {
    return sizeof(RouterEntry);
}
inline std::size_t UringAsyncBackend::router_table_bytes_for_test() const noexcept {
    return cookie_table_for_test_ ? cookie_table_for_test_->fixed_bytes() : 0;
}

} // namespace sluice::async

#endif
