#include <sluice/async/uring_backend.hpp>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/detail/file_semantics.hpp>
#include <sluice/detail/uring_submit.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include "uring_internal.hpp"

#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(SLUICE_HAS_LIBURING)
#include <liburing.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace sluice::async {

#if !defined(SLUICE_HAS_LIBURING)

UringAsyncBackend::UringAsyncBackend(unsigned queue_depth) : available_(false) {
    (void)queue_depth;
    throw std::runtime_error(
        "sluice::async::UringAsyncBackend: io_uring profile unavailable (built without liburing)");
}

UringAsyncBackend::~UringAsyncBackend() = default;

Result<detail::RequestKey> UringAsyncBackend::submit_read(ReadOp, Completion<std::size_t>*) {
    return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
}
Result<detail::RequestKey> UringAsyncBackend::submit_write(WriteOp, Completion<std::size_t>*) {
    return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
}
Result<detail::RequestKey> UringAsyncBackend::submit_sync_data(SyncDataOp, Completion<void>*) {
    return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
}
Result<detail::RequestKey> UringAsyncBackend::submit_sync_all(SyncAllOp, Completion<void>*) {
    return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
}
Result<detail::RequestKey> UringAsyncBackend::submit_file_info(FileInfoOp, Completion<FileInfo>*) {
    return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
}
Result<detail::RequestKey> UringAsyncBackend::submit_size(SizeOp, Completion<FileSize>*) {
    return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
}

std::size_t UringAsyncBackend::poll() {
    return 0;
}
void UringAsyncBackend::cancel(Completion<std::size_t>&) {}
void UringAsyncBackend::cancel(Completion<void>&) {}

void UringAsyncBackend::close_admission() {}
std::size_t UringAsyncBackend::outstanding() const noexcept {
    return 0;
}
bool UringAsyncBackend::internal_work_retired() const noexcept {
    return true;
}
void UringAsyncBackend::retire_execution_resources() noexcept {}
bool UringAsyncBackend::available() const noexcept {
    return available_;
}

#else

namespace {

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
std::atomic<bool> g_injected_statx_probe_failure{false};
std::atomic<bool> g_injected_opcode_probe_failure{false};
#endif

}
Result<void> UringAsyncBackend::validate_read(ReadOp op) {
    const sluice::detail::DataOpVerdict verdict = sluice::detail::precheck_data_op(
        {op.file.fd < 0, op.file.access, sluice::detail::FileOperation::read, op.offset, op.len});
    if (verdict == sluice::detail::DataOpVerdict::execute && op.dst == nullptr)
        return make_unexpected<void>(IoError{.code = IoError::Code::invalid_argument});
    return sluice::detail::accept_or_reject(verdict);
}
Result<void> UringAsyncBackend::validate_write(WriteOp op) {
    const sluice::detail::DataOpVerdict verdict = sluice::detail::precheck_data_op(
        {op.file.fd < 0, op.file.access, sluice::detail::FileOperation::write, op.offset, op.len});
    if (verdict == sluice::detail::DataOpVerdict::execute && op.src == nullptr)
        return make_unexpected<void>(IoError{.code = IoError::Code::invalid_argument});
    return sluice::detail::accept_or_reject(verdict);
}
Result<void> UringAsyncBackend::validate_sync(SyncDataOp op) {
    return sluice::detail::accept_or_reject(sluice::detail::precheck_state_op(
        op.file.fd < 0, op.file.access, sluice::detail::FileOperation::sync_data));
}
Result<void> UringAsyncBackend::validate_sync(SyncAllOp op) {
    return sluice::detail::accept_or_reject(sluice::detail::precheck_state_op(
        op.file.fd < 0, op.file.access, sluice::detail::FileOperation::sync_all));
}
Result<void> UringAsyncBackend::validate_file_info(FileInfoOp op) {
    return sluice::detail::accept_or_reject(sluice::detail::precheck_state_op(
        op.file.fd < 0, op.file.access, sluice::detail::FileOperation::file_info));
}
Result<void> UringAsyncBackend::validate_size(SizeOp op) {
    return sluice::detail::accept_or_reject(sluice::detail::precheck_state_op(
        op.file.fd < 0, op.file.access, sluice::detail::FileOperation::file_info));
}

