#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/reference_ready_sink.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/detail/uring_submit.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#if defined(SLUICE_HAS_LIBURING)

#include <sluice/async/detail/uring_wait_source.hpp>
#endif

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <sluice/async/detail/submit_transaction.hpp>
#endif

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
struct io_uring;
#endif

namespace sluice::async {

#if defined(SLUICE_HAS_LIBURING)

struct UringRingState;

#endif

#if defined(SLUICE_HAS_LIBURING)

struct UringConfig {
    std::size_t request_capacity = 64;
    unsigned queue_depth = 64;
};
#endif

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)

struct UringBackendSubmitTestHooks;
#endif

class UringAsyncBackend : public AsyncBackend {
  public:
    explicit UringAsyncBackend(unsigned queue_depth = 64);

#if defined(SLUICE_HAS_LIBURING)

    explicit UringAsyncBackend(UringConfig config);
#endif
#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
    UringAsyncBackend(UringConfig config, UringBackendSubmitTestHooks hooks);
#endif
    ~UringAsyncBackend() override;

    UringAsyncBackend(const UringAsyncBackend&) = delete;
    UringAsyncBackend& operator=(const UringAsyncBackend&) = delete;

  private:
    Result<detail::RequestKey> submit_read(ReadOp op, Completion<std::size_t>* c) override;
    Result<detail::RequestKey> submit_write(WriteOp op, Completion<std::size_t>* c) override;
    Result<detail::RequestKey> submit_sync_data(SyncDataOp op, Completion<void>* c) override;
    Result<detail::RequestKey> submit_sync_all(SyncAllOp op, Completion<void>* c) override;

#if defined(SLUICE_HAS_LIBURING)
  public:
    bool supports_request_identity() const noexcept override { return true; }

  private:
    Result<RequestHandleState> resolve_identity_state(std::uint64_t ctx, std::uint32_t slot,
                                                      std::uint64_t gen) const override;

    detail::PublicCancel cancel_identity(detail::RequestKey key) override;

  public:
#endif

    std::size_t poll() override;
    Result<std::size_t> wait_one() override;

    void cancel(Completion<std::size_t>& c) override;
    void cancel(Completion<void>& c) override;

    Result<void> register_waiter(Completion<std::size_t>& c, detail::WaiterToken token,
                                 detail::RoutingLease lease) override;
    Result<void> register_waiter(Completion<void>& c, detail::WaiterToken token,
                                 detail::RoutingLease lease) override;
    Result<detail::RoutingLease> cancel_waiter(Completion<std::size_t>& c) override;
    Result<detail::RoutingLease> cancel_waiter(Completion<void>& c) override;

    std::size_t outstanding() const noexcept override;

    bool available() const noexcept;

    void close_admission();

#if defined(SLUICE_HAS_LIBURING)

    BackendWaitSource* wait_source() noexcept override {
        return have_ring_ ? wait_source_.get() : nullptr;
    }

