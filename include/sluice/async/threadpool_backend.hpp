#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/ready_wait_source.hpp>
#include <sluice/async/detail/reference_ready_sink.hpp>
#include <sluice/async/detail/request_key.hpp>
#include <sluice/async/detail/request_slot.hpp>
#include <sluice/detail/posix_retry.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include <sluice/async/detail/submit_transaction.hpp>
#endif

namespace sluice::async {

struct ThreadPoolConfig {
    std::size_t request_capacity = 64;
    std::size_t worker_count = 4;
};

class ThreadPoolBackend : public AsyncBackend {
  public:
    ThreadPoolBackend() : ThreadPoolBackend(ThreadPoolConfig{}) {}

    explicit ThreadPoolBackend(ThreadPoolConfig config);

    ~ThreadPoolBackend() override;

    ThreadPoolBackend(const ThreadPoolBackend&) = delete;
    ThreadPoolBackend& operator=(const ThreadPoolBackend&) = delete;

    bool supports_request_identity() const noexcept override { return true; }

  private:
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override;
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override;
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override;
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override;

    Result<RequestHandleState> resolve_identity_state(std::uint64_t ctx, std::uint32_t slot,
                                                      std::uint64_t gen) const override;

    std::size_t adopt_context_identity(detail::ContextIdentity) noexcept override;

    bool adopt_request_core(detail::RequestCore* core) noexcept override;

  public:
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

    BackendWaitSource* wait_source() noexcept override { return &ready_wait_; }

    void close_admission();

    std::size_t slot_capacity() const noexcept { return capacity_; }
    std::size_t configured_worker_count() const noexcept { return workers_.size(); }

    std::size_t dispatch_occupancy() const;

    std::size_t dispatch_high_water_mark() const;

    std::size_t active_workers() const;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    std::size_t workers_spawned_for_test() const noexcept;
    std::size_t active_workers_for_test() const;
    std::size_t dispatch_size_for_test() const;
    std::size_t dispatch_high_water_for_test() const;
    std::uint64_t syscall_count_for_test() const noexcept;
    std::optional<BackendWaitToken> try_wait_token_for_test() const noexcept;
    void set_wait_phase_flag_for_test(std::atomic<bool>* flag) noexcept;
    void set_wait_prepark_counter_for_test(std::atomic<int>* counter) noexcept;
    void wait_epoch_changed_for_test(BackendWaitToken observed) noexcept;
    std::optional<detail::RequestKey> request_key_for_test(const Completion<std::size_t>& c) const;
    std::optional<detail::RequestKey> request_key_for_test(const Completion<void>& c) const;
    detail::RequestCore* adopted_core_for_test() const noexcept { return core_; }
    std::size_t publication_pending_size_for_test() const;
    bool event_owed_for_test(std::uint32_t slot) const;

    struct SubmitEntryPauseGate;
    struct PreAcceptCommitPauseGate;
    struct AcceptedPreDispatchPauseGate;
    struct BeforeWorkerDequeuePauseGate;
    struct WorkerClaimedPauseGate;
    struct WorkerOutcomePreTerminalPauseGate;
    struct PublicationEpiloguePauseGate;
    struct ControlWakeFinalReapPauseGate;

    struct DispatchFailureInjection;
    struct SubmitStageFailureInjection;

    void set_submit_entry_pause_gate(SubmitEntryPauseGate* gate) noexcept;
    void set_pre_accept_commit_pause_gate(PreAcceptCommitPauseGate* gate) noexcept;
    void set_accepted_pre_dispatch_pause_gate(AcceptedPreDispatchPauseGate* gate) noexcept;
    void set_before_dequeue_pause_gate(BeforeWorkerDequeuePauseGate* gate) noexcept;
    void set_worker_claimed_pause_gate(WorkerClaimedPauseGate* gate) noexcept;
    void set_worker_outcome_pre_terminal_pause_gate(
        WorkerOutcomePreTerminalPauseGate* gate) noexcept;
    void set_publication_epilogue_pause_gate(PublicationEpiloguePauseGate* gate) noexcept;
    void set_control_wake_final_reap_pause_gate(ControlWakeFinalReapPauseGate* gate) noexcept;
    void set_dispatch_failure_injection(DispatchFailureInjection* injection) noexcept;
    void set_submit_stage_failure_injection(SubmitStageFailureInjection* injection) noexcept;

    static void set_injected_worker_spawn_failure_index(std::size_t index) noexcept;
    static std::size_t injected_worker_spawn_failure_index() noexcept;

    Result<void> register_waiter_key_for_test(detail::RequestKey key, detail::WaiterToken token,
                                              detail::RoutingLease lease);
    Result<detail::RoutingLease> cancel_waiter_key_for_test(detail::RequestKey key);
    detail::PublicCancel cancel_key_for_test(detail::RequestKey key);

    struct WaiterObservation {
        detail::WaiterRegistration registration;
        bool delivery_present;
        detail::WaiterToken token;
        std::uint64_t lease_id;
    };
    std::optional<WaiterObservation> waiter_of_slot_for_test(std::uint32_t slot) const;

    std::size_t sink_deliveries() const noexcept;
    detail::RequestKey sink_last_key() const noexcept;
    bool sink_last_has_waiter() const noexcept;
    detail::WaiterToken sink_last_token() const noexcept;
    std::uint64_t sink_last_lease_id() const noexcept;
#endif

