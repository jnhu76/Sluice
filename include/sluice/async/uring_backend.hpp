#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/reference_ready_sink.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/async/detail/submit_transaction.hpp>
#include <sluice/detail/io_validation.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#if defined(SLUICE_HAS_LIBURING)

#include <sluice/async/detail/uring_wait_source.hpp>
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

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

struct RouterCookieTableForTest;
#endif
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
    // AsyncBackend::submit_* stay private end-to-end: only AsyncIoContext may
    // enter them, so the access-legality matrix cannot be bypassed by calling
    // a backend directly.
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override;
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override;
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override;
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override;

#if defined(SLUICE_HAS_LIBURING)
  public:
    bool supports_request_identity() const noexcept override { return true; }

  private:
    Result<RequestHandleState> resolve_identity_state(std::uint64_t ctx, std::uint32_t slot,
                                                      std::uint64_t gen) const override {
        return arena_.identity_handle_state(detail::SlotIndex{slot}, detail::Generation{gen},
                                            detail::ContextIdentity{ctx});
    }

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

    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_accepted_outstanding() const noexcept {
        return arena_.accepted_outstanding();
    }
    std::size_t arena_capacity_rejections() const noexcept { return arena_.capacity_rejections(); }
    std::size_t configured_queue_depth() const noexcept { return queue_depth_; }