    std::size_t slot_capacity() const noexcept override { return capacity_; }
#endif

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)

    std::uint64_t submit_flushes_for_test() const noexcept;
    std::size_t live_cookies_for_test() const noexcept;
    std::uint64_t peek_next_cookie_for_test() const noexcept;
    std::optional<std::uint64_t>
    live_cookie_for_offset_for_test(std::uint64_t offset) const noexcept;

    std::size_t dispatch_size_for_test() const noexcept;
    std::size_t transport_ledger_size_for_test() const noexcept;
    std::size_t sq_ready_for_test() const noexcept;
    std::size_t live_control_sqes_for_test() const noexcept;

    void inject_cqe_for_test(std::uint64_t cookie, int res) noexcept;
    std::optional<detail::RequestKey>
    request_key_for_test(const Completion<std::size_t>& c) const noexcept;
    std::optional<detail::RequestKey>
    request_key_for_test(const Completion<void>& c) const noexcept;
    detail::PublicCancel cancel_key_for_test(detail::RequestKey key) noexcept {
        return cancel_key(key);
    }

    std::size_t sink_deliveries() const noexcept;
    bool sink_last_has_waiter() const noexcept;
    detail::WaiterToken sink_last_token() const noexcept;
    std::uint64_t sink_last_lease_id() const noexcept;

    struct SubmitEntryPauseGate;
    struct PreAcceptCommitPauseGate;
    struct AcceptedPreDispatchPauseGate;
    struct PublicationEpiloguePauseGate;

    void set_submit_entry_pause_gate(SubmitEntryPauseGate* gate) noexcept;
    void set_pre_accept_commit_pause_gate(PreAcceptCommitPauseGate* gate) noexcept;
    void set_accepted_pre_dispatch_pause_gate(AcceptedPreDispatchPauseGate* gate) noexcept;
    void set_publication_epilogue_pause_gate(PublicationEpiloguePauseGate* gate) noexcept;

    struct DispatchFailureInjection;
    struct SubmitStageFailureInjection;

    void set_dispatch_failure_injection(DispatchFailureInjection* injection) noexcept;
    void set_submit_stage_failure_injection(SubmitStageFailureInjection* injection) noexcept;

    struct WaiterObservation {
        detail::WaiterRegistration registration;
        bool delivery_present;
        detail::WaiterToken token;
        std::uint64_t lease_id;
    };
    std::optional<WaiterObservation> waiter_of_slot_for_test(std::uint32_t slot) const noexcept;

    void set_wait_phase_flag_for_test(std::atomic<bool>* flag) noexcept;
    void set_wait_prepark_counter_for_test(std::atomic<int>* counter) noexcept;
    void set_wait_control_wake_final_reap_pause_gate(
        detail::UringWaitSource::ControlWakeFinalReapPauseGate* gate) noexcept;
    void set_wait_before_physical_poll_pause_gate(
        detail::UringWaitSource::BeforePhysicalPollPauseGate* gate) noexcept;
    void set_wait_poll_ring_fd_override_for_test(int fd) noexcept;
    void set_wait_poll_fn_for_test(detail::UringWaitSource::PollFn fn, void* ctx) noexcept;

    bool wait_epoch_changed_for_test(BackendWaitToken observed) noexcept;
    std::optional<BackendWaitToken> try_wait_token_for_test() const noexcept;
#endif

  private:
#if defined(SLUICE_HAS_LIBURING)
    struct ValidatedConfigTag {};
    static UringConfig validate_config_(UringConfig config);
    UringAsyncBackend(UringConfig config, ValidatedConfigTag);

    struct PreparedUringOp {
        detail::OperationKind kind = detail::OperationKind::read;
        int fd = -1;
        const std::byte* buffer = nullptr;
        std::size_t length = 0;
        unsigned native_length = 0;
        std::uint64_t offset = 0;
    };

    struct RouterEntry {
        enum class ControlState : std::uint8_t { none, prepared, submitted };

        std::uint64_t cookie = 0;
        detail::SlotHandle handle{};
        ControlState control_state = ControlState::none;
        // The original operation's outcome has been offered to the core; the
        // entry must still survive until the control CQE when one is live.
        bool terminal_delivered = false;
        bool in_use = false;
    };

    // Access to a delivery record is serialized by the owning context's
    // access mutex: submit, waiter registration and the publication driver
    // all enter through public context entry points, and CQE reaping holds
    // the same access lock.
    struct DeliveryRecord {
        void* completion = nullptr;
        void (*publish)(void* completion, const sluice::detail::IoOutcome&) noexcept = nullptr;
        detail::OperationKind kind = detail::OperationKind::read;
        detail::WaiterRegistration registration = detail::WaiterRegistration::open_no_waiter;
        detail::WaiterToken waiter_token{};
        detail::RoutingLease waiter_lease{};
        bool waiter_delivery_present = false;
        // event_owed pairs with one core control ref on owed_key: the ref is
        // acquired before this flag is set and released after delivery.
        bool event_owed = false;
        detail::RequestKey owed_key{};
    };

    class BoundedDispatchQueue;

    class TransportLedger;

    static Result<void> validate_read(ReadOp op);
    static Result<void> validate_write(WriteOp op);
    static Result<void> validate_sync(SyncDataOp op);
    static Result<void> validate_sync(SyncAllOp op);
    template <class Op> static Result<void> validate_op(const Op& op) noexcept;

    template <class Op> static const std::byte* buffer_of(const Op& op) noexcept {
        if constexpr (std::is_same_v<Op, ReadOp>) {
            return static_cast<const std::byte*>(op.dst);
        } else if constexpr (std::is_same_v<Op, WriteOp>) {
            return op.src;
        } else {
            return nullptr;
        }
    }

    template <class Comp> static auto publish_thunk() noexcept {
        if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
            return &UringAsyncBackend::publish_size_ready;
        } else {
            return &UringAsyncBackend::publish_void_ready;
        }
    }

    template <class Op, class Comp>
    Result<detail::RequestKey> submit_request(Op op, Comp* c, detail::OperationKind kind,
                                              detail::RequestOp core_op);

    static void publish_size_ready(void* completion,
                                   const sluice::detail::IoOutcome& outcome) noexcept;
    static void publish_void_ready(void* completion,
                                   const sluice::detail::IoOutcome& outcome) noexcept;
    static void publish_request_ready(void* completion,
                                      const sluice::detail::IoOutcome& outcome) noexcept;

    void dispatch_after_accept(detail::SlotHandle h) noexcept;
    void publish_zero_op_inline(detail::RequestKey id, detail::SlotHandle h) noexcept;
    void publish_one(detail::SlotHandle h);
    void deliver_event(detail::RequestKey key, detail::OperationKind kind);
    detail::PublicCancel cancel_key(detail::RequestKey key) noexcept;

    bool dispatch_one_locked(detail::SlotHandle h) noexcept;

    int submit_transport_locked() noexcept;

    void account_transport_result_locked(int rc, bool had_pending_transport) noexcept;

    void poison_and_recover_locked(IoError error) noexcept;

    int wait_cqe_without_submit() noexcept;

    std::size_t reap_cqes() noexcept;

    void handle_one_cqe(std::uint64_t user_data, int res) noexcept;

    void finalize_operation_terminal_(RouterEntry& route, std::size_t router_index,
                                      const detail::TerminalResult& terminal) noexcept;

    std::uint64_t allocate_cookie_() noexcept;

    std::size_t find_live_router_index_(detail::SlotHandle h) const noexcept;
    std::size_t find_live_router_cookie_(std::uint64_t cookie) const noexcept;
    void retire_router_entry_(std::size_t router_index) noexcept;

    void issue_running_cancel_locked_(detail::SlotHandle h) noexcept;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    void wait_submit_entry_pause_() noexcept;
    void wait_pre_accept_commit_pause_() noexcept;
    void wait_accepted_pre_dispatch_pause_() noexcept;
    void wait_publication_epilogue_pause_() noexcept;

    using SubmitStage = detail::SubmitStage;

    std::optional<IoError> injected_precommit_stage_failure_(SubmitStage stage) noexcept;
#endif

    void signal_ready_progress() noexcept {
        if (wait_source_) {
            wait_source_->signal_progress();
        }
    }

    std::size_t capacity_ = 0;
    std::vector<PreparedUringOp> prepared_ops_;
    std::vector<DeliveryRecord> delivery_;
    std::vector<RouterEntry> router_;
    std::vector<detail::SlotIndex> cookie_free_list_;
    std::uint64_t next_cookie_ = 1;

    detail::ReferenceReadySink sink_;

    std::unique_ptr<UringRingState> ring_state_;
    std::unique_ptr<TransportLedger> transport_ledger_;

    std::unique_ptr<detail::UringWaitSource> wait_source_;
    bool have_ring_ = false;
    std::optional<IoError> fatal_error_;

    mutable std::mutex dispatch_mtx_;
    std::unique_ptr<BoundedDispatchQueue> dispatch_;
    std::unique_ptr<BoundedDispatchQueue> publication_pending_;

    std::atomic<std::uint64_t> submit_flushes_{0};
    std::atomic<std::size_t> live_cookies_{0};
    std::atomic<std::size_t> live_control_sqes_{0};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    std::atomic<SubmitEntryPauseGate*> submit_entry_gate_{nullptr};
    std::atomic<PreAcceptCommitPauseGate*> pre_accept_commit_gate_{nullptr};
    std::atomic<AcceptedPreDispatchPauseGate*> accepted_pre_dispatch_gate_{nullptr};
    std::atomic<PublicationEpiloguePauseGate*> publication_epilogue_gate_{nullptr};

    std::atomic<DispatchFailureInjection*> dispatch_failure_injection_{nullptr};
    std::atomic<SubmitStageFailureInjection*> submit_stage_failure_injection_{nullptr};
#endif
#endif

    bool available_ = false;
};

}

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include "uring_test_seams.hpp"
#endif