template <class Op> Result<void> UringAsyncBackend::validate_op(const Op& op) noexcept {
    if constexpr (std::is_same_v<Op, ReadOp>) {
        return validate_read(op);
    } else if constexpr (std::is_same_v<Op, WriteOp>) {
        return validate_write(op);
    } else if constexpr (std::is_same_v<Op, FileInfoOp>) {
        return validate_file_info(op);
    } else if constexpr (std::is_same_v<Op, SizeOp>) {
        return validate_size(op);
    } else {
        return validate_sync(op);
    }
}

void UringAsyncBackend::publish_size_ready(void* completion,
                                           const detail::PublicationPayload& payload) noexcept {
    const sluice::detail::IoOutcome& outcome = payload.outcome;
    Result<std::size_t> result =
        outcome.succeeded
            ? Result<std::size_t>{static_cast<std::size_t>(outcome.effect.confirmed_bytes)}
            : make_unexpected<std::size_t>(outcome.error);
    AsyncBackend::publish(*static_cast<Completion<std::size_t>*>(completion), std::move(result));
}

void UringAsyncBackend::publish_void_ready(void* completion,
                                           const detail::PublicationPayload& payload) noexcept {
    const sluice::detail::IoOutcome& outcome = payload.outcome;
    Result<void> result = outcome.succeeded ? Result<void>{} : make_unexpected<void>(outcome.error);
    AsyncBackend::publish(*static_cast<Completion<void>*>(completion), std::move(result));
}

void UringAsyncBackend::publish_file_info_ready(void* completion,
                                                const detail::PublicationPayload& payload) noexcept {
    const sluice::detail::IoOutcome& outcome = payload.outcome;
    Result<FileInfo> result = outcome.succeeded
                                  ? Result<FileInfo>{payload.metadata}
                                  : make_unexpected<FileInfo>(outcome.error);
    AsyncBackend::publish(*static_cast<Completion<FileInfo>*>(completion), std::move(result));
}

void UringAsyncBackend::publish_size_value_ready(
    void* completion, const detail::PublicationPayload& payload) noexcept {
    const sluice::detail::IoOutcome& outcome = payload.outcome;
    Result<FileSize> result = outcome.succeeded
                                  ? Result<FileSize>{FileSize{payload.metadata.size}}
                                  : make_unexpected<FileSize>(outcome.error);
    AsyncBackend::publish(*static_cast<Completion<FileSize>*>(completion), std::move(result));
}

void UringAsyncBackend::publish_request_ready(void* completion,
                                              const detail::PublicationPayload& payload) noexcept {
    (void)completion;
    (void)payload;
}

UringAsyncBackend::UringAsyncBackend(unsigned queue_depth)
    : UringAsyncBackend(UringConfig{static_cast<std::size_t>(queue_depth > 0 ? queue_depth : 64),
                                    queue_depth > 0 ? queue_depth : 64}) {}

UringAsyncBackend::UringAsyncBackend(UringConfig config)
    : UringAsyncBackend(validate_config_(config), ValidatedConfigTag{}) {}

UringConfig UringAsyncBackend::validate_config_(UringConfig config) {
    constexpr std::size_t slot_index_max =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    if (config.request_capacity == 0 || config.request_capacity > slot_index_max ||
        config.queue_depth == 0) {
        throw std::invalid_argument(
            "UringConfig request_capacity must be in [1, UINT32_MAX] and queue_depth must be > 0");
    }
    return config;
}

