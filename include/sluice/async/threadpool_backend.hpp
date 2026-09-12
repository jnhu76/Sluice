#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/ready_wait_source.hpp>
#include <sluice/async/detail/reference_ready_sink.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/async/detail/submit_transaction.hpp>
#include <sluice/detail/posix_retry.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

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
    // AsyncBackend::submit_* stay private end-to-end: only AsyncIoContext may
    // enter them, so the access-legality matrix cannot be bypassed by calling
    // a backend directly.
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override;
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override;
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override;
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override;

    Result<RequestHandleState> resolve_identity_state(std::uint64_t ctx, std::uint32_t slot,
                                                      std::uint64_t gen) const override {
        return arena_.identity_handle_state(detail::SlotIndex{slot}, detail::Generation{gen},
                                            detail::ContextIdentity{ctx});
    }

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

    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_capacity_rejections() const noexcept { return arena_.capacity_rejections(); }
    std::size_t configured_worker_count() const noexcept { return workers_.size(); }

    std::size_t arena_high_water_mark() const noexcept { return arena_.high_water_mark(); }

    std::size_t dispatch_occupancy() const;

    std::size_t dispatch_high_water_mark() const;

    std::size_t active_workers() const;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    std::size_t workers_spawned_for_test() const noexcept;
    std::size_t active_workers_for_test() const;
    std::size_t dispatch_size_for_test() const;
    std::size_t dispatch_high_water_for_test() const;
    std::uint64_t syscall_count_for_test() const noexcept;
    std::size_t backend_ready_count_for_test() const noexcept;
    std::optional<BackendWaitToken> try_wait_token_for_test() const noexcept;
    std::optional<std::size_t> try_outstanding_for_test() const noexcept;
    std::optional<std::size_t> try_backend_ready_count_for_test() const noexcept;
    void set_wait_phase_flag_for_test(std::atomic<bool>* flag) noexcept;
    void set_wait_prepark_counter_for_test(std::atomic<int>* counter) noexcept;
    void wait_epoch_changed_for_test(BackendWaitToken observed) noexcept;
    std::optional<detail::SlotHandle>
    handle_for_completion_for_test(const void* completion) const noexcept;
    std::optional<detail::RequestArena::RequestObservation>
    observe_for_test(detail::SlotHandle h) const noexcept;
    detail::CancelDisposition cancel_handle_for_test(detail::SlotHandle h) noexcept;

    struct AfterArenaEnqueueBeforeDispatchPushPauseGate;
    struct BeforeWorkerDequeuePauseGate;
    struct PostResumePrePopHoldGate;
    struct WorkerRunningPauseGate;
    struct TerminalPublicationPauseGate;
    struct ControlWakeFinalReapPauseGate;
    struct BeforeEnqueueLockPauseGate;
    struct BeforeAdmissionLockPauseGate;
    struct BeforeCommitBindingPauseGate;

    struct DispatchFailureInjection;
    struct SubmitStageFailureInjection;

    void set_after_enqueue_before_push_pause_gate(
        AfterArenaEnqueueBeforeDispatchPushPauseGate* gate) noexcept;
    void set_before_dequeue_pause_gate(BeforeWorkerDequeuePauseGate* gate) noexcept;
    void set_post_resume_pre_pop_hold_gate(PostResumePrePopHoldGate* gate) noexcept;
    void set_running_pause_gate(WorkerRunningPauseGate* gate) noexcept;
    void set_terminal_publication_pause_gate(TerminalPublicationPauseGate* gate) noexcept;
    void set_control_wake_final_reap_pause_gate(ControlWakeFinalReapPauseGate* gate) noexcept;
    void set_before_enqueue_lock_pause_gate(BeforeEnqueueLockPauseGate* gate) noexcept;
    void set_before_admission_lock_pause_gate(BeforeAdmissionLockPauseGate* gate) noexcept;
    void set_before_commit_binding_pause_gate(BeforeCommitBindingPauseGate* gate) noexcept;
    void set_dispatch_failure_injection(DispatchFailureInjection* injection) noexcept;
    void set_submit_stage_failure_injection(SubmitStageFailureInjection* injection) noexcept;

    static void set_injected_worker_spawn_failure_index(std::size_t index) noexcept;
    static std::size_t injected_worker_spawn_failure_index() noexcept;

    Result<void> register_waiter_for_test(Completion<std::size_t>& c, detail::WaiterToken token,
                                          detail::RoutingLease lease);
    Result<void> register_waiter_for_test(Completion<void>& c, detail::WaiterToken token,
                                          detail::RoutingLease lease);
    Result<detail::RoutingLease> cancel_waiter_for_test(Completion<std::size_t>& c);
    Result<detail::RoutingLease> cancel_waiter_for_test(Completion<void>& c);
    Result<void> register_waiter_handle_for_test(detail::SlotHandle h, detail::WaiterToken token,
                                                 detail::RoutingLease lease);
    Result<detail::RoutingLease> cancel_waiter_handle_for_test(detail::SlotHandle h);
    std::optional<detail::RequestArena::BorrowSnapshot>
    borrow_for_test(detail::SlotHandle h) const noexcept;
    std::optional<detail::RequestArena::WaiterObservation>
    waiter_for_test(detail::SlotHandle h) const noexcept;

    std::size_t sink_deliveries() const noexcept;
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

    class BoundedDispatchQueue {
      public:
        explicit BoundedDispatchQueue(std::size_t capacity)
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

    static std::uint64_t next_backend_id() noexcept {
        static std::atomic<std::uint64_t> id{0x54500000u};
        return ++id;
    }

    static Result<void> validate_read(ReadOp op);
    static Result<void> validate_write(WriteOp op);
    static Result<void> validate_sync(SyncDataOp op);
    static Result<void> validate_sync(SyncAllOp op);

    template <class Op> static Result<void> validate_op(const Op& op) noexcept;

    template <class Op>
    Result<void> submit_size(Op op, Completion<std::size_t>& c, detail::OperationKind kind);
    template <class Op>
    Result<void> submit_void(Op op, Completion<void>& c, detail::OperationKind kind);

    template <class Op, class Comp> struct SubmitPolicy {
        using completion_type = Comp;
        using op_type = Op;

        SubmitPolicy(ThreadPoolBackend& self, detail::OperationKind kind) noexcept
            : self_(self), kind_(kind) {}

        detail::OperationKind kind() const noexcept { return kind_; }
        static detail::BorrowMetadata borrow(const Op& op) noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                return borrow_of(op);
            } else {
                return detail::BorrowMetadata{op.file.fd, nullptr, 0};
            }
        }
        static std::uint64_t requested_bytes(const Op& op) noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                return op.len;
            } else {
                return 0;
            }
        }
        static auto publish_thunk() noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                return &ThreadPoolBackend::publish_size_ready;
            } else {
                return &ThreadPoolBackend::publish_void_ready;
            }
        }

        static bool begin_binding(Comp& c) noexcept { return ThreadPoolBackend::begin_binding(c); }
        static void install_binding(Comp& c, detail::RequestArena* arena,
                                    detail::SlotHandle h) noexcept {
            ThreadPoolBackend::install_binding(c, arena, h);
        }
        static void commit_binding(Comp& c) noexcept { ThreadPoolBackend::commit_binding(c); }
        static void rollback_binding(Comp& c) noexcept {
            ThreadPoolBackend::rollback_binding_before_accept(c);
        }

        Result<void> stage0_precheck() const noexcept { return {}; }
        Result<void> validate(const Op& op) const noexcept { return self_.validate_op(op); }
        void write_scratch(detail::SlotHandle h, const Op& op) const noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                // Zero-length ops never perform data I/O; normalize the offset
                // so an unrepresentable offset cannot fail the lowering.
                const std::uint64_t off = op.len == 0 ? 0 : op.offset;
                self_.prepared_ops_[h.slot.value] = PreparedBlockingOp{
                    kind_, op.file.fd, static_cast<const std::byte*>(borrow_of(op).address), op.len,
                    off};
            } else {
                self_.prepared_ops_[h.slot.value] = PreparedBlockingOp{
                    kind_, op.file.fd, nullptr, std::size_t{0}, std::uint64_t{0}};
            }
        }
        void pause_before_commit_binding() noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

            self_.wait_before_commit_binding_pause_();
