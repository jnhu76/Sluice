#pragma once

#include <sluice/async/detail/async_backend.hpp>
#include <sluice/async/detail/reference_ready_sink.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/detail/uring_submit.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <vector>

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
struct io_uring;
#endif

// UAPI forward declaration; the full definition lives either in
// <linux/statx.h> or in the guarded mirror inside uring_backend.cpp.
#if defined(SLUICE_HAS_LIBURING)
struct statx;
#endif

namespace sluice::async {

#if defined(SLUICE_HAS_LIBURING)
struct UringRingState;
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
    Result<detail::RequestKey> submit_read(ReadOp op) override;
    Result<detail::RequestKey> submit_write(WriteOp op) override;
    Result<detail::RequestKey> submit_sync_data(SyncDataOp op) override;
    Result<detail::RequestKey> submit_sync_all(SyncAllOp op) override;
    Result<detail::RequestKey> submit_file_info(FileInfoOp op) override;
    Result<detail::RequestKey> submit_size(SizeOp op) override;

#if defined(SLUICE_HAS_LIBURING)
  public:
    bool signals_physical_progress() const noexcept override { return have_ring_; }

  private:
    ProgressPass poll_progress() override;

    detail::PublicCancel cancel_identity(detail::RequestKey key) override;

  public:
#endif

    std::size_t poll() override;

    std::size_t outstanding() const noexcept override;

    bool available() const noexcept;

    bool internal_work_retired() const noexcept override;

    void retire_execution_resources() noexcept override;

    void close_admission();

#if defined(SLUICE_HAS_LIBURING)