UringAsyncBackend::UringAsyncBackend(UringConfig config, ValidatedConfigTag)
    : capacity_(config.request_capacity), prepared_ops_(config.request_capacity),
      delivery_(config.request_capacity), router_(config.request_capacity),
      cookie_free_list_(config.request_capacity), statx_buffers_(config.request_capacity),
      ring_state_(std::make_unique<UringRingState>()) {
    for (std::uint32_t i = 0; i < config.request_capacity; ++i) {
        cookie_free_list_[i] = detail::SlotIndex{i};
    }
    dispatch_ = std::make_unique<BoundedDispatchQueue>(config.request_capacity);
    publication_pending_ = std::make_unique<BoundedDispatchQueue>(config.request_capacity);
    if (::io_uring_queue_init(config.queue_depth, &ring_state_->ring, 0) != 0) {
        throw std::runtime_error(
            "sluice::async::UringAsyncBackend: io_uring ring construction failed "
            "(required-profile setup failure)");
    }
    try {
        transport_ledger_ =
            std::make_unique<TransportLedger>(ring_state_->ring.sq.ring_entries);
    } catch (...) {
        ::io_uring_queue_exit(&ring_state_->ring);
        throw;
    }
    have_ring_ = true;
#if defined(SLUICE_E1_MUTANT_OPCODE_PROBE_IGNORED)
    if (false) {
#else
    if (!probe_required_opcodes_()) {
#endif
        ::io_uring_queue_exit(&ring_state_->ring);
        have_ring_ = false;
        throw std::runtime_error(
            "sluice::async::UringAsyncBackend: io_uring required request opcodes unavailable "
            "(required-profile setup failure)");
    }
#if defined(SLUICE_E1_MUTANT_CAPABILITY_PROBE_IGNORED)
    if (false) {
#else
    if (!probe_statx_support_()) {
#endif

        ::io_uring_queue_exit(&ring_state_->ring);
        have_ring_ = false;
        throw std::runtime_error(
            "sluice::async::UringAsyncBackend: io_uring statx mechanism unavailable "
            "(required metadata capability missing at profile setup)");
    }
    available_ = true;
}

bool UringAsyncBackend::probe_required_opcodes_() noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (g_injected_opcode_probe_failure.load(std::memory_order_acquire)) {
        return false;
    }
#endif
    static constexpr int kRequiredOpcodes[] = {IORING_OP_READ, IORING_OP_WRITE, IORING_OP_FSYNC,
                                               IORING_OP_STATX};
    struct ::io_uring_probe* probe = ::io_uring_get_probe_ring(&ring_state_->ring);
    if (probe == nullptr) {
        return false;
    }
    bool all_supported = true;
    for (const int opcode : kRequiredOpcodes) {
        if (::io_uring_opcode_supported(probe, opcode) == 0) {
            all_supported = false;
            break;
        }
    }
    ::io_uring_free_probe(probe);
    return all_supported;
}

bool UringAsyncBackend::probe_statx_support_() noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (g_injected_statx_probe_failure.load(std::memory_order_acquire)) {
        return false;
    }
#endif
    struct ::statx probe_buffer {};
    io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_state_->ring);
    if (sqe == nullptr) {
        return false;
    }
    ::io_uring_prep_statx(sqe, AT_FDCWD, ".", 0, STATX_TYPE, &probe_buffer);
    ::io_uring_sqe_set_data64(sqe, 0);
    if (::io_uring_submit_and_wait(&ring_state_->ring, 1) < 0) {
        return false;
    }
    io_uring_cqe* cqe = nullptr;
    if (::io_uring_peek_cqe(&ring_state_->ring, &cqe) != 0 || cqe == nullptr) {
        return false;
    }
    const int res = cqe->res;
    ::io_uring_cqe_seen(&ring_state_->ring, cqe);
    return res >= 0;
}

#if defined(SLUICE_HAS_LIBURING)
bool UringAsyncBackend::progress_port_attached() noexcept {
    if (!have_ring_) {
        return true;
    }
    const bool installed = progress_port_.register_notification_with_kernel(
        [](void* self, int fd) noexcept -> bool {
            auto* backend = static_cast<UringAsyncBackend*>(self);
            int rc = 0;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            backend->eventfd_registrations_.fetch_add(1, std::memory_order_relaxed);
            if (backend->ring_state_->test_hooks.register_eventfd != nullptr) {
                rc = backend->ring_state_->test_hooks.register_eventfd(
                    backend->ring_state_->test_hooks.context, &backend->ring_state_->ring, fd);
            } else {
                rc = ::io_uring_register_eventfd(&backend->ring_state_->ring, fd);
            }
#else
            rc = ::io_uring_register_eventfd(&backend->ring_state_->ring, fd);
#endif
            backend->eventfd_registered_ = rc == 0;
            return rc == 0;
        },
        this);
    return installed;
}

bool UringAsyncBackend::has_immediate_physical_work() const noexcept {
    // CQ visibility follows the kernel's CQ publication protocol, not the
    // eventfd; the overflow flag covers completions parked outside the ring.
    if (!have_ring_)
        return false;
    if (::io_uring_cq_ready(&ring_state_->ring) > 0)
        return true;
    return ::io_uring_cq_has_overflow(&ring_state_->ring);
}