#endif

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)

    std::uint64_t submit_flushes_for_test() const noexcept;
    std::size_t live_cookies_for_test() const noexcept;
    void inject_cqe_for_test(std::uint64_t cookie, int res) noexcept;
    std::uint64_t peek_next_cookie_for_test() const noexcept;
    std::optional<std::uint64_t>
    live_cookie_for_offset_for_test(std::uint64_t offset) const noexcept;
    static Result<void> validate_write_for_test(WriteOp op) noexcept;

    std::size_t dispatch_size_for_test() const noexcept;
    std::size_t transport_ledger_size_for_test() const noexcept;
    std::size_t sq_ready_for_test() const noexcept;
    std::size_t live_control_entries_for_test() const noexcept;
    std::size_t backend_ready_count_for_test() const noexcept;
    std::size_t live_control_sqes_for_test() const noexcept;

    std::optional<detail::SlotHandle>
    handle_for_completion_for_test(const void* completion) const noexcept;
    std::optional<detail::RequestArena::RequestObservation>
    observe_for_test(detail::SlotHandle h) const noexcept;
    detail::CancelDisposition cancel_handle_for_test(detail::SlotHandle h) noexcept;
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

    struct AfterCommitBeforeEnqueuePauseGate;
    struct BeforeDispatchTransferPauseGate;
    struct BeforeCommitBindingPauseGate;
    struct BeforeAdmissionLockPauseGate;

    void
    set_after_commit_before_enqueue_pause_gate(AfterCommitBeforeEnqueuePauseGate* gate) noexcept;
    void set_before_dispatch_transfer_pause_gate(BeforeDispatchTransferPauseGate* gate) noexcept;
    void set_before_commit_binding_pause_gate(BeforeCommitBindingPauseGate* gate) noexcept;
    void set_before_admission_lock_pause_gate(BeforeAdmissionLockPauseGate* gate) noexcept;

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
    std::optional<std::size_t> try_outstanding_for_test() const noexcept;
    std::optional<std::size_t> try_backend_ready_count_for_test() const noexcept;

    using BeforeQueueExitFn = void (*)(void*);
    void set_before_queue_exit_hook_for_test(BeforeQueueExitFn fn, void* ctx) noexcept;

    enum class RouterScanModeForTest : std::uint8_t {
        reverse_production,
        forward_ablation,

    };

    enum class RouterLookupKindForTest : std::uint8_t {
        operation_cqe,
        control_cqe,
        transport,
    };
    struct RouterScanDiagnosticsForTest {
        std::uint64_t operation_cookie_lookup_calls = 0;
        std::uint64_t control_cookie_lookup_calls = 0;
        std::uint64_t transport_cookie_lookup_calls = 0;
        std::uint64_t operation_lookup_iterations_total = 0;
        std::uint64_t operation_lookup_iterations_max = 0;
        std::uint64_t control_lookup_iterations_total = 0;
        std::uint64_t control_lookup_iterations_max = 0;
        std::uint64_t transport_lookup_iterations_total = 0;
        std::uint64_t transport_lookup_iterations_max = 0;

        std::uint64_t lookup_calls = 0;
        std::uint64_t lookup_hits = 0;
        std::uint64_t lookup_misses = 0;
        std::uint64_t matched_router_index_sum = 0;
        std::uint64_t matched_router_index_max = 0;
        std::uint64_t reverse_mode_calls = 0;
        std::uint64_t last_call_iterations = 0;

        std::uint64_t table_insert_calls = 0;
        std::uint64_t table_insert_probes_total = 0;
        std::uint64_t table_insert_probes_max = 0;
        std::uint64_t table_lookup_probes_total = 0;
        std::uint64_t table_lookup_probes_max = 0;
        std::uint64_t table_erase_calls = 0;
        std::uint64_t table_erase_probes_total = 0;
        std::uint64_t table_erase_probes_max = 0;
    };
    void set_router_scan_mode_for_test(RouterScanModeForTest mode) noexcept;
    RouterScanModeForTest router_scan_mode_for_test() const noexcept;

    std::size_t find_live_router_cookie_for_test(std::uint64_t cookie) const noexcept;
    const RouterScanDiagnosticsForTest& router_scan_diagnostics_for_test() const noexcept;
    void reset_router_scan_diagnostics_for_test() noexcept;

    enum class RouterFixModeForTest : std::uint8_t {
        production_baseline,
        reverse_scan,
        low_placement_forward,
        bounded_cookie_table,
    };

    void set_router_fix_mode_for_test(RouterFixModeForTest mode) noexcept;
    RouterFixModeForTest router_fix_mode_for_test() const noexcept;

    std::size_t router_install_cookie_for_test() noexcept;
    void router_retire_cookie_for_test(std::size_t router_index) noexcept;

    static std::size_t router_entry_bytes_for_test() noexcept;
    std::size_t router_table_bytes_for_test() const noexcept;
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
        detail::TerminalResult deferred_terminal{};
        ControlState control_state = ControlState::none;
        bool deferred_terminal_stored = false;
        bool in_use = false;
    };

    class BoundedDispatchQueue;

    class TransportLedger;

    static std::uint64_t next_backend_id() noexcept {
        static std::atomic<std::uint64_t> id{0x55720000u};
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

        SubmitPolicy(UringAsyncBackend& self, detail::OperationKind kind) noexcept
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
                return &UringAsyncBackend::publish_size_ready;
            } else {
                return &UringAsyncBackend::publish_void_ready;
            }
        }

        static bool begin_binding(Comp& c) noexcept { return UringAsyncBackend::begin_binding(c); }
        static void install_binding(Comp& c, detail::RequestArena* arena,
                                    detail::SlotHandle h) noexcept {
            UringAsyncBackend::install_binding(c, arena, h);
        }
        static void commit_binding(Comp& c) noexcept { UringAsyncBackend::commit_binding(c); }
        static void rollback_binding(Comp& c) noexcept {
            UringAsyncBackend::rollback_binding_before_accept(c);
        }

        Result<void> stage0_precheck() const noexcept {
            if (!self_.have_ring_) {
                return make_unexpected<void>(IoError{IoError::Code::backend_error});
            }

            if (self_.fatal_error_.has_value()) {
                return make_unexpected<void>(*self_.fatal_error_);
            }
            if (self_.admission_closed_) {
                return make_unexpected<void>(IoError{IoError::Code::invalid_state});
            }
            return {};
        }
        Result<void> validate(const Op& op) const noexcept { return self_.validate_op(op); }
        void write_scratch(detail::SlotHandle h, const Op& op) const noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                // Zero-length ops never perform data I/O; normalize the offset
                // so an unrepresentable offset cannot fail the lowering.
                const std::uint64_t off = op.len == 0 ? 0 : op.offset;
                self_.prepared_ops_[h.slot.value] =
                    PreparedUringOp{kind_,
                                    op.file.fd,
                                    static_cast<const std::byte*>(borrow_of(op).address),
                                    op.len,
                                    sluice::detail::uring_chunk_length(op.len),
                                    off};
            } else {
                self_.prepared_ops_[h.slot.value] = PreparedUringOp{
                    kind_, op.file.fd, nullptr, std::size_t{0}, 0u, std::uint64_t{0}};
            }
        }
        void pause_before_commit_binding() noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

            self_.wait_before_commit_binding_pause_();
#endif
        }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        std::optional<IoError>
        injected_precommit_stage_failure(detail::SubmitStage) const noexcept {
            return std::nullopt;
        }