#endif
        }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        std::optional<IoError>
        injected_precommit_stage_failure(detail::SubmitStage stage) const noexcept {
            return self_.injected_precommit_stage_failure_(stage);
        }
#endif

      private:
        ThreadPoolBackend& self_;
        detail::OperationKind kind_;
    };

    void enqueue_after_commit(detail::SlotHandle h) noexcept;

    template <class Op> static detail::BorrowMetadata borrow_of(const Op& op) noexcept {
        if constexpr (std::is_same_v<Op, ReadOp>) {
            return {op.file.fd, op.dst, op.len};
        } else {
            return {op.file.fd, op.src, op.len};
        }
    }

    static void publish_size_ready(void* completion, const detail::TerminalResult& t) noexcept;
    static void publish_void_ready(void* completion, const detail::TerminalResult& t) noexcept;
    static Result<std::size_t> terminal_to_size(const detail::TerminalResult& t) noexcept;
    static Result<void> terminal_to_void(const detail::TerminalResult& t) noexcept;

    static detail::TerminalResult run_syscall(const PreparedBlockingOp& p) noexcept;

    void worker_loop();

    void signal_ready_progress() noexcept;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    void wait_after_enqueue_before_push_pause_(bool inside_work_mtx) noexcept;

    std::uint64_t wait_before_dequeue_pause_() noexcept;

    void ack_dequeue_gate_generation_(std::uint64_t generation) noexcept;

    void wait_post_resume_pre_pop_hold_() noexcept;
    void wait_running_pause_() noexcept;
    void wait_terminal_publication_pause_() noexcept;
    void wait_before_enqueue_lock_pause_() noexcept;
    void wait_control_wake_final_reap_pause_() noexcept;
    void wait_before_admission_lock_pause_() noexcept;
    void wait_before_commit_binding_pause_() noexcept;

    using SubmitStage = detail::SubmitStage;

    std::optional<IoError> injected_precommit_stage_failure_(SubmitStage stage) noexcept;