AsyncBackend::ProgressPass UringAsyncBackend::poll_progress() {
    ProgressPass pass;
    pass.completed = poll();
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        pass.immediate_work_remains = !publication_pending_->empty();
#if defined(SLUICE_B1C_MUTANT_TRANSPORT_RETRY_UNADVERTISED)
        pass.dispatch_retry_remains = false;
#else
        pass.dispatch_retry_remains = dispatch_retry_remains_locked_();
#endif
        // Sticky backend health under the same lock that owns fatal_error_;
        // the context composes it with the core and wait-domain verdicts.
        pass.health_failed = fatal_error_.has_value();
    }
    // A capped pass can leave reaped-eligible CQ entries behind; that
    // leftover is immediate work the next pass owes.
    pass.immediate_work_remains = pass.immediate_work_remains || has_immediate_physical_work();
    pass.accepted_work_remains = outstanding() != 0;
    return pass;
}
#endif

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
UringAsyncBackend::UringAsyncBackend(UringConfig config, UringBackendSubmitTestHooks hooks)
    : UringAsyncBackend(config) {
    ring_state_->test_hooks = hooks;
}
#endif

UringAsyncBackend::~UringAsyncBackend() {
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        const detail::CoreOccupancy occupancy =
            core_ != nullptr ? core_->occupancy() : detail::CoreOccupancy{};
        const bool ledger_quiescent =
            transport_ledger_ == nullptr || transport_ledger_->empty() ||
            (fatal_error_.has_value() && transport_ledger_->all_class_a_recovery_retired());
        bool event_owed = false;
        for (const DeliveryRecord& record : delivery_) {
            if (record.event_owed) {
                event_owed = true;
                break;
            }
        }
        // Published slots with live public bindings may remain: they are
        // retained results; every other accepted fact here is unsettled work.
        if (!dispatch_->empty() || !publication_pending_->empty() ||
            live_cookies_.load(std::memory_order_relaxed) != 0 ||
            live_control_sqes_.load(std::memory_order_relaxed) != 0 || !ledger_quiescent ||
            event_owed || occupancy.outstanding != 0 || occupancy.execution_refs != 0 ||
            occupancy.control_refs != 0 || occupancy.publication_inflight != 0 ||
            occupancy.observer_registrations != 0) {
            detail::uring_non_quiescent_destruction_fail_fast();
        }
    }
    retire_execution_resources();
}

bool UringAsyncBackend::internal_work_retired() const noexcept {
    if (!have_ring_) {
        return true;
    }
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    const bool ledger_quiescent =
        transport_ledger_ == nullptr || transport_ledger_->empty() ||
        (fatal_error_.has_value() && transport_ledger_->all_class_a_recovery_retired());
    bool event_owed = false;
    for (const DeliveryRecord& record : delivery_) {
        if (record.event_owed) {
            event_owed = true;
            break;
        }
    }
    return dispatch_->empty() && publication_pending_->empty() && !event_owed &&
           live_cookies_.load(std::memory_order_relaxed) == 0 &&
           live_control_sqes_.load(std::memory_order_relaxed) == 0 && ledger_quiescent;
}