  private:
    struct PreparedBlockingOp {
        detail::OperationKind kind = detail::OperationKind::read;
        int fd = -1;
        const std::byte* buffer = nullptr;
        std::size_t length = 0;
        std::uint64_t offset = 0;
    };

    // Access to a delivery record is serialized by the owning context's
    // access mutex: submit, register/cancel waiter and the publication driver
    // all enter through public context entry points, and workers never touch
    // delivery records.
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

    class BoundedHandleRing {
      public:
        explicit BoundedHandleRing(std::size_t capacity)
            : storage_(capacity), capacity_(capacity) {}
        bool empty() const noexcept { return size_ == 0; }
        std::size_t size() const noexcept { return size_; }
        std::size_t capacity() const noexcept { return capacity_; }
        std::size_t high_water() const noexcept { return high_water_; }

        void push_back(detail::SlotHandle h) noexcept;

        bool pop_front(detail::SlotHandle& out) noexcept;

        bool remove_exact(detail::SlotHandle h) noexcept;

      private:
        std::vector<detail::SlotHandle> storage_;
        std::size_t head_ = 0;
        std::size_t size_ = 0;
        std::size_t high_water_ = 0;
        std::size_t capacity_;
    };

    static Result<void> validate_read(ReadOp op);
    static Result<void> validate_write(WriteOp op);
    static Result<void> validate_sync(SyncDataOp op);
    static Result<void> validate_sync(SyncAllOp op);

    template <class Op> static Result<void> validate_op(const Op& op) noexcept;

    template <class Op>
    static const std::byte* buffer_of(const Op& op) noexcept {
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
            return &ThreadPoolBackend::publish_size_ready;
        } else {
            return &ThreadPoolBackend::publish_void_ready;
        }
    }

    template <class Op, class Comp>
    Result<void> submit_request(Op op, Comp& c, detail::OperationKind kind,
                                detail::RequestOp core_op);

    void dispatch_after_accept(detail::SlotHandle h) noexcept;
    void publish_zero_op_inline(detail::RequestKey id, detail::SlotHandle h) noexcept;
    void publish_one(detail::SlotHandle h);
    void deliver_event(detail::RequestKey key, detail::OperationKind kind);
    detail::PublicCancel cancel_key(detail::RequestKey key);

    static void publish_size_ready(void* completion,
                                   const sluice::detail::IoOutcome& outcome) noexcept;
    static void publish_void_ready(void* completion,
                                   const sluice::detail::IoOutcome& outcome) noexcept;

    static sluice::detail::IoOutcome run_syscall(const PreparedBlockingOp& p) noexcept;

    void worker_loop();

    void signal_ready_progress() noexcept;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    void wait_submit_entry_pause_() noexcept;
    void wait_pre_accept_commit_pause_() noexcept;
    void wait_accepted_pre_dispatch_pause_() noexcept;
    void wait_before_dequeue_pause_() noexcept;
    void wait_worker_claimed_pause_() noexcept;
    void wait_worker_outcome_pre_terminal_pause_() noexcept;
    void wait_publication_epilogue_pause_() noexcept;
    void wait_control_wake_final_reap_pause_() noexcept;

    using SubmitStage = detail::SubmitStage;

    std::optional<IoError> injected_precommit_stage_failure_(SubmitStage stage) noexcept;
#endif

    void tally_canceled() noexcept {
        if (stats_)
            ++stats_->canceled_ops;
    }

    detail::RequestCore* core_ = nullptr;
    std::size_t capacity_ = 0;
    std::vector<PreparedBlockingOp> prepared_ops_;
    std::vector<DeliveryRecord> delivery_;
    detail::ReferenceReadySink sink_;

    mutable std::mutex work_mtx_;
    std::condition_variable work_cv_;
    BoundedHandleRing dispatch_;
    BoundedHandleRing publication_pending_;
    std::size_t active_workers_ = 0;
    bool stopping_ = false;

    detail::ReadyWaitSource ready_wait_;

    std::vector<std::thread> workers_;

    std::atomic<std::uint64_t> syscall_count_{0};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    std::atomic<SubmitEntryPauseGate*> submit_entry_gate_{nullptr};
    std::atomic<PreAcceptCommitPauseGate*> pre_accept_commit_gate_{nullptr};
    std::atomic<AcceptedPreDispatchPauseGate*> accepted_pre_dispatch_gate_{nullptr};
    std::atomic<BeforeWorkerDequeuePauseGate*> before_dequeue_gate_{nullptr};
    std::atomic<WorkerClaimedPauseGate*> worker_claimed_gate_{nullptr};
    std::atomic<WorkerOutcomePreTerminalPauseGate*> worker_outcome_pre_terminal_gate_{nullptr};
    std::atomic<PublicationEpiloguePauseGate*> publication_epilogue_gate_{nullptr};
    std::atomic<ControlWakeFinalReapPauseGate*> control_wake_final_reap_gate_{nullptr};

    std::atomic<DispatchFailureInjection*> dispatch_failure_injection_{nullptr};

    std::atomic<SubmitStageFailureInjection*> submit_stage_failure_injection_{nullptr};
#endif
};

}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include "threadpool_test_seams.hpp"
#endif