#endif

    void tally_canceled() noexcept {
        if (stats_)
            ++stats_->canceled_ops;
    }

    detail::RequestArena arena_;
    detail::ReferenceReadySink sink_;
    std::vector<PreparedBlockingOp> prepared_ops_;

    mutable std::mutex admission_mtx_;

    mutable std::mutex work_mtx_;
    std::condition_variable work_cv_;
    BoundedDispatchQueue dispatch_;
    std::size_t active_workers_ = 0;
    bool stopping_ = false;

    detail::ReadyWaitSource ready_wait_;

    std::vector<std::thread> workers_;

    std::atomic<std::uint64_t> syscall_count_{0};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    std::atomic<AfterArenaEnqueueBeforeDispatchPushPauseGate*> after_enqueue_before_push_gate_{
        nullptr};
    std::atomic<BeforeWorkerDequeuePauseGate*> before_dequeue_gate_{nullptr};
    std::atomic<PostResumePrePopHoldGate*> post_resume_pre_pop_hold_gate_{nullptr};
    std::atomic<WorkerRunningPauseGate*> running_gate_{nullptr};
    std::atomic<TerminalPublicationPauseGate*> terminal_publication_gate_{nullptr};
    std::atomic<BeforeEnqueueLockPauseGate*> before_enqueue_lock_gate_{nullptr};

    std::atomic<ControlWakeFinalReapPauseGate*> control_wake_final_reap_gate_{nullptr};

    std::atomic<BeforeAdmissionLockPauseGate*> before_admission_lock_gate_{nullptr};
    std::atomic<BeforeCommitBindingPauseGate*> before_commit_binding_gate_{nullptr};

    std::atomic<DispatchFailureInjection*> dispatch_failure_injection_{nullptr};

    std::atomic<SubmitStageFailureInjection*> submit_stage_failure_injection_{nullptr};
#endif
};

} // namespace sluice::async

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include "threadpool_test_seams.hpp"
#endif