void UringAsyncBackend::retire_execution_resources() noexcept {
    if (!have_ring_) {
        return;
    }
    if (eventfd_registered_) {
        int rc = 0;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        eventfd_unregistrations_.fetch_add(1, std::memory_order_relaxed);
        if (ring_state_->test_hooks.unregister_eventfd != nullptr) {
            rc = ring_state_->test_hooks.unregister_eventfd(ring_state_->test_hooks.context,
                                                            &ring_state_->ring);
        } else {
            rc = ::io_uring_unregister_eventfd(&ring_state_->ring);
        }
#else
        rc = ::io_uring_unregister_eventfd(&ring_state_->ring);
#endif
        if (rc != 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: eventfd unregister "
                                 "failed before ring exit (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        eventfd_registered_ = false;
    }
    ::io_uring_queue_exit(&ring_state_->ring);
    have_ring_ = false;
}

bool UringAsyncBackend::backend_health_failed() const noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    return fatal_error_.has_value();
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
void UringAsyncBackend::set_injected_statx_probe_failure(bool value) noexcept {
    g_injected_statx_probe_failure.store(value, std::memory_order_release);
}

bool UringAsyncBackend::injected_statx_probe_failure() noexcept {
    return g_injected_statx_probe_failure.load(std::memory_order_acquire);
}

void UringAsyncBackend::set_injected_opcode_probe_failure(bool value) noexcept {
    g_injected_opcode_probe_failure.store(value, std::memory_order_release);
}

bool UringAsyncBackend::injected_opcode_probe_failure() noexcept {
    return g_injected_opcode_probe_failure.load(std::memory_order_acquire);
}

#endif

template <class Op, class Comp>
Result<detail::RequestKey> UringAsyncBackend::submit_request(Op op, Comp* c,
                                                             detail::OperationKind kind,
                                                             detail::RequestOp core_op) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    wait_submit_entry_pause_();
#endif
    if (auto v = validate_op(op); !v.has_value()) {
        return make_unexpected<detail::RequestKey>(v.error());
    }
    if (!have_ring_) {
        return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
    }
    if (fatal_error_.has_value()) {
        return make_unexpected<detail::RequestKey>(*fatal_error_);
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    if (auto inj = injected_precommit_stage_failure_(SubmitStage::reserve); inj.has_value()) {
        return make_unexpected<detail::RequestKey>(*inj);
    }
#endif

    const auto reservation = core_->reserve();
    if (!reservation.ok()) {
        const IoError::Code code = reservation.status == detail::ReserveStatus::admission_closed
                                       ? IoError::Code::invalid_state
                                       : IoError::Code::would_block;
        return make_unexpected<detail::RequestKey>(IoError{code});
    }
    const detail::SlotHandle h{reservation.reservation.slot, reservation.reservation.generation};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    if (auto inj = injected_precommit_stage_failure_(SubmitStage::prepare); inj.has_value()) {
        (void)core_->rollback(reservation.reservation);
        return make_unexpected<detail::RequestKey>(*inj);
    }
#endif

    std::uint64_t length = 0;
    std::uint64_t offset = 0;
    bool zero_op = false;
    if constexpr (std::is_same_v<Op, ReadOp> || std::is_same_v<Op, WriteOp>) {
        length = op.len;
        offset = op.len == 0 ? std::uint64_t{0} : op.offset;
        zero_op = op.len == 0;
    }
#if defined(SLUICE_B1C_MUTANT_PREMATURE_ZERO_OP_DISPATCH)
    zero_op = false;
#endif
    prepared_ops_[h.slot.value] =
        PreparedUringOp{kind, op.file.fd, buffer_of(op), static_cast<std::size_t>(length),
                        sluice::detail::uring_chunk_length(static_cast<std::size_t>(length)),
                        offset,
                        (kind == detail::OperationKind::file_info ||
                         kind == detail::OperationKind::size)
                            ? &statx_buffers_[h.slot.value]
                            : nullptr};

    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        DeliveryRecord& record = delivery_[h.slot.value];
        record.completion = c;
        record.publish =
            c != nullptr ? publish_thunk<Comp>() : &UringAsyncBackend::publish_request_ready;
        record.kind = kind;
        record.event_owed = false;
        record.owed_key = {};
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    if (auto inj = injected_precommit_stage_failure_(SubmitStage::commit); inj.has_value()) {
        (void)core_->rollback(reservation.reservation);
        return make_unexpected<detail::RequestKey>(*inj);
    }

    wait_pre_accept_commit_pause_();
#endif

    if (c != nullptr && !begin_binding(*c)) {
        (void)core_->rollback(reservation.reservation);
        return make_unexpected<detail::RequestKey>(IoError{IoError::Code::invalid_state});
    }

    const detail::RequestDescriptor descriptor{core_op, offset, length, zero_op};
    const detail::BorrowFacts borrow{op.file.fd, buffer_of(op), length};
    const auto accepted = core_->accept(reservation.reservation, descriptor, borrow);
    if (!accepted.ok()) {
        if (c != nullptr) {
            rollback_binding_before_accept(*c);
        }
        (void)core_->rollback(reservation.reservation);
        return make_unexpected<detail::RequestKey>(IoError{IoError::Code::invalid_state});
    }

    if (c != nullptr) {
        install_core_binding(*c, core_, accepted.id);
        commit_binding(*c);
    }

    if (descriptor.zero_op) {
        publish_zero_op_inline(accepted.id, h);
    } else {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        wait_accepted_pre_dispatch_pause_();
#endif
        dispatch_after_accept(h);
    }
    return accepted.id;
}

Result<detail::RequestKey> UringAsyncBackend::submit_read(ReadOp op, Completion<std::size_t>* c) {
    return submit_request(op, c, detail::OperationKind::read, detail::RequestOp::read);
}
Result<detail::RequestKey> UringAsyncBackend::submit_write(WriteOp op, Completion<std::size_t>* c) {
    return submit_request(op, c, detail::OperationKind::write, detail::RequestOp::write);
}
Result<detail::RequestKey> UringAsyncBackend::submit_sync_data(SyncDataOp op, Completion<void>* c) {
    return submit_request(op, c, detail::OperationKind::sync_data, detail::RequestOp::sync_data);
}
Result<detail::RequestKey> UringAsyncBackend::submit_sync_all(SyncAllOp op, Completion<void>* c) {
    return submit_request(op, c, detail::OperationKind::sync_all, detail::RequestOp::sync_all);
}

Result<detail::RequestKey> UringAsyncBackend::submit_file_info(FileInfoOp op,
                                                               Completion<FileInfo>* c) {
    return submit_request(op, c, detail::OperationKind::file_info, detail::RequestOp::file_info);
}

Result<detail::RequestKey> UringAsyncBackend::submit_size(SizeOp op, Completion<FileSize>* c) {
    return submit_request(op, c, detail::OperationKind::size, detail::RequestOp::size);
}

void UringAsyncBackend::publish_zero_op_inline(detail::RequestKey id,
                                               detail::SlotHandle h) noexcept {
    if (!core_->acquire_control(id)) {
        detail::uring_core_handoff_fail_fast();
    }
    detail::PublicationPayload payload;
    if (core_->begin_publication(id, &payload) != detail::PublicationGrant::granted) {
        detail::uring_core_handoff_fail_fast();
    }
    DeliveryRecord& record = delivery_[h.slot.value];
    record.publish(record.completion, payload);
    if (core_->complete_publication(id) != detail::PublicationCompletion::completed) {
        detail::uring_core_handoff_fail_fast();
    }
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        record.event_owed = true;
        record.owed_key = id;
    }
    signal_ready_progress();
}