#endif

      private:
        UringAsyncBackend& self_;
        detail::OperationKind kind_;
    };

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

    bool dispatch_one(detail::SlotHandle h) noexcept;

    bool dispatch_one_locked(detail::SlotHandle h) noexcept;

    void enqueue_after_commit(detail::SlotHandle h) noexcept;

    int submit_transport_locked() noexcept;

    void account_transport_result_locked(int rc, bool had_pending_transport) noexcept;

    void poison_and_recover_locked(IoError error) noexcept;

    int wait_cqe_without_submit() noexcept;

    std::size_t reap_cqes() noexcept;

    void handle_one_cqe(std::uint64_t user_data, int res) noexcept;

    void finalize_operation_terminal_(std::size_t router_index,
                                      const detail::TerminalResult& terminal) noexcept;

    std::uint64_t allocate_cookie_() noexcept;

    std::size_t find_live_router_index_(detail::SlotHandle h) const noexcept;
    std::size_t find_live_router_cookie_(std::uint64_t cookie) const noexcept;
    void retire_router_entry_(std::size_t router_index) noexcept;

    struct CancelScratch {
        bool cancel_queued = false;
    };

    void issue_running_cancel(detail::SlotHandle h) noexcept;

    detail::CancelDisposition cancel_handle_(detail::SlotHandle h) noexcept;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    void wait_after_commit_before_enqueue_pause_() noexcept;
    void wait_before_dispatch_transfer_pause_() noexcept;
    void wait_before_commit_binding_pause_() noexcept;
    void wait_before_admission_lock_pause_() noexcept;
#endif

    void signal_ready_progress() noexcept {
        if (wait_source_) {
            wait_source_->signal_progress();
        }
    }

    detail::RequestArena arena_;
    detail::ReferenceReadySink sink_;
    std::vector<PreparedUringOp> prepared_ops_;
    std::vector<RouterEntry> router_;
    std::vector<CancelScratch> cancel_scratch_;
    std::vector<detail::SlotIndex> cookie_free_list_;
    std::uint64_t next_cookie_ = 1;
    unsigned queue_depth_ = 64;

    std::unique_ptr<UringRingState> ring_state_;
    std::unique_ptr<TransportLedger> transport_ledger_;

    std::unique_ptr<detail::UringWaitSource> wait_source_;
    bool have_ring_ = false;
    bool admission_closed_ = false;
    std::optional<IoError> fatal_error_;

    mutable std::mutex dispatch_mtx_;
    std::unique_ptr<BoundedDispatchQueue> dispatch_;

    std::atomic<std::uint64_t> submit_flushes_{0};
    std::atomic<std::size_t> live_cookies_{0};
    std::atomic<std::size_t> live_control_sqes_{0};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    std::atomic<AfterCommitBeforeEnqueuePauseGate*> after_commit_before_enqueue_gate_{nullptr};
    std::atomic<BeforeDispatchTransferPauseGate*> before_dispatch_transfer_gate_{nullptr};
    std::atomic<BeforeCommitBindingPauseGate*> before_commit_binding_gate_{nullptr};
    std::atomic<BeforeAdmissionLockPauseGate*> before_admission_lock_gate_{nullptr};
    std::atomic<BeforeQueueExitFn> before_queue_exit_fn_{nullptr};
    std::atomic<void*> before_queue_exit_ctx_{nullptr};

    mutable RouterScanModeForTest router_scan_mode_for_test_ =
        RouterScanModeForTest::reverse_production;
    mutable RouterScanDiagnosticsForTest router_diag_for_test_{};

    mutable RouterFixModeForTest router_fix_mode_for_test_ =
        RouterFixModeForTest::production_baseline;
    std::unique_ptr<RouterCookieTableForTest> cookie_table_for_test_;

    void router_table_insert_(std::uint64_t cookie, std::size_t router_index) noexcept;
    void router_table_erase_(std::uint64_t cookie) noexcept;

    void fold_router_table_probes_for_test_(char which, std::uint64_t probes) const noexcept;

    void fold_router_lookup_diag_for_test(RouterLookupKindForTest kind) const noexcept;

    std::size_t router_extent_() const noexcept;
    std::size_t router_extent_cached_for_test_ = 0;
#endif
#endif

    bool available_ = false;
};

} // namespace sluice::async

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include "uring_test_seams.hpp"
#endif