    std::size_t slot_capacity() const noexcept override { return capacity_; }
#endif

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)

    std::uint64_t submit_flushes_for_test() const noexcept;
    std::size_t live_cookies_for_test() const noexcept;
    std::uint64_t peek_next_cookie_for_test() const noexcept;
    std::size_t owed_count_for_test() const noexcept;
    bool owed_scan_for_test() const noexcept;
    std::optional<std::uint64_t>
    live_cookie_for_offset_for_test(std::uint64_t offset) const noexcept;

    struct PendingControlSnapshot {
        std::size_t derived_count = 0;
        std::size_t authoritative_pending = 0;
        std::size_t live_routes = 0;
        std::size_t prepared_controls = 0;
        std::size_t submitted_controls = 0;
    };

    PendingControlSnapshot pending_control_snapshot_for_test() const noexcept;

    std::size_t dispatch_size_for_test() const noexcept;
    std::size_t transport_ledger_size_for_test() const noexcept;
    std::size_t sq_ready_for_test() const noexcept;
    std::size_t live_control_sqes_for_test() const noexcept;
    unsigned eventfd_registrations_for_test() const noexcept;
    unsigned eventfd_unregistrations_for_test() const noexcept;

    void inject_cqe_for_test(std::uint64_t cookie, int res) noexcept;
    detail::PublicCancel cancel_key_for_test(detail::RequestKey key) noexcept {
        return cancel_key(key);
    }

    static void set_injected_statx_probe_failure(bool value) noexcept;
    static bool injected_statx_probe_failure() noexcept;
    static void set_injected_opcode_probe_failure(bool value) noexcept;
    static bool injected_opcode_probe_failure() noexcept;

    std::size_t sink_deliveries() const noexcept;
    detail::RequestKey sink_last_key() const noexcept;

    struct SubmitEntryPauseGate;
    struct PreAcceptCommitPauseGate;
    struct AcceptedPreDispatchPauseGate;
    struct PublicationEpiloguePauseGate;
    struct DeliverySweepEpiloguePauseGate;

    void set_submit_entry_pause_gate(SubmitEntryPauseGate* gate) noexcept;
    void set_pre_accept_commit_pause_gate(PreAcceptCommitPauseGate* gate) noexcept;
    void set_accepted_pre_dispatch_pause_gate(AcceptedPreDispatchPauseGate* gate) noexcept;
    void set_publication_epilogue_pause_gate(PublicationEpiloguePauseGate* gate) noexcept;
    void set_delivery_sweep_epilogue_pause_gate(DeliverySweepEpiloguePauseGate* gate) noexcept;

    struct DispatchFailureInjection;
    struct SubmitStageFailureInjection;

    void set_dispatch_failure_injection(DispatchFailureInjection* injection) noexcept;
    void set_submit_stage_failure_injection(SubmitStageFailureInjection* injection) noexcept;
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
        struct ::statx* statx_buffer = nullptr;
    };

    bool probe_statx_support_() noexcept;
    bool probe_required_opcodes_() noexcept;
    std::size_t reconcile_ledger_with_kernel_locked_() noexcept;
    void mark_consumed_control_locked_(std::uint64_t cookie, detail::RequestReservation handle) noexcept;

    bool progress_port_attached() noexcept override;

    bool has_immediate_physical_work() const noexcept override;

    struct RouterEntry {
        enum class ControlState : std::uint8_t { none, prepared, submitted };

        std::uint64_t cookie = 0;
        detail::RequestReservation handle{};
        ControlState control_state = ControlState::none;
        // A cancel intent whose control SQE could not be allocated yet; serviced
        // by later progress passes so the intent is never silently discarded.
        bool control_pending = false;
        // The original operation's outcome has been offered to the core; the
        // entry must still survive until the control CQE when one is live.
        bool terminal_delivered = false;
        bool in_use = false;
    };

    // Delivery records hand off under dispatch_mtx_: submit initializes a
    // slot and marks its owed event under the lock, the progress owner
    // consumes under it, and publish_one's reads ride the
    // publication_pending_ queue transitions already taken under it.
    // on_ready callbacks never run under the lock.
    struct DeliveryRecord {
        detail::OperationKind kind = detail::OperationKind::read;
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
    static Result<void> validate_file_info(FileInfoOp op);
    static Result<void> validate_size(SizeOp op);
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

    template <class Op>
    Result<detail::RequestKey> submit_request(Op op, detail::OperationKind kind,
                                              detail::RequestOp core_op);

    void dispatch_after_accept(detail::RequestReservation h) noexcept;
    void publish_zero_op_inline(detail::RequestKey id, detail::RequestReservation h) noexcept;
    void publish_one(detail::RequestReservation h);
    void deliver_event(detail::RequestKey key, detail::OperationKind kind);
    detail::PublicCancel cancel_key(detail::RequestKey key) noexcept;

    bool dispatch_one_locked(detail::RequestReservation h) noexcept;

    // Reservations pushed after poison never took an SQE (dispatch refuses
    // under fatal_error_ before any ring access), so they are provably
    // kernel-invisible and must retire through the failure terminal instead
    // of holding a dispatch-queue seat no later pass can drain.
    void retire_undispatched_after_poison_locked_() noexcept;

    // Accepted work that has not reached the kernel: undispatched entries or
    // prepared transport the last submit attempt did not consume. Poisoned
    // (retired) ledger entries are not retryable work.
    bool dispatch_retry_remains_locked_() const noexcept;

    int submit_transport_locked() noexcept;

    void account_transport_result_locked(int rc, bool had_pending_transport) noexcept;

    void poison_and_recover_locked(IoError error) noexcept;

    void reap_cqes() noexcept;

    bool any_event_owed_locked_() const noexcept;

    bool owed_visible_locked_() const noexcept {
#if defined(SLUICE_URING_MUTANT_OWED_EMPTY_VERDICT_ALWAYS)
        return false;
#else
        return owed_count_ != 0;
#endif
    }

    void handle_one_cqe(std::uint64_t user_data, int res) noexcept;

    void finalize_operation_terminal_(RouterEntry& route, std::size_t router_index,
                                      const sluice::detail::IoOutcome& terminal,
                                      const sluice::FileInfo* metadata) noexcept;

    std::uint64_t allocate_cookie_() noexcept;

    std::size_t find_live_router_index_(detail::RequestReservation h) const noexcept;
    std::size_t find_live_router_cookie_(std::uint64_t cookie) const noexcept;
    void retire_router_entry_(std::size_t router_index) noexcept;

    void issue_running_cancel_locked_(detail::RequestReservation h) noexcept;
    void service_pending_controls_locked_() noexcept;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    void wait_submit_entry_pause_() noexcept;
    void wait_pre_accept_commit_pause_() noexcept;
    void wait_accepted_pre_dispatch_pause_() noexcept;
    void wait_publication_epilogue_pause_() noexcept;
    void wait_delivery_sweep_epilogue_pause_() noexcept;

    using SubmitStage = detail::SubmitStage;

    std::optional<IoError> injected_precommit_stage_failure_(SubmitStage stage) noexcept;
#endif

    void signal_ready_progress() noexcept {
        progress_port_.signal();
    }

    std::size_t capacity_ = 0;
    std::vector<PreparedUringOp> prepared_ops_;
    std::vector<DeliveryRecord> delivery_;
    std::size_t owed_count_ = 0;
    std::size_t pending_control_count_ = 0;
    std::vector<RouterEntry> router_;
    std::vector<detail::SlotIndex> cookie_free_list_;
    std::vector<struct ::statx> statx_buffers_;
    std::uint64_t next_cookie_ = 1;

    detail::ReferenceReadySink sink_;

    std::unique_ptr<UringRingState> ring_state_;
    std::unique_ptr<TransportLedger> transport_ledger_;

    bool have_ring_ = false;
    bool eventfd_registered_ = false;
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
    std::atomic<DeliverySweepEpiloguePauseGate*> delivery_sweep_epilogue_gate_{nullptr};

    std::atomic<DispatchFailureInjection*> dispatch_failure_injection_{nullptr};
    std::atomic<SubmitStageFailureInjection*> submit_stage_failure_injection_{nullptr};

    std::atomic<unsigned> eventfd_registrations_{0};
    std::atomic<unsigned> eventfd_unregistrations_{0};
#endif
#endif

    bool available_ = false;
};

}

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include "uring_test_seams.hpp"
#endif