void UringAsyncBackend::publish_one(detail::SlotHandle h) {
    const detail::RequestKey key{core_->context(), h.slot, h.generation};
    detail::PublicationPayload payload;
    if (core_->begin_publication(key, &payload) != detail::PublicationGrant::granted) {
        detail::uring_core_handoff_fail_fast();
    }
    DeliveryRecord& record = delivery_[h.slot.value];
    // kind is copied while publication_inflight still pins the slot: after
    // complete_publication the slot may be reclaimed and re-initialized
    // concurrently, so no record field may be read past that point.
    const detail::OperationKind kind = record.kind;
    record.publish(record.completion, payload);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    wait_publication_epilogue_pause_();
#endif
    if (core_->complete_publication(key) != detail::PublicationCompletion::completed) {
        detail::uring_core_handoff_fail_fast();
    }
    deliver_event(payload.id, kind);
}

void UringAsyncBackend::deliver_event(detail::RequestKey key, detail::OperationKind kind) {
    (void)core_->claim_observer_delivery(key);
    (routing_sink_ ? *routing_sink_ : sink_).on_ready(detail::ReadyEvent{key, kind});
}

std::size_t UringAsyncBackend::poll() {
    if (!have_ring_)
        return 0;

    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        if (!fatal_error_.has_value()) {
            while (!dispatch_->empty()) {
                detail::SlotHandle h = dispatch_->front();
                if (!dispatch_one_locked(h))
                    break;
            }
        }

        if (!fatal_error_.has_value()) {
            (void)submit_transport_locked();
            service_pending_controls_locked_();
        }
    }

    reap_cqes();

    std::size_t published = 0;
    std::size_t pass_bound = 0;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        pass_bound = publication_pending_->size();
    }
    // Same pass boundary as the reap: entries pushed during this pass are the
    // next pass's immediate work.
    for (std::size_t n = 0; n < pass_bound; ++n) {
        detail::SlotHandle h{};
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(dispatch_mtx_);
            have = publication_pending_->pop_front(h);
        }
        if (!have)
            break;
        publish_one(h);
        ++published;
    }
    for (std::uint32_t i = 0; i < capacity_; ++i) {
        detail::RequestKey owed{};
        detail::OperationKind owed_kind = detail::OperationKind::read;
        {
            std::lock_guard<std::mutex> lk(dispatch_mtx_);
            DeliveryRecord& record = delivery_[i];
            if (!record.event_owed)
                continue;
            record.event_owed = false;
            owed = record.owed_key;
            record.owed_key = {};
            owed_kind = record.kind;
        }
        deliver_event(owed, owed_kind);
        if (core_->release_control(owed) != detail::ControlRelease::released) {
            detail::uring_core_handoff_fail_fast();
        }
        ++published;
    }

    return published;
}

void UringAsyncBackend::close_admission() {
    if (!have_ring_)
        return;
#if defined(SLUICE_C2E_MUTANT_CLOSE_ADMISSION_ALWAYS_SIGNALS)
    if (core_ != nullptr) {
        (void)core_->close_admission();
    }
    signal_ready_progress();
#else
    if (core_ != nullptr && core_->close_admission()) {
        signal_ready_progress();
    }
#endif
}

std::size_t UringAsyncBackend::outstanding() const noexcept {
    return core_ != nullptr ? core_->occupancy().outstanding : 0;
}

bool UringAsyncBackend::available() const noexcept {
    return available_;
}

Result<RequestHandleState> UringAsyncBackend::resolve_identity_state(
    std::uint64_t ctx, std::uint32_t slot, std::uint64_t gen) const {
    if (core_ == nullptr) {
        return RequestHandleState::not_found;
    }
    const detail::RequestKey key{detail::ContextIdentity{ctx}, detail::SlotIndex{slot},
                                 detail::Generation{gen}};
    switch (core_->lookup(key)) {
    case detail::PublicLookup::outstanding:
        return RequestHandleState::outstanding;
    case detail::PublicLookup::published:
        return RequestHandleState::completion_ready;
    case detail::PublicLookup::not_found:
        return RequestHandleState::not_found;
    }
    return RequestHandleState::not_found;
}

void UringAsyncBackend::cancel(Completion<std::size_t>& c) {
    auto key = core_binding(c);
    if (!key.has_value())
        return;
    (void)cancel_key(*key);
}

void UringAsyncBackend::cancel(Completion<void>& c) {
    auto key = core_binding(c);
    if (!key.has_value())
        return;
    (void)cancel_key(*key);
}

detail::PublicCancel UringAsyncBackend::cancel_identity(detail::RequestKey key) {
    return cancel_key(key);
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

void UringAsyncBackend::wait_submit_entry_pause_() noexcept {
    auto* g = submit_entry_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_all();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_all();
}

void UringAsyncBackend::wait_pre_accept_commit_pause_() noexcept {
    auto* g = pre_accept_commit_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_all();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_all();
}

void UringAsyncBackend::wait_accepted_pre_dispatch_pause_() noexcept {
    auto* g = accepted_pre_dispatch_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_all();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_all();
}

void UringAsyncBackend::wait_publication_epilogue_pause_() noexcept {
    auto* g = publication_epilogue_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_all();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_all();
}

std::optional<IoError>
UringAsyncBackend::injected_precommit_stage_failure_(SubmitStage stage) noexcept {
    auto* inj = submit_stage_failure_injection_.load(std::memory_order_acquire);
    if (inj == nullptr)
        return std::nullopt;
    switch (stage) {
    case SubmitStage::reserve:
        if (inj->fail_reserve.load(std::memory_order_acquire)) {
            inj->reserve_fired.fetch_add(1, std::memory_order_relaxed);

            return IoError{IoError::Code::would_block};
        }
        break;
    case SubmitStage::prepare:
        if (inj->fail_prepare.load(std::memory_order_acquire)) {
            inj->prepare_fired.fetch_add(1, std::memory_order_relaxed);
            return IoError{IoError::Code::invalid_state};
        }
        break;
    case SubmitStage::commit:
        if (inj->fail_commit.load(std::memory_order_acquire)) {
            inj->commit_fired.fetch_add(1, std::memory_order_relaxed);
            return IoError{IoError::Code::invalid_state};
        }
        break;
    }
    return std::nullopt;
}
#endif

#endif

}
